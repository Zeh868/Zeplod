/**
 * @file test_event_timer.c
 * @brief 延迟发布与周期发布（CONFIG_EVENT_TIMER_POOL）单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-09-27
 *
 * 仅当 CONFIG_EVENT_TIMER_POOL=y 时由 tests/CMakeLists.txt 编入；
 * 运行需叠加 tests/prj_event_ext.conf（CONF_FILE 三段叠加）。
 *
 * 时序注意：qemu_riscv32 默认 100Hz tick（k_msleep 量化 10ms），全部等待断言
 * 使用宽容窗口（如 60ms 周期 3s 窗口内要求 fire>=20）。
 * slab 耗尽负测利用 tests/prj.conf 的最小事件壳池（CONFIG_EVENT_SLAB_NORMAL_COUNT=4，
 * 大块数据池关闭）压满 NORMAL 壳池制造 event_create_with_data_rt 失败。
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>
#include <string.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include <zeplod/event_timer.h>
#include "event_memory.h"

LOG_MODULE_REGISTER(test_event_timer);

/* 本套件专用事件类型（各用例先 shutdown 再注册，避免与其他套件残留注册冲突） */
#define TIMER_TYPE_MAIN 240U
#define TIMER_TYPE_AUX  241U
#define TIMER_TYPE_THIRD 242U

/** 延迟基本投递时长（毫秒）：5 tick，远大于量化误差 */
#define TIMER_DELAY_BASIC_MS 50U

/** 取消后观察无投递的窗口（毫秒，虚拟时钟下瞬时度过） */
#define TIMER_CANCEL_OBSERVE_MS 2000U

/** 超内联负载用例长度：超过内联容量（48B）且低于 4KB 上界 */
#define TIMER_LARGE_LEN 200U

/** 负载上界负测长度：延迟/周期负载上界为 4096B（API 契约），4097 应被拒 */
#define TIMER_PAYLOAD_OVER_MAX 4097U

/** 周期基本周期（毫秒）：6 tick */
#define TIMER_PERIOD_BASIC_MS 60U

/** 周期速率窗口（毫秒）：60ms 周期下期望约 50 轮，取 20 为宽容下界 */
#define TIMER_PERIOD_WINDOW_MS 3000U
#define TIMER_PERIOD_MIN_FIRES 20

/** 回调负载快照缓冲（须覆盖 TIMER_LARGE_LEN 用例） */
#define TIMER_SINK_BUF_LEN 256U

/** 接收顺序记录容量 */
#define TIMER_SEQ_MAX 16U

/** 泄漏断言基线快照（未开 CONFIG_EVENT_DEBUG_MEM 时恒 0，断言退化为不校验） */
#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
#define TIMER_LEAK_SNAPSHOT() event_check_leaks()
#else
#define TIMER_LEAK_SNAPSHOT() 0U
#endif

typedef struct {
    atomic_t calls;                             /**< 回调被调用次数 */
    uint32_t last_u32;                          /**< 最近一次 4 字节载荷 */
    uint32_t last_len;                          /**< 最近一次负载长度（字节） */
    uint32_t first_ms;                          /**< 首次回调时刻（毫秒 uptime） */
    bool     mismatch;                          /**< 多轮载荷出现不一致 */
    uint32_t seq[TIMER_SEQ_MAX];                /**< 4 字节载荷接收顺序 */
    uint32_t seq_len;                           /**< 已记录条数 */
    uint8_t  last_payload[TIMER_SINK_BUF_LEN];  /**< 最近一次负载副本 */
} timer_sink_t;

static timer_sink_t g_sink_a;
static timer_sink_t g_sink_b;

/* =============================================================================
 * 测试辅助
 * ============================================================================= */

/** @brief 回到已知初始态：shutdown → init/start → dispatcher init/start → 统计复位
 *
 * 须显式 event_dispatcher_start()：shutdown 清除 restart 标记后，
 * event_system_start() 不会自动拉起分发线程，队列将无人消费。
 */
static void timer_test_reset(void) {
    zassert_equal(event_system_shutdown(), EVENT_OK, "shutdown failed");
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);
    event_dispatcher_reset_stats();
}

static void timer_sink_reset(timer_sink_t* sink) {
    atomic_set(&sink->calls, 0);
    sink->last_u32 = 0U;
    sink->last_len = 0U;
    sink->first_ms = 0U;
    sink->mismatch = false;
    sink->seq_len = 0U;
    memset(sink->seq, 0, sizeof(sink->seq));
    memset(sink->last_payload, 0, sizeof(sink->last_payload));
}

