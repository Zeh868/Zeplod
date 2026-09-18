/**
 * @file test_example_module_gpio.c
 * @brief GPIO 控制示例模块单元测试
 * @author OpenClaw Agent
 * @version 1.1
 * @date 2026-04-10
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-10       1.0            agent          为 example_module_gpio 编写完整测试用例
 * 2026-09-18       1.1            zeh            同步删除 src 侧已移除的 led_pin/
 *                                                 button_pin 配置字段；按键读取用例补
 *                                                 稳定性断言；无效命令魔数具名；teardown
 *                                                 补 event_dispatcher_stop
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>
#include <string.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include <zeplod/example_module_gpio.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(test_example_module_gpio);

/* 无效控制命令探针：落在 GPIO_CMD_* 合法命令集合之外 */
#define CMD_INVALID 9999U

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
    /* 与其他套件对齐：分发器未初始化时 stop 幂等返回 EVENT_OK，不产生副作用 */
    (void) event_dispatcher_stop();
    event_system_stop();
}

/* =============================================================================
 * 测试用例：初始化与配置
 * ============================================================================= */

/**
 * @brief 测试模块初始化 - NULL 配置
 */
ZTEST(example_module_gpio, test_init_null_config) {
    example_module_gpio_shutdown(); /* 确保干净状态 */

    int ret = example_module_gpio_init(NULL);
    zassert_equal(ret, 0, "NULL 配置初始化应返回 0");

    module_status_t status = example_module_gpio_get_status();
    zassert_equal(status, MODULE_STATUS_INITIALIZED, "初始化后状态应为 INITIALIZED");
}

/**
 * @brief 测试模块初始化 - 有效配置
 */
ZTEST(example_module_gpio, test_init_valid_config) {
    example_module_gpio_config_t config = {.blink_interval_ms = 100, .enable_button = true};

    int ret = example_module_gpio_init(&config);
    zassert_equal(ret, 0, "有效配置初始化应返回 0");

    module_status_t status = example_module_gpio_get_status();
    zassert_equal(status, MODULE_STATUS_INITIALIZED, "初始化后状态应为 INITIALIZED");
}

/* =============================================================================
 * 测试用例：生命周期
 * ============================================================================= */

/**
 * @brief 测试模块启动
 */
ZTEST(example_module_gpio, test_start) {
    example_module_gpio_init(NULL);

    int ret = example_module_gpio_start();
    zassert_equal(ret, 0, "模块启动应返回 0");

    module_status_t status = example_module_gpio_get_status();
    zassert_equal(status, MODULE_STATUS_RUNNING, "启动后状态应为 RUNNING");

    example_module_gpio_stop();
    example_module_gpio_shutdown();
}

/**
 * @brief 测试模块停止
 */
ZTEST(example_module_gpio, test_stop) {
    example_module_gpio_init(NULL);
    example_module_gpio_start();

    int ret = example_module_gpio_stop();
    zassert_equal(ret, 0, "模块停止应返回 0");

    module_status_t status = example_module_gpio_get_status();
    zassert_equal(status, MODULE_STATUS_STOPPED, "停止后状态应为 STOPPED");

    example_module_gpio_shutdown();
}

/**
 * @brief 测试完整生命周期
 */
ZTEST(example_module_gpio, test_lifecycle) {
    module_status_t status;

    /* 初始化 */
    example_module_gpio_init(NULL);
    status = example_module_gpio_get_status();
    zassert_equal(status, MODULE_STATUS_INITIALIZED, "初始化后应为 INITIALIZED");

    /* 启动 */
    example_module_gpio_start();
    status = example_module_gpio_get_status();
    zassert_equal(status, MODULE_STATUS_RUNNING, "启动后应为 RUNNING");

    /* 停止 */
    example_module_gpio_stop();
    status = example_module_gpio_get_status();
    zassert_equal(status, MODULE_STATUS_STOPPED, "停止后应为 STOPPED");

    /* 关闭 */
    example_module_gpio_shutdown();
    status = example_module_gpio_get_status();
    zassert_equal(status, MODULE_STATUS_UNINITIALIZED, "关闭后应为 UNINITIALIZED");
}

