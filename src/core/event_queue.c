/**
 * @file event_queue.c
 * @brief 事件队列实现
 *
 * 基于优先级的队列实现，支持可配置的溢出处理。
 *
 * 实现说明：
 * - 基于 Zephyr k_msgq 实现
 * - 支持多种溢出策略
 * - 提供详细的统计信息
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 *
 */

#include "event_queue.h"
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <errno.h>

LOG_MODULE_REGISTER(event_queue, CONFIG_SYS_LOG_LEVEL);

/* 外部声明：来自 event_system.c 的全局丢弃计数器递增函数 */
extern void event_system_inc_dropped_count(void);

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

/**
 * @brief 扩展队列控制块
 *
 * 包含队列的统计信息和管理数据。
 *
 * SIL-2: HIGH-3 修复 —— 统计计数器改为 atomic_t，使 ISR 路径可正确累计
 * （k_msgq_put 在 ISR 中合法，但互斥锁不可用，原子操作填补此空白）。
 */
typedef struct {
    struct k_msgq* msgq;                       /**< 消息队列指针 */
    atomic_t       enqueue_count;              /**< 入队成功计数（ISR 安全） */
    atomic_t       dequeue_count;              /**< 出队成功计数 */
    atomic_t       overflow_count;             /**< 溢出（队列满）计数（ISR 安全） */
    atomic_t       drop_count;                 /**< 显式丢弃计数（DROP_LOWEST/purge） */
    atomic_t       high_watermark;             /**< 队列深度历史最大值（CAS 更新） */
    atomic_t       reordering;                 /**< 线程侧排空/回灌正在进行中 */
    atomic_t       isr_ops_in_flight;          /**< reordering 之前已准入的 ISR 操作数 */
    uint32_t       capacity;                   /**< 队列容量 */
    struct k_mutex reorder_lock;               /**< DROP_LOWEST 时串行化线程侧 msgq 操作 */
    event_t*       drop_lowest_scratch;        /**< DROP_LOWEST 独立临时缓冲区 */
    event_t*       drop_lowest_restore_failed; /**< DROP_LOWEST 回灌失败后待释放 payload */
} event_queue_cb_t;

/* 静态队列控制块数组，用于跟踪统计信息 */
/* SIL-2: 增加数组大小以支持更多测试场景和并发队列 */
#define MAX_QUEUE_CB_ENTRIES 32

static event_queue_cb_t g_queue_cb[MAX_QUEUE_CB_ENTRIES];

/** 保护队列控制块数组的全局互斥锁 */
static K_MUTEX_DEFINE(g_queue_cb_lock);

/** scratch 已就绪时，线程侧 msgq 操作须串行化（含单元测试运行时指定 DROP_LOWEST 策略） */
static inline bool event_queue_use_op_lock(const event_queue_cb_t* cb) {
    return (cb != NULL) && (cb->drop_lowest_scratch != NULL);
}

static bool event_queue_isr_op_enter(event_queue_cb_t* cb) {
    if (atomic_get(&cb->reordering) != 0) {
        return false;
    }

    /* 自增后重新检查，以便关闭准入的线程能在每个 CPU 上等待所有更早的 ISR 完成 */
    (void) atomic_inc(&cb->isr_ops_in_flight);
    if (atomic_get(&cb->reordering) != 0) {
        atomic_dec(&cb->isr_ops_in_flight);
        return false;
    }

    return true;
}

static void event_queue_isr_op_exit(event_queue_cb_t* cb) {
    atomic_dec(&cb->isr_ops_in_flight);
}

static void event_queue_reorder_begin(event_queue_cb_t* cb) {
    atomic_set(&cb->reordering, 1);
    while (atomic_get(&cb->isr_ops_in_flight) != 0) {
        k_yield();
    }
}

static void event_queue_reorder_end(event_queue_cb_t* cb) {
    atomic_set(&cb->reordering, 0);
}

