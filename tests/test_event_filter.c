/**
 * @file test_event_filter.c
 * @brief 订阅级过滤（CONFIG_EVENT_SUBSCRIBER_FILTER）单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-09-27
 *
 * 仅当 CONFIG_EVENT_SUBSCRIBER_FILTER=y 时由 tests/CMakeLists.txt 编入；
 * 运行需叠加 tests/prj_event_ext.conf（CONF_FILE 三段叠加）。
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>
#include <string.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include "event_system_internal.h"
#include "test_event_stubs.h"
#include "ztest_sync.h"

LOG_MODULE_REGISTER(test_event_filter);

/* 本套件专用事件类型（各用例先 shutdown 再注册，避免与其他套件残留注册冲突） */
#define FILTER_TEST_TYPE_A 205U
#define FILTER_TEST_TYPE_B 206U

/** 并发热换用例的发布总数（奇偶交替两种载荷） */
#define FILTER_PAIR_PUBLISHED 40U

/** STATS=n 静默判定轮数（1ms 轮询间隔下约 300ms 无新交付即视为排空） */
#define FILTER_PAIR_QUIET_POLLS 300U

/** 回调/过滤上下文魔数：ud 内嵌魔数校验配对一致（串线/撕裂即计数异常） */
#define FILTER_CTX_MAGIC_A 0xA11CE00AU
#define FILTER_CTX_MAGIC_B 0xB0B0F00BU
#define FILTER_CB_MAGIC    0xC0FFE00CU

typedef struct {
    uint32_t magic;        /**< 上下文魔数（A/B 区分热换两侧） */
    uint32_t tag;          /**< 放行条件：事件 payload == tag */
    atomic_t filter_calls; /**< 过滤函数被调用次数 */
    atomic_t filter_ok;    /**< 过滤放行次数 */
} filter_ctx_t;

typedef struct {
    uint32_t magic;     /**< 回调上下文魔数（FILTER_CB_MAGIC） */
    atomic_t cb_calls;  /**< 回调被调用次数 */
} cb_sink_t;

static cb_sink_t g_sink = {.magic = FILTER_CB_MAGIC};
static atomic_t  g_filter_anomalies;   /**< 回调/过滤内检测到的配对异常（用例末尾断言为 0） */
static atomic_t  g_global_filter_calls; /**< 全局分发器过滤被调用次数（叠加顺序用例） */

/* =============================================================================
 * 测试辅助
 * ============================================================================= */

/** @brief 回到已知初始态：shutdown → init → dispatcher init → 统计复位 → start；
 *         ztest 实际执行顺序与声明顺序无关，用例间共享的计数器必须在此清零 */
static void filter_test_reset(void) {
    zassert_equal(event_system_shutdown(), EVENT_OK, "shutdown failed");
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    event_dispatcher_reset_stats();
    atomic_set(&g_sink.cb_calls, 0);
    atomic_set(&g_global_filter_calls, 0);
    atomic_set(&g_filter_anomalies, 0);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
}

/** @brief 发布一条内联 uint32 载荷事件（分发由用例手动 process_one 驱动） */
static void filter_test_publish(uint8_t type, uint32_t value) {
    event_t event = {0};

    event.type = type;
    event.priority = EVENT_PRIORITY_NORMAL;
    event.timestamp = k_uptime_get_32();
    event.flags = EVENT_FLAG_DATA_INLINE;
    event.data_len = sizeof(value);
    memcpy(event.data.inline_data, &value, sizeof(value));

    zassert_equal(event_publish(&event), EVENT_OK, "publish value=%u failed", value);
}

static uint32_t filter_test_payload(const event_t* event) {
    uint32_t value = 0U;

    memcpy(&value, event->data.inline_data, sizeof(value));
    return value;
}

/** @brief 手动消费 count 条事件，并断言队列随后已空 */
static void filter_test_drain(uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        zassert_equal(event_dispatcher_process_one(K_NO_WAIT), EVENT_OK, "process_one #%u failed", i);
    }
    zassert_equal(event_dispatcher_process_one(K_NO_WAIT), EVENT_ERR_QUEUE_EMPTY, "queue should be empty");
}

