/**
 * @file test_factory_mode.c
 * @brief factory_mode 模块单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 4 初始版本
 * 2026-06-13       1.1            zeh            事件订阅、失败路径与边界用例
 * 2026-09-18       1.2            zeh            新增 value 超长拒绝用例（错误码统一
 *                                                 APP_ERR_INVALID_PARAM 防复发）；snprintf
 *                                                 返回值核对；补套件 teardown 停分发器
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zeplod/app_config.h>
#include <zeplod/app_kv.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include <zeplod/factory_mode.h>

/* =============================================================================
 * 测试夹具
 * ============================================================================= */

static void setup_factory(void) {
    app_kv_init();
    app_kv_clear();
    zassert_equal(factory_mode_control(FACTORY_MODE_CMD_RESET, NULL), 0, NULL);
    zassert_equal(factory_mode_init(NULL), 0, NULL);
    zassert_equal(factory_mode_init(NULL), 0, "init idempotent");
    zassert_equal(factory_mode_start(), 0, NULL);
}

static void teardown_factory(void) {
    (void) factory_mode_stop();
    (void) factory_mode_shutdown();
    app_kv_clear();
}

static void* factory_suite_setup(void) {
    int ret;

    ret = event_system_init();
    zassert_true(ret == EVENT_OK || ret == -EALREADY, "event_system_init: %d", ret);
    ret = event_system_start();
    zassert_true(ret == EVENT_OK || ret == -EALREADY, "event_system_start: %d", ret);
    ret = event_dispatcher_init(NULL);
    zassert_true(ret == EVENT_OK || ret == -EALREADY, "event_dispatcher_init: %d", ret);
    ret = event_dispatcher_start();
    zassert_true(ret == EVENT_OK || ret == -EALREADY, "event_dispatcher_start: %d", ret);

    return NULL;
}

static void factory_suite_teardown(void* fixture) {
    (void) fixture;

    /* 套件收尾对称：setup 启动了分发器与事件系统，这里统一停掉，
     * 避免分发线程泄漏影响同构建内的后续套件 */
    (void) event_dispatcher_stop();

    (void) event_system_stop();
}

ZTEST_SUITE(factory_mode_tests, NULL, factory_suite_setup, NULL, NULL, factory_suite_teardown);

/* =============================================================================
 * 事件回调
 * ============================================================================= */

static volatile factory_state_t g_last_factory_state;
static volatile int             g_last_factory_error;
static volatile int             g_factory_event_count;
static struct k_sem             g_factory_sem;

static void factory_state_event_cb(const event_t* ev, void* user_data) {
    const factory_status_t* st;

    (void) user_data;
    if (ev->type != EVENT_FACTORY_STATE_CHANGED || ev->data_len < sizeof(factory_status_t)) {
        return;
    }
    st = (const factory_status_t*) ev->data.inline_data;
    g_last_factory_state = st->state;
    g_last_factory_error = st->error_code;
    g_factory_event_count++;
    k_sem_give(&g_factory_sem);
}

static void fill_app_kv_full(void) {
    char key[16];

    for (int i = 0; i < APP_KV_MAX_ENTRIES; i++) {
        /* key 上限 16 字节、i < APP_KV_MAX_ENTRIES(16)，两位序号不会截断 */
        int n = snprintf(key, sizeof(key), "fill%d", i);
        zassert_true(n >= 0 && n < (int) sizeof(key), "fill key snprintf 应完整写入");
        zassert_equal(app_kv_set(key, "v"), APP_OK, "fill kv %d", i);
    }
}

/* =============================================================================
 * 用例
 * ============================================================================= */

