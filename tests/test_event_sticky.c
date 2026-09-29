/**
 * @file test_event_sticky.c
 * @brief Sticky 事件锁存与订阅重放（CONFIG_EVENT_STICKY）单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-09-29
 *
 * 仅当 CONFIG_EVENT_STICKY=y 时由 tests/CMakeLists.txt 编入；
 * 运行需叠加 tests/prj_event_ext.conf（CONF_FILE 三段叠加）。
 *
 * @par 修改日志:
 *
 * Date Version Author Description
 * 2026-09-27 1.0 zeh 初始版本
 * 2026-09-29 1.1 zeh 覆盖类型 255 占槽，以及 pending DISABLE 期间再次 enable
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>
#include <string.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include "event_memory.h"

LOG_MODULE_REGISTER(test_event_sticky);

/* 本套件专用事件类型（各用例先 shutdown 再注册，避免与其他套件残留注册冲突） */
#define STICKY_TYPE_A              233U
#define STICKY_TYPE_B              234U
#define STICKY_TYPE_C              235U
#define STICKY_TYPE_D              236U
#define STICKY_TYPE_E              237U
#define STICKY_TYPE_F              238U

/** 重放跟踪槽容量外加一个线程，用于验证归还行为。 */
#define STICKY_REPLAY_THREAD_COUNT 9U
#define STICKY_TOGGLE_ITERATIONS   100U

/** 大负载用例长度：超过内联容量（48B）且低于 4KB 锁存上界 */
#define STICKY_LARGE_LEN           200U

/** too-large 用例长度：4.1KB 超过 4096B 锁存上界（发布本身仍合法） */
#define STICKY_TOO_LARGE_LEN       4200U

/** ISR 发布后等待定时器触发的余量（毫秒） */
#define STICKY_ISR_WAIT_MS         50U

/** 回调负载快照缓冲（须覆盖 STICKY_LARGE_LEN 用例） */
#define STICKY_SINK_BUF_LEN        256U

/** 泄漏断言基线快照（未开 CONFIG_EVENT_DEBUG_MEM 时恒 0，断言退化为不校验） */
#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
#define STICKY_LEAK_SNAPSHOT() event_check_leaks()
#else
#define STICKY_LEAK_SNAPSHOT() 0U
#endif

typedef struct {
    atomic_t calls;                             /**< 回调被调用次数 */
    uint32_t last_u32;                          /**< 最近一次 4 字节载荷（按 uint32 解释） */
    uint32_t last_len;                          /**< 最近一次负载长度（字节） */
    uint32_t total_bytes;                       /**< 累计收到的负载字节数 */
    uint8_t  last_payload[STICKY_SINK_BUF_LEN]; /**< 最近一次负载副本 */
} sticky_sink_t;

static sticky_sink_t g_sink;

/** 重放回调内的 API 返回码登记（回调内不宜 zassert 失败即中断，登记后由用例断言） */
static atomic_t g_replay_api_ret;

/** 重放武装标记：1=下一次回调来自订阅重放（触发一次后自动撤销），常规投递不动作 */
static atomic_t g_replay_arm;

static struct k_timer g_sticky_isr_timer;
static event_t        g_sticky_isr_event; /**< ISR 发布用静态事件（k_timer 回调 = ISR 上下文） */

static struct k_thread g_sticky_replay_threads[STICKY_REPLAY_THREAD_COUNT];
static K_THREAD_STACK_ARRAY_DEFINE(g_sticky_replay_stacks, STICKY_REPLAY_THREAD_COUNT, 1024);
static atomic_t g_sticky_replay_callbacks;
static atomic_t g_sticky_replay_recursive_callbacks;
static atomic_t g_sticky_replay_callback_depth;
static atomic_t g_sticky_replay_worker_error;

static struct k_thread g_sticky_enable_thread;
static struct k_thread g_sticky_disable_thread;
static K_THREAD_STACK_DEFINE(g_sticky_enable_stack, 1024);
static K_THREAD_STACK_DEFINE(g_sticky_disable_stack, 1024);
static struct k_sem g_sticky_toggle_start;
static atomic_t     g_sticky_toggle_enable_error;
static atomic_t     g_sticky_reenable_disable_ret;
static atomic_t     g_sticky_reenable_enable_ret;

/* =============================================================================
 * 测试辅助
 * ============================================================================= */

/** @brief 回到已知初始态：shutdown → init → dispatcher init → 统计复位 → start */
static void sticky_test_reset(void) {
    zassert_equal(event_system_shutdown(), EVENT_OK, "shutdown failed");
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    event_dispatcher_reset_stats();
    zassert_equal(event_system_start(), EVENT_OK, NULL);
}

/** @brief 重放回调内再次订阅同类型；正确的线程深度防御应阻止递归重放。 */
static void sticky_replay_depth_guard_cb(const event_t* event, void* user_data) {
    uint32_t nested_id = EVENT_SUBSCRIBER_ID_INVALID;

    ARG_UNUSED(user_data);
    atomic_inc(&g_sticky_replay_callbacks);
    if (atomic_cas(&g_sticky_replay_callback_depth, 0, 1)) {
        (void) event_subscribe(event->type, sticky_replay_depth_guard_cb, NULL, &nested_id);
        if (nested_id != EVENT_SUBSCRIBER_ID_INVALID) {
            (void) event_unsubscribe(event->type, nested_id);
        }
        atomic_set(&g_sticky_replay_callback_depth, 0);
    } else {
        atomic_inc(&g_sticky_replay_recursive_callbacks);
    }
}

