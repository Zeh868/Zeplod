/**
 * @file test_event_memory.c
 * @brief 事件内存管理模块单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-17
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-04-17       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            耗尽回调用例真正触发 slab 耗尽；混合优先级断言精确化；分配类用例自带 init 并套件级 shutdown 清理
 */

#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>
#include <zeplod/event_system.h>
#include "event_memory.h"

LOG_MODULE_REGISTER(test_event_memory);

/** 大量分配用例的分配次数（覆盖 NORMAL 池 4 块上限的 4 倍，验证失败路径与释放） */
#define MASS_ALLOC_COUNT 16

/** 混合优先级用例的总分配次数（4 种优先级 + 8 次 NORMAL 追加） */
#define TEST_MIXED_ALLOC_COUNT 12

/* =============================================================================
 * Slab 选择函数测试
 * ============================================================================= */

/**
 * @brief 测试事件 Slab 选择 - NORMAL 优先级
 */
ZTEST(test_event_memory, test_select_event_slab_normal) {
    struct k_mem_slab* slab;

    slab = event_memory_select_event_slab(EVENT_PRIORITY_NORMAL);
#if EVENT_SLAB_ENABLED
    zassert_not_null(slab, "NORMAL 优先级应返回有效 Slab");
#else
    zassert_is_null(slab, "Slab 未启用时应返回 NULL");
#endif
}

/**
 * @brief 测试事件 Slab 选择 - LOW 优先级
 */
ZTEST(test_event_memory, test_select_event_slab_low) {
    struct k_mem_slab* slab;

    slab = event_memory_select_event_slab(EVENT_PRIORITY_LOW);
#if EVENT_SLAB_ENABLED
    /* LOW 优先级通常回退到 NORMAL 池 */
    zassert_not_null(slab, "LOW 优先级应返回有效 Slab");
#else
    zassert_is_null(slab, "Slab 未启用时应返回 NULL");
#endif
}

/**
 * @brief 测试事件 Slab 选择 - HIGH 优先级
 */
ZTEST(test_event_memory, test_select_event_slab_high) {
    struct k_mem_slab* slab;

    slab = event_memory_select_event_slab(EVENT_PRIORITY_HIGH);
#if EVENT_SLAB_ENABLED && EVENT_SLAB_HIGH_AVAILABLE
    zassert_not_null(slab, "HIGH 优先级应返回有效 Slab");
#elif EVENT_SLAB_ENABLED
    /* 如果没有 HIGH 池，应回退到 NORMAL 池 */
    zassert_not_null(slab, "应回退到 NORMAL Slab");
#else
    zassert_is_null(slab, "Slab 未启用时应返回 NULL");
#endif
}

/**
 * @brief 测试事件 Slab 选择 - CRITICAL 优先级
 */
ZTEST(test_event_memory, test_select_event_slab_critical) {
    struct k_mem_slab* slab;

    slab = event_memory_select_event_slab(EVENT_PRIORITY_CRITICAL);
#if EVENT_SLAB_ENABLED && EVENT_SLAB_CRITICAL_AVAILABLE
    zassert_not_null(slab, "CRITICAL 优先级应返回有效 Slab");
#elif EVENT_SLAB_ENABLED
    /* 如果没有 CRITICAL 池，应回退 */
    zassert_not_null(slab, "应回退到可用 Slab");
#else
    zassert_is_null(slab, "Slab 未启用时应返回 NULL");
#endif
}

/* =============================================================================
 * 数据 Slab 选择函数测试
 * ============================================================================= */

/**
 * @brief 测试数据 Slab 选择 - 小数据
 */
ZTEST(test_event_memory, test_select_data_slab_small) {
    struct k_mem_slab* slab;

    /* 0 字节应返回 NULL */
    slab = event_memory_select_data_slab(0);
    zassert_is_null(slab, "0 字节应返回 NULL");

    /* 小于内联数据大小的数据 */
    slab = event_memory_select_data_slab(16);
#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE && EVENT_SLAB_256_AVAILABLE
    zassert_not_null(slab, "小数据应返回 256B Slab");
#else
    /* 取决于配置 */
#endif
}

/**
 * @brief 测试数据 Slab 选择 - 256 字节边界
 */
