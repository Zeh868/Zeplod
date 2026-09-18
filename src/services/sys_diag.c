/**
 * @file sys_diag.c
 * @brief 系统诊断服务实现
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            u32_from_size 辅助函数按 CONFIG_SYS_MEMORY_ENABLE 条件编译，消除禁用内存服务配置下的未使用告警
 *
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zeplod/app_config.h>
#include <zeplod/sys_diag.h>

#if defined(CONFIG_EVENT_SYSTEM)
#include <zeplod/event_system.h>
#endif

#if defined(CONFIG_MODULE_MANAGER)
#include <zeplod/module_manager.h>
#endif

#if defined(CONFIG_SYS_MEMORY_ENABLE)
#include <zeplod/sys_memory.h>
#endif

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

LOG_MODULE_REGISTER(sys_diag, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部定义
 * ============================================================================= */

/** Shell 输出缓冲长度 */
#ifndef SYS_DIAG_FORMAT_BUF_SIZE
#define SYS_DIAG_FORMAT_BUF_SIZE 256U
#endif

/* =============================================================================
 * 内部辅助
 * ============================================================================= */

#if defined(CONFIG_SYS_MEMORY_ENABLE)
static uint32_t sys_diag_u32_from_size(size_t value) {
    return (value > (size_t) UINT32_MAX) ? UINT32_MAX : (uint32_t) value;
}
#endif

#if defined(CONFIG_SYS_MEMORY_ENABLE)
static void sys_diag_collect_heap(sys_diag_snapshot_t* out) {
    const size_t heap_total = sys_mem_get_heap_size();
    const size_t heap_free = sys_mem_get_free_size();

    out->heap_free_bytes = sys_diag_u32_from_size(heap_free);
    if (heap_total >= heap_free) {
        out->heap_used_bytes = sys_diag_u32_from_size(heap_total - heap_free);
    }
}
#endif

#if defined(CONFIG_EVENT_SYSTEM)
static void sys_diag_collect_event_queue(sys_diag_snapshot_t* out) {
    uint32_t total_events = 0U;
    uint32_t queue_depth = 0U;
    uint32_t dropped = 0U;

    event_get_statistics(&total_events, &queue_depth, &dropped);
    (void) total_events;
    out->event_queue_depth = queue_depth;
    out->event_queue_capacity = (uint32_t) CONFIG_EVENT_QUEUE_SIZE;
    out->event_dropped_count = dropped;
}
#endif

#if defined(CONFIG_MODULE_MANAGER)
static void sys_diag_collect_modules(sys_diag_snapshot_t* out) {
    module_mgr_stats_t stats;

    module_manager_get_stats(&stats);
    out->module_count = stats.total_modules;
    out->module_running_count = stats.active_modules;
    out->module_error_count = stats.error_modules;
}
#endif

/* =============================================================================
 * 核心 API
 * ============================================================================= */

int sys_diag_init(void) {
    LOG_DBG("sys_diag initialized");
    return 0;
}

int sys_diag_collect(sys_diag_snapshot_t* out) {
    if (out == NULL) {
        return -EINVAL;
    }

    memset(out, 0, sizeof(*out));

#if defined(CONFIG_SYS_MEMORY_ENABLE)
    sys_diag_collect_heap(out);
#endif

#if defined(CONFIG_EVENT_SYSTEM)
    sys_diag_collect_event_queue(out);
#endif

#if defined(CONFIG_MODULE_MANAGER)
    sys_diag_collect_modules(out);
#endif

    out->uptime_ms = k_uptime_get_32();
    return 0;
}

int sys_diag_format(const sys_diag_snapshot_t* snap, char* buf, size_t buf_len) {
    int n;

    if (snap == NULL || buf == NULL || buf_len == 0U) {
        return -EINVAL;
    }

    n = snprintf(buf, buf_len, "diag: heap free=%u used=%u evt=%u/%u drop=%u mod=%u run=%u err=%u up=%ums",
                 snap->heap_free_bytes, snap->heap_used_bytes, snap->event_queue_depth, snap->event_queue_capacity,
                 snap->event_dropped_count, snap->module_count, snap->module_running_count, snap->module_error_count,
                 snap->uptime_ms);
    if (n < 0) {
        return -EIO;
    }
    if ((size_t) n >= buf_len) {
        return -ENOSPC;
    }
    return 0;
}

int sys_diag_export_json(const sys_diag_snapshot_t* snap, char* buf, size_t buf_len) {
    sys_diag_snapshot_t local;
    int                 n;

    if (buf == NULL || buf_len == 0U) {
        return -EINVAL;
    }

    if (snap == NULL) {
        if (sys_diag_collect(&local) != 0) {
            return -EIO;
        }
        snap = &local;
    }

    n = snprintf(buf, buf_len,
                 "{\"heap_free\":%u,\"heap_used\":%u,\"evt_depth\":%u,\"evt_cap\":%u,"
                 "\"evt_drop\":%u,\"mod_total\":%u,\"mod_run\":%u,\"mod_err\":%u,\"uptime_ms\":%u}",
                 snap->heap_free_bytes, snap->heap_used_bytes, snap->event_queue_depth, snap->event_queue_capacity,
                 snap->event_dropped_count, snap->module_count, snap->module_running_count, snap->module_error_count,
                 snap->uptime_ms);
    if (n < 0) {
        return -EIO;
    }
    if ((size_t) n >= buf_len) {
        return -ENOSPC;
    }
    return 0;
}

/* =============================================================================
 * SYS_INIT 自动初始化
 * ============================================================================= */

static int sys_diag_auto_init(void) {
    return sys_diag_init();
}

SYS_INIT(sys_diag_auto_init, POST_KERNEL, APP_INIT_PRIO_SYS_DIAG);

#if defined(CONFIG_SHELL)
/* =============================================================================
 * Shell 命令
 * ============================================================================= */

static int cmd_diag_dump(const struct shell* shell, size_t argc, char** argv) {
    sys_diag_snapshot_t snap;
    char                buf[SYS_DIAG_FORMAT_BUF_SIZE];

    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    if (sys_diag_collect(&snap) != 0) {
        shell_error(shell, "collect failed");
        return -EIO;
    }
    if (sys_diag_format(&snap, buf, sizeof(buf)) != 0) {
        shell_error(shell, "format failed");
        return -EIO;
    }
    shell_print(shell, "%s", buf);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_diag, SHELL_CMD(dump, NULL, "Dump health snapshot", cmd_diag_dump),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(diag, &sub_diag, "System diagnostics", NULL);
#endif /* CONFIG_SHELL */
