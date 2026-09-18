/**
 * @file app_shell.c
 * @brief 应用层 Zephyr Shell 命令（app status/kv/...）
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-05-28
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-05-28       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            审查整改：include 顺序对齐规范；补文件头
 *                                                 修改日志；kv 命令栈缓冲注明 shell 栈依赖
 *                                                 并加编译期断言；top 刷新次数上限具名
 *
 */

#include <zephyr/shell/shell.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zeplod/app_config.h>
#include <zeplod/app_kv.h>
#include <zeplod/app_main.h>
#include <zeplod/app_version.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system_compat.h>
#include <zeplod/module_manager_compat.h>
#include <zeplod/sys_log.h>
#include <zeplod/sys_memory.h>
#include <zeplod/sys_watchdog.h>

#ifdef CONFIG_DATA_BUS
#include <zeplod/data_bus.h>
#endif

#ifdef CONFIG_OTA_MODULE
#include <zeplod/ota_module.h>
#endif

/* 受 CONFIG_APP_ENABLE_SHELL 控制（该选项 depends on SHELL）；关闭时不注册任何 app 命令。 */
#ifdef CONFIG_APP_ENABLE_SHELL

/*
 * cmd_app_kv_set/cmd_app_kv_get 各自在 shell 线程栈上使用 APP_KV_VALUE_MAX_LEN
 * （默认 512）字节的值缓冲，叠加 shell 子系统自身及 shell_print 格式化的栈开销，
 * 要求 shell 栈不小于 2KB。Zephyr 默认 CONFIG_SHELL_STACK_SIZE=4096 满足；
 * 裁剪栈前须在真机核查栈水位（登记：真机栈核查延期项）。
 */
#define APP_SHELL_MIN_STACK_SIZE 2048
BUILD_ASSERT(CONFIG_SHELL_STACK_SIZE >= APP_SHELL_MIN_STACK_SIZE,
             "CONFIG_SHELL_STACK_SIZE too small for app kv command buffers (need >= 2048)");

/** app top 命令单次允许的最大刷新次数（每次间隔 1s，防止占用 shell 线程过久） */
#define APP_TOP_MAX_COUNT 60

#if APP_CONFIG_ENABLE_APP_KV
static int kv_join_argv(char* out, size_t out_sz, size_t argc, char** argv, size_t first_idx) {
    size_t pos = 0U;

    for (size_t i = first_idx; i < argc; i++) {
        size_t len = strlen(argv[i]);

        if (pos > 0U) {
            if (pos + 1U >= out_sz) {
                return -ENOSPC;
            }
            out[pos++] = ' ';
        }
        if (pos + len >= out_sz) {
            return -ENOSPC;
        }
        memcpy(out + pos, argv[i], len);
        pos += len;
    }
    out[pos] = '\0';
    return 0;
}
#endif /* APP_CONFIG_ENABLE_APP_KV */

static int cmd_app_kv_set(const struct shell* shell, size_t argc, char** argv) {
#if !APP_CONFIG_ENABLE_APP_KV
    shell_print(shell, "app_kv disabled (APP_CONFIG_ENABLE_APP_KV=0)");
    return 0;
#else
    if (argc < 2) {
        shell_print(shell, "Usage: app kv set <key> [<value>...]");
        return -EINVAL;
    }
    char val[APP_KV_VALUE_MAX_LEN];
    if (argc == 2) {
        if (app_kv_set(argv[1], "") != APP_OK) {
            shell_print(shell, "set failed");
            return -EIO;
        }
        return 0;
    }
    if (kv_join_argv(val, sizeof(val), argc, argv, 2) != 0) {
        shell_print(shell, "value too long");
        return -ENOSPC;
    }
    int r = app_kv_set(argv[1], val);
    if (r == APP_ERR_KV_FULL) {
        shell_print(shell, "kv full (max %d entries)", APP_KV_MAX_ENTRIES);
        return -ENOMEM;
    }
    if (r != APP_OK) {
        shell_print(shell, "set failed (%d)", r);
        return -EIO;
    }
    return 0;
#endif
}