/** @brief 投递/常规消费共用回调：记录次数、顺序、载荷一致性（浅拷贝，只读 event） */
static void timer_capture_cb(const event_t* event, void* user_data) {
    timer_sink_t* sink = (timer_sink_t*) user_data;

    zassert_not_null(sink, "user_data 不得为 NULL");
    atomic_inc(&sink->calls); /* Zephyr atomic_inc 返回旧值：改用 get 判定首次回调 */
    if (atomic_get(&sink->calls) == 1) {
        sink->first_ms = k_uptime_get_32();
    }
    if (event->data_len == sizeof(uint32_t)) {
        uint32_t value = 0U;

        if ((event->flags & EVENT_FLAG_DATA_INLINE) != 0U) {
            memcpy(&value, event->data.inline_data, sizeof(value));
        } else if (event->data.ptr != NULL) {
            memcpy(&value, event->data.ptr, sizeof(value));
        }
        sink->last_u32 = value;
        if (sink->seq_len == 0U) {
            sink->seq[0] = value;
            sink->seq_len = 1U;
        } else if (sink->seq_len < TIMER_SEQ_MAX) {
            sink->seq[sink->seq_len++] = value;
        }
        if (value != sink->seq[0]) {
            sink->mismatch = true; /* 每轮载荷必须一致 */
        }
    }
    sink->last_len = event->data_len;
    if (event->data_len > 0U && event->data_len <= TIMER_SINK_BUF_LEN) {
        if ((event->flags & EVENT_FLAG_DATA_INLINE) != 0U) {
            memcpy(sink->last_payload, event->data.inline_data, event->data_len);
        } else if (event->data.ptr != NULL) {
            memcpy(sink->last_payload, event->data.ptr, event->data_len);
        }
    }
}

/** @brief 等待计数器达到目标（基于 uptime 的宽容窗口），返回是否达成 */
static bool timer_wait_calls(const atomic_t* counter, int32_t target, uint32_t timeout_ms) {
    int64_t deadline = k_uptime_get() + (int64_t) timeout_ms;

    while (atomic_get(counter) < target) {
        if (k_uptime_get() >= deadline) {
            return false;
        }
        k_sleep(K_MSEC(10));
    }
    return true;
}

/** @brief 订阅辅助：内部消化 subscriber_id 输出（本套件用例不关心该 ID） */
static event_status_t timer_subscribe(event_type_t type, timer_sink_t* sink) {
    uint32_t sub_id = 0U;

    return event_subscribe(type, timer_capture_cb, sink, &sub_id);
}

/** 零长投递回归（M-1）：data_len 非 0 的轮次计数 */
static atomic_t g_zero_len_bad_rounds;

/** @brief 零长投递专用回调：任一轮 data_len 非 0 即记违规（M-1 回归） */
static void timer_zero_len_cb(const event_t* event, void* user_data) {
    timer_sink_t* sink = (timer_sink_t*) user_data;

    if (event->data_len != 0U) {
        atomic_inc(&g_zero_len_bad_rounds);
    }
    atomic_inc(&sink->calls);
    sink->last_len = event->data_len;
}

/* =============================================================================
 * 延迟发布
 * ============================================================================= */

/**
 * @brief 50ms 基本投递：载荷逐字节一致、pending 归零、fire 统计正确
 */
ZTEST(test_event_timer, test_timer_delayed_basic_delivery) {
    uint32_t             value = 0x13572468U;
    event_timer_handle_t handle = EVENT_TIMER_HANDLE_INVALID;
    uint32_t             fire = 0U;
    uint32_t             miss = 0U;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "tmr_basic"), EVENT_OK, NULL);
    zassert_equal(timer_subscribe(TIMER_TYPE_MAIN, &g_sink_a), EVENT_OK, NULL);

    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value),
                                             TIMER_DELAY_BASIC_MS, &handle),
                  EVENT_OK, NULL);
    zassert_true(handle.slot < CONFIG_EVENT_TIMER_SLOTS, "句柄应有效");
    zassert_equal(event_timer_pending_count(), 1U, "入槽后 pending 应为 1");

    zassert_true(timer_wait_calls(&g_sink_a.calls, 1, 1000U), "50ms 延迟后应投递");
    zassert_equal(g_sink_a.last_u32, value, "载荷应逐字节一致");
    zassert_equal(g_sink_a.last_len, sizeof(value), NULL);
    zassert_equal(event_timer_pending_count(), 0U, "投递后 pending 应归零");
    zassert_equal(event_timer_get_stats(handle, &fire, &miss), EVENT_ERR_NOT_FOUND,
                  "一次性槽投递后句柄已失效");
    zassert_equal(event_timer_miss_count(), 0U, "正常投递不应计 miss");
}

/**
 * @brief 投递顺序：A(100ms) 后发 B(40ms)，B 先到
 */
