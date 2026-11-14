/**
 * @file test_event_benchmark.c
 * @brief 事件系统性能基准模块 ztest 冒烟测试
 *
 * 仅做结构性断言：返回码、samples/failures 计数以及 min/p50/p95/max/mean
 * 之间的统计关系；不做任何绝对时序数值断言（native_sim/QEMU 计时仅可作
 * 功能回归对比，不能作为门禁）。迭代次数取小值（16）控制总时长。
 *
 * 前置：tests/prj.conf 关闭 EVENT_SYSTEM_AUTOINIT，事件系统由本套件夹具
 * 自行 init+start（两者幂等，其他套件已启动时直接复用）。
 * 配套叠加层 tests/prj_benchmark.conf：开启模块、关闭 shell 子命令。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-11-14
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-11-14       1.0            zeh             B1 冒烟套件初始版本
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <stdint.h>

#include <zeplod/event_benchmark.h>
#include <zeplod/event_system.h>

/** 冒烟迭代次数：小值控制总时长，须远小于 CONFIG_EVENT_BENCHMARK_MAX_SAMPLES */
#define BENCH_SMOKE_ITERATIONS 16U

/** slab 冒烟档位表：覆盖全部正式档位（数组长度由 EVENT_BENCH_SLAB_TIER_COUNT 决定） */
static const event_benchmark_slab_tier_t g_smoke_slab_tiers[EVENT_BENCH_SLAB_TIER_COUNT] = {
    EVENT_BENCH_SLAB_SHELL,
    EVENT_BENCH_SLAB_DATA_256,
    EVENT_BENCH_SLAB_DATA_1K,
    EVENT_BENCH_SLAB_DATA_4K,
};

/** 档位表对应的断言消息标签（与 g_smoke_slab_tiers 一一对应） */
static const char* g_smoke_slab_tier_names[EVENT_BENCH_SLAB_TIER_COUNT] = {
    "slab shell",
    "slab data256",
    "slab data1k",
    "slab data4k",
};

/* =============================================================================
 * 断言辅助
 * ============================================================================= */

/**
 * @brief 断言一轮基准成功并满足统计结构关系
 *
 * 结构关系：min <= p50 <= p95 <= max，且均值落在 [min, max] 区间内；
 * 不涉及任何绝对时序数值。
 *
 * @param rc 基准运行返回码（0 表示成功）
 * @param expect_iters 预期有效样本数（等于本次迭代次数）
 * @param r 基准输出结果
 * @param tag 断言消息标签（定位失败档位/类型）
 */
static void assert_bench_run_ok(int rc, uint32_t expect_iters, const event_benchmark_result_t* r, const char* tag) {
    zassert_equal(rc, 0, "%s: 基准应成功 rc=%d", tag, rc);
    zassert_equal(r->samples, expect_iters, "%s: samples 应为 %u 实为 %u", tag, (unsigned int) expect_iters,
                  (unsigned int) r->samples);
    zassert_equal(r->failures, 0, "%s: failures 应为 0 实为 %u", tag, (unsigned int) r->failures);
    zassert_true(r->min_ns <= r->p50_ns, "%s: min(%llu) 应 <= p50(%llu)", tag, (unsigned long long) r->min_ns,
                 (unsigned long long) r->p50_ns);
    zassert_true(r->p50_ns <= r->p95_ns, "%s: p50(%llu) 应 <= p95(%llu)", tag, (unsigned long long) r->p50_ns,
                 (unsigned long long) r->p95_ns);
    zassert_true(r->p95_ns <= r->max_ns, "%s: p95(%llu) 应 <= max(%llu)", tag, (unsigned long long) r->p95_ns,
                 (unsigned long long) r->max_ns);
    zassert_true(r->min_ns <= r->mean_ns, "%s: min(%llu) 应 <= mean(%llu)", tag, (unsigned long long) r->min_ns,
                 (unsigned long long) r->mean_ns);
    zassert_true(r->mean_ns <= r->max_ns, "%s: mean(%llu) 应 <= max(%llu)", tag, (unsigned long long) r->mean_ns,
                 (unsigned long long) r->max_ns);
}

/* =============================================================================
 * 测试用例
 * ============================================================================= */

/**
 * @brief dispatch 基准冒烟：单轮运行的返回码与统计结构
 */
ZTEST(test_event_benchmark, test_bench_dispatch_smoke) {
    event_benchmark_result_t r;

    int rc = event_benchmark_run_dispatch(BENCH_SMOKE_ITERATIONS, &r);
    assert_bench_run_ok(rc, BENCH_SMOKE_ITERATIONS, &r, "dispatch");
}

/**
 * @brief dispatch 基准重复运行：覆盖 run_id 递增与再次订阅路径
 */
