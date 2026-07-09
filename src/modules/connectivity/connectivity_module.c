/**
 * @file connectivity_module.c
 * @brief 连接管理模块实现
 * @author zeh (china_qzh@163.com)
 * @version 2.0
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 3 初始版本
 * 2026-07-09       2.0            zeh            重构为优先级注册表 + 运行时 failover：
 *                                                 多后端按优先级排序，connect_auto() 择优连接，
 *                                                 活动链路掉线自动重选，高优先级恢复可抢占升级
 *
 */

#include <zeplod/connectivity_module.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zeplod/app_config.h>
#include <zeplod/connectivity_backend.h>
#include <zeplod/lock_order.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(connectivity_module, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

/** 注册表可容纳的后端数量上限（当前 null/wifi/ethernet/cellular 共 4 个） */
#define CONN_BACKEND_MAX              4

/** 抢占去抖阈值：高优先级候选须连续多少次轮询保持可用才实际尝试抢占，避免抖动 */
#define CONN_PREEMPT_STREAK_THRESHOLD 2U

/** 后端描述符：ops + 所属链路类型 + 优先级 + 展示名 */
typedef struct {
    const connectivity_backend_ops_t* ops;       /**< 后端 vtable */
    connectivity_link_type_t          link_type; /**< 该后端对应的链路类型 */
    const char*                       name;      /**< 日志用名称 */
    int                               priority;  /**< 越大优先级越高，connect_auto/failover 择优依据 */
    bool wildcard; /**< true 表示可代表调用方显式指定的任意 link_type（当前仅 null 桩后端具备，
                         用于兼容单 null 后端构建下 connectivity_module_connect() 的既有用法/测试语义） */
} connectivity_backend_desc_t;

/** 连接管理模块运行时控制块 */
typedef struct {
    connectivity_status_t              status;                     /**< 链路类型、连接状态与错误码 */
    connectivity_backend_desc_t        backends[CONN_BACKEND_MAX]; /**< 按 priority 降序排列的后端注册表 */
    int                                backend_count;              /**< 已注册后端数 */
    const connectivity_backend_desc_t* active;                     /**< 当前活动后端；NULL 表示未连接 */
    module_status_t                    module_status;              /**< 模块生命周期 */
    struct k_mutex                     lock;                       /**< 保护 status/backends/active/module_status */
    bool                               lock_ready;
    bool                               events_registered; /**< EVENT_CONNECTIVITY_STATE_CHANGED 已注册 */
#if IS_ENABLED(CONFIG_CONNECTIVITY_FAILOVER)
    struct k_work_delayable            failover_work;     /**< 周期健康轮询 / 抢占升级；仅由本 work 回调自身独占访问 */
    const connectivity_backend_desc_t* preempt_candidate; /**< 抢占去抖：上次发现的高优先级候选 */
    uint8_t                            preempt_streak;    /**< 该候选连续可用的轮询次数 */
    bool                               failover_workq_started; /**< 专用工作队列是否已启动（保证只 start 一次） */
#endif
} connectivity_module_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static connectivity_module_cb_t g_conn;

#if IS_ENABLED(CONFIG_CONNECTIVITY_FAILOVER)
/**
 * failover 专用工作队列及其线程栈。
 *
 * failover 的健康轮询/抢占回调会调用后端 connect()/disconnect()，这些接口可能阻塞数秒
 * （受各后端 connect 超时上限约束，蜂窝可达 30s）。若跑在系统工作队列上，长阻塞会拖住
 * 网络定时器等其他 system work，故这里改用本模块私有队列线程承载，与系统工作队列隔离。
 * 二者均置于 CONFIG_CONNECTIVITY_FAILOVER 内，failover 关闭时不占用任何 RAM。
 */
static struct k_work_q g_conn_failover_workq;
K_THREAD_STACK_DEFINE(conn_failover_stack, CONFIG_CONNECTIVITY_FAILOVER_WORKQ_STACK_SIZE);
#endif

