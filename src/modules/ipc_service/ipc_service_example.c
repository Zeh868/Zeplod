/**
 * @file ipc_service_example.c
 * @brief IPC 服务框架使用示例
 *
 * 演示 IPC 服务的三种调用模式：
 * 1. SYNC（同步）- 阻塞调用，等待结果返回
 * 2. ASYNC（异步）- 回调模式，立即返回，结果通过回调通知
 * 3. FUTURE（未来值）- Future/Promise 模式，可轮询或等待结果
 *
 * 使用流程：
 * 1. 定义服务函数 example_service_func
 * 2. 初始化并启动服务
 * 3. 分别演示三种调用模式
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-05-09       1.1            zeh            文档：Kconfig 编入说明；Future 示例简化
 * 2026-09-18       1.2            zeh            示例线程栈大小具名；k_thread_create 返回值检查
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
#include <zeplod/app_config.h>

#include <zeplod/ipc_service.h>

LOG_MODULE_REGISTER(ipc_example);

/* ============================================================================
 * 示例服务实现 (Example Service Implementation)
 * ============================================================================ */

/**
 * @brief 示例服务函数：处理 IPC 请求
 *
 * 此服务函数简单地返回输入数据和成功结果。
 * 实际应用中可在此实现任意业务逻辑。
 *
 * @param request_id 请求 ID
 * @param data 输入数据
 * @param data_size 输入数据大小
 * @param out_data 输出数据指针
 * @param out_data_size 输出数据大小
 * @return 0 成功
 */
static int example_service_func(ipc_request_id_t request_id, const void* data, size_t data_size, void** out_data,
                                size_t* out_data_size) {
    LOG_INF("Service processing request %u, data_size=%zu", request_id, data_size);

    /* 模拟处理延迟 */
    k_msleep(10);

    /* 示例：直接返回输入数据（零拷贝） */
    *out_data = (void*) data;
    *out_data_size = data_size;

    return 0;
}

/* ============================================================================
 * 全局服务实例 (Global Service Instance)
 * ============================================================================ */

/** 全局 IPC 服务实例 */
static ipc_service_t g_example_service;

/* ============================================================================
 * 模式 1：SYNC 同步调用示例
 * ============================================================================ */

/**
 * @brief 同步调用示例
 *
 * 演示如何使用 ipc_call_sync 进行阻塞式调用。
 */
static void example_sync_call(void) {
    LOG_INF("=== SYNC Mode Example ===");

    const char* input_data = "Hello Sync!";
    void*       output_data = NULL;
    size_t      output_size = 0;

    /* 同步调用：阻塞直到服务返回或超时 */
    int result =
        ipc_call_sync(&g_example_service, input_data, strlen(input_data) + 1, &output_data, &output_size, K_SECONDS(1));

    if (result == 0) {
        LOG_INF("SYNC call succeeded: %s", (char*) output_data);
    } else {
        LOG_ERR("SYNC call failed: %d", result);
    }
}

/* ============================================================================
 * 模式 2：ASYNC 异步调用示例
 * ============================================================================ */

/**
 * @brief 异步调用回调函数
 *
 * 当异步请求完成时由框架调用此函数。
 *
 * @param request_id 请求 ID
 * @param result 服务函数返回值
 * @param data 输出数据
 * @param data_size 输出数据大小
 * @param user_data 用户自定义数据
 */
static void async_callback(ipc_request_id_t request_id, int result, const void* data, size_t data_size,
                           void* user_data) {
    LOG_INF("ASYNC callback: request_id=%u, result=%d, data=%s, user_data=%p", request_id, result, (char*) data,
            user_data);
}

/**
 * @brief 异步调用示例
 *
 * 演示如何使用 ipc_call_async 进行非阻塞调用。
 */
static void example_async_call(void) {
    LOG_INF("=== ASYNC Mode Example ===");

    const char*      input_data = "Hello Async!";
    ipc_request_id_t request_id;

    /* 异步调用：立即返回，回调将在服务完成后执行 */
    int result = ipc_call_async(&g_example_service, input_data, strlen(input_data) + 1, async_callback,
                                (void*) 0x12345678, /* 用户数据 */
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                                0,
#endif
                                &request_id);

    if (result == 0) {
        LOG_INF("ASYNC call sent, request_id=%u", request_id);
        /* 立即返回 - 回调将在稍后被调用 */
    } else {
        LOG_ERR("ASYNC call failed: %d", result);
    }

    /* 等待回调被执行 */
    k_msleep(100);
}

