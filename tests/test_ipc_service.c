/**
 * @file test_ipc_service.c
 * @brief Thread IPC 服务单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.3
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-04-11       1.1            zeh            补充完整测试用例
 * 2026-05-28       1.2            zeh            异步等待改用 ztest_sync（P0）
 * 2026-09-18       1.3            zeh            NULL callback 契约断言具体值；stop 超时时延由 join 超时常量推导；shm 句柄数组具名
 *
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>
#include <string.h>

#if IS_ENABLED(CONFIG_EXAMPLE_MODULE_UART)
#include <zeplod/example_module_uart.h>
#endif
#include <zeplod/ipc_service.h>
#include <zeplod/zepl_thread_service.h>
#include "ztest_sync.h"

LOG_MODULE_REGISTER(test_ipc_service);

/*
 * stop 超时用例的时延均由 IPC 服务线程 join 超时常量推导
 * （ipc_service_lifecycle.c 的 stop 用 IPC_SERVICE_THREAD_JOIN_TIMEOUT_MS =
 * ZEPL_THREAD_SERVICE_JOIN_TIMEOUT_MS 做 k_thread_join，超时返回 -EIO）：
 * - handler 停留 = join 超时 + 200ms —— 保证首次 stop 的 join 必然超时返回 -EIO；
 * - 重试前等待 = join 超时 + 300ms —— 首次 stop 返回时 handler 至多剩 200ms，
 *   再留 ≥100ms 裕量，重试 stop 时 worker 已退出。
 */
#define IPC_UT_STOP_HANDLER_DELAY_MS (ZEPL_THREAD_SERVICE_JOIN_TIMEOUT_MS + 200U)
#define IPC_UT_STOP_RETRY_WAIT_MS    (ZEPL_THREAD_SERVICE_JOIN_TIMEOUT_MS + 300U)

/*
 * shm 耗尽用例的句柄数组上限：测试配置的池大小
 * CONFIG_THREAD_IPC_SERVICE_SHARED_MEM_POOL_SIZE 必须不大于该值，
 * 循环才能在数组范围内把池填满（用例内以 BUILD_ASSERT 前置校验）。
 */
#define IPC_UT_SHM_HANDLE_MAX 16U

static ipc_service_t g_ipc;

static atomic_t g_delayed_handler_in_handler;
static atomic_t g_async_callback_count;
static atomic_t g_handler_call_count;

static bool ipc_async_callback_done(void* ctx) {
    ARG_UNUSED(ctx);
    return atomic_get(&g_async_callback_count) >= 1;
}

static bool ipc_service_pending_zero(void* ctx) {
    ipc_service_t* ipc = ctx;

    return ipc_service_get_pending_count(ipc) == 0U;
}

static bool ipc_delayed_started_or_pending(void* ctx) {
    ipc_service_t* ipc = ctx;

    if (atomic_get(&g_delayed_handler_in_handler) != 0) {
        return true;
    }
    return ipc_service_get_pending_count(ipc) > 0U;
}

static bool ipc_future_ready_ctx(void* ctx) {
    ipc_future_t* future = ctx;

    return ipc_future_is_ready(future);
}

/* =============================================================================
 * 测试辅助函数
 * ============================================================================= */

static int ipc_ut_handler(ipc_request_id_t request_id, const void* data, size_t data_size, void** out_data,
                          size_t* out_data_size) {
    (void) request_id;
    *out_data = (void*) data;
    *out_data_size = data_size;
    return 0;
}

/* 延迟响应的 handler，用于测试异步和 Future */
static int ipc_delayed_handler(ipc_request_id_t request_id, const void* data, size_t data_size, void** out_data,
                               size_t* out_data_size) {
    (void) request_id;
    atomic_set(&g_delayed_handler_in_handler, 1);
    k_msleep(100); /* 模拟处理延迟（被测行为，非测试等待） */
    *out_data = (void*) data;
    *out_data_size = data_size;
    atomic_set(&g_delayed_handler_in_handler, 0);
    return 0;
}

static int ipc_stop_timeout_handler(ipc_request_id_t request_id, const void* data, size_t data_size, void** out_data,
                                    size_t* out_data_size) {
    (void) request_id;
    atomic_set(&g_delayed_handler_in_handler, 1);
    k_msleep(IPC_UT_STOP_HANDLER_DELAY_MS);
    *out_data = (void*) data;
    *out_data_size = data_size;
    atomic_set(&g_delayed_handler_in_handler, 0);
    return 0;
}

/* 返回错误的 handler */
static int ipc_error_handler(ipc_request_id_t request_id, const void* data, size_t data_size, void** out_data,
                             size_t* out_data_size) {
    (void) request_id;
    (void) data;
    (void) data_size;
    (void) out_data;
    (void) out_data_size;
    return -EINVAL;
}

/* 计数器 handler */
static int ipc_counting_handler(ipc_request_id_t request_id, const void* data, size_t data_size, void** out_data,
                                size_t* out_data_size) {
    (void) request_id;
    (void) data;
    (void) data_size;
    atomic_inc(&g_handler_call_count);
    *out_data = NULL;
    *out_data_size = 0;
    return 0;
}