ZTEST(test_event_timer, test_timer_delayed_ordering) {
    const uint32_t       val_a = 0xAAAA0001U;
    const uint32_t       val_b = 0xBBBB0002U;
    event_timer_handle_t handle_a = EVENT_TIMER_HANDLE_INVALID;
    event_timer_handle_t handle_b = EVENT_TIMER_HANDLE_INVALID;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "tmr_order"), EVENT_OK, NULL);
    zassert_equal(timer_subscribe(TIMER_TYPE_MAIN, &g_sink_a), EVENT_OK, NULL);

    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &val_a, sizeof(val_a), 100U,
                                             &handle_a),
                  EVENT_OK, NULL);
    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &val_b, sizeof(val_b), 40U,
                                             &handle_b),
                  EVENT_OK, NULL);

    zassert_true(timer_wait_calls(&g_sink_a.calls, 2, 1000U), "两条延迟事件都应投递");
    zassert_equal(g_sink_a.seq_len, 2U, NULL);
    zassert_equal(g_sink_a.seq[0], val_b, "B(40ms) 应先于 A(100ms) 到达");
    zassert_equal(g_sink_a.seq[1], val_a, NULL);
}

/**
 * @brief 取消后 2s 无投递且 pending 归零
 */
ZTEST(test_event_timer, test_timer_delayed_cancel_no_delivery) {
    uint32_t             value = 0x00C0FFEEU;
    event_timer_handle_t handle = EVENT_TIMER_HANDLE_INVALID;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "tmr_cancel"), EVENT_OK, NULL);
    zassert_equal(timer_subscribe(TIMER_TYPE_MAIN, &g_sink_a), EVENT_OK, NULL);

    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value),
                                             TIMER_CANCEL_OBSERVE_MS + 500U, &handle),
                  EVENT_OK, NULL);
    zassert_equal(event_delayed_cancel(handle), EVENT_OK, NULL);
    zassert_equal(event_timer_pending_count(), 0U, "取消后 pending 应归零");

    k_sleep(K_MSEC(TIMER_CANCEL_OBSERVE_MS));
    zassert_equal(atomic_get(&g_sink_a.calls), 0, "取消后不应有投递");

    /* 重复取消同一句柄：NOT_FOUND */
    zassert_equal(event_delayed_cancel(handle), EVENT_ERR_NOT_FOUND, NULL);
}

/**
 * @brief 到期投递完成后取消返回 NOT_FOUND（负）
 */
ZTEST(test_event_timer, test_timer_cancel_after_fired_not_found) {
    uint32_t             value = 7U;
    event_timer_handle_t handle = EVENT_TIMER_HANDLE_INVALID;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "tmr_late"), EVENT_OK, NULL);
    zassert_equal(timer_subscribe(TIMER_TYPE_MAIN, &g_sink_a), EVENT_OK, NULL);

    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value),
                                             TIMER_DELAY_BASIC_MS, &handle),
                  EVENT_OK, NULL);
    zassert_true(timer_wait_calls(&g_sink_a.calls, 1, 1000U), "应先正常投递");
    zassert_equal(event_delayed_cancel(handle), EVENT_ERR_NOT_FOUND, "已消费的槽取消应返回 NOT_FOUND");
}

/**
 * @brief 8 槽占满后第 9 个提交返回 NO_MEM；释放后可再提交（负）
 */
ZTEST(test_event_timer, test_timer_slots_exhausted_no_mem) {
    uint32_t             value = 1U;
    event_timer_handle_t handles[CONFIG_EVENT_TIMER_SLOTS] = {0};

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "tmr_full"), EVENT_OK, NULL);

    for (uint32_t i = 0; i < CONFIG_EVENT_TIMER_SLOTS; i++) {
        zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value),
                                                 30000U, &handles[i]),
                      EVENT_OK, "提交 #%u 应成功", i);
    }
    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value), 30000U,
                                             NULL),
                  EVENT_ERR_NO_MEM, "第 9 个提交应返回 NO_MEM");
    zassert_equal(event_timer_pending_count(), CONFIG_EVENT_TIMER_SLOTS, NULL);

    for (uint32_t i = 0; i < CONFIG_EVENT_TIMER_SLOTS; i++) {
        zassert_equal(event_delayed_cancel(handles[i]), EVENT_OK, NULL);
    }
    zassert_equal(event_timer_pending_count(), 0U, NULL);
    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value), 30000U,
                                             NULL),
                  EVENT_OK, "释放槽位后应可再提交");
}

/**
 * @brief stop 后提交返回 NOT_RUNNING（负）
 */
ZTEST(test_event_timer, test_timer_submit_after_stop_not_running) {
    uint32_t value = 2U;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "tmr_stop"), EVENT_OK, NULL);

    zassert_equal(event_system_stop(), EVENT_OK, NULL);
    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value),
                                             TIMER_DELAY_BASIC_MS, NULL),
                  EVENT_ERR_NOT_RUNNING, "stop 后提交应被拒");
    zassert_equal(event_system_start(), EVENT_OK, NULL);
}

/**
 * @brief armed 后 stop：到期无投递、miss 增长、槽回收
 */
