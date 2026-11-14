/**
 * @file event_benchmark.c
 * @brief 事件系统性能基准模块实现
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-11-14
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-11-14       1.0            zeh             B1 初始版本
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zeplod/app_config.h>
#include <zeplod/event_benchmark.h>

LOG_MODULE_REGISTER(event_benchmark, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 常量与静态数据
 * ============================================================================= */

/** slab 数据档位负载长度（字节）：> 内联阈值且逐档落在对应 slab 池范围内 */
static const size_t g_bench_tier_payload_len[EVENT_BENCH_SLAB_TIER_COUNT] = {
    0U,    /**< EVENT_BENCH_SLAB_SHELL：仅外壳，无数据负载 */
    200U,  /**< EVENT_BENCH_SLAB_DATA_256：≤256B 池 */
    600U,  /**< EVENT_BENCH_SLAB_DATA_1K：≤1KB 池 */
    3000U, /**< EVENT_BENCH_SLAB_DATA_4K：≤4KB 池 */
};

/** 数据档位最大负载长度（payload 缓冲容量） */
#define BENCH_SLAB_PAYLOAD_MAX 3000U

/** 预分配样本数组：测量循环仅写入此数组，结束后统一排序算分位数 */
static uint64_t g_bench_samples[CONFIG_EVENT_BENCHMARK_MAX_SAMPLES];

/** 数据档位负载缓冲（create_with_data 内部拷贝该模式的耗时计入往返） */
static uint8_t g_bench_slab_payload[BENCH_SLAB_PAYLOAD_MAX];

/**
 * @brief 基准运行时上下文
 *
 * publish_cycle 由 shell/调用线程在发布前写入、订阅回调在分发线程中
 * 读取（k_msgq 入队/出队隐含 happens-before，单核无撕裂）；样本下标
 * 与 armed 标志走原子操作。
 */
static struct {
    volatile uint64_t publish_cycle; /**< 本轮最近一次发布前取到的 cycle 数 */
    uint32_t          run_id;        /**< 当前基准轮次 ID（随事件负载发布，回调据此丢弃上轮残留） */
    uint32_t          expected;      /**< 目标迭代次数（样本数组上界） */
    atomic_t          received;      /**< 回调已执行次数（仅本轮有效投递） */
    atomic_t          armed;         /**< 1=基准进行中，回调才记录样本 */
    atomic_t          busy;          /**< 1=某一基准正在运行（重入保护） */
} g_bench_ctx;

/** 单事件投递完成信号量：回调给出，发布方等待（乒乓投递节拍） */
K_SEM_DEFINE(g_bench_delivery, 0, 1);

/* =============================================================================
 * 内部辅助
 * ============================================================================= */

/**
 * @brief uint64 样本升序比较器（qsort 用）
 */
static int bench_u64_cmp(const void* a, const void* b) {
    uint64_t va = *(const uint64_t*) a;
    uint64_t vb = *(const uint64_t*) b;

    /* 溢出安全的比较写法：避免直接相减被截断 */
    return (va > vb) - (va < vb);
}

/**
 * @brief 排序样本并计算 min/p50/p95/max/mean 统计（ns 口径）
 *
 * 仅测量结束后调用；样本排序在预分配数组原地进行。
 *
 * @param result 输出结果
 * @param samples 有效样本数（>=1）
 * @param failures 失败迭代数
 */
static void bench_fill_stats(event_benchmark_result_t* result, uint32_t samples, uint32_t failures) {
    uint64_t total = 0;

    qsort(g_bench_samples, samples, sizeof(g_bench_samples[0]), bench_u64_cmp);
    for (uint32_t i = 0; i < samples; i++) {
        total += g_bench_samples[i];
    }

    /* p95 取 floor 索引并钳制到末位；单样本时各分位退化为同一样本 */
    uint32_t p95_idx = (uint32_t) (((uint64_t) samples * 95U) / 100U);
    if (p95_idx >= samples) {
        p95_idx = samples - 1U;
    }

    result->samples = samples;
    result->failures = failures;
    result->min_ns = k_cyc_to_ns_floor64(g_bench_samples[0]);
    result->p50_ns = k_cyc_to_ns_floor64(g_bench_samples[samples / 2U]);
    result->p95_ns = k_cyc_to_ns_floor64(g_bench_samples[p95_idx]);
    result->max_ns = k_cyc_to_ns_floor64(g_bench_samples[samples - 1U]);
    result->mean_ns = k_cyc_to_ns_floor64(total / samples);
}

