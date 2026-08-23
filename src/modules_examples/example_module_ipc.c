/**
 * @file example_module_ipc.c
 * @brief 示例：module_manager 生命周期 + Thread IPC Service 集成
 *
 * 演示内容：初始化/启动/停止 IPC 服务线程、演示线程内同步调用、可选事件桥发布。
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

#include <zeplod/app_config.h>
#include <zeplod/example_module_ipc.h>
#include <zeplod/ipc_service.h>
#include <zeplod/module_manager.h>

#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_EVENT_BRIDGE)
#include <zeplod/ipc_service_event.h>
#endif

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(example_module_ipc, CONFIG_SYS_LOG_LEVEL);

/** 模块私有状态：IPC 实例、演示线程及生命周期状态 */
typedef struct {
    module_status_t status;
    ipc_service_t   ipc;
    struct k_thread demo_thread;
    bool            demo_thread_valid;
} example_module_ipc_cb_t;

static example_module_ipc_cb_t g_mod_ipc;

/** 演示线程栈：仅用于 start 后发起一次 ipc_call_sync */
K_THREAD_STACK_DEFINE(example_module_ipc_demo_stack, 2048);

static void demo_thread_fn(void* p1, void* p2, void* p3);

/**
 * IPC 服务处理函数（运行于 IPC worker 线程）
 *
 * 此处将 *out_data 指向入参 data，即“原样回显”调用方缓冲区指针；
 * 调用方在 ipc_call_sync 返回前须保持该缓冲区有效。
 */
static int mod_ipc_service_func(ipc_request_id_t request_id, const void* data, size_t data_size, void** out_data,
                                size_t* out_data_size) {
    int ret = 0;

    ARG_UNUSED(request_id);

    *out_data = (void*) data;
    *out_data_size = data_size;

#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_EVENT_BRIDGE)
    /* 将本次处理结果发布到事件总线，供其他模块订阅（与返回值 ret 一致） */
    (void) thread_ipc_event_publish_result(EXAMPLE_MODULE_IPC_EVENT_SOURCE_ID, request_id, ret, EVENT_PRIORITY_NORMAL);
#endif

    return ret;
}

/** 演示：延迟后发起同步 IPC，验证请求/响应通路 */
static void demo_thread_fn(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    k_msleep(300);

    static const char payload[] = "mod_ipc_demo";

    void*  out_data = NULL;
    size_t out_sz = 0;

    int r = ipc_call_sync(&g_mod_ipc.ipc, payload, sizeof(payload), &out_data, &out_sz, K_SECONDS(2));

    if (r == 0) {
        LOG_INF("Thread IPC demo: sync ok, len=%zu", out_sz);
    } else {
        LOG_WRN("Thread IPC demo: sync failed %d", r);
    }
}

int example_module_ipc_init(void* config) {
    ARG_UNUSED(config);

    memset(&g_mod_ipc, 0, sizeof(g_mod_ipc));

    /* 线程栈大小和队列大小通过 Kconfig 配置，无需传入参数 */
    int ret = ipc_service_init(&g_mod_ipc.ipc, "mod_ipc_svc", mod_ipc_service_func, CONFIG_THREAD_IPC_SERVICE_PRIORITY);

    if (ret != 0) {
        LOG_ERR("ipc_service_init failed: %d", ret);
        return ret;
    }

#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_EVENT_BRIDGE)
    /* 注册 EVENT_TYPE_THREAD_IPC_RESPONSE（幂等，可重复调用） */
    (void) thread_ipc_event_register_types();
#endif

    g_mod_ipc.status = MODULE_STATUS_INITIALIZED;
    LOG_INF("example_module_ipc IPC service initialized");
    return 0;
}