/**
 * @brief 为 DROP_LOWEST 分配 scratch（Kconfig 启用时 init 预分配；否则首次使用时惰性分配）
 *
 * @note L-2（审查记录，良性已知项）：首次惰性分配使 scratch 从 NULL 变为非 NULL，
 *       而无锁 dequeue 路径以 event_queue_use_op_lock()（即 scratch != NULL）决定是否
 *       取 reorder_lock。因此仅在"全局第一次 DROP_LOWEST 入队"且队列恰好已满需 drain
 *       时，存在一个并发 dequeuer 可能观察到 scratch 旧值 NULL、跳过 reorder_lock 的窗口。
 *       由于所有队列变更均经 k_msgq 内部自旋锁串行化，最坏后果仅为该 dequeuer 在本次
 *       reorder 期间得到一次 spurious-empty（无内存破坏、无数据损坏）；此后 scratch 恒为
 *       非 NULL，所有 dequeue 均取 reorder_lock，窗口不再出现。综合权衡保留惰性分配以
 *       维持运行期 DROP_LOWEST 能力与既有单测覆盖。
 */
static event_status_t event_queue_ensure_drop_lowest_scratch(event_queue_cb_t* cb) {
    if (cb->drop_lowest_scratch != NULL && cb->drop_lowest_restore_failed != NULL) {
        return EVENT_OK;
    }

    k_mutex_lock(&g_queue_cb_lock, K_FOREVER);
    if (cb->drop_lowest_scratch == NULL) {
        cb->drop_lowest_scratch = (event_t*) k_malloc(cb->capacity * sizeof(event_t));
        if (cb->drop_lowest_scratch == NULL) {
            k_mutex_unlock(&g_queue_cb_lock);
            LOG_ERR("Failed to allocate drop_lowest_scratch for queue");
            return EVENT_ERR_NO_MEM;
        }
    }
    if (cb->drop_lowest_restore_failed == NULL) {
        cb->drop_lowest_restore_failed = (event_t*) k_malloc(cb->capacity * sizeof(event_t));
        if (cb->drop_lowest_restore_failed == NULL) {
            k_mutex_unlock(&g_queue_cb_lock);
            LOG_ERR("Failed to allocate drop_lowest_restore_failed for queue");
            return EVENT_ERR_NO_MEM;
        }
    }
    k_mutex_unlock(&g_queue_cb_lock);

    return EVENT_OK;
}

/**
 * @brief 验证事件有效性
 */
static bool event_is_valid(const event_t* event) {
    if (event == NULL) {
        return false;
    }
    /* event->type 是 uint8_t，值域天然为 0-255，无需额外范围检查 */
    return true;
}

static void event_free_queued_payload(event_t* ev) {
    /* SIL-2: 使用统一接口释放动态数据，正确处理 slab 来源 */
    event_free_data(ev);
}

/**
 * @brief 记录队列满导致的丢弃（仅在实际丢弃时调用，DROP_LOWEST 成功入队前勿调用）
 */
static void event_queue_record_drop(event_queue_cb_t* cb) {
    atomic_inc(&cb->overflow_count);
    event_system_inc_dropped_count();
}

#if defined(CONFIG_EVENT_QUEUE_OVERFLOW_BLOCK)
/**
 * @brief BLOCK 策略下 K_FOREVER 入队：分段阻塞并轮询 running，避免 stop 时永久卡在 k_msgq_put
 *
 * @note 退出循环最长滞后一个 CONFIG_EVENT_QUEUE_BLOCK_RETRY_MS。event_system_stop()
 *       先置 running=0 再 ops_wait_zero()（上限 EVENT_SYSTEM_OP_WAIT_TIMEOUT_MS，5000ms），
 *       因此 BLOCK_RETRY_MS 必须远小于该超时，否则被阻塞的发布者可能拖到 stop 返回
 *       EVENT_ERR_TIMEOUT。BLOCK 策略不建议用于实时路径。
 */
static int event_msgq_put(struct k_msgq* queue, const void* data, k_timeout_t timeout) {
    if (K_TIMEOUT_EQ(timeout, K_FOREVER)) {
        const int retry_ms = CONFIG_EVENT_QUEUE_BLOCK_RETRY_MS;

        while (event_system_is_running()) {
            int ret = k_msgq_put(queue, data, K_MSEC(retry_ms));

            if (ret == 0) {
                return 0;
            }
            if (ret == -EAGAIN) {
                continue;
            }
            return ret;
        }
        return -ECANCELED;
    }

    return k_msgq_put(queue, data, timeout);
}
#else

static int event_msgq_put(struct k_msgq* queue, const void* data, k_timeout_t timeout) {
    return k_msgq_put(queue, data, timeout);
}
#endif /* CONFIG_EVENT_QUEUE_OVERFLOW_BLOCK */

