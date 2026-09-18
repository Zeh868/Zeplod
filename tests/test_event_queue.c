/**
 * @file test_event_queue.c
 * @brief 事件队列单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            套件声明前的用例归位；循环内入队/出队返回值检查
 *
 */

#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>
#include "event_queue.h"

LOG_MODULE_REGISTER(test_event_queue);

static K_THREAD_STACK_DEFINE(g_delayed_enqueue_stack, 1024);
static struct k_thread g_delayed_enqueue_thread;

static void delayed_enqueue_thread(void* queue_arg, void* event_arg, void* unused) {
    ARG_UNUSED(unused);

    struct k_msgq* queue = queue_arg;
    const event_t* event = event_arg;

    (void) event_queue_enqueue(queue, event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT);
}

/* =============================================================================
 * 测试用例
 * ============================================================================= */

/**
 * @brief 测试队列初始化
 */
ZTEST(test_event_queue, test_queue_init) {
    struct k_msgq  test_queue;
    char           buffer[4 * sizeof(event_t)];
    event_status_t status;

    /* 测试正常初始化 */
    status = event_queue_init(&test_queue, buffer, 4);
    zassert_equal(status, EVENT_OK, "队列初始化失败");

    /* 测试空参数 */
    status = event_queue_init(NULL, buffer, 4);
    zassert_equal(status, EVENT_ERR_INVALID_ARG, "应拒绝空队列参数");

    status = event_queue_init(&test_queue, NULL, 4);
    zassert_equal(status, EVENT_ERR_INVALID_ARG, "应拒绝空缓冲区参数");

    event_queue_deinit(&test_queue);
}

/**
 * @brief 测试队列入队和出队
 */
ZTEST(test_event_queue, test_queue_enqueue_dequeue) {
    struct k_msgq  test_queue;
    char           buffer[4 * sizeof(event_t)];
    event_status_t status;
    event_t        event_in = {0};
    event_t        event_out;

    event_in.type = 50;
    event_in.priority = EVENT_PRIORITY_NORMAL;
    event_in.data_len = 0;

    /* 初始化 */
    status = event_queue_init(&test_queue, buffer, 4);
    zassert_equal(status, EVENT_OK, "队列初始化失败");

    /* 测试入队 */
    status = event_queue_enqueue(&test_queue, &event_in, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, "入队失败");

    /* 测试队列深度 */
    zassert_equal(event_queue_depth(&test_queue), 1, "队列深度应为 1");

    /* 测试出队 */
    status = event_queue_dequeue(&test_queue, &event_out, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, "出队失败");
    zassert_equal(event_out.type, 50, "事件类型不匹配");

    /* 测试空队列出队 */
    status = event_queue_dequeue(&test_queue, &event_out, K_NO_WAIT);
    zassert_equal(status, EVENT_ERR_QUEUE_EMPTY, "空队列应返回 EMPTY");

    event_queue_deinit(&test_queue);
}

/**
 * @brief 测试队列满的情况
 */
ZTEST(test_event_queue, test_queue_full) {
    struct k_msgq  test_queue;
    char           buffer[3 * sizeof(event_t)];
    event_status_t status;
    event_t        event = {.type = 51, .priority = EVENT_PRIORITY_NORMAL};

    /* 初始化小队列 */
    status = event_queue_init(&test_queue, buffer, 3);
    zassert_equal(status, EVENT_OK, "队列初始化失败");

    /* 填满队列 */
    status = event_queue_enqueue(&test_queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, "入队 1 失败");

    status = event_queue_enqueue(&test_queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, "入队 2 失败");

    status = event_queue_enqueue(&test_queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, "入队 3 失败");

    /* 队列已满，新入队应失败（DROP_NEWEST 策略）*/
    status = event_queue_enqueue(&test_queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_ERR_QUEUE_FULL, "队列满时应返回 FULL");

    /* 测试 is_full */
    zassert_true(event_queue_is_full(&test_queue), "队列应标记为满");

    event_queue_deinit(&test_queue);
}

/**
 * @brief 测试 DROP_LOWEST：丢弃队列中优先级最低的一条（同优先级 FIFO 最旧）
 */
