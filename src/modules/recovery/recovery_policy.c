/**
 * @file recovery_policy.c
 * @brief 恢复策略模块实现
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 2 初始版本
 * 2026-09-18       1.1            zeh            修复 event->data 匿名联合体误用
 * 2026-09-18       1.2            zeh            复审整改：OTA 钩子回调补事件类型校验，对齐 connectivity 防御模式
 *                                                 （按 DATA_INLINE 标志取内联/指针并本地拷贝）；
 *                                                 restart_count 改原子 CAS 读-改-写
 *
 */

#include <zeplod/recovery_policy.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zeplod/app_config.h>
#include <zeplod/event_system.h>
#include <zeplod/module_manager.h>

#if IS_ENABLED(CONFIG_SYS_FAULT_DUMP_ENABLE)
#include <zeplod/sys_fault_dump.h>
#endif
#if IS_ENABLED(CONFIG_RECOVERY_POLICY_OTA_HOOK) && IS_ENABLED(CONFIG_OTA_MODULE)
#include <zeplod/ota_module.h>
#endif
#if IS_ENABLED(CONFIG_RECOVERY_POLICY_WATCHDOG_HOOK) && IS_ENABLED(CONFIG_SYS_WATCHDOG_ENABLE)
#include <zeplod/sys_watchdog.h>
#endif

LOG_MODULE_REGISTER(recovery_policy, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    module_status_t status;
    /* 重启计数会被模块错误回调与用户 API 并发读-改-写，用原子数组免加锁；
       注意：计数无老化/衰减机制（只能经 RECOVERY_POLICY_CMD_RESET_COUNTS 或重启清零），
       长期运行后可能过早升级为 RESTART_ALL/REBOOT——老化策略登记为技术债延期处理 */
    atomic_t restart_count[CONFIG_MAX_MODULES];
    bool     callback_registered;
#if IS_ENABLED(CONFIG_RECOVERY_POLICY_OTA_HOOK) && IS_ENABLED(CONFIG_OTA_MODULE)
    uint32_t ota_event_subscriber_id;
    bool     ota_event_subscribed;
#endif
} recovery_policy_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static recovery_policy_cb_t g_recovery;

/* =============================================================================
 * 前置声明
 * ============================================================================= */

static recovery_action_t recovery_default_action(void);
static uint32_t          recovery_slot_index(uint32_t module_id);
static int               recovery_restart_module(uint32_t module_id);
static int               recovery_restart_all(void);
static int               recovery_apply_action(uint32_t module_id, recovery_action_t action);
static void              recovery_mgr_callback(uint32_t module_id, module_mgr_event_t event, void* user_data);

/* =============================================================================
 * 内部辅助
 * ============================================================================= */

static recovery_action_t recovery_default_action(void) {
#if IS_ENABLED(CONFIG_RECOVERY_POLICY_ACTION_RESTART_ALL)
    return RECOVERY_ACTION_RESTART_ALL;
#elif IS_ENABLED(CONFIG_RECOVERY_POLICY_ACTION_REBOOT)
    return RECOVERY_ACTION_REBOOT;
#elif IS_ENABLED(CONFIG_RECOVERY_POLICY_ACTION_NONE)
    return RECOVERY_ACTION_NONE;
#else
    return RECOVERY_ACTION_RESTART_MODULE;
#endif
}

static uint32_t recovery_slot_index(uint32_t module_id) {
    if (module_id == 0U || module_id > (uint32_t) CONFIG_MAX_MODULES) {
        return UINT32_MAX;
    }
    return module_id - 1U;
}

static int recovery_restart_module(uint32_t module_id) {
    int ret;

    (void) module_manager_stop_module(module_id);
    (void) module_manager_clear_error_state(module_id);
    ret = module_manager_start_module(module_id);
    if (ret != 0) {
        LOG_WRN("recovery restart module %u failed: %d", module_id, ret);
    }
    return ret;
}

static int recovery_restart_all(void) {
    int stopped;
    int started;

    (void) module_manager_clear_all_error_states();
    stopped = module_manager_stop_all();
    started = module_manager_start_all();

    LOG_INF("recovery restart_all: stopped=%d started=%d", stopped, started);
    return (started < 0) ? started : 0;
}