/** @brief 按载荷过滤：payload == tag 放行 */
static bool filter_by_tag(const event_t* event, void* filter_user_data) {
    filter_ctx_t* ctx = (filter_ctx_t*) filter_user_data;

    if (ctx == NULL) {
        atomic_inc(&g_filter_anomalies);
        return false;
    }
    atomic_inc(&ctx->filter_calls);
    if (filter_test_payload(event) == ctx->tag) {
        atomic_inc(&ctx->filter_ok);
        return true;
    }
    return false;
}

/** @brief 全放行过滤（占槽用） */
static bool filter_allpass(const event_t* event, void* filter_user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(filter_user_data);
    return true;
}

/** @brief 全局分发器过滤：计数并拒绝（叠加顺序用例） */
static bool filter_global_reject(const event_t* event, void* filter_user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(filter_user_data);
    atomic_inc(&g_global_filter_calls);
    return false;
}

static void filter_cb_count(const event_t* event, void* user_data) {
    cb_sink_t* sink = (cb_sink_t*) user_data;

    if (sink == NULL || sink->magic != FILTER_CB_MAGIC) {
        atomic_inc(&g_filter_anomalies);
        return;
    }
    ARG_UNUSED(event);
    atomic_inc(&sink->cb_calls);
}

/* =============================================================================
 * 基础语义用例
 * ============================================================================= */

/**
 * @brief 匹配载荷收到、不匹配载荷跳过（负测）
 */
ZTEST(test_event_filter, test_filter_match_and_skip) {
    filter_ctx_t ctx = {.magic = FILTER_CTX_MAGIC_A, .tag = 1U};
    uint32_t     sub_id = 0U;
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    uint64_t skip_before;
#endif

    filter_test_reset();
    zassert_equal(event_register_type(FILTER_TEST_TYPE_A, "flt_a"), EVENT_OK, NULL);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    skip_before = event_subscriber_filter_get_skip_count();
#endif

    zassert_equal(
        event_subscribe_filtered(FILTER_TEST_TYPE_A, filter_cb_count, &g_sink, filter_by_tag, &ctx, &sub_id),
        EVENT_OK, NULL);

    filter_test_publish(FILTER_TEST_TYPE_A, 1U);
    filter_test_publish(FILTER_TEST_TYPE_A, 2U);
    filter_test_publish(FILTER_TEST_TYPE_A, 1U);
    filter_test_drain(3U);

    zassert_equal(atomic_get(&g_sink.cb_calls), 2, "匹配载荷应收到 2 次");
    zassert_equal(atomic_get(&ctx.filter_ok), 2, NULL);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    zassert_equal(event_subscriber_filter_get_skip_count() - skip_before, 1U, "不匹配载荷应跳过 1 次");
#endif
    zassert_equal(atomic_get(&g_filter_anomalies), 0, NULL);
}

/**
 * @brief skip 计数与 dispatcher 统计口径：事件级照常计数，订阅级跳过单独累计
 */
