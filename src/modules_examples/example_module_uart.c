/**
 * @file example_module_uart.c
 * @brief UART 通信示例模块实现
 *
 * 演示如何使用 Zephyr UART API 进行串口通信。
 * @author zeh (china_qzh@163.com)
 * @version 1.3
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            审查整改：删除 ISR 内 TX 完成空转轮询；
 *                                                 init 校验缓冲区配置防越界；stop 改有界
 *                                                 k_thread_join；status 改 atomic；补返回值
 *                                                 检查与 switch default；删除无发布方的
 *                                                 TX_COMPLETE/ERROR 死注册与死字段 baudrate
 * 2026-09-18       1.3            zeh            复审整改：init 事件注册失败路径回退状态；
 *                                                 rx 线程 create 返回值检查
 *
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>
#include <zeplod/app_config.h>
#include <zeplod/event_system.h>
#include <zeplod/example_module_uart.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(example_module_uart, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部定义
 * ============================================================================= */

#define UART_THREAD_PRIORITY   5
#define UART_THREAD_STACK_SIZE 2048

/** rx_thread 单次批量读取并发布的最大字节数（栈上缓冲，须小于环形缓冲容量） */
#define UART_RX_BATCH_SIZE 64

/** stop 时等待 rx_thread 退出的有界超时；线程循环 k_msleep(10)，正常远小于该值 */
#define UART_THREAD_JOIN_TIMEOUT_MS 200

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    example_module_uart_config_t config;
    /*
     * status 被 rx_thread/ISR 与模块管理器线程共同访问：
     * 状态切换仅为枚举整数值的单写者多读者场景，用 atomic 保证可见性即可，
     * 不引入互斥锁（读侧容忍读到上一状态的旧值，最多多执行一轮循环判断）。
     */
    atomic_t                     status;
    struct k_thread              rx_thread;
    K_KERNEL_STACK_MEMBER(rx_stack, UART_THREAD_STACK_SIZE);
    const struct device* dev;
    uint8_t*             rx_buffer;
    uint8_t*             tx_buffer;
    size_t               rx_head;
    size_t               rx_tail;
    size_t               tx_count;
    size_t               rx_count;
    size_t               error_count;
    struct k_spinlock    rx_lock;
    struct k_mutex       tx_lock;
} example_module_uart_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static example_module_uart_cb_t g_module_uart;
static uint8_t                  g_rx_buffer_static[256];
static uint8_t                  g_tx_buffer_static[256];

/* =============================================================================
 * 前向声明
 * ============================================================================= */

static void uart_rx_thread(void* p1, void* p2, void* p3);
static void uart_irq_callback(const struct device* dev, void* user_data);

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int example_module_uart_init(void* config) {
    LOG_INF("初始化 UART 模块...");

    memset(&g_module_uart, 0, sizeof(g_module_uart));

    /* 设置配置 */
    if (config != NULL) {
        g_module_uart.config = *(example_module_uart_config_t*) config;
    } else {
        g_module_uart.config.device_name = "UART_0";
        g_module_uart.config.rx_buffer_size = sizeof(g_rx_buffer_static);
        g_module_uart.config.tx_buffer_size = sizeof(g_tx_buffer_static);
        g_module_uart.config.enable_interrupt = true;
    }

    /* 缓冲区配置须落在内部静态缓冲范围内，否则取模运算会越界（外部输入先校验再使用） */
    if (g_module_uart.config.rx_buffer_size == 0U || g_module_uart.config.rx_buffer_size > sizeof(g_rx_buffer_static) ||
        g_module_uart.config.tx_buffer_size == 0U || g_module_uart.config.tx_buffer_size > sizeof(g_tx_buffer_static)) {
        LOG_ERR("Invalid buffer config: rx=%u tx=%u (1..%u)",
                (unsigned int) g_module_uart.config.rx_buffer_size, (unsigned int) g_module_uart.config.tx_buffer_size,
                (unsigned int) sizeof(g_rx_buffer_static));
        return -EINVAL;
    }

    g_module_uart.rx_buffer = g_rx_buffer_static;
    g_module_uart.tx_buffer = g_tx_buffer_static;
    atomic_set(&g_module_uart.status, MODULE_STATUS_INITIALIZED);

    k_mutex_init(&g_module_uart.tx_lock);

    /* 注册事件类型 */
    if (event_register_type(EVENT_TYPE_UART_RX_DATA, "uart_rx_data") != EVENT_OK) {
        LOG_ERR("event_register_type uart_rx_data failed");
        atomic_set(&g_module_uart.status, MODULE_STATUS_UNINITIALIZED);
        return -EIO;
    }

    LOG_INF("UART 模块初始化完成");
    return 0;
}