/**
 * @brief 原子更新水位线（仅当新值大于当前值时更新）
 *
 * SIL-2: HIGH-3 修复后的 CAS 循环实现，避免并发更新丢失。
 * ISR 安全：原子操作不依赖互斥锁。
 *
 * @param hw 水位线 atomic 指针
 * @param depth 当前深度
 */
static inline void update_high_watermark(atomic_t* hw, uint32_t depth) {
    atomic_val_t old_hw;

    do {
        old_hw = atomic_get(hw);
        if ((atomic_val_t) depth <= old_hw) {
            return;
        }
    } while (!atomic_cas(hw, old_hw, (atomic_val_t) depth));
}

/**
 * 队列已满时：丢弃队列中优先级最低的一条（priority 数值最大；相等则 FIFO 最旧），再入队 event。
 * 若 event 比队列中最差的一条还差，则丢弃 event（不入队）。
 *
 * @pre 调用方已持有 cb->reorder_lock；cb->drop_lowest_scratch 已分配
 */
static event_status_t enqueue_drop_lowest_locked(struct k_msgq* queue, const event_t* event, k_timeout_t timeout,
                                                 event_queue_cb_t* cb) {
    /* SIL-2: 验证输入事件有效性 */
    if (!event_is_valid(event)) {
        LOG_ERR("Invalid event in enqueue_drop_lowest");
        return EVENT_ERR_INVALID_ARG;
    }

    struct k_msgq_attrs attrs;

    k_msgq_get_attrs(queue, &attrs);

    /* SIL-2: 检查 scratch 缓冲区是否已分配 */
    if (cb->drop_lowest_scratch == NULL || cb->drop_lowest_restore_failed == NULL) {
        LOG_ERR("DROP_LOWEST scratch not allocated");
        return EVENT_ERR_INVALID_ARG;
    }

    if (attrs.max_msgs > cb->capacity) {
        LOG_ERR("Queue capacity exceeds DROP_LOWEST scratch capacity");
        return EVENT_ERR_INVALID_ARG;
    }

    uint32_t n = attrs.used_msgs;

    if (n < attrs.max_msgs) {
        int pret = event_msgq_put(queue, event, timeout);

        if (pret != 0) {
            if (pret == -ECANCELED) {
                return EVENT_ERR_NOT_RUNNING;
            }
            if (pret == -ENOMSG) {
                return EVENT_ERR_QUEUE_FULL;
            }
            return EVENT_ERR_TIMEOUT;
        }

        atomic_inc(&cb->enqueue_count);
        update_high_watermark(&cb->high_watermark, k_msgq_num_used_get(queue));

        return EVENT_OK;
    }

    /* 排空/回灌期间屏蔽 ISR 入队，避免与 scratch 算法并发修改 k_msgq */
    event_queue_reorder_begin(cb);
    uint32_t restore_free_count = 0U;
    event_t  worst_ev = {0};

    for (uint32_t i = 0; i < n; i++) {
        if (k_msgq_get(queue, &cb->drop_lowest_scratch[i], K_NO_WAIT) != 0) {
            LOG_ERR("DROP_LOWEST drain failed at %u, restoring %u events", i, i);
            for (uint32_t j = 0; j < i; j++) {
                if (k_msgq_put(queue, &cb->drop_lowest_scratch[j], K_NO_WAIT) != 0) {
                    LOG_ERR("DROP_LOWEST restore failed at %u during drain recovery", j);
                    cb->drop_lowest_restore_failed[restore_free_count++] = cb->drop_lowest_scratch[j];
                    atomic_inc(&cb->drop_count);
                    event_system_inc_dropped_count();
                }
            }
            event_queue_reorder_end(cb);
            for (uint32_t k = 0U; k < restore_free_count; k++) {
                event_free_queued_payload(&cb->drop_lowest_restore_failed[k]);
            }
            return EVENT_ERR_QUEUE_FULL;
        }
    }

    uint32_t worst = 0U;

    for (uint32_t i = 1U; i < n; i++) {
        if (cb->drop_lowest_scratch[i].priority > cb->drop_lowest_scratch[worst].priority) {
            worst = i;
        }
    }

    if (event->priority > cb->drop_lowest_scratch[worst].priority) {
        for (uint32_t i = 0; i < n; i++) {
            (void) k_msgq_put(queue, &cb->drop_lowest_scratch[i], K_NO_WAIT);
        }
        event_queue_reorder_end(cb);
        atomic_inc(&cb->drop_count);
        event_system_inc_dropped_count();
        LOG_DBG("Queue full, incoming lower than worst queued; drop newest");
        return EVENT_ERR_QUEUE_FULL;
    }

    worst_ev = cb->drop_lowest_scratch[worst];

    atomic_inc(&cb->drop_count);
    event_system_inc_dropped_count();

    for (uint32_t i = 0; i < n; i++) {
        if (i != worst) {
            (void) k_msgq_put(queue, &cb->drop_lowest_scratch[i], K_NO_WAIT);
        }
    }

    int ret = k_msgq_put(queue, event, K_NO_WAIT);

    event_queue_reorder_end(cb);
    /* 被顶替块已脱离队列且 ref_count==1，无人可再访问，解锁后释放 */
    event_free_queued_payload(&worst_ev);

    if (ret != 0) {
        if (ret == -ENOMSG) {
            return EVENT_ERR_QUEUE_FULL;
        }
        return EVENT_ERR_TIMEOUT;
    }

    atomic_inc(&cb->enqueue_count);
    update_high_watermark(&cb->high_watermark, k_msgq_num_used_get(queue));

    return EVENT_OK;
}

