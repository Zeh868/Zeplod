/**
 * @file module_manager_lifecycle.c
 * @brief 模块管理器与单模块生命周期
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-05-28
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <zeplod/state_machine.h>
#include "module_manager_internal.h"
#include "module_manager_planner_internal.h"

LOG_MODULE_DECLARE(module_manager, CONFIG_SYS_LOG_LEVEL);

int module_manager_init(void) {
    LOG_DBG("Initializing module manager...");

    if (atomic_get(&g_module_mgr_initialized)) {
        LOG_WRN("Module manager already initialized");
        return MODULE_ERR_ALREADY_EXISTS;
    }

    (void) memset(&g_module_mgr, 0, sizeof(g_module_mgr));
    k_mutex_init(&g_module_mgr.lock);
    zepl_state_machine_init(&g_module_mgr.lifecycle, ZEP_STATE_UNINIT);

    atomic_set(&g_module_mgr_shutting_down, 0);

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        g_module_mgr.modules[i].status = MODULE_STATUS_UNINITIALIZED;
        g_module_mgr.modules[i].id = 0U;
    }

    (void) zepl_state_machine_try_transition(&g_module_mgr.lifecycle, ZEP_STATE_INITED);
    g_module_mgr.initialized = true;
    atomic_set(&g_module_mgr_initialized, 1);

    LOG_DBG("Module manager initialized");
    return MODULE_OK;
}

int module_manager_start(void) {
    if (!atomic_get(&g_module_mgr_initialized)) {
        LOG_ERR("Module manager not initialized");
        return MODULE_ERR_NOT_INITIALIZED;
    }

    module_manager_lock();
    zepl_state_t state = module_manager_lifecycle_state_locked();

    if (state == ZEP_STATE_RUNNING) {
        module_manager_unlock();
        LOG_WRN("Module manager already running");
        return MODULE_ERR_ALREADY_RUNNING;
    }

    if (state == ZEP_STATE_UNINIT || state == ZEP_STATE_ERROR || state == ZEP_STATE_STOPPING) {
        module_manager_unlock();
        LOG_ERR("Module manager is not in a startable state: %s", zepl_state_name(state));
        return MODULE_ERR_INVALID_ARG;
    }

    if (zepl_state_machine_try_transition(&g_module_mgr.lifecycle, ZEP_STATE_STARTING) != 0 ||
        zepl_state_machine_try_transition(&g_module_mgr.lifecycle, ZEP_STATE_RUNNING) != 0) {
        module_manager_unlock();
        LOG_ERR("Failed to transition module manager to RUNNING from %s", zepl_state_name(state));
        return MODULE_ERR_INVALID_ARG;
    }

    g_module_mgr.running = true;
    module_manager_unlock();

    LOG_DBG("Module manager started");
    return MODULE_OK;
}

int module_manager_stop(void) {
    if (!atomic_get(&g_module_mgr_initialized)) {
        LOG_ERR("Module manager not initialized");
        return MODULE_ERR_NOT_INITIALIZED;
    }

    module_manager_lock();
    zepl_state_t state = module_manager_lifecycle_state_locked();

    if (state == ZEP_STATE_UNINIT) {
        module_manager_unlock();
        LOG_ERR("Module manager not initialized");
        return MODULE_ERR_NOT_INITIALIZED;
    }

    if (state == ZEP_STATE_STOPPED) {
        g_module_mgr.running = false;
        module_manager_unlock();
        return MODULE_OK;
    }

    if (state == ZEP_STATE_ERROR) {
        module_manager_unlock();
        LOG_ERR("Module manager is in error state");
        return MODULE_ERR_INVALID_ARG;
    }

    if (state != ZEP_STATE_STOPPING) {
        if (zepl_state_machine_try_transition(&g_module_mgr.lifecycle, ZEP_STATE_STOPPING) != 0) {
            module_manager_unlock();
            LOG_ERR("Failed to transition module manager to STOPPING from %s", zepl_state_name(state));
            return MODULE_ERR_INVALID_ARG;
        }
    }

    g_module_mgr.running = false;
    module_manager_unlock();

    const uint32_t wait_start = k_uptime_get_32();
    for (;;) {
        (void) module_manager_stop_all();

        bool busy = false;
        module_manager_lock();
        for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
            const module_info_t* info = &g_module_mgr.modules[i];
            if (info->status == MODULE_STATUS_INITIALIZING || info->status == MODULE_STATUS_RUNNING ||
                info->op_state != MM_OP_IDLE) {
                busy = true;
                break;
            }
        }
        module_manager_unlock();

        if (!busy) {
            break;
        }
        if ((uint32_t) (k_uptime_get_32() - wait_start) >= MM_DRAIN_TIMEOUT_MS) {
            LOG_ERR("Timed out waiting module operations to stop");
            return MODULE_ERR_TIMEOUT;
        }
        k_msleep(1);
    }

    module_manager_lock();
    (void) zepl_state_machine_try_transition(&g_module_mgr.lifecycle, ZEP_STATE_STOPPED);
    module_manager_unlock();

    LOG_DBG("Module manager stopped");
    return MODULE_OK;
}

int module_manager_shutdown(void) {
    start_order_entry_t entries[CONFIG_MAX_MODULES];
    int                 n = 0;
    int                 result = MODULE_OK;

    LOG_DBG("Shutting down module manager...");

    if (!atomic_get(&g_module_mgr_initialized)) {
        LOG_ERR("Module manager not initialized");
        return MODULE_ERR_NOT_INITIALIZED;
    }

    if (!atomic_cas(&g_module_mgr_shutting_down, 0, 1)) {
        return MODULE_ERR_BUSY;
    }

    module_manager_lock();

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        module_info_t* info = &g_module_mgr.modules[i];

        if (info->status != MODULE_STATUS_UNINITIALIZED && info->interface != NULL) {
            entries[n].id = info->id;
            entries[n].priority = info->interface->priority;
#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
            entries[n].depends_on = info->interface->depends_on;
            entries[n].depends_version_min = info->interface->depends_version_min;
#endif
            n++;
        }
    }

    module_manager_unlock();

#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
    (void) mm_dep_planner_build_stop_order(entries, n);
#else
    mm_dep_planner_sort_priority_asc(entries, n);
    for (int i = 0; i < n / 2; i++) {
        const int           j = n - 1 - i;
        start_order_entry_t t = entries[i];

        entries[i] = entries[j];
        entries[j] = t;
    }
#endif

    const int stop_ret = module_manager_stop();
    if (stop_ret != MODULE_OK) {
        atomic_set(&g_module_mgr_shutting_down, 0);
        return stop_ret;
    }

    /* 等待在 shutdown 开始前已进入的 init/start/stop/unregister 完成。 */
    const uint32_t wait_start = k_uptime_get_32();
    for (;;) {
        bool busy = false;

        module_manager_lock();
        for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
            const module_info_t* info = &g_module_mgr.modules[i];
            if (info->status == MODULE_STATUS_INITIALIZING || info->op_state != MM_OP_IDLE || info->unregistering) {
                busy = true;
                break;
            }
        }
        module_manager_unlock();

        if (!busy) {
            break;
        }
        if ((uint32_t) (k_uptime_get_32() - wait_start) >= MM_DRAIN_TIMEOUT_MS) {
            atomic_set(&g_module_mgr_shutting_down, 0);
            return MODULE_ERR_TIMEOUT;
        }
        k_msleep(1);
    }

    for (int i = 0; i < n; i++) {
        const int ret = module_manager_unregister(entries[i].id);
        if (ret == MODULE_ERR_NOT_FOUND) {
            continue;
        }
        if (ret != MODULE_OK) {
            result = ret;
            if (ret == MODULE_ERR_TIMEOUT || ret == MODULE_ERR_BUSY) {
                atomic_set(&g_module_mgr_shutting_down, 0);
                return ret;
            }
        }
    }

    module_manager_lock();

    if (g_module_mgr.module_count != 0U) {
        module_manager_unlock();
        atomic_set(&g_module_mgr_shutting_down, 0);
        return result != MODULE_OK ? result : MODULE_ERR_BUSY;
    }

    (void) memset(&g_module_mgr.stats, 0, sizeof(g_module_mgr.stats));
    (void) zepl_state_machine_try_transition(&g_module_mgr.lifecycle, ZEP_STATE_UNINIT);
    g_module_mgr.initialized = false;
    atomic_set(&g_module_mgr_initialized, 0);
    g_module_mgr.running = false;

    module_manager_unlock();

    atomic_set(&g_module_mgr_shutting_down, 0);

    LOG_DBG("Module manager shutdown complete");
    return result;
}

