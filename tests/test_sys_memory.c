/**
 * @file test_sys_memory.c
 * @brief sys_memory 单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            新增分配器整块路径防复发用例
 *                                                 （actual_block_size 别名覆写修复）
 *
 */

#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>
#include <zeplod/sys_memory.h>

LOG_MODULE_REGISTER(test_sys_memory);

/* 整块路径用例的自定义池大小：8 对齐，预留 header 后仍可容纳可观测的剩余空间 */
#define WHOLE_BLOCK_POOL_SIZE 256U

ZTEST(sys_memory, test_init_and_alloc_free) {
    void* p;

    zassert_equal(sys_mem_init(NULL), 0, "sys_mem_init 失败");

    p = sys_mem_alloc(SYS_MEM_POOL_GENERAL, 32);
    zassert_not_null(p, "alloc 失败");

    sys_mem_free(SYS_MEM_POOL_GENERAL, p);
}

ZTEST(sys_memory, test_zero_size_alloc) {
    zassert_equal(sys_mem_init(NULL), 0, NULL);

    zassert_is_null(sys_mem_alloc(SYS_MEM_POOL_GENERAL, 0), "size 0 应返回 NULL");
}

ZTEST(sys_memory, test_stats) {
    sys_mem_stats_t stats;

    zassert_equal(sys_mem_init(NULL), 0, NULL);

    sys_mem_get_stats(SYS_MEM_POOL_GENERAL, &stats);
    zassert_true(stats.total_size > 0U, "应有池大小");
}

/**
 * @brief 分配器整块路径防复发用例（2026-09-18 Critical 修复）
 *
 * first-fit 分割分支剩余不足 MIN_BLOCK_SIZE 时整块消耗，actual_block_size 必须
 * 取「写入 header 前」保存的原空闲块尺寸。历史缺陷在写入 header 后才读该字段，
 * 读到被覆写的垃圾值，导致 used_size 账目漂移、释放后块缩水、同尺寸再分配失败。
 *
 * header 大小随实现（如新增 freed_magic 字段）变化，故不硬编码：利用「分配成功
 * 当且仅当 header+对齐请求 <= 池大小」的单调性，在自定义小池上二分出最大可分配
 * 请求——该请求恰好触发整块消耗。
 */
ZTEST(sys_memory, test_alloc_whole_block_accounting) {
    sys_mem_config_t cfg = {
        .pool_sizes =
            {
                [SYS_MEM_POOL_GENERAL] = WHOLE_BLOCK_POOL_SIZE,
                [SYS_MEM_POOL_EVENT] = 0,
                [SYS_MEM_POOL_MODULE] = 0,
                [SYS_MEM_POOL_DMA] = 0,
            },
        .enable_tracking = true,
        .enable_defrag = false,
        .max_allocations = 8,
    };
    sys_mem_stats_t stats;
    size_t          lo = 1U;
    size_t          hi = WHOLE_BLOCK_POOL_SIZE;
    void*           p;

    zassert_equal(sys_mem_init(&cfg), 0, "自定义小池初始化失败");

    /* 二分最大可分配请求；每次探测后释放，空闲链合并回单一整块，保证单调前提 */
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1U) / 2U;

        p = sys_mem_alloc(SYS_MEM_POOL_GENERAL, mid);
        if (p != NULL) {
            sys_mem_free(SYS_MEM_POOL_GENERAL, p);
            lo = mid;
        } else {
            hi = mid - 1U;
        }
    }
    zassert_true(lo > 0U && lo < WHOLE_BLOCK_POOL_SIZE, "最大可分配请求应落在 (0, 池大小) 内");

    /* 最大请求触发整块消耗：账目必须记整块而非请求值 */
    p = sys_mem_alloc(SYS_MEM_POOL_GENERAL, lo);
    zassert_not_null(p, "最大可分配请求应成功");
    sys_mem_get_stats(SYS_MEM_POOL_GENERAL, &stats);
    zassert_equal(stats.used_size, stats.total_size, "整块消耗后 used_size 应等于整池（账目不漂移）");

    /* 释放后块不缩水：账目清零、可用恢复整池 */
    sys_mem_free(SYS_MEM_POOL_GENERAL, p);
    sys_mem_get_stats(SYS_MEM_POOL_GENERAL, &stats);
    zassert_equal(stats.used_size, 0U, "释放后 used_size 应归零");
    zassert_equal(stats.free_size, stats.total_size, "释放后 free_size 应恢复整池");

    /* 同尺寸再分配应成功，且账目再次精确等于整池 */
    p = sys_mem_alloc(SYS_MEM_POOL_GENERAL, lo);
    zassert_not_null(p, "释放后同尺寸再分配应成功（块不得缩水）");
    sys_mem_get_stats(SYS_MEM_POOL_GENERAL, &stats);
    zassert_equal(stats.used_size, stats.total_size, "再分配后账目应再次等于整池");

    sys_mem_free(SYS_MEM_POOL_GENERAL, p);
    zassert_equal(sys_mem_get_active_allocations(SYS_MEM_POOL_GENERAL), 0U, "无活跃分配残留");
}

ZTEST_SUITE(sys_memory, NULL, NULL, NULL, NULL, NULL);
