/**
 * @file test_core_coverage.c
 * @brief 补充 src/core 边界路径与统计/锁顺序覆盖
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            首用例补 shutdown 兜底；新增 publish 级 DROP_LOWEST 防复发用例（配置门控）
 */

#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>
#include <errno.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include <zeplod/event_system_compat.h>
#include <zeplod/lock_order.h>
#include <zeplod/state_machine.h>
#include "event_queue.h"
#include "event_system_internal.h"
#include "ztest_sync.h"

static atomic_t g_cov_stats_dispatched;

static void cov_stats_handler(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
    atomic_set(&g_cov_stats_dispatched, 1);
}

static void core_cov_teardown(void* fixture) {
    ARG_UNUSED(fixture);
    zepl_lock_reset_current_thread();
    (void) event_system_shutdown();
}

ZTEST(core_coverage, test_event_system_get_queue_uninitialized) {
    /* 兜底 shutdown：本用例依赖"系统未初始化"前置，不再隐式依赖前序套件的 teardown */
    (void) event_system_shutdown();
    zassert_is_null(event_system_get_queue(), NULL);
}

ZTEST(core_coverage, test_event_statistics_before_init_and_reset) {
    uint32_t total = 1U;
    uint32_t depth = 1U;
    uint32_t dropped = 1U;

    event_get_statistics(&total, &depth, &dropped);
    zassert_equal(total, 0U, NULL);
    zassert_equal(depth, 0U, NULL);
    zassert_equal(dropped, 0U, NULL);

    event_system_reset_statistics();
}