ZTEST(test_event_filter, test_filter_stats_accounting) {
    filter_ctx_t   ctx = {.magic = FILTER_CTX_MAGIC_A, .tag = 7U};
    uint32_t       sub_id = 0U;
    dispatcher_stats_t stats;
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    uint64_t skip_before;
#endif

    filter_test_reset();
    zassert_equal(event_register_type(FILTER_TEST_TYPE_A, "flt_stats"), EVENT_OK, NULL);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    skip_before = event_subscriber_filter_get_skip_count();
#endif

    zassert_equal(
        event_subscribe_filtered(FILTER_TEST_TYPE_A, filter_cb_count, &g_sink, filter_by_tag, &ctx, &sub_id),
        EVENT_OK, NULL);

    filter_test_publish(FILTER_TEST_TYPE_A, 7U);
    filter_test_publish(FILTER_TEST_TYPE_A, 8U);
    filter_test_publish(FILTER_TEST_TYPE_A, 7U);
    filter_test_publish(FILTER_TEST_TYPE_A, 9U);
    filter_test_drain(4U);

    zassert_equal(atomic_get(&g_sink.cb_calls), 2, NULL);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    zassert_equal(event_subscriber_filter_get_skip_count() - skip_before, 2U, "应跳过 2 次订阅者交付");
#endif

    event_dispatcher_get_stats(&stats);
    zassert_equal(stats.events_processed, 4U, "事件级消费含被订阅级跳过的事件");
    zassert_equal(stats.events_filtered, 0U, "未设全局过滤，events_filtered 应为 0");
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    zassert_equal(stats.subscriber_filtered, 2U, "订阅级跳过应镜像进 dispatcher 统计");
#endif
}

/**
 * @brief 同类型两订阅者过滤相互独立
 */
ZTEST(test_event_filter, test_filter_two_subscribers_independent) {
    filter_ctx_t ctx_a = {.magic = FILTER_CTX_MAGIC_A, .tag = 1U};
    filter_ctx_t ctx_b = {.magic = FILTER_CTX_MAGIC_B, .tag = 2U};
    uint32_t     id_a = 0U;
    uint32_t     id_b = 0U;
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    uint64_t skip_before;
#endif

    filter_test_reset();
    zassert_equal(event_register_type(FILTER_TEST_TYPE_A, "flt_two"), EVENT_OK, NULL);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    skip_before = event_subscriber_filter_get_skip_count();
#endif

    zassert_equal(event_subscribe_filtered(FILTER_TEST_TYPE_A, filter_cb_count, &g_sink, filter_by_tag, &ctx_a, &id_a),
                  EVENT_OK, NULL);
    zassert_equal(event_subscribe_filtered(FILTER_TEST_TYPE_A, filter_cb_count, &g_sink, filter_by_tag, &ctx_b, &id_b),
                  EVENT_OK, NULL);

    filter_test_publish(FILTER_TEST_TYPE_A, 1U);
    filter_test_publish(FILTER_TEST_TYPE_A, 2U);
    filter_test_publish(FILTER_TEST_TYPE_A, 3U);
    filter_test_drain(3U);

    zassert_equal(atomic_get(&ctx_a.filter_ok), 1, "A 只放行载荷 1");
    zassert_equal(atomic_get(&ctx_b.filter_ok), 1, "B 只放行载荷 2");
    zassert_equal(atomic_get(&g_sink.cb_calls), 2, "两订阅者各收到 1 次");
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    /* 跳过 = 事件1 对 B + 事件2 对 A + 事件3 对 A、B 各一次 = 4 */
    zassert_equal(event_subscriber_filter_get_skip_count() - skip_before, 4U, "共应跳过 4 次订阅者交付");
#endif
}

/**
 * @brief filter=NULL 的 filtered 订阅等价普通订阅（负测：NULL 回调仍拒绝）
 */
ZTEST(test_event_filter, test_filter_null_equals_plain_subscribe) {
    uint32_t plain_id = 0U;
    uint32_t filtered_id = 0U;
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    uint64_t skip_before;
#endif

    filter_test_reset();
    zassert_equal(event_register_type(FILTER_TEST_TYPE_A, "flt_null"), EVENT_OK, NULL);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    skip_before = event_subscriber_filter_get_skip_count();
#endif

    zassert_equal(event_subscribe_filtered(FILTER_TEST_TYPE_A, NULL, NULL, NULL, NULL, &filtered_id),
                  EVENT_ERR_INVALID_ARG, "NULL 回调应拒绝");

    zassert_equal(event_subscribe(FILTER_TEST_TYPE_A, filter_cb_count, &g_sink, &plain_id), EVENT_OK, NULL);
    zassert_equal(
        event_subscribe_filtered(FILTER_TEST_TYPE_A, filter_cb_count, &g_sink, NULL, NULL, &filtered_id),
        EVENT_OK, NULL);

    for (uint32_t i = 1U; i <= 3U; i++) {
        filter_test_publish(FILTER_TEST_TYPE_A, i);
    }
    filter_test_drain(3U);

    zassert_equal(atomic_get(&g_sink.cb_calls), 6, "两个订阅者均应全收（3x2）");
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    zassert_equal(event_subscriber_filter_get_skip_count() - skip_before, 0U, "无过滤不应产生跳过计数");
#endif
}

