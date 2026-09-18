/**
 * @file example_module_a.c
 * @brief 示例模块 A 实现
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            审查整改：stop 改有界 k_thread_join；
 *                                                 status 改 atomic；补事件 API 返回值检查；
 *                                                 手搓 event_t 发布改 event_publish_copy；
 *                                                 删除无读取方的 buffer_size 配置字段
 *
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
#include <zeplod/app_config.h>
#include <zeplod/event_system.h>
#include <zeplod/example_module_a.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(example_module_a, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部定义
 * ============================================================================= */

#define EXAMPLE_MODULE_A_BUFFER_SIZE       256
#define EXAMPLE_MODULE_A_THREAD_PRIORITY   5
#define EXAMPLE_MODULE_A_THREAD_STACK_SIZE 2048

/** 模拟传感器读数的递增步进（演示值） */
#define EXAMPLE_MODULE_A_SAMPLE_STEP 100

/** stop 时等待采集线程退出的有界超时；线程循环 k_msleep(10)，正常远小于该值 */
#define EXAMPLE_MODULE_A_THREAD_JOIN_TIMEOUT_MS 200

/* 控制命令 */
#define CMD_SET_RATE                       1
#define CMD_GET_RATE                       2
#define CMD_RESET                          3

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    int32_t  value;
    uint32_t timestamp;
    uint8_t  quality;
} sensor_sample_t;

typedef struct {
    example_module_a_config_t config;
    /*
     * status 被采集线程与模块管理器线程共同访问：
     * 枚举整数的单写者多读者切换，用 atomic 保证可见性即可；
     * 读侧容忍读到上一状态旧值，不加互斥锁。
     */
    atomic_t                  status;
    struct k_thread           thread;
    K_KERNEL_STACK_MEMBER(stack, EXAMPLE_MODULE_A_THREAD_STACK_SIZE);
    struct k_mutex  buffer_lock;
    sensor_sample_t buffer[EXAMPLE_MODULE_A_BUFFER_SIZE];
    uint32_t        write_idx;
    uint32_t        read_idx;
    uint32_t        sample_count;
    uint32_t        error_count;
    bool            data_ready;
} example_module_a_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static example_module_a_cb_t g_module_a;
static uint32_t              g_subscriber_id = 0;

/* =============================================================================
 * 前置声明
 * ============================================================================= */

static void module_a_thread_func(void* p1, void* p2, void* p3);
static void publish_sensor_data(int32_t value);

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int example_module_a_init(void* config) {
    LOG_INF("Initializing Example Module A...");

    memset(&g_module_a, 0, sizeof(g_module_a));

    /* 设置默认或提供的配置 */
    if (config != NULL) {
        g_module_a.config = *(example_module_a_config_t*) config;
    } else {
        g_module_a.config.sample_rate_ms = 100;
        g_module_a.config.enable_filtering = true;
    }

    atomic_set(&g_module_a.status, MODULE_STATUS_INITIALIZED);
    g_module_a.write_idx = 0;
    g_module_a.read_idx = 0;

    k_mutex_init(&g_module_a.buffer_lock);

    /* 注册事件类型 */
    if (event_register_type(EVENT_TYPE_SENSOR_DATA, "sensor_data") != EVENT_OK) {
        LOG_ERR("event_register_type sensor_data failed");
        return -EIO;
    }
    if (event_register_type(EVENT_TYPE_SENSOR_CONFIG, "sensor_config") != EVENT_OK) {
        LOG_ERR("event_register_type sensor_config failed");
        return -EIO;
    }

    LOG_INF("Example Module A initialized: sample_rate=%dms", g_module_a.config.sample_rate_ms);
    return 0;
}