/** @brief 短生命周期线程：订阅触发一次重放后解除订阅。 */
static void sticky_replay_worker(void* p1, void* p2, void* p3) {
    uint32_t sub_id = EVENT_SUBSCRIBER_ID_INVALID;

    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    if (event_subscribe(STICKY_TYPE_A, sticky_replay_depth_guard_cb, NULL, &sub_id) != EVENT_OK) {
        atomic_inc(&g_sticky_replay_worker_error);
        return;
    }
    if (event_unsubscribe(STICKY_TYPE_A, sub_id) != EVENT_OK) {
        atomic_inc(&g_sticky_replay_worker_error);
    }
}

/** @brief 并发 enable 工作者：与 disable 同时切换同一类型的 sticky 状态。 */
static void sticky_enable_worker(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    k_sem_take(&g_sticky_toggle_start, K_FOREVER);
    for (uint32_t i = 0U; i < STICKY_TOGGLE_ITERATIONS; i++) {
        if (event_sticky_enable(STICKY_TYPE_A) != EVENT_OK) {
            atomic_inc(&g_sticky_toggle_enable_error);
        }
        k_yield();
    }
}

/** @brief 并发 disable 工作者：未启用时的 NOT_FOUND 属预期，其余状态由最终映射断言覆盖。 */
static void sticky_disable_worker(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    k_sem_take(&g_sticky_toggle_start, K_FOREVER);
    for (uint32_t i = 0U; i < STICKY_TOGGLE_ITERATIONS; i++) {
        (void) event_sticky_disable(STICKY_TYPE_A);
        k_yield();
    }
}

static void sticky_sink_reset(void) {
    atomic_set(&g_sink.calls, 0);
    g_sink.last_u32 = 0U;
    g_sink.last_len = 0U;
    g_sink.total_bytes = 0U;
    memset(g_sink.last_payload, 0, sizeof(g_sink.last_payload));
}

/** @brief 重放/常规投递共用回调：记录调用次数与负载副本（浅拷贝，只读 event） */
static void sticky_capture_cb(const event_t* event, void* user_data) {
    sticky_sink_t* sink = (sticky_sink_t*) user_data;

    zassert_not_null(sink, "user_data 不得为 NULL");
    atomic_inc(&sink->calls);
    sink->last_len = event->data_len;
    sink->total_bytes += event->data_len;
    if (event->data_len > 0U && event->data_len <= STICKY_SINK_BUF_LEN) {
        if ((event->flags & EVENT_FLAG_DATA_INLINE) != 0U) {
            memcpy(sink->last_payload, event->data.inline_data, event->data_len);
        } else if (event->data.ptr != NULL) {
            memcpy(sink->last_payload, event->data.ptr, event->data_len);
        } else {
            zassert_unreachable("动态负载指针不得为 NULL");
        }
    }
    if (event->data_len == sizeof(uint32_t)) {
        memcpy(&sink->last_u32, sink->last_payload, sizeof(sink->last_u32));
    }
}

/** @brief 发布一条内联 uint32 载荷事件（栈上构造，锁存发生在入队成功时） */
static void sticky_publish_u32(uint8_t type, uint32_t value) {
    event_t event = {0};

    event.type = type;
    event.priority = EVENT_PRIORITY_NORMAL;
    event.timestamp = k_uptime_get_32();
    event.flags = EVENT_FLAG_DATA_INLINE;
    event.data_len = sizeof(value);
    memcpy(event.data.inline_data, &value, sizeof(value));

    zassert_equal(event_publish(&event), EVENT_OK, "publish value=0x%x failed", value);
}

/** @brief 手动消费 count 条事件，并断言队列随后已空 */
static void sticky_drain(uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        zassert_equal(event_dispatcher_process_one(K_NO_WAIT), EVENT_OK, "process_one #%u failed", i);
    }
    zassert_equal(event_dispatcher_process_one(K_NO_WAIT), EVENT_ERR_QUEUE_EMPTY, "queue should be empty");
}

/** @brief ISR 发布定时器回调（k_timer 过期函数运行于 ISR 上下文） */
static void sticky_isr_timer_fn(struct k_timer* timer) {
    ARG_UNUSED(timer);

    (void) event_publish_from_isr(&g_sticky_isr_event);
}

/** @brief 构造一条内联 uint32 载荷的静态 ISR 事件 */
static void sticky_isr_event_prepare(uint8_t type, uint32_t value) {
    memset(&g_sticky_isr_event, 0, sizeof(g_sticky_isr_event));
    g_sticky_isr_event.type = type;
    g_sticky_isr_event.priority = EVENT_PRIORITY_NORMAL;
    g_sticky_isr_event.timestamp = k_uptime_get_32();
    g_sticky_isr_event.flags = EVENT_FLAG_DATA_INLINE;
    g_sticky_isr_event.data_len = sizeof(value);
    memcpy(g_sticky_isr_event.data.inline_data, &value, sizeof(value));
}

/* 重放回调族：先按常规记录重放沿，再在重放上下文内触发被测 API 并登记返回码 */

/** @brief 重放回调：对同类型再发布一条事件（验证锁存让位计 miss、旧沿不变） */
static void sticky_replay_publish_cb(const event_t* event, void* user_data) {
    event_t  again = {0};
    uint32_t value = 0xDEAD0002U;

    sticky_capture_cb(event, user_data);
    /* 仅重放那一次触发同类型发布；常规投递进入本回调不得再发布（否则队列永不清空） */
    if (atomic_set(&g_replay_arm, 0) != 1U) {
        return;
    }
    again.type = event->type;
    again.priority = EVENT_PRIORITY_NORMAL;
    again.timestamp = k_uptime_get_32();
    again.flags = EVENT_FLAG_DATA_INLINE;
    again.data_len = sizeof(value);
    memcpy(again.data.inline_data, &value, sizeof(value));
    atomic_set(&g_replay_api_ret, (atomic_val_t) event_publish(&again));
}

