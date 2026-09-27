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
 * @version 1.2
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            拆分 enqueue/drop_lowest 超长函数；msgq 最后写发布；裸 extern 移入 internal 头
 * 2026-09-27       1.2            zeh            新增优先级批出队（aging 有效优先级）；scratch 预分配扩展至 PRIORITY_DISPATCH
 *
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <errno.h>

#include <zeplod/event_dispatcher.h>
#include "event_queue.h"
#include "event_system_internal.h"

LOG_MODULE_REGISTER(event_queue, CONFIG_SYS_LOG_LEVEL);

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
    struct k_mutex reorder_lock;               /**< scratch 启用时串行化线程侧 msgq 操作 */
    event_t*       drop_lowest_scratch;        /**< 排空/重排临时缓冲区（DROP_LOWEST 与优先级批出队共用） */
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
 * @brief 记录一次成功入队：计数 +1 并刷新水位线（ISR 安全，仅原子操作）
 */
static void event_queue_record_enqueue(event_queue_cb_t* cb, struct k_msgq* queue) {
    atomic_inc(&cb->enqueue_count);
    update_high_watermark(&cb->high_watermark, k_msgq_num_used_get(queue));
}

/**
 * @brief DROP_LOWEST 满队列替换路径的前置校验
 *
 * @param event 待入队事件（防御性 NULL 检查，正常路径已由 event_queue_enqueue 保证非 NULL）
 * @param cb 队列控制块
 * @param attrs 队列属性快照（调用方读取一次，保证排空前后的容量判定一致）
 * @return EVENT_OK 校验通过，EVENT_ERR_INVALID_ARG 校验失败
 */
static event_status_t event_queue_drop_lowest_check(const event_t* event, const event_queue_cb_t* cb,
                                                    const struct k_msgq_attrs* attrs) {
    if (event == NULL) {
        LOG_ERR("Invalid event in enqueue_drop_lowest");
        return EVENT_ERR_INVALID_ARG;
    }

    /* SIL-2: 检查 scratch 缓冲区是否已分配 */
    if (cb->drop_lowest_scratch == NULL || cb->drop_lowest_restore_failed == NULL) {
        LOG_ERR("DROP_LOWEST scratch not allocated");
        return EVENT_ERR_INVALID_ARG;
    }

    if (attrs->max_msgs > cb->capacity) {
        LOG_ERR("Queue capacity exceeds DROP_LOWEST scratch capacity");
        return EVENT_ERR_INVALID_ARG;
    }

    return EVENT_OK;
}

/**
 * @brief 排空队列到 scratch；中途取出失败时回灌已取出的部分
 *
 * @param[out] restore_failed_count 回灌失败、待释放负载的条目数（写入 cb->drop_lowest_restore_failed）
 * @return true 全部取出；false 排空失败（已尽力回灌，待释放条目由调用方在 reorder_end 后释放）
 */
static bool event_queue_drop_lowest_drain(struct k_msgq* queue, event_queue_cb_t* cb, uint32_t n,
                                          uint32_t* restore_failed_count) {
    *restore_failed_count = 0U;

    for (uint32_t i = 0; i < n; i++) {
        if (k_msgq_get(queue, &cb->drop_lowest_scratch[i], K_NO_WAIT) != 0) {
            LOG_ERR("DROP_LOWEST drain failed at %u, restoring %u events", i, i);
            for (uint32_t j = 0; j < i; j++) {
                if (k_msgq_put(queue, &cb->drop_lowest_scratch[j], K_NO_WAIT) != 0) {
                    LOG_ERR("DROP_LOWEST restore failed at %u during drain recovery", j);
                    cb->drop_lowest_restore_failed[(*restore_failed_count)++] = cb->drop_lowest_scratch[j];
                    atomic_inc(&cb->drop_count);
                    event_system_inc_dropped_count();
                }
            }
            return false;
        }
    }
    return true;
}