/**
 * @brief 获取消息队列属性（常量版本）
 *
 * LOW-2: Zephyr 不提供 k_msgq_get_attrs_const()，因此需要移除 const 修饰
 * 以调用其非 const API。此处的 const cast 是 Zephyr 内核 API 设计限制所致，
 * 函数内部不会修改 queue 内容，调用方可安全地传入 const 指针。
 *
 * @param queue 队列指针（只读）
 * @param attrs 输出：属性结构
 */
static void msgq_get_attrs_const(const struct k_msgq* queue, struct k_msgq_attrs* attrs) {
    k_msgq_get_attrs((struct k_msgq*) queue, attrs);
}

/**
 * @brief 在已持有 g_queue_cb_lock 时查找控制块
 */
static event_queue_cb_t* event_queue_cb_borrow_locked(const struct k_msgq* queue) {
    for (size_t i = 0; i < MAX_QUEUE_CB_ENTRIES; i++) {
        if (g_queue_cb[i].msgq == queue) {
            return &g_queue_cb[i];
        }
    }
    return NULL;
}

/**
 * @brief 线程上下文借用控制块（无锁扫描）
 *
 * 注册表为写少读多（仅 init/deinit 修改，均持 g_queue_cb_lock）。msgq 为
 * 对齐的字长指针，任何受支持架构上的读取都是原子的；并发新增最多让本次
 * 扫描 miss（返回 NULL → EVENT_ERR_INVALID_ARG，调用方稍后重试即可），
 * 并发删除只发生在 deinit，此时调用方本就不应再访问队列（与 ISR 路径
 * event_queue_cb_borrow_isr 的契约一致）。热路径省去每事件两次全局锁
 * 往返（enqueue + dequeue 各一次）。
 */
static event_queue_cb_t* event_queue_cb_borrow(const struct k_msgq* queue) {
    return event_queue_cb_borrow_locked(queue);
}

/**
 * @brief ISR 借用控制块：无锁；调用方须保证 queue 已 init 且未与 deinit 并发
 */
static event_queue_cb_t* event_queue_cb_borrow_isr(const struct k_msgq* queue) {
    return event_queue_cb_borrow_locked(queue);
}

static void event_queue_cb_release(event_queue_cb_t* cb) {
    ARG_UNUSED(cb);
}

/* =============================================================================
 * 队列 API 实现
 * ============================================================================= */

/**
 * @brief 初始化事件队列
 *
 * @param queue 队列指针
 * @param buffer 缓冲区指针
 * @param capacity 队列容量
 * @return EVENT_OK 成功，EVENT_ERR_INVALID_ARG 无效参数
 */
