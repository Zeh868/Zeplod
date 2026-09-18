/**
 * @file remote_ops.c
 * @brief 远程运维钩子模块实现
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 5 初始版本
 * 2026-06-13       1.1            zeh            生命周期与统计互斥
 * 2026-09-18       1.2            zeh            export_diag 改为锁内取快照、锁外执行
 *                                                 sys_diag/后端导出（外部回调不得在模块
 *                                                 锁内调用）；新增 export_lock 串行化共享
 *                                                 导出缓冲；补文件修改日志；格式对齐仓库基线
 *
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <string.h>

#include <zeplod/app_config.h>
#include <zeplod/lock_order.h>
#include <zeplod/module_manager.h>
#include <zeplod/remote_ops.h>
#include <zeplod/remote_ops_backend.h>
#include <zeplod/sys_diag.h>

LOG_MODULE_REGISTER(remote_ops, CONFIG_SYS_LOG_LEVEL);

/** 远程运维模块运行时控制块 */
typedef struct {
    const remote_ops_backend_ops_t* backend;       /**< 编译期选定的导出后端 */
    remote_ops_status_t             stats;         /**< 导出统计（导出次数/最近载荷长度） */
    module_status_t                 module_status; /**< 模块生命周期 */
    struct k_mutex                  lock;          /**< 保护 backend/stats/module_status（声明
                                                        顺序须在 export_lock 之前，保证锁序 key
                                                        单调递增） */
    struct k_mutex export_lock;                    /**< 串行化导出流程：共享缓冲 g_export_json
                                                        与后端导出调用互斥；仅在 g_remote.lock
                                                        之外持有，不与模块锁嵌套 */
    bool lock_ready;
    bool events_registered;
} remote_ops_cb_t;

static remote_ops_cb_t g_remote;

/** 共享导出缓冲：sys_diag 生成 JSON、后端从此处取走载荷 */
static char g_export_json[CONFIG_REMOTE_OPS_EXPORT_BUF_SIZE];

static void remote_lock(void) {
    zepl_lock_enter(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_remote.lock);
    k_mutex_lock(&g_remote.lock, K_FOREVER);
}

static void remote_unlock(void) {
    k_mutex_unlock(&g_remote.lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_remote.lock);
}

static const remote_ops_backend_ops_t* remote_select_backend(void) {
#if IS_ENABLED(CONFIG_REMOTE_OPS_BACKEND_NULL)
    return remote_ops_backend_null_get();
#else
    return NULL;
#endif
}

static int remote_register_event_types(void) {
    event_status_t st;

    if (g_remote.events_registered) {
        return 0;
    }

    st = event_register_type(EVENT_REMOTE_OPS_DIAG_EXPORTED, "remote_diag");
    if (st != EVENT_OK) {
        LOG_ERR("register EVENT_REMOTE_OPS_DIAG_EXPORTED failed: %d", st);
        return -EIO;
    }

    g_remote.events_registered = true;
    return 0;
}

static int remote_publish_exported(uint32_t payload_len) {
    event_status_t ev_st;

    ev_st =
        event_publish_copy(EVENT_REMOTE_OPS_DIAG_EXPORTED, EVENT_PRIORITY_NORMAL, &payload_len, sizeof(payload_len));
    if (ev_st != EVENT_OK) {
        LOG_WRN("remote_ops export event failed: %d", ev_st);
        return -EIO;
    }

    return 0;
}

int remote_ops_export_diag(void) {
    const remote_ops_backend_ops_t* backend;
    int                             ret;
    uint32_t                        payload_len;

    remote_lock();
    if (g_remote.module_status == MODULE_STATUS_UNINITIALIZED) {
        remote_unlock();
        return APP_ERR_INIT;
    }
    if (g_remote.module_status != MODULE_STATUS_RUNNING) {
        remote_unlock();
        return APP_ERR_INIT;
    }
    backend = g_remote.backend;
    remote_unlock();

    if (backend == NULL || backend->export_diag == NULL) {
        return APP_ERR_REMOTE_OPS;
    }

    /* 对齐 connectivity 模块手法：sys_diag 与后端均为外部实现，可能在内部加锁或回调，
       禁止在持有 g_remote.lock 时调用。export_lock 仅串行化并发导出对 g_export_json
       的交错写入，与模块锁无嵌套关系 */
    zepl_lock_enter(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_remote.export_lock);
    k_mutex_lock(&g_remote.export_lock, K_FOREVER);

    ret = sys_diag_export_json(NULL, g_export_json, sizeof(g_export_json));
    if (ret != 0) {
        k_mutex_unlock(&g_remote.export_lock);
        zepl_lock_exit(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_remote.export_lock);
        return APP_ERR_REMOTE_OPS;
    }

    payload_len = (uint32_t) strlen(g_export_json);
    ret = backend->export_diag((remote_ops_backend_ops_t*) backend, g_export_json, payload_len);

    k_mutex_unlock(&g_remote.export_lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_remote.export_lock);

    if (ret != 0) {
        return APP_ERR_REMOTE_OPS;
    }

    /* 回锁仅做统计记账；导出已成功，即便期间模块被 stop 也如实记录 */
    remote_lock();
    g_remote.stats.export_count++;
    g_remote.stats.last_payload_len = payload_len;
    remote_unlock();

    (void) remote_publish_exported(payload_len);
    return 0;
}