int example_module_a_start(void) {
    if (atomic_get(&g_module_a.status) != MODULE_STATUS_INITIALIZED &&
        atomic_get(&g_module_a.status) != MODULE_STATUS_STOPPED) {
        return -1;
    }

    atomic_set(&g_module_a.status, MODULE_STATUS_RUNNING);

    /* 创建数据采集线程 */
    k_thread_create(&g_module_a.thread, g_module_a.stack, K_THREAD_STACK_SIZEOF(g_module_a.stack), module_a_thread_func,
                    NULL, NULL, NULL, EXAMPLE_MODULE_A_THREAD_PRIORITY, 0, K_FOREVER);

    k_thread_name_set(&g_module_a.thread, "mod_a_sensor");
    k_thread_start(&g_module_a.thread);

    /* 订阅配置事件 */
    if (event_subscribe(EVENT_TYPE_SENSOR_CONFIG, example_module_a_on_event, &g_module_a, &g_subscriber_id) !=
        EVENT_OK) {
        LOG_ERR("event_subscribe sensor_config failed");
        atomic_set(&g_module_a.status, MODULE_STATUS_ERROR);
        return -EIO;
    }

    LOG_INF("Example Module A started");
    return 0;
}

int example_module_a_stop(void) {
    if (atomic_get(&g_module_a.status) != MODULE_STATUS_RUNNING) {
        return 0;
    }

    /* 先设置状态，让线程看到并退出循环 */
    atomic_set(&g_module_a.status, MODULE_STATUS_STOPPED);

    /* 有界汇合采集线程：线程循环 k_msleep(10)，正常应立即退出；超时说明线程异常滞留 */
    int join_ret = k_thread_join(&g_module_a.thread, K_MSEC(EXAMPLE_MODULE_A_THREAD_JOIN_TIMEOUT_MS));
    if (join_ret != 0) {
        LOG_ERR("module a thread join timeout/failed: %d", join_ret);
    }

    /* 取消事件订阅 */
    if (g_subscriber_id != 0) {
        event_unsubscribe(EVENT_TYPE_SENSOR_CONFIG, g_subscriber_id);
        g_subscriber_id = 0;
    }

    LOG_INF("Example Module A stopped");
    return 0;
}

int example_module_a_shutdown(void) {
    example_module_a_stop();
    atomic_set(&g_module_a.status, MODULE_STATUS_UNINITIALIZED);
    LOG_INF("Example Module A shutdown");
    return 0;
}

void example_module_a_on_event(const event_t* event, void* user_data) {
    if (event == NULL || user_data == NULL) {
        return;
    }

    example_module_a_cb_t* cb = (example_module_a_cb_t*) user_data;

    LOG_DBG("Module A received event: type=%d", event->type);

    switch (event->type) {
    case EVENT_TYPE_SENSOR_CONFIG:
        /* 处理配置变更 */
        if (event->data_len >= sizeof(uint32_t)) {
            uint32_t new_rate;
            if (event->flags & EVENT_FLAG_DATA_INLINE) {
                memcpy(&new_rate, event->data.inline_data, sizeof(uint32_t));
            } else {
                new_rate = *(uint32_t*) event->data.ptr;
            }
            cb->config.sample_rate_ms = new_rate;
            LOG_INF("Sensor rate updated to %dms", new_rate);
        }
        break;

    default:
        LOG_DBG("Unhandled event type: %d", event->type);
        break;
    }
}

module_status_t example_module_a_get_status(void) {
    return (module_status_t) atomic_get(&g_module_a.status);
}

int example_module_a_control(int cmd, void* arg) {
    switch (cmd) {
    case CMD_SET_RATE:
        if (arg == NULL)
            return -1;
        /* sample_rate_ms 为 32 位对齐标量，跨线程仅此处（control 调用方）与
         * on_event（分发器线程）两个写入方，采集线程只读；
         * 单核/强序 MCU 上对齐 32 位读写本身原子，容忍读者最多晚一轮看到新值。 */
        g_module_a.config.sample_rate_ms = *(uint32_t*) arg;
        return 0;

    case CMD_GET_RATE:
        if (arg == NULL)
            return -1;
        *(uint32_t*) arg = g_module_a.config.sample_rate_ms;
        return 0;

    case CMD_RESET:
        k_mutex_lock(&g_module_a.buffer_lock, K_FOREVER);
        g_module_a.write_idx = 0;
        g_module_a.read_idx = 0;
        g_module_a.sample_count = 0;
        g_module_a.error_count = 0;
        k_mutex_unlock(&g_module_a.buffer_lock);
        return 0;

    default:
        return -1;
    }
}

/* =============================================================================
 * 模块专用 API
 * ============================================================================= */