/* =============================================================================
 * 测试用例：GPIO 控制 API
 * ============================================================================= */

/**
 * @brief 测试设置 LED 状态
 */
ZTEST(example_module_gpio, test_set_led) {
    example_module_gpio_init(NULL);
    example_module_gpio_start();

    /* 点亮 LED */
    int ret = example_module_gpio_set_led(true);
    zassert_equal(ret, 0, "设置 LED 亮应返回 0");

    /* 熄灭 LED */
    ret = example_module_gpio_set_led(false);
    zassert_equal(ret, 0, "设置 LED 灭应返回 0");

    example_module_gpio_stop();
    example_module_gpio_shutdown();
}

/**
 * @brief 测试切换 LED 状态
 */
ZTEST(example_module_gpio, test_toggle_led) {
    example_module_gpio_init(NULL);
    example_module_gpio_start();

    /* 初始状态熄灭 */
    example_module_gpio_set_led(false);

    /* 切换 - 应变为亮 */
    bool state = example_module_gpio_toggle_led();
    zassert_true(state, "切换后应为亮状态");

    /* 再次切换 - 应变为灭 */
    state = example_module_gpio_toggle_led();
    zassert_false(state, "再次切换后应为灭状态");

    example_module_gpio_stop();
    example_module_gpio_shutdown();
}

/**
 * @brief 测试获取按键状态
 *
 * 真值取决于硬件/devicetree（无 sw0 时恒为 false），无法断言具体电平；
 * 但同一静态输入下连续两次读取必须一致——状态抖动即实现缺陷。
 */
ZTEST(example_module_gpio, test_get_button) {
    example_module_gpio_init(NULL);
    example_module_gpio_start();

    /* 获取按键状态 - 不应崩溃 */
    bool pressed = example_module_gpio_get_button();

    /* 再获取一次确认稳定 */
    bool pressed2 = example_module_gpio_get_button();
    zassert_equal(pressed2, pressed, "连续两次按键读取应一致（读值=%d/%d）", pressed, pressed2);

    example_module_gpio_stop();
    example_module_gpio_shutdown();
}

/**
 * @brief 测试设置闪烁间隔
 */
ZTEST(example_module_gpio, test_set_blink_interval) {
    example_module_gpio_init(NULL);
    example_module_gpio_start();

    /* 设置有效间隔 */
    int ret = example_module_gpio_set_blink_interval(200);
    zassert_equal(ret, 0, "设置有效间隔应返回 0");

    /* 设置 0 间隔 - 应返回错误 */
    ret = example_module_gpio_set_blink_interval(0);
    zassert_true(ret != 0, "设置 0 间隔应返回错误");

    example_module_gpio_stop();
    example_module_gpio_shutdown();
}

/* =============================================================================
 * 测试用例：控制命令
 * ============================================================================= */

/**
 * @brief 测试控制命令 - 设置 LED
 */
ZTEST(example_module_gpio, test_control_set_led) {
    example_module_gpio_init(NULL);
    example_module_gpio_start();

    uint8_t led_on = 1;
    int     ret = example_module_gpio_control(GPIO_CMD_SET_LED, &led_on);
    zassert_equal(ret, 0, "SET_LED 命令应返回 0");

    uint8_t led_off = 0;
    ret = example_module_gpio_control(GPIO_CMD_SET_LED, &led_off);
    zassert_equal(ret, 0, "SET_LED(off) 命令应返回 0");

    example_module_gpio_stop();
    example_module_gpio_shutdown();
}

/**
 * @brief 测试控制命令 - 切换 LED
 */
