/**
 * @file test_feature_gate.c
 * @brief feature_gate 模块单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh             初始版本
 * 2026-09-18       1.2            zeh             event_unsubscribe 改双参（类型+ID）；
 *                                                  补套件 teardown 停分发器与事件系统
 *
 */

#include <zephyr/kernel.h>

#include <zephyr/ztest.h>

#include <string.h>

#include <zeplod/app_config.h>

#include <zeplod/event_dispatcher.h>

#include <zeplod/event_system.h>

#include <zeplod/feature_gate.h>

static volatile bool g_last_license_valid;

static volatile int g_license_event_count;

static struct k_sem g_license_sem;

static void setup_gate(void) {
    zassert_equal(feature_gate_init(NULL), 0, NULL);

    zassert_equal(feature_gate_start(), 0, NULL);
}

static void teardown_gate(void) {
    (void) feature_gate_stop();

    (void) feature_gate_shutdown();
}

static void license_event_cb(const event_t* ev, void* user_data) {
    const bool* valid;

    (void) user_data;

    if (ev->type != EVENT_FEATURE_GATE_LICENSE_CHANGED || ev->data_len < sizeof(bool)) {
        return;
    }

    valid = (const bool*) ev->data.inline_data;

    g_last_license_valid = *valid;

    g_license_event_count++;

    k_sem_give(&g_license_sem);
}

static void gate_suite_teardown(void* fixture) {
    (void) fixture;

    /* 套件收尾对称：setup 启动了分发器与事件系统，这里统一停掉，
     * 避免分发线程泄漏影响同构建内的后续套件 */
    (void) event_dispatcher_stop();

    (void) event_system_stop();
}

static void* gate_suite_setup(void) {
    int ret;

    k_sem_init(&g_license_sem, 0, 1);

    g_last_license_valid = false;

    g_license_event_count = 0;

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

ZTEST_SUITE(feature_gate_tests, NULL, gate_suite_setup, NULL, NULL, gate_suite_teardown);

ZTEST(feature_gate_tests, test_core_always_enabled) {
    setup_gate();

    zassert_true(feature_gate_is_enabled(FEATURE_GATE_NAME_CORE), NULL);

    teardown_gate();
}

ZTEST(feature_gate_tests, test_premium_locked_until_license) {
    uint32_t sub_id = 0U;

    setup_gate();

    zassert_equal(event_subscribe(EVENT_FEATURE_GATE_LICENSE_CHANGED, license_event_cb, NULL, &sub_id),

                  EVENT_OK, NULL);

    zassert_false(feature_gate_is_enabled(FEATURE_GATE_NAME_CLOUD), NULL);

    zassert_false(feature_gate_is_enabled(FEATURE_GATE_NAME_REMOTE), NULL);

    zassert_equal(feature_gate_apply_license("zeplod-test-license"), 0, NULL);

    zassert_equal(k_sem_take(&g_license_sem, K_SECONDS(2)), 0, NULL);

    zassert_true(g_last_license_valid, NULL);

    zassert_equal(g_license_event_count, 1, NULL);

    zassert_true(feature_gate_license_valid(), NULL);

    zassert_true(feature_gate_is_enabled(FEATURE_GATE_NAME_CLOUD), NULL);

    zassert_true(feature_gate_is_enabled(FEATURE_GATE_NAME_REMOTE), NULL);

    zassert_equal(feature_gate_apply_license("wrong"), 0, NULL);

    zassert_equal(k_sem_take(&g_license_sem, K_SECONDS(2)), 0, NULL);

    zassert_false(g_last_license_valid, NULL);

    zassert_false(feature_gate_license_valid(), NULL);

    zassert_false(feature_gate_is_enabled(FEATURE_GATE_NAME_CLOUD), NULL);

    event_unsubscribe(EVENT_FEATURE_GATE_LICENSE_CHANGED, sub_id);

    teardown_gate();
}

ZTEST(feature_gate_tests, test_unknown_feature_disabled) {
    setup_gate();

    zassert_false(feature_gate_is_enabled("unknown_feature"), NULL);

    teardown_gate();
}

ZTEST(feature_gate_tests, test_api_before_init) {
    feature_gate_status_t st;

    zassert_equal(feature_gate_apply_license("zeplod-test-license"), APP_ERR_INIT, NULL);

    zassert_false(feature_gate_license_valid(), NULL);

    zassert_false(feature_gate_is_enabled(FEATURE_GATE_NAME_CLOUD), NULL);

    zassert_equal(feature_gate_get_status_snapshot(&st), APP_ERR_INIT, NULL);

    zassert_equal(feature_gate_apply_license(NULL), APP_ERR_INVALID_PARAM, NULL);
}

ZTEST(feature_gate_tests, test_boot_license_preapplied) {
    if ((strlen(CONFIG_FEATURE_GATE_BOOT_LICENSE) == 0U) ||

        (strcmp(CONFIG_FEATURE_GATE_BOOT_LICENSE, CONFIG_FEATURE_GATE_LICENSE) != 0)) {
        ztest_test_skip();

        return;
    }

    zassert_equal(feature_gate_init(NULL), 0, NULL);

    zassert_true(feature_gate_license_valid(), NULL);

    zassert_true(feature_gate_is_enabled(FEATURE_GATE_NAME_CLOUD), NULL);

    teardown_gate();
}
