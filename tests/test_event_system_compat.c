/**
 * @file test_event_system_compat.c
 * @brief event_system_compat 应用适配层单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            补全文件头模板（author/version/date/修改日志）
 */

#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>
#include <errno.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include <zeplod/event_system_compat.h>
#include "ztest_sync.h"

static atomic_t g_compat_stats_dispatched;

static void compat_stats_handler(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
    atomic_set(&g_compat_stats_dispatched, 1);
}

static void compat_suite_teardown(void* fixture) {
    ARG_UNUSED(fixture);
    (void) event_system_shutdown();
}

ZTEST(event_system_compat, test_compat_lifecycle_and_statistics) {
    event_compat_stats_t  stats;
    event_compat_config_t cfg = {0};
    uint32_t              subscriber_id;

    atomic_set(&g_compat_stats_dispatched, 0);

    zassert_equal(event_compat_init(&cfg), 0, NULL);
    zassert_false(event_compat_is_running(), NULL);
    zassert_equal(event_compat_start(), 0, NULL);
    zassert_true(event_compat_is_running(), NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    zassert_equal(event_register_type(51U, "compat_stats"), EVENT_OK, NULL);
    zassert_equal(event_subscribe(51U, compat_stats_handler, NULL, &subscriber_id), EVENT_OK, NULL);
    zassert_equal(event_publish_copy(51U, EVENT_PRIORITY_NORMAL, NULL, 0), EVENT_OK, NULL);
    zassert_true(ztest_wait_atomic_nonzero(&g_compat_stats_dispatched, 2000U), "事件应被分发");

    memset(&stats, 0xAA, sizeof(stats));
    event_compat_get_statistics(&stats);
    zassert_true(stats.total_events >= 1U, NULL);

    event_compat_reset_statistics();
    event_compat_get_statistics(&stats);
    zassert_equal(stats.total_events, 0U, NULL);
    zassert_equal(stats.high_priority_processed, 0U, NULL);
    zassert_equal(stats.batch_operations, 0U, NULL);

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_deinit(), EVENT_OK, NULL);
    zassert_equal(event_compat_stop(), 0, NULL);
    zassert_false(event_compat_is_running(), NULL);
    zassert_equal(event_compat_shutdown(), 0, NULL);
}

ZTEST(event_system_compat, test_compat_get_statistics_null) {
    event_compat_get_statistics(NULL);
    zassert_true(true, NULL);
}

ZTEST(event_system_compat, test_compat_stop_without_start) {
    event_compat_config_t cfg = {0};

    zassert_equal(event_compat_init(&cfg), 0, NULL);
    zassert_equal(event_compat_stop(), 0, NULL);
    zassert_equal(event_compat_shutdown(), 0, NULL);
}

ZTEST(event_system_compat, test_compat_init_idempotent) {
    zassert_equal(event_compat_init(NULL), 0, NULL);
    zassert_equal(event_compat_init(NULL), 0, NULL);
    zassert_equal(event_compat_shutdown(), 0, NULL);
}

ZTEST_SUITE(event_system_compat, NULL, NULL, NULL, compat_suite_teardown, NULL);