ZTEST(core_coverage, test_event_statistics_with_publish_and_reset) {
    uint32_t total;
    uint32_t depth;
    uint32_t dropped;
    uint32_t subscriber_id;

    atomic_set(&g_cov_stats_dispatched, 0);

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(50U, "cov_stats"), EVENT_OK, NULL);
    zassert_equal(event_subscribe(50U, cov_stats_handler, NULL, &subscriber_id), EVENT_OK, NULL);
    zassert_equal(event_publish_copy(50U, EVENT_PRIORITY_NORMAL, NULL, 0), EVENT_OK, NULL);
    zassert_true(ztest_wait_atomic_nonzero(&g_cov_stats_dispatched, 2000U), "事件应被分发");

    event_get_statistics(&total, &depth, &dropped);
    zassert_true(total >= 1U, NULL);
    zassert_true(depth <= CONFIG_EVENT_QUEUE_SIZE, NULL);

    event_system_reset_statistics();
    event_get_statistics(&total, &depth, &dropped);
    zassert_equal(total, 0U, NULL);
    zassert_equal(dropped, 0U, NULL);

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_deinit(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

ZTEST(core_coverage, test_lock_order_global_resource_and_tokens) {
    zepl_lock_token_t tok;

    zepl_lock_reset_current_thread();

    zassert_true(zepl_lock_order_is_valid(ZEP_LOCK_LEVEL_GLOBAL, 0x10U), NULL);
    zepl_lock_enter(ZEP_LOCK_LEVEL_GLOBAL, 0x10U);
    zassert_equal(zepl_lock_current_level(), ZEP_LOCK_LEVEL_GLOBAL, NULL);
    zassert_equal(zepl_lock_current_key(), 0x10U, NULL);

    zassert_true(zepl_lock_order_is_valid(ZEP_LOCK_LEVEL_RESOURCE, 0x20U), NULL);
    zepl_lock_enter_token((zepl_lock_token_t) {.level = ZEP_LOCK_LEVEL_RESOURCE, .key = 0x20U});
    zassert_equal(zepl_lock_current_depth(), 2U, NULL);

    tok = zepl_lock_current_token();
    zassert_equal(tok.level, ZEP_LOCK_LEVEL_RESOURCE, NULL);
    zassert_equal(tok.key, 0x20U, NULL);

    zepl_lock_exit_token((zepl_lock_token_t) {.level = ZEP_LOCK_LEVEL_RESOURCE, .key = 0x20U});
    zepl_lock_exit(ZEP_LOCK_LEVEL_GLOBAL, 0x10U);
    zassert_equal(zepl_lock_current_depth(), 0U, NULL);
}

ZTEST(core_coverage, test_lock_order_invalid_level_and_mismatched_exit) {
    zepl_lock_reset_current_thread();

    zassert_false(zepl_lock_order_is_valid((zepl_lock_level_t) 0U, 0U), NULL);
    zepl_lock_enter((zepl_lock_level_t) 0U, 0U);
    zassert_equal(zepl_lock_current_depth(), 0U, NULL);

    zepl_lock_enter(ZEP_LOCK_LEVEL_STATE, 0x100U);
    zepl_lock_exit(ZEP_LOCK_LEVEL_TABLE, 0x100U);
    zassert_equal(zepl_lock_current_depth(), 1U, NULL);
    zepl_lock_exit(ZEP_LOCK_LEVEL_STATE, 0x100U);
}

ZTEST(core_coverage, test_state_machine_error_recovery_and_null) {
    zepl_state_machine_t machine;

    zassert_equal(zepl_state_machine_get(NULL), ZEP_STATE_ERROR, NULL);
    zassert_is_null(zepl_state_machine_get_backend_context(NULL), NULL);

    zepl_state_machine_init_with_backend(NULL, ZEP_STATE_UNINIT, NULL, NULL);
    zassert_equal(zepl_state_machine_try_transition(NULL, ZEP_STATE_RUNNING), -EINVAL, NULL);

    zepl_state_machine_init(&machine, ZEP_STATE_RUNNING);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_ERROR), 0, NULL);
    zassert_true(zepl_state_machine_is_terminal(&machine, ZEP_STATE_ERROR), NULL);
    zassert_str_equal(zepl_state_machine_get_name(&machine, ZEP_STATE_ERROR), "ERROR", NULL);
    zassert_true(zepl_state_machine_can_transition(&machine, ZEP_STATE_ERROR, ZEP_STATE_STOPPED), NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_STOPPED), 0, NULL);

    zepl_state_machine_init(&machine, ZEP_STATE_INITED);
    zassert_true(zepl_state_can_transition(ZEP_STATE_INITED, ZEP_STATE_STOPPING), NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_STOPPING), 0, NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_STOPPED), 0, NULL);
    zassert_true(zepl_state_can_transition(ZEP_STATE_STOPPED, ZEP_STATE_STARTING), NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_STARTING), 0, NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_RUNNING), 0, NULL);

    zepl_state_machine_init(&machine, ZEP_STATE_STARTING);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_STOPPING), 0, NULL);

    zepl_state_machine_init(&machine, ZEP_STATE_UNINIT);
    zassert_false(zepl_state_can_transition(ZEP_STATE_UNINIT, ZEP_STATE_RUNNING), NULL);
    zassert_str_equal(zepl_state_name((zepl_state_t) 99), "UNKNOWN", NULL);

    zassert_equal(zepl_state_machine_set_backend(&machine, NULL, NULL), 0, NULL);
    zassert_not_null(zepl_state_machine_get_backend(&machine), NULL);
}

ZTEST(core_coverage, test_state_machine_same_level_key_ordering) {
    zepl_state_machine_t machine;

    zepl_state_machine_init(&machine, ZEP_STATE_RUNNING);
    zassert_true(zepl_state_can_transition(ZEP_STATE_RUNNING, ZEP_STATE_RUNNING), NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_RUNNING), 0, NULL);
}