ZTEST(test_event_memory, test_select_data_slab_256_boundary) {
    struct k_mem_slab* slab;

    /* 恰好 256 字节 */
    slab = event_memory_select_data_slab(256);
#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE && EVENT_SLAB_256_AVAILABLE
    zassert_not_null(slab, "256 字节应返回 256B Slab");
#endif

    /* 257 字节（超过 256）*/
    slab = event_memory_select_data_slab(257);
#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE && EVENT_SLAB_1K_AVAILABLE
    zassert_not_null(slab, "257 字节应返回 1K Slab");
#elif EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE && EVENT_SLAB_256_AVAILABLE
    zassert_is_null(slab, "超过 256B 但无 1K 池应返回 NULL");
#endif
}

/**
 * @brief 测试数据 Slab 选择 - 1KB 边界
 */
ZTEST(test_event_memory, test_select_data_slab_1k_boundary) {
    struct k_mem_slab* slab;

    /* 恰好 1024 字节 */
    slab = event_memory_select_data_slab(1024);
#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE && EVENT_SLAB_1K_AVAILABLE
    zassert_not_null(slab, "1024 字节应返回 1K Slab");
#endif

    /* 1025 字节 */
    slab = event_memory_select_data_slab(1025);
#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE && EVENT_SLAB_4K_AVAILABLE
    zassert_not_null(slab, "1025 字节应返回 4K Slab");
#endif
}

/**
 * @brief 测试数据 Slab 选择 - 4KB 边界
 */
ZTEST(test_event_memory, test_select_data_slab_4k_boundary) {
    struct k_mem_slab* slab;

    /* 恰好 4096 字节 */
    slab = event_memory_select_data_slab(4096);
#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE && EVENT_SLAB_4K_AVAILABLE
    zassert_not_null(slab, "4096 字节应返回 4K Slab");
#endif

    /* 超过 4KB */
    slab = event_memory_select_data_slab(4097);
    zassert_is_null(slab, "超过 4KB 应返回 NULL（需要 k_malloc）");
}

/**
 * @brief 测试数据 Slab 选择 - 大数据
 */
ZTEST(test_event_memory, test_select_data_slab_large) {
    struct k_mem_slab* slab;

    /* 10KB 数据 */
    slab = event_memory_select_data_slab(10 * 1024);
    zassert_is_null(slab, "10KB 数据应返回 NULL（超出 Slab 范围）");

    /* 1MB 数据 */
    slab = event_memory_select_data_slab(1024 * 1024);
    zassert_is_null(slab, "1MB 数据应返回 NULL");
}

/* =============================================================================
 * 运行时状态 API 测试
 * ============================================================================= */

#if defined(CONFIG_EVENT_RUNTIME_STATUS) && (CONFIG_EVENT_RUNTIME_STATUS == 1)
/**
 * @brief 测试 Slab 可用性检查
 */
ZTEST(test_event_memory, test_slab_available) {
    bool available;

    /* NORMAL 优先级 */
    available = event_slab_available(EVENT_PRIORITY_NORMAL);
#if EVENT_SLAB_ENABLED
    zassert_true(available, "NORMAL Slab 应可用");
#else
    zassert_false(available, "Slab 未启用时应返回 false");
#endif

    /* CRITICAL 优先级 */
    available = event_slab_available(EVENT_PRIORITY_CRITICAL);
#if EVENT_SLAB_ENABLED && EVENT_SLAB_CRITICAL_AVAILABLE
    /* 独立 CRITICAL 池存在且未使用，初始状态应可用 */
    zassert_true(available, "CRITICAL 池应可用");
#elif EVENT_SLAB_ENABLED
    /* 测试配置无独立 CRITICAL 池，回退 NORMAL 池；套件内无泄漏时应有余量 */
    zassert_true(available, "回退 NORMAL 池应可用");
#else
    zassert_false(available, "Slab 未启用时应返回 false");
#endif
}

/**
 * @brief 测试 Slab 剩余块数
 */
ZTEST(test_event_memory, test_slab_remaining) {
    uint32_t remaining;

    remaining = event_slab_remaining(EVENT_PRIORITY_NORMAL);
#if EVENT_SLAB_ENABLED
    zassert_true(remaining > 0, "NORMAL Slab 应有剩余块");
    zassert_true(remaining <= CONFIG_EVENT_SLAB_NORMAL_COUNT, "剩余块不应超过总数");
#else
    zassert_equal(remaining, 0, "Slab 未启用时应返回 0");
#endif
}

/**
 * @brief 测试获取 Slab 统计信息
 */
