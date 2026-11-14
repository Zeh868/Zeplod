/**
 * @file event_benchmark_shell.c
 * @brief 事件系统性能基准 Shell 命令
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

#include <zephyr/shell/shell.h>

#include <errno.h>
#include <stdlib.h>

#include <zeplod/event_benchmark.h>

#if defined(CONFIG_SHELL) && IS_ENABLED(CONFIG_EVENT_BENCHMARK_SHELL)

/** slab 档位输出标签（下标与 event_benchmark_slab_tier_t 一一对应） */
static const char* const g_bench_tier_labels[EVENT_BENCH_SLAB_TIER_COUNT] = {
    "slab shell",
    "slab data256",
    "slab data1k",
    "slab data4k",
};

/**
 * @brief 解析可选的迭代次数参数（argc>1 时）
 *
 * @param shell shell 实例（错误输出用）
 * @param arg 迭代次数字符串
 * @param iters 输出解析结果
 * @return 0 成功；-EINVAL 解析失败（非正整数/尾随字符/溢出/超 uint32 上限）
 */
static int bench_parse_iters(const struct shell* shell, const char* arg, uint32_t* iters) {
    char* end = NULL;

    errno = 0;
    long v = strtol(arg, &end, 10);

    /* 整串须为正整数：拒绝空转换/尾随字符/ERANGE 溢出/超 uint32 上限 */
    if ((end == arg) || (*end != '\0') || (errno == ERANGE) || (v <= 0) ||
        ((unsigned long) v > (unsigned long) UINT32_MAX)) {
        shell_print(shell, "bench: error invalid iterations '%s'", arg);
        return -EINVAL;
    }
    *iters = (uint32_t) v;
    return 0;
}

/**
 * @brief 输出一条统计行（ns 与 µs 双单位；`bench:` 前缀为 CI 冒烟约定）
 *
 * @param shell shell 实例
 * @param tag 统计对象标签（dispatch / slab shell / slab data256 ...）
 * @param result 统计结果
 * @param iterations 请求迭代数
 */
static void bench_print_result(const struct shell* shell, const char* tag, const event_benchmark_result_t* result,
                               uint32_t iterations) {
    shell_print(shell,
                "bench: %s samples=%u/%u failures=%u "
                "min=%llu ns (%llu.%03llu us) p50=%llu ns (%llu.%03llu us) p95=%llu ns (%llu.%03llu us) "
                "max=%llu ns (%llu.%03llu us) mean=%llu ns (%llu.%03llu us)",
                tag, (unsigned int) result->samples, (unsigned int) iterations, (unsigned int) result->failures,
                (unsigned long long) result->min_ns, (unsigned long long) (result->min_ns / 1000U),
                (unsigned long long) (result->min_ns % 1000U), (unsigned long long) result->p50_ns,
                (unsigned long long) (result->p50_ns / 1000U), (unsigned long long) (result->p50_ns % 1000U),
                (unsigned long long) result->p95_ns, (unsigned long long) (result->p95_ns / 1000U),
                (unsigned long long) (result->p95_ns % 1000U), (unsigned long long) result->max_ns,
                (unsigned long long) (result->max_ns / 1000U), (unsigned long long) (result->max_ns % 1000U),
                (unsigned long long) result->mean_ns, (unsigned long long) (result->mean_ns / 1000U),
                (unsigned long long) (result->mean_ns % 1000U));
}

/**
 * @brief 依次运行全部 slab 档位基准并输出
 *
 * 单档位失败不阻断其余档位（如个别数据池耗尽时其余档位仍有统计价值）。
 *
 * @param shell shell 实例
 * @param iters 每档位迭代次数
 * @return 0 全部档位成功；否则返回首个失败档位的错误码
 */
static int bench_run_slab_all(const struct shell* shell, uint32_t iters) {
    int rc = 0;

    for (int tier = 0; tier < (int) EVENT_BENCH_SLAB_TIER_COUNT; tier++) {
        event_benchmark_result_t result;
        int                      tier_rc = event_benchmark_run_slab((event_benchmark_slab_tier_t) tier, iters, &result);

        if (tier_rc != 0) {
            shell_print(shell, "bench: error %s rc=%d", g_bench_tier_labels[tier], tier_rc);
            if (rc == 0) {
                rc = tier_rc;
            }
            continue;
        }
        bench_print_result(shell, g_bench_tier_labels[tier], &result, iters);
    }
    return rc;
}

/**
 * @brief `bench dispatch [iters]`：分发延迟基准
 */
static int cmd_bench_dispatch(const struct shell* shell, size_t argc, char** argv) {
    uint32_t iters = EVENT_BENCHMARK_DISPATCH_DEFAULT_ITERS;

    if ((argc > 1) && (bench_parse_iters(shell, argv[1], &iters) != 0)) {
        return -EINVAL;
    }

    event_benchmark_result_t result;
    int                      rc = event_benchmark_run_dispatch(iters, &result);

    if (rc != 0) {
        shell_print(shell, "bench: error dispatch rc=%d", rc);
        return rc;
    }
    bench_print_result(shell, "dispatch", &result, iters);
    return 0;
}

/**
 * @brief `bench slab [iters]`：全部 slab 档位分配往返基准
 */
static int cmd_bench_slab(const struct shell* shell, size_t argc, char** argv) {
    uint32_t iters = EVENT_BENCHMARK_SLAB_DEFAULT_ITERS;

    if ((argc > 1) && (bench_parse_iters(shell, argv[1], &iters) != 0)) {
        return -EINVAL;
    }
    return bench_run_slab_all(shell, iters);
}

/**
 * @brief `bench all [iters]`：dispatch + 全部 slab 档位
 *
 * dispatch 失败不阻断 slab 统计；整体返回首个失败值。
 */
static int cmd_bench_all(const struct shell* shell, size_t argc, char** argv) {
    uint32_t dispatch_iters = EVENT_BENCHMARK_DISPATCH_DEFAULT_ITERS;
    uint32_t slab_iters = EVENT_BENCHMARK_SLAB_DEFAULT_ITERS;

    if (argc > 1) {
        if (bench_parse_iters(shell, argv[1], &dispatch_iters) != 0) {
            return -EINVAL;
        }
        slab_iters = dispatch_iters;
    }

    event_benchmark_result_t result;
    int                      rc = event_benchmark_run_dispatch(dispatch_iters, &result);

    if (rc != 0) {
        shell_print(shell, "bench: error dispatch rc=%d", rc);
    } else {
        bench_print_result(shell, "dispatch", &result, dispatch_iters);
    }

    int slab_rc = bench_run_slab_all(shell, slab_iters);

    return (rc != 0) ? rc : slab_rc;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_bench, SHELL_CMD(dispatch, NULL, "Publish->deliver latency benchmark [iters=1000]", cmd_bench_dispatch),
    SHELL_CMD(slab, NULL, "Create->free slab benchmark per tier [iters=5000]", cmd_bench_slab),
    SHELL_CMD(all, NULL, "Run dispatch + slab benchmarks [iters]", cmd_bench_all), SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(bench, &sub_bench, "Event system performance benchmarks", NULL);

#endif /* CONFIG_SHELL && CONFIG_EVENT_BENCHMARK_SHELL */