ZTEST(test_event_timer, test_timer_armed_stop_expire_miss) {
    uint32_t value = 3U;
    uint32_t miss_before = 0U;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "tmr_miss"), EVENT_OK, NULL);
    zassert_equal(timer_subscribe(TIMER_TYPE_MAIN, &g_sink_a), EVENT_OK, NULL);
    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value), 100U,
                                             NULL),
                  EVENT_OK, NULL);

    miss_before = event_timer_miss_count();
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
    k_sleep(K_MSEC(400)); /* 越过 100ms 到期点：投递被 running=0 拒绝计 miss */
    zassert_equal(atomic_get(&g_sink_a.calls), 0, "stop 期间不应投递");
    zassert_true(event_timer_miss_count() > miss_before, "到期被拒应计 miss");
    zassert_equal(event_timer_pending_count(), 0U, "一次性槽到期后即回收");
    zassert_equal(event_system_start(), EVENT_OK, NULL);
}

/**
 * @brief 8 个 armed 槽 shutdown 全取消无泄漏；重新 init 后槽可复用
 */
ZTEST(test_event_timer, test_timer_shutdown_cancels_all_no_leak) {
    uint8_t  large[TIMER_LARGE_LEN];
    uint32_t leak_base = 0U;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "tmr_sd"), EVENT_OK, NULL);
    leak_base = TIMER_LEAK_SNAPSHOT();

    for (uint32_t i = 0; i < CONFIG_EVENT_TIMER_SLOTS; i++) {
        /* 交替提交内联/超内联负载：两种槽内负载形态都被 shutdown 释放 */
        if ((i & 1U) == 0U) {
            uint32_t value = i;

            zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value),
                                                     30000U, NULL),
                          EVENT_OK, NULL);
        } else {
            for (uint32_t j = 0; j < TIMER_LARGE_LEN; j++) {
                large[j] = (uint8_t) (i * 31U + j);
            }
            zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, large, sizeof(large),
                                                     30000U, NULL),
                          EVENT_OK, NULL);
        }
    }
    zassert_equal(event_timer_pending_count(), CONFIG_EVENT_TIMER_SLOTS, NULL);

    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
    zassert_equal(event_check_leaks(), leak_base, "shutdown 后槽内负载应全部释放");
#endif

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_timer_pending_count(), 0U, "重启后不应有残留槽");

    uint32_t value = 0U;

    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "tmr_sd2"), EVENT_OK, NULL);
    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value), 30000U,
                                             NULL),
                  EVENT_OK, "槽位应可复用");
}

/**
 * @brief handle_out=NULL 与 delay_ms=0（按 1ms 处理）语义
 */
ZTEST(test_event_timer, test_timer_handle_null_and_zero_delay) {
    uint32_t value = 0x0D0F0001U;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "tmr_null"), EVENT_OK, NULL);
    zassert_equal(timer_subscribe(TIMER_TYPE_MAIN, &g_sink_a), EVENT_OK, NULL);

    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value), 0U, NULL),
                  EVENT_OK, "handle_out=NULL 且 delay 0 应成功（0 按 1ms 处理）");
    zassert_true(timer_wait_calls(&g_sink_a.calls, 1, 1000U), "delay 0 应按 1ms 立即投递");
    zassert_equal(g_sink_a.last_u32, value, NULL);
}

/**
 * @brief 非法参数负测：data/len 不一致、超 4KB、未注册类型、非法句柄
 */
ZTEST(test_event_timer, test_timer_invalid_args) {
    /* static：4097B 超出 ZTEST 线程栈（3072B），不得放栈上 */
    static uint8_t big[TIMER_PAYLOAD_OVER_MAX];

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "tmr_inv"), EVENT_OK, NULL);

    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, NULL, 4U, 100U, NULL),
                  EVENT_ERR_INVALID_ARG, "data=NULL 且 len>0 应拒绝");
    memset(big, 0xA5, sizeof(big));
    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, big, sizeof(big), 100U, NULL),
                  EVENT_ERR_INVALID_ARG, "len 超 4096 应拒绝");
    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_AUX, EVENT_PRIORITY_NORMAL, NULL, 0U, 100U, NULL),
                  EVENT_ERR_NOT_FOUND, "未注册类型应返回 NOT_FOUND");
    zassert_equal(event_delayed_cancel(EVENT_TIMER_HANDLE_INVALID), EVENT_ERR_INVALID_ARG,
                  "无效句柄格式应返回 INVALID_ARG");
}

/**
 * @brief 200B 超内联负载路径：投递内容逐字节一致、无泄漏
 */
ZTEST(test_event_timer, test_timer_delayed_large_payload) {
    uint8_t  payload[TIMER_LARGE_LEN];
    uint32_t leak_base = 0U;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "tmr_large"), EVENT_OK, NULL);
    zassert_equal(timer_subscribe(TIMER_TYPE_MAIN, &g_sink_a), EVENT_OK, NULL);
    leak_base = TIMER_LEAK_SNAPSHOT();

    for (uint32_t i = 0; i < TIMER_LARGE_LEN; i++) {
        payload[i] = (uint8_t) (i * 7U + 3U);
    }
    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, payload, sizeof(payload),
                                             TIMER_DELAY_BASIC_MS, NULL),
                  EVENT_OK, NULL);

    zassert_true(timer_wait_calls(&g_sink_a.calls, 1, 1000U), "大负载延迟投递应到达");
    zassert_equal(g_sink_a.last_len, TIMER_LARGE_LEN, NULL);
    zassert_equal(memcmp(g_sink_a.last_payload, payload, TIMER_LARGE_LEN), 0, "大负载内容应逐字节一致");

    k_sleep(K_MSEC(100)); /* 等分发器释放队列副本 */