static int cmd_app_kv_get(const struct shell* shell, size_t argc, char** argv) {
#if !APP_CONFIG_ENABLE_APP_KV
    shell_print(shell, "app_kv disabled");
    return 0;
#else
    if (argc < 2) {
        shell_print(shell, "Usage: app kv get <key>");
        return -EINVAL;
    }
    char buf[APP_KV_VALUE_MAX_LEN];
    int  r = app_kv_get(argv[1], buf, sizeof(buf));

    if (r == APP_ERR_NOT_FOUND) {
        shell_print(shell, "(not found)");
        return 0;
    }
    if (r != APP_OK) {
        shell_print(shell, "get failed (%d)", r);
        return -EIO;
    }
    shell_print(shell, "%s", buf);
    return 0;
#endif
}

static int cmd_app_kv_del(const struct shell* shell, size_t argc, char** argv) {
#if !APP_CONFIG_ENABLE_APP_KV
    shell_print(shell, "app_kv disabled");
    return 0;
#else
    if (argc < 2) {
        shell_print(shell, "Usage: app kv del <key>");
        return -EINVAL;
    }
    int r = app_kv_remove(argv[1]);

    if (r == APP_ERR_NOT_FOUND) {
        shell_print(shell, "(not found)");
        return 0;
    }
    return r == APP_OK ? 0 : -EIO;
#endif
}

#if APP_CONFIG_ENABLE_APP_KV
static int cmd_app_kv_list_cb(const char* k, const char* v, void* user) {
    shell_print((const struct shell*) user, "  %s = %s", k, v);
    return 0;
}
#endif /* APP_CONFIG_ENABLE_APP_KV */

static int cmd_app_kv_list(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
#if !APP_CONFIG_ENABLE_APP_KV
    shell_print(shell, "app_kv disabled");
    return 0;
#else
    shell_print(shell, "KV entries: %u / %d", (unsigned) app_kv_count(), APP_KV_MAX_ENTRIES);
    (void) app_kv_foreach(cmd_app_kv_list_cb, (void*) shell);
    return 0;
#endif
}

static int cmd_app_kv_clear(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
#if !APP_CONFIG_ENABLE_APP_KV
    shell_print(shell, "app_kv disabled");
    return 0;
#else
    app_kv_clear();
    shell_print(shell, "ok");
    return 0;
#endif
}

static int cmd_app_kv_save(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
#if !APP_CONFIG_ENABLE_APP_KV
    shell_print(shell, "app_kv disabled");
    return 0;
#elif !IS_ENABLED(CONFIG_APP_KV_PERSIST)
    shell_print(shell, "persist off (CONFIG_APP_KV_PERSIST=n)");
    return 0;
#else
    int r = app_kv_save();

    shell_print(shell, "%s (%d)", r == APP_OK ? "saved" : "save failed", r);
    return r == APP_OK ? 0 : -EIO;
#endif
}

static int cmd_app_kv_load(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
#if !APP_CONFIG_ENABLE_APP_KV
    shell_print(shell, "app_kv disabled");
    return 0;
#elif !IS_ENABLED(CONFIG_APP_KV_PERSIST)
    shell_print(shell, "persist off (CONFIG_APP_KV_PERSIST=n)");
    return 0;
#else
    int r = app_kv_load();

    shell_print(shell, "%s (%d)", r == APP_OK ? "loaded" : "load failed", r);
    return r == APP_OK ? 0 : -EIO;
#endif
}