/**
 * @brief 订阅回调：仅取回调入口时间戳、写入样本数组并给出投递信号量
 *
 * 分发线程上下文执行；不打印、不分配、不阻塞。
 */
static void bench_dispatch_callback(const event_t* event, void* user_data) {
    uint64_t now = k_cycle_get_64();

    ARG_UNUSED(user_data);

    /* 未武装时收到的多为上一轮超时残留的陈旧事件，直接忽略 */
    if (atomic_get(&g_bench_ctx.armed) != 1U) {
        return;
    }

    /* 负载为本轮 run_id：上轮投递超时后经 unsub 快照迟到执行的残留事件
     * 在此被丢弃——不写样本、不 give 信号量，避免乒乓失拍 */
    uint32_t seq = 0U;
    if ((event == NULL) || (event->data_len != sizeof(seq))) {
        return;
    }
    memcpy(&seq, event->data.inline_data, sizeof(seq));
    if (seq != g_bench_ctx.run_id) {
        return;
    }

    uint32_t idx = (uint32_t) atomic_inc(&g_bench_ctx.received);
    if (idx < g_bench_ctx.expected) {
        g_bench_samples[idx] = now - g_bench_ctx.publish_cycle;
    }
    k_sem_give(&g_bench_delivery);
}

/**
 * @brief 校验 iterations/result 并清零结果；ISR 上下文直接拒绝
 */
static bool bench_args_valid(event_benchmark_result_t* result, uint32_t iterations) {
    if ((result == NULL) || (iterations == 0U) || (iterations > (uint32_t) CONFIG_EVENT_BENCHMARK_MAX_SAMPLES)) {
        return false;
    }
    if (k_is_in_isr()) {
        return false;
    }
    memset(result, 0, sizeof(*result));
    return true;
}

/* =============================================================================
 * 公共 API
 * ============================================================================= */

int event_benchmark_run_dispatch(uint32_t iterations, event_benchmark_result_t* result) {
    if (!bench_args_valid(result, iterations)) {
        return EVENT_ERR_INVALID_ARG;
    }
    if (atomic_cas(&g_bench_ctx.busy, 0, 1U) != true) {
        return APP_ERR_BUSY;
    }

    if (!event_system_is_running()) {
        atomic_set(&g_bench_ctx.busy, 0);
        return EVENT_ERR_NOT_RUNNING;
    }

    /* 专用基准事件类型：重复注册同名同 ID 幂等（event_system.h 契约） */
    event_status_t st = event_register_type(EVENT_BENCHMARK_EVENT_TYPE, EVENT_BENCHMARK_EVENT_NAME);
    if (st != EVENT_OK) {
        LOG_ERR("register type %u failed: %d", (unsigned int) EVENT_BENCHMARK_EVENT_TYPE, (int) st);
        atomic_set(&g_bench_ctx.busy, 0);
        return (int) st;
    }

    uint32_t sub_id = 0U;
    st = event_subscribe(EVENT_BENCHMARK_EVENT_TYPE, bench_dispatch_callback, NULL, &sub_id);
    if (st != EVENT_OK) {
        LOG_ERR("subscribe type %u failed: %d", (unsigned int) EVENT_BENCHMARK_EVENT_TYPE, (int) st);
        atomic_set(&g_bench_ctx.busy, 0);
        return (int) st;
    }

    g_bench_ctx.expected = iterations;
    g_bench_ctx.run_id++; /* 每轮递增：上轮残留事件负载不匹配，回调直接丢弃 */
    atomic_set(&g_bench_ctx.received, 0);
    /* 先清残留信号量计数（上轮超时后迟到的回调仍可能 give），再武装回调 */
    k_sem_reset(&g_bench_delivery);
    atomic_set(&g_bench_ctx.armed, 1);

    uint32_t failures = 0U;
    bool     timed_out = false;

    /* 乒乓投递：每迭代发布携带轮次 ID 的 4B 内联负载（无动态分配、无打印），等待投递完成 */
    for (uint32_t i = 0U; i < iterations; i++) {
        g_bench_ctx.publish_cycle = k_cycle_get_64();
        st = event_publish_copy(EVENT_BENCHMARK_EVENT_TYPE, EVENT_PRIORITY_NORMAL, &g_bench_ctx.run_id,
                                sizeof(g_bench_ctx.run_id));
        if (st != EVENT_OK) {
            /* 入队失败（队列满/系统停止）：计失败并继续，不污染已收样本 */
            failures++;
            continue;
        }
        if (k_sem_take(&g_bench_delivery, K_MSEC(CONFIG_EVENT_BENCHMARK_DELIVERY_TIMEOUT_MS)) != 0) {
            /* 投递链路异常：中止本轮，避免故障状态下长时间挂起 */
            failures++;
            timed_out = true;
            break;
        }
    }

    atomic_set(&g_bench_ctx.armed, 0);
    (void) event_unsubscribe(EVENT_BENCHMARK_EVENT_TYPE, sub_id);

    uint32_t samples = (uint32_t) atomic_get(&g_bench_ctx.received);
    if (samples > iterations) {
        /* armed 后收到的陈旧样本只计数不落数组，钳回上界保持口径一致 */
        samples = iterations;
    }

    if (samples == 0U) {
        atomic_set(&g_bench_ctx.busy, 0);
        return timed_out ? EVENT_ERR_TIMEOUT : EVENT_ERR_QUEUE_FULL;
    }

    if (failures != 0U) {
        LOG_WRN("dispatch bench: %u failures (publish or delivery)", (unsigned int) failures);
    }
    bench_fill_stats(result, samples, failures);

    atomic_set(&g_bench_ctx.busy, 0);
    return 0;
}

