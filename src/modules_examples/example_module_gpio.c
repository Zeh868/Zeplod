/**
 * @file example_module_gpio.c
 * @brief GPIO 控制示例模块实现
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
 * 2026-09-18       1.1            zeh            审查整改：stop 注销 GPIO 回调并关中断、
 * 2026-09-18       1.2            zeh            复审整改：SET_BLINK 与 set_blink_interval 契约对齐（拒绝 0）
 *                                                 改有界 k_thread_join；status 改 atomic；
 *                                                 补事件/GPIO API 返回值检查与 switch
 *                                                 default；button_isr 演示 ISR 安全发布；
 *                                                 删除不生效的 led_pin/button_pin 配置字段
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

/** stop 时等待 GPIO 线程退出的有界超时；线程循环 k_msleep(50)，正常远小于该值 */
#define GPIO_THREAD_JOIN_TIMEOUT_MS 200

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    example_module_gpio_config_t config;
    /*
     * status 被 GPIO 线程/ISR 与模块管理器线程共同访问：
     * 枚举整数的单写者多读者切换，用 atomic 保证可见性即可，不加互斥锁。
     */
    atomic_t                     status;
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
static int  gpio_button_hw_teardown(void);

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
        g_module_gpio.config.blink_interval_ms = 500;
        g_module_gpio.config.enable_button = true;
    }

#if !DT_NODE_EXISTS(DT_ALIAS(sw0))
    g_module_gpio.config.enable_button = false;
#endif

    /* 引脚由 devicetree alias (led0/sw0) 决定，配置中的引脚名字符串不生效 */
    g_module_gpio.led = &s_led0;
#if DT_NODE_EXISTS(DT_ALIAS(sw0))
    g_module_gpio.button = &s_sw0;
#else
    g_module_gpio.button = NULL;
#endif

    atomic_set(&g_module_gpio.status, MODULE_STATUS_INITIALIZED);

    /* 注册事件类型 */
    if (event_register_type(EVENT_TYPE_GPIO_BUTTON_PRESSED, "gpio_button_pressed") != EVENT_OK) {
        LOG_ERR("event_register_type gpio_button_pressed failed");
        return -EIO;
    }
    if (event_register_type(EVENT_TYPE_GPIO_BUTTON_RELEASED, "gpio_button_released") != EVENT_OK) {
        LOG_ERR("event_register_type gpio_button_released failed");
        return -EIO;
    }
    if (event_register_type(EVENT_TYPE_GPIO_LED_STATE, "gpio_led_state") != EVENT_OK) {
        LOG_ERR("event_register_type gpio_led_state failed");
        return -EIO;
    }

    LOG_INF("GPIO 模块初始化完成");
    return 0;
}

int example_module_gpio_start(void) {
    if (atomic_get(&g_module_gpio.status) != MODULE_STATUS_INITIALIZED &&
        atomic_get(&g_module_gpio.status) != MODULE_STATUS_STOPPED) {
        return -1;
    }

    if (!gpio_is_ready_dt(g_module_gpio.led)) {
        LOG_WRN("LED GPIO 未就绪");
    } else {
        if (gpio_pin_configure_dt(g_module_gpio.led, GPIO_OUTPUT_ACTIVE) != 0) {
            LOG_ERR("LED GPIO configure failed");
            atomic_set(&g_module_gpio.status, MODULE_STATUS_ERROR);
            return -EIO;
        }
        g_module_gpio.led_state = true;
    }

    if (g_module_gpio.config.enable_button && g_module_gpio.button != NULL) {
        if (!gpio_is_ready_dt(g_module_gpio.button)) {
            LOG_WRN("按键 GPIO 未就绪");
        } else {
            int ret = gpio_pin_configure_dt(g_module_gpio.button, GPIO_INPUT | GPIO_PULL_UP);
            if (ret != 0) {
                LOG_ERR("按键 GPIO configure failed: %d", ret);
                atomic_set(&g_module_gpio.status, MODULE_STATUS_ERROR);
                return -EIO;
            }
            ret = gpio_pin_interrupt_configure_dt(g_module_gpio.button, GPIO_INT_EDGE_BOTH);
            if (ret != 0) {
                LOG_ERR("按键中断 configure failed: %d", ret);
                atomic_set(&g_module_gpio.status, MODULE_STATUS_ERROR);
                return -EIO;
            }
            gpio_init_callback(&g_module_gpio.button_cb, button_isr, BIT(g_module_gpio.button->pin));
            ret = gpio_add_callback(g_module_gpio.button->port, &g_module_gpio.button_cb);
            if (ret != 0) {
                LOG_ERR("gpio_add_callback failed: %d", ret);
                atomic_set(&g_module_gpio.status, MODULE_STATUS_ERROR);
                return -EIO;
            }
        }
    }

    atomic_set(&g_module_gpio.status, MODULE_STATUS_RUNNING);

    /* 创建线程 */
    k_thread_create(&g_module_gpio.thread, g_module_gpio.stack, K_THREAD_STACK_SIZEOF(g_module_gpio.stack),
                    gpio_thread_func, NULL, NULL, NULL, GPIO_THREAD_PRIORITY, 0, K_FOREVER);

    k_thread_name_set(&g_module_gpio.thread, "mod_gpio");
    k_thread_start(&g_module_gpio.thread);

    LOG_INF("GPIO 模块已启动");
    return 0;
}