#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
    zassert_equal(event_check_leaks(), leak_base, "投递完成后负载应全部释放");
#endif
}

/* =============================================================================
 * 周期发布
 * ============================================================================= */

/**
 * @brief 60ms 周期：3s 窗口内 fire>=20 且每轮载荷一致
 */
ZTEST(test_event_timer, test_periodic_basic_rate_and_payload) {
    uint32_t             value = 0x50117500U;
    event_timer_handle_t handle = EVENT_TIMER_HANDLE_INVALID;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "prd_basic"), EVENT_OK, NULL);
    zassert_equal(timer_subscribe(TIMER_TYPE_MAIN, &g_sink_a), EVENT_OK, NULL);
    zassert_equal(event_periodic_active_count(), 0U, NULL);

    zassert_equal(event_periodic_start(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value),
                                       TIMER_PERIOD_BASIC_MS, 0U, &handle),
                  EVENT_OK, NULL);
    zassert_equal(event_periodic_active_count(), 1U, NULL);

    zassert_true(timer_wait_calls(&g_sink_a.calls, TIMER_PERIOD_MIN_FIRES, TIMER_PERIOD_WINDOW_MS),
                 "3s 内应至少投递 %d 轮", TIMER_PERIOD_MIN_FIRES);
    zassert_false(g_sink_a.mismatch, "每轮载荷应一致");
    zassert_equal(g_sink_a.last_u32, value, NULL);

    zassert_equal(event_periodic_cancel(handle), EVENT_OK, NULL);
    zassert_equal(event_periodic_active_count(), 0U, NULL);
    zassert_equal(event_timer_pending_count(), 0U, NULL);
}

/**
 * @brief initial_delay=0 首投在 period_ms 时刻，initial_delay=200 首投显著推后
 */
ZTEST(test_event_timer, test_periodic_initial_delay_zero_vs_200) {
    uint32_t             value = 9U;
    event_timer_handle_t handle_now = EVENT_TIMER_HANDLE_INVALID;
    event_timer_handle_t handle_late = EVENT_TIMER_HANDLE_INVALID;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    timer_sink_reset(&g_sink_b);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "prd_now"), EVENT_OK, NULL);
    zassert_equal(event_register_type(TIMER_TYPE_AUX, "prd_late"), EVENT_OK, NULL);
    zassert_equal(timer_subscribe(TIMER_TYPE_MAIN, &g_sink_a), EVENT_OK, NULL);
    zassert_equal(timer_subscribe(TIMER_TYPE_AUX, &g_sink_b), EVENT_OK, NULL);

    zassert_equal(event_periodic_start(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value), 100U, 0U,
                                       &handle_now),
                  EVENT_OK, NULL);
    zassert_equal(event_periodic_start(TIMER_TYPE_AUX, EVENT_PRIORITY_NORMAL, &value, sizeof(value), 100U, 200U,
                                       &handle_late),
                  EVENT_OK, NULL);

    zassert_true(timer_wait_calls(&g_sink_b.calls, 1, 2000U), "initial 200ms 首投应到达");
    /* k_timer 不提前触发：a 首投 >= period(100ms)，b 首投 >= initial(200ms)；
     * 断言用下界 + 严格先后，不含分发延迟抖动（tick 10ms 留余量） */
    zassert_true(g_sink_a.first_ms >= 60U, "initial=0 首投不应早于 period 下界");
    zassert_true(g_sink_b.first_ms >= 150U, "initial=200 首投不应早于 initial 下界");
    zassert_true(g_sink_b.first_ms > g_sink_a.first_ms, "initial=200 首投应晚于 initial=0");

    zassert_equal(event_periodic_cancel(handle_now), EVENT_OK, NULL);
    zassert_equal(event_periodic_cancel(handle_late), EVENT_OK, NULL);
}

/**
 * @brief 取消后无新投递、active_count 减一、母本释放无泄漏
 */