/** @brief 重放回调：对同类型 event_sticky_clear（验证返回 OK 且 pending 延迟收尾） */
static void sticky_replay_clear_cb(const event_t* event, void* user_data) {
    sticky_capture_cb(event, user_data);
    atomic_set(&g_replay_api_ret, (atomic_val_t) event_sticky_clear(event->type));
}

/** @brief 重放回调：对同类型 event_sticky_disable（验证返回 OK 且 pending 延迟收尾） */
static void sticky_replay_disable_cb(const event_t* event, void* user_data) {
    sticky_capture_cb(event, user_data);
    atomic_set(&g_replay_api_ret, (atomic_val_t) event_sticky_disable(event->type));
}

/** @brief 仅重放那一次：disable 后立刻 enable，验证满池时复用原槽而不是 NOT_FOUND */
static void sticky_replay_reenable_cb(const event_t* event, void* user_data) {
    ARG_UNUSED(user_data);
    if (!atomic_cas(&g_replay_arm, 1, 0)) {
        return;
    }
    atomic_set(&g_sticky_reenable_disable_ret, (atomic_val_t) event_sticky_disable(event->type));
    atomic_set(&g_sticky_reenable_enable_ret, (atomic_val_t) event_sticky_enable(event->type));
}

/* =============================================================================
 * 锁存与重放语义
 * ============================================================================= */

/**
 * @brief 发布后新订阅者同步收到一次重放且负载一致；常规投递照常进行
 */
/**
 * @brief 首次 init 后非零类型可领取槽位，不依赖此前执行 shutdown 的副作用
 */
ZTEST(a_event_sticky_cold, test_sticky_cold_init_nonzero_type_can_enable) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_F, "stk_cold"), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_F), EVENT_OK, "首次 init 必须将 BSS 槽复位为空闲标记");
    zassert_equal(event_sticky_disable(STICKY_TYPE_F), EVENT_OK, NULL);
}

ZTEST(test_event_sticky, test_sticky_replay_matches_payload) {
    uint32_t sub_id = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_replay"), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);

    sticky_publish_u32(STICKY_TYPE_A, 0x13572468U);
    zassert_true(event_sticky_is_latched(STICKY_TYPE_A), "入队成功后应已锁存");

    zassert_equal(event_subscribe(STICKY_TYPE_A, sticky_capture_cb, &g_sink, &sub_id), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 1, "订阅成功应同步重放一次");
    zassert_equal(g_sink.last_u32, 0x13572468U, "重放载荷应与锁存沿一致");
    zassert_equal(g_sink.last_len, sizeof(uint32_t), NULL);

    /* 常规投递不受重放影响：队列中的原事件照常分发 */
    sticky_drain(1U);
    zassert_equal(atomic_get(&g_sink.calls), 2, "原事件仍应常规投递");
}

/**
 * @brief 连发多条时只锁存最近一条沿，新订阅者只得到最后一条
 */
ZTEST(test_event_sticky, test_sticky_replay_latest_edge_only) {
    uint32_t sub_id = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_latest"), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);

    sticky_publish_u32(STICKY_TYPE_A, 1U);
    sticky_publish_u32(STICKY_TYPE_A, 2U);
    sticky_publish_u32(STICKY_TYPE_A, 3U);
    sticky_drain(3U);

    zassert_equal(event_subscribe(STICKY_TYPE_A, sticky_capture_cb, &g_sink, &sub_id), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 1, "只应重放一次");
    zassert_equal(g_sink.last_u32, 3U, "重放的应是最近一条沿");
}

/**
 * @brief 未启用 sticky 的类型订阅不重放（负测）
 */
ZTEST(test_event_sticky, test_sticky_no_replay_when_not_enabled) {
    uint32_t sub_id = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_B, "stk_noenable"), EVENT_OK, NULL);
    zassert_false(event_sticky_is_latched(STICKY_TYPE_B), NULL);

    sticky_publish_u32(STICKY_TYPE_B, 7U);
    sticky_drain(1U);

    zassert_equal(event_subscribe(STICKY_TYPE_B, sticky_capture_cb, &g_sink, &sub_id), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 0, "未启用 sticky 不应重放");
}

/**
 * @brief 启用后从未发布过：订阅成功、无重放、无错误（边界）
 */
ZTEST(test_event_sticky, test_sticky_no_replay_before_first_publish) {
    uint32_t sub_id = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_C, "stk_nopub"), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_C), EVENT_OK, NULL);
    zassert_false(event_sticky_is_latched(STICKY_TYPE_C), "未发布过不应有锁存沿");

    zassert_equal(event_subscribe(STICKY_TYPE_C, sticky_capture_cb, &g_sink, &sub_id), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 0, "无锁存沿不应重放");

    /* 后续常规路径不受影响 */
    sticky_publish_u32(STICKY_TYPE_C, 42U);
    sticky_drain(1U);
    zassert_equal(atomic_get(&g_sink.calls), 1, NULL);
}

/**
 * @brief clear 后新订阅不重放；再次发布后恢复锁存与重放
 */