int example_module_uart_start(void) {
    if (atomic_get(&g_module_uart.status) != MODULE_STATUS_INITIALIZED &&
        atomic_get(&g_module_uart.status) != MODULE_STATUS_STOPPED) {
        return -1;
    }

    /* 获取 UART 设备 */
#if IS_ENABLED(CONFIG_EXAMPLE_MODULE_UART_USE_ZEPHYR_CONSOLE)
#if DT_HAS_CHOSEN(zephyr_console)
    g_module_uart.dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
#else
    LOG_ERR("devicetree 未选择 zephyr_console");
    atomic_set(&g_module_uart.status, MODULE_STATUS_ERROR);
    return -1;
#endif
#else
    g_module_uart.dev = device_get_binding(CONFIG_EXAMPLE_MODULE_UART_DEVICE_NAME);
#endif
    if (g_module_uart.dev == NULL) {
        LOG_ERR("找不到 UART 设备");
        atomic_set(&g_module_uart.status, MODULE_STATUS_ERROR);
        return -1;
    }

    if (!device_is_ready(g_module_uart.dev)) {
        LOG_ERR("UART 设备未就绪");
        atomic_set(&g_module_uart.status, MODULE_STATUS_ERROR);
        return -1;
    }

    /* 若绑定的是 Zephyr console UART，不要抢占其 IRQ 回调，否则会导致 Shell/LOG 无输出。 */
#if IS_ENABLED(CONFIG_EXAMPLE_MODULE_UART_USE_ZEPHYR_CONSOLE)
    if (g_module_uart.config.enable_interrupt) {
        LOG_WRN("UART module shares zephyr_console; disable IRQ hook to keep Shell/LOG output");
        g_module_uart.config.enable_interrupt = false;
    }
#endif

    /* 配置中断接收（仅非 console 共享场景） */
    if (g_module_uart.config.enable_interrupt &&
        uart_irq_callback_user_data_set(g_module_uart.dev, uart_irq_callback, &g_module_uart) == 0) {
        uart_irq_rx_enable(g_module_uart.dev);
    }

    atomic_set(&g_module_uart.status, MODULE_STATUS_RUNNING);

    /* 创建接收线程 */
    k_tid_t rx_tid =
        k_thread_create(&g_module_uart.rx_thread, g_module_uart.rx_stack, K_THREAD_STACK_SIZEOF(g_module_uart.rx_stack),
                        uart_rx_thread, NULL, NULL, NULL, UART_THREAD_PRIORITY, 0, K_FOREVER);
    if (rx_tid == NULL) {
        LOG_ERR("Failed to create UART rx thread");
        atomic_set(&g_module_uart.status, MODULE_STATUS_ERROR);
        return -ENOMEM;
    }

    k_thread_name_set(rx_tid, "mod_uart_rx");
    k_thread_start(rx_tid);

    /* 波特率由 devicetree/驱动默认配置决定，本模块不做 uart_configure */
    LOG_INF("UART 模块已启动：%s", g_module_uart.dev->name);
    return 0;
}

int example_module_uart_stop(void) {
    if (atomic_get(&g_module_uart.status) != MODULE_STATUS_RUNNING) {
        return 0;
    }

    if (g_module_uart.config.enable_interrupt && g_module_uart.dev != NULL) {
        uart_irq_rx_disable(g_module_uart.dev);
    }

    /* SIL-2: 先设置状态让线程自行退出，避免 k_thread_abort 强制终止 */
    atomic_set(&g_module_uart.status, MODULE_STATUS_STOPPED);

    /* 有界汇合接收线程：线程循环 k_msleep(10)，正常应立即退出；超时说明线程异常滞留 */
    int join_ret = k_thread_join(&g_module_uart.rx_thread, K_MSEC(UART_THREAD_JOIN_TIMEOUT_MS));
    if (join_ret != 0) {
        LOG_ERR("uart rx_thread join timeout/failed: %d", join_ret);
    }

    LOG_INF("UART 模块已停止");
    return 0;
}