ZTEST(test_event_timer, test_periodic_cancel_no_delivery_no_leak) {
    uint32_t             value = 0x0CA110U;
    uint32_t             leak_base = 0U;
    int32_t              calls_at_cancel = 0;
    event_timer_handle_t handle = EVENT_TIMER_HANDLE_INVALID;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "prd_cancel"), EVENT_OK, NULL);
    zassert_equal(timer_subscribe(TIMER_TYPE_MAIN, &g_sink_a), EVENT_OK, NULL);
    leak_base = TIMER_LEAK_SNAPSHOT();

    zassert_equal(event_periodic_start(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value), 50U, 0U,
                                       &handle),
                  EVENT_OK, NULL);
    zassert_true(timer_wait_calls(&g_sink_a.calls, 3, 2000U), "取消前应有多轮投递");
    zassert_false(g_sink_a.mismatch, "每轮载荷应与母本一致");

    calls_at_cancel = atomic_get(&g_sink_a.calls);
    zassert_equal(event_periodic_cancel(handle), EVENT_OK, NULL);
    zassert_equal(event_periodic_active_count(), 0U, "取消后 active 应减一");

    k_sleep(K_MSEC(300));
    zassert_equal(atomic_get(&g_sink_a.calls), calls_at_cancel, "取消后不应再有投递");
#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
    zassert_equal(event_check_leaks(), leak_base, "取消后母本应释放");
#endif

    /* 场景 B：超内联母本（提交时 k_malloc 私有副本）。测试配置未开大块 slab，
     * 每轮副本生成失败仅计 miss，但母本必须随 cancel 释放且无泄漏。 */
    uint8_t master[TIMER_LARGE_LEN];

    for (uint32_t i = 0; i < TIMER_LARGE_LEN; i++) {
        master[i] = (uint8_t) (i ^ 0x3CU);
    }
    zassert_equal(event_periodic_start(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, master, sizeof(master), 50U, 0U,
                                       &handle),
                  EVENT_OK, "超内联母本入槽应成功");
    k_sleep(K_MSEC(200)); /* 数轮 miss：母本持续持有 */
    zassert_equal(event_periodic_cancel(handle), EVENT_OK, NULL);
#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
    zassert_equal(event_check_leaks(), leak_base, "取消后超内联母本应释放");
#endif
}

/**
 * @brief 取消后重提交复用槽且 gen 递增（防 ABA）
 */
ZTEST(test_event_timer, test_periodic_resubmit_gen_increment) {
    uint32_t             value = 5U;
    event_timer_handle_t handle1 = EVENT_TIMER_HANDLE_INVALID;
    event_timer_handle_t handle2 = EVENT_TIMER_HANDLE_INVALID;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "prd_gen"), EVENT_OK, NULL);

    zassert_equal(event_periodic_start(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value), 1000U, 0U,
                                       &handle1),
                  EVENT_OK, NULL);
    zassert_equal(event_periodic_cancel(handle1), EVENT_OK, NULL);
    zassert_equal(event_periodic_start(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value), 1000U, 0U,
                                       &handle2),
                  EVENT_OK, "取消后同槽应可复用");
    zassert_equal(handle2.slot, handle1.slot, "应复用同一槽位");
    zassert_true(handle2.gen != handle1.gen, "复用后 gen 应递增");
    zassert_equal(event_periodic_cancel(handle2), EVENT_OK, NULL);

    /* 旧句柄已失效：cancel/get_stats 均为 NOT_FOUND */
    zassert_equal(event_periodic_cancel(handle1), EVENT_ERR_NOT_FOUND, NULL);
    zassert_equal(event_timer_get_stats(handle1, NULL, NULL), EVENT_ERR_NOT_FOUND, NULL);
}

/**
 * @brief armed 周期跨越 stop/start：停止期间轮次计 miss，恢复后继续投递
 */
ZTEST(test_event_timer, test_periodic_stop_miss_then_start_resume) {
    uint32_t             value = 6U;
    uint32_t             miss_before = 0U;
    int32_t              calls_at_stop = 0;
    event_timer_handle_t handle = EVENT_TIMER_HANDLE_INVALID;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "prd_stop"), EVENT_OK, NULL);
    zassert_equal(timer_subscribe(TIMER_TYPE_MAIN, &g_sink_a), EVENT_OK, NULL);

    zassert_equal(event_periodic_start(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value), 100U, 0U,
                                       &handle),
                  EVENT_OK, NULL);
    zassert_true(timer_wait_calls(&g_sink_a.calls, 2, 2000U), "停止前应正常投递");

    calls_at_stop = atomic_get(&g_sink_a.calls);
    miss_before = event_timer_miss_count();
    zassert_equal(event_system_stop(), EVENT_OK, NULL);
    k_sleep(K_MSEC(500)); /* 约 5 轮全部被拒计 miss */
    zassert_equal(atomic_get(&g_sink_a.calls), calls_at_stop, "停止期间不应投递");
    zassert_true(event_timer_miss_count() > miss_before, "停止期间轮次应计 miss");
    zassert_equal(event_periodic_active_count(), 1U, "stop 不取消周期槽");

    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_true(timer_wait_calls(&g_sink_a.calls, calls_at_stop + 2, 2000U), "start 后应恢复投递");

    zassert_equal(event_periodic_cancel(handle), EVENT_OK, NULL);
}

/**
 * @brief 周期槽 shutdown 全停无泄漏
 */
