/**
 * @file connectivity_module.c
 * @brief 连接管理模块实现
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 3 初始版本
 *
 */

#include <zeplod/connectivity_module.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <string.h>

#include <zeplod/app_config.h>
#include <zeplod/connectivity_backend.h>
#include <zeplod/lock_order.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(connectivity_module, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

/** 连接管理模块运行时控制块 */
typedef struct {
    connectivity_status_t             status;        /**< 链路类型、连接状态与错误码 */
    const connectivity_backend_ops_t* backend;       /**< Kconfig 选定的传输后端 */
    module_status_t                   module_status; /**< 模块生命周期 */
    struct k_mutex                    lock;          /**< 保护 status 与 module_status */
    bool                              lock_ready;
    bool                              events_registered; /**< EVENT_CONNECTIVITY_STATE_CHANGED 已注册 */
} connectivity_module_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static connectivity_module_cb_t g_conn;

/* =============================================================================
 * 前置声明
 * ============================================================================= */

static void conn_lock(void);
static void conn_unlock(void);
static int  conn_register_event_types(void);
static int  conn_publish_state(const connectivity_status_t* st);

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

/** 按 Kconfig 选择后端实现（null 桩 或 Wi-Fi） */
static const connectivity_backend_ops_t* conn_select_backend(void) {
#if IS_ENABLED(CONFIG_CONNECTIVITY_BACKEND_NULL)
    return connectivity_backend_null_get();
#elif IS_ENABLED(CONFIG_CONNECTIVITY_BACKEND_WIFI)
    return connectivity_backend_wifi_get();
#else
    return NULL;
#endif
}

/** 查询后端链路是否已 up（后端或回调缺失时视为 down） */
static bool conn_backend_link_up_locked(void) {
    if (g_conn.backend == NULL || g_conn.backend->is_link_up == NULL) {
        return false;
    }
    return g_conn.backend->is_link_up(g_conn.backend);
}

/* =============================================================================
 * 模块专用 API
 * ============================================================================= */

int connectivity_module_connect(connectivity_link_type_t link_type) {
    connectivity_status_t snap;
    int                   ret;

    conn_lock();

    if (g_conn.module_status == MODULE_STATUS_UNINITIALIZED) {
        conn_unlock();
        return APP_ERR_INIT;
    }
    if (g_conn.module_status != MODULE_STATUS_RUNNING) {
        conn_unlock();
        return APP_ERR_INIT;
    }
    if (g_conn.backend == NULL || g_conn.backend->connect == NULL) {
        conn_unlock();
        return APP_ERR_CONNECTIVITY;
    }
    /* 已连接且后端确认链路 up 时幂等返回 */
    if (g_conn.status.state == CONNECTIVITY_STATE_UP && conn_backend_link_up_locked()) {
        conn_unlock();
        return 0;
    }

    g_conn.status.link_type = link_type;
    conn_set_state_locked(CONNECTIVITY_STATE_CONNECTING, 0);
    snap = g_conn.status;
    conn_unlock();
    (void) conn_publish_state(&snap);

    /* 锁外调用后端 connect，避免后端回调再入 */
    conn_lock();
    ret = g_conn.backend->connect((connectivity_backend_ops_t*) g_conn.backend);
    if (ret != 0) {
        conn_set_state_locked(CONNECTIVITY_STATE_ERROR, ret);
        snap = g_conn.status;
        conn_unlock();
        (void) conn_publish_state(&snap);
        return APP_ERR_CONNECTIVITY;
    }
    if (!conn_backend_link_up_locked()) {
        conn_set_state_locked(CONNECTIVITY_STATE_ERROR, -EIO);
        snap = g_conn.status;
        conn_unlock();
        (void) conn_publish_state(&snap);
        return APP_ERR_CONNECTIVITY;
    }

    conn_set_state_locked(CONNECTIVITY_STATE_UP, 0);
    snap = g_conn.status;
    conn_unlock();
    (void) conn_publish_state(&snap);
    return 0;
}

int connectivity_module_disconnect(void) {
    connectivity_status_t snap;

    conn_lock();

    if (g_conn.backend != NULL && g_conn.backend->disconnect != NULL) {
        (void) g_conn.backend->disconnect((connectivity_backend_ops_t*) g_conn.backend);
    }

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
    /* 惰性校正：缓存为 UP 但后端已 down 时同步为 DOWN */
    if (g_conn.status.state == CONNECTIVITY_STATE_UP && !conn_backend_link_up_locked()) {
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
    g_conn.backend = conn_select_backend();
    if (g_conn.backend == NULL) {
        return APP_ERR_CONNECTIVITY;
    }
    g_conn.module_status = MODULE_STATUS_INITIALIZED;

    if (g_conn.backend->init != NULL) {
        ret = g_conn.backend->init((connectivity_backend_ops_t*) g_conn.backend);
        if (ret != 0) {
            g_conn.module_status = MODULE_STATUS_UNINITIALIZED;
            return APP_ERR_CONNECTIVITY;
        }
    }

    ret = conn_register_event_types();
    if (ret != 0) {
        g_conn.module_status = MODULE_STATUS_UNINITIALIZED;
        return ret;
    }

    LOG_INF("Connectivity module initialized (backend=%s)",
           IS_ENABLED(CONFIG_CONNECTIVITY_BACKEND_WIFI) ? "wifi" : "null");
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

    LOG_INF("Connectivity module started");
    return 0;
}

int connectivity_module_stop(void) {
    (void) connectivity_module_disconnect();

    conn_lock();
    g_conn.module_status = MODULE_STATUS_STOPPED;
    conn_unlock();
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