#if IS_ENABLED(CONFIG_RECOVERY_POLICY_OTA_HOOK) && IS_ENABLED(CONFIG_OTA_MODULE)
static void recovery_ota_event_handler(const event_t* event, void* user_data) {
    ota_progress_t prog_copy;
    uint32_t       ota_id;

    ARG_UNUSED(user_data);

    if (g_recovery.status != MODULE_STATUS_RUNNING || event == NULL) {
        return;
    }
    if (event->type != EVENT_OTA_STATE_CHANGED || event->data_len < sizeof(ota_progress_t)) {
        return;
    }

    /* event->data 为匿名联合体（inline_data/ptr），不能与 NULL 比较或直接转型：
       依 EVENT_FLAG_DATA_INLINE 选择来源后拷贝到本地使用（对齐 connectivity 模块手法） */
    const ota_progress_t* prog = (event->flags & EVENT_FLAG_DATA_INLINE)
                                     ? (const ota_progress_t*) event->data.inline_data
                                     : (const ota_progress_t*) event->data.ptr;
    if (prog == NULL) {
        return;
    }
    prog_copy = *prog; /* event 仅在回调期间有效 */

    if (prog_copy.state != OTA_STATE_ERROR) {
        return;
    }

#if IS_ENABLED(CONFIG_SYS_FAULT_DUMP_ENABLE)
    (void) sys_fault_dump_record(FAULT_DUMP_KIND_OTA_ERROR, &prog_copy, sizeof(prog_copy));
#endif

    ota_id = module_manager_get_id_by_name("ota_module");
    if (ota_id != 0U) {
        (void) recovery_policy_on_module_error(ota_id);
    }
}
#endif

#if IS_ENABLED(CONFIG_RECOVERY_POLICY_WATCHDOG_HOOK) && IS_ENABLED(CONFIG_SYS_WATCHDOG_ENABLE)
static void recovery_wdt_pre_expire(void* user_data) {
    ARG_UNUSED(user_data);

    if (g_recovery.status != MODULE_STATUS_RUNNING) {
        return;
    }

    LOG_WRN("watchdog pre-expire: attempting restart_all");
#if IS_ENABLED(CONFIG_SYS_FAULT_DUMP_ENABLE)
    (void) sys_fault_dump_record(FAULT_DUMP_KIND_WDT_PRE_EXPIRE, NULL, 0U);
#endif
    (void) recovery_restart_all();
}
#endif

static int recovery_apply_action(uint32_t module_id, recovery_action_t action) {
    switch (action) {
    case RECOVERY_ACTION_NONE:
        LOG_WRN("module %u ERROR — recovery disabled", module_id);
        return 0;
    case RECOVERY_ACTION_RESTART_MODULE:
        return recovery_restart_module(module_id);
    case RECOVERY_ACTION_RESTART_ALL:
        return recovery_restart_all();
    case RECOVERY_ACTION_REBOOT:
        LOG_ERR("module %u ERROR — rebooting", module_id);
        sys_reboot(SYS_REBOOT_WARM);
        return 0;
    default:
        return -EINVAL;
    }
}

static void recovery_mgr_callback(uint32_t module_id, module_mgr_event_t event, void* user_data) {
    ARG_UNUSED(user_data);

    if (event != MODULE_MGR_EVENT_ERROR) {
        return;
    }

#if IS_ENABLED(CONFIG_SYS_FAULT_DUMP_ENABLE)
    (void) sys_fault_dump_record(FAULT_DUMP_KIND_MODULE_ERROR, &module_id, sizeof(module_id));
#endif

    (void) recovery_policy_on_module_error(module_id);
}

/* =============================================================================
 * 模块专用 API
 * ============================================================================= */

int recovery_policy_on_module_error(uint32_t module_id) {
    uint32_t          slot;
    recovery_action_t action;
    atomic_t*         cell;
    atomic_val_t      prev;
    int               ret;

    if (g_recovery.status != MODULE_STATUS_RUNNING) {
        return -EINVAL;
    }

    slot = recovery_slot_index(module_id);
    if (slot == UINT32_MAX) {
        return -EINVAL;
    }

    /* 读-改-写用 CAS 循环保证原子：并发错误上报同一模块时计数不丢失、不越过上限语义 */
    cell = &g_recovery.restart_count[slot];
    action = RECOVERY_ACTION_RESTART_MODULE;
    for (;;) {
        prev = atomic_get(cell);
        if ((uint32_t) prev >= (uint32_t) CONFIG_RECOVERY_POLICY_MAX_RESTARTS) {
            action = recovery_default_action();
            if (action == RECOVERY_ACTION_RESTART_MODULE) {
                action = RECOVERY_ACTION_RESTART_ALL;
            }
            break; /* 已达上限：不再递增，直接走升级动作 */
        }
        if (atomic_cas(cell, prev, prev + 1)) {
            break;
        }
    }

    LOG_INF("recovery module %u (attempt %u/%u) action=%d", module_id, (uint32_t) atomic_get(cell),
            (uint32_t) CONFIG_RECOVERY_POLICY_MAX_RESTARTS, (int) action);

    ret = recovery_apply_action(module_id, action);
    return ret;
}

uint32_t recovery_policy_get_restart_count(uint32_t module_id) {
    const uint32_t slot = recovery_slot_index(module_id);

    if (slot == UINT32_MAX) {
        return 0U;
    }
    return (uint32_t) atomic_get(&g_recovery.restart_count[slot]);
}

