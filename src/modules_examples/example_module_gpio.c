/**
 * @file example_module_gpio.c
 * @brief GPIO 控制示例模块实现
 *
 * 演示如何使用 Zephyr GPIO API 控制 LED 和读取按键。
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 *
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <zeplod/app_config.h>
#include <zeplod/event_system.h>
#include <zeplod/example_module_gpio.h>
#include <zeplod/module_manager.h>

#if !DT_NODE_EXISTS(DT_ALIAS(led0))
#error "example_module_gpio requires devicetree alias led0"
#endif

static const struct gpio_dt_spec s_led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#if DT_NODE_EXISTS(DT_ALIAS(sw0))
static const struct gpio_dt_spec s_sw0 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
#endif

LOG_MODULE_REGISTER(example_module_gpio, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部定义
 * ============================================================================= */

#define GPIO_THREAD_PRIORITY   5
#define GPIO_THREAD_STACK_SIZE 2048

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    example_module_gpio_config_t config;
    module_status_t              status;
    struct k_thread              thread;
    K_KERNEL_STACK_MEMBER(stack, GPIO_THREAD_STACK_SIZE);
    const struct gpio_dt_spec* led;
    const struct gpio_dt_spec* button;
    struct gpio_callback       button_cb;
    bool                       led_state;
    bool                       button_pressed;
    uint32_t                   event_count;
} example_module_gpio_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static example_module_gpio_cb_t g_module_gpio;

/* =============================================================================
 * 前向声明
 * ============================================================================= */

static void gpio_thread_func(void* p1, void* p2, void* p3);
static void button_isr(const struct device* dev, struct gpio_callback* cb, uint32_t pins);

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int example_module_gpio_init(void* config) {
    LOG_INF("初始化 GPIO 模块...");

    memset(&g_module_gpio, 0, sizeof(g_module_gpio));

    /* 设置配置 */
    if (config != NULL) {
        g_module_gpio.config = *(example_module_gpio_config_t*) config;
    } else {
        /* 默认配置 */
        g_module_gpio.config.led_pin = "LED0";
        g_module_gpio.config.button_pin = "SW0";
        g_module_gpio.config.blink_interval_ms = 500;
        g_module_gpio.config.enable_button = true;
    }

#if !DT_NODE_EXISTS(DT_ALIAS(sw0))
    g_module_gpio.config.enable_button = false;
#endif

    g_module_gpio.led = &s_led0;
#if DT_NODE_EXISTS(DT_ALIAS(sw0))
    g_module_gpio.button = &s_sw0;
#else
    g_module_gpio.button = NULL;
#endif

    g_module_gpio.status = MODULE_STATUS_INITIALIZED;

    /* 注册事件类型 */
    event_register_type(EVENT_TYPE_GPIO_BUTTON_PRESSED, "gpio_button_pressed");
    event_register_type(EVENT_TYPE_GPIO_BUTTON_RELEASED, "gpio_button_released");
    event_register_type(EVENT_TYPE_GPIO_LED_STATE, "gpio_led_state");

    LOG_INF("GPIO 模块初始化完成");
    return 0;
}

int example_module_gpio_start(void) {
    if (g_module_gpio.status != MODULE_STATUS_INITIALIZED && g_module_gpio.status != MODULE_STATUS_STOPPED) {
        return -1;
    }

    if (!gpio_is_ready_dt(g_module_gpio.led)) {
        LOG_WRN("LED GPIO 未就绪");
    } else {
        gpio_pin_configure_dt(g_module_gpio.led, GPIO_OUTPUT_ACTIVE);
        g_module_gpio.led_state = true;
    }

    if (g_module_gpio.config.enable_button && g_module_gpio.button != NULL) {
        if (!gpio_is_ready_dt(g_module_gpio.button)) {
            LOG_WRN("按键 GPIO 未就绪");
        } else {
            gpio_pin_configure_dt(g_module_gpio.button, GPIO_INPUT | GPIO_PULL_UP);
            gpio_pin_interrupt_configure_dt(g_module_gpio.button, GPIO_INT_EDGE_BOTH);
            gpio_init_callback(&g_module_gpio.button_cb, button_isr, BIT(g_module_gpio.button->pin));
            gpio_add_callback(g_module_gpio.button->port, &g_module_gpio.button_cb);
        }
    }

    g_module_gpio.status = MODULE_STATUS_RUNNING;

    /* 创建线程 */
    k_thread_create(&g_module_gpio.thread, g_module_gpio.stack, K_THREAD_STACK_SIZEOF(g_module_gpio.stack),
                    gpio_thread_func, NULL, NULL, NULL, GPIO_THREAD_PRIORITY, 0, K_FOREVER);

    k_thread_name_set(&g_module_gpio.thread, "mod_gpio");
    k_thread_start(&g_module_gpio.thread);

    LOG_INF("GPIO 模块已启动");
    return 0;
}

int example_module_gpio_stop(void) {
    if (g_module_gpio.status != MODULE_STATUS_RUNNING) {
        return 0;
    }

    /* SIL-2: 先设置状态让线程自行退出，避免 k_thread_abort 强制终止 */
    g_module_gpio.status = MODULE_STATUS_STOPPED;

    /* 等待线程检测到状态变化并自然退出
     * 线程循环中有 k_msleep(50)，等待 150ms 足够
     */
    k_msleep(150);

    LOG_INF("GPIO 模块已停止");
    return 0;
}

