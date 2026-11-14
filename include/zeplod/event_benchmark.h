/**
 * @file event_benchmark.h
 * @brief 事件系统性能基准模块公共头文件
 *
 * 提供事件系统 publish→deliver 分发延迟与事件 slab 分配往返开销的
 * 统计测量接口：程序化 API（ztest/上层代码可直接调用）+ shell 根命令
 * `bench`（dispatch / slab / all 子命令）。
 *
 * 测量口径：
 * - dispatch：发布时刻（event_publish_copy 调用前取 k_cycle_get_64()）到
 *   订阅回调入口（回调内再取 k_cycle_get_64()）的差值，含队列等待与
 *   发布线程→分发线程切换开销；逐事件乒乓投递，测量期间队列中至多
 *   存在一个在途事件。
 * - slab：基准事件 create→release（event_create / event_create_with_data
 *   至 event_free）的完整往返耗时，按事件外壳与 256B/1KB/4KB 数据块
 *   各档位分别统计。
 *
 * 统计输出：样本数 + min/p50/p95/max/mean（单位 ns，由
 * k_cyc_to_ns_floor64() 换算），shell 输出行统一以 `bench:` 前缀开头
 * （CI 冒烟依赖此前缀，勿改）。
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

#ifndef EVENT_BENCHMARK_H
#define EVENT_BENCHMARK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zeplod/event_system.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 常量定义
 * ============================================================================= */

/**
 * @brief 基准专用事件类型 ID
 * @note 须先经 event_register_type() 注册（run 函数内自动注册，幂等）；
 *       该 ID 由本模块独占，须 CONFIG_EVENT_MAX_TYPES > 80
 */
#define EVENT_BENCHMARK_EVENT_TYPE             ((event_type_t) 80U)

/** 基准专用事件类型名称（注册用，shell `app events` 可见） */
#define EVENT_BENCHMARK_EVENT_NAME             "bench_dispatch"

/**
 * @brief dispatch 基准默认迭代次数
 * @note shell `bench dispatch` / `bench all` 未带参数时使用
 */
#define EVENT_BENCHMARK_DISPATCH_DEFAULT_ITERS 1000U

/**
 * @brief slab 基准默认迭代次数
 * @note shell `bench slab` / `bench all` 未带参数时使用
 */
#define EVENT_BENCHMARK_SLAB_DEFAULT_ITERS     5000U

/* =============================================================================
 * 类型定义
 * ============================================================================= */

/**
 * @brief 基准统计结果
 *
 * 各分位与均值均由预分配数组中的有效样本排序后计算，单位 ns
 * （uint64）；shell 输出同时给出 ns 与 µs（µs = ns/1000，3 位小数）。
 */
typedef struct {
    uint32_t samples;  /**< 有效样本数（投递/分配成功的迭代数） */
    uint32_t failures; /**< 失败迭代数（发布失败/投递超时/创建失败） */
    uint64_t min_ns;   /**< 最小值（ns） */
    uint64_t p50_ns;   /**< 中位数 p50（ns） */
    uint64_t p95_ns;   /**< 95 分位数 p95（ns） */
    uint64_t max_ns;   /**< 最大值（ns） */
    uint64_t mean_ns;  /**< 平均值（ns） */
} event_benchmark_result_t;

/**
 * @brief slab 基准档位
 *
 * 每个档位对应一次 event_benchmark_run_slab() 调用，shell `bench slab`
 * 会依次运行全部档位并在输出行标注档位名。
 *
 * 档位与测量对象：
 * - EVENT_BENCH_SLAB_SHELL：事件外壳 create→free 往返
 *   （slab NORMAL 池，slab 未启用时为 k_malloc 回退路径）。
 * - EVENT_BENCH_SLAB_DATA_256：外壳 + 256B 数据块 create→free 往返
 *   （负载 data_len=200，> 内联阈值且 ≤256B）。
 * - EVENT_BENCH_SLAB_DATA_1K：外壳 + 1KB 数据块（负载 data_len=600）。
 * - EVENT_BENCH_SLAB_DATA_4K：外壳 + 4KB 数据块（负载 data_len=3000）。
 *
 * @note 数据档位在对应 slab 池未启用/未配置时自动回退 k_malloc 路径，
 *       此时统计的是回退路径的开销（conf 提示见仓库 Kconfig help）。
 */