/**
 * @brief 将 scratch 中的事件回灌进队列（回灌失败静默，与原实现一致）
 *
 * @param skip_index 跳过不下回灌的下标；传 n（越界值）表示全量回灌
 */
static void event_queue_drop_lowest_requeue(struct k_msgq* queue, event_queue_cb_t* cb, uint32_t n,
                                            uint32_t skip_index) {
    for (uint32_t i = 0; i < n; i++) {
        if (i != skip_index) {
            (void) k_msgq_put(queue, &cb->drop_lowest_scratch[i], K_NO_WAIT);
        }
    }
}

/**
 * @brief 满队列替换路径：排空→选最差→丢弃→回灌→入队
 *
 * @pre 已持 cb->reorder_lock；本函数内 reorder_begin/end 屏蔽 ISR 入队
 */
static event_status_t event_queue_drop_lowest_replace_locked(struct k_msgq* queue, const event_t* event,
                                                             event_queue_cb_t* cb, uint32_t n) {
    /* 排空/回灌期间屏蔽 ISR 入队，避免与 scratch 算法并发修改 k_msgq */
    event_queue_reorder_begin(cb);

    uint32_t restore_failed_count = 0U;

    if (!event_queue_drop_lowest_drain(queue, cb, n, &restore_failed_count)) {
        event_queue_reorder_end(cb);
        for (uint32_t k = 0U; k < restore_failed_count; k++) {
            event_free_queued_payload(&cb->drop_lowest_restore_failed[k]);
        }
        return EVENT_ERR_QUEUE_FULL;
    }

    /* priority 数值最大者为最差；相等时取扫描到的第一个（FIFO 最旧） */
    uint32_t worst = 0U;

    for (uint32_t i = 1U; i < n; i++) {
        if (cb->drop_lowest_scratch[i].priority > cb->drop_lowest_scratch[worst].priority) {
            worst = i;
        }
    }

    if (event->priority > cb->drop_lowest_scratch[worst].priority) {
        event_queue_drop_lowest_requeue(queue, cb, n, n);
        event_queue_reorder_end(cb);
        atomic_inc(&cb->drop_count);
        event_system_inc_dropped_count();
        LOG_DBG("Queue full, incoming lower than worst queued; drop newest");
        return EVENT_ERR_QUEUE_FULL;
    }

    event_t worst_ev = cb->drop_lowest_scratch[worst];

    /* 统计先行：被顶替块自 scratch 摘除即视为已丢弃 */
    atomic_inc(&cb->drop_count);
    event_system_inc_dropped_count();

    event_queue_drop_lowest_requeue(queue, cb, n, worst);

    int ret = k_msgq_put(queue, event, K_NO_WAIT);

    event_queue_reorder_end(cb);
    /* 被顶替块已脱离队列且 ref_count==1，无人可再访问，解除 ISR 屏蔽后释放 */
    event_free_queued_payload(&worst_ev);

    if (ret != 0) {
        if (ret == -ENOMSG) {
            return EVENT_ERR_QUEUE_FULL;
        }
        return EVENT_ERR_TIMEOUT;
    }

    event_queue_record_enqueue(cb, queue);

    return EVENT_OK;
}

/**
 * 队列已满时：丢弃队列中优先级最低的一条（priority 数值最大；相等则 FIFO 最旧），再入队 event。
 * 若 event 比队列中最差的一条还差，则丢弃 event（不入队）。
 *
 * @pre 调用方已持有 cb->reorder_lock；cb->drop_lowest_scratch 已分配
 */