/* 异步回调状态 */
static int              g_async_callback_result = 0;
static size_t           g_async_callback_data_size = 0;
static ipc_request_id_t g_async_callback_request_id = 0;

static void async_test_callback(ipc_request_id_t request_id, int result, const void* data, size_t data_size,
                                void* user_data) {
    (void) data;
    (void) user_data;
    atomic_inc(&g_async_callback_count);
    g_async_callback_result = result;
    g_async_callback_data_size = data_size;
    g_async_callback_request_id = request_id;
}

/* =============================================================================
 * 测试套件 setup/teardown
 * ============================================================================= */

static void* test_suite_setup(void) {
#if IS_ENABLED(CONFIG_EXAMPLE_MODULE_UART)
    /* 确保 UART 模块不会在后台运行（避免 spinlock 竞争） */
    example_module_uart_stop();
#endif

    return NULL;
}

static void test_suite_teardown(void* fixture) {
    (void) fixture;
    ipc_service_stop(&g_ipc);
}

/* =============================================================================
 * 基础功能测试
 * ============================================================================= */

ZTEST(ipc_service, test_init_start_sync_stop) {
    const char payload[] = "ipc_ut";
    void*      out = NULL;
    size_t     outsz = 0;
    int        r;

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_ut_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    r = ipc_call_sync(&g_ipc, payload, sizeof(payload), &out, &outsz, K_SECONDS(2));
    zassert_equal(r, 0, "ipc_call_sync failed: %d", r);
    zassert_equal(outsz, sizeof(payload), NULL);
    zassert_mem_equal(out, payload, sizeof(payload), NULL);

    r = ipc_service_stop(&g_ipc);
    zassert_equal(r, 0, "ipc_service_stop failed: %d", r);
}

ZTEST(ipc_service, test_sync_call_with_large_data) {
    char   large_payload[256];
    void*  out = NULL;
    size_t outsz = 0;
    int    r;

    memset(large_payload, 'A', sizeof(large_payload) - 1);
    large_payload[sizeof(large_payload) - 1] = '\0';

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_ut_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    r = ipc_call_sync(&g_ipc, large_payload, sizeof(large_payload), &out, &outsz, K_SECONDS(2));
    zassert_equal(r, 0, "ipc_call_sync failed: %d", r);
    zassert_equal(outsz, sizeof(large_payload), NULL);
    zassert_mem_equal(out, large_payload, sizeof(large_payload), NULL);

    ipc_service_stop(&g_ipc);
}

ZTEST(ipc_service, test_sync_call_error_handler) {
    const char payload[] = "error_test";
    void*      out = NULL;
    size_t     outsz = 0;
    int        r;

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_error_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    r = ipc_call_sync(&g_ipc, payload, sizeof(payload), &out, &outsz, K_SECONDS(2));
    zassert_equal(r, -EINVAL, "应返回 -EINVAL，实际返回: %d", r);

    ipc_service_stop(&g_ipc);
}

/* =============================================================================
 * 异步调用测试
 * ============================================================================= */

ZTEST(ipc_service, test_async_call) {
    const char       payload[] = "async_test";
    ipc_request_id_t request_id = 0;
    int              r;

    atomic_set(&g_async_callback_count, 0);
    g_async_callback_result = 0;
    g_async_callback_data_size = 0;

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_ut_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    r = ipc_call_async(&g_ipc, payload, sizeof(payload), async_test_callback, NULL,
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                       0,
#endif
                       &request_id);
    zassert_equal(r, 0, "ipc_call_async failed: %d", r);
    zassert_not_equal(request_id, 0, "request_id 不应为 0");

    zassert_true(ztest_wait_until(ipc_async_callback_done, NULL, 2000U), "回调应已执行");

    zassert_equal(atomic_get(&g_async_callback_count), 1, "回调应被调用一次");
    zassert_equal(g_async_callback_result, 0, "回调结果应为 0");
    zassert_equal(g_async_callback_data_size, sizeof(payload), "数据大小应匹配");
    zassert_equal(g_async_callback_request_id, request_id, "request_id 应匹配");

    ipc_service_stop(&g_ipc);
}

ZTEST(ipc_service, test_async_call_with_delayed_handler) {
    const char       payload[] = "delayed_async";
    ipc_request_id_t request_id = 0;
    int              r;

    atomic_set(&g_async_callback_count, 0);

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_delayed_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    r = ipc_call_async(&g_ipc, payload, sizeof(payload), async_test_callback, NULL,
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                       0,
#endif
                       &request_id);
    zassert_equal(r, 0, "ipc_call_async failed: %d", r);

    zassert_true(ztest_wait_until(ipc_async_callback_done, NULL, 2000U), "延迟 handler 完成后回调应执行");
    zassert_equal(atomic_get(&g_async_callback_count), 1, "回调应被调用一次");

    ipc_service_stop(&g_ipc);
}

/* =============================================================================
 * Future 模式测试
 * ============================================================================= */

