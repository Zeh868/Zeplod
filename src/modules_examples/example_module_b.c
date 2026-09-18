/**
 * @file example_module_b.c
 * @brief 示例模块 B 实现
 * @author zeh (china_qzh@163.com)
 * @version 1.3
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            审查整改：init 补全零初始化；stop 改有界
 * 2026-09-18       1.2            zeh            复审整改：start 先置 RUNNING 再以 K_FOREVER+k_thread_start 启动线程，消除启动竞态
 * 2026-09-18       1.3            zeh            复审整改：删除死配置字段，init 忽略 config、注册传 NULL
 *                                                 k_thread_join；status 改 atomic；补订阅
 *                                                 返回值检查；事件负载解引用改 memcpy 保对齐
 *
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
#include <zeplod/app_config.h>
#include <zeplod/event_system.h>
#include <zeplod/example_module_b.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(example_module_b, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部定义
 * ============================================================================= */

#define EXAMPLE_MODULE_B_THREAD_PRIORITY   6
#define EXAMPLE_MODULE_B_THREAD_STACK_SIZE 2048

/** stop 时等待模块线程退出的有界超时；线程循环 k_msleep(50)，正常远小于该值 */
#define EXAMPLE_MODULE_B_THREAD_JOIN_TIMEOUT_MS 200

/* 控制命令 */
#define CMD_SEND_DATA                      1
#define CMD_GET_STATS                      2
#define CMD_CLEAR_BUFFERS                  3

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    /*
     * status 被模块线程与模块管理器线程共同访问：
     * 枚举整数的单写者多读者切换，用 atomic 保证可见性即可，不加互斥锁。
     */
    atomic_t                  status;
    struct k_thread           thread;
    K_KERNEL_STACK_MEMBER(stack, EXAMPLE_MODULE_B_THREAD_STACK_SIZE);
    uint32_t subscriber_id;
    uint32_t event_type;
} example_module_b_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static example_module_b_cb_t g_module_b;

/* =============================================================================
 * 前置声明
 * ============================================================================= */

static void module_b_thread_func(void* p1, void* p2, void* p3);
static void on_sensor_data(const event_t* event, void* user_data);

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int example_module_b_init(void* config) {
    LOG_INF("Initializing Example Module B...");

    memset(&g_module_b, 0, sizeof(g_module_b));
    /* 模块无运行参数：config 忽略（模块接口约定 init 形参保留） */
    ARG_UNUSED(config);

    atomic_set(&g_module_b.status, MODULE_STATUS_INITIALIZED);
    g_module_b.subscriber_id = 0;
    g_module_b.event_type = EVENT_TYPE_SENSOR_DATA;

    LOG_INF("Module B initialized");
    return 0;
}

int example_module_b_start(void) {
    LOG_INF("Starting Module B...");

    if (atomic_get(&g_module_b.status) != MODULE_STATUS_INITIALIZED &&
        atomic_get(&g_module_b.status) != MODULE_STATUS_STOPPED) {
        LOG_ERR("Module not initialized");
        return -1;
    }

    /* 订阅来自模块 A 的传感器数据事件 */
    g_module_b.event_type = EVENT_TYPE_SENSOR_DATA;
    if (event_subscribe(g_module_b.event_type, on_sensor_data, &g_module_b, &g_module_b.subscriber_id) != EVENT_OK) {
        LOG_ERR("event_subscribe sensor_data failed");
        atomic_set(&g_module_b.status, MODULE_STATUS_ERROR);
        return -EIO;
    }

    /* 先置 RUNNING 再创建线程（K_FOREVER + k_thread_start）：避免工作线程立即抢占
     * 读到旧状态而直接退出（对齐 example_module_a 的安全模式） */
    atomic_set(&g_module_b.status, MODULE_STATUS_RUNNING);

    k_tid_t tid = k_thread_create(&g_module_b.thread, g_module_b.stack, K_THREAD_STACK_SIZEOF(g_module_b.stack),
                                  module_b_thread_func, &g_module_b, NULL, NULL, EXAMPLE_MODULE_B_THREAD_PRIORITY, 0,
                                  K_FOREVER);
    if (tid == NULL) {
        LOG_ERR("Failed to create module B thread");
        atomic_set(&g_module_b.status, MODULE_STATUS_ERROR);
        return -ENOMEM;
    }
    k_thread_name_set(tid, "mod_b_receiver");
    k_thread_start(tid);

    LOG_INF("Module B started");
    return 0;
}

int example_module_b_stop(void) {
    LOG_INF("Stopping Module B...");

    if (atomic_get(&g_module_b.status) != MODULE_STATUS_RUNNING) {
        return 0;
    }

    /* SIL-2: 先设置状态让线程自行退出，避免 k_thread_abort 强制终止 */
    atomic_set(&g_module_b.status, MODULE_STATUS_STOPPED);

    /* 有界汇合模块线程：线程循环 k_msleep(50)，正常应立即退出；超时说明线程异常滞留 */
    int join_ret = k_thread_join(&g_module_b.thread, K_MSEC(EXAMPLE_MODULE_B_THREAD_JOIN_TIMEOUT_MS));
    if (join_ret != 0) {
        LOG_ERR("module b thread join timeout/failed: %d", join_ret);
    }

    /* 取消事件订阅 */
    if (g_module_b.subscriber_id != 0) {
        event_unsubscribe(g_module_b.event_type, g_module_b.subscriber_id);
        g_module_b.subscriber_id = 0;
    }

    LOG_INF("Module B stopped");
    return 0;
}