ZTEST(test_event_benchmark, test_bench_dispatch_repeat_runs) {
    event_benchmark_result_t r;

    for (uint32_t round = 1U; round <= 2U; round++) {
        int rc = event_benchmark_run_dispatch(BENCH_SMOKE_ITERATIONS, &r);
        assert_bench_run_ok(rc, BENCH_SMOKE_ITERATIONS, &r, (round == 1U) ? "dispatch run1" : "dispatch run2");
    }
}

/**
 * @brief slab 基准冒烟：全部档位逐一运行并校验统计结构
 *
 * 本测试配置（EVENT_SLAB_LARGE_ENABLE=n）下数据档位走 k_malloc 回退路径，
 * 属预期，不断言具体路径。
 */
ZTEST(test_event_benchmark, test_bench_slab_all_tiers) {
    event_benchmark_result_t r;

    for (uint32_t i = 0U; i < (uint32_t) EVENT_BENCH_SLAB_TIER_COUNT; i++) {
        int rc = event_benchmark_run_slab(g_smoke_slab_tiers[i], BENCH_SMOKE_ITERATIONS, &r);
        assert_bench_run_ok(rc, BENCH_SMOKE_ITERATIONS, &r, g_smoke_slab_tier_names[i]);
    }
}

/**
 * @brief 非法参数拒绝：result NULL / iterations 0 / 超 MAX_SAMPLES / tier 越界
 */
ZTEST(test_event_benchmark, test_bench_invalid_args_rejected) {
    event_benchmark_result_t r;

    /* result 为 NULL：dispatch 与 slab 均拒绝 */
    zassert_equal(event_benchmark_run_dispatch(BENCH_SMOKE_ITERATIONS, NULL), EVENT_ERR_INVALID_ARG,
                  "result=NULL 应返回 EVENT_ERR_INVALID_ARG");
    zassert_equal(event_benchmark_run_slab(EVENT_BENCH_SLAB_SHELL, BENCH_SMOKE_ITERATIONS, NULL), EVENT_ERR_INVALID_ARG,
                  "result=NULL 应返回 EVENT_ERR_INVALID_ARG");

    /* iterations 为 0 */
    zassert_equal(event_benchmark_run_dispatch(0U, &r), EVENT_ERR_INVALID_ARG,
                  "iterations=0 应返回 EVENT_ERR_INVALID_ARG");
    zassert_equal(event_benchmark_run_slab(EVENT_BENCH_SLAB_SHELL, 0U, &r), EVENT_ERR_INVALID_ARG,
                  "iterations=0 应返回 EVENT_ERR_INVALID_ARG");

    /* iterations 超过预分配样本上限（上限本身合法，+1 非法） */
    zassert_equal(event_benchmark_run_dispatch((uint32_t) CONFIG_EVENT_BENCHMARK_MAX_SAMPLES + 1U, &r),
                  EVENT_ERR_INVALID_ARG, "iterations 超上限应返回 EVENT_ERR_INVALID_ARG");
    zassert_equal(
        event_benchmark_run_slab(EVENT_BENCH_SLAB_SHELL, (uint32_t) CONFIG_EVENT_BENCHMARK_MAX_SAMPLES + 1U, &r),
        EVENT_ERR_INVALID_ARG, "iterations 超上限应返回 EVENT_ERR_INVALID_ARG");

    /* tier 越界：档位总数为哨兵值，非法 */
    zassert_equal(event_benchmark_run_slab(EVENT_BENCH_SLAB_TIER_COUNT, BENCH_SMOKE_ITERATIONS, &r),
                  EVENT_ERR_INVALID_ARG, "tier 越界应返回 EVENT_ERR_INVALID_ARG");
}

/* =============================================================================
 * 测试夹具与套件
 * ============================================================================= */

/**
 * @brief 套件级前置：确保事件系统处于 running
 *
 * tests/prj.conf 关闭 AUTOINIT；init/start 均幂等，其他套件已启动时直接复用。
 */
static void* bench_suite_setup(void) {
    int ret;

    if (!event_system_is_running()) {
        ret = event_system_init();
        zassert_true(ret == EVENT_OK || ret == -EALREADY, "event_system_init failed: %d", ret);
        ret = event_system_start();
        zassert_true(ret == EVENT_OK || ret == -EALREADY, "event_system_start failed: %d", ret);
    }
    return NULL;
}

/**
 * @brief 套件级收尾：停止事件系统（stop 幂等，未运行时无害）
 */
static void bench_suite_teardown(void* fixture) {
    (void) fixture;
    (void) event_system_stop();
}

/* Zephyr: setup, before_each, after_each, suite_teardown（对照 test_event_system.c） */
ZTEST_SUITE(test_event_benchmark, NULL, bench_suite_setup, NULL, NULL, bench_suite_teardown);