int module_manager_start_module(uint32_t module_id) {
    int (*start_fn)(void);
    char        name_buf[MM_MODULE_NAME_MAX];
    const char* name;
    int         ret = MODULE_OK;

    const module_interface_t* captured_iface;
    uint32_t                  captured_gen;

    if (!atomic_get(&g_module_mgr_initialized)) {
        return MODULE_ERR_NOT_INITIALIZED;
    }

    module_manager_lock();

    /* 门控：manager 必须处于 RUNNING 才允许启动单模块（#6）。 */
    const zepl_state_t mgr_state = module_manager_lifecycle_state_locked();
    if (mgr_state != ZEP_STATE_RUNNING || atomic_get(&g_module_mgr_shutting_down) != 0) {
        module_manager_unlock();
        LOG_WRN("start_module rejected: manager state=%s", zepl_state_name(mgr_state));
        return MODULE_ERR_INVALID_STATE;
    }

    module_info_t* info = find_module_by_id_locked(module_id);

    if (info == NULL || info->interface == NULL) {
        module_manager_unlock();
        return MODULE_ERR_NOT_FOUND;
    }

    if (info->unregistering || info->op_state != MM_OP_IDLE) {
        module_manager_unlock();
        return MODULE_ERR_BUSY;
    }

    if (info->status != MODULE_STATUS_INITIALIZED && info->status != MODULE_STATUS_STOPPED) {
        module_manager_unlock();
        return MODULE_ERR_INVALID_ARG;
    }

    /* 捕获快照：回调执行期间，槽位可能被 unregister 并复用。
     * 锁外重入后用 (id, iface, gen) 三元组校验仍是同一「代」模块。 */
    captured_iface = info->interface;
    captured_gen = info->generation;
    start_fn = info->interface->start;
    mm_copy_module_name(name_buf, info->interface->name);
    name = name_buf;

    info->op_state = MM_OP_STARTING;
    atomic_inc(&info->in_flight);
    module_manager_unlock();

    if (start_fn != NULL) {
        ret = start_fn();
    }

    module_manager_lock();

    info = find_module_by_id_locked(module_id);
    if (info == NULL || info->interface != captured_iface || info->generation != captured_gen) {
        /* 槽位已被回收/复用——本操作的返回值与计数修改一律丢弃（#1）。 */
        module_manager_unlock();
        return MODULE_ERR_IO;
    }

    info->op_state = MM_OP_IDLE;

    if (ret != MODULE_OK) {
        LOG_ERR("Module '%s' start failed: %d", name != NULL ? name : "?", ret);
        info->status = MODULE_STATUS_ERROR;
        g_module_mgr.stats.error_modules++;
        atomic_dec(&info->in_flight);
        module_manager_unlock();
        module_mgr_notify_callback(module_id, MODULE_MGR_EVENT_ERROR);
        return ret;
    }

    info->status = MODULE_STATUS_RUNNING;
    g_module_mgr.stats.active_modules++;
    atomic_dec(&info->in_flight);

    module_manager_unlock();

    LOG_DBG("Module started: %s", name != NULL ? name : "?");
    module_mgr_notify_callback(module_id, MODULE_MGR_EVENT_STARTED);
    return MODULE_OK;
}

