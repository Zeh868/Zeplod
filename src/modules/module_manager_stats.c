/**
 * @file module_manager_stats.c
 * @brief 模块管理器统计、调试与状态回调
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-05-28
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-05-28       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            补修改日志段
 */

#include <zephyr/logging/log.h>
#include <string.h>
#include "module_manager_internal.h"

LOG_MODULE_DECLARE(module_manager, CONFIG_SYS_LOG_LEVEL);

void module_mgr_notify_callback(uint32_t module_id, module_mgr_event_t evt) {
    module_mgr_callback_t cb;
    void*                 ud;

    if (!atomic_get(&g_module_mgr_initialized)) {
        return;
    }

    module_manager_lock();
    cb = g_module_mgr.callback;
    ud = g_module_mgr.callback_user_data;
    module_manager_unlock();

    if (cb != NULL) {
        cb(module_id, evt, ud);
    }
}

void module_manager_get_stats(module_mgr_stats_t* stats) {
    if (stats == NULL) {
        return;
    }

    if (!atomic_get(&g_module_mgr_initialized)) {
        (void) memset(stats, 0, sizeof(*stats));
        return;
    }

    module_manager_lock();
    *stats = g_module_mgr.stats;
    module_manager_unlock();
}

void module_manager_reset_stats(void) {
    if (!atomic_get(&g_module_mgr_initialized)) {
        return;
    }

    /* 修复 #9：只清零事件计数器。total_modules / active_modules / error_modules
     * 是当前状态量，与模块注册/启停状态绑定，不应被 reset 抹掉。 */
    module_manager_lock();
    g_module_mgr.stats.events_processed = 0U;
    g_module_mgr.stats.events_dropped = 0U;
    module_manager_unlock();
}

/**
 * @brief 锁内拷贝的快照条目。所有指针字段已替换为值字段。
 */
typedef struct {
    uint32_t        id;
    module_status_t status;
    char            name[MM_MODULE_NAME_MAX];
    uint32_t        version;
} mm_dump_entry_t;

void module_manager_dump_info(void) {
    mm_dump_entry_t snap[CONFIG_MAX_MODULES];
    uint32_t        mod_count;
    uint32_t        active;
    uint32_t        errors;
    int             n = 0;

    if (!atomic_get(&g_module_mgr_initialized)) {
        return;
    }

    module_manager_lock();

    mod_count = g_module_mgr.module_count;
    active = g_module_mgr.stats.active_modules;
    errors = g_module_mgr.stats.error_modules;

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        module_info_t* m = &g_module_mgr.modules[i];
        if (m->status == MODULE_STATUS_UNINITIALIZED) {
            continue;
        }
        /* 修复 #10：在持锁期间把 name/version 拷贝到栈缓冲。
         * 锁外只访问本地栈，interface 指针的释放/模块注销都不再影响本函数。 */
        snap[n].id = m->id;
        snap[n].status = m->status;
        snap[n].version = (m->interface != NULL) ? m->interface->version : 0U;
        mm_copy_module_name(snap[n].name, (m->interface != NULL) ? m->interface->name : NULL);
        n++;
    }

    module_manager_unlock();

    LOG_INF("");
    LOG_INF("=== Module Manager Info ===");
    LOG_INF("Total modules: %u / %d", (unsigned int) mod_count, CONFIG_MAX_MODULES);
    LOG_INF("Active: %u, Errors: %u", (unsigned int) active, (unsigned int) errors);

    for (int i = 0; i < n; i++) {
        const char* status_str;

        switch (snap[i].status) {
        case MODULE_STATUS_INITIALIZING:
            status_str = "INITING";
            break;
        case MODULE_STATUS_INITIALIZED:
            status_str = "INIT";
            break;
        case MODULE_STATUS_RUNNING:
            status_str = "RUNNING";
            break;
        case MODULE_STATUS_STOPPED:
            status_str = "STOPPED";
            break;
        case MODULE_STATUS_ERROR:
            status_str = "ERROR";
            break;
        case MODULE_STATUS_SUSPENDED:
            status_str = "SUSPENDED";
            break;
        default:
            status_str = "UNKNOWN";
            break;
        }

        LOG_INF("  [%u] %s - %s (v%u.%u.%u)", (unsigned int) snap[i].id, snap[i].name[0] != '\0' ? snap[i].name : "N/A",
                status_str, (unsigned int) MODULE_VERSION_MAJOR(snap[i].version),
                (unsigned int) MODULE_VERSION_MINOR(snap[i].version),
                (unsigned int) MODULE_VERSION_PATCH(snap[i].version));
    }

    LOG_INF("=== end ===");
}

void module_manager_set_callback(module_mgr_callback_t callback, void* user_data) {
    if (!atomic_get(&g_module_mgr_initialized)) {
        return;
    }

    module_manager_lock();
    g_module_mgr.callback = callback;
    g_module_mgr.callback_user_data = user_data;
    module_manager_unlock();
}