ZTEST(test_event_queue, test_queue_overflow_drop_lowest) {
    struct k_msgq  test_queue;
    char           buffer[3 * sizeof(event_t)];
    event_status_t status;
    event_t        out;

    status = event_queue_init(&test_queue, buffer, 3);
    zassert_equal(status, EVENT_OK, "队列初始化失败");

    event_t e1 = {.type = 1, .priority = EVENT_PRIORITY_NORMAL};
    event_t e2 = {.type = 2, .priority = EVENT_PRIORITY_NORMAL};
    event_t e3 = {.type = 3, .priority = EVENT_PRIORITY_NORMAL};
    event_t hi = {.type = 99, .priority = EVENT_PRIORITY_HIGH};

    status = event_queue_enqueue(&test_queue, &e1, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, "入队 e1");
    status = event_queue_enqueue(&test_queue, &e2, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, "入队 e2");
    status = event_queue_enqueue(&test_queue, &e3, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, "入队 e3");

    status = event_queue_enqueue(&test_queue, &hi, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, "高优先级应挤掉队列中最低/最旧的一条");

    zassert_equal(event_queue_depth(&test_queue), 3, "深度仍为 3");

    status = event_queue_dequeue(&test_queue, &out, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, "出队 1");
    zassert_equal(out.type, 2, "应丢弃最旧 NORMAL，保留 e2");

    status = event_queue_dequeue(&test_queue, &out, K_NO_WAIT);
    zassert_equal(out.type, 3, "e3");

    status = event_queue_dequeue(&test_queue, &out, K_NO_WAIT);
    zassert_equal(out.type, 99, "高优先级事件应在队尾入队");

    event_queue_deinit(&test_queue);
}

/**
 * @brief DROP_LOWEST 成功挤入时不应虚增 overflow_count（仅 drop_count 增加）
 */
ZTEST(test_event_queue, test_drop_lowest_no_spurious_overflow_count) {
    struct k_msgq  test_queue;
    char           buffer[2 * sizeof(event_t)];
    event_status_t status;
    queue_stats_t  stats;
    event_t        e1 = {.type = 1, .priority = EVENT_PRIORITY_NORMAL};
    event_t        e2 = {.type = 2, .priority = EVENT_PRIORITY_NORMAL};
    event_t        hi = {.type = 99, .priority = EVENT_PRIORITY_HIGH};

    status = event_queue_init(&test_queue, buffer, 2);
    zassert_equal(status, EVENT_OK, NULL);

    status = event_queue_enqueue(&test_queue, &e1, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, NULL);
    status = event_queue_enqueue(&test_queue, &e2, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, NULL);

    status = event_queue_enqueue(&test_queue, &hi, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, NULL);

    event_queue_get_stats(&test_queue, &stats);
    zassert_equal(stats.overflow_count, 0, "成功挤入不应计入 overflow");
    zassert_equal(stats.drop_count, 1, "应仅统计被踢出的一条");
    zassert_equal(stats.enqueue_count, 3, NULL);

    event_queue_deinit(&test_queue);
}

ZTEST(test_event_queue, test_drop_lowest_uses_nonblocking_full_probe) {
    struct k_msgq  test_queue;
    char           buffer[2 * sizeof(event_t)];
    event_status_t status;
    event_t        out;
    event_t        e1 = {.type = 1, .priority = EVENT_PRIORITY_NORMAL};
    event_t        e2 = {.type = 2, .priority = EVENT_PRIORITY_NORMAL};
    event_t        hi = {.type = 99, .priority = EVENT_PRIORITY_HIGH};

    status = event_queue_init(&test_queue, buffer, 2);
    zassert_equal(status, EVENT_OK, NULL);

    zassert_equal(event_queue_enqueue(&test_queue, &e1, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT), EVENT_OK, NULL);
    zassert_equal(event_queue_enqueue(&test_queue, &e2, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT), EVENT_OK, NULL);

    status = event_queue_enqueue(&test_queue, &hi, QUEUE_OVERFLOW_DROP_LOWEST, K_MSEC(20));
    zassert_equal(status, EVENT_OK, "DROP_LOWEST must not wait while holding reorder_lock");
    zassert_equal(event_queue_depth(&test_queue), 2, NULL);

    zassert_equal(event_queue_dequeue(&test_queue, &out, K_NO_WAIT), EVENT_OK, NULL);
    zassert_equal(out.type, 2, NULL);
    zassert_equal(event_queue_dequeue(&test_queue, &out, K_NO_WAIT), EVENT_OK, NULL);
    zassert_equal(out.type, 99, NULL);

    event_queue_deinit(&test_queue);
}