static int cmd_app_status(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    char version_str[VERSION_STRING_MAX_LEN];
    char info_str[VERSION_INFO_STRING_MAX_LEN];

    if (app_version_get_string(version_str, sizeof(version_str)) != APP_OK ||
        app_version_get_info_string(info_str, sizeof(info_str)) != APP_OK) {
        shell_print(shell, "version string error");
        return -EIO;
    }

    shell_print(shell, "Application Status:");
    shell_print(shell, "  Version: %s", version_str);
    shell_print(shell, "  Info: %s", info_str);
    shell_print(shell, "  State: %s", app_is_running() ? "RUNNING" : "STOPPED");
    shell_print(shell, "  Uptime: %u ms", app_get_uptime());
    shell_print(shell, "  Heartbeats: %u", (unsigned int) app_get_heartbeat_count());

    return 0;
}

static int cmd_app_modules(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

#if IS_ENABLED(CONFIG_MODULE_MANAGER)
    shell_print(shell, "Registered Modules:");
    module_compat_dump_info();

    module_compat_stats_t stats;

    module_compat_get_stats(&stats);
    shell_print(shell, "Module Statistics:");
    shell_print(shell, "  Total: %u", (unsigned int) stats.total_modules);
    shell_print(shell, "  Active: %u", (unsigned int) stats.active_modules);
    shell_print(shell, "  Errors: %u", (unsigned int) stats.error_modules);
#else
    shell_print(shell, "Module manager disabled (CONFIG_MODULE_MANAGER=n)");
#endif

    return 0;
}

static int cmd_app_events(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    event_compat_stats_t stats;

    event_compat_get_statistics(&stats);

    shell_print(shell, "Event System Statistics:");
    shell_print(shell, "  Total Events: %u", (unsigned int) stats.total_events);
    shell_print(shell, "  Queue Depth: %u", (unsigned int) stats.queue_depth);
    shell_print(shell, "  Dropped: %u", (unsigned int) stats.dropped_events);

#if APP_CONFIG_ENABLE_STATS
    dispatcher_stats_t dstats;

    event_dispatcher_get_stats(&dstats);
    shell_print(shell, "Dispatcher Statistics:");
    shell_print(shell, "  Processed: %llu", (unsigned long long) dstats.events_processed);
    shell_print(shell, "  Dropped: %llu", (unsigned long long) dstats.events_dropped);
    shell_print(shell, "  Max latency (us): %u", dstats.max_latency_us);
    shell_print(shell, "  Avg latency (us): %u", dstats.avg_latency_us);
    shell_print(shell, "  Processing errors: %u", dstats.processing_errors);
#endif

    return 0;
}

static int cmd_app_memory(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

#if APP_CONFIG_ENABLE_MEMORY_MGR
    shell_print(shell, "Memory Statistics:");
    shell_print(shell, "  Heap Size: %zu bytes", sys_mem_get_heap_size());
    shell_print(shell, "  Free: %zu bytes", sys_mem_get_free_size());
    shell_print(shell, "  Min Free: %zu bytes", sys_mem_get_min_free_size());
#else
    shell_print(shell, "Memory service disabled (CONFIG_SYS_MEMORY_ENABLE=n)");
#endif

    return 0;
}