event_status_t event_queue_init(struct k_msgq* queue, void* buffer, size_t capacity) {
    if (queue == NULL || buffer == NULL || capacity == 0) {
        return EVENT_ERR_INVALID_ARG;
    }

    /* SIL-2: HIGH-NEW-2 —— 全程持有 g_queue_cb_lock，确保重复检测、槽位分配、
     * k_msgq_init 和 cb 初始化构成原子序列。任何中途释放锁的做法都会引入 TOCTOU
     * 竞态，允许多线程并发执行 k_msgq_init 导致队列等待队列损坏。 */
    k_mutex_lock(&g_queue_cb_lock, K_FOREVER);

    /* 在持锁状态下完成所有检查与初始化 */
    for (size_t i = 0; i < MAX_QUEUE_CB_ENTRIES; i++) {
        if (g_queue_cb[i].msgq == queue) {
            /* MED-1: 拒绝以不同 capacity 重新初始化已注册队列。
             * 容量决定 drop_lowest_scratch 大小，静默忽略会在 enqueue_drop_lowest 中越界。 */
            if (g_queue_cb[i].capacity != capacity) {
                k_mutex_unlock(&g_queue_cb_lock);
                LOG_ERR("Queue already initialized with capacity %u, refusing %zu", g_queue_cb[i].capacity, capacity);
                return EVENT_ERR_INVALID_ARG;
            }
            k_mutex_unlock(&g_queue_cb_lock);
            LOG_WRN("Queue already initialized, skipping");
            return EVENT_OK; /* 已初始化 */
        }
    }

    /* 寻找空槽 */
    event_queue_cb_t* cb = NULL;
    for (size_t i = 0; i < MAX_QUEUE_CB_ENTRIES; i++) {
        if (g_queue_cb[i].msgq == NULL) {
            cb = &g_queue_cb[i];
            break;
        }
    }

    if (cb == NULL) {
        k_mutex_unlock(&g_queue_cb_lock);
        LOG_ERR("No available queue control block");
        return EVENT_ERR_NO_MEM;
    }

#if defined(CONFIG_EVENT_QUEUE_OVERFLOW_DROP_LOWEST)
    cb->drop_lowest_scratch = (event_t*) k_malloc(capacity * sizeof(event_t));
    if (cb->drop_lowest_scratch == NULL) {
        k_mutex_unlock(&g_queue_cb_lock);
        LOG_ERR("Failed to allocate drop_lowest_scratch for queue");
        return EVENT_ERR_NO_MEM;
    }
    cb->drop_lowest_restore_failed = (event_t*) k_malloc(capacity * sizeof(event_t));
    if (cb->drop_lowest_restore_failed == NULL) {
        k_free(cb->drop_lowest_scratch);
        cb->drop_lowest_scratch = NULL;
        k_mutex_unlock(&g_queue_cb_lock);
        LOG_ERR("Failed to allocate drop_lowest_restore_failed for queue");
        return EVENT_ERR_NO_MEM;
    }
#else
    cb->drop_lowest_scratch = NULL;
    cb->drop_lowest_restore_failed = NULL;
#endif

    k_msgq_init(queue, buffer, sizeof(event_t), capacity);

    cb->msgq = queue;
    cb->capacity = capacity;
    atomic_set(&cb->enqueue_count, 0);
    atomic_set(&cb->dequeue_count, 0);
    atomic_set(&cb->overflow_count, 0);
    atomic_set(&cb->drop_count, 0);
    atomic_set(&cb->high_watermark, 0);
    atomic_set(&cb->reordering, 0);
    atomic_set(&cb->isr_ops_in_flight, 0);
    k_mutex_init(&cb->reorder_lock);

    k_mutex_unlock(&g_queue_cb_lock);

    LOG_DBG("Event queue initialized: capacity=%zu", capacity);
    return EVENT_OK;
}