ZTEST(test_event_timer, test_periodic_shutdown_cancels_all) {
    uint32_t leak_base = 0U;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "prd_sd"), EVENT_OK, NULL);
    leak_base = TIMER_LEAK_SNAPSHOT();

    for (uint32_t i = 0; i < 2U; i++) {
        uint32_t value = i + 1U;

        zassert_equal(event_periodic_start(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value),
                                           30000U + i, 0U, NULL),
                      EVENT_OK, NULL);
    }
    zassert_equal(event_periodic_active_count(), 2U, NULL);

    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
    zassert_equal(event_check_leaks(), leak_base, "shutdown 后周期母本应全部释放");
#endif

    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_periodic_active_count(), 0U, NULL);
    zassert_equal(event_timer_pending_count(), 0U, NULL);
}

/**
 * @brief slab 耗尽该轮 miss、释放后下轮恢复（负）
 *
 * 压满 tests/prj.conf 的最小事件壳池（CONFIG_EVENT_SLAB_NORMAL_COUNT=4，
 * 大块数据池关闭）使 event_create_with_data_rt 失败，周期轮次跳过计 miss。
 */
ZTEST(test_event_timer, test_periodic_slab_exhausted_miss_recover) {
    event_t*             held[CONFIG_EVENT_SLAB_NORMAL_COUNT] = {0};
    uint32_t             value = 0x51AB0001U;
    event_timer_handle_t handle = EVENT_TIMER_HANDLE_INVALID;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "prd_slab"), EVENT_OK, NULL);
    zassert_equal(timer_subscribe(TIMER_TYPE_MAIN, &g_sink_a), EVENT_OK, NULL);

    /* 压满 NORMAL 事件壳池 */
    for (uint32_t i = 0; i < CONFIG_EVENT_SLAB_NORMAL_COUNT; i++) {
        held[i] = event_create_rt(TIMER_TYPE_THIRD, EVENT_PRIORITY_NORMAL);
        zassert_not_null(held[i], "前置：应能领到事件壳 #%u", i);
    }

    zassert_equal(event_periodic_start(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value), 60U, 0U,
                                       &handle),
                  EVENT_OK, "提交本身不分配事件壳，应成功");

    k_sleep(K_MSEC(500));
    zassert_equal(atomic_get(&g_sink_a.calls), 0, "壳池耗尽期间不应有投递");
    zassert_true(event_timer_miss_count() >= 3, "耗尽轮次应计 miss");
    zassert_equal(event_periodic_active_count(), 1U, "耗尽不应取消周期槽");

    for (uint32_t i = 0; i < CONFIG_EVENT_SLAB_NORMAL_COUNT; i++) {
        event_free(held[i]);
        held[i] = NULL;
    }
    zassert_true(timer_wait_calls(&g_sink_a.calls, 1, 2000U), "释放壳池后下轮应恢复投递");
    zassert_equal(g_sink_a.last_u32, value, NULL);

    zassert_equal(event_periodic_cancel(handle), EVENT_OK, NULL);
}

/**
 * @brief 周期参数负测：period 0/超界、data/len 不一致、未注册类型
 */
ZTEST(test_event_timer, test_periodic_invalid_args) {
    timer_test_reset();
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "prd_inv"), EVENT_OK, NULL);

    zassert_equal(event_periodic_start(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, NULL, 0U, 0U, 0U, NULL),
                  EVENT_ERR_INVALID_ARG, "period 0 应拒绝");
    zassert_equal(event_periodic_start(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, NULL, 0U, 0x80000000U, 0U, NULL),
                  EVENT_ERR_INVALID_ARG, "period 超界应拒绝");
    zassert_equal(event_periodic_start(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, NULL, 4U, 100U, 0U, NULL),
                  EVENT_ERR_INVALID_ARG, "data=NULL 且 len>0 应拒绝");
    zassert_equal(event_periodic_start(TIMER_TYPE_AUX, EVENT_PRIORITY_NORMAL, NULL, 0U, 100U, 0U, NULL),
                  EVENT_ERR_NOT_FOUND, "未注册类型应返回 NOT_FOUND");
    zassert_equal(event_periodic_active_count(), 0U, "负测不应占用槽");
}

/**
 * @brief 双订阅者：每轮投递双方都收到且计数一致
 */