ZTEST(test_event_sticky, test_sticky_clear_then_republish_replays_again) {
    uint32_t sub1 = 0U;
    uint32_t sub2 = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_D, "stk_clear"), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_D), EVENT_OK, NULL);

    sticky_publish_u32(STICKY_TYPE_D, 11U);
    zassert_true(event_sticky_is_latched(STICKY_TYPE_D), NULL);

    zassert_equal(event_sticky_clear(STICKY_TYPE_D), EVENT_OK, "clear 应成功");
    zassert_false(event_sticky_is_latched(STICKY_TYPE_D), "clear 后不应再有锁存沿");
    zassert_equal(event_subscribe(STICKY_TYPE_D, sticky_capture_cb, &g_sink, &sub1), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 0, "clear 后订阅不应重放");

    /* 能力保留：再发布重新锁存，新订阅者获得新沿重放 */
    sticky_publish_u32(STICKY_TYPE_D, 22U);
    zassert_true(event_sticky_is_latched(STICKY_TYPE_D), "clear 后再发布应恢复锁存");
    zassert_equal(event_subscribe(STICKY_TYPE_D, sticky_capture_cb, &g_sink, &sub2), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 1, "再发布后的新订阅应重放一次");
    zassert_equal(g_sink.last_u32, 22U, "重放的应是新沿");

    sticky_drain(2U);
    /* 事件 11 与事件 22 各投递给 S1、S2（+4），加 S2 的重放（+1）= 5 */
    zassert_equal(atomic_get(&g_sink.calls), 5, "两条事件 x 两个订阅者 + 1 次重放");
}

/**
 * @brief 无订阅者发布仍锁存；分发消费不影响锁存沿
 */
ZTEST(test_event_sticky, test_sticky_latch_without_subscriber) {
    uint32_t sub_id = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_nosub"), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);

    sticky_publish_u32(STICKY_TYPE_A, 99U);
    sticky_drain(1U); /* 无订阅者：事件被消费后队列即空，锁存沿独立存活 */
    zassert_true(event_sticky_is_latched(STICKY_TYPE_A), "无订阅者发布也应锁存");

    zassert_equal(event_subscribe(STICKY_TYPE_A, sticky_capture_cb, &g_sink, &sub_id), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 1, "订阅成功应重放锁存沿");
    zassert_equal(g_sink.last_u32, 99U, NULL);
}

/**
 * @brief ISR 发布不更新锁存沿（负测；k_timer 回调 = ISR 上下文）
 */
ZTEST(test_event_sticky, test_sticky_isr_publish_not_latched) {
    uint32_t sub1 = 0U;
    uint32_t sub2 = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_isr"), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);

    /* 订阅先于 ISR 发布：无锁存沿时订阅不得重放 */
    zassert_equal(event_subscribe(STICKY_TYPE_A, sticky_capture_cb, &g_sink, &sub1), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 0, NULL);

    sticky_isr_event_prepare(STICKY_TYPE_A, 0x5A5A5A5AU);
    k_timer_init(&g_sticky_isr_timer, sticky_isr_timer_fn, NULL);
    k_timer_start(&g_sticky_isr_timer, K_MSEC(1), K_NO_WAIT);
    k_sleep(K_MSEC(STICKY_ISR_WAIT_MS));
    k_timer_stop(&g_sticky_isr_timer);

    zassert_false(event_sticky_is_latched(STICKY_TYPE_A), "ISR 发布不得更新锁存沿");
    sticky_drain(1U); /* ISR 事件仍应正常入队与投递 */
    zassert_equal(atomic_get(&g_sink.calls), 1, "ISR 事件走常规投递");

    /* 线程侧发布才更新沿 */
    sticky_publish_u32(STICKY_TYPE_A, 0xCAFEBABEU);
    zassert_true(event_sticky_is_latched(STICKY_TYPE_A), "线程侧发布应锁存");
    zassert_equal(event_subscribe(STICKY_TYPE_A, sticky_capture_cb, &g_sink, &sub2), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 2, "新订阅者应收到线程侧沿的重放");
    zassert_equal(g_sink.last_u32, 0xCAFEBABEU, "重放的应是线程侧沿而非 ISR 沿");

    sticky_drain(1U); /* 排空线程侧事件，队列归零 */
    /* 0xCAFEBABEU 投递给 S1、S2 两个订阅者（+2），累计 2 + 2 = 4 */
    zassert_equal(atomic_get(&g_sink.calls), 4, NULL);
}

/**
 * @brief 重放不计入 event_get_statistics().total_events
 */
ZTEST(test_event_sticky, test_sticky_replay_not_counted_in_total_events) {
    uint32_t sub_id = 0U;
    uint32_t total_before = 0U;
    uint32_t total_after = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_total"), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);

    sticky_publish_u32(STICKY_TYPE_A, 5U);
    sticky_drain(1U);
    event_get_statistics(&total_before, NULL, NULL);
    zassert_equal(total_before, 1U, "前置：常规投递已计 1");

    zassert_equal(event_subscribe(STICKY_TYPE_A, sticky_capture_cb, &g_sink, &sub_id), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 1, "重放应已发生");
    event_get_statistics(&total_after, NULL, NULL);
    zassert_equal(total_after, total_before, "重放不得计入 total_events");
}

/* =============================================================================
 * 负载拷贝与内存
 * ============================================================================= */

/**
 * @brief 200B 超内联负载锁存/重放内容正确（k_malloc 回退路径）
 */