int module_manager_stop_module(uint32_t module_id) {
    int (*stop_fn)(void);
    int         ret = MODULE_OK;
    char        name_buf[MM_MODULE_NAME_MAX];
    const char* name;

    const module_interface_t* captured_iface;
    uint32_t                  captured_gen;

    if (!atomic_get(&g_module_mgr_initialized)) {
        return MODULE_ERR_NOT_INITIALIZED;
    }

    module_manager_lock();

    module_info_t* info = find_module_by_id_locked(module_id);

    if (info == NULL || info->interface == NULL) {
        module_manager_unlock();
        return MODULE_ERR_NOT_FOUND;
    }

    if (info->unregistering || info->op_state != MM_OP_IDLE) {
        module_manager_unlock();
        return MODULE_ERR_BUSY;
    }

    if (info->status != MODULE_STATUS_RUNNING) {
        module_manager_unlock();
        return MODULE_OK;
    }

    captured_iface = info->interface;
    captured_gen = info->generation;
    stop_fn = info->interface->stop;
    mm_copy_module_name(name_buf, info->interface->name);
    name = name_buf;

    info->op_state = MM_OP_STOPPING;
    atomic_inc(&info->in_flight);
    module_manager_unlock();

    if (stop_fn != NULL) {
        ret = stop_fn();
    }

    module_manager_lock();

    info = find_module_by_id_locked(module_id);
    if (info == NULL || info->interface != captured_iface || info->generation != captured_gen) {
        module_manager_unlock();
        return MODULE_ERR_IO;
    }

    info->op_state = MM_OP_IDLE;

    if (ret != MODULE_OK) {
        LOG_ERR("Module '%s' stop failed: %d", name != NULL ? name : "?", ret);
        if (info->status == MODULE_STATUS_RUNNING) {
            info->status = MODULE_STATUS_ERROR;
            g_module_mgr.stats.error_modules++;
            /* 修复 #9：stop 失败时将模块移出 active 集合，统计与实际状态一致。 */
            if (g_module_mgr.stats.active_modules > 0U) {
                g_module_mgr.stats.active_modules--;
            }
        }
        atomic_dec(&info->in_flight);
        module_manager_unlock();
        module_mgr_notify_callback(module_id, MODULE_MGR_EVENT_ERROR);
        return ret;
    }

    if (info->status == MODULE_STATUS_RUNNING) {
        info->status = MODULE_STATUS_STOPPED;
        if (g_module_mgr.stats.active_modules > 0U) {
            g_module_mgr.stats.active_modules--;
        }
    }
    atomic_dec(&info->in_flight);

    module_manager_unlock();

    LOG_DBG("Module stopped: %s", name != NULL ? name : "?");
    module_mgr_notify_callback(module_id, MODULE_MGR_EVENT_STOPPED);
    return MODULE_OK;
}