/**
 * @brief 测试 DROP_LOWEST：新事件比队列中最差事件还低时丢弃新事件
 */
ZTEST(test_event_queue, test_queue_overflow_drop_lowest_reject_worse) {
    struct k_msgq  test_queue;
    char           buffer[3 * sizeof(event_t)];
    event_status_t status;
    event_t        out;

    status = event_queue_init(&test_queue, buffer, 3);
    zassert_equal(status, EVENT_OK, "队列初始化失败");

    for (int i = 0; i < 3; i++) {
        event_t hi = {.type = (uint32_t) (10 + i), .priority = EVENT_PRIORITY_HIGH};

        status = event_queue_enqueue(&test_queue, &hi, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT);
        zassert_equal(status, EVENT_OK, "填满 HIGH");
    }

    event_t lo = {.type = 20, .priority = EVENT_PRIORITY_LOW};

    status = event_queue_enqueue(&test_queue, &lo, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_ERR_QUEUE_FULL, "新事件更差时应拒绝入队");

    zassert_equal(event_queue_depth(&test_queue), 3, "队列内容不变");

    for (int t = 10; t <= 12; t++) {
        status = event_queue_dequeue(&test_queue, &out, K_NO_WAIT);
        zassert_equal(status, EVENT_OK, "出队");
        zassert_equal((int) out.type, t, "仍为原 HIGH 事件");
    }

    event_queue_deinit(&test_queue);
}

/**
 * @brief 测试队列清空
 */
