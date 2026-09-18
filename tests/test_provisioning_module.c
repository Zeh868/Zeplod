/**
 * @file test_provisioning_module.c
 * @brief provisioning_module 单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 3 初始版本
 * 2026-06-13       1.1            zeh            事件订阅用例
 * 2026-09-18       1.2            zeh            新增 reset 删除持久化凭据用例（新
 *                                                 契约防复发）；补套件 teardown 停分发器
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <string.h>

#include <zeplod/app_config.h>
#include <zeplod/app_kv.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include <zeplod/provisioning_module.h>

/* =============================================================================
 * 测试夹具
 * ============================================================================= */

static void setup_provisioning(void) {
    zassert_equal(provisioning_module_init(NULL), 0, NULL);
    zassert_equal(provisioning_module_init(NULL), 0, "init idempotent");
    zassert_equal(provisioning_module_start(), 0, NULL);
}

static void teardown_provisioning(void) {
    (void) provisioning_module_stop();
    (void) provisioning_module_shutdown();
}

static void* provisioning_suite_setup(void) {
    int ret;

#ifdef CONFIG_APP_KV_ENABLE
    /* 凭据经 app_kv 持久化：测试前显式初始化并清空，保证 init 探测从干净态开始 */
    app_kv_init();
    app_kv_clear();
#endif

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

static void provisioning_suite_teardown(void* fixture) {
    (void) fixture;

    /* 套件收尾对称：setup 启动了分发器与事件系统，这里统一停掉，
     * 避免分发线程泄漏影响同构建内的后续套件 */
    (void) event_dispatcher_stop();

    (void) event_system_stop();
}

ZTEST_SUITE(provisioning_module_tests, NULL, provisioning_suite_setup, NULL, NULL, provisioning_suite_teardown);

/* =============================================================================
 * 事件回调
 * ============================================================================= */

static volatile provisioning_state_t g_last_prov_state;
static volatile int                  g_prov_event_count;
static struct k_sem                  g_prov_sem;

static void provisioning_state_event_cb(const event_t* ev, void* user_data) {
    const provisioning_status_t* st;

    (void) user_data;
    if (ev->type != EVENT_PROVISIONING_STATE_CHANGED || ev->data_len < sizeof(provisioning_status_t)) {
        return;
    }
    st = (const provisioning_status_t*) ev->data.inline_data;
    g_last_prov_state = st->state;
    g_prov_event_count++;
    k_sem_give(&g_prov_sem);
}

/* =============================================================================
 * 用例
 * ============================================================================= */

ZTEST(provisioning_module_tests, test_begin_stub_success) {
    provisioning_state_t state;

    setup_provisioning();

    zassert_equal(provisioning_module_get_state(&state), 0, NULL);
    zassert_equal(state, PROVISIONING_STATE_UNPROVISIONED, NULL);

    zassert_equal(provisioning_module_begin(NULL), 0, NULL);
    zassert_equal(provisioning_module_get_state(&state), 0, NULL);
    zassert_equal(state, PROVISIONING_STATE_PROVISIONED, NULL);

    zassert_equal(provisioning_module_begin(NULL), APP_ERR_PROVISIONING, "already provisioned");

    teardown_provisioning();
}

ZTEST(provisioning_module_tests, test_begin_publishes_events) {
    uint32_t sub_id;

    k_sem_init(&g_prov_sem, 0, 10);
    g_last_prov_state = PROVISIONING_STATE_UNPROVISIONED;
    g_prov_event_count = 0;

    zassert_equal(event_subscribe(EVENT_PROVISIONING_STATE_CHANGED, provisioning_state_event_cb, NULL, &sub_id),
                  EVENT_OK);

    setup_provisioning();
    zassert_equal(provisioning_module_begin(NULL), 0, NULL);

    /* IN_PROGRESS + PROVISIONED */
    zassert_equal(k_sem_take(&g_prov_sem, K_SECONDS(2)), 0, "first prov event");
    zassert_equal(k_sem_take(&g_prov_sem, K_SECONDS(2)), 0, "second prov event");
    zassert_true(g_prov_event_count >= 2, "event count=%d", g_prov_event_count);
    zassert_equal(g_last_prov_state, PROVISIONING_STATE_PROVISIONED, NULL);

    teardown_provisioning();
    event_unsubscribe(EVENT_PROVISIONING_STATE_CHANGED, sub_id);
}

ZTEST(provisioning_module_tests, test_reset_and_device_id) {
    provisioning_state_t state;
    char                 dev_id[64];

    setup_provisioning();

    zassert_equal(provisioning_module_begin(NULL), 0, NULL);
    zassert_equal(provisioning_module_reset(), 0, NULL);
    zassert_equal(provisioning_module_get_state(&state), 0, NULL);
    zassert_equal(state, PROVISIONING_STATE_UNPROVISIONED, NULL);

    /* 设备 ID 来自 Kconfig 编译期字符串，与配网状态无关（reset 仍可读取） */
    zassert_equal(provisioning_module_get_device_id(dev_id, sizeof(dev_id)), 0, NULL);
    zassert_true(strlen(dev_id) > 0U, NULL);
    zassert_mem_equal(dev_id, CONFIG_PROVISIONING_DEVICE_ID, strlen(CONFIG_PROVISIONING_DEVICE_ID), NULL);

    teardown_provisioning();
}

/**
 * @brief 测试 reset 删除持久化凭据（2026-09-18 新契约防复发）
 *
 * 原 reset 只复位状态机，残留的 wifi.ssid/wifi.psk 会在下次上电被 init 探测
 * 误判为已配网。新契约：reset 须同时删除持久化凭据。
 */
ZTEST(provisioning_module_tests, test_reset_clears_persisted_credentials) {
    provisioning_credentials_t creds = {.ssid = "zeplod_test_ssid", .psk = "zeplod_test_psk"};
    char                       ssid[PROVISIONING_WIFI_SSID_MAX_LEN];
    char                       psk[PROVISIONING_WIFI_PSK_MAX_LEN];

    if (!IS_ENABLED(CONFIG_APP_KV_ENABLE)) {
        /* app_kv 未编译时凭据持久化路径不存在，无从验证删除语义 */
        ztest_test_skip();
        return;
    }

    setup_provisioning();

    /* 先落盘凭据并确认可读回 */
    zassert_equal(provisioning_module_set_credentials(&creds), 0, NULL);
    zassert_equal(provisioning_module_get_credentials(ssid, sizeof(ssid), psk, sizeof(psk)), 0, NULL);
    zassert_equal(strcmp(ssid, "zeplod_test_ssid"), 0, NULL);

    /* reset 后凭据必须被删除：get_credentials 应回落到 NOT_FOUND */
    zassert_equal(provisioning_module_reset(), 0, NULL);
    zassert_equal(provisioning_module_get_credentials(ssid, sizeof(ssid), psk, sizeof(psk)), APP_ERR_NOT_FOUND,
                  "reset 后持久化凭据应被删除");

    /* 兜底清理，避免残留键位影响同构建内其他套件 */
    app_kv_clear();

    teardown_provisioning();
}

ZTEST(provisioning_module_tests, test_begin_without_start_rejected) {
    zassert_equal(provisioning_module_init(NULL), 0, NULL);
    zassert_equal(provisioning_module_begin(NULL), APP_ERR_INIT, NULL);
    (void) provisioning_module_shutdown();
}

ZTEST(provisioning_module_tests, test_get_state_null_rejected) {
    setup_provisioning();
    zassert_equal(provisioning_module_get_state(NULL), APP_ERR_INVALID_PARAM, NULL);
    teardown_provisioning();
}

ZTEST(provisioning_module_tests, test_module_lifecycle) {
    zassert_equal(provisioning_module_init(NULL), 0, NULL);
    zassert_equal(provisioning_module_get_status(), MODULE_STATUS_INITIALIZED, NULL);
    zassert_equal(provisioning_module_start(), 0, NULL);
    zassert_equal(provisioning_module_get_status(), MODULE_STATUS_RUNNING, NULL);
    zassert_equal(provisioning_module_stop(), 0, NULL);
    zassert_equal(provisioning_module_shutdown(), 0, NULL);
    zassert_equal(provisioning_module_get_status(), MODULE_STATUS_UNINITIALIZED, NULL);
}
