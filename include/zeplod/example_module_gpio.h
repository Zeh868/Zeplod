/**
 * @file example_module_gpio.h
 * @brief GPIO 控制示例模块
 *
 * 演示如何使用 Zephyr GPIO API 控制 LED 和读取按键。
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            删除不生效的 led_pin/button_pin 字段
 * 2026-09-18       1.2            zeh            复审整改：事件号 30-32 迁至 33-35，消除与 EVENT_TYPE_DATA_BUS_AVAILABLE(30) 的 ID 冲突
 *                                                 （实际引脚由 devicetree alias 决定）；
 *                                                 补全模块接口 Doxygen
 *
 */

#ifndef EXAMPLE_MODULE_GPIO_H
#define EXAMPLE_MODULE_GPIO_H

#include <zeplod/module_base.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 模块配置
 * ============================================================================= */

typedef struct {
    uint32_t blink_interval_ms; /* LED 闪烁间隔（毫秒），须 > 0 */
    bool     enable_button;     /* 是否启用按键检测（devicetree 无 sw0 时强制关闭） */
} example_module_gpio_config_t;

/* =============================================================================
 * 模块接口
 * ============================================================================= */

/**
 * @brief 初始化 GPIO 模块（由模块管理器调用）
 * @param config 指向 example_module_gpio_config_t 的指针；NULL 时使用默认配置
 *               （blink_interval_ms=500，enable_button=true）
 * @return 0 成功；-EIO 事件类型注册失败
 */
int example_module_gpio_init(void* config);

/**
 * @brief 启动模块：配置 LED/按键引脚、注册中断回调并创建工作线程
 * @return 0 成功；-1 状态非法；-EIO GPIO 配置或回调注册失败
 */
int example_module_gpio_start(void);

/**
 * @brief 停止模块：有界汇合工作线程，并注销按键中断与回调
 * @return 0 成功（含未运行时直接返回）
 */
int example_module_gpio_stop(void);

/**
 * @brief 关停模块：先 stop，再将状态置为 UNINITIALIZED
 * @return 恒为 0
 */
int example_module_gpio_shutdown(void);

/**
 * @brief 模块事件回调：处理 EVENT_TYPE_GPIO_LED_STATE 设置 LED
 * @param event 事件指针；NULL 时直接返回
 * @param user_data 订阅时的用户数据（本模块未使用，可为 NULL）
 */
void example_module_gpio_on_event(const event_t* event, void* user_data);

/**
 * @brief 获取模块当前生命周期状态
 * @return module_status_t 当前状态
 */
module_status_t example_module_gpio_get_status(void);

/**
 * @brief 模块控制命令入口
 * @param cmd GPIO_CMD_* 之一（SET_LED/TOGGLE_LED/GET_BUTTON/SET_BLINK）
 * @param arg cmd 对应的参数指针：SET_LED 传 bool*，GET_BUTTON 传 bool*（输出），
 *            SET_BLINK 传 uint32_t*（毫秒）；TOGGLE_LED 忽略；
 *            需传参的命令为 NULL 时返回 -1
 * @return 0 成功；-1 参数非法或命令不支持
 */
int example_module_gpio_control(int cmd, void* arg);

/**
 * @brief 获取模块接口（供模块管理器注册）
 * @return 指向模块接口的只读指针
 */
const module_interface_t* example_module_gpio_get_interface(void);

/* =============================================================================
 * 模块特定 API
 * ============================================================================= */

/**
 * @brief 设置 LED 状态
 * @param on true 点亮，false 熄灭
 * @return 0 成功，负值错误码
 */
int example_module_gpio_set_led(bool on);

/**
 * @brief 切换 LED 状态
 * @return 当前 LED 状态
 */
bool example_module_gpio_toggle_led(void);

/**
 * @brief 获取按键状态
 * @return true 按键按下，false 未按下
 */
bool example_module_gpio_get_button(void);

/**
 * @brief 设置闪烁间隔
 * @param interval_ms 间隔时间（毫秒）
 * @return 0 成功，负值错误码
 */
int example_module_gpio_set_blink_interval(uint32_t interval_ms);

/* =============================================================================
 * 事件类型定义
 * ============================================================================= */

#define EVENT_TYPE_GPIO_BUTTON_PRESSED  33
#define EVENT_TYPE_GPIO_BUTTON_RELEASED 34
#define EVENT_TYPE_GPIO_LED_STATE       35

/* =============================================================================
 * 控制命令
 * ============================================================================= */

#define GPIO_CMD_SET_LED                1
#define GPIO_CMD_TOGGLE_LED             2
#define GPIO_CMD_GET_BUTTON             3
#define GPIO_CMD_SET_BLINK              4

#ifdef __cplusplus
}
#endif

#endif /* EXAMPLE_MODULE_GPIO_H */