ZTEST(example_module_gpio, test_control_toggle_led) {
    example_module_gpio_init(NULL);
    example_module_gpio_start();

    /* 先确保 LED 处于确定状态 */
    example_module_gpio_set_led(false);

    /* 切换命令 */
    int ret = example_module_gpio_control(GPIO_CMD_TOGGLE_LED, NULL);
    zassert_equal(ret, 0, "TOGGLE_LED 命令应返回 0");

    example_module_gpio_stop();
    example_module_gpio_shutdown();
}

/**
 * @brief 测试控制命令 - 获取按键
 */
ZTEST(example_module_gpio, test_control_get_button) {
    example_module_gpio_init(NULL);
    example_module_gpio_start();

    uint8_t button_state = 0;
    int     ret = example_module_gpio_control(GPIO_CMD_GET_BUTTON, &button_state);
    zassert_equal(ret, 0, "GET_BUTTON 命令应返回 0");
    /* 布尔语义：输出只允许 0/1 两值，出现其他值说明未归一化 */
    zassert_true(button_state == 0U || button_state == 1U, "按键状态应为 0/1，实为 %u", button_state);

    example_module_gpio_stop();
    example_module_gpio_shutdown();
}

/**
 * @brief 测试控制命令 - 设置闪烁
 */
ZTEST(example_module_gpio, test_control_set_blink) {
    example_module_gpio_init(NULL);
    example_module_gpio_start();

    uint32_t interval = 150;
    int      ret = example_module_gpio_control(GPIO_CMD_SET_BLINK, &interval);
    zassert_equal(ret, 0, "SET_BLINK 命令应返回 0");

    example_module_gpio_stop();
    example_module_gpio_shutdown();
}

/**
 * @brief 测试无效控制命令
 */
ZTEST(example_module_gpio, test_control_invalid) {
    example_module_gpio_init(NULL);

    int ret = example_module_gpio_control(CMD_INVALID, NULL);
    zassert_true(ret != 0, "无效命令应返回错误");

    example_module_gpio_shutdown();
}

/* =============================================================================
 * 测试用例：事件处理
 * ============================================================================= */

/**
 * @brief 测试事件处理 - NULL
 */
ZTEST(example_module_gpio, test_event_null) {
    example_module_gpio_on_event(NULL, NULL);
    /* 不应崩溃 */

    event_t event = {0};
    event.type = EVENT_TYPE_GENERIC;
    event.priority = EVENT_PRIORITY_NORMAL;
    event.data_len = 0;
    example_module_gpio_on_event(&event, NULL);
    /* 不应崩溃 */
}

/**
 * @brief 测试事件类型常量
 */
ZTEST(example_module_gpio, test_event_types) {
    /* 验证事件类型定义正确 */
    zassert_true(EVENT_TYPE_GPIO_BUTTON_PRESSED > 0, "按键按下事件类型应 > 0");
    zassert_true(EVENT_TYPE_GPIO_BUTTON_RELEASED > 0, "按键释放事件类型应 > 0");
    zassert_true(EVENT_TYPE_GPIO_LED_STATE > 0, "LED状态事件类型应 > 0");
    zassert_true(EVENT_TYPE_GPIO_BUTTON_PRESSED != EVENT_TYPE_GPIO_BUTTON_RELEASED, "按下和释放事件类型应不同");
}

/* =============================================================================
 * 测试用例：接口
 * ============================================================================= */

/**
 * @brief 测试获取接口
 */
ZTEST(example_module_gpio, test_get_interface) {
    const module_interface_t* iface = example_module_gpio_get_interface();

    zassert_not_null(iface, "接口不应为 NULL");
    zassert_true(strcmp(iface->name, "example_module_gpio") == 0, "接口名称应匹配");
    zassert_true(iface->version == MODULE_VERSION(1, 0, 0), "版本号应匹配");
}

/* =============================================================================
 * 测试套件
 * ============================================================================= */

ZTEST_SUITE(example_module_gpio, NULL, test_suite_setup, NULL, NULL, test_suite_teardown);