/* =============================================================================
 * 前置声明
 * ============================================================================= */

static void conn_lock(void);
static void conn_unlock(void);
static int  conn_register_event_types(void);
static int  conn_publish_state(const connectivity_status_t* st);
static bool conn_desc_link_up(const connectivity_backend_desc_t* d);
static bool conn_desc_available(const connectivity_backend_desc_t* d);
#if IS_ENABLED(CONFIG_CONNECTIVITY_FAILOVER)
static void conn_failover_work_handler(struct k_work* work);
#endif

/* =============================================================================
 * 锁与内部辅助
 * ============================================================================= */

/** 获取模块锁（RESOURCE 层级） */
static void conn_lock(void) {
    zepl_lock_enter(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_conn.lock);
    k_mutex_lock(&g_conn.lock, K_FOREVER);
}

/** 释放模块锁 */
static void conn_unlock(void) {
    k_mutex_unlock(&g_conn.lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_conn.lock);
}

/** 向事件系统注册连接状态变化类型（幂等） */
static int conn_register_event_types(void) {
    event_status_t st;

    if (g_conn.events_registered) {
        return 0;
    }

    st = event_register_type(EVENT_CONNECTIVITY_STATE_CHANGED, "conn_state");
    if (st != EVENT_OK) {
        LOG_ERR("register EVENT_CONNECTIVITY_STATE_CHANGED failed: %d", st);
        return -EIO;
    }

    g_conn.events_registered = true;
    return 0;
}

/** 发布连接状态快照；须在锁外调用 */
static int conn_publish_state(const connectivity_status_t* st) {
    event_status_t ev_st;

    ev_st = event_publish_copy(EVENT_CONNECTIVITY_STATE_CHANGED, EVENT_PRIORITY_NORMAL, st, sizeof(*st));
    if (ev_st != EVENT_OK) {
        LOG_WRN("connectivity state event publish failed: %d", ev_st);
        return -EIO;
    }
    return 0;
}

/** 在已持锁前提下更新状态字段 */
static void conn_set_state_locked(connectivity_state_t state, int err) {
    g_conn.status.state = state;
    g_conn.status.error_code = err;
}

/**
 * @brief 查询某后端链路是否已 up
 * @note is_link_up 依约定为非阻塞快速状态查询，允许在持有模块锁时调用（见 connectivity_backend.h）
 */
static bool conn_desc_link_up(const connectivity_backend_desc_t* d) {
    if (d == NULL || d->ops->is_link_up == NULL) {
        return false;
    }
    return d->ops->is_link_up(d->ops);
}

/**
 * @brief 查询某后端当前是否可用（硬件在位/有载波）
 * @note 未提供 is_available 回调时视为始终可用（如 null 桩后端）
 */
static bool conn_desc_available(const connectivity_backend_desc_t* d) {
    if (d == NULL) {
        return false;
    }
    if (d->ops->is_available == NULL) {
        return true;
    }
    return d->ops->is_available(d->ops);
}

/** 在注册表中查找 link_type 精确匹配的后端；未命中返回 NULL */
static const connectivity_backend_desc_t* conn_find_by_link_type(connectivity_link_type_t link_type) {
    int i;

    for (i = 0; i < g_conn.backend_count; i++) {
        if (g_conn.backends[i].link_type == link_type) {
            return &g_conn.backends[i];
        }
    }
    return NULL;
}

/** 在注册表中查找通配后端（当前仅 null 桩后端具备该属性） */
static const connectivity_backend_desc_t* conn_find_wildcard(void) {
    int i;

    for (i = 0; i < g_conn.backend_count; i++) {
        if (g_conn.backends[i].wildcard) {
            return &g_conn.backends[i];
        }
    }
    return NULL;
}

