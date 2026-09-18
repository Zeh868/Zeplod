/**
 * @file test_recovery_policy.c
 * @brief recovery_policy 模块单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            初始版本
 * 2026-06-13       1.1            zeh            增加 clear_all / 升级路径用例
 * 2026-09-18       1.2            zeh            fault dump 断言改按导出格式解析
 *                                                 （magic + 记录数），不再猜字节偏移
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <string.h>

#include <zeplod/event_system.h>
#include <zeplod/module_manager.h>
#include <zeplod/recovery_policy.h>
#include <zeplod/sys_fault_dump.h>

/* sys_fault_dump 导出的环头部 magic（源：sys_fault_dump.c FAULT_DUMP_MAGIC） */
#define TEST_FAULT_DUMP_RING_MAGIC 0x5A4C4446U

/* =============================================================================
 * 测试用桩模块（start 可配置失败次数）
 * ============================================================================= */

static int g_recovery_test_fail_starts;
static int g_recovery_test_start_calls;

static int recovery_test_mod_init(void* config) {
    ARG_UNUSED(config);
    return 0;
}

static int recovery_test_mod_start(void) {
    g_recovery_test_start_calls++;
    if (g_recovery_test_fail_starts > 0) {
        g_recovery_test_fail_starts--;
        return -EIO;
    }
    return 0;
}

static int recovery_test_mod_stop(void) {
    return 0;
}

static void recovery_test_mod_on_event(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
}

DECLARE_MODULE_INTERFACE_MINIMAL(recovery_test_mod);

/* =============================================================================
 * 测试夹具
 * ============================================================================= */

static void setup_recovery_policy(void) {
    zassert_equal(module_manager_init(), MODULE_OK, NULL);
    zassert_equal(module_manager_start(), MODULE_OK, NULL);
    zassert_equal(recovery_policy_init(NULL), 0, NULL);
    zassert_equal(recovery_policy_start(), 0, NULL);
    recovery_policy_reset_restart_counts();
    g_recovery_test_start_calls = 0;
}

static void teardown_recovery_policy(void) {
    (void) recovery_policy_shutdown();
    (void) module_manager_stop();
    (void) module_manager_shutdown();
}

ZTEST_SUITE(recovery_policy_tests, NULL, NULL, NULL, NULL, NULL);

/* =============================================================================
 * 用例
 * ============================================================================= */

ZTEST(recovery_policy_tests, test_on_module_error_invalid_id) {
    setup_recovery_policy();
    zassert_equal(recovery_policy_on_module_error(0), -EINVAL, NULL);
    teardown_recovery_policy();
}

ZTEST(recovery_policy_tests, test_restart_module_after_start_error) {
    uint32_t      mod_id;
    module_info_t info;

    setup_recovery_policy();

    g_recovery_test_fail_starts = 1;
    zassert_equal(module_manager_register(&recovery_test_mod_interface, NULL, &mod_id), MODULE_OK, NULL);

    zassert_equal(module_manager_start_module(mod_id), -EIO, "first start should fail");

    zassert_equal(module_manager_get_module_info(mod_id, &info), MODULE_OK, NULL);
    zassert_equal(info.status, MODULE_STATUS_RUNNING, "recovery should restart module");
    zassert_equal(recovery_policy_get_restart_count(mod_id), 1U, NULL);

    zassert_equal(module_manager_unregister(mod_id), MODULE_OK, NULL);
    teardown_recovery_policy();
}

ZTEST(recovery_policy_tests, test_clear_error_allows_restart) {
    uint32_t mod_id;

    zassert_equal(module_manager_init(), MODULE_OK, NULL);
    zassert_equal(module_manager_start(), MODULE_OK, NULL);
    zassert_equal(module_manager_register(&recovery_test_mod_interface, NULL, &mod_id), MODULE_OK, NULL);

    g_recovery_test_fail_starts = 1;
    zassert_equal(module_manager_start_module(mod_id), -EIO, NULL);
    zassert_equal(module_manager_clear_error_state(mod_id), MODULE_OK, NULL);

    g_recovery_test_fail_starts = 0;
    zassert_equal(module_manager_start_module(mod_id), MODULE_OK, NULL);

    zassert_equal(module_manager_unregister(mod_id), MODULE_OK, NULL);
    (void) module_manager_shutdown();
}