/**
 * @brief 事后 set_filter 生效；对不存在订阅者返回 NOT_FOUND
 */
ZTEST(test_event_filter, test_filter_set_after_subscribe) {
    filter_ctx_t ctx = {.magic = FILTER_CTX_MAGIC_A, .tag = 2U};
    uint32_t     sub_id = 0U;
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    uint64_t skip_before;
#endif

    filter_test_reset();
    zassert_equal(event_register_type(FILTER_TEST_TYPE_A, "flt_set"), EVENT_OK, NULL);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    skip_before = event_subscriber_filter_get_skip_count();
#endif

    zassert_equal(event_subscribe(FILTER_TEST_TYPE_A, filter_cb_count, &g_sink, &sub_id), EVENT_OK, NULL);
    zassert_equal(event_set_subscriber_filter(sub_id, FILTER_TEST_TYPE_A, filter_by_tag, &ctx), EVENT_OK, NULL);
    zassert_equal(event_set_subscriber_filter(0xDEADU, FILTER_TEST_TYPE_A, filter_by_tag, &ctx),
                  EVENT_ERR_NOT_FOUND, "不存在的订阅者应返回 NOT_FOUND");

    filter_test_publish(FILTER_TEST_TYPE_A, 1U);
    filter_test_publish(FILTER_TEST_TYPE_A, 2U);
    filter_test_drain(2U);

    zassert_equal(atomic_get(&g_sink.cb_calls), 1, "设置过滤后仅匹配载荷到达");
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    zassert_equal(event_subscriber_filter_get_skip_count() - skip_before, 1U, NULL);
#endif
}

/**
 * @brief 清除过滤（filter=NULL）恢复全收
 */
ZTEST(test_event_filter, test_filter_clear_restores_all) {
    filter_ctx_t ctx = {.magic = FILTER_CTX_MAGIC_A, .tag = 3U};
    uint32_t     sub_id = 0U;
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    uint64_t skip_before;
#endif

    filter_test_reset();
    zassert_equal(event_register_type(FILTER_TEST_TYPE_A, "flt_clr"), EVENT_OK, NULL);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    skip_before = event_subscriber_filter_get_skip_count();
#endif

    zassert_equal(
        event_subscribe_filtered(FILTER_TEST_TYPE_A, filter_cb_count, &g_sink, filter_by_tag, &ctx, &sub_id),
        EVENT_OK, NULL);

    filter_test_publish(FILTER_TEST_TYPE_A, 3U);
    filter_test_publish(FILTER_TEST_TYPE_A, 4U);
    filter_test_drain(2U);
    zassert_equal(atomic_get(&g_sink.cb_calls), 1, NULL);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    zassert_equal(event_subscriber_filter_get_skip_count() - skip_before, 1U, NULL);
#endif

    zassert_equal(event_set_subscriber_filter(sub_id, FILTER_TEST_TYPE_A, NULL, NULL), EVENT_OK, NULL);

    filter_test_publish(FILTER_TEST_TYPE_A, 5U);
    filter_test_drain(1U);

    zassert_equal(atomic_get(&g_sink.cb_calls), 2, "清除过滤后应恢复全收");
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    zassert_equal(event_subscriber_filter_get_skip_count() - skip_before, 1U, "清除后不应新增跳过");
#endif
}

/* =============================================================================
 * 侧表槽管理用例
 * ============================================================================= */

/**
 * @brief 槽耗尽：第 17 个过滤订阅返回 NO_MEM（负测）
 */