ZTEST(test_event_queue, test_queue_purge) {
    struct k_msgq  test_queue;
    char           buffer[5 * sizeof(event_t)];
    event_status_t status;
    event_t        event = {.type = 52, .priority = EVENT_PRIORITY_NORMAL};

    /* 初始化 */
    status = event_queue_init(&test_queue, buffer, 5);
    zassert_equal(status, EVENT_OK, "队列初始化失败");

    /* 添加一些事件 */
    for (int i = 0; i < 3; i++) {
        zassert_equal(event_queue_enqueue(&test_queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_OK,
                      "入队 %d 失败", i);
    }

    zassert_equal(event_queue_depth(&test_queue), 3, "队列深度应为 3");

    /* 清空队列 */
    event_queue_purge(&test_queue);

    zassert_equal(event_queue_depth(&test_queue), 0, "队列应为空");
    zassert_true(event_queue_is_empty(&test_queue), "队列应标记为空");

    event_queue_deinit(&test_queue);
}

/**
 * @brief 测试队列统计
 */
ZTEST(test_event_queue, test_queue_stats) {
    struct k_msgq  test_queue;
    char           buffer[5 * sizeof(event_t)];
    event_status_t status;
    queue_stats_t  stats;
    event_t        event = {.type = 53, .priority = EVENT_PRIORITY_NORMAL};

    /* 初始化 */
    status = event_queue_init(&test_queue, buffer, 5);
    zassert_equal(status, EVENT_OK, "队列初始化失败");

    /* 入队一些事件 */
    for (int i = 0; i < 4; i++) {
        zassert_equal(event_queue_enqueue(&test_queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_OK,
                      "入队 %d 失败", i);
    }

    /* 出队一些事件 */
    event_t out;
    for (int i = 0; i < 2; i++) {
        zassert_equal(event_queue_dequeue(&test_queue, &out, K_NO_WAIT), EVENT_OK, "出队 %d 失败", i);
    }

    /* 获取统计 */
    event_queue_get_stats(&test_queue, &stats);

    zassert_true(stats.enqueue_count >= 4, "入队计数应至少为 4");
    zassert_true(stats.dequeue_count >= 2, "出队计数应至少为 2");

    /* 重置统计 */
    event_queue_reset_stats(&test_queue);
    event_queue_get_stats(&test_queue, &stats);
    zassert_equal(stats.enqueue_count, 0, "重置后入队计数应为 0");

    event_queue_deinit(&test_queue);
}

/**
 * @brief 测试队列容量获取
 */
ZTEST(test_event_queue, test_queue_capacity) {
    struct k_msgq  test_queue;
    char           buffer[4 * sizeof(event_t)];
    event_status_t status;
    uint32_t       capacity;

    status = event_queue_init(&test_queue, buffer, 4);
    zassert_equal(status, EVENT_OK, "队列初始化失败");

    /* 获取容量 */
    capacity = event_queue_capacity(&test_queue);
    zassert_equal(capacity, 4, "容量应为 4");

    /* NULL 参数应返回 0 */
    capacity = event_queue_capacity(NULL);
    zassert_equal(capacity, 0, "NULL 队列容量应为 0");

    event_queue_deinit(&test_queue);
}

/**
 * @brief 测试高水位线
 */
ZTEST(test_event_queue, test_queue_high_watermark) {
    struct k_msgq  test_queue;
    char           buffer[5 * sizeof(event_t)];
    event_status_t status;
    queue_stats_t  stats;
    event_t        event = {.type = 60, .priority = EVENT_PRIORITY_NORMAL};

    status = event_queue_init(&test_queue, buffer, 5);
    zassert_equal(status, EVENT_OK, "队列初始化失败");

    /* 填满队列 */
    for (int i = 0; i < 5; i++) {
        event_queue_enqueue(&test_queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT);
    }

    /* 检查高水位线 */
    event_queue_get_stats(&test_queue, &stats);
    zassert_equal(stats.high_watermark, 5, "高水位线应为 5");

    /* 出队一些事件 */
    event_t out;
    for (int i = 0; i < 3; i++) {
        event_queue_dequeue(&test_queue, &out, K_NO_WAIT);
    }

    /* 高水位线应保持不变 */
    event_queue_get_stats(&test_queue, &stats);
    zassert_equal(stats.high_watermark, 5, "高水位线应保持为 5");

    event_queue_deinit(&test_queue);
}

/**
 * @brief 测试队列溢出计数
 */
ZTEST(test_event_queue, test_queue_overflow_count) {
    struct k_msgq  test_queue;
    char           buffer[2 * sizeof(event_t)];
    event_status_t status;
    queue_stats_t  stats;
    event_t        event = {.type = 61, .priority = EVENT_PRIORITY_NORMAL};

    status = event_queue_init(&test_queue, buffer, 2);
    zassert_equal(status, EVENT_OK, "队列初始化失败");

    /* 填满队列 */
    event_queue_enqueue(&test_queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT);
    event_queue_enqueue(&test_queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT);

    /* 尝试再入队（应溢出）*/
    event_queue_enqueue(&test_queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT);

    /* 检查溢出计数 */
    event_queue_get_stats(&test_queue, &stats);
    zassert_true(stats.overflow_count >= 1, "溢出计数应至少为 1");

    event_queue_deinit(&test_queue);
}

/**
 * @brief 测试 NULL 参数处理
 */
ZTEST(test_event_queue, test_null_parameters) {
    struct k_msgq  test_queue;
    char           buffer[3 * sizeof(event_t)];
    event_status_t status;
    event_t        event = {.type = 70, .priority = EVENT_PRIORITY_NORMAL};
    event_t        out;
    queue_stats_t  stats;

    status = event_queue_init(&test_queue, buffer, 3);
    zassert_equal(status, EVENT_OK, "队列初始化失败");

    /* NULL 队列入队 */
    zassert_equal(event_queue_enqueue(NULL, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_ERR_INVALID_ARG,
                  NULL);

    /* NULL 事件入队 */
    zassert_equal(event_queue_enqueue(&test_queue, NULL, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_ERR_INVALID_ARG,
                  NULL);

    /* NULL 队列出队 */
    zassert_equal(event_queue_dequeue(NULL, &out, K_NO_WAIT), EVENT_ERR_INVALID_ARG, NULL);

    /* NULL 事件出队 */
    zassert_equal(event_queue_dequeue(&test_queue, NULL, K_NO_WAIT), EVENT_ERR_INVALID_ARG, NULL);

    /* NULL 队列统计 */
    event_queue_get_stats(NULL, &stats);
    /* 不应崩溃 */

    /* NULL 统计指针 */
    event_queue_get_stats(&test_queue, NULL);
    /* 不应崩溃 */

    /* NULL 队列重置统计 */
    event_queue_reset_stats(NULL);
    /* 不应崩溃 */

    /* NULL 队列深度 */
    zassert_equal(event_queue_depth(NULL), 0, "NULL 队列深度应为 0");

    /* NULL 队列是否为空 */
    zassert_true(event_queue_is_empty(NULL), "NULL 队列应为空");

    /* NULL 队列是否已满 */
    zassert_false(event_queue_is_full(NULL), "NULL 队列不应满");

    event_queue_deinit(&test_queue);
}

/**
 * @brief 测试无效事件类型
 */
ZTEST(test_event_queue, test_invalid_event_type) {
    struct k_msgq  test_queue;
    char           buffer[3 * sizeof(event_t)];
    event_status_t status;
    /* 事件类型 255 是有效的（最大 uint8_t），但可以测试边界 */
    event_t event = {.type = 255, .priority = EVENT_PRIORITY_NORMAL};

    status = event_queue_init(&test_queue, buffer, 3);
    zassert_equal(status, EVENT_OK, "队列初始化失败");

    /* 正常边界值应成功 */
    status = event_queue_enqueue(&test_queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, "类型 255 应成功入队");

    event_queue_deinit(&test_queue);
}

/**
 * @brief 测试未初始化队列
 */
ZTEST(test_event_queue, test_uninitialized_queue) {
    struct k_msgq  test_queue;
    event_t        event = {.type = 80, .priority = EVENT_PRIORITY_NORMAL};
    event_status_t status;

    /* 不初始化队列 */
    memset(&test_queue, 0, sizeof(test_queue));

    /* 入队应失败 */
    status = event_queue_enqueue(&test_queue, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_ERR_INVALID_ARG, "未初始化队列应返回错误");
}

/**
 * @brief 测试不同优先级事件的 DROP_LOWEST 行为
 */
ZTEST(test_event_queue, test_drop_lowest_priority_ordering) {
    struct k_msgq  test_queue;
    char           buffer[4 * sizeof(event_t)];
    event_status_t status;
    event_t        out;

    status = event_queue_init(&test_queue, buffer, 4);
    zassert_equal(status, EVENT_OK, "队列初始化失败");

    /* 按优先级从低到高入队 */
    event_t e1 = {.type = 1, .priority = EVENT_PRIORITY_LOW};      /* priority = 10 */
    event_t e2 = {.type = 2, .priority = EVENT_PRIORITY_NORMAL};   /* priority = 5 */
    event_t e3 = {.type = 3, .priority = EVENT_PRIORITY_HIGH};     /* priority = 2 */
    event_t e4 = {.type = 4, .priority = EVENT_PRIORITY_CRITICAL}; /* priority = 0 */

    event_queue_enqueue(&test_queue, &e1, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT);
    event_queue_enqueue(&test_queue, &e2, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT);
    event_queue_enqueue(&test_queue, &e3, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT);
    event_queue_enqueue(&test_queue, &e4, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT);

    /* 队列已满，再入队一个 CRITICAL 事件，应丢弃 LOW 事件 */
    event_t e5 = {.type = 5, .priority = EVENT_PRIORITY_CRITICAL};
    status = event_queue_enqueue(&test_queue, &e5, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, "CRITICAL 事件应挤掉 LOW 事件");

    /* 验证 LOW 事件被丢弃 */
    status = event_queue_dequeue(&test_queue, &out, K_NO_WAIT);
    zassert_equal(status, EVENT_OK, NULL);
    zassert_equal(out.priority, EVENT_PRIORITY_NORMAL, "第一个应为 NORMAL");

    event_queue_deinit(&test_queue);
}

/**
 * @brief 测试多队列独立统计
 */
ZTEST(test_event_queue, test_multiple_queues) {
    struct k_msgq  queue1, queue2;
    char           buffer1[2 * sizeof(event_t)];
    char           buffer2[3 * sizeof(event_t)];
    event_status_t status;
    queue_stats_t  stats1, stats2;
    event_t        event = {.type = 90, .priority = EVENT_PRIORITY_NORMAL};

    status = event_queue_init(&queue1, buffer1, 2);
    zassert_equal(status, EVENT_OK, "队列 1 初始化失败");

    status = event_queue_init(&queue2, buffer2, 3);
    zassert_equal(status, EVENT_OK, "队列 2 初始化失败");

    /* 向队列 1 入队 2 个事件 */
    zassert_equal(event_queue_enqueue(&queue1, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_OK, NULL);
    zassert_equal(event_queue_enqueue(&queue1, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_OK, NULL);

    /* 向队列 2 入队 3 个事件 */
    for (int i = 0; i < 3; i++) {
        zassert_equal(event_queue_enqueue(&queue2, &event, QUEUE_OVERFLOW_DROP_NEWEST, K_NO_WAIT), EVENT_OK,
                      "队列 2 入队 %d 失败", i);
    }

    /* 验证统计独立 */
    event_queue_get_stats(&queue1, &stats1);
    event_queue_get_stats(&queue2, &stats2);

    zassert_equal(stats1.enqueue_count, 2, "队列 1 入队计数应为 2");
    zassert_equal(stats2.enqueue_count, 3, "队列 2 入队计数应为 3");

    event_queue_deinit(&queue1);
    event_queue_deinit(&queue2);
}

ZTEST(test_event_queue, test_dequeue_unregistered_queue_does_not_consume) {
    struct k_msgq  raw_queue;
    char           buffer[sizeof(event_t)];
    event_t        event_in = {.type = 91, .priority = EVENT_PRIORITY_NORMAL};
    event_t        event_out = {0};
    event_status_t status;

    k_msgq_init(&raw_queue, buffer, sizeof(event_t), 1);
    zassert_equal(k_msgq_put(&raw_queue, &event_in, K_NO_WAIT), 0, NULL);

    status = event_queue_dequeue(&raw_queue, &event_out, K_NO_WAIT);
    zassert_equal(status, EVENT_ERR_INVALID_ARG, "unregistered queue must be rejected");
    zassert_equal(k_msgq_num_used_get(&raw_queue), 1, "rejected dequeue must not consume data");

    zassert_equal(k_msgq_get(&raw_queue, &event_out, K_NO_WAIT), 0, NULL);
    zassert_equal(event_out.type, event_in.type, NULL);
}

ZTEST(test_event_queue, test_drop_lowest_dequeue_waits_for_msgq_notification) {
    struct k_msgq test_queue;
    char          buffer[2 * sizeof(event_t)];
    event_t       seed = {.type = 92, .priority = EVENT_PRIORITY_LOW};
    event_t       delayed = {.type = 93, .priority = EVENT_PRIORITY_HIGH};
    event_t       out = {0};

    zassert_equal(event_queue_init(&test_queue, buffer, 2), EVENT_OK, NULL);

    /* First DROP_LOWEST use allocates scratch and enables reorder_lock serialization. */
    zassert_equal(event_queue_enqueue(&test_queue, &seed, QUEUE_OVERFLOW_DROP_LOWEST, K_NO_WAIT), EVENT_OK, NULL);
    zassert_equal(event_queue_dequeue(&test_queue, &out, K_NO_WAIT), EVENT_OK, NULL);

    k_tid_t tid = k_thread_create(&g_delayed_enqueue_thread, g_delayed_enqueue_stack,
                                  K_THREAD_STACK_SIZEOF(g_delayed_enqueue_stack), delayed_enqueue_thread, &test_queue,
                                  &delayed, NULL, 7, 0, K_MSEC(20));
    zassert_not_null(tid, NULL);

    zassert_equal(event_queue_dequeue(&test_queue, &out, K_MSEC(500)), EVENT_OK, NULL);
    zassert_equal(out.type, delayed.type, NULL);
    zassert_equal(k_thread_join(tid, K_MSEC(500)), 0, NULL);

    event_queue_deinit(&test_queue);
}

/* =============================================================================
 * 测试套件
 * ============================================================================= */

ZTEST_SUITE(test_event_queue, NULL, NULL, NULL, NULL, NULL);