int example_module_gpio_stop(void) {
    if (atomic_get(&g_module_gpio.status) != MODULE_STATUS_RUNNING) {
        return 0;
    }

    /* SIL-2: 先设置状态让线程自行退出，避免 k_thread_abort 强制终止 */
    atomic_set(&g_module_gpio.status, MODULE_STATUS_STOPPED);

    /* 有界汇合 GPIO 线程：线程循环 k_msleep(50)，正常应立即退出；超时说明线程异常滞留 */
    int join_ret = k_thread_join(&g_module_gpio.thread, K_MSEC(GPIO_THREAD_JOIN_TIMEOUT_MS));
    if (join_ret != 0) {
        LOG_ERR("gpio thread join timeout/failed: %d", join_ret);
    }

    /* 注销按键中断与回调，保证 stop→start 重启周期不残留旧回调 */
    (void) gpio_button_hw_teardown();

    LOG_INF("GPIO 模块已停止");
    return 0;
}

int example_module_gpio_shutdown(void) {
    example_module_gpio_stop();
    atomic_set(&g_module_gpio.status, MODULE_STATUS_UNINITIALIZED);
    return 0;
}

void example_module_gpio_on_event(const event_t* event, void* user_data) {
    ARG_UNUSED(user_data);

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

    default:
        LOG_DBG("Unhandled event type: %d", event->type);
        break;
    }
}

module_status_t example_module_gpio_get_status(void) {
    return (module_status_t) atomic_get(&g_module_gpio.status);
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
        /* 与 set_blink_interval 契约一致：0 会使 LED 退化成每循环翻转，拒绝 */
        return example_module_gpio_set_blink_interval(*(uint32_t*) arg);
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
    /* 32 位对齐标量的单写者（调用方）多读者（GPIO 线程）场景，见 control 处说明 */
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

    while (atomic_get(&g_module_gpio.status) == MODULE_STATUS_RUNNING) {
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
                if (event_publish_copy(EVENT_TYPE_GPIO_BUTTON_PRESSED, EVENT_PRIORITY_NORMAL, NULL, 0) != EVENT_OK) {
                    LOG_ERR("publish gpio_button_pressed failed");
                }
                g_module_gpio.event_count++;
                LOG_INF("按键按下");
            } else if (!pressed && g_module_gpio.button_pressed) {
                /* 按键释放事件 */
                if (event_publish_copy(EVENT_TYPE_GPIO_BUTTON_RELEASED, EVENT_PRIORITY_NORMAL, NULL, 0) != EVENT_OK) {
                    LOG_ERR("publish gpio_button_released failed");
                }
                LOG_INF("按键释放");
            }
            g_module_gpio.button_pressed = pressed;
        }

        k_msleep(50);
    }
}

/** ISR 边沿计数：仅 button_isr 单一上下文递增，用于演示 ISR 安全发布 */
static uint8_t g_button_edge_count;

static void button_isr(const struct device* dev, struct gpio_callback* cb, uint32_t pins) {
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    /*
     * 演示 ISR 安全的事件发布：event_publish_from_isr 不加锁、无动态分配，
     * 可在中断上下文调用。事件类型复用本模块已注册的 GPIO_BUTTON_PRESSED，
     * 载荷为 1 字节边沿计数；发布失败（如队列满）在 ISR 内只能静默丢弃，属预期。
     * 按下/释放的语义判定与防抖仍由 gpio_thread_func 轮询完成，此处仅提供
     * 低延迟的边沿通知。
     */
    g_button_edge_count++;

    event_t edge_event = {
        .type = EVENT_TYPE_GPIO_BUTTON_PRESSED,
        .priority = EVENT_PRIORITY_NORMAL,
        .flags = EVENT_FLAG_DATA_INLINE,
        .timestamp = k_uptime_get_32(),
        .data_len = sizeof(g_button_edge_count),
    };
    edge_event.data.inline_data[0] = g_button_edge_count;

    (void) event_publish_from_isr(&edge_event);
}

static int gpio_button_hw_teardown(void) {
    if (!g_module_gpio.config.enable_button || g_module_gpio.button == NULL) {
        return 0;
    }

    int ret = gpio_pin_interrupt_configure_dt(g_module_gpio.button, GPIO_INT_DISABLE);
    if (ret != 0) {
        LOG_ERR("按键中断 disable failed: %d", ret);
    }
    ret = gpio_remove_callback(g_module_gpio.button->port, &g_module_gpio.button_cb);
    if (ret != 0) {
        LOG_ERR("gpio_remove_callback failed: %d", ret);
    }
    return ret;
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
