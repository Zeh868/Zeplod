/**
 * @file test_example_module_multi_dep.c
 * @brief 多依赖示例模块单元测试
 * @author OpenClaw Agent
 * @version 1.1
 * @date 2026-04-10
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-10       1.0            agent          为 example_module_multi_dep 编写测试用例
 * 2026-09-18       1.1            zeh            版本断言改为具体值（消除恒真断言）；
 *                                                 接口函数指针按 MINIMAL_WITH_DEPS
 *                                                 契约精确锚定
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>
#include <string.h>
#include <zeplod/event_system.h>
#include <zeplod/example_module_a.h>
#include <zeplod/example_module_b.h>
#include <zeplod/example_module_multi_dep.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(test_example_module_multi_dep);

/* =============================================================================
 * 测试夹具
 * ============================================================================= */

static void* test_suite_setup(void) {
    zassert_equal(event_system_init(), EVENT_OK, "事件系统初始化失败");
    zassert_equal(module_manager_init(), 0, "模块管理器初始化失败");
    zassert_equal(module_manager_start(), 0, "模块管理器启动失败");
    return NULL;
}

static void test_suite_teardown(void* fixture) {
    (void) fixture;
    module_manager_shutdown();
    event_system_stop();
}

/* =============================================================================
 * 测试用例：接口获取
 * ============================================================================= */

/**
 * @brief 测试获取模块接口
 */
ZTEST(example_module_multi_dep, test_get_interface) {
    const module_interface_t* iface = example_module_multi_dep_get_interface();

    zassert_not_null(iface, "接口不应为 NULL");
    zassert_true(strcmp(iface->name, "example_module_multi_dep") == 0, "接口名称应匹配");
}

/**
 * @brief 测试接口中的 depends_on 数组
 */
ZTEST(example_module_multi_dep, test_depends_on) {
    const module_interface_t* iface = example_module_multi_dep_get_interface();

    zassert_not_null(iface->depends_on, "depends_on 不应为 NULL");
    zassert_not_null(iface->depends_on[0], "第一个依赖不应为 NULL");
    zassert_not_null(iface->depends_on[1], "第二个依赖不应为 NULL");
    zassert_is_null(iface->depends_on[2], "依赖数组应以 NULL 结尾");

    /* 验证依赖的模块名称 */
    zassert_true(strcmp(iface->depends_on[0], "example_module_a") == 0, "第一个依赖应为 example_module_a");
    zassert_true(strcmp(iface->depends_on[1], "example_module_b") == 0, "第二个依赖应为 example_module_b");
}

/* =============================================================================
 * 测试用例：版本信息
 * ============================================================================= */

/**
 * @brief 测试版本号
 *
 * 接口由 DECLARE_MODULE_INTERFACE_MINIMAL_WITH_DEPS 生成，版本号宏写死为
 * MODULE_VERSION(1, 0, 0)——直接断言具体值，恒真范围断言无回归价值。
 */
ZTEST(example_module_multi_dep, test_version) {
    const module_interface_t* iface = example_module_multi_dep_get_interface();

    /* 验证版本号编码正确（与注册宏的实参一致） */
    uint8_t major = MODULE_VERSION_MAJOR(iface->version);
    uint8_t minor = MODULE_VERSION_MINOR(iface->version);
    uint8_t patch = MODULE_VERSION_PATCH(iface->version);

    zassert_equal(major, 1U, "主版本号应为 1");
    zassert_equal(minor, 0U, "次版本号应为 0");
    zassert_equal(patch, 0U, "补丁号应为 0");
    zassert_equal(iface->version, MODULE_VERSION(1, 0, 0), "整体版本号应为 1.0.0");
}

/* =============================================================================
 * 测试用例：优先级
 * ============================================================================= */

/**
 * @brief 测试模块优先级
 */
ZTEST(example_module_multi_dep, test_priority) {
    const module_interface_t* iface = example_module_multi_dep_get_interface();

    /* 注册宏固定为 MODULE_PRIORITY_NORMAL，直接锚定具体值 */
    zassert_equal(iface->priority, MODULE_PRIORITY_NORMAL, "优先级应为 NORMAL");
}

/* =============================================================================
 * 测试用例：模块注册（依赖排序验证）
 * ============================================================================= */

/**
 * @brief 测试按依赖顺序注册模块
 *
 * 注意：此测试验证模块管理器能够处理带依赖的模块注册。
 * 实际运行时依赖排序需要启用 CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES
 */
ZTEST(example_module_multi_dep, test_register_with_deps) {
    uint32_t id_a = 0, id_b = 0, id_multi = 0;

    /* 先注册依赖的模块 */
    zassert_equal(module_manager_register(example_module_a_get_interface(), NULL, &id_a), 0, "注册 module_a 失败");
    zassert_true(id_a > 0, "module_a ID 应 > 0");

    zassert_equal(module_manager_register(example_module_b_get_interface(), NULL, &id_b), 0, "注册 module_b 失败");
    zassert_true(id_b > 0, "module_b ID 应 > 0");

    /* 再注册带依赖的模块 */
    zassert_equal(module_manager_register(example_module_multi_dep_get_interface(), NULL, &id_multi), 0,
                  "注册 module_multi_dep 失败");
    zassert_true(id_multi > 0, "module_multi_dep ID 应 > 0");

    /* 验证 ID 分配正确 */
    zassert_true(id_multi > id_a, "multi_dep 的 ID 应大于 module_a");
    zassert_true(id_multi > id_b, "multi_dep 的 ID 应大于 module_b");
}

/* =============================================================================
 * 测试用例：接口函数指针
 * ============================================================================= */

/**
 * @brief 测试接口函数指针完整性
 *
 * MINIMAL_WITH_DEPS 宏的确定契约：init/start/stop/on_event 必须实现，
 * shutdown/get_status/control 固定为 NULL（管理器调用前自会判空）。
 */
ZTEST(example_module_multi_dep, test_interface_functions) {
    const module_interface_t* iface = example_module_multi_dep_get_interface();

    zassert_not_null(iface->init, "init 不应为 NULL");
    zassert_not_null(iface->start, "start 不应为 NULL");
    zassert_not_null(iface->stop, "stop 不应为 NULL");
    zassert_not_null(iface->on_event, "on_event 不应为 NULL");

    zassert_is_null(iface->shutdown, "MINIMAL 接口 shutdown 应为 NULL");
    zassert_is_null(iface->get_status, "MINIMAL 接口 get_status 应为 NULL");
    zassert_is_null(iface->control, "MINIMAL 接口 control 应为 NULL");
}

/* =============================================================================
 * 测试套件
 * ============================================================================= */

ZTEST_SUITE(example_module_multi_dep, NULL, test_suite_setup, NULL, NULL, test_suite_teardown);