ZTEST(test_event_filter, test_filter_slots_exhausted_no_mem) {
    uint32_t ids[CONFIG_EVENT_SUBSCRIBER_FILTER_SLOTS];
    uint32_t extra_id = 0U;

    filter_test_reset();
    zassert_equal(event_register_type(FILTER_TEST_TYPE_A, "flt_full"), EVENT_OK, NULL);
    zassert_equal(event_register_type(FILTER_TEST_TYPE_B, "flt_full_b"), EVENT_OK, NULL);

    for (uint32_t i = 0; i < CONFIG_EVENT_SUBSCRIBER_FILTER_SLOTS; i++) {
        zassert_equal(event_subscribe_filtered(FILTER_TEST_TYPE_A, test_event_noop_callback, NULL, filter_allpass,
                                               NULL, &ids[i]),
                      EVENT_OK, "filtered subscribe #%u should succeed", i);
    }

    /* A 类订阅槽亦满（16），换 B 类验证的是过滤侧表耗尽而非订阅槽耗尽 */
    zassert_equal(
        event_subscribe_filtered(FILTER_TEST_TYPE_B, test_event_noop_callback, NULL, filter_allpass, NULL, &extra_id),
        EVENT_ERR_NO_MEM, "第 17 个过滤订阅应返回 NO_MEM");

    /* 普通订阅不受过滤侧表影响 */
    zassert_equal(event_subscribe(FILTER_TEST_TYPE_B, test_event_noop_callback, NULL, &extra_id), EVENT_OK,
                  "普通订阅不应受过滤侧表影响");
}

/**
 * @brief 退订回收过滤槽
 */
ZTEST(test_event_filter, test_filter_unsubscribe_recycles_slot) {
    uint32_t ids[CONFIG_EVENT_SUBSCRIBER_FILTER_SLOTS];
    uint32_t recycled_id = 0U;

    filter_test_reset();
    zassert_equal(event_register_type(FILTER_TEST_TYPE_A, "flt_recyc"), EVENT_OK, NULL);

    for (uint32_t i = 0; i < CONFIG_EVENT_SUBSCRIBER_FILTER_SLOTS; i++) {
        zassert_equal(event_subscribe_filtered(FILTER_TEST_TYPE_A, test_event_noop_callback, NULL, filter_allpass,
                                               NULL, &ids[i]),
                      EVENT_OK, NULL);
    }

    zassert_equal(event_unsubscribe(FILTER_TEST_TYPE_A, ids[0]), EVENT_OK, NULL);
    zassert_equal(
        event_subscribe_filtered(FILTER_TEST_TYPE_A, test_event_noop_callback, NULL, filter_allpass, NULL, &recycled_id),
        EVENT_OK, "退订后应回收过滤槽");
}

/**
 * @brief 热换复用原槽不额外占槽，且新过滤立即生效
 */