ZTEST(core_coverage, test_event_queue_null_and_stats_edges) {
    struct k_msgq queue;
    char          buffer[2 * sizeof(event_t)];
    queue_stats_t stats;
    event_t       event = {.type = 1, .priority = EVENT_PRIORITY_NORMAL};

    zassert_equal(event_queue_depth(NULL), 0U, NULL);
    zassert_false(event_queue_is_full(NULL), NULL);
    event_queue_get_stats(NULL, &stats);
    event_queue_reset_stats(NULL);

    zassert_equal(event_queue_init(&queue, buffer, 2), EVENT_OK, NULL);
    zassert_equal(event_queue_enqueue(&queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_OK, NULL);
    zassert_equal(event_queue_enqueue(&queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_OK, NULL);
    zassert_equal(event_queue_enqueue(&queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_ERR_QUEUE_FULL,
                  NULL);

    event_queue_get_stats(&queue, &stats);
    zassert_equal(stats.overflow_count, 1U, NULL);
    event_queue_reset_stats(&queue);
    event_queue_get_stats(&queue, &stats);
    zassert_equal(stats.enqueue_count, 0U, NULL);

    event_queue_deinit(&queue);
}

ZTEST(core_coverage, test_event_get_statistics_partial_null) {
    uint32_t total = 99U;
    uint32_t depth = 99U;
    uint32_t dropped = 99U;

    event_get_statistics(&total, NULL, NULL);
    zassert_equal(total, 0U, NULL);

    event_get_statistics(NULL, &depth, NULL);
    zassert_equal(depth, 0U, NULL);

    event_get_statistics(NULL, NULL, &dropped);
    zassert_equal(dropped, 0U, NULL);
}

ZTEST(core_coverage, test_event_system_lifecycle_edges) {
    zassert_equal(event_system_start(), EVENT_ERR_INVALID_ARG, NULL);
    zassert_false(event_system_is_running(), NULL);

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_not_null(event_system_get_queue(), NULL);
    zassert_equal(event_system_init(), EVENT_OK, NULL);

    zassert_equal(event_system_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
    zassert_is_null(event_system_get_queue(), NULL);
}

ZTEST(core_coverage, test_event_queue_purge_and_capacity) {
    struct k_msgq queue;
    char          buffer[2 * sizeof(event_t)];
    event_t       event = {.type = 2, .priority = EVENT_PRIORITY_NORMAL};
    queue_stats_t stats;

    zassert_equal(event_queue_init(&queue, buffer, 2), EVENT_OK, NULL);
    zassert_equal(event_queue_capacity(&queue), 2U, NULL);

    zassert_equal(event_queue_enqueue(&queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_OK, NULL);
    zassert_equal(event_queue_enqueue(&queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_OK, NULL);
    zassert_true(event_queue_is_full(&queue), NULL);
    zassert_equal(event_queue_depth(&queue), 2U, NULL);
    zassert_false(event_queue_is_empty(&queue), NULL);

    event_queue_purge(&queue);
    zassert_true(event_queue_is_empty(&queue), NULL);
    zassert_equal(event_queue_depth(&queue), 0U, NULL);

    event_queue_get_stats(&queue, &stats);
    zassert_true(stats.drop_count >= 2U, NULL);

    event_queue_deinit(&queue);
}

ZTEST(core_coverage, test_event_queue_purge_unregistered_msgq) {
    struct k_msgq raw_queue;
    char          buffer[sizeof(event_t)];
    event_t       event_in = {.type = 3, .priority = EVENT_PRIORITY_NORMAL};

    k_msgq_init(&raw_queue, buffer, sizeof(event_t), 1);
    zassert_equal(k_msgq_put(&raw_queue, &event_in, K_NO_WAIT), 0, NULL);
    event_queue_purge(&raw_queue);
    zassert_equal(k_msgq_num_used_get(&raw_queue), 0U, NULL);
}

ZTEST(core_coverage, test_lock_order_exit_violation_and_reset) {
    zepl_lock_reset_current_thread();
    zepl_lock_enter(ZEP_LOCK_LEVEL_STATE, 0x1U);
    zepl_lock_exit(ZEP_LOCK_LEVEL_TABLE, 0x1U);
    zassert_equal(zepl_lock_current_depth(), 1U, NULL);
    zepl_lock_reset_current_thread();
    zassert_equal(zepl_lock_current_depth(), 0U, NULL);
}

ZTEST(core_coverage, test_state_machine_error_and_uninit_paths) {
    zepl_state_machine_t machine;

    zepl_state_machine_init(&machine, ZEP_STATE_INITED);
    zassert_true(zepl_state_can_transition(ZEP_STATE_INITED, ZEP_STATE_UNINIT), NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_UNINIT), 0, NULL);
    zassert_true(zepl_state_is_terminal(ZEP_STATE_ERROR), NULL);
    zassert_str_equal(zepl_state_name(ZEP_STATE_INITED), "INITED", NULL);

    zepl_state_machine_init(&machine, ZEP_STATE_ERROR);
    zassert_true(zepl_state_can_transition(ZEP_STATE_ERROR, ZEP_STATE_INITED), NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_INITED), 0, NULL);

    zassert_not_null(zepl_state_machine_default_backend(), NULL);
    zassert_not_null(zepl_state_machine_get_backend(NULL), NULL);
}

ZTEST(core_coverage, test_dispatcher_idle_time_and_dropped_stats) {
    dispatcher_stats_t stats;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    (void) event_dispatcher_get_idle_time_us();
    (void) event_dispatcher_get_current_latency();

    event_dispatcher_stats_inc_dropped();
    event_dispatcher_get_stats(&stats);
    zassert_true(stats.events_dropped >= 1U, NULL);

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_deinit(), EVENT_OK, NULL);
}

#if IS_ENABLED(CONFIG_EVENT_QUEUE_OVERFLOW_DROP_LOWEST)
/**
 * @brief 防复发：DROP_LOWEST 打开时，publish 级满队列应挤掉最低优先级并正确释放被挤事件
 *
 * 被挤事件带堆回退负载（64B > 内联 48B）：挤占路径经 event_free_queued_payload
 * 释放其 DYNAMIC 负载；释放正确性以"不崩溃 + drop 恰好 +1 + 队列内容逐条校验"验证。
 */
ZTEST(core_coverage, test_publish_drop_lowest_evicts_lowest_when_full) {
    struct k_msgq* q;
    queue_stats_t  stats;
    event_t        filler = {.type = 60U, .priority = EVENT_PRIORITY_LOW, .data_len = 0};
    event_t        out;
    uint32_t       drop_before;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(60U, "cov_drop_lowest"), EVENT_OK, NULL);

    q = event_system_get_queue();
    zassert_not_null(q, NULL);
    event_queue_get_stats(q, &stats);
    drop_before = stats.drop_count;

    /* 队首放一条带堆负载的 LOW 事件：它是最旧的 LOW，将被挤占并触发负载释放路径 */
    uint8_t payload[64] = {0};

    zassert_equal(event_publish_copy(60U, EVENT_PRIORITY_LOW, payload, sizeof(payload)), EVENT_OK, NULL);

    /* 用栈上事件（data_len=0，无堆分配）填满队列剩余容量 */
    uint32_t cap = event_queue_capacity(q);

    for (uint32_t i = 0; i < (cap - 1U); i++) {
        zassert_equal(event_publish(&filler), EVENT_OK, "publish %u", i);
    }
    zassert_true(event_queue_is_full(q), NULL);

    /* 高优先级事件应挤掉队首 LOW 事件并成功入队 */
    zassert_equal(event_publish_copy(60U, EVENT_PRIORITY_HIGH, NULL, 0), EVENT_OK,
                  "满队列上高优先级 publish 应挤掉 LOW 成功入队");
    zassert_equal(event_queue_depth(q), cap, "挤占后队列应仍为满");

    event_queue_get_stats(q, &stats);
    zassert_equal(stats.drop_count, drop_before + 1U, "应恰好挤掉 1 条");

    /* 逐条出队验证：被挤的是最旧 LOW，HIGH 在队尾 */
    zassert_equal(event_queue_dequeue(q, &out, K_NO_WAIT), EVENT_OK, NULL);
    zassert_equal(out.priority, EVENT_PRIORITY_LOW, "残留首条应为未被挤掉的 LOW");
    zassert_equal(out.data_len, 0U, "被挤事件的负载不应残留到后续事件");
    for (uint32_t i = 1U; i < (cap - 1U); i++) {
        zassert_equal(event_queue_dequeue(q, &out, K_NO_WAIT), EVENT_OK, "dequeue %u", i);
        zassert_equal(out.priority, EVENT_PRIORITY_LOW, "中间条目应保持 LOW");
    }
    zassert_equal(event_queue_dequeue(q, &out, K_NO_WAIT), EVENT_OK, NULL);
    zassert_equal(out.priority, EVENT_PRIORITY_HIGH, "队尾应为挤入的 HIGH");
    zassert_true(event_queue_is_empty(q), NULL);
}
#else
/* 默认测试配置（tests/prj.conf）未启用 DROP_LOWEST：队列级挤占语义已由
 * test_event_queue.c 的运行时策略用例无条件覆盖，publish 级接线用例仅在打开
 * 该配置时编译；此处显式 skip，避免"静默少测"。 */
ZTEST(core_coverage, test_publish_drop_lowest_evicts_lowest_when_full) {
    ztest_test_skip();
}
#endif /* CONFIG_EVENT_QUEUE_OVERFLOW_DROP_LOWEST */

ZTEST_SUITE(core_coverage, NULL, NULL, NULL, core_cov_teardown, NULL);