int example_module_uart_shutdown(void) {
    example_module_uart_stop();
    atomic_set(&g_module_uart.status, MODULE_STATUS_UNINITIALIZED);
    return 0;
}

void example_module_uart_on_event(const event_t* event, void* user_data) {
    ARG_UNUSED(user_data);

    if (event == NULL)
        return;

    switch (event->type) {
    case EVENT_TYPE_UART_RX_DATA:
        /* RX 数据由 rx_thread 直接发布给订阅者，本模块自身无需处理 */
        break;

    default:
        LOG_DBG("Unhandled event type: %d", event->type);
        break;
    }
}

module_status_t example_module_uart_get_status(void) {
    return (module_status_t) atomic_get(&g_module_uart.status);
}

int example_module_uart_control(int cmd, void* arg) {
    switch (cmd) {
    case UART_CMD_SEND:
        if (arg == NULL)
            return -1;
        {
            const example_module_uart_tx_req_t* tx_req = (const example_module_uart_tx_req_t*) arg;
            int                                 sent = example_module_uart_send(tx_req->data, tx_req->len);
            return (sent >= 0) ? 0 : sent;
        }

    case UART_CMD_GET_RX_COUNT:
        if (arg == NULL)
            return -1;
        *(size_t*) arg = example_module_uart_get_rx_count();
        return 0;

    case UART_CMD_CLEAR_RX:
        example_module_uart_clear_rx_buffer();
        return 0;

    case UART_CMD_GET_STATS:
        if (arg == NULL)
            return -1;
        {
            uint32_t* stats = (uint32_t*) arg;
            example_module_uart_get_stats(&stats[0], &stats[1], &stats[2]);
        }
        return 0;

    default:
        return -1;
    }
}

/* =============================================================================
 * 模块特定 API
 * ============================================================================= */

int example_module_uart_send(const void* data, size_t len) {
    if (data == NULL || len == 0 || g_module_uart.dev == NULL) {
        return -1;
    }

    k_mutex_lock(&g_module_uart.tx_lock, K_FOREVER);

    const uint8_t* tx_data = (const uint8_t*) data;
    size_t         sent = 0;

    for (size_t i = 0; i < len; i++) {
        uart_poll_out(g_module_uart.dev, tx_data[i]);
        sent++;
    }

    g_module_uart.tx_count += sent;
    k_mutex_unlock(&g_module_uart.tx_lock);

    return (int) sent;
}

int example_module_uart_send_string(const char* str) {
    if (str == NULL)
        return -1;
    return example_module_uart_send(str, strlen(str));
}

int example_module_uart_receive(void* data, size_t len) {
    if (data == NULL || len == 0)
        return -1;

    k_spinlock_key_t key = k_spin_lock(&g_module_uart.rx_lock);
    size_t           available = g_module_uart.rx_head - g_module_uart.rx_tail;
    size_t           capacity = g_module_uart.config.rx_buffer_size;

    /* 与 ISR 侧写入路径对称的零容量防护：init 已校验，出现即异常配置 */
    if (capacity == 0U) {
        k_spin_unlock(&g_module_uart.rx_lock, key);
        return 0;
    }

    if (available > capacity) {
        /* 防御性修正：理论上不会出现，出现时丢弃最旧数据保证边界安全 */
        g_module_uart.rx_tail = g_module_uart.rx_head - capacity;
        available = capacity;
    }
    size_t to_copy = MIN(len, available);

    uint8_t* rx_data = (uint8_t*) data;
    for (size_t i = 0; i < to_copy; i++) {
        rx_data[i] = g_module_uart.rx_buffer[(g_module_uart.rx_tail + i) % g_module_uart.config.rx_buffer_size];
    }

    g_module_uart.rx_tail += to_copy;
    g_module_uart.rx_count += to_copy;

    k_spin_unlock(&g_module_uart.rx_lock, key);
    return (int) to_copy;
}

size_t example_module_uart_get_rx_count(void) {
    k_spinlock_key_t key = k_spin_lock(&g_module_uart.rx_lock);
    size_t           count = g_module_uart.rx_head - g_module_uart.rx_tail;
    if (count > g_module_uart.config.rx_buffer_size) {
        count = g_module_uart.config.rx_buffer_size;
    }
    k_spin_unlock(&g_module_uart.rx_lock, key);
    return count;
}

