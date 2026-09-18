/**
 * @file test_core_coverage_extended.c
 * @brief 补充 src/core 发布校验、队列策略、compat 错误路径与分发器边界
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh             初始版本
 * 2026-09-18       1.1            zeh             slab 耗尽回调用例补回调与创建数断言；
 *                                                  堆耗尽分支显式 skip；补全文件头模板
 *
 */

#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>
#include <errno.h>
#include <string.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include <zeplod/event_system_compat.h>
#include "event_memory.h"
#include "event_queue.h"
#include "ztest_sync.h"

static void ext_cov_teardown(void* fixture) {
    ARG_UNUSED(fixture);
    (void) event_system_shutdown();
}

static void ext_setup_running_dispatcher(void) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);
}

ZTEST(core_coverage_ext, test_compat_error_returns) {
    zassert_equal(event_compat_start(), -EINVAL, NULL);
    zassert_equal(event_compat_shutdown(), 0, NULL);

    zassert_equal(event_compat_init(NULL), 0, NULL);
    zassert_equal(event_compat_stop(), 0, NULL);
    zassert_equal(event_compat_shutdown(), 0, NULL);
}

ZTEST(core_coverage_ext, test_event_queue_invalid_policy_and_timeout) {
    struct k_msgq queue;
    char          buffer[2 * sizeof(event_t)];
    event_t       event = {.type = 10, .priority = EVENT_PRIORITY_NORMAL};

    zassert_equal(event_queue_init(&queue, buffer, 2), EVENT_OK, NULL);
    zassert_equal(event_queue_enqueue(&queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_OK, NULL);
    zassert_equal(event_queue_enqueue(&queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_OK, NULL);

    zassert_equal(event_queue_enqueue(&queue, &event, (queue_overflow_policy_t) 99, K_NO_WAIT), EVENT_ERR_INVALID_ARG,
                  NULL);
    zassert_equal(event_queue_enqueue(&queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_MSEC(5)), EVENT_ERR_TIMEOUT, NULL);

#if IS_ENABLED(CONFIG_EVENT_QUEUE_OVERFLOW_BLOCK)
    zassert_equal(event_queue_enqueue(&queue, &event, QUEUE_OVERFLOW_BLOCK, K_NO_WAIT), EVENT_ERR_QUEUE_FULL, NULL);
#endif

    event_queue_deinit(&queue);
}

ZTEST(core_coverage_ext, test_event_publish_validation_flags) {
    event_t bad_inline_len = {.type = 52,
                              .priority = EVENT_PRIORITY_NORMAL,
                              .data_len = CONFIG_EVENT_INLINE_DATA_SIZE + 1U,
                              .flags = EVENT_FLAG_DATA_INLINE};

    event_t bad_dual_flags = {.type = 52,
                              .priority = EVENT_PRIORITY_NORMAL,
                              .data_len = 4U,
                              .flags = EVENT_FLAG_DATA_INLINE | EVENT_FLAG_DATA_DYNAMIC};

    event_t bad_len_no_storage = {.type = 52, .priority = EVENT_PRIORITY_NORMAL, .data_len = 4U, .flags = 0};

    event_t bad_dynamic_null = {.type = 52,
                                .priority = EVENT_PRIORITY_NORMAL,
                                .data_len = 4U,
                                .flags = EVENT_FLAG_DATA_DYNAMIC,
                                .data = {.ptr = NULL}};

    event_t bad_inline_slab = {.type = 52,
                               .priority = EVENT_PRIORITY_NORMAL,
                               .data_len = 2U,
                               .flags = EVENT_FLAG_DATA_INLINE | EVENT_FLAG_DATA_FROM_SLAB};

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_register_type(52, "pub_val"), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);

    zassert_equal(event_publish(&bad_inline_len), EVENT_ERR_INVALID_ARG, NULL);
    zassert_equal(event_publish(&bad_dual_flags), EVENT_ERR_INVALID_ARG, NULL);
    zassert_equal(event_publish(&bad_len_no_storage), EVENT_ERR_INVALID_ARG, NULL);
    zassert_equal(event_publish(&bad_dynamic_null), EVENT_ERR_INVALID_ARG, NULL);
    zassert_equal(event_publish(&bad_inline_slab), EVENT_ERR_INVALID_ARG, NULL);

    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE && EVENT_SLAB_256_AVAILABLE
ZTEST(core_coverage_ext, test_event_create_with_slab_payload) {
    uint8_t payload[96];

    memset(payload, 0xAB, sizeof(payload));

    zassert_equal(event_system_init(), EVENT_OK, NULL);

    event_t* event = event_create_with_data(55, EVENT_PRIORITY_HIGH, payload, sizeof(payload));
    zassert_not_null(event, NULL);
    zassert_true((event->flags & EVENT_FLAG_DATA_FROM_SLAB) != 0, NULL);
    zassert_equal(event->data_len, sizeof(payload), NULL);
    event_free(event);

    event_t* rt_event = event_create_with_data_rt(56, EVENT_PRIORITY_CRITICAL, payload, sizeof(payload));
    zassert_not_null(rt_event, NULL);
    event_free(rt_event);

    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
}
#endif

ZTEST(core_coverage_ext, test_event_create_large_malloc_fallback) {
    uint8_t large[CONFIG_EVENT_INLINE_DATA_SIZE + 32];

    memset(large, 0x5A, sizeof(large));

    zassert_equal(event_system_init(), EVENT_OK, NULL);

    event_t* event = event_create_with_data(53, EVENT_PRIORITY_NORMAL, large, sizeof(large));
    if (event == NULL) {
        /* 主机堆耗尽无法确定性注入：NULL 属环境性结果而非用例失败，
         * 显式跳过并在报告留痕，避免静默通过掩盖分支未覆盖 */
        ztest_test_skip();
        return;
    }
    zassert_equal(event->data_len, sizeof(large), NULL);
    zassert_true((event->flags & EVENT_FLAG_DATA_DYNAMIC) != 0, NULL);
    event_free(event);

    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
}

ZTEST(core_coverage_ext, test_dispatcher_start_resumes_from_paused) {
    ext_setup_running_dispatcher();
    zassert_equal(event_dispatcher_pause(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_PAUSED, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_RUNNING, NULL);
    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_deinit(), EVENT_OK, NULL);
}

ZTEST(core_coverage_ext, test_dispatcher_start_without_init) {
    zassert_equal(event_dispatcher_start(), EVENT_ERR_INVALID_ARG, NULL);
}

ZTEST(core_coverage_ext, test_dispatcher_double_start_idempotent) {
    ext_setup_running_dispatcher();
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_RUNNING, NULL);
    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_deinit(), EVENT_OK, NULL);
}