int module_manager_clear_error_state(uint32_t module_id) {
    if (!atomic_get(&g_module_mgr_initialized)) {
        return MODULE_ERR_NOT_INITIALIZED;
    }

    module_manager_lock();

    module_info_t* info = find_module_by_id_locked(module_id);
    if (info == NULL || info->interface == NULL) {
        module_manager_unlock();
        return MODULE_ERR_NOT_FOUND;
    }

    if (info->status == MODULE_STATUS_ERROR) {
        info->status = MODULE_STATUS_STOPPED;
        if (g_module_mgr.stats.error_modules > 0U) {
            g_module_mgr.stats.error_modules--;
        }
        LOG_INF("Module '%s' error state cleared", info->interface->name != NULL ? info->interface->name : "?");
    }

    module_manager_unlock();
    return MODULE_OK;
}

int module_manager_clear_all_error_states(void) {
    if (!atomic_get(&g_module_mgr_initialized)) {
        return MODULE_ERR_NOT_INITIALIZED;
    }

    module_manager_lock();

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        module_info_t* info = &g_module_mgr.modules[i];

        if (info->interface == NULL || info->status != MODULE_STATUS_ERROR) {
            continue;
        }

        info->status = MODULE_STATUS_STOPPED;
        if (g_module_mgr.stats.error_modules > 0U) {
            g_module_mgr.stats.error_modules--;
        }
        LOG_INF("Module '%s' error state cleared (batch)", info->interface->name != NULL ? info->interface->name : "?");
    }

    module_manager_unlock();
    return MODULE_OK;
}