/** 向注册表追加一个已启用的后端（ops 为 NULL 或注册表已满时忽略） */
static void conn_add_backend(const connectivity_backend_ops_t* ops, connectivity_link_type_t link_type,
                             const char* name, int priority, bool wildcard) {
    connectivity_backend_desc_t* d;

    if (ops == NULL || g_conn.backend_count >= CONN_BACKEND_MAX) {
        return;
    }
    d = &g_conn.backends[g_conn.backend_count++];
    d->ops = ops;
    d->link_type = link_type;
    d->name = name;
    d->priority = priority;
    d->wildcard = wildcard;
}

/** 按已启用的 Kconfig 后端构建注册表，并按 priority 降序排序（元素很少，插入排序即可） */
static void conn_build_registry(void) {
    int i, j;

    g_conn.backend_count = 0;

#if IS_ENABLED(CONFIG_CONNECTIVITY_BACKEND_ETHERNET)
    conn_add_backend(connectivity_backend_ethernet_get(), CONNECTIVITY_LINK_ETHERNET, "ethernet",
                     CONFIG_CONNECTIVITY_BACKEND_ETHERNET_PRIORITY, false);
#endif
#if IS_ENABLED(CONFIG_CONNECTIVITY_BACKEND_WIFI)
    conn_add_backend(connectivity_backend_wifi_get(), CONNECTIVITY_LINK_WIFI, "wifi",
                     CONFIG_CONNECTIVITY_BACKEND_WIFI_PRIORITY, false);
#endif
#if IS_ENABLED(CONFIG_CONNECTIVITY_BACKEND_CELLULAR)
    conn_add_backend(connectivity_backend_cellular_get(), CONNECTIVITY_LINK_CELLULAR, "cellular",
                     CONFIG_CONNECTIVITY_BACKEND_CELLULAR_PRIORITY, false);
#endif
#if IS_ENABLED(CONFIG_CONNECTIVITY_BACKEND_NULL)
    conn_add_backend(connectivity_backend_null_get(), CONNECTIVITY_LINK_NONE, "null",
                     CONFIG_CONNECTIVITY_BACKEND_NULL_PRIORITY, true);
#endif

    for (i = 1; i < g_conn.backend_count; i++) {
        connectivity_backend_desc_t key = g_conn.backends[i];

        j = i - 1;
        while (j >= 0 && g_conn.backends[j].priority < key.priority) {
            g_conn.backends[j + 1] = g_conn.backends[j];
            j--;
        }
        g_conn.backends[j + 1] = key;
    }
}

/**
 * @brief 对注册表中每个后端调用 init()
 *
 * 单个后端 init 失败时记录日志并将其从注册表中剔除（不中止整体模块初始化），使其余
 * 后端仍可正常工作；注册表在剔除后保持原有的 priority 降序相对顺序。全部失败（或注册
 * 表为空）时返回错误。
 */
static int conn_init_backends(void) {
    int read, write;

    for (read = 0, write = 0; read < g_conn.backend_count; read++) {
        connectivity_backend_desc_t* d = &g_conn.backends[read];
        int                          ret = 0;

        if (d->ops->init != NULL) {
            ret = d->ops->init((connectivity_backend_ops_t*) d->ops);
        }
        if (ret != 0) {
            LOG_ERR("backend '%s' init failed (%d); removed from registry", d->name, ret);
            continue;
        }
        if (write != read) {
            g_conn.backends[write] = g_conn.backends[read];
        }
        write++;
    }
    g_conn.backend_count = write;

    return (g_conn.backend_count > 0) ? 0 : APP_ERR_CONNECTIVITY;
}

#if IS_ENABLED(CONFIG_CONNECTIVITY_FAILOVER)
/**
 * @brief failover 管理器周期回调：检测活动链路健康状态，必要时重选或抢占升级到更高优先级后端
 *
 * 锁序约定：仅在持锁区间读写 g_conn 状态；对后端 connect()/disconnect() 的调用可能阻塞
 * （如等待 net_mgmt 回调），一律在锁外进行，避免与 net_mgmt 回调线程互相等待造成死锁。
 * preempt_candidate/preempt_streak 仅由本 work 回调独占读写（该 delayable work 任意时刻
 * 至多一个实例在跑），无需模块锁保护。
 */