int example_module_b_shutdown(void) {
    LOG_INF("Shutting down Module B...");

    example_module_b_stop();
    atomic_set(&g_module_b.status, MODULE_STATUS_UNINITIALIZED);

    LOG_INF("Module B shutdown complete");
    return 0;
}

module_status_t example_module_b_get_status(void) {
    return (module_status_t) atomic_get(&g_module_b.status);
}

void example_module_b_on_event(const event_t* event, void* user_data) {
    ARG_UNUSED(user_data);

    if (event == NULL) {
        return;
    }

    /* 通用处理所有事件 */
    LOG_DBG("Received event type: %d", event->type);

    if (event->data_len >= sizeof(int32_t)) {
        int32_t sensor_value;
        if (event->flags & EVENT_FLAG_DATA_INLINE) {
            memcpy(&sensor_value, event->data.inline_data, sizeof(int32_t));
        } else {
            /* 外部负载对齐未知，memcpy 到本地变量而非直接解引用指针 */
            memcpy(&sensor_value, event->data.ptr, sizeof(int32_t));
        }
        LOG_DBG("Event data: %d", sensor_value);
    }
}

int example_module_b_control(int cmd, void* arg) {
    switch (cmd) {
    case CMD_GET_STATS:
        /* 可返回统计信息 */
        return 0;

    case CMD_CLEAR_BUFFERS:
        /* 可清空缓冲区 */
        return 0;

    default:
        return -1;
    }
}

/* =============================================================================
 * 模块专用 API
 * ============================================================================= */

int example_module_b_send(const void* data, size_t len) {
    if (data == NULL || len == 0) {
        return -1;
    }

    /* 模拟发送数据 */
    LOG_DBG("Sending %d bytes", len);
    return (int) len;
}

int example_module_b_receive(void* data, size_t len) {
    if (data == NULL || len == 0) {
        return -1;
    }

    /* 模拟接收数据 */
    memset(data, 0, len);
    return 0;
}

void example_module_b_get_stats(uint32_t* tx_count, uint32_t* rx_count, uint32_t* errors) {
    if (tx_count != NULL)
        *tx_count = 0;
    if (rx_count != NULL)
        *rx_count = 0;
    if (errors != NULL)
        *errors = 0;
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static void module_b_thread_func(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("Module B thread started");

    while (atomic_get(&g_module_b.status) == MODULE_STATUS_RUNNING) {
        /* 模拟通信处理 */
        k_msleep(50);
    }

    LOG_INF("Module B thread stopped");
}

static void on_sensor_data(const event_t* event, void* user_data) {
    ARG_UNUSED(user_data);

    if (event == NULL) {
        return;
    }

    LOG_DBG("Received sensor data");

    /* 处理传感器数据并可能发送响应 */
    if (event->data_len >= sizeof(int32_t)) {
        int32_t sensor_value;
        if (event->flags & EVENT_FLAG_DATA_INLINE) {
            memcpy(&sensor_value, event->data.inline_data, sizeof(int32_t));
        } else {
            /* 外部负载对齐未知，memcpy 到本地变量而非直接解引用指针 */
            memcpy(&sensor_value, event->data.ptr, sizeof(int32_t));
        }

        /* 示例：发送确认 */
        uint32_t ack = sensor_value * 2; /* Simple transformation */
        example_module_b_send(&ack, sizeof(ack));

        LOG_DBG("Processed sensor value %d", sensor_value);
    }
}

/* =============================================================================
 * 模块接口声明
 * ============================================================================= */

const module_interface_t example_module_b_interface = {.name = "example_module_b",
                                                       .version = MODULE_VERSION(1, 0, 0),
                                                       .priority = MODULE_PRIORITY_NORMAL,
                                                       .depends_on = NULL,
                                                       .init = example_module_b_init,
                                                       .start = example_module_b_start,
                                                       .stop = example_module_b_stop,
                                                       .shutdown = example_module_b_shutdown,
                                                       .on_event = example_module_b_on_event,
                                                       .get_status = example_module_b_get_status,
                                                       .control = example_module_b_control};

const module_interface_t* example_module_b_get_interface(void) {
    return &example_module_b_interface;
}

#if APP_CONFIG_ENABLE_MODULE_B
static int example_module_b_auto_register(void) {
    uint32_t module_id;

    if (module_manager_register(example_module_b_get_interface(), NULL, &module_id) != 0) {
        LOG_ERR("module_manager_register example_module_b failed");
        return -EIO;
    }
    LOG_INF("Registered Module B (id=%u)", module_id);
    return 0;
}

SYS_INIT(example_module_b_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_B);
#endif