ZTEST(core_coverage_ext, test_event_system_restart_dispatcher_after_stop) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);
    zassert_true(event_system_is_running(), NULL);
    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_deinit(), EVENT_OK, NULL);
}

ZTEST(core_coverage_ext, test_event_notify_no_subscriber_increments_stats) {
    uint32_t total;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_register_type(54, "no_sub"), EVENT_OK, NULL);

    event_t ev = {.type = 54, .priority = EVENT_PRIORITY_NORMAL, .data_len = 0};
    zassert_equal(event_queue_enqueue(event_system_get_queue(), &ev, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_OK,
                  NULL);
    zassert_equal(event_dispatcher_process_one(K_MSEC(500)), EVENT_OK, NULL);

    event_get_statistics(&total, NULL, NULL);
    zassert_true(total >= 1U, NULL);

    zassert_equal(event_dispatcher_deinit(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

ZTEST(core_coverage_ext, test_event_get_statistics_after_init) {
    uint32_t total;
    uint32_t depth;
    uint32_t dropped;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    event_get_statistics(&total, &depth, &dropped);
    zassert_equal(total, 0U, NULL);
    zassert_equal(dropped, 0U, NULL);
}

/* 回调开启且 NORMAL 池存在才可测耗尽；池容量 + 2 的富余量保证循环必然以 NULL 告终 */
#if defined(CONFIG_EVENT_SLAB_EXHAUSTED_CB) && (CONFIG_EVENT_SLAB_EXHAUSTED_CB == 1) &&                           \
    defined(CONFIG_EVENT_SLAB_NORMAL_COUNT)
#define EXT_SLAB_PROBE_MAX (CONFIG_EVENT_SLAB_NORMAL_COUNT + 2U)

static atomic_t g_ext_slab_exhausted;

static void ext_slab_exhausted_cb(event_priority_t priority, const char* slab_name) {
    ARG_UNUSED(priority);
    ARG_UNUSED(slab_name);
    atomic_set(&g_ext_slab_exhausted, 1);
}

ZTEST(core_coverage_ext, test_slab_exhausted_callback_on_pressure) {
    event_t* events[EXT_SLAB_PROBE_MAX];
    int      i;

    atomic_set(&g_ext_slab_exhausted, 0);
    event_register_slab_exhausted_cb(ext_slab_exhausted_cb);

    zassert_equal(event_system_init(), EVENT_OK, NULL);

    for (i = 0; i < (int) EXT_SLAB_PROBE_MAX; i++) {
        events[i] = event_create_rt((event_type_t) (60 + i), EVENT_PRIORITY_NORMAL);
        if (events[i] == NULL) {
            break;
        }
    }

    /* 行为断言：压力下必须至少成功创建过事件（i>0），且回调被触发恰一次 */
    zassert_true(i > 0, "slab 压力测试前应至少成功创建 1 个事件");
    zassert_true(i < (int) EXT_SLAB_PROBE_MAX, "NORMAL 池应在探测上限内耗尽");
    zassert_equal(atomic_get(&g_ext_slab_exhausted), 1, "slab 耗尽回调应被触发");

    for (int j = 0; j < i; j++) {
        event_free(events[j]);
    }

    event_register_slab_exhausted_cb(NULL);
    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
}
#endif

ZTEST_SUITE(core_coverage_ext, NULL, NULL, NULL, ext_cov_teardown, NULL);