static event_status_t enqueue_drop_lowest_locked(struct k_msgq* queue, const event_t* event, k_timeout_t timeout,
                                                 event_queue_cb_t* cb) {
    /* 属性仅读取一次：满/未满判定与替换路径使用同一快照 */
    struct k_msgq_attrs attrs;

    k_msgq_get_attrs(queue, &attrs);

    event_status_t vret = event_queue_drop_lowest_check(event, cb, &attrs);
    if (vret != EVENT_OK) {
        return vret;
    }

    if (attrs.used_msgs < attrs.max_msgs) {
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

        event_queue_record_enqueue(cb, queue);

        return EVENT_OK;
    }

    return event_queue_drop_lowest_replace_locked(queue, event, cb, attrs.used_msgs);
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

#if defined(CONFIG_EVENT_QUEUE_OVERFLOW_DROP_LOWEST) || defined(CONFIG_EVENT_PRIORITY_DISPATCH)
    /* reorder scratch 由 DROP_LOWEST 入队挤占与优先级批出队共用（经 reorder_lock
     * 互斥串行）；任一特性启用即在 init 期一次预分配，去除惰性分配的运行期依赖 */
    cb->drop_lowest_scratch = (event_t*) k_malloc(capacity * sizeof(event_t));
    if (cb->drop_lowest_scratch == NULL) {
        k_mutex_unlock(&g_queue_cb_lock);
        LOG_ERR("Failed to allocate drop_lowest_scratch for queue");
        return EVENT_ERR_NO_MEM;
    }
#if defined(CONFIG_EVENT_QUEUE_OVERFLOW_DROP_LOWEST)
    cb->drop_lowest_restore_failed = (event_t*) k_malloc(capacity * sizeof(event_t));
    if (cb->drop_lowest_restore_failed == NULL) {
        k_free(cb->drop_lowest_scratch);
        cb->drop_lowest_scratch = NULL;
        k_mutex_unlock(&g_queue_cb_lock);
        LOG_ERR("Failed to allocate drop_lowest_restore_failed for queue");
        return EVENT_ERR_NO_MEM;
    }
#else
    /* restore_failed 缓冲仅 DROP_LOWEST drain 失败恢复路径使用；优先级批出队的
     * 回灌失败兜底在门控内直接释放负载（与 purge 同一上下文约束），不依赖此缓冲，
     * 运行期经 ensure_drop_lowest_scratch 仍可补齐（如单测显式指定 DROP_LOWEST） */
    cb->drop_lowest_restore_failed = NULL;
#endif
#else
    cb->drop_lowest_scratch = NULL;
    cb->drop_lowest_restore_failed = NULL;
#endif

    k_msgq_init(queue, buffer, sizeof(event_t), capacity);

    /* 无锁借用方以 cb->msgq != NULL 判定控制块可用：capacity、计数器与 reorder_lock
     * 必须先于 msgq 就绪，msgq 最后写作为发布点，避免借用方读到半初始化控制块 */
    cb->capacity = capacity;
    atomic_set(&cb->enqueue_count, 0);
    atomic_set(&cb->dequeue_count, 0);
    atomic_set(&cb->overflow_count, 0);
    atomic_set(&cb->drop_count, 0);
    atomic_set(&cb->high_watermark, 0);
    atomic_set(&cb->reordering, 0);
    atomic_set(&cb->isr_ops_in_flight, 0);
    k_mutex_init(&cb->reorder_lock);
    cb->msgq = queue;

    k_mutex_unlock(&g_queue_cb_lock);

    LOG_DBG("Event queue initialized: capacity=%zu", capacity);
    return EVENT_OK;
}

/**
 * @brief ISR 入队路径：无锁借用控制块，reorder 期间经准入门控拒收
 *
 * CRIT-NEW-2: ISR 路径无锁扫描注册表（与线程路径一致），
 * 统计经 atomic 更新；reorder 期间经 isr_op_enter 关闭准入。
 */
