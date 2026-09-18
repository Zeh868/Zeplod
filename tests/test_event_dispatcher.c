/**
 * @file test_event_dispatcher.c
 * @brief event_dispatcher 单元测试（勿与 event_system_start 同时跑两套队列消费者，避免抢同一队列）
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            嵌套函数提取为文件级 static；publish_copy 返回值检查
 *
 */

#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include "ztest_sync.h"

LOG_MODULE_REGISTER(test_event_dispatcher);

#define DISPATCHER_STOP_FROM_CALLBACK_EVENT_TYPE 230U
#define DISPATCHER_SLOW_CALLBACK_EVENT_TYPE      231U
#define DISPATCHER_SLOW_FILTER_EVENT_TYPE        232U

static atomic_t       g_dispatcher_stop_from_callback_seen;
static event_status_t g_dispatcher_stop_from_callback_status;
static atomic_t       g_dispatcher_slow_callback_started;
static atomic_t       g_dispatcher_slow_callback_done;
static atomic_t       g_dispatcher_slow_filter_started;
static atomic_t       g_dispatcher_slow_filter_done;
static atomic_t       g_filter_call_count;
static atomic_t       g_filter_block_count;

static bool dispatcher_stats_processed_all(void* ctx) {
    dispatcher_stats_t* stats = ctx;

    event_dispatcher_get_stats(stats);
    return stats->events_processed >= 10ULL;
}

static bool filter_blocked_at_least_five(void* ctx) {
    ARG_UNUSED(ctx);
    return atomic_get(&g_filter_block_count) >= 5;
}

static void dispatcher_stop_from_callback_handler(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);

    g_dispatcher_stop_from_callback_status = event_dispatcher_stop();
    atomic_set(&g_dispatcher_stop_from_callback_seen, 1);
}

static void dispatcher_slow_callback_handler(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);

    atomic_set(&g_dispatcher_slow_callback_started, 1);
    k_msleep(EVENT_DISPATCHER_THREAD_JOIN_TIMEOUT_MS + 300U);
    atomic_set(&g_dispatcher_slow_callback_done, 1);
}

static bool dispatcher_slow_filter(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);

    atomic_set(&g_dispatcher_slow_filter_started, 1);
    k_msleep(EVENT_DISPATCHER_THREAD_JOIN_TIMEOUT_MS + 300U);
    atomic_set(&g_dispatcher_slow_filter_done, 1);
    return false;
}

/* 只允许 type=100 的事件通过（test_set_filter 用） */
static bool dispatcher_type100_filter(const event_t* event, void* user_data) {
    ARG_UNUSED(user_data);

    atomic_inc(&g_filter_call_count);
    return event->type == 100;
}

/* 总是拒绝的过滤器，用于统计 events_filtered（test_filter_block_all 用） */
static bool dispatcher_block_all_filter(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);

    atomic_inc(&g_filter_block_count);
    return false;
}

ZTEST(event_dispatcher, test_init_start_stop) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_STOPPED, NULL);

    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_RUNNING, NULL);

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_STOPPED, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief 多轮 init/start/stop/deinit 循环（重构护栏）
 */
ZTEST(event_dispatcher, test_lifecycle_start_stop_cycle) {
    for (int cycle = 0; cycle < 3; cycle++) {
        zassert_equal(event_system_init(), EVENT_OK, "cycle %d init", cycle);
        zassert_equal(event_system_start(), EVENT_OK, "cycle %d sys start", cycle);
        zassert_equal(event_dispatcher_init(NULL), EVENT_OK, "cycle %d disp init", cycle);
        zassert_equal(event_dispatcher_start(), EVENT_OK, "cycle %d disp start", cycle);
        zassert_equal(event_dispatcher_get_state(), DISPATCHER_RUNNING, NULL);
        zassert_equal(event_dispatcher_stop(), EVENT_OK, "cycle %d disp stop", cycle);
        zassert_equal(event_dispatcher_get_state(), DISPATCHER_STOPPED, NULL);
        zassert_equal(event_system_stop(), EVENT_OK, "cycle %d sys stop", cycle);
        zassert_equal(event_dispatcher_deinit(), EVENT_OK, "cycle %d disp deinit", cycle);
        zassert_equal(event_system_shutdown(), EVENT_OK, "cycle %d shutdown", cycle);
    }
}

ZTEST(event_dispatcher, test_stats_reset) {
    dispatcher_stats_t stats;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);
    event_dispatcher_reset_stats();
    event_dispatcher_get_stats(&stats);
    zassert_equal(stats.events_processed, 0ULL, NULL);
    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

