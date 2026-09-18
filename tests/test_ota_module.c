/**
 * @file test_ota_module.c
 * @brief OTA 模块与传输层单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            初始版本
 * 2026-06-13       1.1            zeh            补充幂等/溢出/恢复路径用例
 * 2026-09-18       1.2            zeh            传输层 vtable 以非 const 局部指针承接
 *                                                 （登记 API 设计矛盾）；补套件
 *                                                 teardown 停分发器与事件系统
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <string.h>

#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include <zeplod/module_base.h>
#include <zeplod/ota_module.h>
#include <zeplod/ota_transport.h>

/* =============================================================================
 * 测试夹具
 * ============================================================================= */

static void setup_ota_module(void) {
    zassert_equal(ota_module_init(NULL), 0, NULL);
    zassert_equal(ota_module_init(NULL), 0, "init must be idempotent");
    zassert_equal(ota_module_start(), 0, NULL);
}

static void teardown_ota_module(void) {
    (void) ota_module_stop();
    (void) ota_module_shutdown();
}

/* =============================================================================
 * 测试套件
 *
 * 事件系统与分发器在 suite 级初始化一次：event_dispatcher_init 在分发线程已
 * 运行时会拒绝 re-init（返回 EVENT_ERR_INVALID_ARG），故不能每个用例重复初始化。
 * ============================================================================= */

static void ota_suite_teardown(void* fixture) {
    (void) fixture;

    /* 套件收尾对称：setup 启动了分发器与事件系统，这里统一停掉，
     * 避免分发线程泄漏影响同构建内的后续套件 */
    (void) event_dispatcher_stop();

    (void) event_system_stop();
}

static void* ota_suite_setup(void) {
    int ret;

    ret = event_system_init();
    zassert_true(ret == EVENT_OK || ret == -EALREADY, "event_system_init failed: %d", ret);
    ret = event_system_start();
    zassert_true(ret == EVENT_OK || ret == -EALREADY, "event_system_start failed: %d", ret);
    ret = event_dispatcher_init(NULL);
    zassert_true(ret == EVENT_OK || ret == -EALREADY, "event_dispatcher_init failed: %d", ret);
    ret = event_dispatcher_start();
    zassert_true(ret == EVENT_OK || ret == -EALREADY, "event_dispatcher_start failed: %d", ret);

    return NULL;
}

ZTEST_SUITE(ota_module, NULL, ota_suite_setup, NULL, NULL, ota_suite_teardown);

/* =============================================================================
 * 传输层
 * ============================================================================= */

/*
 * API 设计矛盾登记（根因在框架侧，不改 src）：ota_transport_null_get() 返回
 * const ota_transport_ops_t*，而 vtable 各回调形参为非 const ota_transport_ops_t*。
 * 测试以局部非 const 指针一次性承接并注明，避免在每个调用点重复去 const 强转。
 */
ZTEST(ota_module, test_null_transport_write_read) {
    ota_transport_ops_t* ops = (ota_transport_ops_t*) ota_transport_null_get();

    zassert_not_null(ops, NULL);
    zassert_not_null(ops->open, NULL);
    zassert_not_null(ops->write_chunk, NULL);
    zassert_not_null(ops->verify, NULL);

    zassert_equal(ops->open(ops), 0);

    const uint8_t chunk[] = {0x01, 0x02, 0x03, 0x04};
    zassert_equal(ops->write_chunk(ops, 0, chunk, sizeof(chunk)), 0);
    zassert_equal(ops->verify(ops), 0);
    zassert_equal(ops->close(ops), 0);
}

ZTEST(ota_module, test_null_transport_write_overflow) {
    ota_transport_ops_t* ops = (ota_transport_ops_t*) ota_transport_null_get();

    zassert_equal(ops->open(ops), 0);
    zassert_equal(ops->write_chunk(ops, SIZE_MAX, (const uint8_t*) "x", 1), -ENOMEM);
    (void) ops->close(ops);
}

/* =============================================================================
 * 状态机
 * ============================================================================= */

ZTEST(ota_module, test_state_machine_happy_path) {
    ota_sm_t sm;

    ota_sm_init(&sm);
    zassert_equal(ota_sm_get_state(&sm), OTA_STATE_IDLE);

    zassert_equal(ota_sm_on_download_start(&sm), 0);
    zassert_equal(ota_sm_get_state(&sm), OTA_STATE_DOWNLOADING);

    zassert_equal(ota_sm_on_download_complete(&sm), 0);
    zassert_equal(ota_sm_get_state(&sm), OTA_STATE_VERIFYING);

    zassert_equal(ota_sm_on_verify_ok(&sm), 0);
    zassert_equal(ota_sm_get_state(&sm), OTA_STATE_READY_REBOOT);
}

