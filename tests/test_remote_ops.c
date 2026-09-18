/**
 * @file test_remote_ops.c
 * @brief remote_ops 模块单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh             初始版本
 * 2026-09-18       1.2            zeh             event_unsubscribe 改双参（类型+ID）；
 *                                                  去 const 类型压制；补套件 teardown
 *                                                  停分发器与事件系统
 *
 */

#include <zephyr/kernel.h>

#include <zephyr/ztest.h>

#include <errno.h>

#include <string.h>

#include <zeplod/app_config.h>

#include <zeplod/event_dispatcher.h>

#include <zeplod/event_system.h>

#include <zeplod/remote_ops.h>

static volatile uint32_t g_remote_export_payload_len;

static volatile int g_remote_export_event_count;

static struct k_sem g_remote_export_sem;

static void setup_remote_ops(void) {
    zassert_equal(remote_ops_init(NULL), 0, NULL);

    zassert_equal(remote_ops_start(), 0, NULL);
}

static void teardown_remote_ops(void) {
    (void) remote_ops_stop();

    (void) remote_ops_shutdown();
}

static void remote_export_event_cb(const event_t* ev, void* user_data) {
    const uint32_t* len;

    (void) user_data;

    if (ev->type != EVENT_REMOTE_OPS_DIAG_EXPORTED || ev->data_len < sizeof(uint32_t)) {
        return;
    }

    len = (const uint32_t*) ev->data.inline_data;

    g_remote_export_payload_len = *len;

    g_remote_export_event_count++;

    k_sem_give(&g_remote_export_sem);
}

static void remote_ops_suite_teardown(void* fixture) {
    (void) fixture;

    /* 套件收尾对称：setup 启动了分发器与事件系统，这里统一停掉，
     * 避免分发线程泄漏影响同构建内的后续套件 */
    (void) event_dispatcher_stop();

    (void) event_system_stop();
}

static void* remote_ops_suite_setup(void) {
    int ret;

    k_sem_init(&g_remote_export_sem, 0, 1);

    g_remote_export_payload_len = 0U;

    g_remote_export_event_count = 0;

    ret = event_system_init();

    zassert_true(ret == EVENT_OK || ret == -EALREADY, NULL);

    ret = event_system_start();

    zassert_true(ret == EVENT_OK || ret == -EALREADY, NULL);

    ret = event_dispatcher_init(NULL);

    zassert_true(ret == EVENT_OK || ret == -EALREADY, NULL);

    ret = event_dispatcher_start();

    zassert_true(ret == EVENT_OK || ret == -EALREADY, NULL);

    return NULL;
}

ZTEST_SUITE(remote_ops_tests, NULL, remote_ops_suite_setup, NULL, NULL, remote_ops_suite_teardown);

ZTEST(remote_ops_tests, test_export_diag_json) {
    char buf[CONFIG_REMOTE_OPS_EXPORT_BUF_SIZE];

    remote_ops_status_t stats;

    uint32_t sub_id = 0U;

    setup_remote_ops();

    zassert_equal(event_subscribe(EVENT_REMOTE_OPS_DIAG_EXPORTED, remote_export_event_cb, NULL, &sub_id),

                  EVENT_OK, NULL);

    zassert_equal(remote_ops_export_diag(), 0, NULL);

    zassert_equal(k_sem_take(&g_remote_export_sem, K_SECONDS(2)), 0, NULL);

    zassert_equal(g_remote_export_event_count, 1, NULL);

    zassert_true(g_remote_export_payload_len > 0U, NULL);

    zassert_equal(remote_ops_get_last_export(buf, sizeof(buf)), 0, NULL);

    zassert_not_null(strstr(buf, "\"uptime_ms\""), NULL);

    zassert_equal(remote_ops_get_stats(&stats), 0, NULL);

    zassert_equal(stats.export_count, 1U, NULL);

    zassert_equal(stats.last_payload_len, g_remote_export_payload_len, NULL);

    event_unsubscribe(EVENT_REMOTE_OPS_DIAG_EXPORTED, sub_id);

    teardown_remote_ops();
}

ZTEST(remote_ops_tests, test_get_last_export_empty_before_export) {
    char buf[64];

    setup_remote_ops();

    zassert_equal(remote_ops_get_last_export(buf, sizeof(buf)), -ENOENT, NULL);

    teardown_remote_ops();
}

ZTEST(remote_ops_tests, test_export_requires_running) {
    zassert_equal(remote_ops_init(NULL), 0, NULL);

    zassert_equal(remote_ops_export_diag(), APP_ERR_INIT, NULL);

    zassert_equal(remote_ops_start(), 0, NULL);

    zassert_equal(remote_ops_stop(), 0, NULL);

    zassert_equal(remote_ops_export_diag(), APP_ERR_INIT, NULL);

    teardown_remote_ops();
}

ZTEST(remote_ops_tests, test_export_before_init) {
    char writable[2] = "x"; /* 本地可写缓冲，避免对字面量去 const */

    zassert_equal(remote_ops_export_diag(), APP_ERR_INIT, NULL);

    zassert_equal(remote_ops_get_last_export(writable, sizeof(writable)), APP_ERR_INIT, NULL);

    zassert_equal(remote_ops_get_stats(NULL), APP_ERR_INVALID_PARAM, NULL);
}

ZTEST(remote_ops_tests, test_get_last_export_invalid_args) {
    char buf[64];

    setup_remote_ops();

    zassert_equal(remote_ops_get_last_export(NULL, sizeof(buf)), APP_ERR_INVALID_PARAM, NULL);

    zassert_equal(remote_ops_get_last_export(buf, 0U), APP_ERR_INVALID_PARAM, NULL);

    teardown_remote_ops();
}