ZTEST(event_dispatcher, test_pause_resume) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    zassert_equal(event_dispatcher_pause(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_PAUSED, NULL);

    zassert_equal(event_dispatcher_resume(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_RUNNING, NULL);

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

ZTEST(event_dispatcher, test_set_filter) {
    atomic_set(&g_filter_call_count, 0);

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(100, "filter_t100"), EVENT_OK, NULL);
    zassert_equal(event_register_type(101, "filter_t101"), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);

    event_dispatcher_set_filter(dispatcher_type100_filter, NULL);

    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    /* 发布不同类型的事件 */
    zassert_equal(event_publish_copy(100, EVENT_PRIORITY_NORMAL, "allowed", 7), EVENT_OK, NULL);
    zassert_equal(event_publish_copy(101, EVENT_PRIORITY_NORMAL, "blocked", 7), EVENT_OK, NULL);

    zassert_true(ztest_wait_atomic_nonzero(&g_filter_call_count, 2000U), "过滤器应被调用");
    zassert_true(atomic_get(&g_filter_call_count) > 0, "过滤器应被调用");

    event_dispatcher_clear_filter();
    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

ZTEST(event_dispatcher, test_process_one) {
    event_status_t status;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(200, "process_one_t200"), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    /* 不启动分发器线程，由本测试线程手动消费 */
    zassert_equal(event_publish_copy(200, EVENT_PRIORITY_NORMAL, "test", 4), EVENT_OK, NULL);

    status = event_dispatcher_process_one(K_MSEC(100));
    zassert_equal(status, EVENT_OK, "手动 process_one 应成功处理事件 (got %d)", status);

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

ZTEST(event_dispatcher, test_process_all) {
    uint32_t processed;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(201, "process_all_201"), EVENT_OK, NULL);
    zassert_equal(event_register_type(202, "process_all_202"), EVENT_OK, NULL);
    zassert_equal(event_register_type(203, "process_all_203"), EVENT_OK, NULL);
    zassert_equal(event_register_type(204, "process_all_204"), EVENT_OK, NULL);
    zassert_equal(event_register_type(205, "process_all_205"), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);

    for (int i = 0; i < 5; i++) {
        zassert_equal(event_publish_copy((event_type_t) (201 + i), EVENT_PRIORITY_NORMAL, "test", 4), EVENT_OK, NULL);
    }

    processed = event_dispatcher_process_all(0);
    zassert_equal(processed, 5U, "应处理全部 5 个事件");

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

ZTEST(event_dispatcher, test_get_current_latency) {
    uint32_t latency;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    /* 获取当前延迟（不应崩溃）*/
    latency = event_dispatcher_get_current_latency();
    /* 延迟值应该是一个合理的数值 */
    zassert_true(latency < 1000000, "延迟应小于 1 秒");

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

ZTEST(event_dispatcher, test_stats_comprehensive) {
    dispatcher_stats_t stats;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(50, "stats_t50"), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    /* 发布并处理一些事件 */
    for (int i = 0; i < 10; i++) {
        zassert_equal(event_publish_copy(50, EVENT_PRIORITY_NORMAL, "stats", 5), EVENT_OK, NULL);
    }

    zassert_true(ztest_wait_until(dispatcher_stats_processed_all, &stats, 2000U), "事件应已被分发处理");

    /* 获取统计 */
    event_dispatcher_get_stats(&stats);
    zassert_true(stats.events_processed <= 10, "处理事件数不应超过发布数");

    /* 重置统计 */
    event_dispatcher_reset_stats();
    event_dispatcher_get_stats(&stats);
    zassert_equal(stats.events_processed, 0ULL, "重置后处理计数应为 0");
    zassert_equal(stats.events_dropped, 0ULL, "重置后丢弃计数应为 0");
    zassert_equal(stats.events_filtered, 0ULL, "重置后过滤计数应为 0");
    zassert_equal(stats.processing_errors, 0ULL, "重置后错误计数应为 0");

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

ZTEST(event_dispatcher, test_custom_config) {
    dispatcher_config_t config = {.stack_size = 1024,
                                  .priority = 7,
                                  .thread_name = "test_disp",
                                  .enable_stats = true,
                                  .max_events_per_cycle = 50};

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);

    /* 使用自定义配置初始化 */
    zassert_equal(event_dispatcher_init(&config), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_RUNNING, NULL);

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief 测试无效配置参数 - 栈大小过小
 */
ZTEST(event_dispatcher, test_invalid_config_stack_too_small) {
    dispatcher_config_t config = {.stack_size = 128, /* 小于 EVENT_DISPATCHER_MIN_STACK_SIZE (256) */
                                  .priority = 5,
                                  .thread_name = "test_disp",
                                  .enable_stats = true,
                                  .max_events_per_cycle = 100};

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);

    zassert_equal(event_dispatcher_init(&config), EVENT_ERR_INVALID_ARG, "栈过小应返回错误");

    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief 测试无效配置参数 - 栈大小过大
 */
ZTEST(event_dispatcher, test_invalid_config_stack_too_large) {
    dispatcher_config_t config = {.stack_size = 128 * 1024, /* 超过 EVENT_DISPATCHER_MAX_STACK_SIZE (64KB) */
                                  .priority = 5,
                                  .thread_name = "test_disp",
                                  .enable_stats = true,
                                  .max_events_per_cycle = 100};

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);

    zassert_equal(event_dispatcher_init(&config), EVENT_ERR_INVALID_ARG, "栈过大应返回错误");

    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief 测试无效配置参数 - 优先级超出范围
 */
ZTEST(event_dispatcher, test_invalid_config_priority_out_of_range) {
    dispatcher_config_t config_low = {.stack_size = 1024,
                                      .priority = -1, /* 小于 EVENT_DISPATCHER_MIN_PRIORITY (0) */
                                      .thread_name = "test_disp",
                                      .enable_stats = true,
                                      .max_events_per_cycle = 100};

    dispatcher_config_t config_high = {.stack_size = 1024,
                                       .priority = 20, /* 大于 EVENT_DISPATCHER_MAX_PRIORITY (15) */
                                       .thread_name = "test_disp",
                                       .enable_stats = true,
                                       .max_events_per_cycle = 100};

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);

    zassert_equal(event_dispatcher_init(&config_low), EVENT_ERR_INVALID_ARG, "优先级过低应返回错误");
    zassert_equal(event_dispatcher_init(&config_high), EVENT_ERR_INVALID_ARG, "优先级过高应返回错误");

    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief 测试无效配置参数 - max_events_per_cycle 过大
 */
ZTEST(event_dispatcher, test_invalid_config_max_events_too_large) {
    dispatcher_config_t config = {
        .stack_size = 1024,
        .priority = 5,
        .thread_name = "test_disp",
        .enable_stats = true,
        .max_events_per_cycle = 20000 /* 超过 EVENT_DISPATCHER_MAX_EVENTS_PER_CYCLE (10000) */
    };

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);

    zassert_equal(event_dispatcher_init(&config), EVENT_ERR_INVALID_ARG, "max_events 过大应返回错误");

    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief 测试在 event_system_init 之前调用 dispatcher_init
 */
ZTEST(event_dispatcher, test_dispatcher_init_before_system_init) {
    /* 确保系统未初始化 */
    event_system_shutdown();

    zassert_equal(event_dispatcher_init(NULL), EVENT_ERR_INVALID_ARG, "未初始化系统时应返回错误");
}

/**
 * @brief 测试重复暂停
 */
ZTEST(event_dispatcher, test_double_pause) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    /* 第一次暂停 */
    zassert_equal(event_dispatcher_pause(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_PAUSED, NULL);

    /* 第二次暂停应失败 */
    zassert_equal(event_dispatcher_pause(), EVENT_ERR_INVALID_ARG, "重复暂停应返回错误");

    zassert_equal(event_dispatcher_resume(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief 测试重复恢复
 */
ZTEST(event_dispatcher, test_double_resume) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    /* 未暂停时恢复应失败 */
    zassert_equal(event_dispatcher_resume(), EVENT_ERR_INVALID_ARG, "未暂停时恢复应返回错误");

    /* 暂停后恢复 */
    zassert_equal(event_dispatcher_pause(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_resume(), EVENT_OK, NULL);

    /* 再次恢复应失败 */
    zassert_equal(event_dispatcher_resume(), EVENT_ERR_INVALID_ARG, "重复恢复应返回错误");

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief 测试暂停状态下 process_one
 */
ZTEST(event_dispatcher, test_process_one_when_paused) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    /* 暂停分发器 */
    zassert_equal(event_dispatcher_pause(), EVENT_OK, NULL);

    /* 暂停状态下 process_one 应返回错误 */
    zassert_equal(event_dispatcher_process_one(K_NO_WAIT), EVENT_ERR_INVALID_ARG, "暂停时 process_one 应返回错误");

    zassert_equal(event_dispatcher_resume(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief 测试停止状态下 process_one
 */
ZTEST(event_dispatcher, test_process_one_when_stopped) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);

    zassert_equal(event_dispatcher_process_one(K_NO_WAIT), EVENT_ERR_INVALID_ARG,
                  "start 后 stop 不得再手动 process_one");

    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief 仅 init、从未 start 时允许手动 process_one（空队列返回 EMPTY）
 */
ZTEST(event_dispatcher, test_process_one_manual_without_start) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);

    zassert_equal(event_dispatcher_process_one(K_NO_WAIT), EVENT_ERR_QUEUE_EMPTY,
                  "init 未 start 时可手动 process_one，空队列为 EMPTY");

    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief 分发器线程 RUNNING 时，其他线程不得调用 process_one
 */
ZTEST(event_dispatcher, test_process_one_rejected_while_dispatcher_running) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(206, "process_reject_t206"), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    zassert_equal(event_dispatcher_process_one(K_NO_WAIT), EVENT_ERR_INVALID_ARG,
                  "分发器 RUNNING 时外部线程不得 process_one");

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief 测试过滤器阻止所有事件
 */
ZTEST(event_dispatcher, test_filter_block_all) {
    atomic_set(&g_filter_block_count, 0);

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(210, "filter_block_t210"), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);

    event_dispatcher_set_filter(dispatcher_block_all_filter, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    /* 发布事件 */
    for (int i = 0; i < 5; i++) {
        zassert_equal(event_publish_copy(210, EVENT_PRIORITY_NORMAL, "test", 4), EVENT_OK, "publish %d", i);
    }

    zassert_true(ztest_wait_until(filter_blocked_at_least_five, NULL, 2000U), "事件应被过滤器阻止");
    zassert_true(atomic_get(&g_filter_block_count) >= 5, "事件应被过滤器阻止");

    event_dispatcher_clear_filter();
    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief 测试多次启动
 */
ZTEST(event_dispatcher, test_multiple_start) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);

    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_RUNNING, NULL);

    /* 重复启动应幂等 */
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_RUNNING, NULL);

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief 测试多次停止
 */
ZTEST(event_dispatcher, test_multiple_stop) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_STOPPED, NULL);

    /* 重复停止应幂等 */
    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_STOPPED, NULL);

    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief event_dispatcher_stop 不允许从分发器回调内部调用
 */
ZTEST(event_dispatcher, test_dispatcher_stop_rejected_from_callback) {
    uint32_t subscriber_id;

    atomic_set(&g_dispatcher_stop_from_callback_seen, 0);
    g_dispatcher_stop_from_callback_status = EVENT_OK;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(DISPATCHER_STOP_FROM_CALLBACK_EVENT_TYPE, "disp_stop_cb"), EVENT_OK, NULL);
    zassert_equal(event_subscribe(DISPATCHER_STOP_FROM_CALLBACK_EVENT_TYPE, dispatcher_stop_from_callback_handler, NULL,
                                  &subscriber_id),
                  EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    zassert_equal(event_publish_copy(DISPATCHER_STOP_FROM_CALLBACK_EVENT_TYPE, EVENT_PRIORITY_NORMAL, NULL, 0),
                  EVENT_OK, NULL);
    zassert_true(ztest_wait_atomic_nonzero(&g_dispatcher_stop_from_callback_seen, 2000U), "回调应已执行");

    zassert_equal(atomic_get(&g_dispatcher_stop_from_callback_seen), 1, "回调应已执行");
    zassert_equal(g_dispatcher_stop_from_callback_status, EVENT_ERR_INVALID_ARG, "回调内 stop 应被拒绝");
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_RUNNING, "被拒绝后分发器仍应运行");

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

/**
 * @brief 测试 process_all 带上限
 */
ZTEST(event_dispatcher, test_dispatcher_stop_timeout_does_not_abort_callback) {
    uint32_t subscriber_id;

    atomic_set(&g_dispatcher_slow_callback_started, 0);
    atomic_set(&g_dispatcher_slow_callback_done, 0);

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(DISPATCHER_SLOW_CALLBACK_EVENT_TYPE, "disp_slow_cb"), EVENT_OK, NULL);
    zassert_equal(
        event_subscribe(DISPATCHER_SLOW_CALLBACK_EVENT_TYPE, dispatcher_slow_callback_handler, NULL, &subscriber_id),
        EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    zassert_equal(event_publish_copy(DISPATCHER_SLOW_CALLBACK_EVENT_TYPE, EVENT_PRIORITY_NORMAL, NULL, 0), EVENT_OK,
                  NULL);

    zassert_true(ztest_wait_atomic_nonzero(&g_dispatcher_slow_callback_started, 2000U),
                 "slow callback should have started");
    zassert_equal(atomic_get(&g_dispatcher_slow_callback_started), 1, "slow callback should have started");

    zassert_equal(event_dispatcher_stop(), EVENT_ERR_TIMEOUT, "stop should time out while callback is still running");
    zassert_equal(atomic_get(&g_dispatcher_slow_callback_done), 0, "callback must not be aborted on stop timeout");

    zassert_true(
        ztest_wait_atomic_nonzero(&g_dispatcher_slow_callback_done, EVENT_DISPATCHER_THREAD_JOIN_TIMEOUT_MS + 500U),
        "callback should complete naturally");
    zassert_equal(atomic_get(&g_dispatcher_slow_callback_done), 1, "callback should complete naturally");
    zassert_equal(event_dispatcher_stop(), EVENT_OK, "second stop should join completed dispatcher thread");
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

ZTEST(event_dispatcher, test_event_system_stop_retries_dispatcher_join_after_timeout) {
    atomic_set(&g_dispatcher_slow_filter_started, 0);
    atomic_set(&g_dispatcher_slow_filter_done, 0);

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(DISPATCHER_SLOW_FILTER_EVENT_TYPE, "disp_slow_filter"), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    event_dispatcher_set_filter(dispatcher_slow_filter, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    zassert_equal(event_publish_copy(DISPATCHER_SLOW_FILTER_EVENT_TYPE, EVENT_PRIORITY_NORMAL, NULL, 0), EVENT_OK,
                  NULL);
    zassert_true(ztest_wait_atomic_nonzero(&g_dispatcher_slow_filter_started, 2000U),
                 "slow filter should have started");

    zassert_equal(event_system_stop(), EVENT_ERR_TIMEOUT, "first stop should time out while filter is running");
    zassert_false(event_system_is_running(), "system must reject new publishes after stop begins");

    zassert_true(
        ztest_wait_atomic_nonzero(&g_dispatcher_slow_filter_done, EVENT_DISPATCHER_THREAD_JOIN_TIMEOUT_MS + 500U),
        "slow filter should finish naturally");
    zassert_equal(event_system_stop(), EVENT_OK, "second stop should retry and complete dispatcher join");

    event_dispatcher_clear_filter();
    zassert_equal(event_system_start(), EVENT_OK, "system should be restartable after retrying stop");
    zassert_equal(event_dispatcher_get_state(), DISPATCHER_RUNNING, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

ZTEST(event_dispatcher, test_process_all_with_limit) {
    uint32_t processed;

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_register_type(220, "process_lim_220"), EVENT_OK, NULL);
    zassert_equal(event_register_type(221, "process_lim_221"), EVENT_OK, NULL);
    zassert_equal(event_register_type(222, "process_lim_222"), EVENT_OK, NULL);
    zassert_equal(event_register_type(223, "process_lim_223"), EVENT_OK, NULL);
    zassert_equal(event_register_type(224, "process_lim_224"), EVENT_OK, NULL);
    zassert_equal(event_register_type(225, "process_lim_225"), EVENT_OK, NULL);
    zassert_equal(event_register_type(226, "process_lim_226"), EVENT_OK, NULL);
    zassert_equal(event_register_type(227, "process_lim_227"), EVENT_OK, NULL);
    zassert_equal(event_register_type(228, "process_lim_228"), EVENT_OK, NULL);
    zassert_equal(event_register_type(229, "process_lim_229"), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);

    for (int i = 0; i < 10; i++) {
        zassert_equal(event_publish_copy((event_type_t) (220 + i), EVENT_PRIORITY_NORMAL, "test", 4), EVENT_OK, NULL);
    }

    processed = event_dispatcher_process_all(3);
    zassert_equal(processed, 3U, "应恰好处理 3 个事件");

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
}

static void event_dispatcher_after_each(void* fixture) {
    (void) fixture;
    /* 完整 shutdown：清空已注册事件类型与分发器状态。仅 stop 不会清理类型表，
     * 否则本套件注册的
     * type（如 100/101/210）会污染后续 test_event_system 等套件。 */
    (void) event_system_shutdown();
}

ZTEST_SUITE(event_dispatcher, NULL, NULL, NULL, event_dispatcher_after_each, NULL);