/**
 * @brief 入队操作
 *
 * @param queue 队列指针
 * @param event 要入队的事件
 * @param policy 溢出处理策略
 * @param timeout 等待超时时间
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 */
event_status_t event_queue_enqueue(struct k_msgq* queue, const event_t* event, queue_overflow_policy_t policy,
                                   k_timeout_t timeout) {
    if (queue == NULL || event == NULL) {
        return EVENT_ERR_INVALID_ARG;
    }

    /* CRIT-NEW-2: ISR 路径无锁扫描注册表（与线程路径一致），
     * 统计经 atomic 更新；reorder 期间经 isr_op_enter 关闭准入。 */
    if (k_is_in_isr()) {
        event_queue_cb_t* cb = event_queue_cb_borrow_isr(queue);
        event_status_t    st;

        if (cb == NULL) {
            return EVENT_ERR_INVALID_ARG;
        }

        if (!event_queue_isr_op_enter(cb)) {
            atomic_inc(&cb->overflow_count);
            event_system_inc_dropped_count();
            return EVENT_ERR_QUEUE_FULL;
        }

        int ret = k_msgq_put(queue, event, K_NO_WAIT);
        if (ret == 0) {
            atomic_inc(&cb->enqueue_count);
            update_high_watermark(&cb->high_watermark, k_msgq_num_used_get(queue));
            st = EVENT_OK;
        } else if (ret == -ENOMSG) {
            atomic_inc(&cb->overflow_count);
            event_system_inc_dropped_count();
            st = EVENT_ERR_QUEUE_FULL;
        } else {
            st = EVENT_ERR_TIMEOUT;
        }

        event_queue_isr_op_exit(cb);
        event_queue_cb_release(cb);
        return st;
    }

    event_queue_cb_t* cb = event_queue_cb_borrow(queue);

    if (cb == NULL) {
        return EVENT_ERR_INVALID_ARG;
    }

    event_status_t st = EVENT_OK;

    if (policy == QUEUE_OVERFLOW_DROP_LOWEST) {
        st = event_queue_ensure_drop_lowest_scratch(cb);
        if (st != EVENT_OK) {
            goto out;
        }
    }

    const bool use_op_lock = event_queue_use_op_lock(cb);

    if (use_op_lock) {
        k_mutex_lock(&cb->reorder_lock, K_FOREVER);
    }

    k_timeout_t put_timeout = timeout;
    if (policy == QUEUE_OVERFLOW_DROP_LOWEST) {
        /* Never block while holding reorder_lock: dequeue also takes the same lock. */
        put_timeout = K_NO_WAIT;
    }

    int ret = event_msgq_put(queue, event, put_timeout);

    if (ret == 0) {
        atomic_inc(&cb->enqueue_count);
        update_high_watermark(&cb->high_watermark, k_msgq_num_used_get(queue));
        if (use_op_lock) {
            k_mutex_unlock(&cb->reorder_lock);
        }
        goto out;
    }

    if (ret == -ECANCELED) {
        if (use_op_lock) {
            k_mutex_unlock(&cb->reorder_lock);
        }
        st = EVENT_ERR_NOT_RUNNING;
        goto out;
    }

    if (ret == -EAGAIN) {
        if (use_op_lock) {
            k_mutex_unlock(&cb->reorder_lock);
        }
        st = EVENT_ERR_TIMEOUT;
        goto out;
    }

    if (ret == -ENOMSG) {
        switch (policy) {
        case QUEUE_OVERFLOW_DROP_NEWEST:
            event_queue_record_drop(cb);
            LOG_DBG("Queue full, dropping newest event");
            st = EVENT_ERR_QUEUE_FULL;
            break;

        case QUEUE_OVERFLOW_DROP_LOWEST:
            st = enqueue_drop_lowest_locked(queue, event, K_NO_WAIT, cb);
            break;

        case QUEUE_OVERFLOW_BLOCK:
            event_queue_record_drop(cb);
            LOG_DBG("Queue full under BLOCK policy (non-blocking timeout)");
            st = EVENT_ERR_QUEUE_FULL;
            break;

        default:
            LOG_ERR("Unknown overflow policy: %d", policy);
            st = EVENT_ERR_INVALID_ARG;
            break;
        }

        if (use_op_lock) {
            k_mutex_unlock(&cb->reorder_lock);
        }
        goto out;
    }

    if (use_op_lock) {
        k_mutex_unlock(&cb->reorder_lock);
    }
    st = EVENT_ERR_INVALID_ARG;

out:
    event_queue_cb_release(cb);
    return st;
}