ZTEST(ipc_service, test_future_call) {
    const char    payload[] = "future_test";
    ipc_future_t* future = NULL;
    int           result = -1;
    const void*   out_data = NULL;
    size_t        out_size = 0;
    int           r;

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_ut_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    r = ipc_call_future(&g_ipc, payload, sizeof(payload),
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                        0,
#endif
                        &future);
    zassert_equal(r, 0, "ipc_call_future failed: %d", r);
    zassert_not_null(future, "future 不应为 NULL");

    r = ipc_future_wait(&g_ipc, future, &result, &out_data, &out_size, K_SECONDS(2));
    zassert_equal(r, 0, "ipc_future_wait failed: %d", r);
    zassert_equal(result, 0, "result 应为 0");
    zassert_equal(out_size, sizeof(payload), "数据大小应匹配");
    zassert_mem_equal(out_data, payload, sizeof(payload), NULL);

    result = -1;
    r = ipc_future_wait(&g_ipc, future, &result, &out_data, &out_size, K_NO_WAIT);
    zassert_equal(r, 0, "completed future should support repeated wait: %d", r);
    zassert_equal(result, 0, "repeated wait result should remain available");

    r = ipc_future_release(&g_ipc, future);
    zassert_equal(r, 0, "ipc_future_release failed: %d", r);

    ipc_service_stop(&g_ipc);
}

ZTEST(ipc_service, test_future_is_ready) {
    const char    payload[] = "future_ready_test";
    ipc_future_t* future = NULL;
    int           r;

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_delayed_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    r = ipc_call_future(&g_ipc, payload, sizeof(payload),
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                        0,
#endif
                        &future);
    zassert_equal(r, 0, "ipc_call_future failed: %d", r);

    /* 立即检查，应未就绪 */
    bool ready = ipc_future_is_ready(future);
    zassert_equal(ready, false, "future 应未就绪");

    zassert_true(ztest_wait_until(ipc_future_ready_ctx, future, 2000U), "future 应变为就绪");
    ready = ipc_future_is_ready(future);
    zassert_equal(ready, true, "future 应已就绪");

    ipc_future_release(&g_ipc, future);
    ipc_service_stop(&g_ipc);
}

ZTEST(ipc_service, test_future_wait_timeout) {
    const char    payload[] = "timeout_test";
    ipc_future_t* future = NULL;
    int           result = 0;
    const void*   out_data = NULL;
    size_t        out_size = 0;
    int           r;

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_delayed_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    r = ipc_call_future(&g_ipc, payload, sizeof(payload),
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                        0,
#endif
                        &future);
    zassert_equal(r, 0, "ipc_call_future failed: %d", r);

    /* 使用很短的超时，应超时 */
    r = ipc_future_wait(&g_ipc, future, &result, &out_data, &out_size, K_MSEC(10));
    zassert_equal(r, -EAGAIN, "应返回 -EAGAIN，实际: %d", r);

    zassert_true(ztest_wait_until(ipc_future_ready_ctx, future, 2000U), "future 应在 handler 完成后就绪");
    ipc_future_wait(&g_ipc, future, &result, &out_data, &out_size, K_SECONDS(1));
    ipc_future_release(&g_ipc, future);
    ipc_service_stop(&g_ipc);
}

ZTEST(ipc_service, test_stop_completes_pending_future) {
    const char    payload[] = "future_stop_test";
    ipc_future_t* future = NULL;
    int           future_result = 0;
    const void*   out_data = NULL;
    size_t        out_size = 0;
    int           r;

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_delayed_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    r = ipc_call_future(&g_ipc, payload, sizeof(payload),
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                        0,
#endif
                        &future);
    zassert_equal(r, 0, "ipc_call_future failed: %d", r);
    zassert_not_null(future, "future should not be NULL");

    /* stop 必须唤醒 future 等待者，不能让 future 悬挂在 pending 表中。 */
    r = ipc_service_stop(&g_ipc);
    zassert_equal(r, 0, "ipc_service_stop failed: %d", r);

    zassert_true(ipc_future_is_ready(future), "future should be completed by stop");

    r = ipc_future_wait(&g_ipc, future, &future_result, &out_data, &out_size, K_NO_WAIT);
    zassert_equal(r, 0, "ipc_future_wait after stop failed: %d", r);
    zassert_equal(future_result, -ECANCELED, "future should be canceled by stop");
    zassert_is_null(out_data, "canceled future should not expose data");
    zassert_equal(out_size, 0, "canceled future data size should be 0");

    r = ipc_future_release(&g_ipc, future);
    zassert_equal(r, 0, "ipc_future_release failed: %d", r);
}

/* =============================================================================
 * 请求管理测试
 * ============================================================================= */