int event_benchmark_run_slab(event_benchmark_slab_tier_t tier, uint32_t iterations, event_benchmark_result_t* result) {
    if (!bench_args_valid(result, iterations)) {
        return EVENT_ERR_INVALID_ARG;
    }
    if (((int) tier < 0) || (tier >= EVENT_BENCH_SLAB_TIER_COUNT)) {
        return EVENT_ERR_INVALID_ARG;
    }
    if (atomic_cas(&g_bench_ctx.busy, 0, 1U) != true) {
        return APP_ERR_BUSY;
    }

    /* 填充固定负载模式：create_with_data 的拷贝耗时属于往返的一部分 */
    for (size_t i = 0U; i < sizeof(g_bench_slab_payload); i++) {
        g_bench_slab_payload[i] = (uint8_t) (i & 0xFFU);
    }

    uint32_t failures = 0U;
    uint32_t count = 0U; /* 写游标：仅成功迭代递增，保证前 count 个样本全部为本轮有效样本 */

    /* 逐迭代 create→free 往返；每迭代至多一个事件在外，不耗尽各档位池 */
    for (uint32_t i = 0U; i < iterations; i++) {
        uint64_t t0 = k_cycle_get_64();

        event_t* ev = NULL;
        if (tier == EVENT_BENCH_SLAB_SHELL) {
            ev = event_create(EVENT_BENCHMARK_EVENT_TYPE, EVENT_PRIORITY_NORMAL);
        } else {
            ev = event_create_with_data(EVENT_BENCHMARK_EVENT_TYPE, EVENT_PRIORITY_NORMAL, g_bench_slab_payload,
                                        g_bench_tier_payload_len[tier]);
        }

        if (ev == NULL) {
            failures++;
            continue;
        }
        event_free(ev);
        g_bench_samples[count] = k_cycle_get_64() - t0;
        count++;
    }

    uint32_t samples = count;
    if (samples == 0U) {
        /* 全部创建失败：事件系统未 init 或池/堆耗尽 */
        LOG_ERR("slab bench tier %d: all %u creates failed", (int) tier, (unsigned int) iterations);
        atomic_set(&g_bench_ctx.busy, 0);
        return EVENT_ERR_NO_MEM;
    }

    if (failures != 0U) {
        LOG_WRN("slab bench tier %d: %u create failures", (int) tier, (unsigned int) failures);
    }
    bench_fill_stats(result, samples, failures);

    atomic_set(&g_bench_ctx.busy, 0);
    return 0;
}