static void conn_failover_work_handler(struct k_work* work) {
    const connectivity_backend_desc_t* cur;
    bool                               cur_down = false;
#if IS_ENABLED(CONFIG_CONNECTIVITY_FAILOVER_PREEMPT)
    const connectivity_backend_desc_t* candidate = NULL;
    int                                i;
#endif

    ARG_UNUSED(work);

    conn_lock();
    if (g_conn.module_status != MODULE_STATUS_RUNNING) {
        conn_unlock();
        goto reschedule;
    }

    cur = g_conn.active;
    if (cur != NULL) {
        if (!conn_desc_link_up(cur)) {
            cur_down = true;
        }
#if IS_ENABLED(CONFIG_CONNECTIVITY_FAILOVER_PREEMPT)
        else {
            /* 注册表已按 priority 降序排列：从头遍历到当前活动后端为止即可，
               遇到的第一个可用后端就是优先级最高的抢占候选 */
            for (i = 0; i < g_conn.backend_count; i++) {
                const connectivity_backend_desc_t* d = &g_conn.backends[i];

                if (d == cur) {
                    break;
                }
                if (conn_desc_available(d)) {
                    candidate = d;
                    break;
                }
            }
        }
#endif
    }
    conn_unlock();

    if (cur_down) {
        connectivity_status_t snap;

        LOG_WRN("failover: active backend '%s' link down, reselecting", cur->name);

        conn_lock();
        g_conn.active = NULL;
        g_conn.status.link_type = CONNECTIVITY_LINK_NONE;
        conn_set_state_locked(CONNECTIVITY_STATE_DOWN, 0);
        snap = g_conn.status;
        conn_unlock();
        (void) conn_publish_state(&snap);

#if IS_ENABLED(CONFIG_CONNECTIVITY_FAILOVER_PREEMPT)
        g_conn.preempt_candidate = NULL;
        g_conn.preempt_streak = 0;
#endif
        (void) connectivity_module_connect_auto();
        goto reschedule;
    }

#if IS_ENABLED(CONFIG_CONNECTIVITY_FAILOVER_PREEMPT)
    if (candidate == NULL) {
        g_conn.preempt_candidate = NULL;
        g_conn.preempt_streak = 0;
    } else if (candidate == g_conn.preempt_candidate) {
        if (g_conn.preempt_streak < UINT8_MAX) {
            g_conn.preempt_streak++;
        }
    } else {
        g_conn.preempt_candidate = candidate;
        g_conn.preempt_streak = 1;
    }

    /* 简单去抖：候选须连续 CONN_PREEMPT_STREAK_THRESHOLD 次轮询均可用才实际尝试抢占，
       避免硬件在可用/不可用之间抖动时每个轮询周期都触发一次 connect 尝试 */
    if (candidate != NULL && g_conn.preempt_streak >= CONN_PREEMPT_STREAK_THRESHOLD) {
        int ret = candidate->ops->connect((connectivity_backend_ops_t*) candidate->ops);

        if (ret == 0 && conn_desc_link_up(candidate)) {
            connectivity_status_t              snap;
            const connectivity_backend_desc_t* old;

            conn_lock();
            old = g_conn.active;
            g_conn.active = candidate;
            g_conn.status.link_type = candidate->link_type;
            conn_set_state_locked(CONNECTIVITY_STATE_UP, 0);
            snap = g_conn.status;
            conn_unlock();
            (void) conn_publish_state(&snap);

            /* 新链路已确认 up，再断开旧的低优先级链路（make-before-break，避免中断） */
            if (old != NULL && old->ops->disconnect != NULL) {
                (void) old->ops->disconnect((connectivity_backend_ops_t*) old->ops);
            }
            LOG_INF("failover: preempted to higher-priority backend '%s'", candidate->name);
        } else {
            LOG_WRN("failover: preempt attempt on '%s' failed (ret=%d)", candidate->name, ret);
            if (candidate->ops->disconnect != NULL) {
                (void) candidate->ops->disconnect((connectivity_backend_ops_t*) candidate->ops);
            }
        }
        g_conn.preempt_candidate = NULL;
        g_conn.preempt_streak = 0;
    }
#endif /* CONFIG_CONNECTIVITY_FAILOVER_PREEMPT */

reschedule:
    /* 在重新调度前于锁内复核 module_status：connectivity_module_stop() 先置位 STOPPED
       再调用 k_work_cancel_delayable_sync()，故此处一旦看到非 RUNNING 即不再自重新调度，
       与 stop() 的取消调用之间不存在竞态（互斥锁提供的先行发生序保证二者互斥观察一致）。 */
    conn_lock();
    if (g_conn.module_status == MODULE_STATUS_RUNNING) {
        conn_unlock();
        (void) k_work_reschedule_for_queue(&g_conn_failover_workq, &g_conn.failover_work,
                                           K_MSEC(CONFIG_CONNECTIVITY_FAILOVER_POLL_MS));
    } else {
        conn_unlock();
    }
}
#endif /* CONFIG_CONNECTIVITY_FAILOVER */