ZTEST(test_event_filter, test_filter_hot_swap_reuses_slot) {
    filter_ctx_t ctx_a = {.magic = FILTER_CTX_MAGIC_A, .tag = 1U};
    filter_ctx_t ctx_b = {.magic = FILTER_CTX_MAGIC_B, .tag = 2U};
    uint32_t     swap_id = 0U;
    uint32_t     filler_id = 0U;
    uint32_t     extra_id = 0U;
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    uint64_t skip_before;
#endif

    filter_test_reset();
    zassert_equal(event_register_type(FILTER_TEST_TYPE_A, "flt_swap"), EVENT_OK, NULL);
    zassert_equal(event_register_type(FILTER_TEST_TYPE_B, "flt_swap_b"), EVENT_OK, NULL);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    skip_before = event_subscriber_filter_get_skip_count();
#endif

    zassert_equal(
        event_subscribe_filtered(FILTER_TEST_TYPE_A, filter_cb_count, &g_sink, filter_by_tag, &ctx_a, &swap_id),
        EVENT_OK, NULL);
    /* 填满其余过滤槽 */
    for (uint32_t i = 0; i < CONFIG_EVENT_SUBSCRIBER_FILTER_SLOTS - 1U; i++) {
        zassert_equal(event_subscribe_filtered(FILTER_TEST_TYPE_A, test_event_noop_callback, NULL, filter_allpass,
                                               NULL, &filler_id),
                      EVENT_OK, NULL);
    }

    /* 热换：替换为 filter_b，应复用原槽 */
    zassert_equal(event_set_subscriber_filter(swap_id, FILTER_TEST_TYPE_A, filter_by_tag, &ctx_b), EVENT_OK,
                  "热换应复用原槽");
    /* 第 17 个过滤订阅换 B 类：A 类订阅槽已满，需排除订阅槽耗尽的干扰 */
    zassert_equal(
        event_subscribe_filtered(FILTER_TEST_TYPE_B, test_event_noop_callback, NULL, filter_allpass, NULL, &extra_id),
        EVENT_ERR_NO_MEM, "热换不应额外占槽");

    filter_test_publish(FILTER_TEST_TYPE_A, 1U);
    filter_test_publish(FILTER_TEST_TYPE_A, 2U);
    filter_test_drain(2U);

    zassert_equal(atomic_get(&ctx_b.filter_ok), 1, "热换后应按新过滤放行载荷 2");
    zassert_equal(atomic_get(&ctx_a.filter_ok), 0, "旧过滤不应再放行");
    zassert_equal(atomic_get(&g_sink.cb_calls), 1, NULL);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    zassert_equal(event_subscriber_filter_get_skip_count() - skip_before, 1U, NULL);
#endif
}

/**
 * @brief 与全局过滤器叠加：全局先判，全局拒绝时订阅级过滤不被调用
 */
ZTEST(test_event_filter, test_filter_global_precedes_subscriber) {
    filter_ctx_t ctx = {.magic = FILTER_CTX_MAGIC_A, .tag = 1U};
    uint32_t     sub_id = 0U;
    dispatcher_stats_t stats;
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    uint64_t skip_before;
#endif

    filter_test_reset();
    zassert_equal(event_register_type(FILTER_TEST_TYPE_A, "flt_global"), EVENT_OK, NULL);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    skip_before = event_subscriber_filter_get_skip_count();
#endif
    atomic_set(&g_global_filter_calls, 0);

    event_dispatcher_set_filter(filter_global_reject, NULL);
    zassert_equal(
        event_subscribe_filtered(FILTER_TEST_TYPE_A, filter_cb_count, &g_sink, filter_by_tag, &ctx, &sub_id),
        EVENT_OK, NULL);

    filter_test_publish(FILTER_TEST_TYPE_A, 1U);
    filter_test_drain(1U);

    zassert_equal(atomic_get(&g_global_filter_calls), 1, "全局过滤应被调用");
    zassert_equal(atomic_get(&ctx.filter_calls), 0, "全局拒绝后订阅级过滤不应被调用");
    zassert_equal(atomic_get(&g_sink.cb_calls), 0, NULL);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    zassert_equal(event_subscriber_filter_get_skip_count() - skip_before, 0U, NULL);
#endif

    event_dispatcher_get_stats(&stats);
    zassert_equal(stats.events_filtered, 1U, "全局过滤拒绝应计入 events_filtered");

    event_dispatcher_clear_filter();
}

/* =============================================================================
 * 并发用例（参照 test_concurrency_stress.c 风格）
 * ============================================================================= */

static K_THREAD_STACK_DEFINE(g_filter_producer_stack, 1024);
static struct k_thread g_filter_producer_thread;
static atomic_t        g_pair_published;  /**< 成功入队的事件数（守恒口径） */
static atomic_t        g_pair_dropped;    /**< 入队失败被丢弃的事件数 */
static atomic_t        g_pair_producer_done;
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
static uint64_t        g_pair_skip_base; /**< 并发用例起始时的跳过计数基线 */
#else
static atomic_t g_pair_quiet_rounds;    /**< STATS=n 静默判定：交付计数不变的连续轮询轮数 */
static atomic_t g_pair_prev_delivered;  /**< STATS=n 静默判定：上一轮询的交付计数 */
#endif