ZTEST(test_event_timer, test_periodic_two_subscribers) {
    uint32_t             value = 0x7E570001U;
    event_timer_handle_t handle = EVENT_TIMER_HANDLE_INVALID;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    timer_sink_reset(&g_sink_b);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "prd_two"), EVENT_OK, NULL);
    zassert_equal(timer_subscribe(TIMER_TYPE_MAIN, &g_sink_a), EVENT_OK, NULL);
    zassert_equal(timer_subscribe(TIMER_TYPE_MAIN, &g_sink_b), EVENT_OK, NULL);

    zassert_equal(event_periodic_start(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, &value, sizeof(value),
                                       TIMER_PERIOD_BASIC_MS, 0U, &handle),
                  EVENT_OK, NULL);

    zassert_true(timer_wait_calls(&g_sink_a.calls, 10, TIMER_PERIOD_WINDOW_MS), "订阅者 A 应持续收到");
    zassert_true(timer_wait_calls(&g_sink_b.calls, 10, TIMER_PERIOD_WINDOW_MS), "订阅者 B 应持续收到");
    zassert_false(g_sink_a.mismatch, NULL);
    zassert_false(g_sink_b.mismatch, NULL);

    /* 先取消并等在途事件分发完毕，再做精确相等断言：
     * 避免在分发器两次回调之间读计数造成的偶发不相等 */
    zassert_equal(event_periodic_cancel(handle), EVENT_OK, NULL);
    k_sleep(K_MSEC(200));
    zassert_equal(atomic_get(&g_sink_a.calls), atomic_get(&g_sink_b.calls), "双订阅者计数应一致");
}

/* =============================================================================
 * 零长负载与槽复用（M-1 回归）
 * ============================================================================= */

/**
 * @brief 200B 延迟发布投递成功后同槽被零长周期发布复用：每轮负载长度必须为 0
 *
 * 旧缺陷：一次性槽投递完成后 slot->len 残留 200，零长提交未重定义长度，
 * 周期每轮按残留长度投递（读越界 garbage）。本用例锁定该回归：
 * 任一轮 data_len 非 0 即记入 g_zero_len_bad_rounds。
 */
ZTEST(test_event_timer, test_periodic_zero_len_after_large_oneshot_same_slot) {
    uint8_t              payload[TIMER_LARGE_LEN];
    uint32_t             leak_base = 0U;
    uint32_t             sub1 = 0U;
    uint32_t             sub2 = 0U;
    event_timer_handle_t h_oneshot = EVENT_TIMER_HANDLE_INVALID;
    event_timer_handle_t h_periodic = EVENT_TIMER_HANDLE_INVALID;

    timer_test_reset();
    timer_sink_reset(&g_sink_a);
    zassert_equal(event_register_type(TIMER_TYPE_MAIN, "tmr_zlen"), EVENT_OK, NULL);
    zassert_equal(event_subscribe(TIMER_TYPE_MAIN, timer_capture_cb, &g_sink_a, &sub1), EVENT_OK, NULL);

    /* 第一段：200B 负载延迟发布，正常投递（此时槽内 len=200，随后随槽回收） */
    for (uint32_t i = 0; i < TIMER_LARGE_LEN; i++) {
        payload[i] = (uint8_t) (i * 13U + 5U);
    }
    zassert_equal(event_publish_delayed_copy(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, payload, sizeof(payload),
                                             TIMER_DELAY_BASIC_MS, &h_oneshot),
                  EVENT_OK, NULL);
    zassert_true(timer_wait_calls(&g_sink_a.calls, 1, 1000U), "200B 延迟发布应投递成功");
    zassert_equal(g_sink_a.last_len, TIMER_LARGE_LEN, NULL);

    k_sleep(K_MSEC(100)); /* 等分发器释放队列副本，泄漏基线在稳定态提取 */
#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
    leak_base = event_check_leaks();
#endif

    /* 第二段：同槽被零长周期发布复用（claim 取最低空闲槽，前槽已回收） */
    zassert_equal(event_unsubscribe(TIMER_TYPE_MAIN, sub1), EVENT_OK, NULL);
    atomic_set(&g_zero_len_bad_rounds, 0);
    zassert_equal(event_subscribe(TIMER_TYPE_MAIN, timer_zero_len_cb, &g_sink_a, &sub2), EVENT_OK, NULL);
    timer_sink_reset(&g_sink_a);

    zassert_equal(event_periodic_start(TIMER_TYPE_MAIN, EVENT_PRIORITY_NORMAL, NULL, 0U, TIMER_PERIOD_BASIC_MS, 0U,
                                       &h_periodic),
                  EVENT_OK, "零长母本周期发布应成功");
    zassert_equal(h_periodic.slot, h_oneshot.slot, "应复用一次性槽已归还的同一槽位");

    zassert_true(timer_wait_calls(&g_sink_a.calls, 3, TIMER_PERIOD_WINDOW_MS), "零长周期应持续投递");
    zassert_equal(event_periodic_cancel(h_periodic), EVENT_OK, NULL);
    k_sleep(K_MSEC(300)); /* 等在途轮次分发完毕 */

    zassert_equal(atomic_get(&g_zero_len_bad_rounds), 0, "任一轮负载长度非 0 即为 M-1 回归");
    zassert_equal(g_sink_a.last_len, 0U, "零长周期投递长度应为 0");
#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
    zassert_equal(event_check_leaks(), leak_base, "零长周期全程应无内存泄漏");
#endif
}

ZTEST_SUITE(test_event_timer, NULL, NULL, NULL, NULL, NULL);