/**
 * @brief 出队操作
 *
 * @param queue 队列指针
 * @param event 输出：出队的事件
 * @param timeout 等待超时时间
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 */
event_status_t event_queue_dequeue(struct k_msgq* queue, event_t* event, k_timeout_t timeout) {
    if (queue == NULL || event == NULL) {
        return EVENT_ERR_INVALID_ARG;
    }

    event_queue_cb_t* cb = event_queue_cb_borrow(queue);

    if (cb == NULL) {
        LOG_ERR("Queue not initialized via event_queue_init(); refusing dequeue");
        return EVENT_ERR_INVALID_ARG;
    }

    event_status_t st = EVENT_OK;

    if (event_queue_use_op_lock(cb)) {
        k_timepoint_t       end = sys_timepoint_calc(timeout);
        struct k_poll_event poll_event =
            K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, queue);

        while (true) {
            k_mutex_lock(&cb->reorder_lock, K_FOREVER);
            int ret = k_msgq_get(queue, event, K_NO_WAIT);
            k_mutex_unlock(&cb->reorder_lock);

            if (ret == 0) {
                atomic_inc(&cb->dequeue_count);
                goto out;
            }

            if (ret != -ENOMSG) {
                st = EVENT_ERR_TIMEOUT;
                goto out;
            }

            if (K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
                st = EVENT_ERR_QUEUE_EMPTY;
                goto out;
            }

            if (!K_TIMEOUT_EQ(timeout, K_FOREVER) && sys_timepoint_expired(end)) {
                st = EVENT_ERR_TIMEOUT;
                goto out;
            }

            poll_event.state = K_POLL_STATE_NOT_READY;
            k_timeout_t poll_timeout = K_TIMEOUT_EQ(timeout, K_FOREVER) ? K_FOREVER : sys_timepoint_timeout(end);
            ret = k_poll(&poll_event, 1, poll_timeout);
            if (ret == -EAGAIN) {
                st = EVENT_ERR_TIMEOUT;
                goto out;
            }
            if (ret != 0 && ret != -EINTR) {
                st = EVENT_ERR_INVALID_ARG;
                goto out;
            }
        }
    }

    int ret = k_msgq_get(queue, event, timeout);
    if (ret != 0) {
        if (ret == -ENOMSG) {
            st = EVENT_ERR_QUEUE_EMPTY;
        } else {
            st = EVENT_ERR_TIMEOUT;
        }
        goto out;
    }

    atomic_inc(&cb->dequeue_count);

out:
    event_queue_cb_release(cb);
    return st;
}

/**
 * @brief 检查队列是否为空
 *
 * @param queue 队列指针
 * @return true 队列为空，false 队列非空
 */
bool event_queue_is_empty(const struct k_msgq* queue) {
    if (queue == NULL) {
        return true;
    }

    struct k_msgq_attrs attrs;

    msgq_get_attrs_const(queue, &attrs);
    return attrs.used_msgs == 0U;
}

/**
 * @brief 检查队列是否已满
 *
 * @param queue 队列指针
 * @return true 队列已满，false 队列未满
 */
bool event_queue_is_full(const struct k_msgq* queue) {
    if (queue == NULL) {
        return false;
    }

    struct k_msgq_attrs attrs;

    msgq_get_attrs_const(queue, &attrs);
    return attrs.used_msgs >= attrs.max_msgs;
}

/**
 * @brief 获取队列深度
 *
 * @param queue 队列指针
 * @return 队列中的事件数量
 */
uint32_t event_queue_depth(const struct k_msgq* queue) {
    if (queue == NULL) {
        return 0U;
    }

    struct k_msgq_attrs attrs;

    msgq_get_attrs_const(queue, &attrs);
    return attrs.used_msgs;
}

/**
 * @brief 获取队列容量
 *
 * @param queue 队列指针
 * @return 队列最大容量
 */
uint32_t event_queue_capacity(const struct k_msgq* queue) {
    if (queue == NULL) {
        return 0U;
    }

    struct k_msgq_attrs attrs;

    msgq_get_attrs_const(queue, &attrs);
    return attrs.max_msgs;
}

/**
 * @brief 清空队列
 *
 * @param queue 队列指针
 */
void event_queue_purge(struct k_msgq* queue) {
    if (queue == NULL) {
        return;
    }

    event_queue_cb_t* cb = event_queue_cb_borrow(queue);
    event_t           ev;
    uint32_t          purged = 0U;

    /* MED-2: 队列未通过 event_queue_init() 注册时，event_free_queued_payload
     * 可能对裸 k_msgq_put 投递的事件错误地解释 EVENT_FLAG_DATA_DYNAMIC。
     * 此处仍执行清空避免内存泄漏，但记录警告便于诊断违反契约的调用。 */
    if (cb == NULL) {
        LOG_WRN("Purge on queue %p without event_queue_init(); payload free and concurrency safety are caller-managed",
                queue);
        while (k_msgq_get(queue, &ev, K_NO_WAIT) == 0) {
            purged++;
            event_free_queued_payload(&ev);
        }
        LOG_DBG("Event queue purged, dropped=%u", purged);
        return;
    }

    k_mutex_lock(&cb->reorder_lock, K_FOREVER);
    event_queue_reorder_begin(cb);

    while (k_msgq_get(queue, &ev, K_NO_WAIT) == 0) {
        event_free_queued_payload(&ev);
        purged++;
    }

    event_queue_reorder_end(cb);
    k_mutex_unlock(&cb->reorder_lock);

    if (purged > 0U) {
        atomic_add(&cb->drop_count, (atomic_val_t) purged);
    }

    LOG_DBG("Event queue purged, dropped=%u", purged);
    event_queue_cb_release(cb);
}