#if APP_CONFIG_ENABLE_TOP
static void cmd_app_top_once(const struct shell* shell) {
    event_compat_stats_t event_stats;

    event_compat_get_statistics(&event_stats);

    shell_print(shell, "Top Snapshot:");
    shell_print(shell, "  Uptime: %u ms  State: %s", app_get_uptime(), app_is_running() ? "RUNNING" : "STOPPED");
#if IS_ENABLED(CONFIG_MODULE_MANAGER)
    module_compat_stats_t module_stats;

    module_compat_get_stats(&module_stats);
    shell_print(shell, "  Modules: total=%u active=%u errors=%u events=%u dropped=%u",
                (unsigned int) module_stats.total_modules, (unsigned int) module_stats.active_modules,
                (unsigned int) module_stats.error_modules, (unsigned int) module_stats.events_processed,
                (unsigned int) module_stats.events_dropped);
#else
    shell_print(shell, "  Modules: disabled");
#endif
    shell_print(shell, "  Events: total=%u queue=%u dropped=%u", (unsigned int) event_stats.total_events,
                (unsigned int) event_stats.queue_depth, (unsigned int) event_stats.dropped_events);
#if APP_CONFIG_ENABLE_MEMORY_MGR
    shell_print(shell, "  Memory: free=%zu min_free=%zu heap=%zu", sys_mem_get_free_size(), sys_mem_get_min_free_size(),
                sys_mem_get_heap_size());
#else
    shell_print(shell, "  Memory: disabled");
#endif

#ifdef CONFIG_DATA_BUS
    data_bus_overview_t data_bus_stats;
    data_bus_get_overview(&data_bus_stats);
    shell_print(shell,
                "  DataBus: channels=%u active=%u queue=%u pub=%u drop=%u alloc_fail=%u malloc_fb=%u slab_exh=%u",
                (unsigned int) data_bus_stats.channel_count, (unsigned int) data_bus_stats.active_channel_count,
                (unsigned int) data_bus_stats.total_queue_used, (unsigned int) data_bus_stats.total_publish_count,
                (unsigned int) data_bus_stats.total_drop_count, (unsigned int) data_bus_stats.total_alloc_fail_count,
                (unsigned int) data_bus_stats.total_malloc_fallback_count,
                (unsigned int) data_bus_stats.total_slab_exhausted_count);
#else
    shell_print(shell, "  DataBus: disabled");
#endif

#ifdef CONFIG_THREAD_IPC_SERVICE
    shell_print(shell, "  IPC: enabled (per-service pending/shm stats)");
#else
    shell_print(shell, "  IPC: disabled");
#endif

#ifdef CONFIG_SYS_WATCHDOG_ENABLE
    wdt_stats_t wdt_stats;
    sys_wdt_get_stats(&wdt_stats);
    shell_print(shell, "  Watchdog: feeds=%u warnings=%u expires=%u since_feed=%u ms max_interval=%u ms",
                (unsigned int) wdt_stats.feed_count, (unsigned int) wdt_stats.warning_count,
                (unsigned int) wdt_stats.expire_count, (unsigned int) sys_wdt_get_time_since_feed(),
                (unsigned int) wdt_stats.max_feed_interval_ms);
#else
    shell_print(shell, "  Watchdog: disabled");
#endif
}

static int cmd_app_top(const struct shell* shell, size_t argc, char** argv) {
    int count = 1;

    if (argc > 1) {
        count = atoi(argv[1]);
        if (count <= 0) {
            shell_print(shell, "Usage: app top [count]");
            return -EINVAL;
        }
        if (count > APP_TOP_MAX_COUNT) {
            count = APP_TOP_MAX_COUNT;
        }
    }

    for (int i = 0; i < count; i++) {
        cmd_app_top_once(shell);
        if (i + 1 < count) {
            k_sleep(K_SECONDS(1));
        }
    }

    return 0;
}
#endif /* APP_CONFIG_ENABLE_TOP */

static int cmd_app_log(const struct shell* shell, size_t argc, char** argv) {
#if !APP_CONFIG_ENABLE_LOG_DUMP
    shell_print(shell, "Log dump disabled (enable CONFIG_APP_ENABLE_LOG_DUMP in prj.conf or overlay)");
    return 0;
#else
    if (argc > 1) {
        int level = atoi(argv[1]);

        if (level < SYS_LOG_LEVEL_OFF) {
            level = SYS_LOG_LEVEL_OFF;
        } else if (level >= SYS_LOG_LEVEL_MAX) {
            level = SYS_LOG_LEVEL_DBG;
        }
        sys_log_dump((sys_log_level_t) level);
    } else {
        sys_log_dump(SYS_LOG_LEVEL_INF);
    }

    return 0;
#endif
}