ZTEST(recovery_policy_tests, test_clear_all_error_states) {
    uint32_t      mod_id;
    module_info_t info;

    zassert_equal(module_manager_init(), MODULE_OK, NULL);
    zassert_equal(module_manager_start(), MODULE_OK, NULL);
    zassert_equal(module_manager_register(&recovery_test_mod_interface, NULL, &mod_id), MODULE_OK, NULL);

    g_recovery_test_fail_starts = 1;
    zassert_equal(module_manager_start_module(mod_id), -EIO, NULL);
    zassert_equal(module_manager_get_module_info(mod_id, &info), MODULE_OK, NULL);
    zassert_equal(info.status, MODULE_STATUS_ERROR, NULL);

    zassert_equal(module_manager_clear_all_error_states(), MODULE_OK, NULL);
    zassert_equal(module_manager_get_module_info(mod_id, &info), MODULE_OK, NULL);
    zassert_equal(info.status, MODULE_STATUS_STOPPED, NULL);

    g_recovery_test_fail_starts = 0;
    zassert_equal(module_manager_start_module(mod_id), MODULE_OK, NULL);

    zassert_equal(module_manager_unregister(mod_id), MODULE_OK, NULL);
    (void) module_manager_shutdown();
}

ZTEST(recovery_policy_tests, test_escalate_restart_all_after_max_retries) {
    uint32_t      mod_id;
    module_info_t info;

    setup_recovery_policy();

    g_recovery_test_fail_starts = 100;
    g_recovery_test_start_calls = 0;
    zassert_equal(module_manager_register(&recovery_test_mod_interface, NULL, &mod_id), MODULE_OK, NULL);

    /* MAX_RESTARTS=2：单模块重试耗尽后应升级 restart_all 并再次尝试 start */
    zassert_equal(module_manager_start_module(mod_id), -EIO, NULL);
    zassert_true(g_recovery_test_start_calls >= 3, "restart_all should retry start after clear ERROR");

    zassert_equal(module_manager_get_module_info(mod_id, &info), MODULE_OK, NULL);
    zassert_equal(info.status, MODULE_STATUS_ERROR, "module still fails after restart_all");
    zassert_equal(recovery_policy_get_restart_count(mod_id), 2U, NULL);

    zassert_equal(module_manager_unregister(mod_id), MODULE_OK, NULL);
    teardown_recovery_policy();
}

ZTEST(recovery_policy_tests, test_fault_dump_on_module_error) {
    uint32_t mod_id;
    uint8_t  buf[512];
    size_t   n = 0U;
    uint32_t ring_magic = 0U;
    uint16_t ring_count = 0U;

    setup_recovery_policy();
    sys_fault_dump_clear();

    g_recovery_test_fail_starts = 1;
    zassert_equal(module_manager_register(&recovery_test_mod_interface, NULL, &mod_id), MODULE_OK, NULL);
    zassert_equal(module_manager_start_module(mod_id), -EIO, NULL);

    zassert_equal(sys_fault_dump_export(buf, sizeof(buf), &n), 0, NULL);

    /* 导出格式为原始 fault_dump_ring_t：{magic u32, head u16, count u16, entries[]}。
     * 按头三字段解析并校验记录数，不再对具体字节偏移做猜测性断言 */
    zassert_true(n >= sizeof(ring_magic) + 2U * sizeof(uint16_t), "导出长度应至少包含环头部");
    memcpy(&ring_magic, buf, sizeof(ring_magic));
    memcpy(&ring_count, buf + sizeof(ring_magic) + sizeof(uint16_t), sizeof(ring_count));
    zassert_equal(ring_magic, TEST_FAULT_DUMP_RING_MAGIC, "fault ring magic 不匹配");
    zassert_true(ring_count >= 1U, "模块启动失败后 fault ring 应含至少一条记录");

    zassert_equal(module_manager_unregister(mod_id), MODULE_OK, NULL);
    teardown_recovery_policy();
}

ZTEST(recovery_policy_tests, test_control_reset_counts) {
    setup_recovery_policy();
    zassert_equal(recovery_policy_control(RECOVERY_POLICY_CMD_RESET_COUNTS, NULL), 0, NULL);
    zassert_equal(recovery_policy_get_restart_count(1U), 0U, NULL);
    teardown_recovery_policy();
}