ZTEST(test_event_sticky, test_sticky_large_payload_latch_replay) {
    uint8_t  payload[STICKY_LARGE_LEN];
    uint32_t sub_id = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_large"), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);

    for (uint32_t i = 0; i < STICKY_LARGE_LEN; i++) {
        payload[i] = (uint8_t) (i * 7U + 3U);
    }
    zassert_equal(event_publish_copy(STICKY_TYPE_A, EVENT_PRIORITY_NORMAL, payload, sizeof(payload)), EVENT_OK, NULL);

    zassert_equal(event_subscribe(STICKY_TYPE_A, sticky_capture_cb, &g_sink, &sub_id), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 1, NULL);
    zassert_equal(g_sink.last_len, STICKY_LARGE_LEN, "重放负载长度应一致");
    zassert_equal(memcmp(g_sink.last_payload, payload, STICKY_LARGE_LEN), 0, "重放负载内容应逐字节一致");

    /* 常规投递：队列中的原事件（200B 动态负载）随后由分发器释放 */
    sticky_drain(1U);
    zassert_equal(atomic_get(&g_sink.calls), 2, "常规投递仍应到达");
    zassert_equal(memcmp(g_sink.last_payload, payload, STICKY_LARGE_LEN), 0, NULL);
}

/**
 * @brief disable 释放锁存副本无泄漏，槽位可复用
 */
ZTEST(test_event_sticky, test_sticky_disable_releases_no_leak) {
    uint8_t  payload[STICKY_LARGE_LEN];
    uint32_t leak_base = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_dis"), EVENT_OK, NULL);
    leak_base = STICKY_LEAK_SNAPSHOT();

    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);
    for (uint32_t i = 0; i < STICKY_LARGE_LEN; i++) {
        payload[i] = (uint8_t) (i + 1U);
    }
    zassert_equal(event_publish_copy(STICKY_TYPE_A, EVENT_PRIORITY_NORMAL, payload, sizeof(payload)), EVENT_OK, NULL);
    sticky_drain(1U);
    zassert_true(event_sticky_is_latched(STICKY_TYPE_A), NULL);

    zassert_equal(event_sticky_disable(STICKY_TYPE_A), EVENT_OK, NULL);
    zassert_false(event_sticky_is_latched(STICKY_TYPE_A), NULL);
#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
    zassert_equal(event_check_leaks(), leak_base, "disable 后锁存副本应全部释放");
#endif

    /* 槽位已归还：可再次启用 */
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, "disable 后槽位应可复用");
    zassert_equal(event_sticky_disable(STICKY_TYPE_A), EVENT_OK, NULL);
#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
    zassert_equal(event_check_leaks(), leak_base, NULL);
#endif
}

/**
 * @brief 槽池耗尽：第 5 个类型启用返回 NOT_FOUND；释放后可再启用
 */
ZTEST(test_event_sticky, test_sticky_slot_pool_exhausted_not_found) {
    const uint8_t types[5] = {STICKY_TYPE_A, STICKY_TYPE_B, STICKY_TYPE_C, STICKY_TYPE_D, STICKY_TYPE_E};
    uint32_t      latched = 0U;
    uint32_t      miss = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_pool_a"), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_B, "stk_pool_b"), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_C, "stk_pool_c"), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_D, "stk_pool_d"), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_E, "stk_pool_e"), EVENT_OK, NULL);

    for (uint32_t i = 0; i < 4U; i++) {
        zassert_equal(event_sticky_enable(types[i]), EVENT_OK, "enable #%u should succeed", i);
    }
    zassert_equal(event_sticky_enable(types[4]), EVENT_ERR_NOT_FOUND, "第 5 个类型应返回 NOT_FOUND");

    event_sticky_get_stats(&latched, &miss);
    zassert_equal(latched, 0U, "未发布不应有锁存沿");
    zassert_equal(miss, 0U, NULL);

    /* 释放一个槽后可再启用 */
    zassert_equal(event_sticky_disable(STICKY_TYPE_B), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_E), EVENT_OK, "释放槽位后应可启用");

    /* 收尾归还全部槽位，避免影响后续用例 */
    zassert_equal(event_sticky_disable(STICKY_TYPE_A), EVENT_OK, NULL);
    zassert_equal(event_sticky_disable(STICKY_TYPE_C), EVENT_OK, NULL);
    zassert_equal(event_sticky_disable(STICKY_TYPE_D), EVENT_OK, NULL);
    zassert_equal(event_sticky_disable(STICKY_TYPE_E), EVENT_OK, NULL);
}

/**
 * @brief shutdown 释放全部锁存负载无泄漏；重新 init 后统计清零、槽位可复用
 */
ZTEST(test_event_sticky, test_sticky_shutdown_releases_all_no_leak) {
    uint8_t  payload[STICKY_LARGE_LEN];
    event_t  empty_event = {0};
    uint32_t leak_base = 0U;
    uint32_t latched = 0U;
    uint32_t miss = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_sd_a"), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_B, "stk_sd_b"), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_C, "stk_sd_c"), EVENT_OK, NULL);
    leak_base = STICKY_LEAK_SNAPSHOT();

    /* 三种锁存形态：内联负载 / 200B 动态负载 / 空负载 */
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_B), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_C), EVENT_OK, NULL);

    sticky_publish_u32(STICKY_TYPE_A, 0xA5A5A5A5U);
    for (uint32_t i = 0; i < STICKY_LARGE_LEN; i++) {
        payload[i] = (uint8_t) (i ^ 0x5AU);
    }
    zassert_equal(event_publish_copy(STICKY_TYPE_B, EVENT_PRIORITY_NORMAL, payload, sizeof(payload)), EVENT_OK, NULL);
    empty_event.type = STICKY_TYPE_C;
    empty_event.priority = EVENT_PRIORITY_NORMAL;
    empty_event.timestamp = k_uptime_get_32();
    zassert_equal(event_publish(&empty_event), EVENT_OK, "空负载事件应可发布");
    sticky_drain(3U);

    event_sticky_get_stats(&latched, &miss);
    zassert_equal(latched, 3U, "三种形态均应持有锁存沿");
    zassert_true(event_sticky_is_latched(STICKY_TYPE_A), NULL);
    zassert_true(event_sticky_is_latched(STICKY_TYPE_B), NULL);
    zassert_true(event_sticky_is_latched(STICKY_TYPE_C), NULL);

    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
    zassert_equal(event_check_leaks(), leak_base, "shutdown 后锁存负载应全部释放");