void example_module_uart_clear_rx_buffer(void) {
    k_spinlock_key_t key = k_spin_lock(&g_module_uart.rx_lock);
    g_module_uart.rx_head = 0;
    g_module_uart.rx_tail = 0;
    k_spin_unlock(&g_module_uart.rx_lock, key);
}

void example_module_uart_get_stats(uint32_t* tx_count, uint32_t* rx_count, uint32_t* errors) {
    /* 统计仅作观测用途：tx_count/error_count 由 ISR 与线程无锁递增，此处读取
     * 不加锁，可能读到略微滞后的瞬时值；对诊断语义可接受，不值得引入临界区。 */
    if (tx_count != NULL)
        *tx_count = g_module_uart.tx_count;
    if (rx_count != NULL)
        *rx_count = g_module_uart.rx_count;
    if (errors != NULL)
        *errors = g_module_uart.error_count;
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static void uart_rx_thread(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("UART 接收线程已启动");

    while (atomic_get(&g_module_uart.status) == MODULE_STATUS_RUNNING) {
        /* 检查接收缓冲区 */
        size_t count = example_module_uart_get_rx_count();

        if (count > 0) {
            /* 读取数据并发布事件 */
            uint8_t buffer[UART_RX_BATCH_SIZE];
            int     received = example_module_uart_receive(buffer, MIN(count, sizeof(buffer)));

            if (received > 0) {
                if (event_publish_copy(EVENT_TYPE_UART_RX_DATA, EVENT_PRIORITY_NORMAL, buffer, (size_t) received) !=
                    EVENT_OK) {
                    LOG_ERR("publish uart_rx_data failed (len=%d)", received);
                }
                LOG_DBG("UART 接收 %d 字节", received);
            }
        }

        k_msleep(10);
    }
}

static void uart_irq_callback(const struct device* dev, void* user_data) {
    ARG_UNUSED(dev);
    example_module_uart_cb_t* cb = (example_module_uart_cb_t*) user_data;

    if (uart_irq_update(cb->dev)) {
        while (uart_irq_rx_ready(cb->dev)) {
            uint8_t byte;
            if (uart_fifo_read(cb->dev, &byte, 1) > 0) {
                /* 存入环形缓冲区 */
                k_spinlock_key_t key = k_spin_lock(&cb->rx_lock);
                size_t           cap = cb->config.rx_buffer_size;
                if (cap > 0U) {
                    cb->rx_buffer[cb->rx_head % cap] = byte;
                    cb->rx_head++;
                    if ((cb->rx_head - cb->rx_tail) > cap) {
                        /* 缓冲区满时覆盖最旧字节，避免计数无限增长后越界语义 */
                        cb->rx_tail = cb->rx_head - cap;
                    }
                }
                k_spin_unlock(&cb->rx_lock, key);
            }
        }

        /* 发送走 uart_poll_out（线程侧阻塞式逐字节发送），TX 完成中断不使能，
         * 无需在此轮询等待 TX 完成；误留轮询会在 TX 中断未使能时死循环。 */

        if (uart_err_check(cb->dev) != 0) {
            /* 统计级语义：仅观测用，ISR 内无锁递增，读侧容忍瞬时旧值 */
            cb->error_count++;
        }
    }
}

/* =============================================================================
 * 模块接口声明
 * ============================================================================= */

DECLARE_MODULE_INTERFACE(example_module_uart);

const module_interface_t* example_module_uart_get_interface(void) {
    return &example_module_uart_interface;
}

static int example_module_uart_auto_register(void) {
    uint32_t                     module_id;
    example_module_uart_config_t uart_cfg = {
        .device_name = CONFIG_EXAMPLE_MODULE_UART_DEVICE_NAME,
        .rx_buffer_size = sizeof(g_rx_buffer_static),
        .tx_buffer_size = sizeof(g_tx_buffer_static),
        .enable_interrupt = true,
    };

    if (module_manager_register(example_module_uart_get_interface(), &uart_cfg, &module_id) != 0) {
        LOG_ERR("module_manager_register example_module_uart failed");
        return -EIO;
    }
    LOG_INF("Registered example_module_uart (id=%u)", module_id);
    return 0;
}

SYS_INIT(example_module_uart_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_UART);