int example_module_a_get_data(void* data, size_t len) {
    if (data == NULL || len == 0) {
        return -1;
    }

    size_t samples_to_read = len / sizeof(sensor_sample_t);
    size_t samples_read = 0;

    k_mutex_lock(&g_module_a.buffer_lock, K_FOREVER);

    while (samples_read < samples_to_read && g_module_a.read_idx != g_module_a.write_idx) {
        ((sensor_sample_t*) data)[samples_read] = g_module_a.buffer[g_module_a.read_idx];

        g_module_a.read_idx = (g_module_a.read_idx + 1) % EXAMPLE_MODULE_A_BUFFER_SIZE;
        samples_read++;
    }

    k_mutex_unlock(&g_module_a.buffer_lock);
    return (int) (samples_read * sizeof(sensor_sample_t));
}

int example_module_a_set_rate(uint32_t rate_ms) {
    if (rate_ms == 0) {
        return -1;
    }

    g_module_a.config.sample_rate_ms = rate_ms;
    LOG_INF("Sample rate set to %dms", rate_ms);
    return 0;
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static void module_a_thread_func(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("Module A thread started");

    uint32_t last_sample_time = k_uptime_get_32();

    while (atomic_get(&g_module_a.status) == MODULE_STATUS_RUNNING) {
        uint32_t now = k_uptime_get_32();
        uint32_t elapsed = now - last_sample_time;

        if (elapsed >= g_module_a.config.sample_rate_ms) {
            /* 模拟传感器读取 */
            static int32_t simulated_value = 0;
            simulated_value += EXAMPLE_MODULE_A_SAMPLE_STEP; /* 递增值用于演示 */

            sensor_sample_t sample = {
                .value = simulated_value, .timestamp = now, .quality = 100 /* 100% 质量 */
            };

            /* 存入缓冲区 */
            k_mutex_lock(&g_module_a.buffer_lock, K_FOREVER);
            g_module_a.buffer[g_module_a.write_idx] = sample;
            g_module_a.write_idx = (g_module_a.write_idx + 1) % EXAMPLE_MODULE_A_BUFFER_SIZE;
            g_module_a.sample_count++;
            k_mutex_unlock(&g_module_a.buffer_lock);

            /* 发布事件 */
            publish_sensor_data(simulated_value);

            last_sample_time = now;
            g_module_a.data_ready = true;

            LOG_DBG("Sample acquired: value=%d, count=%d", simulated_value, g_module_a.sample_count);
        }

        /* 短暂睡眠 */
        k_msleep(10);
    }

    LOG_INF("Module A thread stopped");
}

static void publish_sensor_data(int32_t value) {
    /* 用框架提供的 copy 发布替代手搓 event_t：避免 flags/data_len 不一致被
     * event_publish 拒绝，时间戳/来源由框架统一填充 */
    if (event_publish_copy(EVENT_TYPE_SENSOR_DATA, EVENT_PRIORITY_NORMAL, &value, sizeof(value)) != EVENT_OK) {
        LOG_ERR("publish sensor_data failed (value=%d)", value);
    }
}

/* =============================================================================
 * 模块接口声明
 * ============================================================================= */

const module_interface_t example_module_a_interface = {.name = "example_module_a",
                                                       .version = MODULE_VERSION(1, 0, 0),
                                                       .priority = MODULE_PRIORITY_NORMAL,
                                                       .depends_on = NULL,
                                                       .init = example_module_a_init,
                                                       .start = example_module_a_start,
                                                       .stop = example_module_a_stop,
                                                       .shutdown = example_module_a_shutdown,
                                                       .on_event = example_module_a_on_event,
                                                       .get_status = example_module_a_get_status,
                                                       .control = example_module_a_control};

const module_interface_t* example_module_a_get_interface(void) {
    return &example_module_a_interface;
}

#if APP_CONFIG_ENABLE_MODULE_A
static int example_module_a_auto_register(void) {
    uint32_t                  module_id;
    example_module_a_config_t config_a = {.sample_rate_ms = 100, .enable_filtering = true};

    if (module_manager_register(example_module_a_get_interface(), &config_a, &module_id) != 0) {
        LOG_ERR("module_manager_register example_module_a failed");
        return -EIO;
    }
    LOG_INF("Registered Module A (id=%u)", module_id);
    return 0;
}

SYS_INIT(example_module_a_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_A);
#endif