#endif

    /* 重新 init：sticky 统计清零、槽位全部可复用 */
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    event_sticky_get_stats(&latched, &miss);
    zassert_equal(latched, 0U, "shutdown 后不应残留锁存沿");
    zassert_equal(miss, 0U, NULL);

    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_sd_a2"), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, "槽位应可复用");
    zassert_equal(event_sticky_disable(STICKY_TYPE_A), EVENT_OK, NULL);
}

/* =============================================================================
 * 重放让位与 pending 收尾（B2 复审补测）
 * ============================================================================= */

/**
 * @brief 重放回调内对同类型发布：发布成功、计 miss、锁存沿保持旧值
 */
ZTEST(test_event_sticky, test_sticky_publish_inside_replay_miss_keeps_edge) {
    uint32_t sub1 = 0U;
    uint32_t sub2 = 0U;
    uint32_t latched = 0U;
    uint32_t miss_before = 0U;
    uint32_t miss_after = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_rp_pub"), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);

    sticky_publish_u32(STICKY_TYPE_A, 0x1111U);
    sticky_drain(1U);
    event_sticky_get_stats(&latched, &miss_before);

    atomic_set(&g_replay_api_ret, -1);
    atomic_set(&g_replay_arm, 1); /* 仅重放那一次触发回调内发布 */
    zassert_equal(event_subscribe(STICKY_TYPE_A, sticky_replay_publish_cb, &g_sink, &sub1), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 1, "重放应发生一次");
    zassert_equal((event_status_t) atomic_get(&g_replay_api_ret), EVENT_OK, "重放回调内发布应成功入队");
    event_sticky_get_stats(&latched, &miss_after);
    zassert_equal(miss_after, miss_before + 1U, "重放期间同类型发布应让位计 miss");
    zassert_true(event_sticky_is_latched(STICKY_TYPE_A), "让位后旧沿应保留");

    sticky_drain(1U); /* 排空回调内发布的事件 */

    /* 新订阅者重放到的仍是旧沿：回调内发布不得覆盖锁存 */
    sticky_sink_reset();
    zassert_equal(event_subscribe(STICKY_TYPE_A, sticky_capture_cb, &g_sink, &sub2), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 1, NULL);
    zassert_equal(g_sink.last_u32, 0x1111U, "锁存沿应仍是旧值 0x1111");
}

/**
 * @brief 重放回调内 clear：返回 OK，回调返回后 is_latched 翻转、副本延迟释放无泄漏
 */
ZTEST(test_event_sticky, test_sticky_clear_inside_replay_pending_release) {
    uint8_t  payload[STICKY_LARGE_LEN];
    uint32_t sub_id = 0U;
    uint32_t leak_base = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_rp_clr"), EVENT_OK, NULL);
    /* 基线须取在锁存建立之前：event_check_leaks 统计在途分配数，
     * 锁存副本本身在 pending 排空后应归还，使计数回到基线 */
    leak_base = STICKY_LEAK_SNAPSHOT();
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);
    for (uint32_t i = 0; i < STICKY_LARGE_LEN; i++) {
        payload[i] = (uint8_t) (i * 11U + 7U);
    }
    zassert_equal(event_publish_copy(STICKY_TYPE_A, EVENT_PRIORITY_NORMAL, payload, sizeof(payload)), EVENT_OK, NULL);
    sticky_drain(1U);
    zassert_true(event_sticky_is_latched(STICKY_TYPE_A), NULL);

    atomic_set(&g_replay_api_ret, -1);
    zassert_equal(event_subscribe(STICKY_TYPE_A, sticky_replay_clear_cb, &g_sink, &sub_id), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 1, "重放应发生一次");
    zassert_equal((event_status_t) atomic_get(&g_replay_api_ret), EVENT_OK, "重放回调内 clear 应返回 OK");
    zassert_false(event_sticky_is_latched(STICKY_TYPE_A), "回调返回后 pending CLEAR 应已排空执行");
#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
    zassert_equal(event_check_leaks(), leak_base, "pending CLEAR 排空后锁存副本应释放无泄漏");
#endif
}

/**
 * @brief 重放回调内 disable：返回 OK，回调返回后槽位归还可复用、副本释放无泄漏
 */
ZTEST(test_event_sticky, test_sticky_disable_inside_replay_pending_release) {
    uint8_t  payload[STICKY_LARGE_LEN];
    uint32_t sub_id = 0U;
    uint32_t leak_base = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_rp_dis"), EVENT_OK, NULL);
    /* 基线须取在锁存建立之前（与 clear 用例同理） */
    leak_base = STICKY_LEAK_SNAPSHOT();
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);
    for (uint32_t i = 0; i < STICKY_LARGE_LEN; i++) {
        payload[i] = (uint8_t) (i * 5U + 1U);
    }
    zassert_equal(event_publish_copy(STICKY_TYPE_A, EVENT_PRIORITY_NORMAL, payload, sizeof(payload)), EVENT_OK, NULL);
    sticky_drain(1U);
    zassert_true(event_sticky_is_latched(STICKY_TYPE_A), NULL);

    atomic_set(&g_replay_api_ret, -1);
    zassert_equal(event_subscribe(STICKY_TYPE_A, sticky_replay_disable_cb, &g_sink, &sub_id), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 1, "重放应发生一次");
    zassert_equal((event_status_t) atomic_get(&g_replay_api_ret), EVENT_OK, "重放回调内 disable 应返回 OK");
    zassert_false(event_sticky_is_latched(STICKY_TYPE_A), "回调返回后 pending DISABLE 应已排空执行");