ZTEST(factory_mode_tests, test_enter_gpio_finalize_flow) {
    factory_status_t st;
    char             buf[64];

    setup_factory();

    zassert_equal(factory_mode_enter(), 0, NULL);
    zassert_equal(factory_mode_get_state(&st), 0, NULL);
    zassert_equal(st.state, FACTORY_STATE_ACTIVE, NULL);
    zassert_false(st.gpio_passed, NULL);
    zassert_equal(st.error_code, 0, NULL);

    zassert_equal(factory_mode_set_calibration("offset", "12.5"), 0, NULL);
    zassert_equal(factory_mode_get_calibration("offset", buf, sizeof(buf)), 0, NULL);
    zassert_mem_equal(buf, "12.5", 5, NULL);

    zassert_equal(factory_mode_finalize_pass(), APP_ERR_FACTORY, "gpio not run yet");

    zassert_equal(factory_mode_run_gpio_loopback(), 0, NULL);
    zassert_equal(factory_mode_finalize_pass(), 0, NULL);

    zassert_equal(factory_mode_get_state(&st), 0, NULL);
    zassert_equal(st.state, FACTORY_STATE_PASSED, NULL);
    zassert_equal(st.error_code, 0, NULL);

    zassert_equal(app_kv_get("factory.cal.offset", buf, sizeof(buf)), APP_OK, NULL);
    zassert_mem_equal(buf, "12.5", 5, NULL);

    teardown_factory();
}

ZTEST(factory_mode_tests, test_enter_publishes_events) {
    uint32_t sub_id;

    k_sem_init(&g_factory_sem, 0, 10);
    g_last_factory_state = FACTORY_STATE_INACTIVE;
    g_last_factory_error = 0;
    g_factory_event_count = 0;

    zassert_equal(event_subscribe(EVENT_FACTORY_STATE_CHANGED, factory_state_event_cb, NULL, &sub_id), EVENT_OK);

    setup_factory();
    zassert_equal(factory_mode_enter(), 0, NULL);
    zassert_equal(k_sem_take(&g_factory_sem, K_SECONDS(2)), 0, "enter event");
    zassert_equal(g_last_factory_state, FACTORY_STATE_ACTIVE, NULL);
    zassert_equal(g_last_factory_error, 0, NULL);

    zassert_equal(factory_mode_run_gpio_loopback(), 0, NULL);
    zassert_equal(factory_mode_set_calibration("gain", "1.0"), 0, NULL);
    zassert_equal(factory_mode_finalize_pass(), 0, NULL);
    zassert_equal(k_sem_take(&g_factory_sem, K_SECONDS(2)), 0, "passed event");
    zassert_equal(g_last_factory_state, FACTORY_STATE_PASSED, NULL);
    zassert_true(g_factory_event_count >= 2, "event count=%d", g_factory_event_count);

    teardown_factory();
    event_unsubscribe(EVENT_FACTORY_STATE_CHANGED, sub_id);
}

ZTEST(factory_mode_tests, test_finalize_failed_reports_error) {
    factory_status_t st;
    char             key[16];

    setup_factory();
    fill_app_kv_full();

    zassert_equal(factory_mode_enter(), 0, NULL);
    zassert_equal(factory_mode_run_gpio_loopback(), 0, NULL);
    zassert_equal(factory_mode_set_calibration("offset", "12.5"), 0, NULL);
    zassert_equal(factory_mode_finalize_pass(), APP_ERR_KV_FULL, NULL);

    zassert_equal(factory_mode_get_state(&st), 0, NULL);
    zassert_equal(st.state, FACTORY_STATE_FAILED, NULL);
    zassert_equal(st.error_code, APP_ERR_KV_FULL, NULL);

    /* 先前已写入的 KV 条目仍在（无回滚） */
    zassert_true(app_kv_has("fill0"), NULL);
    /* fill 前缀 + 两位序号在 16 字节内不会截断 */
    int n = snprintf(key, sizeof(key), "fill%d", APP_KV_MAX_ENTRIES - 1);
    zassert_true(n >= 0 && n < (int) sizeof(key), "fill key snprintf 应完整写入");
    zassert_true(app_kv_has(key), NULL);

    teardown_factory();
}

ZTEST(factory_mode_tests, test_exit_clears_active) {
    factory_status_t st;
    char             buf[64];

    setup_factory();

    zassert_equal(factory_mode_enter(), 0, NULL);
    zassert_equal(factory_mode_set_calibration("offset", "12.5"), 0, NULL);
    zassert_equal(factory_mode_exit(), 0, NULL);
    zassert_equal(factory_mode_get_state(&st), 0, NULL);
    zassert_equal(st.state, FACTORY_STATE_INACTIVE, NULL);
    zassert_equal(factory_mode_get_calibration("offset", buf, sizeof(buf)), APP_ERR_NOT_FOUND, NULL);
    zassert_equal(factory_mode_enter(), 0, NULL);

    teardown_factory();
}

