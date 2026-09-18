/**
 * @file test_factory_mode_no_kv.c
 * @brief factory_mode 在无 app_kv 时的行为测试
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh             初始版本
 * 2026-09-18       1.1            zeh             补套件 teardown 停分发器与事件系统
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <errno.h>

#include <zeplod/app_config.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include <zeplod/factory_mode.h>

static void* factory_no_kv_suite_setup(void) {
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

static void factory_no_kv_suite_teardown(void* fixture) {
    (void) fixture;

    /* 套件收尾对称：setup 启动了分发器与事件系统，这里统一停掉，
     * 避免分发线程泄漏影响同构建内的后续套件 */
    (void) event_dispatcher_stop();

    (void) event_system_stop();
}

ZTEST_SUITE(factory_mode_no_kv_tests, NULL, factory_no_kv_suite_setup, NULL, NULL, factory_no_kv_suite_teardown);

ZTEST(factory_mode_no_kv_tests, test_finalize_disabled_without_app_kv) {
    zassert_equal(factory_mode_control(FACTORY_MODE_CMD_RESET, NULL), 0, NULL);
    zassert_equal(factory_mode_init(NULL), 0, NULL);
    zassert_equal(factory_mode_start(), 0, NULL);

    zassert_equal(factory_mode_enter(), 0, NULL);
    zassert_equal(factory_mode_run_gpio_loopback(), 0, NULL);
    zassert_equal(factory_mode_set_calibration("offset", "12.5"), 0, NULL);
    zassert_equal(factory_mode_finalize_pass(), APP_ERR_DISABLED, NULL);

    (void) factory_mode_shutdown();
}