ZTEST(test_event_memory, test_get_slab_stats) {
    event_slab_stats_t stats;

    event_get_slab_stats(&stats);

#if EVENT_SLAB_ENABLED
    /* NORMAL 池必须存在 */
    zassert_true(stats.normal_total >= 4, "NORMAL 池至少应有 4 块");
    zassert_true(stats.normal_used <= stats.normal_total, "已用块不应超过总数");

#if EVENT_SLAB_CRITICAL_AVAILABLE
    zassert_true(stats.critical_total > 0, "CRITICAL 池应存在");
#endif

#if EVENT_SLAB_HIGH_AVAILABLE
    zassert_true(stats.high_total > 0, "HIGH 池应存在");
#endif

#if EVENT_SLAB_256_AVAILABLE
    zassert_true(stats.data_256_total > 0, "256B 数据池应存在");
#endif

#if EVENT_SLAB_1K_AVAILABLE
    zassert_true(stats.data_1k_total > 0, "1K 数据池应存在");
#endif

#if EVENT_SLAB_4K_AVAILABLE
    zassert_true(stats.data_4k_total > 0, "4K 数据池应存在");
#endif
#else
    /* Slab 未启用时，统计值应全为 0 */
    zassert_equal(stats.normal_total, 0, "Slab 未启用时应为 0");
#endif
}

/**
 * @brief 测试 Slab 统计 NULL 参数
 */
ZTEST(test_event_memory, test_get_slab_stats_null) {
    /* 不应崩溃 */
    event_get_slab_stats(NULL);
}

/**
 * @brief 测试 Slab 耗尽后的统计
 */
ZTEST(test_event_memory, test_slab_stats_after_allocation) {
    event_slab_stats_t stats_before, stats_after;
    event_t*           events[8];
    int                allocated = 0;

    event_get_slab_stats(&stats_before);

    /* 分配一些事件 */
    for (int i = 0; i < 8; i++) {
        events[i] = event_create_rt(100 + i, EVENT_PRIORITY_NORMAL);
        if (events[i] != NULL) {
            allocated++;
        }
    }

    event_get_slab_stats(&stats_after);

#if EVENT_SLAB_ENABLED
    zassert_true(stats_after.normal_used >= stats_before.normal_used + allocated, "已用块应增加");
#endif

    /* 释放事件 */
    for (int i = 0; i < allocated; i++) {
        event_free(events[i]);
    }
}
#endif /* CONFIG_EVENT_RUNTIME_STATUS */

/* =============================================================================
 * Slab 耗尽回调测试
 * ============================================================================= */

#if defined(CONFIG_EVENT_SLAB_EXHAUSTED_CB) && (CONFIG_EVENT_SLAB_EXHAUSTED_CB == 1) && EVENT_SLAB_ENABLED
static int              g_exhausted_callback_count = 0;
static event_priority_t g_last_exhausted_priority = (event_priority_t) 0;

static void test_exhausted_callback(event_priority_t priority, const char* slab_name) {
    g_exhausted_callback_count++;
    g_last_exhausted_priority = priority;
    LOG_INF("Slab exhausted: priority=%d, slab=%s", priority, slab_name);
}

/**
 * @brief 占满 NORMAL 池后继续分配，应触发耗尽回调（注册/触发/清除一条龙）
 *
 * 触发契约（对照 src/core/event_memory.c 与 event_system_memory.c）：
 * event_create_rt 中 k_mem_slab_alloc 失败时调用 notify_slab_exhausted，
 * 每失败一次回调已注册的 g_slab_exhausted_cb 一次，携带触发耗尽的优先级。
 */
ZTEST(test_event_memory, test_exhausted_callback_fires_on_pool_depleted) {
    event_t* events[CONFIG_EVENT_SLAB_NORMAL_COUNT];
    uint32_t allocated = 0U;

    zassert_equal(event_system_init(), EVENT_OK, NULL);

    g_exhausted_callback_count = 0;
    g_last_exhausted_priority = (event_priority_t) 0;
    event_register_slab_exhausted_cb(test_exhausted_callback);

    /* 占满 NORMAL 池：此阶段不应触发回调（event_type_t 为 uint8_t，类型值保持在 255 内） */
    for (int i = 0; i < CONFIG_EVENT_SLAB_NORMAL_COUNT; i++) {
        events[i] = event_create_rt((event_type_t) (240 + i), EVENT_PRIORITY_NORMAL);
        zassert_not_null(events[i], "NORMAL 池未满前分配应成功（第 %d 块）", i);
        allocated++;
    }
    zassert_equal(g_exhausted_callback_count, 0, "池未耗尽不应触发回调");

    /* 池已满：继续分配应失败并恰好触发一次回调 */
    event_t* overflow = event_create_rt(244, EVENT_PRIORITY_NORMAL);

    zassert_is_null(overflow, "NORMAL 池耗尽后 RT 创建应失败");
    zassert_equal(g_exhausted_callback_count, 1, "耗尽回调应恰好触发一次");
    zassert_equal(g_last_exhausted_priority, EVENT_PRIORITY_NORMAL, "回调应携带触发耗尽的优先级");

    /* 清除回调并释放，避免污染后续用例 */
    event_register_slab_exhausted_cb(NULL);
    for (uint32_t i = 0; i < allocated; i++) {
        event_free(events[i]);
    }
}
#endif /* CONFIG_EVENT_SLAB_EXHAUSTED_CB && EVENT_SLAB_ENABLED */

