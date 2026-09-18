/**
 * @file test_example_module_b.c
 * @brief 示例模块 B 单元测试
 * @author OpenClaw Agent
 * @version 1.2
 * @date 2026-04-10
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-10       1.0            agent          为 example_module_b 编写完整测试用例
 * 2026-09-18       1.1            zeh            发送/接收/统计用例按桩实现契约补强
 * 2026-09-18       1.2            zeh            复审整改：配置结构体删除后 init 用例改传 NULL
 *                                                 断言；无效命令魔数具名；teardown 补
 *                                                 event_dispatcher_stop 对齐 A 套件
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>
#include <string.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include <zeplod/example_module_b.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(test_example_module_b);

/* 无效控制命令探针：落在 example_module_b_control 合法命令集合之外 */
#define CMD_INVALID 9999U

/* =============================================================================
 * 测试夹具 (Test Fixtures)
 * ============================================================================= */

static void* test_suite_setup(void) {
    int ret;

    /* 全局初始化 - 允许重复初始化（返回 -EALREADY） */
    ret = event_system_init();
    zassert_true(ret == EVENT_OK || ret == -EALREADY, "事件系统初始化失败: %d", ret);

    ret = event_system_start();
    zassert_true(ret == EVENT_OK || ret == -EALREADY, "事件系统启动失败: %d", ret);

    ret = module_manager_init();
    zassert_true(ret == 0 || ret == -EALREADY, "模块管理器初始化失败: %d", ret);

    ret = module_manager_start();
    zassert_true(ret == 0 || ret == -EALREADY, "模块管理器启动失败: %d", ret);

    return NULL;
}

static void test_suite_teardown(void* fixture) {
    (void) fixture;
    module_manager_shutdown();
    /* 与其他套件对齐：分发器未初始化时 stop 幂等返回 EVENT_OK，不产生副作用 */
    (void) event_dispatcher_stop();
    event_system_stop();
}

/* =============================================================================
 * 测试用例：模块初始化
 * ============================================================================= */

/**
 * @brief 测试模块初始化 - NULL 配置
 */
ZTEST(example_module_b, test_init_null_config) {
    /* 先确保处于干净状态 */
    example_module_b_shutdown();

    int ret = example_module_b_init(NULL);
    zassert_equal(ret, 0, "NULL 配置初始化应返回 0");

    module_status_t status = example_module_b_get_status();
    zassert_equal(status, MODULE_STATUS_INITIALIZED, "初始化后状态应为 INITIALIZED");
}

/**
 * @brief 测试模块初始化（模块无运行参数，config 传 NULL 或任意指针均被忽略）
 */
ZTEST(example_module_b, test_init_valid_config) {
    example_module_b_init(NULL);

    module_status_t status = example_module_b_get_status();
    zassert_equal(status, MODULE_STATUS_INITIALIZED, "初始化后状态应为 INITIALIZED");
}

/* =============================================================================
 * 测试用例：模块启动与停止
 * ============================================================================= */

/**
 * @brief 测试模块启动
 */
ZTEST(example_module_b, test_start) {
    example_module_b_init(NULL);

    int ret = example_module_b_start();
    zassert_equal(ret, 0, "模块启动应返回 0");

    module_status_t status = example_module_b_get_status();
    zassert_equal(status, MODULE_STATUS_RUNNING, "启动后状态应为 RUNNING");

    example_module_b_stop();
    example_module_b_shutdown();
}

/**
 * @brief 测试模块停止
 */
ZTEST(example_module_b, test_stop) {
    example_module_b_init(NULL);
    example_module_b_start();

    int ret = example_module_b_stop();
    zassert_equal(ret, 0, "模块停止应返回 0");

    module_status_t status = example_module_b_get_status();
    zassert_equal(status, MODULE_STATUS_STOPPED, "停止后状态应为 STOPPED");

    example_module_b_shutdown();
}

/**
 * @brief 测试停止后重启
 */
ZTEST(example_module_b, test_restart) {
    example_module_b_init(NULL);
    example_module_b_start();
    example_module_b_stop();

    int ret = example_module_b_start();
    zassert_equal(ret, 0, "重启应成功");

    module_status_t status = example_module_b_get_status();
    zassert_equal(status, MODULE_STATUS_RUNNING, "重启后状态应为 RUNNING");

    example_module_b_stop();
    example_module_b_shutdown();
}

/* =============================================================================
 * 测试用例：通信功能
 * ============================================================================= */

/**
 * @brief 测试发送数据 - 空参数
 */
ZTEST(example_module_b, test_send_null) {
    example_module_b_init(NULL);
    example_module_b_start();

    /* NULL 数据应返回错误 */
    int ret = example_module_b_send(NULL, 100);
    zassert_true(ret < 0, "NULL 数据发送应返回错误");

    example_module_b_stop();
    example_module_b_shutdown();
}

/**
 * @brief 测试接收数据 - 空参数
 */
ZTEST(example_module_b, test_receive_null) {
    example_module_b_init(NULL);
    example_module_b_start();

    /* NULL 缓冲区应返回错误 */
    int ret = example_module_b_receive(NULL, 100);
    zassert_true(ret < 0, "NULL 缓冲区接收应返回错误");

    example_module_b_stop();
    example_module_b_shutdown();
}

/**
 * @brief 测试发送接收流程
 *
 * 按当前桩实现契约断言：send 原样返回入参长度；receive 固定返回 0 字节
 * 并清零缓冲。实现尚无真实回环通道，无法做内容回环比对——若后续接入真实
 * 通道，应同步把本用例升级为「发送内容 == 接收内容」。
 */