static filter_ctx_t g_pair_ctx_a = {.magic = FILTER_CTX_MAGIC_A, .tag = 0xAAU};
static filter_ctx_t g_pair_ctx_b = {.magic = FILTER_CTX_MAGIC_B, .tag = 0xBBU};

/** @brief 配对过滤 A：ud 魔数必须与 A 侧一致（撕裂组合在此暴露） */
static bool filter_pair_a(const event_t* event, void* filter_user_data) {
    filter_ctx_t* ctx = (filter_ctx_t*) filter_user_data;

    if (ctx == NULL || ctx->magic != FILTER_CTX_MAGIC_A) {
        atomic_inc(&g_filter_anomalies);
        return false;
    }
    return filter_by_tag(event, filter_user_data);
}

/** @brief 配对过滤 B：ud 魔数必须与 B 侧一致 */
static bool filter_pair_b(const event_t* event, void* filter_user_data) {
    filter_ctx_t* ctx = (filter_ctx_t*) filter_user_data;

    if (ctx == NULL || ctx->magic != FILTER_CTX_MAGIC_B) {
        atomic_inc(&g_filter_anomalies);
        return false;
    }
    return filter_by_tag(event, filter_user_data);
}

static void filter_pair_producer(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (uint32_t i = 0; i < FILTER_PAIR_PUBLISHED; i++) {
        uint32_t payload = (i & 1U) != 0U ? 0xBBU : 0xAAU;

        /* 只把成功入队的事件计入守恒口径：被丢弃事件既不交付也不跳过 */
        if (event_publish_copy(FILTER_TEST_TYPE_A, EVENT_PRIORITY_NORMAL, &payload, sizeof(payload)) == EVENT_OK) {
            atomic_inc(&g_pair_published);
        } else {
            atomic_inc(&g_pair_dropped);
        }
        k_yield();
    }
    atomic_set(&g_pair_producer_done, 1);
}

#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
/** @brief 排空判定（STATS=y）：交付数 + 跳过增量 >= 发布数 */
static bool filter_pair_drained_pred(void* ctx) {
    atomic_val_t skip_now = (atomic_val_t) (event_subscriber_filter_get_skip_count() - g_pair_skip_base);
    atomic_val_t delivered = atomic_get(&g_sink.cb_calls);

    ARG_UNUSED(ctx);
    return (delivered + skip_now) >= atomic_get(&g_pair_published);
}
#else
/** @brief 排空判定（STATS=n，无跳过计数可用）：生产结束且交付计数静默 300ms */
static bool filter_pair_quiesced_pred(void* ctx) {
    ARG_UNUSED(ctx);

    if (atomic_get(&g_pair_producer_done) == 0) {
        return false;
    }
    atomic_val_t delivered = atomic_get(&g_sink.cb_calls);

    if (delivered != atomic_get(&g_pair_prev_delivered)) {
        atomic_set(&g_pair_prev_delivered, delivered);
        atomic_set(&g_pair_quiet_rounds, 0);
        return false;
    }
    atomic_inc(&g_pair_quiet_rounds);
    return atomic_get(&g_pair_quiet_rounds) >= FILTER_PAIR_QUIET_POLLS;
}
#endif /* CONFIG_EVENT_SUBSCRIBER_FILTER_STATS */

/**
 * @brief 并发热换与分发一致性：分发线程始终看到完整 (filter, ud) 配对，
 *       每条事件恰好「交付或跳过」二选一，总量守恒
 */