#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
    zassert_equal(event_check_leaks(), leak_base, "pending DISABLE 排空后副本应释放无泄漏");
#endif

    /* 槽位已随 pending DISABLE 归还：可再次启用并正常锁存 */
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, "pending DISABLE 后槽位应可复用");
    sticky_publish_u32(STICKY_TYPE_A, 0x7777U);
    zassert_true(event_sticky_is_latched(STICKY_TYPE_A), "重新启用后应恢复锁存");
    zassert_equal(event_sticky_disable(STICKY_TYPE_A), EVENT_OK, NULL);
    sticky_drain(1U); /* 排空重新启用期间的发布，保持队列干净 */
}

/**
 * @brief too-large 锁存 miss：4.1KB 发布成功、miss+1、旧沿保留
 */
ZTEST(test_event_sticky, test_sticky_too_large_latch_miss_keeps_old_edge) {
    /* static：4.1KB 超出 ZTEST 线程栈（3072B），不得放栈上 */
    static uint8_t big[STICKY_TOO_LARGE_LEN];
    uint32_t       sub_id = 0U;
    uint32_t       latched = 0U;
    uint32_t       miss_before = 0U;
    uint32_t       miss_after = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_big"), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);

    sticky_publish_u32(STICKY_TYPE_A, 0x2222U); /* 先建立旧沿 */
    sticky_drain(1U);
    zassert_true(event_sticky_is_latched(STICKY_TYPE_A), NULL);
    event_sticky_get_stats(&latched, &miss_before);

    /* 先订阅：订阅即重放旧沿，作为 too-large 不覆盖锁存的对照 */
    zassert_equal(event_subscribe(STICKY_TYPE_A, sticky_capture_cb, &g_sink, &sub_id), EVENT_OK, NULL);
    zassert_equal(atomic_get(&g_sink.calls), 1, "订阅应重放旧沿一次");
    zassert_equal(g_sink.last_u32, 0x2222U, NULL);

    memset(big, 0x5A, sizeof(big));
    zassert_equal(event_publish_copy(STICKY_TYPE_A, EVENT_PRIORITY_NORMAL, big, sizeof(big)), EVENT_OK,
                  "超锁存上界的发布本身应成功");
    event_sticky_get_stats(&latched, &miss_after);
    zassert_equal(miss_after, miss_before + 1U, "too-large 应计 sticky miss");
    zassert_true(event_sticky_is_latched(STICKY_TYPE_A), "锁存失败应保留旧沿");

    sticky_drain(1U); /* 4.1KB 事件照常入队投递 */
    zassert_equal(atomic_get(&g_sink.calls), 2, "4.1KB 事件应照常常规投递");
    zassert_equal(g_sink.last_len, STICKY_TOO_LARGE_LEN, "常规投递应完整到达");
}

/**
 * @brief 同类型 enable/disable 并发后，最终启用必须仍指向其自有槽位并可正常锁存
 */
ZTEST(test_event_sticky, test_sticky_enable_disable_concurrent_mapping_consistent) {
    k_tid_t enable_tid;
    k_tid_t disable_tid;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_toggle"), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_B, "stk_toggle_reuse"), EVENT_OK, NULL);
    k_sem_init(&g_sticky_toggle_start, 0, 2);
    atomic_set(&g_sticky_toggle_enable_error, 0);

    enable_tid =
        k_thread_create(&g_sticky_enable_thread, g_sticky_enable_stack, K_THREAD_STACK_SIZEOF(g_sticky_enable_stack),
                        sticky_enable_worker, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    disable_tid =
        k_thread_create(&g_sticky_disable_thread, g_sticky_disable_stack, K_THREAD_STACK_SIZEOF(g_sticky_disable_stack),
                        sticky_disable_worker, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    zassert_not_null(enable_tid, NULL);
    zassert_not_null(disable_tid, NULL);
    k_sem_give(&g_sticky_toggle_start);
    k_sem_give(&g_sticky_toggle_start);
    zassert_equal(k_thread_join(enable_tid, K_SECONDS(3)), 0, NULL);
    zassert_equal(k_thread_join(disable_tid, K_SECONDS(3)), 0, NULL);
    zassert_equal(atomic_get(&g_sticky_toggle_enable_error), 0, "并发 enable 不应失败");

    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);
    sticky_publish_u32(STICKY_TYPE_A, 0xA55AA55AU);
    zassert_true(event_sticky_is_latched(STICKY_TYPE_A), "最终 entry 必须映射到该类型拥有的槽位");
    zassert_equal(event_sticky_disable(STICKY_TYPE_A), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_B), EVENT_OK, "释放后的槽位必须可由其他类型复用");
    zassert_equal(event_sticky_disable(STICKY_TYPE_B), EVENT_OK, NULL);
    sticky_drain(1U);
}

/**
 * @brief 超过八个短生命周期重放线程后，递归防御仍应生效
 */