/* =============================================================================
 * 模块专用 API
 * ============================================================================= */

int connectivity_module_connect(connectivity_link_type_t link_type) {
    connectivity_status_t              snap;
    const connectivity_backend_desc_t* target;
    int                                ret;

    conn_lock();

    if (g_conn.module_status == MODULE_STATUS_UNINITIALIZED) {
        conn_unlock();
        return APP_ERR_INIT;
    }
    if (g_conn.module_status != MODULE_STATUS_RUNNING) {
        conn_unlock();
        return APP_ERR_INIT;
    }

    /* 优先精确匹配 link_type 的已注册后端；未命中时退化到通配后端（当前仅 null），
       兼容单 null 后端构建下调用方传入任意 link_type 标签的既有用法/测试语义 */
    target = conn_find_by_link_type(link_type);
    if (target == NULL) {
        target = conn_find_wildcard();
    }
    if (target == NULL || target->ops->connect == NULL) {
        conn_unlock();
        return APP_ERR_CONNECTIVITY;
    }

    /* 已连接且目标即当前活动后端并确认链路 up 时幂等返回 */
    if (g_conn.status.state == CONNECTIVITY_STATE_UP && g_conn.active == target && conn_desc_link_up(target)) {
        conn_unlock();
        return 0;
    }

    g_conn.status.link_type = link_type;
    conn_set_state_locked(CONNECTIVITY_STATE_CONNECTING, 0);
    snap = g_conn.status;
    conn_unlock();
    (void) conn_publish_state(&snap);

    /* 锁外调用后端 connect（可能阻塞等待 net_mgmt 回调），避免持锁期间被后端事件回调再入 */
    ret = target->ops->connect((connectivity_backend_ops_t*) target->ops);

    conn_lock();
    if (ret != 0 || !conn_desc_link_up(target)) {
        if (g_conn.active == target) {
            g_conn.active = NULL;
        }
        conn_set_state_locked(CONNECTIVITY_STATE_ERROR, (ret != 0) ? ret : -EIO);
        snap = g_conn.status;
        conn_unlock();
        (void) conn_publish_state(&snap);
        return APP_ERR_CONNECTIVITY;
    }

    g_conn.active = target;
    conn_set_state_locked(CONNECTIVITY_STATE_UP, 0);
    snap = g_conn.status;
    conn_unlock();
    (void) conn_publish_state(&snap);
    return 0;
}