ZTEST(ipc_service, test_get_pending_count) {
    const char       payload[] = "pending_test";
    ipc_request_id_t request_id = 0;
    int              r;

    atomic_set(&g_delayed_handler_in_handler, 0);

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_delayed_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    /* 初始应为 0 */
    size_t count = ipc_service_get_pending_count(&g_ipc);
    zassert_equal(count, 0, "初始 pending 数量应为 0");

    /* 发送异步请求 */
    r = ipc_call_async(&g_ipc, payload, sizeof(payload), async_test_callback, NULL,
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                       0,
#endif
                       &request_id);
    zassert_equal(r, 0, "ipc_call_async failed: %d", r);

    zassert_true(ztest_wait_until(ipc_delayed_started_or_pending, &g_ipc, 500U), "请求应已入队或进入 delayed handler");
    count = ipc_service_get_pending_count(&g_ipc);
    zassert_true(count <= 1, "pending 数量应 <= 1");

    zassert_true(ztest_wait_until(ipc_service_pending_zero, &g_ipc, 2000U), "处理完成后 pending 应为 0");
    count = ipc_service_get_pending_count(&g_ipc);
    zassert_equal(count, 0, "处理完成后 pending 数量应为 0");

    ipc_service_stop(&g_ipc);
}

ZTEST(ipc_service, test_cancel_future_before_stop) {
    const char    payload[] = "cancel_future_stop";
    ipc_future_t* future = NULL;
    int           future_result = 0;
    const void*   out_data = NULL;
    size_t        out_size = 0;
    int           r;

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_delayed_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    r = ipc_call_future(&g_ipc, payload, sizeof(payload),
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                        0,
#endif
                        &future);
    zassert_equal(r, 0, "ipc_call_future failed: %d", r);
    zassert_not_null(future, "future should not be NULL");

    r = ipc_service_cancel(&g_ipc, future->request_id);
    zassert_equal(r, 0, "cancel future request failed: %d", r);
    zassert_true(ipc_future_is_ready(future), "future should be ready after cancel");

    r = ipc_future_wait(&g_ipc, future, &future_result, &out_data, &out_size, K_NO_WAIT);
    zassert_equal(r, 0, "ipc_future_wait after cancel failed: %d", r);
    zassert_equal(future_result, -ECANCELED, "canceled future should return -ECANCELED");

    r = ipc_future_release(&g_ipc, future);
    zassert_equal(r, 0, "ipc_future_release failed: %d", r);

    /* cancel 已收尾 pending 后，stop 仍应正常完成且可再次 start */
    r = ipc_service_stop(&g_ipc);
    zassert_equal(r, 0, "ipc_service_stop after cancel failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start after cancel+stop failed: %d", r);

    ipc_service_stop(&g_ipc);
}

ZTEST(ipc_service, test_cancel_async_then_stop) {
    const char       payload[] = "cancel_async_stop";
    ipc_request_id_t request_id = 0;
    int              r;

    atomic_set(&g_async_callback_count, 0);

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_delayed_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    r = ipc_call_async(&g_ipc, payload, sizeof(payload), async_test_callback, NULL,
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                       0,
#endif
                       &request_id);
    zassert_equal(r, 0, "ipc_call_async failed: %d", r);

    (void) ipc_service_cancel(&g_ipc, request_id);

    r = ipc_service_stop(&g_ipc);
    zassert_equal(r, 0, "ipc_service_stop after cancel+async failed: %d", r);

    zassert_equal(ipc_service_get_pending_count(&g_ipc), 0U, "stop 后 pending 应为 0");
}

ZTEST(ipc_service, test_cancel_request) {
    const char       payload[] = "cancel_test";
    ipc_request_id_t request_id = 0;
    int              r;

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_delayed_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    /* 发送异步请求 */
    r = ipc_call_async(&g_ipc, payload, sizeof(payload), async_test_callback, NULL,
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                       0,
#endif
                       &request_id);
    zassert_equal(r, 0, "ipc_call_async failed: %d", r);

    /* 尝试取消请求（可能已在处理中，所以结果不确定） */
    r = ipc_service_cancel(&g_ipc, request_id);
    /* 结果可能是 0（成功取消）或 -ENOENT（未找到/已完成） */
    zassert_true(r == 0 || r == -ENOENT, "取消结果应为 0 或 -ENOENT，实际: %d", r);

    /* 取消不存在的请求 */
    r = ipc_service_cancel(&g_ipc, 99999);
    zassert_equal(r, -ENOENT, "取消不存在的请求应返回 -ENOENT");

    ipc_service_stop(&g_ipc);
}

ZTEST(ipc_service, test_generate_request_id) {
    ipc_request_id_t id1 = ipc_generate_request_id();
    ipc_request_id_t id2 = ipc_generate_request_id();
    ipc_request_id_t id3 = ipc_generate_request_id();

    zassert_not_equal(id1, 0, "ID 不应为 0");
    zassert_not_equal(id2, 0, "ID 不应为 0");
    zassert_not_equal(id3, 0, "ID 不应为 0");

    /* ID 应该是唯一的 */
    zassert_not_equal(id1, id2, "ID 应唯一");
    zassert_not_equal(id2, id3, "ID 应唯一");
    zassert_not_equal(id1, id3, "ID 应唯一");
}

/* =============================================================================
 * 边界条件测试
 * ============================================================================= */

ZTEST(ipc_service, test_null_service_init) {
    int r = ipc_service_init(NULL, "test", ipc_ut_handler, 5);
    zassert_not_equal(r, 0, "NULL service 应失败");
}

ZTEST(ipc_service, test_null_handler_init) {
    int r = ipc_service_init(&g_ipc, "test", NULL, 5);
    zassert_not_equal(r, 0, "NULL handler 应失败");
}