int remote_ops_get_last_export(char* out, size_t out_len) {
    const remote_ops_backend_ops_t* backend;
    int                             ret;

    if (out == NULL || out_len == 0U) {
        return APP_ERR_INVALID_PARAM;
    }

    /* get_last_export 为后端回调（null 后端仅 memcpy），约定为快速非阻塞操作，
       允许在模块锁内调用（与 export_diag 的慢路径由 export_lock 隔离） */
    remote_lock();
    if (g_remote.module_status == MODULE_STATUS_UNINITIALIZED) {
        remote_unlock();
        return APP_ERR_INIT;
    }
    backend = g_remote.backend;
    if (backend == NULL || backend->get_last_export == NULL) {
        remote_unlock();
        return APP_ERR_REMOTE_OPS;
    }
    ret = backend->get_last_export((remote_ops_backend_ops_t*) backend, out, out_len);
    remote_unlock();

    return ret;
}

int remote_ops_get_stats(remote_ops_status_t* out) {
    if (out == NULL) {
        return APP_ERR_INVALID_PARAM;
    }

    remote_lock();
    if (g_remote.module_status == MODULE_STATUS_UNINITIALIZED) {
        remote_unlock();
        return APP_ERR_INIT;
    }
    *out = g_remote.stats;
    remote_unlock();

    return 0;
}

int remote_ops_init(void* config) {
    int ret;

    ARG_UNUSED(config);

    if (g_remote.module_status != MODULE_STATUS_UNINITIALIZED) {
        return 0;
    }

    if (!g_remote.lock_ready) {
        k_mutex_init(&g_remote.lock);
        k_mutex_init(&g_remote.export_lock);
        g_remote.lock_ready = true;
    }

    memset(&g_remote.stats, 0, sizeof(g_remote.stats));

    g_remote.backend = remote_select_backend();
    if (g_remote.backend == NULL) {
        return APP_ERR_REMOTE_OPS;
    }

    g_remote.module_status = MODULE_STATUS_INITIALIZED;

    if (g_remote.backend->init != NULL) {
        ret = g_remote.backend->init((remote_ops_backend_ops_t*) g_remote.backend);
        if (ret != 0) {
            g_remote.module_status = MODULE_STATUS_UNINITIALIZED;
            return APP_ERR_REMOTE_OPS;
        }
    }

    ret = remote_register_event_types();
    if (ret != 0) {
        g_remote.module_status = MODULE_STATUS_UNINITIALIZED;
        return ret;
    }

    LOG_INF("remote_ops initialized (null backend)");
    return 0;
}

int remote_ops_start(void) {
    remote_lock();
    if (g_remote.module_status == MODULE_STATUS_UNINITIALIZED) {
        remote_unlock();
        return APP_ERR_INIT;
    }
    if (g_remote.module_status == MODULE_STATUS_RUNNING) {
        remote_unlock();
        return 0;
    }
    g_remote.module_status = MODULE_STATUS_RUNNING;
    remote_unlock();

    return 0;
}

int remote_ops_stop(void) {
    remote_lock();
    if (g_remote.module_status == MODULE_STATUS_RUNNING) {
        g_remote.module_status = MODULE_STATUS_STOPPED;
    }
    remote_unlock();

    return 0;
}

int remote_ops_shutdown(void) {
    const remote_ops_backend_ops_t* backend;

    remote_lock();
    backend = g_remote.backend;
    g_remote.module_status = MODULE_STATUS_UNINITIALIZED;
    remote_unlock();

    /* 后端 deinit 属外部实现，移到模块锁外调用 */
    if (backend != NULL && backend->deinit != NULL) {
        (void) backend->deinit((remote_ops_backend_ops_t*) backend);
    }

    return 0;
}

void remote_ops_on_event(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
    /* 占位：Phase 5 无入站远程事件；mcumgr 后端可在此订阅连接状态 */
}

module_status_t remote_ops_get_status(void) {
    module_status_t st;

    remote_lock();
    st = g_remote.module_status;
    remote_unlock();

    return st;
}

int remote_ops_control(int cmd, void* arg) {
    ARG_UNUSED(cmd);
    ARG_UNUSED(arg);
    return -ENOTSUP;
}

DECLARE_MODULE_INTERFACE(remote_ops);

#if IS_ENABLED(CONFIG_REMOTE_OPS_MODULE_AUTOINIT)
static int remote_ops_auto_register(void) {
    uint32_t id;

    return module_manager_register(&remote_ops_interface, NULL, &id) ? -EIO : 0;
}

SYS_INIT(remote_ops_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_REMOTE_OPS);
#endif
