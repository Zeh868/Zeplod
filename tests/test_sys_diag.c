/**
 * @file test_sys_diag.c
 * @brief sys_diag 单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            模块注册用例末尾补注销清理；去 const
 *                                                 类型压制（改本地可写缓冲）
 *
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <zeplod/event_system.h>
#include <zeplod/sys_diag.h>

#if defined(CONFIG_SYS_MEMORY_ENABLE)
#include <zeplod/sys_memory.h>
#endif

#if defined(CONFIG_MODULE_MANAGER)
#include <zeplod/module_manager.h>

static int diag_stub_init(void* config) {
    (void) config;
    return 0;
}

static int diag_stub_start(void) {
    return 0;
}

static int diag_stub_stop(void) {
    return 0;
}

static void diag_stub_on_event(const event_t* event, void* user_data) {
    (void) event;
    (void) user_data;
}

DECLARE_MODULE_INTERFACE_MINIMAL(diag_stub);
#endif

ZTEST_SUITE(sys_diag_tests, NULL, NULL, NULL, NULL, NULL);

static void setup_event_for_diag(void) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);
}

ZTEST(sys_diag_tests, test_init_idempotent) {
    zassert_equal(sys_diag_init(), 0, NULL);
    zassert_equal(sys_diag_init(), 0, NULL);
}

ZTEST(sys_diag_tests, test_collect_returns_sane_values) {
    setup_event_for_diag();
    k_sleep(K_MSEC(1));

    sys_diag_snapshot_t snap;
    zassert_equal(sys_diag_collect(&snap), 0, NULL);

    zassert_true(snap.event_queue_capacity > 0U, "capacity should be > 0");
    zassert_true(snap.event_queue_depth <= snap.event_queue_capacity, "depth <= capacity");
    zassert_true(snap.uptime_ms >= 1U, "uptime should advance");
}

ZTEST(sys_diag_tests, test_format_non_empty) {
    setup_event_for_diag();

    sys_diag_snapshot_t snap;
    zassert_equal(sys_diag_collect(&snap), 0, NULL);

    char buf[256];
    zassert_equal(sys_diag_format(&snap, buf, sizeof(buf)), 0, NULL);
    zassert_true(strlen(buf) > 10U, "formatted output too short");
    zassert_not_null(strstr(buf, "diag:"), "missing prefix");
    zassert_not_null(strstr(buf, "drop="), "missing dropped count");
}

ZTEST(sys_diag_tests, test_collect_null_rejected) {
    zassert_equal(sys_diag_collect(NULL), -EINVAL, NULL);
}

ZTEST(sys_diag_tests, test_format_invalid_args) {
    sys_diag_snapshot_t snap;
    char                writable[2] = "x"; /* 本地可写缓冲，避免对字面量去 const */

    zassert_equal(sys_diag_format(NULL, writable, sizeof(writable)), -EINVAL, NULL);
    zassert_equal(sys_diag_format(&snap, NULL, 16U), -EINVAL, NULL);
    zassert_equal(sys_diag_format(&snap, writable, 0U), -EINVAL, NULL);
}

ZTEST(sys_diag_tests, test_format_buffer_too_small) {
    sys_diag_snapshot_t snap;
    char                buf[32];

    memset(&snap, 0, sizeof(snap));
    zassert_equal(sys_diag_format(&snap, buf, sizeof(buf)), -ENOSPC, NULL);
}

ZTEST(sys_diag_tests, test_export_json_contains_fields) {
    setup_event_for_diag();

    sys_diag_snapshot_t snap;
    char                buf[384];

    zassert_equal(sys_diag_collect(&snap), 0, NULL);
    zassert_equal(sys_diag_export_json(&snap, buf, sizeof(buf)), 0, NULL);
    zassert_not_null(strstr(buf, "\"uptime_ms\""), NULL);
    zassert_not_null(strstr(buf, "\"heap_free\""), NULL);
}

ZTEST(sys_diag_tests, test_export_json_null_snap_collects) {
    char buf[384];

    setup_event_for_diag();
    zassert_equal(sys_diag_export_json(NULL, buf, sizeof(buf)), 0, NULL);
    zassert_true(buf[0] == '{', NULL);
}

ZTEST(sys_diag_tests, test_export_json_buffer_too_small) {
    sys_diag_snapshot_t snap;
    char                buf[16];

    memset(&snap, 0, sizeof(snap));
    zassert_equal(sys_diag_export_json(&snap, buf, sizeof(buf)), -ENOSPC, NULL);
}

#if defined(CONFIG_SYS_MEMORY_ENABLE)
ZTEST(sys_diag_tests, test_collect_heap_when_memory_enabled) {
    zassert_equal(sys_mem_init(NULL), 0, NULL);

    sys_diag_snapshot_t snap;
    zassert_equal(sys_diag_collect(&snap), 0, NULL);
    zassert_true(snap.heap_free_bytes > 0U, "heap free should be > 0 after init");
}
#endif

#if defined(CONFIG_MODULE_MANAGER)
ZTEST(sys_diag_tests, test_collect_modules_when_manager_enabled) {
    uint32_t module_id = 0U;
    int      ret;

    setup_event_for_diag();

    ret = module_manager_init();
    zassert_true(ret == 0 || ret == -EALREADY, "module_manager_init: %d", ret);

    ret = module_manager_start();
    zassert_true(ret == 0 || ret == -EALREADY || ret == MODULE_ERR_ALREADY_RUNNING, "module_manager_start: %d", ret);

    zassert_equal(module_manager_register(&diag_stub_interface, NULL, &module_id), 0, NULL);
    zassert_true(module_id > 0U, "module id should be non-zero");

    sys_diag_snapshot_t snap;
    zassert_equal(sys_diag_collect(&snap), 0, NULL);
    zassert_true(snap.module_count >= 1U, "module_count should reflect registration");

    /* 用例自清理：注销桩模块，避免残留注册影响同构建内其他套件 */
    zassert_equal(module_manager_unregister(module_id), 0, "unregister diag_stub failed");
}
#endif