ZTEST(ipc_service, test_stop_without_start) {
    int r = ipc_service_init(&g_ipc, "ut_ipc", ipc_ut_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    /* 未启动直接停止 */
    r = ipc_service_stop(&g_ipc);
    zassert_equal(r, 0, "未启动直接停止应返回 0");
}

ZTEST(ipc_service, test_repeat_stop_idempotent) {
    int r = ipc_service_init(&g_ipc, "ut_ipc", ipc_ut_handler, 5);

    zassert_equal(r, 0, NULL);
    zassert_equal(ipc_service_start(&g_ipc), 0, NULL);
    zassert_equal(ipc_service_stop(&g_ipc), 0, NULL);
    zassert_equal(ipc_service_stop(&g_ipc), 0, "重复 stop 应幂等返回 0");
}

ZTEST(ipc_service, test_duplicate_start_ealready) {
    int r = ipc_service_init(&g_ipc, "ut_ipc", ipc_ut_handler, 5);

    zassert_equal(r, 0, NULL);
    zassert_equal(ipc_service_start(&g_ipc), 0, NULL);
    r = ipc_service_start(&g_ipc);
    zassert_equal(r, -EALREADY, "重复 start 应返回 -EALREADY");
    zassert_equal(ipc_service_stop(&g_ipc), 0, NULL);
}

ZTEST(ipc_service, test_sync_call_before_start) {
    const char payload[] = "test";
    void*      out = NULL;
    size_t     outsz = 0;

    int r = ipc_service_init(&g_ipc, "ut_ipc", ipc_ut_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_call_sync(&g_ipc, payload, sizeof(payload), &out, &outsz, K_MSEC(100));
    zassert_not_equal(r, 0, "未启动调用应失败");
}

ZTEST(ipc_service, test_multiple_start_stop_cycles) {
    const char payload[] = "cycle_test";
    void*      out = NULL;
    size_t     outsz = 0;
    int        r;

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_ut_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    for (int i = 0; i < 3; i++) {
        r = ipc_service_start(&g_ipc);
        zassert_equal(r, 0, "ipc_service_start failed: %d", r);

        r = ipc_call_sync(&g_ipc, payload, sizeof(payload), &out, &outsz, K_SECONDS(1));
        zassert_equal(r, 0, "ipc_call_sync failed: %d", r);

        r = ipc_service_stop(&g_ipc);
        zassert_equal(r, 0, "ipc_service_stop failed: %d", r);

        zassert_true(ztest_wait_until(ipc_service_pending_zero, &g_ipc, 500U), "stop 后 pending 应为 0");
    }
}

ZTEST(ipc_service, test_concurrent_requests) {
    const char payload1[] = "req1";
    const char payload2[] = "req2";
    const char payload3[] = "req3";
    void*      out1 = NULL;
    void*      out2 = NULL;
    void*      out3 = NULL;
    size_t     outsz1 = 0;
    size_t     outsz2 = 0;
    size_t     outsz3 = 0;
    int        r;

    atomic_set(&g_handler_call_count, 0);

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_counting_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    /* 顺序发送多个请求 */
    r = ipc_call_sync(&g_ipc, payload1, sizeof(payload1), &out1, &outsz1, K_SECONDS(1));
    zassert_equal(r, 0, "req1 failed");

    r = ipc_call_sync(&g_ipc, payload2, sizeof(payload2), &out2, &outsz2, K_SECONDS(1));
    zassert_equal(r, 0, "req2 failed");

    r = ipc_call_sync(&g_ipc, payload3, sizeof(payload3), &out3, &outsz3, K_SECONDS(1));
    zassert_equal(r, 0, "req3 failed");

    zassert_equal(atomic_get(&g_handler_call_count), 3, "handler 应被调用 3 次");

    ipc_service_stop(&g_ipc);
}

ZTEST(ipc_service, test_sync_call_timeout) {
    const char payload[] = "timeout_test";
    void*      out = NULL;
    size_t     outsz = 0;
    int        r;

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_delayed_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    /* 使用很短的超时，应超时 */
    r = ipc_call_sync(&g_ipc, payload, sizeof(payload), &out, &outsz, K_MSEC(10));
    zassert_equal(r, -EAGAIN, "应返回 -EAGAIN，实际: %d", r);

    zassert_true(ztest_wait_until(ipc_service_pending_zero, &g_ipc, 2000U), "超时后 handler 应完成且 pending 清零");

    ipc_service_stop(&g_ipc);
}

ZTEST(ipc_service, test_stop_timeout_can_be_retried) {
    const char    payload[] = "stop_timeout";
    ipc_future_t* future = NULL;
    int           future_result = 0;
    int           r;

    atomic_set(&g_delayed_handler_in_handler, 0);

    r = ipc_service_init(&g_ipc, "stop_timeout", ipc_stop_timeout_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);
    zassert_equal(ipc_service_start(&g_ipc), 0, "ipc_service_start failed");

    r = ipc_call_future(&g_ipc, payload, sizeof(payload),
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                        0,
#endif
                        &future);
    zassert_equal(r, 0, "ipc_call_future failed: %d", r);
    zassert_true(ztest_wait_atomic_nonzero(&g_delayed_handler_in_handler, 500U), "handler should have started");

    r = ipc_service_stop(&g_ipc);
    zassert_equal(r, -EIO, "first stop should report join timeout: %d", r);
    zassert_true(ipc_service_get_pending_count(&g_ipc) > 0U, "timed-out stop must preserve pending state");

    /* 等待时长由 join 超时推导（见 IPC_UT_STOP_RETRY_WAIT_MS 注释），不依赖裸魔数 */
    k_msleep(IPC_UT_STOP_RETRY_WAIT_MS);
    r = ipc_service_stop(&g_ipc);
    zassert_equal(r, 0, "retry stop should join and clean up: %d", r);

    r = ipc_future_wait(&g_ipc, future, &future_result, NULL, NULL, K_NO_WAIT);
    zassert_equal(r, 0, "future should be completed during successful retry stop: %d", r);
    zassert_equal(future_result, -ECANCELED, "pending future should be canceled");
    zassert_equal(ipc_future_release(&g_ipc, future), 0, "future release failed");
}

ZTEST(ipc_service, test_async_null_callback) {
    const char       payload[] = "null_cb_test";
    ipc_request_id_t request_id = 0;
    int              r;

    r = ipc_service_init(&g_ipc, "ut_ipc", ipc_ut_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    /* 契约（src/modules/ipc_service/ipc_service_call.c ipc_call_async）：
     * callback == NULL 直接拒绝返回 -EINVAL，不存在 fire-and-forget 模式；
     * 拒绝发生在入队之前，不产生 pending 请求，out_request_id 不被写出。 */
    r = ipc_call_async(&g_ipc, payload, sizeof(payload), NULL, NULL,
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                       0,
#endif
                       &request_id);
    zassert_equal(r, -EINVAL, "NULL callback 应被拒绝并返回 -EINVAL: %d", r);
    zassert_equal(request_id, 0, "被拒绝的请求不得写出 request_id");
    zassert_equal(ipc_service_get_pending_count(&g_ipc), 0U, "被拒绝的请求不得占用 pending 槽位");

    ipc_service_stop(&g_ipc);
}

#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
/* =============================================================================
 * 共享内存引用计数测试
 * ============================================================================= */

/* 服务函数：分配共享内存并返回（通过测试全局 g_ipc） */
static int ipc_shm_alloc_handler(ipc_request_id_t request_id, const void* data, size_t data_size, void** out_data,
                                 size_t* out_data_size) {
    (void) request_id;
    (void) data;
    (void) data_size;

    ipc_shm_handle_t handle = ipc_shm_alloc(&g_ipc, 32);

    if (handle == IPC_SHM_HANDLE_INVALID) {
        return -ENOMEM;
    }

    void* ptr = ipc_shm_get_ptr(&g_ipc, handle);

    if (ptr == NULL) {
        (void) ipc_shm_release(&g_ipc, handle);
        return -EIO;
    }

    (void) memset(ptr, 0, 32);
    (void) memcpy(ptr, "shm_out", 8);

    *out_data = ptr;
    *out_data_size = 32;
    return 0;
}

static int ipc_shm_distinct_output_handler(ipc_request_id_t request_id, const void* data, size_t data_size,
                                           void** out_data, size_t* out_data_size) {
    (void) request_id;
    (void) data;
    (void) data_size;

    ipc_shm_handle_t handle = ipc_shm_alloc(&g_ipc, 32);
    if (handle == IPC_SHM_HANDLE_INVALID) {
        return -ENOMEM;
    }

    void* ptr = ipc_shm_get_ptr(&g_ipc, handle);
    if (ptr == NULL) {
        (void) ipc_shm_release(&g_ipc, handle);
        return -EIO;
    }

    memcpy(ptr, "distinct_out", sizeof("distinct_out"));
    *out_data = ptr;
    *out_data_size = sizeof("distinct_out");
    return 0;
}

/* 测试 sync_shm 端到端：service 分配 shm，调用方通过句柄释放 */
ZTEST(ipc_service, test_sync_call_shm_output) {
    void*            out_data = NULL;
    size_t           out_size = 0;
    ipc_shm_handle_t out_handle = IPC_SHM_HANDLE_INVALID;
    int              r;

    r = ipc_service_init(&g_ipc, "shm_sync", ipc_shm_alloc_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    r = ipc_call_sync_shm(&g_ipc, NULL, 0, 0, &out_data, &out_size, &out_handle, K_SECONDS(2));
    zassert_equal(r, 0, "ipc_call_sync_shm failed: %d", r);
    zassert_true(out_handle != IPC_SHM_HANDLE_INVALID, "expected valid out shm handle");
    zassert_not_null(out_data, "expected out_data");
    zassert_equal(out_size, 32, "unexpected out_size");
    zassert_mem_equal(out_data, "shm_out", 7, "unexpected shm payload");

    r = ipc_shm_release(&g_ipc, out_handle);
    zassert_equal(r, 0, "shm_release failed: %d", r);

    ipc_service_stop(&g_ipc);
}

/* 测试共享内存基本功能 */
ZTEST(ipc_service, test_shared_memory_basic) {
    int r;

    r = ipc_service_init(&g_ipc, "shm_test", ipc_shm_alloc_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    r = ipc_service_start(&g_ipc);
    zassert_equal(r, 0, "ipc_service_start failed: %d", r);

    /* 测试共享内存分配 */
    ipc_shm_handle_t handle = ipc_shm_alloc(&g_ipc, 64);
    zassert_true(handle != IPC_SHM_HANDLE_INVALID, "shm_alloc failed");

    void* ptr = ipc_shm_get_ptr(&g_ipc, handle);
    zassert_not_null(ptr, "shm_get_ptr returned NULL");

    size_t size = ipc_shm_get_size(&g_ipc, handle);
    zassert_equal(size, 64, "shm_get_size returned wrong size: %zu", size);

    /* 写入测试数据 */
    const char test_data[] = "hello shm";
    memcpy(ptr, test_data, sizeof(test_data));

    /* 测试引用计数 */
    zassert_equal(ipc_shm_acquire(&g_ipc, handle), 0, "ipc_shm_acquire failed");
    r = ipc_shm_release(&g_ipc, handle);
    zassert_equal(r, 0, "shm_release failed: %d", r);

    /* 再次释放，引用计数应降至 0 */
    r = ipc_shm_release(&g_ipc, handle);
    zassert_equal(r, 0, "shm_release failed on final release: %d", r);

    /* 验证句柄不再有效 */
    zassert_false(ipc_shm_is_valid(&g_ipc, handle), "handle should be invalid after release");

    ipc_service_stop(&g_ipc);
}

ZTEST(ipc_service, test_shared_memory_rejects_stale_handle) {
    int r = ipc_service_init(&g_ipc, "shm_stale", ipc_shm_alloc_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    ipc_shm_handle_t stale = ipc_shm_alloc(&g_ipc, 32);
    zassert_not_equal(stale, IPC_SHM_HANDLE_INVALID, "initial allocation failed");
    zassert_equal(ipc_shm_release(&g_ipc, stale), 0, "initial release failed");

    ipc_shm_handle_t current = ipc_shm_alloc(&g_ipc, 32);
    zassert_not_equal(current, IPC_SHM_HANDLE_INVALID, "replacement allocation failed");
    zassert_not_equal(current, stale, "reused slot must have a new generation handle");
    zassert_false(ipc_shm_is_valid(&g_ipc, stale), "stale handle must remain invalid");
    zassert_equal(ipc_shm_release(&g_ipc, stale), -EINVAL, "stale release must be rejected");
    zassert_true(ipc_shm_is_valid(&g_ipc, current), "stale release must not affect current allocation");
    zassert_equal(ipc_shm_release(&g_ipc, current), 0, "current release failed");

    g_ipc.shm_pool.alloc_counter = 0xFFFFU;
    ipc_shm_handle_t wrapped = ipc_shm_alloc(&g_ipc, 32);
    zassert_not_equal(wrapped, IPC_SHM_HANDLE_INVALID, "allocation after 16-bit counter boundary failed");
    zassert_true(ipc_shm_is_valid(&g_ipc, wrapped), "handle after 16-bit counter boundary must remain valid");
    zassert_equal(ipc_shm_release(&g_ipc, wrapped), 0, "boundary handle release failed");

    ipc_service_stop(&g_ipc);
}

ZTEST(ipc_service, test_sync_shm_distinct_output_releases_input) {
    void*            out_data = NULL;
    size_t           out_size = 0;
    ipc_shm_handle_t out_handle = IPC_SHM_HANDLE_INVALID;
    int              r = ipc_service_init(&g_ipc, "shm_distinct", ipc_shm_distinct_output_handler, 5);

    zassert_equal(r, 0, "ipc_service_init failed: %d", r);
    zassert_equal(ipc_service_start(&g_ipc), 0, "ipc_service_start failed");

    ipc_shm_handle_t input_handle = ipc_shm_alloc(&g_ipc, 32);
    zassert_not_equal(input_handle, IPC_SHM_HANDLE_INVALID, "input allocation failed");
    void* input_data = ipc_shm_get_ptr(&g_ipc, input_handle);
    zassert_not_null(input_data, "input pointer missing");

    r = ipc_call_sync_shm(&g_ipc, input_data, 32, input_handle, &out_data, &out_size, &out_handle, K_SECONDS(2));
    zassert_equal(r, 0, "ipc_call_sync_shm failed: %d", r);
    zassert_not_equal(out_handle, IPC_SHM_HANDLE_INVALID, "output handle missing");
    zassert_not_equal(out_handle, input_handle, "output should use a distinct block");
    zassert_false(ipc_shm_is_valid(&g_ipc, input_handle), "transferred input should be released");
    zassert_mem_equal(out_data, "distinct_out", sizeof("distinct_out"), "unexpected output");
    zassert_equal(ipc_shm_release(&g_ipc, out_handle), 0, "output release failed");

    ipc_service_stop(&g_ipc);
}

/* 测试共享内存池统计 */
ZTEST(ipc_service, test_shared_memory_stats) {
    int      r;
    uint32_t active, peak, free;

    r = ipc_service_init(&g_ipc, "shm_stats", ipc_shm_alloc_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    /* 初始状态 */
    ipc_shm_get_stats(&g_ipc, &active, &peak, &free);
    zassert_equal(active, 0, "initial active should be 0");
    zassert_equal(peak, 0, "initial peak should be 0");
    zassert_equal(free, CONFIG_THREAD_IPC_SERVICE_SHARED_MEM_POOL_SIZE, "initial free should equal pool size");

    /* 分配几个块 */
    ipc_shm_handle_t h1 = ipc_shm_alloc(&g_ipc, 32);
    ipc_shm_handle_t h2 = ipc_shm_alloc(&g_ipc, 64);

    ipc_shm_get_stats(&g_ipc, &active, &peak, &free);
    zassert_equal(active, 2, "active should be 2");
    zassert_equal(peak, 2, "peak should be 2");

    /* 释放一个 */
    ipc_shm_release(&g_ipc, h1);
    ipc_shm_get_stats(&g_ipc, &active, &peak, &free);
    zassert_equal(active, 1, "active should be 1 after release");
    zassert_equal(peak, 2, "peak should remain 2");

    /* 释放另一个 */
    ipc_shm_release(&g_ipc, h2);

    ipc_service_stop(&g_ipc);
}

/* 测试共享内存池耗尽 */
ZTEST(ipc_service, test_shared_memory_exhaustion) {
    int              r;
    uint32_t         pool_size = CONFIG_THREAD_IPC_SERVICE_SHARED_MEM_POOL_SIZE;
    ipc_shm_handle_t handles[IPC_UT_SHM_HANDLE_MAX];

    /* 池必须能被句柄数组覆盖，否则"分配直到池满"循环无法触及耗尽边界 */
    BUILD_ASSERT(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM_POOL_SIZE <= IPC_UT_SHM_HANDLE_MAX,
                 "shm pool size must fit IPC_UT_SHM_HANDLE_MAX");

    r = ipc_service_init(&g_ipc, "shm_exhaust", ipc_shm_alloc_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    /* 分配直到池满 */
    uint32_t allocated = 0;
    for (uint32_t i = 0; i < pool_size && i < ARRAY_SIZE(handles); i++) {
        handles[i] = ipc_shm_alloc(&g_ipc, 32);
        if (handles[i] != IPC_SHM_HANDLE_INVALID) {
            allocated++;
        } else {
            break;
        }
    }

    zassert_true(allocated > 0, "should allocate at least one block");

    /* 尝试再分配一个，应该失败 */
    ipc_shm_handle_t extra = ipc_shm_alloc(&g_ipc, 32);
    /* 可能成功（如果池大小>handles 数组）或失败 */
    if (extra != IPC_SHM_HANDLE_INVALID) {
        ipc_shm_release(&g_ipc, extra);
    }

    /* 释放所有已分配的块 */
    for (uint32_t i = 0; i < allocated; i++) {
        r = ipc_shm_release(&g_ipc, handles[i]);
        zassert_equal(r, 0, "release failed at index %u: %d", i, r);
    }

    ipc_service_stop(&g_ipc);
}

/* 测试无效参数处理 */
ZTEST(ipc_service, test_shared_memory_invalid_params) {
    int r;

    r = ipc_service_init(&g_ipc, "shm_invalid", ipc_shm_alloc_handler, 5);
    zassert_equal(r, 0, "ipc_service_init failed: %d", r);

    /* NULL service */
    zassert_equal(ipc_shm_alloc(NULL, 32), IPC_SHM_HANDLE_INVALID, "alloc with NULL service should fail");
    zassert_false(ipc_shm_is_valid(NULL, 123), "is_valid with NULL service should return false");

    /* 过大尺寸 */
    ipc_shm_handle_t h = ipc_shm_alloc(&g_ipc, CONFIG_THREAD_IPC_SERVICE_SHARED_MEM_BLOCK_SIZE + 1);
    zassert_equal(h, IPC_SHM_HANDLE_INVALID, "alloc with oversized should fail");

    /* 零尺寸 */
    h = ipc_shm_alloc(&g_ipc, 0);
    zassert_equal(h, IPC_SHM_HANDLE_INVALID, "alloc zero size should fail");

    /* 无效句柄 */
    zassert_is_null(ipc_shm_get_ptr(&g_ipc, IPC_SHM_HANDLE_INVALID), "get_ptr invalid handle should return NULL");
    zassert_equal(ipc_shm_get_size(&g_ipc, IPC_SHM_HANDLE_INVALID), 0, "get_size invalid handle should return 0");
    zassert_equal(ipc_shm_release(&g_ipc, IPC_SHM_HANDLE_INVALID), -EINVAL, "release invalid handle should fail");

    ipc_service_stop(&g_ipc);
}
#endif /* CONFIG_THREAD_IPC_SERVICE_SHARED_MEM */

ZTEST_SUITE(ipc_service, NULL, test_suite_setup, NULL, NULL, test_suite_teardown);
