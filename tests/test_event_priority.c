/**
 * @file test_event_priority.c
 * @brief 优先级有序分发测试（须 CONFIG_EVENT_PRIORITY_DISPATCH=y）
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-09-27
 *
 * 覆盖：批出队排序、同级 FIFO、remaining 回灌、aging 提档与饿死界、
 * aging 未生效窗口负控、与 DROP_LOWEST 共用 scratch、pause/resume、
 * 全局 filter 占用最优选择、手动 process_one 有序、提档统计、
 * 洪泛 + ISR 发布总数守恒。仅当 CONFIG_EVENT_PRIORITY_DISPATCH=y 时
 * 由 tests/CMakeLists.txt 编入（默认套件不编译，行为零变化）。
 *
 * @note AGING_STEP 依赖 prj_event_ext.conf 固定的 100ms：
 *       提档窗口（10x100=1000ms）与负控窗口（50ms < 100ms）按此值推导。
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include "event_queue.h"
#include "ztest_sync.h"

LOG_MODULE_REGISTER(test_event_priority);

/* =============================================================================
 * 常量与共享记录设施
 * ============================================================================= */

/* 事件类型（与其他套件错开；CONFIG_EVENT_MAX_TYPES=256） */
#define PRIO_TYPE_CRIT   230U
#define PRIO_TYPE_NORMAL 231U
#define PRIO_TYPE_LOW    232U

/* 饿死界 = 最大优先级差(10-0) x AGING_STEP + 宽容窗口（qemu 100Hz tick 抖动） */
#define PRIO_STARVE_BOUND_MS (10U * CONFIG_EVENT_PRIORITY_AGING_STEP_MS + 500U)

/* LOW(10) 有效优先级归 0 并与新到 CRITICAL 平局所需等待（>= 10 x AGING_STEP） */
#define PRIO_AGING_FULL_MS (10U * CONFIG_EVENT_PRIORITY_AGING_STEP_MS + 50U)

/* 负控窗口：小于一个 AGING_STEP，等效于「aging 关闭」（如 AGING_STEP=60000） */
#define PRIO_FRESH_WINDOW_MS 50U

/* 并发守恒测试的发布数量 */
#define PRIO_CONSERVE_THREAD_N 20
#define PRIO_CONSERVE_ISR_N    10U

/* ISR 与优先级批出队并发回归：每批释放 32 个容量，ISR 应不因重排窗口被拒。 */
#define PRIO_ISR_REORDER_CAPACITY 128U
#define PRIO_ISR_REORDER_BATCHES  500U
#define PRIO_ISR_REORDER_OUT       32U

#define PRIO_RECORD_MAX 512U

static atomic_t g_rec_count;
static uint8_t  g_rec_priority[PRIO_RECORD_MAX];
static uint32_t g_rec_type[PRIO_RECORD_MAX];

/** 提档计数基线（各用例自-reset 起断言，避免跨用例累计） */
static uint64_t g_aged_base;

static struct k_msgq  g_isr_reorder_queue;
static char           g_isr_reorder_buffer[PRIO_ISR_REORDER_CAPACITY * sizeof(event_t)];
static struct k_timer g_isr_reorder_timer;
static event_t        g_isr_reorder_event;
static atomic_t       g_isr_reorder_attempts;
static atomic_t       g_isr_reorder_successes;
static atomic_t       g_isr_reorder_failures;

/** @brief ISR 回调：直接向正在批排序的本地队列入队，记录真实返回值。 */
static void isr_reorder_timer_fn(struct k_timer* timer) {
    event_status_t ret;

    ARG_UNUSED(timer);
    atomic_inc(&g_isr_reorder_attempts);
    ret = event_queue_enqueue(&g_isr_reorder_queue, &g_isr_reorder_event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT);
    if (ret == EVENT_OK) {
        atomic_inc(&g_isr_reorder_successes);
    } else {
        atomic_inc(&g_isr_reorder_failures);
    }
}

static void prio_record_cb(const event_t* event, void* user_data) {
    ARG_UNUSED(user_data);

    /* 单一消费者（分发器线程或手动消费线程）串行调用：先落数据再发布计数，
     * 其他线程据计数读取记录不会看到半写状态 */
    uint32_t idx = (uint32_t) atomic_get(&g_rec_count);

    if (idx < PRIO_RECORD_MAX) {
        g_rec_priority[idx] = event->priority;
        g_rec_type[idx] = event->type;
    }
    atomic_inc(&g_rec_count);
}