ZTEST(ota_module, test_state_machine_invalid_transition) {
    ota_sm_t sm;

    ota_sm_init(&sm);
    zassert_equal(ota_sm_on_verify_ok(&sm), -EINVAL);
    zassert_equal(ota_sm_get_state(&sm), OTA_STATE_IDLE);
}

/* =============================================================================
 * 模块集成
 * ============================================================================= */

static volatile ota_state_t g_last_ota_state;
static volatile int         g_progress_event_count;
static struct k_sem         g_ota_sem;

static void ota_state_event_cb(const event_t* ev, void* user_data) {
    (void) user_data;
    if (ev->type != EVENT_OTA_STATE_CHANGED || ev->data_len < sizeof(ota_progress_t)) {
        return;
    }
    const ota_progress_t* p = (const ota_progress_t*) ev->data.inline_data;
    g_last_ota_state = p->state;
    k_sem_give(&g_ota_sem);
}

static void ota_progress_event_cb(const event_t* ev, void* user_data) {
    (void) user_data;
    if (ev->type != EVENT_OTA_PROGRESS || ev->data_len < sizeof(ota_progress_t)) {
        return;
    }
    g_progress_event_count++;
    k_sem_give(&g_ota_sem);
}

ZTEST(ota_module, test_module_start_download_publishes_events) {
    k_sem_init(&g_ota_sem, 0, 10);
    g_last_ota_state = OTA_STATE_IDLE;
    g_progress_event_count = 0;

    uint32_t sub_state;
    uint32_t sub_progress;
    zassert_equal(event_register_type(EVENT_OTA_STATE_CHANGED, "ota_state"), EVENT_OK);
    zassert_equal(event_register_type(EVENT_OTA_PROGRESS, "ota_progress"), EVENT_OK);
    zassert_equal(event_subscribe(EVENT_OTA_STATE_CHANGED, ota_state_event_cb, NULL, &sub_state), EVENT_OK);
    zassert_equal(event_subscribe(EVENT_OTA_PROGRESS, ota_progress_event_cb, NULL, &sub_progress), EVENT_OK);

    setup_ota_module();
    zassert_equal(ota_module_get_status(), MODULE_STATUS_RUNNING, NULL);
    zassert_equal(ota_module_begin_update(), 0);

    /* begin 同步发布 STATE_CHANGED + PROGRESS 两个事件，须等待两者均被分发后再断言 */
    zassert_equal(k_sem_take(&g_ota_sem, K_SECONDS(2)), 0, "first OTA event not received");
    zassert_equal(k_sem_take(&g_ota_sem, K_SECONDS(2)), 0, "second OTA event not received");
    zassert_equal(g_last_ota_state, OTA_STATE_DOWNLOADING);
    zassert_true(g_progress_event_count > 0, "EVENT_OTA_PROGRESS not received");

    teardown_ota_module();
    event_unsubscribe(EVENT_OTA_STATE_CHANGED, sub_state);
    event_unsubscribe(EVENT_OTA_PROGRESS, sub_progress);
}

ZTEST(ota_module, test_full_null_transport_update) {
    setup_ota_module();

    zassert_equal(ota_module_begin_update(), 0);

    static const uint8_t firmware[] = "ZEPLD_TEST_IMG_v1";
    zassert_equal(ota_module_write_chunk(0, firmware, sizeof(firmware)), 0);
    zassert_equal(ota_module_finish_update(), 0);

    ota_state_t st;
    zassert_equal(ota_module_get_state(&st), 0);
    zassert_equal(st, OTA_STATE_READY_REBOOT);
    zassert_equal(ota_module_get_status(), MODULE_STATUS_RUNNING, NULL);

    teardown_ota_module();
}

ZTEST(ota_module, test_begin_after_error_recovers) {
    setup_ota_module();

    zassert_equal(ota_module_begin_update(), 0);
    zassert_not_equal(ota_module_finish_update(), 0, "finish without data should fail");

    ota_state_t st;
    zassert_equal(ota_module_get_state(&st), 0);
    zassert_equal(st, OTA_STATE_ERROR);

    zassert_equal(ota_module_begin_update(), 0);
    zassert_equal(ota_module_get_state(&st), 0);
    zassert_equal(st, OTA_STATE_DOWNLOADING);

    (void) ota_module_abort_update();
    teardown_ota_module();
}