int connectivity_module_connect_auto(void) {
    connectivity_status_t              snap;
    const connectivity_backend_desc_t* chosen = NULL;
    int                                i;

    conn_lock();
    if (g_conn.module_status != MODULE_STATUS_RUNNING) {
        conn_unlock();
        return APP_ERR_INIT;
    }
    if (g_conn.backend_count == 0) {
        conn_unlock();
        return APP_ERR_CONNECTIVITY;
    }
    conn_set_state_locked(CONNECTIVITY_STATE_CONNECTING, 0);
    snap = g_conn.status;
    conn_unlock();
    (void) conn_publish_state(&snap);

    /* 注册表已按 priority 降序排列，按序尝试直至首个连接成功且确认 up 的候选 */
    for (i = 0; i < g_conn.backend_count; i++) {
        const connectivity_backend_desc_t* d = &g_conn.backends[i];
        int                                ret;

        if (!conn_desc_available(d) || d->ops->connect == NULL) {
            continue;
        }
        ret = d->ops->connect((connectivity_backend_ops_t*) d->ops);
        if (ret == 0 && conn_desc_link_up(d)) {
            chosen = d;
            break;
        }
        LOG_WRN("connect_auto: backend '%s' failed (ret=%d)", d->name, ret);
        if (d->ops->disconnect != NULL) {
            (void) d->ops->disconnect((connectivity_backend_ops_t*) d->ops);
        }
    }

    conn_lock();
    if (chosen != NULL) {
        g_conn.active = chosen;
        g_conn.status.link_type = chosen->link_type;
        conn_set_state_locked(CONNECTIVITY_STATE_UP, 0);
    } else {
        g_conn.active = NULL;
        g_conn.status.link_type = CONNECTIVITY_LINK_NONE;
        conn_set_state_locked(CONNECTIVITY_STATE_ERROR, -ENOENT);
    }
    snap = g_conn.status;
    conn_unlock();
    (void) conn_publish_state(&snap);

    return (chosen != NULL) ? 0 : APP_ERR_CONNECTIVITY;
}

int connectivity_module_disconnect(void) {
    connectivity_status_t              snap;
    const connectivity_backend_desc_t* d;

    conn_lock();
    d = g_conn.active;
    conn_unlock();

    if (d != NULL && d->ops->disconnect != NULL) {
        (void) d->ops->disconnect((connectivity_backend_ops_t*) d->ops);
    }

    conn_lock();
    g_conn.active = NULL;
    g_conn.status.link_type = CONNECTIVITY_LINK_NONE;
    conn_set_state_locked(CONNECTIVITY_STATE_DOWN, 0);
    snap = g_conn.status;
    conn_unlock();
    (void) conn_publish_state(&snap);
    return 0;
}

int connectivity_module_get_state(connectivity_status_t* out) {
    if (out == NULL) {
        return APP_ERR_INVALID_PARAM;
    }

    conn_lock();
    /* 惰性校正：缓存为 UP 但活动后端已 down（或已被清除）时同步为 DOWN */
    if (g_conn.status.state == CONNECTIVITY_STATE_UP && !conn_desc_link_up(g_conn.active)) {
        g_conn.active = NULL;
        g_conn.status.link_type = CONNECTIVITY_LINK_NONE;
        conn_set_state_locked(CONNECTIVITY_STATE_DOWN, 0);
    }
    *out = g_conn.status;
    conn_unlock();
    return 0;
}

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int connectivity_module_init(void* config) {
    int ret;

    ARG_UNUSED(config);

    if (g_conn.module_status != MODULE_STATUS_UNINITIALIZED) {
        return 0;
    }

    if (!g_conn.lock_ready) {
        k_mutex_init(&g_conn.lock);
        g_conn.lock_ready = true;
    }

    memset(&g_conn.status, 0, sizeof(g_conn.status));
    g_conn.status.state = CONNECTIVITY_STATE_DOWN;
    g_conn.active = NULL;

    conn_build_registry();
    ret = conn_init_backends();
    if (ret != 0) {
        LOG_ERR("no usable connectivity backend after init (registry empty)");
        return APP_ERR_CONNECTIVITY;
    }
    g_conn.module_status = MODULE_STATUS_INITIALIZED;

    ret = conn_register_event_types();
    if (ret != 0) {
        g_conn.module_status = MODULE_STATUS_UNINITIALIZED;
        return ret;
    }

#if IS_ENABLED(CONFIG_CONNECTIVITY_FAILOVER)
    k_work_init_delayable(&g_conn.failover_work, conn_failover_work_handler);
    g_conn.preempt_candidate = NULL;
    g_conn.preempt_streak = 0;

    /* 启动 failover 专用工作队列线程（仅一次，兼容 init 幂等）；后续 shutdown→init
       再启动时 module_status 已回落 UNINITIALIZED，但队列线程一经启动即长驻，用
       failover_workq_started 去重，避免对同一队列重复 k_work_queue_start */
    if (!g_conn.failover_workq_started) {
        struct k_work_queue_config wq_cfg = {
            .name = "conn_failover",
            .no_yield = false,
        };

        k_work_queue_init(&g_conn_failover_workq);
        k_work_queue_start(&g_conn_failover_workq, conn_failover_stack, K_THREAD_STACK_SIZEOF(conn_failover_stack),
                           CONFIG_CONNECTIVITY_FAILOVER_THREAD_PRIORITY, &wq_cfg);
        g_conn.failover_workq_started = true;
    }
#endif

    LOG_INF("Connectivity module initialized (%d backend(s) registered, highest priority='%s')", g_conn.backend_count,
            g_conn.backends[0].name);
    return 0;
}