static int cmd_app_help(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(shell, "Available commands:");
    shell_print(shell, "  app status     - Show application status");
    shell_print(shell, "  app modules    - Show module information");
    shell_print(shell, "  app events     - Show event statistics");
    shell_print(shell, "  app memory     - Show memory statistics");
#if APP_CONFIG_ENABLE_TOP
    shell_print(shell, "  app top [n]    - Show compact runtime snapshot");
#endif
    shell_print(shell, "  app log [lvl]  - Dump log buffer");
    shell_print(shell, "  app kv ...     - Key-value (set/get/del/list/clear; save/load if persist)");
    shell_print(shell, "  app help       - Show this help");

    return 0;
}

#ifdef CONFIG_OTA_MODULE
/* =============================================================================
 * OTA Shell 命令
 * ============================================================================= */

static const char* ota_state_name(ota_state_t state) {
    switch (state) {
    case OTA_STATE_IDLE:
        return "idle";
    case OTA_STATE_DOWNLOADING:
        return "downloading";
    case OTA_STATE_VERIFYING:
        return "verifying";
    case OTA_STATE_READY_REBOOT:
        return "ready_reboot";
    case OTA_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static int cmd_ota_status(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    ota_state_t st;
    if (ota_module_get_state(&st) != 0) {
        shell_error(shell, "OTA state unavailable");
        return -EIO;
    }
    shell_print(shell, "OTA state: %s", ota_state_name(st));
    return 0;
}

static int cmd_ota_begin(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    int ret = ota_module_begin_update();
    if (ret != 0) {
        shell_error(shell, "begin failed: %d", ret);
        return ret;
    }
    shell_print(shell, "OTA download started (null transport)");
    return 0;
}

static int cmd_ota_abort(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    int ret = ota_module_abort_update();
    if (ret != 0) {
        shell_error(shell, "abort failed: %d", ret);
        return ret;
    }
    shell_print(shell, "OTA session aborted");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ota, SHELL_CMD(status, NULL, "Show OTA state", cmd_ota_status),
                               SHELL_CMD(begin, NULL, "Start OTA download (null transport)", cmd_ota_begin),
                               SHELL_CMD(abort, NULL, "Abort OTA session and return to idle", cmd_ota_abort),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ota, &sub_ota, "OTA commands", NULL);
#endif /* CONFIG_OTA_MODULE */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_app_kv, SHELL_CMD(set, NULL, "Set key [value words...]", cmd_app_kv_set),
                               SHELL_CMD(get, NULL, "Get key", cmd_app_kv_get),
                               SHELL_CMD(del, NULL, "Delete key", cmd_app_kv_del),
                               SHELL_CMD(list, NULL, "List all entries", cmd_app_kv_list),
                               SHELL_CMD(clear, NULL, "Remove all entries", cmd_app_kv_clear),
                               SHELL_CMD(save, NULL, "Write KV to flash (CONFIG_APP_KV_PERSIST)", cmd_app_kv_save),
                               SHELL_CMD(load, NULL, "Reload KV from flash", cmd_app_kv_load), SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_app, SHELL_CMD(status, NULL, "Show application status", cmd_app_status),
                               SHELL_CMD(modules, NULL, "Show registered modules", cmd_app_modules),
                               SHELL_CMD(events, NULL, "Show event statistics", cmd_app_events),
                               SHELL_CMD(memory, NULL, "Show memory statistics", cmd_app_memory),
#if APP_CONFIG_ENABLE_TOP
                               SHELL_CMD(top, NULL, "Show compact runtime snapshot [count]", cmd_app_top),
#endif
                               SHELL_CMD(log, NULL, "Dump log buffer [level]", cmd_app_log),
                               SHELL_CMD(kv, &sub_app_kv, "String key/value store (RAM)", NULL),
                               SHELL_CMD(help, NULL, "Show application help", cmd_app_help), SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(app, &sub_app, "Application commands", NULL);
#endif /* CONFIG_APP_ENABLE_SHELL */