/**
 * @brief 检查条目的所有依赖项当前是否处于 RUNNING。
 *
 * 必须在持锁下调用。不修改任何状态，仅查询。
 *
 * @return true 所有依赖 RUNNING 或无依赖；false 任一依赖缺失/未运行。
 */
static bool mm_entry_deps_all_running_locked(const start_order_entry_t* entry) {
#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
    if (entry->depends_on == NULL) {
        return true;
    }
    for (unsigned int di = 0U; di < (unsigned int) CONFIG_MODULE_MANAGER_DEPENDS_LIST_MAX; di++) {
        const char* const depn = entry->depends_on[di];
        if (depn == NULL) {
            return true;
        }
        const uint32_t       did = find_module_id_by_name_locked(depn);
        module_info_t* const dep = find_module_by_id_locked(did);
        if (dep == NULL || dep->status != MODULE_STATUS_RUNNING) {
            return false;
        }
    }
    return true;
#else
    ARG_UNUSED(entry);
    return true;
#endif
}

int module_manager_start_all(void) {
    start_order_entry_t entries[CONFIG_MAX_MODULES];
    int                 n = 0;

    if (!atomic_get(&g_module_mgr_initialized)) {
        return MODULE_ERR_NOT_INITIALIZED;
    }

    module_manager_lock();

    /* 门控（#6）：STOPPING / ERROR 状态不允许批量启动。
     * 拒绝时返回负错误码而非 0，调用方可区分「被拒绝」与「无事可做」。 */
    const zepl_state_t mgr_state = module_manager_lifecycle_state_locked();
    if (mgr_state != ZEP_STATE_RUNNING || atomic_get(&g_module_mgr_shutting_down) != 0) {
        module_manager_unlock();
        LOG_WRN("start_all rejected: manager state=%s", zepl_state_name(mgr_state));
        return MODULE_ERR_INVALID_STATE;
    }

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        module_info_t* m = &g_module_mgr.modules[i];

        if ((m->status == MODULE_STATUS_INITIALIZED || m->status == MODULE_STATUS_STOPPED) && m->interface != NULL) {
            entries[n].id = m->id;
            entries[n].priority = m->interface->priority;
#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
            entries[n].depends_on = m->interface->depends_on;
            entries[n].depends_version_min = m->interface->depends_version_min;
#endif
            n++;
        }
    }

    module_manager_unlock();

#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
    n = mm_dep_planner_build_start_order(entries, n);
#else
    mm_dep_planner_sort_priority_asc(entries, n);
#endif

    int started = 0;

    for (int i = 0; i < n; i++) {
        /* 修复 #5：每个模块启动前实时重检依赖 RUNNING。默认配置下不再依赖
         * CONFIG_MODULE_MANAGER_START_ALL_ABORT_ON_FAILURE 即可保证语义。 */
        bool deps_ok = true;
        module_manager_lock();
        module_info_t* m = find_module_by_id_locked(entries[i].id);
        if (m == NULL) {
            deps_ok = false;
        } else {
            deps_ok = mm_entry_deps_all_running_locked(&entries[i]);
        }
        module_manager_unlock();

        if (!deps_ok) {
            LOG_ERR("start_all: skip id=%u (dep not running or slot lost)", (unsigned int) entries[i].id);
            continue;
        }

        const int ret = module_manager_start_module(entries[i].id);

        if (ret == 0) {
            started++;
        } else if (IS_ENABLED(CONFIG_MODULE_MANAGER_START_ALL_ABORT_ON_FAILURE)) {
            break;
        }
    }

    return started;
}

int module_manager_stop_all(void) {
#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
    start_order_entry_t entries[CONFIG_MAX_MODULES];
#else
    uint32_t ids[CONFIG_MAX_MODULES];
#endif
    int n = 0;

    if (!atomic_get(&g_module_mgr_initialized)) {
        return MODULE_ERR_NOT_INITIALIZED;
    }

    module_manager_lock();

#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        module_info_t* const m = &g_module_mgr.modules[i];

        if (m->status == MODULE_STATUS_RUNNING && m->interface != NULL) {
            entries[n].id = m->id;
            entries[n].priority = m->interface->priority;
            entries[n].depends_on = m->interface->depends_on;
            entries[n].depends_version_min = m->interface->depends_version_min;
            n++;
        }
    }