/* ============================================================================
 * 模式 3：FUTURE 未来值调用示例
 * ============================================================================ */

/**
 * @brief Future 模式调用示例
 *
 * 演示如何使用 ipc_call_future 和 ipc_future_wait 进行调用。
 */
static void example_future_call(void) {
    LOG_INF("=== FUTURE Mode Example ===");

    const char*   input_data = "Hello Future!";
    ipc_future_t* future = NULL;

    /* Future 模式调用：返回 future 对象，可稍后获取结果 */
    int result = ipc_call_future(&g_example_service, input_data, strlen(input_data) + 1,
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                                 0,
#endif
                                 &future);

    if (result == 0 && future != NULL) {
        LOG_INF("FUTURE call sent, waiting for result...");

        int         future_result;
        const void* output_data;
        size_t      output_size;

        result = ipc_future_wait(&g_example_service, future, &future_result, &output_data, &output_size, K_SECONDS(1));

        if (result == 0) {
            LOG_INF("FUTURE call succeeded: result=%d, data=%s", future_result, (char*) output_data);
        } else {
            LOG_ERR("FUTURE wait failed: %d", result);
        }

        /* 释放 future 对象（重要：避免资源泄漏） */
        ipc_future_release(&g_example_service, future);
    } else {
        LOG_ERR("FUTURE call failed: %d", result);
    }
}

/* ============================================================================
 * 测试线程 (Test Thread)
 * ============================================================================ */

/** 示例线程栈大小（字节） */
#define EXAMPLE_THREAD_STACK_SIZE 2048

/**
 * @brief 示例线程栈
 */
static K_THREAD_STACK_DEFINE(example_stack, EXAMPLE_THREAD_STACK_SIZE);

/**
 * @brief 示例线程函数
 *
 * 按顺序演示三种调用模式。
 */
static void example_thread_func(void* p1, void* p2, void* p3) {
    LOG_INF("IPC Service Example Started");

    /* 等待服务启动完成 */
    k_msleep(100);

    /* 运行所有示例 */
    example_sync_call();
    k_msleep(200);

    example_async_call();
    k_msleep(200);

    example_future_call();
    k_msleep(200);

    LOG_INF("IPC Service Example Completed");

    /* 打印统计信息 */
    LOG_INF("Pending requests: %zu", ipc_service_get_pending_count(&g_example_service));
}

/* ============================================================================
 * 初始化 (Initialization)
 * ============================================================================ */

/**
 * @brief 初始化并启动 IPC 服务
 *
 * 此函数在系统启动时自动调用（通过 SYS_INIT）。
 *
 * @return 0 成功，负值错误码失败
 */
static int ipc_service_example_init(void) {
    int ret;

    /* 初始化服务 */
    ret = ipc_service_init(&g_example_service, "example_service", example_service_func,
                           CONFIG_THREAD_IPC_SERVICE_PRIORITY);

    if (ret != 0) {
        LOG_ERR("Failed to initialize IPC service: %d", ret);
        return ret;
    }

    /* 启动服务 */
    ret = ipc_service_start(&g_example_service);
    if (ret != 0) {
        LOG_ERR("Failed to start IPC service: %d", ret);
        return ret;
    }

    /* 创建示例线程 */
    static struct k_thread example_thread;
    k_tid_t example_tid = k_thread_create(&example_thread, example_stack, K_THREAD_STACK_SIZEOF(example_stack),
                                          example_thread_func, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    if (example_tid == NULL) {
        LOG_ERR("Failed to create example thread");
        return -EIO;
    }

    LOG_INF("IPC Service Example initialized");

    return 0;
}

#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_EXAMPLE)
/* POST_KERNEL：APPLICATION 阶段需在 Zephyr CONFIG_APPLICATION_INIT 下才链接，否则会报
 * "Undefined initialization levels used" */
SYS_INIT(ipc_service_example_init, POST_KERNEL, APP_INIT_PRIO_MODULE_IPC);
#endif