/* =============================================================================
 * 内存调试 API 测试
 * ============================================================================= */

#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
/**
 * @brief 测试内存泄漏检测
 */
ZTEST(test_event_memory, test_check_leaks) {
    uint32_t leaks;
    event_t* event = NULL;

    /* 初始状态应无泄漏 */
    leaks = event_check_leaks();
    zassert_equal(leaks, 0, "初始状态应无泄漏");

    /* 创建事件但不释放 */
    event = event_create(200, EVENT_PRIORITY_NORMAL);
    if (event != NULL) {
        leaks = event_check_leaks();
        zassert_true(leaks >= 1, "创建事件后应有泄漏");

        /* 释放事件 */
        event_free(event);
        leaks = event_check_leaks();
        zassert_equal(leaks, 0, "释放后应无泄漏");
    }
}

/**
 * @brief 测试打印内存泄漏详情
 */
ZTEST(test_event_memory, test_dump_leaks) {
    event_t* event = NULL;

    /* 无泄漏时打印 */
    event_dump_leaks();

    /* 创建事件后打印 */
    event = event_create(201, EVENT_PRIORITY_NORMAL);
    if (event != NULL) {
        event_dump_leaks();
        event_free(event);
    }
}
#endif /* CONFIG_EVENT_DEBUG_MEM */

/* =============================================================================
 * 与事件系统集成测试
 * ============================================================================= */

/**
 * @brief 测试通过事件系统分配的事件使用 Slab
 *
 * event_create_rt 依赖事件系统 magic（未 init 时静默返回 NULL），故自带 init。
 */
ZTEST(test_event_memory, test_event_create_rt_integration) {
    zassert_equal(event_system_init(), EVENT_OK, NULL);

    event_t* event = event_create_rt(210, EVENT_PRIORITY_NORMAL);

    /* 测试配置 NORMAL 池（4 块）空闲，应确定性成功 */
    zassert_not_null(event, "NORMAL 池空闲时 RT 创建应成功");
    zassert_equal(event->type, 210, "事件类型不匹配");
    zassert_equal(event->priority, EVENT_PRIORITY_NORMAL, "优先级不匹配");
    zassert_true((event->flags & EVENT_FLAG_FROM_SLAB) != 0, "应标记为来自 Slab");

    event_free(event);
}

/**
 * @brief 测试带数据的实时安全创建
 *
 * 32 字节 < 内联上限（测试配置 48 字节），HIGH 无独立池回退 NORMAL 池，应确定性成功。
 */
ZTEST(test_event_memory, test_event_create_with_data_rt_integration) {
    event_t* event;
    uint8_t  data[32];

    for (int i = 0; i < 32; i++) {
        data[i] = (uint8_t) (i * 7);
    }

    zassert_equal(event_system_init(), EVENT_OK, NULL);

    event = event_create_with_data_rt(220, EVENT_PRIORITY_HIGH, data, sizeof(data));

    zassert_not_null(event, "内联负载 + 池空闲时 RT 创建应成功");
    zassert_equal(event->type, 220, "事件类型不匹配");
    zassert_equal(event->priority, EVENT_PRIORITY_HIGH, "优先级不匹配");
    zassert_equal(event->data_len, sizeof(data), "数据长度不匹配");
    zassert_true((event->flags & EVENT_FLAG_DATA_INLINE) != 0, "内联上限内应走内联存储");

    /* 验证数据 */
    zassert_mem_equal(event->data.inline_data, data, sizeof(data), "数据内容不匹配");

    event_free(event);
}

/**
 * @brief 测试大量分配后的 Slab 状态
 */
