/**
 * @file test_sys_log.c
 * @brief sys_log 单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            dump 顺序用例补行为断言（条目序号
 *                                                 严格递增、dump 只读不改环）
 *
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/ztest.h>
#include <zeplod/sys_log.h>

ZTEST_SUITE(sys_log_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(sys_log_tests, test_console_and_memory_ring) {
    sys_log_config_t cfg = {
        .default_level = SYS_LOG_LEVEL_INF,
        .destinations = SYS_LOG_DEST_CONSOLE | SYS_LOG_DEST_MEMORY,
        .enable_timestamp = false,
        .enable_colors = false,
        .enable_module_name = false,
        .memory_buffer_size = CONFIG_SYS_MEMORY_POOL_SIZE,
    };

    zassert_equal(sys_log_init(&cfg), 0, "sys_log_init failed");
    sys_log_clear_buffer();

    sys_log_print(SYS_LOG_LEVEL_INF, "test", "hello");
    zassert_true(sys_log_get_count() >= 1U, "memory ring should record lines");
}

ZTEST(sys_log_tests, test_memory_destination_off) {
    sys_log_config_t cfg = {
        .default_level = SYS_LOG_LEVEL_INF,
        .destinations = SYS_LOG_DEST_CONSOLE,
        .enable_timestamp = false,
        .enable_colors = false,
        .enable_module_name = false,
        .memory_buffer_size = CONFIG_SYS_MEMORY_POOL_SIZE,
    };

    zassert_equal(sys_log_init(&cfg), 0, "sys_log_init failed");
    sys_log_clear_buffer();

    sys_log_print(SYS_LOG_LEVEL_INF, "test", "no ring");
    zassert_equal(sys_log_get_count(), 0U, "memory disabled: ring count should stay 0");
}

ZTEST(sys_log_tests, test_set_destination_memory_toggle) {
    sys_log_config_t cfg = {
        .default_level = SYS_LOG_LEVEL_INF,
        .destinations = SYS_LOG_DEST_CONSOLE | SYS_LOG_DEST_MEMORY,
        .enable_timestamp = false,
        .enable_colors = false,
        .enable_module_name = false,
        .memory_buffer_size = CONFIG_SYS_MEMORY_POOL_SIZE,
    };

    zassert_equal(sys_log_init(&cfg), 0, "sys_log_init failed");
    sys_log_clear_buffer();

    sys_log_set_destination(SYS_LOG_DEST_MEMORY, false);
    sys_log_print(SYS_LOG_LEVEL_INF, "test", "skip ring");
    zassert_equal(sys_log_get_count(), 0U, "memory off");

    sys_log_set_destination(SYS_LOG_DEST_MEMORY, true);
    sys_log_print(SYS_LOG_LEVEL_INF, "test", "with ring");
    zassert_true(sys_log_get_count() >= 1U, "memory on");
}

ZTEST(sys_log_tests, test_dump_oldest_first_no_repeat) {
    sys_log_config_t cfg = {
        .default_level = SYS_LOG_LEVEL_DBG,
        .destinations = SYS_LOG_DEST_CONSOLE | SYS_LOG_DEST_MEMORY,
        .enable_timestamp = false,
        .enable_colors = false,
        .enable_module_name = false,
        .memory_buffer_size = sizeof(sys_log_entry_t) * 8U,
    };
    sys_log_entry_t entries[8];
    sys_log_entry_t reread[8];
    char            expect[16];

    zassert_equal(sys_log_init(&cfg), 0, NULL);
    sys_log_clear_buffer();

    for (int i = 0; i < 5; i++) {
        sys_log_print(SYS_LOG_LEVEL_INF, "test", "line %d", i);
    }

    zassert_equal(sys_log_get_count(), 5U, "expected 5 ring entries");
    zassert_equal(sys_log_get_entries(entries, 8, true), 5U, "get_entries");

    /* 顺序断言：oldest_first 下条目序号必须严格递增且无重复 */
    for (int i = 0; i < 5; i++) {
        int n = snprintf(expect, sizeof(expect), "line %d", i);
        zassert_true(n > 0 && n < (int) sizeof(expect), "期望消息构造不应截断");
        zassert_equal(strcmp(entries[i].message, expect), 0, "entries[%d] 应为 \"%s\"，实为 \"%s\"", i, expect,
                      entries[i].message);
    }

    /* dump 与 ring 一致性：dump 为只读操作，输出后环内容与读取顺序不得变化 */
    sys_log_dump(SYS_LOG_LEVEL_INF);

    zassert_equal(sys_log_get_count(), 5U, "dump 不应改动环计数");
    zassert_equal(sys_log_get_entries(reread, 8, true), 5U, "dump 后应仍能读出全部条目");
    for (int i = 0; i < 5; i++) {
        zassert_equal(strcmp(reread[i].message, entries[i].message), 0, "dump 后条目 %d 内容应与 dump 前一致", i);
    }
}