ZTEST(test_event_sticky, test_sticky_replay_track_released_after_thread_exit) {
    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_tracks"), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);
    sticky_publish_u32(STICKY_TYPE_A, 0x12345678U);
    sticky_drain(1U);
    atomic_set(&g_sticky_replay_callbacks, 0);
    atomic_set(&g_sticky_replay_recursive_callbacks, 0);
    atomic_set(&g_sticky_replay_callback_depth, 0);
    atomic_set(&g_sticky_replay_worker_error, 0);

    for (uint32_t i = 0U; i < STICKY_REPLAY_THREAD_COUNT; i++) {
        k_tid_t tid = k_thread_create(&g_sticky_replay_threads[i], g_sticky_replay_stacks[i],
                                      K_THREAD_STACK_SIZEOF(g_sticky_replay_stacks[i]), sticky_replay_worker, NULL,
                                      NULL, NULL, 5, 0, K_NO_WAIT);

        zassert_not_null(tid, "replay worker %u create failed", i);
        zassert_equal(k_thread_join(tid, K_SECONDS(2)), 0, "replay worker %u timeout", i);
    }
    zassert_equal(atomic_get(&g_sticky_replay_worker_error), 0, NULL);
    zassert_equal(atomic_get(&g_sticky_replay_callbacks), STICKY_REPLAY_THREAD_COUNT, "每个线程只能得到一次外层重放");
    zassert_equal(atomic_get(&g_sticky_replay_recursive_callbacks), 0, "跟踪槽归还后递归重放防御必须仍然生效");
    zassert_equal(event_sticky_disable(STICKY_TYPE_A), EVENT_OK, NULL);
}

/**
 * @brief 类型 255 不得与空闲哨兵别名：它占用真实槽，统计可见，且不会被后续 enable 偷走
 */
ZTEST(test_event_sticky, test_sticky_type_255_occupies_real_slot) {
    uint32_t latched = 0U;
    uint32_t miss = 0U;
    uint32_t sub_id = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(255, "stk_255"), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_255_a"), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_B, "stk_255_b"), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_C, "stk_255_c"), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_D, "stk_255_d"), EVENT_OK, NULL);

    zassert_equal(event_sticky_enable(255), EVENT_OK, NULL);
    sticky_publish_u32(255, 0x255U);
    zassert_true(event_sticky_is_latched(255), "类型 255 发布后应锁存");
    event_sticky_get_stats(&latched, &miss);
    zassert_equal(latched, 1U, "类型 255 的锁存必须计入统计，不能被当成空闲槽");

    zassert_equal(event_subscribe(255, sticky_capture_cb, &g_sink, &sub_id), EVENT_OK, NULL);
    zassert_equal(g_sink.last_u32, 0x255U, "类型 255 的锁存沿应能重放");

    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_B), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_C), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_D), EVENT_ERR_NOT_FOUND, "255 已占一槽，第 5 个类型应失败");
    zassert_true(event_sticky_is_latched(255), "后续 enable 不得偷走类型 255 的槽");

    zassert_equal(event_sticky_disable(255), EVENT_OK, NULL);
    zassert_false(event_sticky_is_latched(255), NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_D), EVENT_OK, "释放 255 的槽后应可复用");
    sticky_drain(1U);
}

/**
 * @brief 槽池已满时，重放回调里 disable 后立刻 enable 必须复用原槽
 */
ZTEST(test_event_sticky, test_sticky_reenable_during_pending_disable_reclaims_slot) {
    uint32_t sub_id = 0U;

    sticky_test_reset();
    sticky_sink_reset();
    zassert_equal(event_register_type(STICKY_TYPE_A, "stk_re_a"), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_B, "stk_re_b"), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_C, "stk_re_c"), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_D, "stk_re_d"), EVENT_OK, NULL);
    zassert_equal(event_register_type(STICKY_TYPE_E, "stk_re_e"), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_A), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_B), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_C), EVENT_OK, NULL);
    zassert_equal(event_sticky_enable(STICKY_TYPE_D), EVENT_OK, NULL);

    sticky_publish_u32(STICKY_TYPE_A, 0xA1U);
    atomic_set(&g_replay_arm, 1);
    atomic_set(&g_sticky_reenable_disable_ret, -1);
    atomic_set(&g_sticky_reenable_enable_ret, -1);
    zassert_equal(event_subscribe(STICKY_TYPE_A, sticky_replay_reenable_cb, NULL, &sub_id), EVENT_OK, NULL);
    zassert_equal((event_status_t) atomic_get(&g_sticky_reenable_disable_ret), EVENT_OK, NULL);
    zassert_equal((event_status_t) atomic_get(&g_sticky_reenable_enable_ret), EVENT_OK,
                  "满池且 disable 尚未收尾时，再次 enable 应复用原槽");
    zassert_equal(event_sticky_enable(STICKY_TYPE_E), EVENT_ERR_NOT_FOUND, "复用原槽后池子仍应是满的");

    sticky_publish_u32(STICKY_TYPE_A, 0xA2U);
    zassert_true(event_sticky_is_latched(STICKY_TYPE_A), "复用后的槽应能重新锁存");
    zassert_equal(event_sticky_disable(STICKY_TYPE_B), EVENT_OK, NULL);
    zassert_equal(event_sticky_disable(STICKY_TYPE_C), EVENT_OK, NULL);
    zassert_equal(event_sticky_disable(STICKY_TYPE_D), EVENT_OK, NULL);
    zassert_equal(event_sticky_disable(STICKY_TYPE_A), EVENT_OK, NULL);
    sticky_drain(2U);
}

ZTEST_SUITE(a_event_sticky_cold, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(test_event_sticky, NULL, NULL, NULL, NULL, NULL);