int connectivity_module_start(void) {
    conn_lock();
    if (g_conn.module_status == MODULE_STATUS_UNINITIALIZED) {
        conn_unlock();
        return APP_ERR_INIT;
    }
    if (g_conn.module_status == MODULE_STATUS_RUNNING) {
        conn_unlock();
        return 0;
    }
    g_conn.module_status = MODULE_STATUS_RUNNING;
    conn_unlock();

#if IS_ENABLED(CONFIG_CONNECTIVITY_FAILOVER)
    (void) k_work_reschedule_for_queue(&g_conn_failover_workq, &g_conn.failover_work,
                                       K_MSEC(CONFIG_CONNECTIVITY_FAILOVER_POLL_MS));
#endif

    LOG_INF("Connectivity module started");
    return 0;
}

int connectivity_module_stop(void) {
    /* 先置位 STOPPED，再取消 failover 轮询：确保并发中的 failover 回调在其重新调度前的
       状态复核（见 conn_failover_work_handler 的 reschedule 标签）一定能看到 STOPPED，
       避免其在本函数取消之后又自行重新调度出一个新的悬挂定时器 */
    conn_lock();
    g_conn.module_status = MODULE_STATUS_STOPPED;
    conn_unlock();

#if IS_ENABLED(CONFIG_CONNECTIVITY_FAILOVER)
    {
        struct k_work_sync sync;

        (void) k_work_cancel_delayable_sync(&g_conn.failover_work, &sync);
    }
#endif

    (void) connectivity_module_disconnect();
    return 0;
}

int connectivity_module_shutdown(void) {
    (void) connectivity_module_stop();

    conn_lock();
    g_conn.module_status = MODULE_STATUS_UNINITIALIZED;
    conn_unlock();
    return 0;
}

/** Phase 3：暂无事件驱动逻辑，占位 */
void connectivity_module_on_event(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
}

module_status_t connectivity_module_get_status(void) {
    module_status_t st;

    conn_lock();
    st = g_conn.module_status;
    conn_unlock();
    return st;
}

int connectivity_module_control(int cmd, void* arg) {
    ARG_UNUSED(cmd);
    ARG_UNUSED(arg);
    return -ENOTSUP;
}

/* =============================================================================
 * 模块注册
 * ============================================================================= */

DECLARE_MODULE_INTERFACE(connectivity_module);

#if IS_ENABLED(CONFIG_CONNECTIVITY_MODULE_AUTOINIT)
static int connectivity_module_auto_register(void) {
    uint32_t id;

    return module_manager_register(&connectivity_module_interface, NULL, &id) ? -EIO : 0;
}

SYS_INIT(connectivity_module_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_CONNECTIVITY);
#endif