ZTEST(factory_mode_tests, test_double_enter_rejected) {
    setup_factory();

    zassert_equal(factory_mode_enter(), 0, NULL);
    zassert_equal(factory_mode_enter(), APP_ERR_FACTORY, NULL);

    teardown_factory();
}

ZTEST(factory_mode_tests, test_cal_slots_full) {
    char key[8];
    char val[4] = "1";

    setup_factory();
    zassert_equal(factory_mode_enter(), 0, NULL);

    for (size_t i = 0U; i < CONFIG_FACTORY_MODE_CAL_MAX_ENTRIES; i++) {
        /* "k" + 个位/十位序号远小于 8 字节缓冲，不会截断 */
        int n = snprintf(key, sizeof(key), "k%zu", i);
        zassert_true(n >= 0 && n < (int) sizeof(key), "cal key snprintf 应完整写入");
        zassert_equal(factory_mode_set_calibration(key, val), 0, "slot %zu", i);
    }

    zassert_equal(factory_mode_set_calibration("overflow", val), -ENOMEM, NULL);

    teardown_factory();
}

/**
 * @brief 测试校准值超长拒绝（2026-09-18 契约：统一 APP_ERR_INVALID_PARAM）
 *
 * 原实现对 value 超长返回 -ENOMEM，与表满路径混淆；现与 key 超长统一为
 * APP_ERR_INVALID_PARAM。本用例为该契约的防复发锚点。
 */
ZTEST(factory_mode_tests, test_calibration_value_too_long_rejected) {
    char long_val[CONFIG_FACTORY_MODE_CAL_VALUE_MAX_LEN];

    setup_factory();
    zassert_equal(factory_mode_enter(), 0, NULL);

    /* 长度恰等于上限（不含结尾 0）：契约要求 strlen < 上限，此处必须拒绝 */
    memset(long_val, 'v', sizeof(long_val) - 1U);
    long_val[sizeof(long_val) - 1U] = '\0';
    zassert_equal(factory_mode_set_calibration("toolong", long_val), APP_ERR_INVALID_PARAM,
                  "value 达长度上限应返回 APP_ERR_INVALID_PARAM");

    teardown_factory();
}

ZTEST(factory_mode_tests, test_passed_rejects_calibration) {
    factory_status_t st;

    setup_factory();

    zassert_equal(factory_mode_enter(), 0, NULL);
    zassert_equal(factory_mode_run_gpio_loopback(), 0, NULL);
    zassert_equal(factory_mode_set_calibration("offset", "12.5"), 0, NULL);
    zassert_equal(factory_mode_finalize_pass(), 0, NULL);
    zassert_equal(factory_mode_get_state(&st), 0, NULL);
    zassert_equal(st.state, FACTORY_STATE_PASSED, NULL);
    zassert_equal(factory_mode_set_calibration("other", "1"), APP_ERR_FACTORY, NULL);

    teardown_factory();
}

ZTEST(factory_mode_tests, test_get_state_null_rejected) {
    setup_factory();
    zassert_equal(factory_mode_get_state(NULL), APP_ERR_INVALID_PARAM, NULL);
    teardown_factory();
}

ZTEST(factory_mode_tests, test_module_lifecycle) {
    zassert_equal(factory_mode_init(NULL), 0, NULL);
    zassert_equal(factory_mode_get_status(), MODULE_STATUS_INITIALIZED, NULL);
    zassert_equal(factory_mode_start(), 0, NULL);
    zassert_equal(factory_mode_get_status(), MODULE_STATUS_RUNNING, NULL);
    zassert_equal(factory_mode_stop(), 0, NULL);
    zassert_equal(factory_mode_shutdown(), 0, NULL);
    zassert_equal(factory_mode_get_status(), MODULE_STATUS_UNINITIALIZED, NULL);
}