#else
    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        if (g_module_mgr.modules[i].status == MODULE_STATUS_RUNNING) {
            ids[n++] = g_module_mgr.modules[i].id;
        }
    }
#endif

    module_manager_unlock();

#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
    (void) mm_dep_planner_build_stop_order(entries, n);
#endif

    int stopped = 0;

    for (int i = 0; i < n; i++) {
#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
        if (module_manager_stop_module(entries[i].id) == 0) {
#else
        if (module_manager_stop_module(ids[i]) == 0) {
#endif
            stopped++;
        }
    }

    return stopped;
}

int module_manager_suspend_module(uint32_t module_id) {
    if (!atomic_get(&g_module_mgr_initialized)) {
        return MODULE_ERR_NOT_INITIALIZED;
    }

    module_manager_lock();

    if (module_manager_lifecycle_state_locked() != ZEP_STATE_RUNNING || atomic_get(&g_module_mgr_shutting_down) != 0) {
        module_manager_unlock();
        return MODULE_ERR_INVALID_STATE;
    }

    module_info_t* info = find_module_by_id_locked(module_id);

    if (info == NULL || info->interface == NULL) {
        module_manager_unlock();
        return MODULE_ERR_NOT_FOUND;
    }

    if (info->unregistering || info->op_state != MM_OP_IDLE || atomic_get(&info->draining) != 0) {
        module_manager_unlock();
        return MODULE_ERR_BUSY;
    }

    if (info->status != MODULE_STATUS_RUNNING) {
        module_manager_unlock();
        return MODULE_ERR_INVALID_ARG;
    }

    if (g_module_mgr.stats.active_modules > 0U) {
        g_module_mgr.stats.active_modules--;
    }
    info->status = MODULE_STATUS_SUSPENDED;
    char        name_buf[MM_MODULE_NAME_MAX];
    const char* name;

    mm_copy_module_name(name_buf, info->interface->name);
    name = name_buf;

    module_manager_unlock();

    LOG_DBG("Module suspended: %s", name[0] != '\0' ? name : "?");
    module_mgr_notify_callback(module_id, MODULE_MGR_EVENT_STATUS_CHANGED);
    return MODULE_OK;
}

int module_manager_resume_module(uint32_t module_id) {
    if (!atomic_get(&g_module_mgr_initialized)) {
        return MODULE_ERR_NOT_INITIALIZED;
    }

    module_manager_lock();

    if (module_manager_lifecycle_state_locked() != ZEP_STATE_RUNNING || atomic_get(&g_module_mgr_shutting_down) != 0) {
        module_manager_unlock();
        return MODULE_ERR_INVALID_STATE;
    }

    module_info_t* info = find_module_by_id_locked(module_id);

    if (info == NULL || info->interface == NULL) {
        module_manager_unlock();
        return MODULE_ERR_NOT_FOUND;
    }

    if (info->unregistering || info->op_state != MM_OP_IDLE || atomic_get(&info->draining) != 0) {
        module_manager_unlock();
        return MODULE_ERR_BUSY;
    }

    if (info->status != MODULE_STATUS_SUSPENDED) {
        module_manager_unlock();
        return MODULE_ERR_INVALID_ARG;
    }

    info->status = MODULE_STATUS_RUNNING;
    g_module_mgr.stats.active_modules++;
    char        name_buf[MM_MODULE_NAME_MAX];
    const char* name;

    mm_copy_module_name(name_buf, info->interface->name);
    name = name_buf;

    module_manager_unlock();

    LOG_DBG("Module resumed: %s", name[0] != '\0' ? name : "?");
    module_mgr_notify_callback(module_id, MODULE_MGR_EVENT_STATUS_CHANGED);
    return MODULE_OK;
}