/**
 * @brief 获取队列统计信息
 *
 * @param queue 队列指针
 * @param stats 输出：统计信息结构
 */
void event_queue_get_stats(const struct k_msgq* queue, queue_stats_t* stats) {
    if (queue == NULL || stats == NULL) {
        return;
    }

    event_queue_cb_t* cb = event_queue_cb_borrow(queue);
    if (cb == NULL) {
        *stats = (queue_stats_t) {0};
        return;
    }

    /* SIL-2: HIGH-3 修复后从 atomic 计数器重建快照。
     * 各计数器独立读取，整体快照非原子（不同字段对应不同瞬时值），
     * 但相比丢失 ISR 路径统计，此妥协可接受。 */
    stats->enqueue_count = (uint32_t) atomic_get(&cb->enqueue_count);
    stats->dequeue_count = (uint32_t) atomic_get(&cb->dequeue_count);
    stats->overflow_count = (uint32_t) atomic_get(&cb->overflow_count);
    stats->drop_count = (uint32_t) atomic_get(&cb->drop_count);
    stats->high_watermark = (uint32_t) atomic_get(&cb->high_watermark);
    event_queue_cb_release(cb);
}

/**
 * @brief 重置队列统计信息
 *
 * @param queue 队列指针
 */
void event_queue_reset_stats(struct k_msgq* queue) {
    if (queue == NULL) {
        return;
    }

    event_queue_cb_t* cb = event_queue_cb_borrow(queue);
    if (cb == NULL) {
        return;
    }

    atomic_set(&cb->enqueue_count, 0);
    atomic_set(&cb->dequeue_count, 0);
    atomic_set(&cb->overflow_count, 0);
    atomic_set(&cb->drop_count, 0);
    atomic_set(&cb->high_watermark, 0);

    LOG_DBG("Queue statistics reset");
    event_queue_cb_release(cb);
}

/**
 * @brief 反初始化事件队列
 *
 * SIL-2: 释放队列初始化时分配的所有动态资源，防止内存泄漏。
 * 包括 DROP_LOWEST scratch 缓冲区和控制块状态清理。
 *
 * @param queue 队列实例
 */
void event_queue_deinit(struct k_msgq* queue) {
    if (queue == NULL) {
        return;
    }

    event_queue_cb_t* cb = event_queue_cb_borrow(queue);
    if (cb == NULL) {
        return;
    }

    /* SIL-2: 先在锁外清空队列残留事件，避免持 g_queue_cb_lock 时调用 purge 造成自死锁
     * （purge 内部经 event_queue_cb_borrow 也会申请 g_queue_cb_lock）。 */
    event_queue_purge(queue);

    /* SIL-2 L-1 修复：持 g_queue_cb_lock 后再释放 scratch / 清零 cb 字段，
     * 消除与 event_queue_init / event_queue_cb_borrow_locked 并发扫描的数据竞争。 */
    k_mutex_lock(&g_queue_cb_lock, K_FOREVER);

    /* SIL-2: 释放 DROP_LOWEST scratch 缓冲区（CRIT-1 修复） */
    if (cb->drop_lowest_scratch != NULL) {
        k_free(cb->drop_lowest_scratch);
        cb->drop_lowest_scratch = NULL;
    }

    /* 清理控制块状态 */
    if (cb->drop_lowest_restore_failed != NULL) {
        k_free(cb->drop_lowest_restore_failed);
        cb->drop_lowest_restore_failed = NULL;
    }

    cb->msgq = NULL;
    cb->capacity = 0;
    atomic_set(&cb->enqueue_count, 0);
    atomic_set(&cb->dequeue_count, 0);
    atomic_set(&cb->overflow_count, 0);
    atomic_set(&cb->drop_count, 0);
    atomic_set(&cb->high_watermark, 0);
    atomic_set(&cb->reordering, 0);
    atomic_set(&cb->isr_ops_in_flight, 0);

    k_mutex_unlock(&g_queue_cb_lock);

    LOG_DBG("Event queue deinitialized");
    event_queue_cb_release(cb);
}