ZTEST(example_module_b, test_send_receive) {
    example_module_b_init(NULL);
    example_module_b_start();

    /* 发送测试数据：返回值即实际发送字节数 */
    const char test_data[] = "Hello, Module B!";
    int        sent = example_module_b_send(test_data, sizeof(test_data));
    zassert_equal(sent, (int) sizeof(test_data), "发送应返回全部字节数");

    /* 接收：桩实现固定返回 0 字节，且按契约清零输出缓冲 */
    char recv_buf[64];
    memset(recv_buf, 0xA5, sizeof(recv_buf));
    int received = example_module_b_receive(recv_buf, sizeof(recv_buf));
    zassert_equal(received, 0, "桩实现接收应返回 0 字节");
    for (size_t i = 0; i < sizeof(recv_buf); i++) {
        zassert_equal(recv_buf[i], 0, "接收缓冲应被清零（偏移 %zu）", i);
    }

    example_module_b_stop();
    example_module_b_shutdown();
}

/**
 * @brief 测试通信统计
 *
 * 桩实现的 get_stats 恒写 0（无真实计数器）：发送前后统计均应为 0。
 * 固定该契约——若实现开始真实计数，此处失败即提醒同步更新断言。
 */
ZTEST(example_module_b, test_get_stats) {
    example_module_b_init(NULL);
    example_module_b_start();

    uint32_t tx_count = 0, rx_count = 0, errors = 0;

    /* 获取初始统计 */
    example_module_b_get_stats(&tx_count, &rx_count, &errors);
    zassert_equal(tx_count, 0U, "初始发送计数应为 0");
    zassert_equal(rx_count, 0U, "初始接收计数应为 0");
    zassert_equal(errors, 0U, "初始错误计数应为 0");

    /* 发送一些数据 */
    const char test_data[] = "Test data";
    int        sent = example_module_b_send(test_data, sizeof(test_data));
    zassert_equal(sent, (int) sizeof(test_data), "发送应返回全部字节数");

    /* 再次获取统计 */
    uint32_t tx_after = 0, rx_after = 0, errors_after = 0;
    example_module_b_get_stats(&tx_after, &rx_after, &errors_after);
    zassert_equal(tx_after, 0U, "桩实现不计数，发送后统计仍应为 0");
    zassert_equal(rx_after, 0U, "桩实现不计数，接收统计仍应为 0");
    zassert_equal(errors_after, 0U, "桩实现不计数，错误统计仍应为 0");

    example_module_b_stop();
    example_module_b_shutdown();
}

/* =============================================================================
 * 测试用例：模块控制
 * ============================================================================= */

/**
 * @brief 测试无效控制命令
 */
ZTEST(example_module_b, test_control_invalid) {
    example_module_b_init(NULL);

    int ret = example_module_b_control(CMD_INVALID, NULL);
    zassert_true(ret != 0, "无效命令应返回错误");

    example_module_b_shutdown();
}

/* =============================================================================
 * 测试用例：事件处理
 * ============================================================================= */

/**
 * @brief 测试事件处理 - NULL 事件
 */
ZTEST(example_module_b, test_event_null) {
    example_module_b_on_event(NULL, NULL);
    /* 不应崩溃 */
}

/**
 * @brief 测试事件处理 - NULL user_data
 */
ZTEST(example_module_b, test_event_null_user_data) {
    event_t event = {0};
    event.type = EVENT_TYPE_GENERIC;
    event.priority = EVENT_PRIORITY_NORMAL;
    event.data_len = 0;

    example_module_b_on_event(&event, NULL);
    /* 不应崩溃 */
}

/* =============================================================================
 * 测试用例：生命周期
 * ============================================================================= */

/**
 * @brief 测试完整生命周期
 */
ZTEST(example_module_b, test_lifecycle) {
    module_status_t status;

    /* 初始化 */
    example_module_b_init(NULL);
    status = example_module_b_get_status();
    zassert_equal(status, MODULE_STATUS_INITIALIZED, "初始化后应为 INITIALIZED");

    /* 启动 */
    example_module_b_start();
    status = example_module_b_get_status();
    zassert_equal(status, MODULE_STATUS_RUNNING, "启动后应为 RUNNING");

    /* 停止 */
    example_module_b_stop();
    status = example_module_b_get_status();
    zassert_equal(status, MODULE_STATUS_STOPPED, "停止后应为 STOPPED");

    /* 关闭 */
    example_module_b_shutdown();
    status = example_module_b_get_status();
    zassert_equal(status, MODULE_STATUS_UNINITIALIZED, "关闭后应为 UNINITIALIZED");
}

/* =============================================================================
 * 测试用例：接口
 * ============================================================================= */

/**
 * @brief 测试获取接口
 */
ZTEST(example_module_b, test_get_interface) {
    const module_interface_t* iface = example_module_b_get_interface();

    zassert_not_null(iface, "接口不应为 NULL");
    zassert_true(strcmp(iface->name, "example_module_b") == 0, "接口名称应匹配");
    zassert_true(iface->version == MODULE_VERSION(1, 0, 0), "版本号应匹配");
}

/* =============================================================================
 * 测试套件
 * ============================================================================= */

ZTEST_SUITE(example_module_b, NULL, test_suite_setup, NULL, NULL, test_suite_teardown);