int example_module_ipc_start(void) {
    /* 仅允许从”已初始化”或”已停止”进入运行 */
    if (g_mod_ipc.status != MODULE_STATUS_INITIALIZED && g_mod_ipc.status != MODULE_STATUS_STOPPED) {
        return -EINVAL;
    }

    /* stop 后需重新 init（队列/线程元数据由 init 重置） */
    if (g_mod_ipc.status == MODULE_STATUS_STOPPED) {
        int ret =
            ipc_service_init(&g_mod_ipc.ipc, "mod_ipc_svc", mod_ipc_service_func, CONFIG_THREAD_IPC_SERVICE_PRIORITY);
        if (ret != 0) {
            LOG_ERR("ipc_service_init(restart) failed: %d", ret);
            return ret;
        }
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_EVENT_BRIDGE)
        (void) thread_ipc_event_register_types();
#endif
    }

    int ret = ipc_service_start(&g_mod_ipc.ipc);

    if (ret != 0) {
        LOG_ERR("ipc_service_start failed: %d", ret);
        return ret;
    }

    /* 优先级 8 为演示固定值；生产代码建议与业务线程策略统一或做成 Kconfig */
    k_thread_create(&g_mod_ipc.demo_thread, example_module_ipc_demo_stack,
                    K_THREAD_STACK_SIZEOF(example_module_ipc_demo_stack), demo_thread_fn, NULL, NULL, NULL, 8, 0,
                    K_NO_WAIT);
#if IS_ENABLED(CONFIG_THREAD_NAME)
    k_thread_name_set(&g_mod_ipc.demo_thread, "mod_ipc_demo");
#endif
    g_mod_ipc.demo_thread_valid = true;

    g_mod_ipc.status = MODULE_STATUS_RUNNING;
    LOG_INF("example_module_ipc started");
    return 0;
}

int example_module_ipc_stop(void) {
    if (g_mod_ipc.status != MODULE_STATUS_RUNNING) {
        return 0;
    }

    /* 先汇合演示线程，避免其仍向已停止的 IPC 投递请求 */
    if (g_mod_ipc.demo_thread_valid) {
        k_thread_join(&g_mod_ipc.demo_thread, K_FOREVER);
        g_mod_ipc.demo_thread_valid = false;
    }

    int ret = ipc_service_stop(&g_mod_ipc.ipc);

    g_mod_ipc.status = MODULE_STATUS_STOPPED;
    LOG_INF("example_module_ipc stopped (ipc_service_stop=%d)", ret);
    return ret;
}

int example_module_ipc_shutdown(void) {
    (void) example_module_ipc_stop();
    /* 标记未初始化；若需再次使用须重新走 init */
    g_mod_ipc.status = MODULE_STATUS_UNINITIALIZED;
    return 0;
}

void example_module_ipc_on_event(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
}

module_status_t example_module_ipc_get_status(void) {
    return g_mod_ipc.status;
}

int example_module_ipc_control(int cmd, void* arg) {
    ARG_UNUSED(cmd);
    ARG_UNUSED(arg);
    return -1; /* 本示例未实现扩展控制命令 */
}

int example_module_ipc_demo_call_sync(const char* msg, size_t len) {
    if (msg == NULL || len == 0U) {
        return -EINVAL;
    }

    if (g_mod_ipc.status != MODULE_STATUS_RUNNING) {
        return -EINVAL;
    }

    void*  out = NULL;
    size_t outsz = 0;

    return ipc_call_sync(&g_mod_ipc.ipc, msg, len, &out, &outsz, K_SECONDS(2));
}

static const module_interface_t example_module_ipc_interface = {
    .name = "example_module_ipc",
    .version = MODULE_VERSION(1, 0, 0),
    .priority = MODULE_PRIORITY_NORMAL,
    .depends_on = NULL,
    .init = example_module_ipc_init,
    .start = example_module_ipc_start,
    .stop = example_module_ipc_stop,
    .shutdown = example_module_ipc_shutdown,
    .on_event = example_module_ipc_on_event,
    .get_status = example_module_ipc_get_status,
    .control = example_module_ipc_control,
};

const module_interface_t* example_module_ipc_get_interface(void) {
    return &example_module_ipc_interface;
}

static int example_module_ipc_auto_register(void) {
    uint32_t                    module_id;
    example_module_ipc_config_t config_ipc = {.reserved = 0};

    if (module_manager_register(example_module_ipc_get_interface(), &config_ipc, &module_id) != 0) {
        LOG_ERR("module_manager_register example_module_ipc failed");
        return -EIO;
    }
    LOG_INF("Registered example_module_ipc (id=%u)", module_id);
    return 0;
}

SYS_INIT(example_module_ipc_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_IPC);
