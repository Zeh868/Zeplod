/**
 * @file module_manager.c
 * @brief 模块管理器全局状态与锁
 *
 * 实现按职责拆分至：
 * - module_manager_lifecycle.c   管理器/模块生命周期
 * - module_manager_registry.c      注册表与查询
 * - module_manager_dependency.c  依赖规划器（mm_dep_planner_*）
 * - module_manager_event.c       事件订阅与分发
 * - module_manager_stats.c       统计与回调
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            快照栈上限 3072 提取为具名宏 MM_MAX_SNAPSHOT_BYTES
 */

#include <zephyr/logging/log.h>
#include <string.h>
#include <zeplod/lock_order.h>
#include <zeplod/module_manager.h>
#include "module_manager_internal.h"

LOG_MODULE_REGISTER(module_manager, CONFIG_SYS_LOG_LEVEL);

module_manager_cb_t g_module_mgr;

atomic_t        g_module_mgr_shutting_down = ATOMIC_INIT(0);
atomic_t        g_module_mgr_initialized = ATOMIC_INIT(0);
static uint32_t g_module_event_cookie_next = 1U;

BUILD_ASSERT(sizeof(module_info_t) * CONFIG_MAX_MODULES <= MM_MAX_SNAPSHOT_BYTES,
             "module_manager snapshot too large for stack; reduce CONFIG_MAX_MODULES or "
             "CONFIG_MODULE_MAX_EVENT_SUBSCRIPTIONS");

void mm_copy_module_name(char* dst, const char* src) {
    if (dst == NULL) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, MM_MODULE_NAME_MAX - 1U);
    dst[MM_MODULE_NAME_MAX - 1U] = '\0';
}

uint32_t allocate_event_cookie_locked(void) {
    if (g_module_event_cookie_next == 0U) {
        return 0U;
    }

    const uint32_t cookie = g_module_event_cookie_next;
    g_module_event_cookie_next = (cookie == UINT32_MAX) ? 0U : (cookie + 1U);
    return cookie;
}

int mm_wait_in_flight_drain(module_info_t* info, uint32_t timeout_ms) {
    if (info == NULL) {
        return MODULE_ERR_INVALID_ARG;
    }
    const uint32_t start = k_uptime_get_32();
    const uint32_t deadline = start + timeout_ms;
    while (atomic_get(&info->in_flight) > 0) {
        if ((int32_t) (k_uptime_get_32() - deadline) >= 0) {
            LOG_ERR("module id=%u: in_flight drain timeout (count=%ld, waited=%u ms)", (unsigned int) info->id,
                    (long) atomic_get(&info->in_flight), (unsigned int) (k_uptime_get_32() - start));
            return MODULE_ERR_TIMEOUT;
        }
        k_msleep(1);
    }
    return MODULE_OK;
}

zepl_state_t module_manager_lifecycle_state_locked(void) {
    return zepl_state_machine_get(&g_module_mgr.lifecycle);
}

void module_manager_lock(void) {
    zepl_lock_enter(ZEP_LOCK_LEVEL_GLOBAL, (uintptr_t) &g_module_mgr.lock);
    k_mutex_lock(&g_module_mgr.lock, K_FOREVER);
}

void module_manager_unlock(void) {
    k_mutex_unlock(&g_module_mgr.lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_GLOBAL, (uintptr_t) &g_module_mgr.lock);
}
