/**
 * @file test_connectivity_module.c
 * @brief connectivity_module 单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 3 初始版本
 * 2026-06-13       1.1            zeh            事件订阅、RUNNING/幂等路径
 * 2026-09-18       1.2            zeh            补套件 teardown 停分发器与事件系统
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <errno.h>

#include <zeplod/app_config.h>
#include <zeplod/connectivity_module.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>

/* =============================================================================
 * 测试夹具
 * ============================================================================= */

static void setup_connectivity(void) {
    zassert_equal(connectivity_module_init(NULL), 0, NULL);
    zassert_equal(connectivity_module_init(NULL), 0, "init idempotent");
    zassert_equal(connectivity_module_start(), 0, NULL);
}

static void teardown_connectivity(void) {
    (void) connectivity_module_stop();
    (void) connectivity_module_shutdown();
}

static void* connectivity_suite_setup(void) {
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

static void connectivity_suite_teardown(void* fixture) {
    (void) fixture;

    /* 套件收尾对称：setup 启动了分发器与事件系统，这里统一停掉，
     * 避免分发线程泄漏影响同构建内的后续套件 */
    (void) event_dispatcher_stop();

    (void) event_system_stop();
}

ZTEST_SUITE(connectivity_module_tests, NULL, connectivity_suite_setup, NULL, NULL, connectivity_suite_teardown);

/* =============================================================================
 * 事件回调
 * ============================================================================= */

static volatile connectivity_state_t g_last_conn_state;
static volatile int                  g_conn_event_count;
static struct k_sem                  g_conn_sem;

static void connectivity_state_event_cb(const event_t* ev, void* user_data) {
    const connectivity_status_t* st;

    (void) user_data;
    if (ev->type != EVENT_CONNECTIVITY_STATE_CHANGED || ev->data_len < sizeof(connectivity_status_t)) {
        return;
    }
    st = (const connectivity_status_t*) ev->data.inline_data;
    g_last_conn_state = st->state;
    g_conn_event_count++;
    k_sem_give(&g_conn_sem);
}

/* =============================================================================
 * 用例
 * ============================================================================= */

ZTEST(connectivity_module_tests, test_connect_disconnect_null_backend) {
    connectivity_status_t st;

    setup_connectivity();

    zassert_equal(connectivity_module_connect(CONNECTIVITY_LINK_WIFI), 0, NULL);

    zassert_equal(connectivity_module_get_state(&st), 0, NULL);
    zassert_equal(st.state, CONNECTIVITY_STATE_UP, NULL);
    zassert_equal(st.link_type, CONNECTIVITY_LINK_WIFI, NULL);

    zassert_equal(connectivity_module_disconnect(), 0, NULL);
    zassert_equal(connectivity_module_get_state(&st), 0, NULL);
    zassert_equal(st.state, CONNECTIVITY_STATE_DOWN, NULL);
    zassert_equal(st.link_type, CONNECTIVITY_LINK_NONE, NULL);

    teardown_connectivity();
}

ZTEST(connectivity_module_tests, test_connect_publishes_events) {
    uint32_t sub_id;

    k_sem_init(&g_conn_sem, 0, 10);
    g_last_conn_state = CONNECTIVITY_STATE_DOWN;
    g_conn_event_count = 0;

    zassert_equal(event_subscribe(EVENT_CONNECTIVITY_STATE_CHANGED, connectivity_state_event_cb, NULL, &sub_id),
                  EVENT_OK);

    setup_connectivity();
    zassert_equal(connectivity_module_connect(CONNECTIVITY_LINK_WIFI), 0, NULL);

    /* CONNECTING + UP */
    zassert_equal(k_sem_take(&g_conn_sem, K_SECONDS(2)), 0, "first conn event");
    zassert_equal(k_sem_take(&g_conn_sem, K_SECONDS(2)), 0, "second conn event");
    zassert_true(g_conn_event_count >= 2, "event count=%d", g_conn_event_count);
    zassert_equal(g_last_conn_state, CONNECTIVITY_STATE_UP, NULL);

    teardown_connectivity();
    event_unsubscribe(EVENT_CONNECTIVITY_STATE_CHANGED, sub_id);
}

ZTEST(connectivity_module_tests, test_connect_idempotent_when_up) {
    setup_connectivity();

    zassert_equal(connectivity_module_connect(CONNECTIVITY_LINK_WIFI), 0, NULL);
    zassert_equal(connectivity_module_connect(CONNECTIVITY_LINK_WIFI), 0, "idempotent when UP");

    teardown_connectivity();
}

ZTEST(connectivity_module_tests, test_connect_without_start_rejected) {
    zassert_equal(connectivity_module_init(NULL), 0, NULL);
    zassert_equal(connectivity_module_connect(CONNECTIVITY_LINK_WIFI), APP_ERR_INIT, NULL);
    (void) connectivity_module_shutdown();
}

ZTEST(connectivity_module_tests, test_get_state_null_rejected) {
    setup_connectivity();
    zassert_equal(connectivity_module_get_state(NULL), APP_ERR_INVALID_PARAM, NULL);
    teardown_connectivity();
}

ZTEST(connectivity_module_tests, test_module_lifecycle) {
    zassert_equal(connectivity_module_init(NULL), 0, NULL);
    zassert_equal(connectivity_module_get_status(), MODULE_STATUS_INITIALIZED, NULL);
    zassert_equal(connectivity_module_start(), 0, NULL);
    zassert_equal(connectivity_module_get_status(), MODULE_STATUS_RUNNING, NULL);
    zassert_equal(connectivity_module_stop(), 0, NULL);
    zassert_equal(connectivity_module_shutdown(), 0, NULL);
    zassert_equal(connectivity_module_get_status(), MODULE_STATUS_UNINITIALIZED, NULL);
}