void recovery_policy_reset_restart_counts(void) {
    memset(g_recovery.restart_count, 0, sizeof(g_recovery.restart_count));
}

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int recovery_policy_init(void* config) {
    ARG_UNUSED(config);

    if (g_recovery.status != MODULE_STATUS_UNINITIALIZED) {
        return 0;
    }

    memset(g_recovery.restart_count, 0, sizeof(g_recovery.restart_count));
    g_recovery.callback_registered = false;
#if IS_ENABLED(CONFIG_RECOVERY_POLICY_OTA_HOOK) && IS_ENABLED(CONFIG_OTA_MODULE)
    g_recovery.ota_event_subscriber_id = 0U;
    g_recovery.ota_event_subscribed = false;
#endif
    g_recovery.status = MODULE_STATUS_INITIALIZED;
    LOG_INF("Recovery policy initialized");
    return 0;
}

int recovery_policy_start(void) {
    if (g_recovery.status == MODULE_STATUS_UNINITIALIZED) {
        return APP_ERR_INIT;
    }
    if (g_recovery.status == MODULE_STATUS_RUNNING) {
        return 0;
    }

    if (!g_recovery.callback_registered) {
        module_manager_set_callback(recovery_mgr_callback, NULL);
        g_recovery.callback_registered = true;
    }

#if IS_ENABLED(CONFIG_RECOVERY_POLICY_OTA_HOOK) && IS_ENABLED(CONFIG_OTA_MODULE)
    if (!g_recovery.ota_event_subscribed) {
        const event_status_t st = event_subscribe(EVENT_OTA_STATE_CHANGED, recovery_ota_event_handler, NULL,
                                                  &g_recovery.ota_event_subscriber_id);
        if (st == EVENT_OK) {
            g_recovery.ota_event_subscribed = true;
        } else {
            LOG_WRN("OTA event subscribe failed: %d", st);
        }
    }
#endif

#if IS_ENABLED(CONFIG_RECOVERY_POLICY_WATCHDOG_HOOK) && IS_ENABLED(CONFIG_SYS_WATCHDOG_ENABLE)
    (void) sys_wdt_set_pre_expire_callback(recovery_wdt_pre_expire, NULL);
#endif

    g_recovery.status = MODULE_STATUS_RUNNING;
    LOG_INF("Recovery policy started");
    return 0;
}

int recovery_policy_stop(void) {
    if (g_recovery.status == MODULE_STATUS_RUNNING) {
#if IS_ENABLED(CONFIG_RECOVERY_POLICY_WATCHDOG_HOOK) && IS_ENABLED(CONFIG_SYS_WATCHDOG_ENABLE)
        (void) sys_wdt_set_pre_expire_callback(NULL, NULL);
#endif
        g_recovery.status = MODULE_STATUS_STOPPED;
    }
    return 0;
}

int recovery_policy_shutdown(void) {
    (void) recovery_policy_stop();

    if (g_recovery.callback_registered) {
        module_manager_set_callback(NULL, NULL);
        g_recovery.callback_registered = false;
    }

#if IS_ENABLED(CONFIG_RECOVERY_POLICY_OTA_HOOK) && IS_ENABLED(CONFIG_OTA_MODULE)
    if (g_recovery.ota_event_subscribed) {
        (void) event_unsubscribe(EVENT_OTA_STATE_CHANGED, g_recovery.ota_event_subscriber_id);
        g_recovery.ota_event_subscribed = false;
        g_recovery.ota_event_subscriber_id = 0U;
    }
#endif

#if IS_ENABLED(CONFIG_RECOVERY_POLICY_WATCHDOG_HOOK) && IS_ENABLED(CONFIG_SYS_WATCHDOG_ENABLE)
    (void) sys_wdt_set_pre_expire_callback(NULL, NULL);
#endif

    g_recovery.status = MODULE_STATUS_UNINITIALIZED;
    LOG_INF("Recovery policy shutdown");
    return 0;
}

void recovery_policy_on_event(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
}

module_status_t recovery_policy_get_status(void) {
    return g_recovery.status;
}

int recovery_policy_control(int cmd, void* arg) {
    ARG_UNUSED(arg);

    switch (cmd) {
    case RECOVERY_POLICY_CMD_RESET_COUNTS:
        recovery_policy_reset_restart_counts();
        return 0;
    default:
        return -ENOTSUP;
    }
}

/* =============================================================================
 * 模块注册
 * ============================================================================= */

DECLARE_MODULE_INTERFACE(recovery_policy);

#if IS_ENABLED(CONFIG_RECOVERY_POLICY_AUTOINIT)
static int recovery_policy_auto_register(void) {
    uint32_t id;

    return module_manager_register(&recovery_policy_interface, NULL, &id) ? -EIO : 0;
}

SYS_INIT(recovery_policy_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_RECOVERY);
#endif