static event_status_t event_queue_enqueue_isr(struct k_msgq* queue, const event_t* event) {
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
        event_queue_record_enqueue(cb, queue);
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

/**
 * @brief 满队列（-ENOMSG）时按溢出策略处置
 *
 * @pre 调用方已持有 cb->reorder_lock（若启用）
 */
static event_status_t event_queue_enqueue_full_policy(struct k_msgq* queue, const event_t* event,
                                                      queue_overflow_policy_t policy, event_queue_cb_t* cb) {
    switch (policy) {
    case QUEUE_OVERFLOW_DROP_NEWEST:
        event_queue_record_drop(cb);
        LOG_DBG("Queue full, dropping newest event");
        return EVENT_ERR_QUEUE_FULL;

    case QUEUE_OVERFLOW_DROP_LOWEST:
        return enqueue_drop_lowest_locked(queue, event, K_NO_WAIT, cb);

    case QUEUE_OVERFLOW_BLOCK:
        event_queue_record_drop(cb);
        LOG_DBG("Queue full under BLOCK policy (non-blocking timeout)");
        return EVENT_ERR_QUEUE_FULL;

    default:
        LOG_ERR("Unknown overflow policy: %d", policy);
        return EVENT_ERR_INVALID_ARG;
    }
}

/**
 * @brief 线程入队路径：按需取 reorder_lock，一次 put 后按返回值分流
 *
 * @note 加解锁边界与拆分前一致：持锁覆盖 put 与错误处置，分支内不再单独解锁
 */
static event_status_t event_queue_enqueue_thread(struct k_msgq* queue, const event_t* event,
                                                 queue_overflow_policy_t policy, k_timeout_t timeout,
                                                 event_queue_cb_t* cb) {
    event_status_t st = EVENT_OK;

    if (policy == QUEUE_OVERFLOW_DROP_LOWEST) {
        st = event_queue_ensure_drop_lowest_scratch(cb);
        if (st != EVENT_OK) {
            return st;
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
        event_queue_record_enqueue(cb, queue);
    } else if (ret == -ECANCELED) {
        st = EVENT_ERR_NOT_RUNNING;
    } else if (ret == -EAGAIN) {
        st = EVENT_ERR_TIMEOUT;
    } else if (ret == -ENOMSG) {
        st = event_queue_enqueue_full_policy(queue, event, policy, cb);
    } else {
        st = EVENT_ERR_INVALID_ARG;
    }

    if (use_op_lock) {
        k_mutex_unlock(&cb->reorder_lock);
    }

    return st;
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

    if (k_is_in_isr()) {
        return event_queue_enqueue_isr(queue, event);
    }

    event_queue_cb_t* cb = event_queue_cb_borrow(queue);

    if (cb == NULL) {
        return EVENT_ERR_INVALID_ARG;
    }

    event_status_t st = event_queue_enqueue_thread(queue, event, policy, timeout, cb);

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

#if defined(CONFIG_EVENT_PRIORITY_DISPATCH)
/**
 * @brief 计算事件的有效优先级（aging 后）
 *
 * age_steps = (now_ms - timestamp) / CONFIG_EVENT_PRIORITY_AGING_STEP_MS；
 * effective = max(priority - age_steps, 0)。无符号减法天然容忍 timestamp 回绕；
 * timestamp 为 0（手搓未设置）时 effective 归 0，退化为 FIFO 平局次序。
 */
static inline uint32_t event_queue_effective_priority(const event_t* event, uint32_t now_ms) {
    uint32_t age_steps = (now_ms - event->timestamp) / CONFIG_EVENT_PRIORITY_AGING_STEP_MS;
    uint32_t nominal   = (uint32_t) event->priority;

    return (nominal > age_steps) ? (nominal - age_steps) : 0U;
}

/**
 * @brief 优先级批出队：排空→按有效优先级选最优（≤max_out 条）→其余保序回灌
 *
 * 复用 DROP_LOWEST 的 scratch/reorder_lock：线程侧入队与批出队串行，ISR 入队不受
 * 准入门控影响。批出队仅摘取开始时的队列快照，期间到达的 ISR 事件保留在 msgq，
 * 留给下一批排序；回灌时若这些新事件已真实占满容量，则按溢出处置被挤出的旧事件。按
 * 有效优先级（数值最小者最优，平局取下标最小=FIFO 最旧）选出至多 max_out 条
 * 写入 out；其余条目保持原 FIFO 相对次序回灌。与 DROP_LOWEST 入队挤占共用
 * 同一 scratch，经 reorder_lock 互斥串行。
 */
uint32_t event_queue_dequeue_priority_batch(struct k_msgq* queue, event_t* out, uint32_t max_out, uint32_t* remaining) {
    if (queue == NULL || out == NULL || max_out == 0U || k_is_in_isr()) {
        return 0U;
    }

    event_queue_cb_t* cb = event_queue_cb_borrow(queue);

    if (cb == NULL || cb->drop_lowest_scratch == NULL) {
        LOG_ERR("Priority batch dequeue requires event_queue_init() scratch");
        return 0U;
    }

    k_mutex_lock(&cb->reorder_lock, K_FOREVER);

    /* 仅摘取批次开始时的快照。ISR 在此期间可继续入队，留给下一批排序。 */
    uint32_t n = MIN(k_msgq_num_used_get(queue), cb->capacity);

    for (uint32_t i = 0U; i < n; i++) {
        if (k_msgq_get(queue, &cb->drop_lowest_scratch[i], K_NO_WAIT) != 0) {
            n = i;
            break;
        }
    }

    const uint32_t now_ms  = k_uptime_get_32();
    uint32_t       left     = n; /* scratch[0, left) 为尚未选出的条目；移除只收缩右端 */
    uint32_t       selected = 0U;

    while (selected < max_out && left > 0U) {
        /* 在 [0, left) 内选有效优先级最小者；严格小于保证平局取下标最小（FIFO 最旧） */
        uint32_t best     = 0U;
        uint32_t best_eff = event_queue_effective_priority(&cb->drop_lowest_scratch[0], now_ms);

        for (uint32_t i = 1U; i < left; i++) {
            uint32_t eff = event_queue_effective_priority(&cb->drop_lowest_scratch[i], now_ms);

            if (eff < best_eff) {
                best     = i;
                best_eff = eff;
            }
        }

        /* 提档统计先行摘录：有效优先级低于名义优先级说明 aging 生效 */
        if (best_eff < (uint32_t) cb->drop_lowest_scratch[best].priority) {
            event_dispatcher_stats_inc_aged_promotions();
        }

        out[selected] = cb->drop_lowest_scratch[best];

        /* 关闭空隙并保持其余条目的 FIFO 相对次序（保序回灌的前提） */
        if (best + 1U < left) {
            memmove(&cb->drop_lowest_scratch[best], &cb->drop_lowest_scratch[best + 1U],
                    (size_t) (left - best - 1U) * sizeof(event_t));
        }
        left--;
        selected++;
    }

    /* 并发 ISR 事件保留在队列并留给下一批；若它们已实际占满容量，回灌失败时释放
     * 本批旧事件并按真实溢出计数，避免泄漏或虚假成功。 */
    for (uint32_t i = 0U; i < left; i++) {
        if (k_msgq_put(queue, &cb->drop_lowest_scratch[i], K_NO_WAIT) != 0) {
            LOG_ERR("Priority requeue failed at %u; dropping event", i);
            event_free_queued_payload(&cb->drop_lowest_scratch[i]);
            atomic_inc(&cb->drop_count);
            event_queue_record_drop(cb);
        }
    }

    if (remaining != NULL) {
        *remaining = k_msgq_num_used_get(queue);
    }
    if (selected > 0U) {
        atomic_add(&cb->dequeue_count, (atomic_val_t) selected);
    }

    k_mutex_unlock(&cb->reorder_lock);
    event_queue_cb_release(cb);

    return selected;
}
#endif /* CONFIG_EVENT_PRIORITY_DISPATCH */

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