typedef enum {
    EVENT_BENCH_SLAB_SHELL = 0,    /**< 事件外壳（NORMAL 池 / 堆回退） */
    EVENT_BENCH_SLAB_DATA_256 = 1, /**< 256B 数据块档（负载 200B） */
    EVENT_BENCH_SLAB_DATA_1K = 2,  /**< 1KB 数据块档（负载 600B） */
    EVENT_BENCH_SLAB_DATA_4K = 3,  /**< 4KB 数据块档（负载 3000B） */
    EVENT_BENCH_SLAB_TIER_COUNT    /**< 档位总数（数组长度用） */
} event_benchmark_slab_tier_t;

/* =============================================================================
 * 公共 API
 * 仅线程上下文调用（内部拒绝 ISR）；可重复执行；同一时刻仅允许一个
 * 基准在运行（并发调用返回 APP_ERR_BUSY）。
 * ============================================================================= */

/**
 * @brief 运行分发延迟基准（publish→deliver）
 *
 * 自动注册并订阅专用基准事件类型（幂等），随后逐迭代以
 * event_publish_copy(EVENT_BENCHMARK_EVENT_TYPE, NORMAL, 负载 4B) 发布，
 * 等待订阅回调投递完成后进入下一迭代；样本为发布前与回调入口的
 * cycle 差，结束后排序计算分位数。测量循环内无动态分配、无打印。
 *
 * @param iterations 迭代次数（1..CONFIG_EVENT_BENCHMARK_MAX_SAMPLES，
 *                   超出上限返回 EVENT_ERR_INVALID_ARG）
 * @param result 输出统计结果（不能为 NULL；samples==0 时内容无意义）
 * @return 0 成功（result 有效）
 * @return EVENT_ERR_INVALID_ARG 参数非法（NULL/0/超上限/ISR 上下文）
 * @return APP_ERR_BUSY 另一基准正在运行
 * @return EVENT_ERR_NOT_RUNNING 事件系统未处于 running
 * @return EVENT_ERR_NOT_FOUND 事件类型注册或订阅失败
 * @return EVENT_ERR_QUEUE_FULL 发布入队失败且无任何样本
 * @return EVENT_ERR_TIMEOUT 投递超时且无任何样本（逐迭代超时即中止本轮，
 *                          已收样本仍计入 result 并返回 0）
 *
 * @note 须在事件系统 init+start 之后调用（应用默认 AUTOINIT 已满足）
 * @note 返回后订阅已取消；可重复调用
 * @note 事件负载携带每轮递增的轮次 ID：上一轮投递超时后经取消订阅快照
 *       迟到执行的残留事件会在回调中被直接丢弃（不写样本、不给出信号量），
 *       因此残留事件不会污染本轮统计或打乱乒乓节拍
 */
int event_benchmark_run_dispatch(uint32_t iterations, event_benchmark_result_t* result);

/**
 * @brief 运行单档位 slab 分配基准（create→release 往返）
 *
 * 逐迭代创建基准事件（按 tier 决定是否附带数据负载）并立即
 * event_free 释放，统计往返耗时；不经事件队列、不投递。
 * 各档位测量对象见 event_benchmark_slab_tier_t 说明。
 *
 * @param tier 基准档位（EVENT_BENCH_SLAB_*）
 * @param iterations 迭代次数（1..CONFIG_EVENT_BENCHMARK_MAX_SAMPLES）
 * @param result 输出统计结果（不能为 NULL）
 * @return 0 成功（result 有效）
 * @return EVENT_ERR_INVALID_ARG 参数非法（tier 越界/NULL/0/超上限/ISR）
 * @return APP_ERR_BUSY 另一基准正在运行
 * @return EVENT_ERR_NO_MEM 全部迭代创建失败（事件系统未初始化或池/堆耗尽）
 *
 * @note 与 dispatch 不同：不要求事件系统处于 running（仅要求已 init）
 * @note 返回后无残留资源；可重复调用
 */
int event_benchmark_run_slab(event_benchmark_slab_tier_t tier, uint32_t iterations, event_benchmark_result_t* result);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_BENCHMARK_H */