/** @brief 统一复位：重启事件系统 + 分发器（仅 init，不 start；提档计数清零） */
static void prio_test_reset(void) {
    (void) event_system_shutdown();
    zassert_equal(event_system_init(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_init(NULL), EVENT_OK, NULL);
    event_dispatcher_reset_stats();
    g_aged_base = event_dispatcher_get_aged_promotions();
    atomic_set(&g_rec_count, 0);
}

/** @brief 注册三类测试类型并订阅统一记录回调 */
static void prio_setup_types_and_subscriber(void) {
    uint32_t sub = 0U;

    zassert_equal(event_register_type((event_type_t) PRIO_TYPE_CRIT, "prio_crit"), EVENT_OK, NULL);
    zassert_equal(event_register_type((event_type_t) PRIO_TYPE_NORMAL, "prio_norm"), EVENT_OK, NULL);
    zassert_equal(event_register_type((event_type_t) PRIO_TYPE_LOW, "prio_low"), EVENT_OK, NULL);
    zassert_equal(event_subscribe((event_type_t) PRIO_TYPE_CRIT, prio_record_cb, NULL, &sub), EVENT_OK, NULL);
    zassert_equal(event_subscribe((event_type_t) PRIO_TYPE_NORMAL, prio_record_cb, NULL, &sub), EVENT_OK, NULL);
    zassert_equal(event_subscribe((event_type_t) PRIO_TYPE_LOW, prio_record_cb, NULL, &sub), EVENT_OK, NULL);
}

/** @brief 构造「无负载、新鲜 timestamp」的合法事件并线程侧发布 */
static void prio_publish(uint8_t type, uint8_t priority) {
    event_t ev = {0};

    ev.type = type;
    ev.priority = priority;
    ev.timestamp = k_uptime_get_32(); /* 手搓 event_t 必须设置 timestamp（优先级排序依赖） */
    zassert_equal(event_publish(&ev), EVENT_OK, "publish type=%u prio=%u failed", type, priority);
}

/** @brief 向本地测试队列入队一条新鲜事件（不经过事件系统发布路径） */
static void prio_local_enqueue(struct k_msgq* queue, uint8_t type, uint8_t priority) {
    event_t ev = {0};

    ev.type = type;
    ev.priority = priority;
    ev.timestamp = k_uptime_get_32();
    zassert_equal(event_queue_enqueue(queue, &ev, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_OK, NULL);
}

/** @brief 读取第 idx 条投递记录的优先级（越界返回 0） */
static uint8_t prio_record_priority(uint32_t idx) {
    return (idx < PRIO_RECORD_MAX) ? g_rec_priority[idx] : (uint8_t) 0U;
}

/* =============================================================================
 * 批出队基本语义（本地队列，不依赖分发器线程）
 * ============================================================================= */

/**
 * @brief 入队 LOW/CRITICAL/NORMAL（FIFO 序），一次批取出应为 CRITICAL→NORMAL→LOW
 */
ZTEST(test_event_priority, test_priority_batch_order_basic) {
    static struct k_msgq queue;
    static char          buffer[8 * sizeof(event_t)];
    event_t              out[8];
    uint32_t             remaining = 0U;
    queue_stats_t        stats;

    prio_test_reset();
    zassert_equal(event_queue_init(&queue, buffer, 8), EVENT_OK, NULL);

    prio_local_enqueue(&queue, PRIO_TYPE_LOW, EVENT_PRIORITY_LOW);
    prio_local_enqueue(&queue, PRIO_TYPE_CRIT, EVENT_PRIORITY_CRITICAL);
    prio_local_enqueue(&queue, PRIO_TYPE_NORMAL, EVENT_PRIORITY_NORMAL);

    uint32_t n = event_queue_dequeue_priority_batch(&queue, out, 8U, &remaining);

    zassert_equal(n, 3, "应一次取出全部 3 条");
    zassert_equal(remaining, 0, "取批后队列应空");
    zassert_equal(out[0].priority, EVENT_PRIORITY_CRITICAL, "最高优先级最先");
    zassert_equal(out[1].priority, EVENT_PRIORITY_NORMAL, "NORMAL 次之");
    zassert_equal(out[2].priority, EVENT_PRIORITY_LOW, "LOW 最后");

    event_queue_get_stats(&queue, &stats);
    zassert_equal(stats.dequeue_count, 3, "批出队应累计 dequeue_count");
    zassert_equal(stats.enqueue_count, 3, NULL);

    event_queue_deinit(&queue);
}

/**
 * @brief 同级 5 条保持 FIFO 次序（平局取 FIFO 最旧）
 */
ZTEST(test_event_priority, test_priority_batch_same_level_fifo) {
    static struct k_msgq queue;
    static char          buffer[8 * sizeof(event_t)];
    event_t              out[8];

    prio_test_reset();
    zassert_equal(event_queue_init(&queue, buffer, 8), EVENT_OK, NULL);

    for (uint8_t i = 0; i < 5; i++) {
        prio_local_enqueue(&queue, (uint8_t) (PRIO_TYPE_NORMAL + i), EVENT_PRIORITY_NORMAL);
    }

    uint32_t n = event_queue_dequeue_priority_batch(&queue, out, 8U, NULL);

    zassert_equal(n, 5, "同级 5 条应全部取出");
    for (uint8_t i = 0; i < 5; i++) {
        zassert_equal(out[i].type, PRIO_TYPE_NORMAL + i, "同级应保持 FIFO（第 %u 条）", i);
    }

    event_queue_deinit(&queue);
}

/**
 * @brief max_out 截批：remaining 正确回灌，第二次批取出剩余并保序
 */
ZTEST(test_event_priority, test_priority_batch_remaining_requeue) {
    static struct k_msgq queue;
    static char          buffer[4 * sizeof(event_t)];
    event_t              out[4];
    uint32_t             remaining = 0U;

    prio_test_reset();
    zassert_equal(event_queue_init(&queue, buffer, 4), EVENT_OK, NULL);

    prio_local_enqueue(&queue, PRIO_TYPE_LOW, EVENT_PRIORITY_LOW);
    prio_local_enqueue(&queue, PRIO_TYPE_NORMAL, EVENT_PRIORITY_NORMAL);
    prio_local_enqueue(&queue, PRIO_TYPE_CRIT, EVENT_PRIORITY_HIGH);
    prio_local_enqueue(&queue, PRIO_TYPE_CRIT, EVENT_PRIORITY_CRITICAL);

    uint32_t n = event_queue_dequeue_priority_batch(&queue, out, 2U, &remaining);

    zassert_equal(n, 2, "max_out=2 应截批");
    zassert_equal(remaining, 2, "剩余 2 条应回灌");
    zassert_equal(event_queue_depth(&queue), 2, "队列深度应与 remaining 一致");
    zassert_equal(out[0].priority, EVENT_PRIORITY_CRITICAL, "第一批最优");
    zassert_equal(out[1].priority, EVENT_PRIORITY_HIGH, "第一批次优");

    n = event_queue_dequeue_priority_batch(&queue, out, 4U, &remaining);

    zassert_equal(n, 2, NULL);
    zassert_equal(remaining, 0, NULL);
    zassert_equal(out[0].priority, EVENT_PRIORITY_NORMAL, "回灌后次序保持（NORMAL 先于 LOW）");
    zassert_equal(out[1].priority, EVENT_PRIORITY_LOW, NULL);

    event_queue_deinit(&queue);
}

/**
 * @brief 空队列与非法参数返回 0，不崩溃
 */
ZTEST(test_event_priority, test_priority_batch_empty_and_invalid) {
    static struct k_msgq queue;
    static char          buffer[2 * sizeof(event_t)];
    event_t              out[2];
    uint32_t             remaining = 99U;

    prio_test_reset();
    zassert_equal(event_queue_init(&queue, buffer, 2), EVENT_OK, NULL);

    zassert_equal(event_queue_dequeue_priority_batch(&queue, out, 2U, &remaining), 0, "空队列应取 0 条");
    zassert_equal(remaining, 0, "空队列 remaining 应为 0");

    zassert_equal(event_queue_dequeue_priority_batch(NULL, out, 2U, NULL), 0, "NULL 队列应取 0 条");
    zassert_equal(event_queue_dequeue_priority_batch(&queue, NULL, 2U, NULL), 0, "NULL 输出应取 0 条");
    zassert_equal(event_queue_dequeue_priority_batch(&queue, out, 0U, NULL), 0, "max_out=0 应取 0 条");

    event_queue_deinit(&queue);
}

/* =============================================================================
 * aging 语义（确定性窗口，本地队列）
 * ============================================================================= */

/**
 * @brief aging 负控：等待小于一个 AGING_STEP 的窗口内 LOW 不被提档
 *
 * 等效于 AGING_STEP=60000 的构建（「aging 关闭」）：有限时长内新鲜 LOW 始终
 * 排在新鲜 CRITICAL 之后、不被提档；提档计数不增长。
 */
ZTEST(test_event_priority, test_priority_no_aging_within_step) {
    static struct k_msgq queue;
    static char          buffer[8 * sizeof(event_t)];
    event_t              out[8];

    prio_test_reset();
    zassert_equal(event_queue_init(&queue, buffer, 8), EVENT_OK, NULL);

    prio_local_enqueue(&queue, PRIO_TYPE_LOW, EVENT_PRIORITY_LOW);
    prio_local_enqueue(&queue, PRIO_TYPE_CRIT, EVENT_PRIORITY_CRITICAL);
    k_msleep(PRIO_FRESH_WINDOW_MS);

    uint32_t n = event_queue_dequeue_priority_batch(&queue, out, 8U, NULL);

    zassert_equal(n, 2, NULL);
    zassert_equal(out[0].priority, EVENT_PRIORITY_CRITICAL, "窗口 < AGING_STEP：CRITICAL 仍应先于 LOW");
    zassert_equal(out[1].priority, EVENT_PRIORITY_LOW, NULL);
    zassert_equal(event_dispatcher_get_aged_promotions(), g_aged_base, "新鲜事件不应计入提档");

    event_queue_deinit(&queue);
}

/**
 * @brief aging 提档：LOW 等待超过 10 x AGING_STEP 后先于新到 CRITICAL 出队
 */
ZTEST(test_event_priority, test_priority_aging_promotes_low) {
    static struct k_msgq queue;
    static char          buffer[8 * sizeof(event_t)];
    event_t              out[8];
    dispatcher_stats_t   stats;

    prio_test_reset();
    zassert_equal(event_queue_init(&queue, buffer, 8), EVENT_OK, NULL);

    prio_local_enqueue(&queue, PRIO_TYPE_LOW, EVENT_PRIORITY_LOW);
    k_msleep(PRIO_AGING_FULL_MS); /* >= 10 x AGING_STEP：LOW 有效优先级归 0 */

    prio_local_enqueue(&queue, PRIO_TYPE_CRIT, EVENT_PRIORITY_CRITICAL);
    prio_local_enqueue(&queue, PRIO_TYPE_NORMAL, EVENT_PRIORITY_NORMAL);

    uint32_t n = event_queue_dequeue_priority_batch(&queue, out, 8U, NULL);

    zassert_equal(n, 3, NULL);
    zassert_equal(out[0].priority, EVENT_PRIORITY_LOW, "老化为 0 后与 CRITICAL 平局，FIFO 最旧的 LOW 应胜出");
    zassert_equal(out[1].priority, EVENT_PRIORITY_CRITICAL, NULL);
    zassert_equal(out[2].priority, EVENT_PRIORITY_NORMAL, NULL);
    zassert_true(event_dispatcher_get_aged_promotions() > g_aged_base, "被提档事件应计入提档统计");

    /* 两种统计口径一致：get_stats 镜像与独立诊断 getter */
    event_dispatcher_get_stats(&stats);
    zassert_equal(stats.aged_promotions, event_dispatcher_get_aged_promotions(), "get_stats 与 getter 口径一致");

    event_queue_deinit(&queue);
}

/* =============================================================================
 * 分发器线程路径
 * ============================================================================= */

/* ---- 饿死界验证（全手动有界批消费：无线程、无竞态、时序确定） ---- */

/**
 * @brief 饿死界：LOW 先被 CRITICAL 压制；老化 10 x AGING_STEP 后越过新到 CRITICAL
 *
 * 批出队一次取整批，若依赖分发线程持续消费，「新鲜 CRITICAL 压制 LOW」与
 * 「批内排序」会在同一周期内混合，无法稳定观察滞留。故用 max_events=1 的
 * 手动批消费逐条放行 CRITICAL（等价于「持续洪泛下每次只消费当前最优」）。
 *
 * 阶段 1（未老化）：[C,C,C,LOW] 逐条消费 3 次 → 3 条 CRITICAL 先出，LOW 滞留。
 * 阶段 2（老化后）：等待 >= 10 x AGING_STEP，补 2 条新鲜 CRITICAL →
 * 一次批消费应 LOW 最先（提档归 0，平局取最旧），随后才是新 CRITICAL。
 * LOW 总滞留 <= (10 x AGING_STEP) + 宽容窗口（qemu 100Hz tick 抖动）。
 */
ZTEST(test_event_priority, test_priority_aging_starvation_bound) {
    uint32_t t0 = 0U;

    prio_test_reset();
    prio_setup_types_and_subscriber();
    zassert_equal(event_system_start(), EVENT_OK, NULL); /* 分发器保持未启动，手动消费 */

    /* 阶段 1：未老化，逐条放行 CRITICAL，LOW 持续滞留队列 */
    prio_publish(PRIO_TYPE_CRIT, EVENT_PRIORITY_CRITICAL);
    prio_publish(PRIO_TYPE_CRIT, EVENT_PRIORITY_CRITICAL);
    prio_publish(PRIO_TYPE_CRIT, EVENT_PRIORITY_CRITICAL);
    t0 = k_uptime_get_32();
    prio_publish(PRIO_TYPE_LOW, EVENT_PRIORITY_LOW);

    for (uint32_t i = 0U; i < 3U; i++) {
        zassert_equal(event_dispatcher_process_all(1U), 1U, "手动批消费 #%u 应恰好取 1 条", i);
        zassert_equal(prio_record_priority(i), EVENT_PRIORITY_CRITICAL, "未老化时 CRITICAL 应先于 LOW（第 %u 条）", i);
    }
    zassert_equal(event_queue_depth(event_system_get_queue()), 1, "未老化前 LOW 应持续滞留队列");

    /* 阶段 2：等待老化（>= 10 x AGING_STEP），补新鲜 CRITICAL 后一次批消费 */
    k_msleep(PRIO_AGING_FULL_MS);
    prio_publish(PRIO_TYPE_CRIT, EVENT_PRIORITY_CRITICAL);
    prio_publish(PRIO_TYPE_CRIT, EVENT_PRIORITY_CRITICAL);

    zassert_equal(event_dispatcher_process_all(0), 3U, "老化后 LOW 与 2 条新 CRITICAL 应一次批出");
    zassert_equal(prio_record_priority(3), EVENT_PRIORITY_LOW, "老化归 0 后 LOW 应越过新到 CRITICAL");
    zassert_equal(prio_record_priority(4), EVENT_PRIORITY_CRITICAL, NULL);
    zassert_equal(prio_record_priority(5), EVENT_PRIORITY_CRITICAL, NULL);
    zassert_true(k_uptime_get_32() - t0 <= PRIO_STARVE_BOUND_MS, "LOW 投递时刻应在饿死界 (%u ms) 内",
                 PRIO_STARVE_BOUND_MS);
    zassert_true(event_dispatcher_get_aged_promotions() > g_aged_base, "被提档的 LOW 应计入提档统计");

    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
}

/* ---- pause/resume 按优先序消化 ---- */

/**
 * @brief pause 期间积压混合优先级事件，resume 后按 CRITICAL→NORMAL→LOW 消化
 */
ZTEST(test_event_priority, test_priority_pause_resume_priority_drain) {
    prio_test_reset();
    prio_setup_types_and_subscriber();
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_pause(), EVENT_OK, NULL);

    prio_publish(PRIO_TYPE_LOW, EVENT_PRIORITY_LOW);
    prio_publish(PRIO_TYPE_CRIT, EVENT_PRIORITY_CRITICAL);
    prio_publish(PRIO_TYPE_NORMAL, EVENT_PRIORITY_NORMAL);
    zassert_equal(event_queue_depth(event_system_get_queue()), 3, "暂停期间应积压 3 条");

    zassert_equal(event_dispatcher_resume(), EVENT_OK, NULL);
    zassert_true(ztest_wait_atomic_eq(&g_rec_count, 3, 2000), "resume 后 3 条应全部投递");
    zassert_equal(prio_record_priority(0), EVENT_PRIORITY_CRITICAL, "resume 后第一条应为 CRITICAL");
    zassert_equal(prio_record_priority(1), EVENT_PRIORITY_NORMAL, NULL);
    zassert_equal(prio_record_priority(2), EVENT_PRIORITY_LOW, NULL);

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
}

/* ---- 全局 filter 与优先级出队的交互 ---- */

static bool reject_critical_filter(const event_t* event, void* user_data) {
    ARG_UNUSED(user_data);
    return event->priority != EVENT_PRIORITY_CRITICAL;
}

/**
 * @brief 全局 filter 拒掉 CRITICAL 后，NORMAL 成为投递最优；高优事件占用一次选择
 */
ZTEST(test_event_priority, test_priority_global_filter_rejects_critical) {
    dispatcher_stats_t stats;
    uint32_t           sub = 0U;

    prio_test_reset();
    zassert_equal(event_register_type((event_type_t) PRIO_TYPE_CRIT, "prio_flt_crit"), EVENT_OK, NULL);
    zassert_equal(event_register_type((event_type_t) PRIO_TYPE_NORMAL, "prio_flt_norm"), EVENT_OK, NULL);
    zassert_equal(event_register_type((event_type_t) PRIO_TYPE_LOW, "prio_flt_low"), EVENT_OK, NULL);
    zassert_equal(event_subscribe((event_type_t) PRIO_TYPE_CRIT, prio_record_cb, NULL, &sub), EVENT_OK, NULL);
    zassert_equal(event_subscribe((event_type_t) PRIO_TYPE_NORMAL, prio_record_cb, NULL, &sub), EVENT_OK, NULL);
    zassert_equal(event_subscribe((event_type_t) PRIO_TYPE_LOW, prio_record_cb, NULL, &sub), EVENT_OK, NULL);

    event_dispatcher_set_filter(reject_critical_filter, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);

    prio_publish(PRIO_TYPE_CRIT, EVENT_PRIORITY_CRITICAL);
    prio_publish(PRIO_TYPE_NORMAL, EVENT_PRIORITY_NORMAL);
    prio_publish(PRIO_TYPE_LOW, EVENT_PRIORITY_LOW);

    /* 手动消费（分发器线程未启动，init 后允许）*/
    uint32_t processed = event_dispatcher_process_all(0);

    zassert_equal(processed, 3, "3 条均应被消费（含被过滤的 1 条）");
    zassert_equal(atomic_get(&g_rec_count), 2, "订阅者只应收到 2 条");
    zassert_equal(prio_record_priority(0), EVENT_PRIORITY_NORMAL, "CRITICAL 被拒后 NORMAL 成为投递最优");
    zassert_equal(prio_record_priority(1), EVENT_PRIORITY_LOW, NULL);

    event_dispatcher_get_stats(&stats);
    zassert_equal(stats.events_filtered, 1, "被拒 CRITICAL 应计入过滤统计（占用一次最优选择）");
    zassert_equal(stats.events_processed, 2, NULL);

    event_dispatcher_clear_filter();
    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
}

/* ---- 手动 process_one 有序 ---- */

/**
 * @brief 手动 process_one 单条消费亦按优先级有序（行为变化点）
 */
ZTEST(test_event_priority, test_priority_process_one_manual_order) {
    prio_test_reset();
    prio_setup_types_and_subscriber();
    zassert_equal(event_system_start(), EVENT_OK, NULL);

    prio_publish(PRIO_TYPE_LOW, EVENT_PRIORITY_LOW);
    prio_publish(PRIO_TYPE_CRIT, EVENT_PRIORITY_CRITICAL);
    prio_publish(PRIO_TYPE_NORMAL, EVENT_PRIORITY_NORMAL);

    zassert_equal(event_dispatcher_process_one(K_NO_WAIT), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_process_one(K_NO_WAIT), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_process_one(K_NO_WAIT), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_process_one(K_NO_WAIT), EVENT_ERR_QUEUE_EMPTY, "应恰好消费 3 条");

    zassert_equal(atomic_get(&g_rec_count), 3, NULL);
    zassert_equal(prio_record_priority(0), EVENT_PRIORITY_CRITICAL, "手动单条亦应 CRITICAL 最先");
    zassert_equal(prio_record_priority(1), EVENT_PRIORITY_NORMAL, NULL);
    zassert_equal(prio_record_priority(2), EVENT_PRIORITY_LOW, NULL);

    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
}

/* =============================================================================
 * 与 DROP_LOWEST 组合（共用 scratch）
 * ============================================================================= */

/**
 * @brief 满队列 DROP_LOWEST 挤入 + 优先级批出队：两机制共用 scratch 无冲突、计数无虚增
 */
ZTEST(test_event_priority, test_priority_with_drop_lowest_combo) {
    static struct k_msgq queue;
    static char          buffer[3 * sizeof(event_t)];
    event_t              out[3];
    queue_stats_t        stats;

    prio_test_reset();
    zassert_equal(event_queue_init(&queue, buffer, 3), EVENT_OK, NULL);

    /* 入队侧机制：满队列时 CRITICAL 挤掉 FIFO 最旧的 NORMAL */
    prio_local_enqueue(&queue, PRIO_TYPE_NORMAL, EVENT_PRIORITY_NORMAL);
    prio_local_enqueue(&queue, PRIO_TYPE_NORMAL, EVENT_PRIORITY_NORMAL);
    prio_local_enqueue(&queue, PRIO_TYPE_NORMAL, EVENT_PRIORITY_NORMAL);

    event_t hi = {0};

    hi.type = (uint8_t) PRIO_TYPE_CRIT;
    hi.priority = EVENT_PRIORITY_CRITICAL;
    hi.timestamp = k_uptime_get_32();
    zassert_equal(event_queue_enqueue(&queue, &hi, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT), EVENT_OK,
                  "CRITICAL 应挤掉最旧 NORMAL");

    /* 出队侧机制：同一 scratch 上批出队排序 */
    uint32_t n = event_queue_dequeue_priority_batch(&queue, out, 3U, NULL);

    zassert_equal(n, 3, NULL);
    zassert_equal(out[0].priority, EVENT_PRIORITY_CRITICAL, "批出队应先取挤入的 CRITICAL");
    zassert_equal(out[1].type, PRIO_TYPE_NORMAL, NULL);
    zassert_equal(out[2].type, PRIO_TYPE_NORMAL, "剩余 NORMAL 保持 FIFO 次序");

    event_queue_get_stats(&queue, &stats);
    zassert_equal(stats.drop_count, 1, "应仅统计被挤掉的 1 条");
    zassert_equal(stats.overflow_count, 0, "成功挤入不应虚增 overflow");
    zassert_equal(stats.enqueue_count, 4, NULL);
    zassert_equal(stats.dequeue_count, 3, "批出队 3 条应计入 dequeue");

    event_queue_deinit(&queue);
}

/**
 * @brief 批排序窗口内 ISR 在有实际容量时必须入队，不得因 reordering 门控返回 QUEUE_FULL
 */
ZTEST(test_event_priority, test_priority_reorder_allows_isr_when_capacity_available) {
    event_t       out[PRIO_ISR_REORDER_OUT];
    event_t       filler = {0};
    queue_stats_t stats;

    prio_test_reset();
    zassert_equal(event_queue_init(&g_isr_reorder_queue, g_isr_reorder_buffer, PRIO_ISR_REORDER_CAPACITY), EVENT_OK,
                  NULL);
    for (uint32_t i = 0U; i < PRIO_ISR_REORDER_CAPACITY; i++) {
        prio_local_enqueue(&g_isr_reorder_queue, PRIO_TYPE_NORMAL, EVENT_PRIORITY_NORMAL);
    }
    memset(&g_isr_reorder_event, 0, sizeof(g_isr_reorder_event));
    g_isr_reorder_event.type = PRIO_TYPE_CRIT;
    g_isr_reorder_event.priority = EVENT_PRIORITY_CRITICAL;
    g_isr_reorder_event.timestamp = k_uptime_get_32();
    atomic_set(&g_isr_reorder_attempts, 0);
    atomic_set(&g_isr_reorder_successes, 0);
    atomic_set(&g_isr_reorder_failures, 0);
    k_timer_init(&g_isr_reorder_timer, isr_reorder_timer_fn, NULL);
    k_timer_start(&g_isr_reorder_timer, K_MSEC(1), K_MSEC(1));

    filler.type = PRIO_TYPE_NORMAL;
    filler.priority = EVENT_PRIORITY_NORMAL;
    for (uint32_t i = 0U; i < PRIO_ISR_REORDER_BATCHES; i++) {
        /* 每批至少释放 32 个容量，覆盖本批排序期间连续到达的 ISR 事件。 */
        (void) event_queue_dequeue_priority_batch(&g_isr_reorder_queue, out, PRIO_ISR_REORDER_OUT, NULL);
        if (event_queue_depth(&g_isr_reorder_queue) < PRIO_ISR_REORDER_CAPACITY) {
            event_status_t ret;

            filler.timestamp = k_uptime_get_32();
            ret = event_queue_enqueue(&g_isr_reorder_queue, &filler, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT);
            zassert_true(ret == EVENT_OK || ret == EVENT_ERR_QUEUE_FULL, "filler enqueue returned %d", ret);
        }
    }
    k_timer_stop(&g_isr_reorder_timer);

    zassert_true(atomic_get(&g_isr_reorder_attempts) > 0, "测试期间 ISR 应至少尝试一次入队");
    zassert_equal(atomic_get(&g_isr_reorder_failures), 0,
                  "队列为 ISR 保留批次释放的容量，不应发生 reordering-only 拒绝");
    zassert_equal(atomic_get(&g_isr_reorder_successes), atomic_get(&g_isr_reorder_attempts), NULL);
    event_queue_get_stats(&g_isr_reorder_queue, &stats);
    zassert_equal(stats.overflow_count, 0U, "无真实满队列时不应记录溢出");
    event_queue_deinit(&g_isr_reorder_queue);
}

/* =============================================================================
 * 洪泛 + ISR 侧发布总数守恒（并发）
 * ============================================================================= */

static atomic_t        g_conserve_delivered;
static atomic_t        g_conserve_thread_ok;
static atomic_t        g_conserve_isr_ok;
static struct k_timer  g_conserve_isr_timer;
static event_t         g_conserve_isr_event;
static struct k_thread g_conserve_thread;
static K_THREAD_STACK_DEFINE(g_conserve_stack, 1024);

static void conserve_cb(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
    atomic_inc(&g_conserve_delivered);
}

/** @brief ISR 发布定时器回调（k_timer 过期函数运行于 ISR 上下文）；不足额则续期重试 */
static void conserve_isr_timer_fn(struct k_timer* timer) {
    if (atomic_get(&g_conserve_isr_ok) >= (atomic_val_t) PRIO_CONSERVE_ISR_N) {
        return; /* 不续期，自然停止 */
    }

    if (event_publish_from_isr(&g_conserve_isr_event) == EVENT_OK) {
        atomic_inc(&g_conserve_isr_ok);
    }
    /* 队列满时下个到期重试（ISR 不可阻塞/自旋）。 */
    k_timer_start(timer, K_MSEC(2), K_NO_WAIT);
}

static void conserve_producer(void* p1, void* p2, void* p3) {
    int total = (int) (intptr_t) p1;

    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (int i = 0; i < total; i++) {
        event_t ev = {0};

        ev.type = (uint8_t) PRIO_TYPE_NORMAL;
        ev.priority = EVENT_PRIORITY_CRITICAL;
        ev.timestamp = k_uptime_get_32();
        if (event_publish(&ev) == EVENT_OK) {
            atomic_inc(&g_conserve_thread_ok);
        }
        k_sleep(K_MSEC(1));
    }
}

/** @brief 谓词：投递数已追上成功发布总数（线程 + ISR） */
static bool pred_delivered_caught_up(void* unused) {
    ARG_UNUSED(unused);

    uint32_t published = (uint32_t) (atomic_get(&g_conserve_thread_ok) + atomic_get(&g_conserve_isr_ok));

    return (uint32_t) atomic_get(&g_conserve_delivered) >= published;
}

/** @brief 谓词：全局队列已空 */
static bool pred_queue_empty(void* unused) {
    ARG_UNUSED(unused);
    return event_queue_depth(event_system_get_queue()) == 0U;
}

/**
 * @brief 分发洪泛同时 ISR 侧发布：成功发布总数 == 投递总数（守恒），队列收尾为空
 */
ZTEST(test_event_priority, test_priority_isr_publish_conservation) {
    uint32_t sub = 0U;
    uint32_t published = 0U;

    prio_test_reset();
    zassert_equal(event_register_type((event_type_t) PRIO_TYPE_NORMAL, "prio_conserve"), EVENT_OK, NULL);
    zassert_equal(event_subscribe((event_type_t) PRIO_TYPE_NORMAL, conserve_cb, NULL, &sub), EVENT_OK, NULL);
    zassert_equal(event_system_start(), EVENT_OK, NULL);
    zassert_equal(event_dispatcher_start(), EVENT_OK, NULL);

    atomic_set(&g_conserve_delivered, 0);
    atomic_set(&g_conserve_thread_ok, 0);
    atomic_set(&g_conserve_isr_ok, 0);

    memset(&g_conserve_isr_event, 0, sizeof(g_conserve_isr_event));
    g_conserve_isr_event.type = (uint8_t) PRIO_TYPE_NORMAL;
    g_conserve_isr_event.priority = EVENT_PRIORITY_LOW;
    g_conserve_isr_event.timestamp = k_uptime_get_32();

    k_timer_init(&g_conserve_isr_timer, conserve_isr_timer_fn, NULL);
    k_timer_start(&g_conserve_isr_timer, K_MSEC(2), K_NO_WAIT);

    zassert_not_null(k_thread_create(&g_conserve_thread, g_conserve_stack, K_THREAD_STACK_SIZEOF(g_conserve_stack),
                                     conserve_producer, (void*) (intptr_t) PRIO_CONSERVE_THREAD_N, NULL, NULL, 5, 0,
                                     K_NO_WAIT),
                     "conserve producer create failed");

    zassert_equal(k_thread_join(&g_conserve_thread, K_SECONDS(3)), 0, NULL);
    zassert_equal(atomic_get(&g_conserve_thread_ok), PRIO_CONSERVE_THREAD_N, "线程侧发布应全部成功");

    k_timer_stop(&g_conserve_isr_timer);
    zassert_true(atomic_get(&g_conserve_isr_ok) >= 1, "ISR 侧至少应成功发布 1 条");

    zassert_true(ztest_wait_until(pred_queue_empty, NULL, 2000), "队列应最终排空");

    /* 排空 + 停计时器后再读两侧计数：此后不会再有发布与投递 */
    published = (uint32_t) (atomic_get(&g_conserve_thread_ok) + atomic_get(&g_conserve_isr_ok));
    zassert_true(ztest_wait_until(pred_delivered_caught_up, NULL, 2000), "投递数应追上成功发布数");
    zassert_equal((uint32_t) atomic_get(&g_conserve_delivered), published, "成功发布总数应等于投递总数（守恒）");

    zassert_equal(event_dispatcher_stop(), EVENT_OK, NULL);
    zassert_equal(event_system_shutdown(), EVENT_OK, NULL);
}

/* =============================================================================
 * 测试套件
 * ============================================================================= */

ZTEST_SUITE(test_event_priority, NULL, NULL, NULL, NULL, NULL);