ZTEST(test_event_memory, test_mass_allocation) {
    event_t* events[MASS_ALLOC_COUNT] = {0};
    int      allocated = 0;

    zassert_equal(event_system_init(), EVENT_OK, NULL);

#if defined(CONFIG_EVENT_RUNTIME_STATUS) && (CONFIG_EVENT_RUNTIME_STATUS == 1)
    event_slab_stats_t stats_before;

    event_get_slab_stats(&stats_before);
#endif

    /* 大量分配：超出 NORMAL 池容量的部分应失败而不是崩溃 */
    for (int i = 0; i < MASS_ALLOC_COUNT; i++) {
        events[i] = event_create_rt(230 + i, EVENT_PRIORITY_NORMAL);
        if (events[i] != NULL) {
            allocated++;
        }
    }

    /* 应该成功分配一些事件 */
    zassert_true(allocated > 0, "应至少分配一个事件");

#if defined(CONFIG_EVENT_RUNTIME_STATUS) && (CONFIG_EVENT_RUNTIME_STATUS == 1)
    /* 检查 Slab 状态 */
    event_slab_stats_t stats;
    event_get_slab_stats(&stats);
    zassert_true(stats.normal_used >= (uint32_t) allocated, "已用块数应正确");
#endif

    /* 全部释放 */
    for (int i = 0; i < allocated; i++) {
        event_free(events[i]);
    }

#if defined(CONFIG_EVENT_RUNTIME_STATUS) && (CONFIG_EVENT_RUNTIME_STATUS == 1)
    /* 验证释放后的状态：已用块应回到分配前水位 */
    event_get_slab_stats(&stats);
    zassert_equal(stats.normal_used, stats_before.normal_used, "全部释放后已用块应恢复原水位");
#endif
}

/**
 * @brief 测试不同优先级混合分配
 *
 * 测试配置（tests/prj.conf）：HIGH/CRITICAL 池为 0 块，四种优先级全部回退
 * NORMAL 池（4 块，见 event_memory.c 的 select 回退设计）。因此前 4 次分配
 * （CRITICAL/HIGH/NORMAL/LOW）恰好占满 NORMAL 池，后续 8 次 NORMAL 分配全部失败。
 */
ZTEST(test_event_memory, test_mixed_priority_allocation) {
    event_t* events[TEST_MIXED_ALLOC_COUNT] = {0};
    uint32_t allocated = 0U;

    zassert_equal(event_system_init(), EVENT_OK, NULL);

    /* 混合优先级分配（固定槽位，便于逐槽断言） */
    events[0] = event_create_rt(240, EVENT_PRIORITY_CRITICAL);
    events[1] = event_create_rt(241, EVENT_PRIORITY_HIGH);
    events[2] = event_create_rt(242, EVENT_PRIORITY_NORMAL);
    events[3] = event_create_rt(243, EVENT_PRIORITY_LOW);

    /* 再追加 NORMAL 分配直到超出池容量（240+i 对应 244..251，保持在 uint8_t 域内） */
    for (int i = 4; i < TEST_MIXED_ALLOC_COUNT; i++) {
        events[i] = event_create_rt((event_type_t) (240 + i), EVENT_PRIORITY_NORMAL);
    }

    for (int i = 0; i < TEST_MIXED_ALLOC_COUNT; i++) {
        if (events[i] != NULL) {
            allocated++;
        }
    }

#if EVENT_SLAB_ENABLED && !EVENT_SLAB_HIGH_AVAILABLE && !EVENT_SLAB_CRITICAL_AVAILABLE
    /* 全部回退 NORMAL 池：恰好前 4 次成功 */
    zassert_equal(allocated, 4U, "四种优先级共享 NORMAL 池时应有 4 次成功");
    for (int i = 0; i < 4; i++) {
        zassert_not_null(events[i], "前 4 次分配（槽位 %d）应成功", i);
    }
    for (int i = 4; i < TEST_MIXED_ALLOC_COUNT; i++) {
        zassert_is_null(events[i], "NORMAL 池已满，槽位 %d 分配应失败", i);
    }
#else
    /* 存在独立 HIGH/CRITICAL 池的配置下不假设精确水位，只验证基本可用性 */
    zassert_true(allocated >= 1U, "应至少成功分配一个事件");
#endif

    /* 全部释放（含 NULL 槽位跳过） */
    for (int i = 0; i < TEST_MIXED_ALLOC_COUNT; i++) {
        if (events[i] != NULL) {
            event_free(events[i]);
        }
    }
}

/* =============================================================================
 * 测试套件
 * ============================================================================= */

static void event_memory_after_each(void* fixture) {
    (void) fixture;
    /* 分配类用例自带 init；此处统一 shutdown，清除类型表并复位控制块，
     * 避免依赖其他套件遗留的初始化状态 */
    (void) event_system_shutdown();
}

ZTEST_SUITE(test_event_memory, NULL, NULL, NULL, event_memory_after_each, NULL);