ZTEST(test_event_filter, test_filter_concurrent_swap_consistency) {
    uint32_t   sub_id = 0U;
    atomic_val_t published;
    atomic_val_t delivered;
    atomic_val_t accepted;
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    uint64_t skip_before;
#endif
    bool       swap_a = false;

    filter_test_reset();
    zassert_equal(event_register_type(FILTER_TEST_TYPE_A, "flt_pair"), EVENT_OK, NULL);
    atomic_set(&g_pair_published, 0);
    atomic_set(&g_pair_dropped, 0);
    atomic_set(&g_pair_producer_done, 0);
    atomic_set(&g_pair_ctx_a.filter_calls, 0);
    atomic_set(&g_pair_ctx_a.filter_ok, 0);
    atomic_set(&g_pair_ctx_b.filter_calls, 0);
    atomic_set(&g_pair_ctx_b.filter_ok, 0);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    skip_before = event_subscriber_filter_get_skip_count();
    g_pair_skip_base = skip_before;
#else
    atomic_set(&g_pair_quiet_rounds, 0);
    atomic_set(&g_pair_prev_delivered, 0);
#endif

    zassert_equal(
        event_subscribe_filtered(FILTER_TEST_TYPE_A, filter_cb_count, &g_sink, filter_pair_a, &g_pair_ctx_a, &sub_id),
        EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    k_tid_t tid = k_thread_create(&g_filter_producer_thread, g_filter_producer_stack,
                                  K_THREAD_STACK_SIZEOF(g_filter_producer_stack), filter_pair_producer, NULL, NULL,
                                  NULL, 5, 0, K_NO_WAIT);
    k_thread_name_set(tid, "flt_pair_pub");

    /* 生产期间持续热换过滤配对；两个配对各自校验 ud 魔数，撕裂组合会计入异常 */
    for (uint32_t i = 0; i < 200U && atomic_get(&g_pair_producer_done) == 0; i++) {
        if (swap_a) {
            (void) event_set_subscriber_filter(sub_id, FILTER_TEST_TYPE_A, filter_pair_a, &g_pair_ctx_a);
        } else {
            (void) event_set_subscriber_filter(sub_id, FILTER_TEST_TYPE_A, filter_pair_b, &g_pair_ctx_b);
        }
        swap_a = !swap_a;
        k_msleep(1);
    }

    zassert_equal(k_thread_join(&g_filter_producer_thread, K_SECONDS(3)), 0, "producer should finish");

#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    /* 等待分发排空：每条已入队事件恰好「交付或跳过」二选一 */
    published = atomic_get(&g_pair_published);
    zassert_true(ztest_wait_until(filter_pair_drained_pred, NULL, 2000U),
                 "drain timeout: published=%d delivered=%d skip_delta=%d dropped=%d", (int) published,
                 (int) atomic_get(&g_sink.cb_calls),
                 (int) (event_subscriber_filter_get_skip_count() - skip_before), (int) atomic_get(&g_pair_dropped));
#else
    /* STATS=n 无跳过计数：以静默窗口近似排空后再停止分发器 */
    zassert_true(ztest_wait_until(filter_pair_quiesced_pred, NULL, 3000U), "quiesce timeout");
    published = atomic_get(&g_pair_published);
#endif

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);

    delivered = atomic_get(&g_sink.cb_calls);
    accepted = atomic_get(&g_pair_ctx_a.filter_ok) + atomic_get(&g_pair_ctx_b.filter_ok);
    zassert_equal(atomic_get(&g_filter_anomalies), 0, "不得出现撕裂的 (filter, ud) 配对");
    zassert_true(delivered == accepted, "delivered=%d accepted=%d", (int) delivered, (int) accepted);
    zassert_true(delivered > 0, "两种载荷都应至少交付一次");
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    zassert_equal((atomic_val_t) (event_subscriber_filter_get_skip_count() - skip_before) + delivered, published,
                  "skip=%d delivered=%d published=%d dropped=%d",
                  (int) (event_subscriber_filter_get_skip_count() - skip_before), (int) delivered, (int) published,
                  (int) atomic_get(&g_pair_dropped));
#endif
    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
}

ZTEST_SUITE(test_event_filter, NULL, NULL, NULL, NULL, NULL);