int example_module_gpio_shutdown(void) {
    example_module_gpio_stop();
    g_module_gpio.status = MODULE_STATUS_UNINITIALIZED;
    return 0;
}

void example_module_gpio_on_event(const event_t* event, void* user_data) {
    if (event == NULL)
        return;

    switch (event->type) {
    case EVENT_TYPE_GPIO_LED_STATE:
        if (event->data_len >= sizeof(bool)) {
            bool led_state;
            if (event->flags & EVENT_FLAG_DATA_INLINE) {
                memcpy(&led_state, event->data.inline_data, sizeof(bool));
            } else {
                led_state = *(bool*) event->data.ptr;
            }
            example_module_gpio_set_led(led_state);
        }
        break;
    }
}

module_status_t example_module_gpio_get_status(void) {
    return g_module_gpio.status;
}

int example_module_gpio_control(int cmd, void* arg) {
    switch (cmd) {
    case GPIO_CMD_SET_LED:
        if (arg == NULL)
            return -1;
        return example_module_gpio_set_led(*(bool*) arg);

    case GPIO_CMD_TOGGLE_LED:
        example_module_gpio_toggle_led();
        return 0;

    case GPIO_CMD_GET_BUTTON:
        if (arg == NULL)
            return -1;
        *(bool*) arg = example_module_gpio_get_button();
        return 0;

    case GPIO_CMD_SET_BLINK:
        if (arg == NULL)
            return -1;
        g_module_gpio.config.blink_interval_ms = *(uint32_t*) arg;
        return 0;

    default:
        return -1;
    }
}

/* =============================================================================
 * 模块特定 API
 * ============================================================================= */

int example_module_gpio_set_led(bool on) {
    if (g_module_gpio.led == NULL || !gpio_is_ready_dt(g_module_gpio.led)) {
        return -1;
    }

    gpio_pin_set_dt(g_module_gpio.led, on ? 1 : 0);
    g_module_gpio.led_state = on;
    return 0;
}

bool example_module_gpio_toggle_led(void) {
    bool new_state = !g_module_gpio.led_state;
    example_module_gpio_set_led(new_state);
    return new_state;
}

bool example_module_gpio_get_button(void) {
    if (g_module_gpio.button == NULL || !gpio_is_ready_dt(g_module_gpio.button)) {
        return false;
    }

    return gpio_pin_get_dt(g_module_gpio.button) == 0; /* 低电平有效 */
}

int example_module_gpio_set_blink_interval(uint32_t interval_ms) {
    if (interval_ms == 0)
        return -1;
    g_module_gpio.config.blink_interval_ms = interval_ms;
    return 0;
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static void gpio_thread_func(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("GPIO 线程已启动");

    uint32_t last_toggle = k_uptime_get_32();

    while (g_module_gpio.status == MODULE_STATUS_RUNNING) {
        uint32_t now = k_uptime_get_32();

        /* LED 闪烁 */
        if (now - last_toggle >= g_module_gpio.config.blink_interval_ms) {
            example_module_gpio_toggle_led();
            last_toggle = now;
        }

        /* 检测按键 */
        if (g_module_gpio.config.enable_button && g_module_gpio.button != NULL &&
            gpio_is_ready_dt(g_module_gpio.button)) {
            bool pressed = example_module_gpio_get_button();
            if (pressed && !g_module_gpio.button_pressed) {
                /* 按键按下事件 */
                event_publish_copy(EVENT_TYPE_GPIO_BUTTON_PRESSED, EVENT_PRIORITY_NORMAL, NULL, 0);
                g_module_gpio.event_count++;
                LOG_INF("按键按下");
            } else if (!pressed && g_module_gpio.button_pressed) {
                /* 按键释放事件 */
                event_publish_copy(EVENT_TYPE_GPIO_BUTTON_RELEASED, EVENT_PRIORITY_NORMAL, NULL, 0);
                LOG_INF("按键释放");
            }
            g_module_gpio.button_pressed = pressed;
        }

        k_msleep(50);
    }
}

static void button_isr(const struct device* dev, struct gpio_callback* cb, uint32_t pins) {
    /* 占位空实现：当前未做任何处理，按键检测完全由 gpio_thread_func 以 50ms
     * 周期轮询 example_module_gpio_get_button() 完成（见上方线程循环）。 */
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);
}

/* =============================================================================
 * 模块接口声明
 * ============================================================================= */

DECLARE_MODULE_INTERFACE(example_module_gpio);

const module_interface_t* example_module_gpio_get_interface(void) {
    return &example_module_gpio_interface;
}

static int example_module_gpio_auto_register(void) {
    uint32_t                     module_id;
    example_module_gpio_config_t gpio_cfg = {
        .led_pin = "LED0",
        .button_pin = "SW0",
        .blink_interval_ms = 500,
        .enable_button = true,
    };

    if (module_manager_register(example_module_gpio_get_interface(), &gpio_cfg, &module_id) != 0) {
        LOG_ERR("module_manager_register example_module_gpio failed");
        return -EIO;
    }
    LOG_INF("Registered example_module_gpio (id=%u)", module_id);
    return 0;
}

SYS_INIT(example_module_gpio_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_GPIO);
