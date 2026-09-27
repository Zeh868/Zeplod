/**
 * @file test_data_bus.c
 * @brief Data Bus 单元测试
 *
 * 覆盖：初始化/反初始化、通道管理、自动释放、多消费者、
 * retain 异步持有、队列溢出、消费者注销、publish_block、
 * manual_release、ISR 发布、统计重置。
 * @author zeh (china_qzh@163.com)
 * @version 2.5
 * @date 2026-05-15
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-05-15       2.0            zeh            重构：删除 COPY 模式测试，添加 retain 测试
 * 2026-05-15       2.1            zeh            无消费者时队列排空回归测试
 * 2026-05-20       2.2            zeh            消费者固定槽位：交错注销不使 out_consumer 指针错位
 * 2026-05-28       2.3            zeh            deinit 分发线程拒绝路径（P2 契约）
 * 2026-09-18       2.4            zeh            超时/容量魔数具名；裸调用返回值检查
 * 2026-09-27       2.5            zeh            消费者用例补注销修复 destroy -EBUSY；新增 destroy 忙判定用例
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>
#include <string.h>
#include <zeplod/data_bus.h>
#include "data_bus_channel.h"
#include "data_bus_internal.h"
#include "data_bus_memory.h"
#include "ztest_sync.h"

LOG_MODULE_REGISTER(test_data_bus);

BUILD_ASSERT(sizeof(((data_bus_event_notification_t*) 0)->channel_name) == CONFIG_DATA_BUS_CHANNEL_NAME_MAX);
BUILD_ASSERT(sizeof(((data_bus_memory_warning_event_t*) 0)->channel_name) == CONFIG_DATA_BUS_CHANNEL_NAME_MAX);

/* ============================================================================
 * 超时/容量参数（集中定义，避免散落魔数）
 * ============================================================================ */

/** 强制复位循环：至多重试次数（每次间隔 RESET_RETRY_INTERVAL_MS，总窗约 1.5s） */
#define RESET_MAX_ATTEMPTS 30
/** 强制复位循环：单次重试间隔（等待在途回调退出） */
#define RESET_RETRY_INTERVAL_MS 50
/** 通道静默（分发空闲）判定超时 */
#define CHANNEL_QUIESCENT_TIMEOUT_MS 2000U
/** OVERWRITE 连发用例：连发次数（数倍于队列深度，充分触发覆盖旧块路径） */
#define OVERWRITE_BURST_PUBLISH_COUNT 100
/** IMU 基准用例：OVERWRITE 高频发布轮数 */
#define IMU_BENCH_ROUND_COUNT 200

/* ============================================================================
 * 测试夹具
 * ============================================================================ */

static struct k_sem      g_test_sem;
static atomic_t          g_call_count;
static uint32_t          g_recv_seq = 0;
static data_bus_block_t* g_retained_block = NULL;
#if !IS_ENABLED(CONFIG_DATA_BUS_NO_MALLOC)
static uint8_t g_large_payload[4097];
#endif
static struct k_thread g_unregister_thread;
static struct k_thread g_deinit_thread;
static K_THREAD_STACK_DEFINE(g_unregister_stack, 1024);
static K_THREAD_STACK_DEFINE(g_deinit_stack, 1024);
static struct k_sem         g_concurrent_cb_entered;
static struct k_sem         g_concurrent_cb_continue;
static atomic_t             g_concurrent_publish_result;
static atomic_t             g_concurrent_unregister_result;
static atomic_t             g_concurrent_deinit_result;
static data_bus_consumer_t* g_concurrent_consumer;
static data_bus_consumer_t* g_self_unregister_consumer;
static atomic_t             g_self_unregister_result;

/* ============================================================================
 * 消费者回调
 * ============================================================================ */

static void auto_consumer_cb(data_bus_channel_t* ch, data_bus_block_t* block, void* user_data) {
    ARG_UNUSED(ch);
    ARG_UNUSED(user_data);

    atomic_inc(&g_call_count);
    g_recv_seq = block->seq;
    /* 框架自动释放；不要在这里调用 release */
    k_sem_give(&g_test_sem);
}

static void retain_consumer_cb(data_bus_channel_t* ch, data_bus_block_t* block, void* user_data) {
    ARG_UNUSED(ch);
    ARG_UNUSED(user_data);

    atomic_inc(&g_call_count);
    g_recv_seq = block->seq;

    /* 保留供异步处理 */
    g_retained_block = data_bus_block_retain(block);
    k_sem_give(&g_test_sem);
}

static atomic_t g_manual_release_count;

static bool data_bus_channel_quiescent(void* ctx) {
    data_bus_channel_t* ch = ctx;
    bool                queue_empty;

    k_spinlock_key_t key = k_spin_lock(&ch->lock);

    queue_empty = (ch->queue_used == 0U);
    k_spin_unlock(&ch->lock, key);

    return atomic_get(&ch->publish_hold) == 0 && atomic_get(&ch->dispatch_hold) == 0 &&
           atomic_get(&ch->lifecycle_hold) == 0 && queue_empty;
}

static void data_bus_test_wait_channel_quiescent(data_bus_channel_t* ch) {
    zassert_true(ztest_wait_until(data_bus_channel_quiescent, ch, CHANNEL_QUIESCENT_TIMEOUT_MS),
                 "channel dispatch should become idle");
}

static void data_bus_test_destroy_channel(data_bus_channel_t* ch) {
    data_bus_test_wait_channel_quiescent(ch);
    zassert_equal(data_bus_channel_destroy(ch), 0, NULL);
}

static void manual_release_consumer_cb(data_bus_channel_t* ch, data_bus_block_t* block, void* user_data) {
    ARG_UNUSED(ch);
    ARG_UNUSED(user_data);

    atomic_inc(&g_manual_release_count);
    g_recv_seq = block->seq;

    /* manual_release=true：消费者自行管理引用 */
    data_bus_block_release(block);
    k_sem_give(&g_test_sem);
}

static void concurrent_publish_consumer_cb(data_bus_channel_t* ch, data_bus_block_t* block, void* user_data) {
    ARG_UNUSED(block);
    ARG_UNUSED(user_data);

    k_sem_give(&g_concurrent_cb_entered);
    k_sem_take(&g_concurrent_cb_continue, K_FOREVER);

    const uint8_t payload[] = {0xC1};
    atomic_set(&g_concurrent_publish_result, data_bus_publish(ch, payload, sizeof(payload)));
}

static void self_unregister_consumer_cb(data_bus_channel_t* ch, data_bus_block_t* block, void* user_data) {
    ARG_UNUSED(ch);
    ARG_UNUSED(block);
    ARG_UNUSED(user_data);

    atomic_set(&g_self_unregister_result, data_bus_consumer_unregister(g_self_unregister_consumer));
    k_sem_give(&g_test_sem);
}

static void unregister_thread_entry(void* arg1, void* arg2, void* arg3) {
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    atomic_set(&g_concurrent_unregister_result, data_bus_consumer_unregister((data_bus_consumer_t*) arg1));
}

static void deinit_thread_entry(void* arg1, void* arg2, void* arg3) {
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    atomic_set(&g_concurrent_deinit_result, data_bus_deinit());
}

static bool data_bus_lifecycle_hold_nonzero(void* ctx) {
    data_bus_channel_t* ch = ctx;

    return atomic_get(&ch->lifecycle_hold) != 0;
}

/* ============================================================================
 * ISR 发布测试（定时器回调在 ISR 上下文）
 * ============================================================================ */

static struct k_timer      g_isr_timer;
static data_bus_channel_t* g_isr_ch = NULL;

static void isr_timer_handler(struct k_timer* timer) {
    ARG_UNUSED(timer);

    const uint8_t data[] = {0x99, 0x88, 0x77};
    (void) data_bus_publish(g_isr_ch, data, sizeof(data));
}

/* ============================================================================
 * 设置 / 清理
 * ============================================================================ */

static void data_bus_test_force_reset(void) {
    for (int attempt = 0; attempt < RESET_MAX_ATTEMPTS; attempt++) {
        int ret = data_bus_deinit();

        if (ret == 0) {
            return;
        }
        if (ret == -ENODEV) {
            return;
        }
        k_sleep(K_MSEC(RESET_RETRY_INTERVAL_MS));
    }
    zassert_unreachable("data_bus_deinit 无法在超时内完成清理");
}

static void data_bus_test_setup(void) {
    data_bus_test_force_reset();

    k_sem_init(&g_test_sem, 0, K_SEM_MAX_LIMIT);
    atomic_set(&g_call_count, 0);
    atomic_set(&g_manual_release_count, 0);
    g_recv_seq = 0;
    g_retained_block = NULL;
    g_isr_ch = NULL;
    g_concurrent_consumer = NULL;
    g_self_unregister_consumer = NULL;
    k_sem_init(&g_concurrent_cb_entered, 0, 1);
    k_sem_init(&g_concurrent_cb_continue, 0, 1);
    atomic_set(&g_concurrent_publish_result, -EINPROGRESS);
    atomic_set(&g_concurrent_unregister_result, -EINPROGRESS);
    atomic_set(&g_concurrent_deinit_result, -EINPROGRESS);
    atomic_set(&g_self_unregister_result, -EINPROGRESS);
}

/* ============================================================================
 * 测试用例
 * ============================================================================ */

/**
 * @brief 测试 Data Bus 初始化与反初始化
 */
ZTEST(test_data_bus, test_init_deinit) {
    data_bus_test_setup();

    int ret = data_bus_init();
    zassert_equal(ret, 0, "data_bus_init 失败");

    /* 重复初始化应成功（幂等） */
    ret = data_bus_init();
    zassert_equal(ret, 0, "重复初始化应成功");

    ret = data_bus_deinit();
    zassert_equal(ret, 0, "data_bus_deinit 失败");
}

/**
 * @brief 重复 deinit 幂等（P2 生命周期契约）
 */
ZTEST(test_data_bus, test_repeat_deinit_idempotent) {
    data_bus_test_setup();

    zassert_equal(data_bus_init(), 0, NULL);
    zassert_equal(data_bus_deinit(), 0, NULL);
    zassert_equal(data_bus_deinit(), 0, "重复 deinit 应返回 0");
}

static atomic_t     g_deinit_from_dispatcher_result;
static struct k_sem g_deinit_from_dispatcher_sem;
static bool         g_deinit_from_dispatcher_tried;

static void deinit_from_dispatcher_consumer_cb(data_bus_channel_t* ch, data_bus_block_t* block, void* user_data) {
    ARG_UNUSED(ch);
    ARG_UNUSED(block);
    ARG_UNUSED(user_data);

    if (!g_deinit_from_dispatcher_tried) {
        g_deinit_from_dispatcher_tried = true;
        atomic_set(&g_deinit_from_dispatcher_result, data_bus_deinit());
    }
    k_sem_give(&g_deinit_from_dispatcher_sem);
}

/**
 * @brief 分发线程内调用 deinit 应被拒绝（P2 生命周期契约）
 */
ZTEST(test_data_bus, test_deinit_rejected_from_dispatcher_thread) {
    data_bus_channel_t*     ch = NULL;
    data_bus_consumer_t*    cons = NULL;
    data_bus_consumer_cfg_t cfg = {
        .name = "deinit_cb",
        .manual_release = false,
        .callback = deinit_from_dispatcher_consumer_cb,
        .user_data = NULL,
    };

    data_bus_test_setup();
    k_sem_init(&g_deinit_from_dispatcher_sem, 0, 1);
    g_deinit_from_dispatcher_tried = false;
    atomic_set(&g_deinit_from_dispatcher_result, 0);

    zassert_equal(data_bus_init(), 0, NULL);
    zassert_equal(data_bus_channel_create("deinit_cb_ch", &ch), 0, NULL);
    zassert_equal(data_bus_consumer_register(ch, &cfg, &cons), 0, NULL);

    const uint8_t payload[] = {0x01};
    zassert_equal(data_bus_publish(ch, payload, sizeof(payload)), 0, NULL);

    zassert_true(ztest_wait_sem(&g_deinit_from_dispatcher_sem, K_MSEC(2000U)), "consumer should run on dispatcher");
    zassert_equal(atomic_get(&g_deinit_from_dispatcher_result), -EINVAL, "dispatcher 线程 deinit 应返回 -EINVAL");

    zassert_equal(data_bus_consumer_unregister(cons), 0, NULL);
    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

ZTEST(test_data_bus, test_deinit_drain_timeout_keeps_bus_closed) {
    data_bus_channel_t* ch = NULL;

    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);
    zassert_equal(data_bus_channel_create("deinit_timeout", &ch), 0, NULL);

    (void) atomic_inc(&ch->publish_hold);
    zassert_equal(data_bus_deinit(), -EBUSY, "active channel operation should time out deinit");
    zassert_equal(data_bus_init(), -ESHUTDOWN, "init must not reopen a partially deinitialized bus");

    const uint8_t payload[] = {0xD1};
    zassert_equal(data_bus_publish(ch, payload, sizeof(payload)), -ESHUTDOWN,
                  "publish must remain rejected after deinit timeout");

    (void) atomic_dec(&ch->publish_hold);
    zassert_equal(data_bus_deinit(), 0, "deinit retry should finish after the operation releases its hold");
}

/**
 * @brief 测试通道创建、查找与销毁
 */
ZTEST(test_data_bus, test_channel_create_destroy) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t* ch = NULL;

    /* 创建通道 */
    int ret = data_bus_channel_create("test_ch", &ch);
    zassert_equal(ret, 0, "通道创建失败");
    zassert_not_null(ch, "通道不应为 NULL");

    /* 查找通道 */
    data_bus_channel_t* found = data_bus_channel_find("test_ch");
    zassert_equal(found, ch, "查找应返回同一通道");

    /* 尝试重复名称 */
    data_bus_channel_t* ch2 = NULL;
    ret = data_bus_channel_create("test_ch", &ch2);
    zassert_equal(ret, -EEXIST, "重复名称应返回 -EEXIST");

    /* 销毁 */
    ret = data_bus_channel_destroy(ch);
    zassert_equal(ret, 0, "通道销毁失败");

    /* 销毁后查找 */
    found = data_bus_channel_find("test_ch");
    zassert_is_null(found, "销毁后查找应返回 NULL");

    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 销毁后的失效通道指针不得被统计接口解引用
 */
ZTEST(test_data_bus, test_stats_reject_destroyed_channel) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t* ch = NULL;
    zassert_equal(data_bus_channel_create("stale_stats", &ch), 0, NULL);
    zassert_equal(data_bus_channel_destroy(ch), 0, NULL);

    data_bus_stats_t stats;
    memset(&stats, 0xA5, sizeof(stats));
    data_bus_channel_get_stats(ch, &stats);

    data_bus_stats_t zero_stats = {0};
    zassert_mem_equal(&stats, &zero_stats, sizeof(stats), "失效通道统计应返回全 0");

    data_bus_reset_stats(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 测试自动释放的发布与消费
 */
ZTEST(test_data_bus, test_publish_auto_release) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t*  ch = NULL;
    data_bus_consumer_t* cons = NULL;
    int                  ret = data_bus_channel_create("auto_ch", &ch);
    zassert_equal(ret, 0, "通道创建失败");

    /* 注册 manual_release=false（启用自动释放）的消费者 */
    data_bus_consumer_cfg_t cfg = {
        .name = "auto_consumer",
        .manual_release = false,
        .callback = auto_consumer_cb,
        .user_data = NULL,
    };
    ret = data_bus_consumer_register(ch, &cfg, &cons);
    zassert_equal(ret, 0, "消费者注册失败");

    /* 发布数据 */
    const uint8_t test_data[] = {0x01, 0x02, 0x03, 0x04};
    ret = data_bus_publish(ch, test_data, sizeof(test_data));
    zassert_equal(ret, 0, "发布失败");

    /* 等待消费者回调 */
    ret = k_sem_take(&g_test_sem, K_MSEC(100));
    zassert_equal(ret, 0, "消费者回调超时");
    zassert_equal(atomic_get(&g_call_count), 1, "消费者应被调用一次");
    zassert_equal(g_recv_seq, 0, "第一个 seq 应为 0");

    /* destroy 要求先注销全部消费者 */
    zassert_equal(data_bus_consumer_unregister(cons), 0, NULL);
    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 测试多消费者自动释放
 */
ZTEST(test_data_bus, test_multi_consumer) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t*  ch = NULL;
    data_bus_consumer_t* cons_a = NULL;
    data_bus_consumer_t* cons_b = NULL;
    int                  ret = data_bus_channel_create("multi_ch", &ch);
    zassert_equal(ret, 0, "通道创建失败");

    /* 注册两个自动释放消费者 */
    data_bus_consumer_cfg_t cfg1 = {
        .name = "consumer_a",
        .manual_release = false,
        .callback = auto_consumer_cb,
        .user_data = NULL,
    };
    ret = data_bus_consumer_register(ch, &cfg1, &cons_a);
    zassert_equal(ret, 0, "消费者 A 注册失败");

    data_bus_consumer_cfg_t cfg2 = {
        .name = "consumer_b",
        .manual_release = false,
        .callback = auto_consumer_cb,
        .user_data = NULL,
    };
    ret = data_bus_consumer_register(ch, &cfg2, &cons_b);
    zassert_equal(ret, 0, "消费者 B 注册失败");

    /* 发布数据 */
    const uint8_t test_data[] = {0xAB, 0xCD};
    ret = data_bus_publish(ch, test_data, sizeof(test_data));
    zassert_equal(ret, 0, "发布失败");

    /* 等待两个消费者（2 次回调） */
    ret = k_sem_take(&g_test_sem, K_MSEC(100));
    zassert_equal(ret, 0, "第一个消费者超时");
    ret = k_sem_take(&g_test_sem, K_MSEC(100));
    zassert_equal(ret, 0, "第二个消费者超时");

    zassert_equal(atomic_get(&g_call_count), 2, "两个消费者都应被调用");

    /* destroy 要求先注销全部消费者 */
    zassert_equal(data_bus_consumer_unregister(cons_a), 0, NULL);
    zassert_equal(data_bus_consumer_unregister(cons_b), 0, NULL);
    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 测试 data_bus_block_retain 异步持有
 */
ZTEST(test_data_bus, test_retain) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t*  ch = NULL;
    data_bus_consumer_t* cons = NULL;
    int                  ret = data_bus_channel_create("retain_ch", &ch);
    zassert_equal(ret, 0, "通道创建失败");

    /* 注册 retain 消费者的块 */
    data_bus_consumer_cfg_t cfg = {
        .name = "retain_consumer",
        .manual_release = false,
        .callback = retain_consumer_cb,
        .user_data = NULL,
    };
    ret = data_bus_consumer_register(ch, &cfg, &cons);
    zassert_equal(ret, 0, "消费者注册失败");

    /* 发布数据 */
    const char* msg = "retain me";
    size_t      msg_len = strlen(msg) + 1;
    ret = data_bus_publish(ch, msg, msg_len);
    zassert_equal(ret, 0, "发布失败");

    /* 等待回调 */
    ret = k_sem_take(&g_test_sem, K_MSEC(100));
    zassert_equal(ret, 0, "消费者回调超时");
    zassert_equal(atomic_get(&g_call_count), 1, "消费者应被调用一次");

    /* 被 retain 的块应该仍然有效 */
    zassert_not_null(g_retained_block, "retain 的块不应为 NULL");
    zassert_equal(g_retained_block->len, msg_len, "retain 长度不匹配");
    zassert_mem_equal(g_retained_block->ptr, msg, msg_len, "retain 数据不匹配");

    /* 释放被 retain 的块 */
    data_bus_block_release(g_retained_block);
    g_retained_block = NULL;

    /* destroy 要求先注销全部消费者 */
    zassert_equal(data_bus_consumer_unregister(cons), 0, NULL);
    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 测试 manual_release=true 的消费者自行管理引用
 */
ZTEST(test_data_bus, test_manual_release) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t*  ch = NULL;
    data_bus_consumer_t* cons = NULL;
    int                  ret = data_bus_channel_create("manual_ch", &ch);
    zassert_equal(ret, 0, "通道创建失败");

    /* 注册 manual_release=true 的消费者 */
    data_bus_consumer_cfg_t cfg = {
        .name = "manual_consumer",
        .manual_release = true,
        .callback = manual_release_consumer_cb,
        .user_data = NULL,
    };
    ret = data_bus_consumer_register(ch, &cfg, &cons);
    zassert_equal(ret, 0, "消费者注册失败");

    /* 发布数据 */
    const uint8_t test_data[] = {0x01, 0x02};
    ret = data_bus_publish(ch, test_data, sizeof(test_data));
    zassert_equal(ret, 0, "发布失败");

    /* 等待消费者回调 */
    ret = k_sem_take(&g_test_sem, K_MSEC(100));
    zassert_equal(ret, 0, "消费者回调超时");
    zassert_equal(atomic_get(&g_manual_release_count), 1, "消费者应被调用一次");
    zassert_equal(g_recv_seq, 0, "seq 应为 0");

    /* destroy 要求先注销全部消费者 */
    zassert_equal(data_bus_consumer_unregister(cons), 0, NULL);
    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 测试队列溢出处理
 */
ZTEST(test_data_bus, test_queue_overflow) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t* ch = NULL;
    int                 ret = data_bus_channel_create("overflow_ch", &ch);
    zassert_equal(ret, 0, "通道创建失败");

    /* 填满队列（无消费者，块留在队列中） */
    const uint8_t test_data[] = {0x01};
    int           published = 0;

    for (int i = 0; i < CONFIG_DATA_BUS_CHANNEL_QUEUE_DEPTH + 5; i++) {
        ret = data_bus_publish(ch, test_data, sizeof(test_data));
        if (ret == 0) {
            published++;
        } else {
            zassert_equal(ret, -ENOBUFS, "满时应返回 -ENOBUFS");
        }
    }

    zassert_equal(published, CONFIG_DATA_BUS_CHANNEL_QUEUE_DEPTH, "应恰好发布 queue_depth 个条目");

    /* 检查统计 */
    data_bus_stats_t stats;
    data_bus_channel_get_stats(ch, &stats);
    zassert_true(stats.drop_count > 0, "drop_count 应 > 0");
    zassert_true(stats.queue_full_count > 0, "queue_full_count 应 > 0");

    /* 队列非空时 destroy 应返回 -EAGAIN */
    ret = data_bus_channel_destroy(ch);
    zassert_equal(ret, -EAGAIN, "队列非空时应返回 -EAGAIN");

    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 测试反初始化清理挂起块
 */
ZTEST(test_data_bus, test_deinit_cleanup) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t* ch = NULL;
    int                 ret = data_bus_channel_create("cleanup_ch", &ch);
    zassert_equal(ret, 0, "通道创建失败");

    /* 发布一些数据（无消费者） */
    const uint8_t test_data[] = {0x01, 0x02, 0x03};
    for (int i = 0; i < 3; i++) {
        ret = data_bus_publish(ch, test_data, sizeof(test_data));
        zassert_equal(ret, 0, "发布失败");
    }

    /* 反初始化应清理一切且不泄漏 */
    ret = data_bus_deinit();
    zassert_equal(ret, 0, "反初始化失败");

    /* 验证通道已不存在 */
    data_bus_channel_t* found = data_bus_channel_find("cleanup_ch");
    zassert_is_null(found, "反初始化后通道应不存在");

    /* 重新初始化应干净工作 */
    ret = data_bus_init();
    zassert_equal(ret, 0, "反初始化后重新初始化失败");

    /* 创建新通道 */
    data_bus_channel_t* ch2 = NULL;
    ret = data_bus_channel_create("new_ch", &ch2);
    zassert_equal(ret, 0, "重新初始化后通道创建失败");

    zassert_equal(data_bus_channel_destroy(ch2), 0, NULL);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 无消费者或 consumer_count==0 时分发线程仍须排空 ring 队列（防块泄漏）
 */
ZTEST(test_data_bus, test_dispatch_drains_without_consumer) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t* ch = NULL;
    zassert_equal(data_bus_channel_create("orphan_ch", &ch), 0, NULL);

    const uint8_t p1[] = {0x01};
    zassert_equal(data_bus_publish(ch, p1, sizeof(p1)), 0, NULL);
    data_bus_test_destroy_channel(ch);

    /* 最后一个消费者注销后再次 publish，队列须在 consumer_count==0 时被排空 */
    ch = NULL;
    zassert_equal(data_bus_channel_create("orphan2", &ch), 0, NULL);
    data_bus_consumer_cfg_t cfg = {
        .name = "tmp_cons",
        .manual_release = false,
        .callback = auto_consumer_cb,
        .user_data = NULL,
    };
    data_bus_consumer_t* cons = NULL;
    zassert_equal(data_bus_consumer_register(ch, &cfg, &cons), 0, NULL);
    const uint8_t p2[] = {0x02};
    zassert_equal(data_bus_publish(ch, p2, sizeof(p2)), 0, NULL);
    zassert_equal(k_sem_take(&g_test_sem, K_MSEC(200)), 0, NULL);
    zassert_equal(data_bus_consumer_unregister(cons), 0, NULL);
    const uint8_t p3[] = {0x03};
    zassert_equal(data_bus_publish(ch, p3, sizeof(p3)), 0, NULL);
    data_bus_test_destroy_channel(ch);

    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief publish 后 destroy 在 -EAGAIN 上重试直至空闲，不应因 ready 竞态挂死
 */
ZTEST(test_data_bus, test_channel_destroy_retries_after_publish) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t* ch = NULL;

    zassert_equal(data_bus_channel_create("destroy_retry", &ch), 0, NULL);

    const uint8_t payload[] = {0xAA, 0xBB};
    zassert_equal(data_bus_publish(ch, payload, sizeof(payload)), 0, NULL);

    uint32_t start_ms = k_uptime_get_32();
    int      ret = -EAGAIN;

    while ((k_uptime_get_32() - start_ms) < 2000U) {
        ret = data_bus_channel_destroy(ch);
        if (ret == 0) {
            break;
        }
        zassert_equal(ret, -EAGAIN, "destroy should retry while dispatch idle");
        k_sleep(K_MSEC(1));
    }
    zassert_equal(ret, 0, "destroy should succeed without hang");

    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 有活跃消费者时 destroy 应返回 -EBUSY，注销后 destroy 应成功（正向契约）
 */
ZTEST(test_data_bus, test_channel_destroy_rejects_active_consumer) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t*     ch = NULL;
    data_bus_consumer_t*    cons = NULL;
    data_bus_consumer_cfg_t cfg = {
        .name = "busy_consumer",
        .manual_release = false,
        .callback = auto_consumer_cb,
        .user_data = NULL,
    };

    zassert_equal(data_bus_channel_create("busy_ch", &ch), 0, NULL);
    zassert_equal(data_bus_consumer_register(ch, &cfg, &cons), 0, NULL);

    zassert_equal(data_bus_channel_destroy(ch), -EBUSY, "有活跃消费者时 destroy 应返回 -EBUSY");

    zassert_equal(data_bus_consumer_unregister(cons), 0, NULL);
    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 测试消费者注销
 */
ZTEST(test_data_bus, test_consumer_unregister) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t* ch = NULL;
    int                 ret = data_bus_channel_create("unregister_ch", &ch);
    zassert_equal(ret, 0, "通道创建失败");

    data_bus_consumer_t*    consumer = NULL;
    data_bus_consumer_cfg_t cfg = {
        .name = "temp_consumer",
        .manual_release = false,
        .callback = auto_consumer_cb,
        .user_data = NULL,
    };
    ret = data_bus_consumer_register(ch, &cfg, &consumer);
    zassert_equal(ret, 0, "消费者注册失败");
    zassert_not_null(consumer, "out_consumer 应被设置");

    /* 注销 */
    ret = data_bus_consumer_unregister(consumer);
    zassert_equal(ret, 0, "消费者注销失败");

    /* 注销后发布 — 不应触发回调 */
    const uint8_t test_data[] = {0x01};
    ret = data_bus_publish(ch, test_data, sizeof(test_data));
    zassert_equal(ret, 0, "发布失败");

    data_bus_test_wait_channel_quiescent(ch);
    zassert_equal(atomic_get(&g_call_count), 0, "注销后回调不应触发");

    zassert_equal(data_bus_channel_destroy(ch), 0, NULL);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

ZTEST(test_data_bus, test_unregister_allows_callback_publish_to_finish) {
    data_bus_channel_t*     ch = NULL;
    data_bus_consumer_cfg_t cfg = {
        .name = "pub_unreg",
        .manual_release = false,
        .callback = concurrent_publish_consumer_cb,
        .user_data = NULL,
    };

    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);
    zassert_equal(data_bus_channel_create("unreg_publish", &ch), 0, NULL);
    zassert_equal(data_bus_consumer_register(ch, &cfg, &g_concurrent_consumer), 0, NULL);

    const uint8_t payload[] = {0xA1};
    zassert_equal(data_bus_publish(ch, payload, sizeof(payload)), 0, NULL);
    zassert_true(ztest_wait_sem(&g_concurrent_cb_entered, K_MSEC(2000)), "callback should start");

    k_tid_t unregister_tid =
        k_thread_create(&g_unregister_thread, g_unregister_stack, K_THREAD_STACK_SIZEOF(g_unregister_stack),
                        unregister_thread_entry, g_concurrent_consumer, NULL, NULL, 5, 0, K_NO_WAIT);
    zassert_not_null(unregister_tid, NULL);
    zassert_true(ztest_wait_until(data_bus_lifecycle_hold_nonzero, ch, 2000U),
                 "unregister should hold the channel lifecycle");

    k_sem_give(&g_concurrent_cb_continue);
    zassert_equal(k_thread_join(unregister_tid, K_MSEC(2000)), 0, "unregister should not deadlock callback publish");
    zassert_equal(atomic_get(&g_concurrent_publish_result), 0, NULL);
    zassert_equal(atomic_get(&g_concurrent_unregister_result), 0, NULL);

    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

ZTEST(test_data_bus, test_unregister_lifecycle_hold_blocks_deinit_free) {
    data_bus_channel_t*     ch = NULL;
    data_bus_consumer_cfg_t cfg = {
        .name = "unreg_deinit",
        .manual_release = false,
        .callback = concurrent_publish_consumer_cb,
        .user_data = NULL,
    };

    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);
    zassert_equal(data_bus_channel_create("unreg_deinit", &ch), 0, NULL);
    zassert_equal(data_bus_consumer_register(ch, &cfg, &g_concurrent_consumer), 0, NULL);

    const uint8_t payload[] = {0xA2};
    zassert_equal(data_bus_publish(ch, payload, sizeof(payload)), 0, NULL);
    zassert_true(ztest_wait_sem(&g_concurrent_cb_entered, K_MSEC(2000)), "callback should start");

    k_tid_t unregister_tid =
        k_thread_create(&g_unregister_thread, g_unregister_stack, K_THREAD_STACK_SIZEOF(g_unregister_stack),
                        unregister_thread_entry, g_concurrent_consumer, NULL, NULL, 5, 0, K_NO_WAIT);
    zassert_not_null(unregister_tid, NULL);
    zassert_true(ztest_wait_until(data_bus_lifecycle_hold_nonzero, ch, 2000U),
                 "unregister should hold the channel lifecycle");

    k_tid_t deinit_tid = k_thread_create(&g_deinit_thread, g_deinit_stack, K_THREAD_STACK_SIZEOF(g_deinit_stack),
                                         deinit_thread_entry, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    zassert_not_null(deinit_tid, NULL);
    zassert_true(ztest_wait_atomic_nonzero(&g_shutting_down, 2000U), "deinit should close the bus");

    k_sem_give(&g_concurrent_cb_continue);
    zassert_equal(k_thread_join(unregister_tid, K_MSEC(2000)), 0, NULL);
    zassert_equal(k_thread_join(deinit_tid, K_MSEC(2000)), 0, NULL);
    zassert_equal(atomic_get(&g_concurrent_unregister_result), 0, NULL);
    zassert_equal(atomic_get(&g_concurrent_deinit_result), 0, NULL);
    zassert_equal(atomic_get(&g_concurrent_publish_result), -ESHUTDOWN, NULL);
}

ZTEST(test_data_bus, test_unregister_rejected_from_dispatcher_callback) {
    data_bus_channel_t*     ch = NULL;
    data_bus_consumer_cfg_t cfg = {
        .name = "self_unregister",
        .manual_release = false,
        .callback = self_unregister_consumer_cb,
        .user_data = NULL,
    };

    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);
    zassert_equal(data_bus_channel_create("self_unregister", &ch), 0, NULL);
    zassert_equal(data_bus_consumer_register(ch, &cfg, &g_self_unregister_consumer), 0, NULL);

    const uint8_t payload[] = {0xA3};
    zassert_equal(data_bus_publish(ch, payload, sizeof(payload)), 0, NULL);
    zassert_true(ztest_wait_sem(&g_test_sem, K_MSEC(2000)), "callback should attempt unregister");
    zassert_equal(atomic_get(&g_self_unregister_result), -EDEADLK, NULL);

    zassert_equal(data_bus_consumer_unregister(g_self_unregister_consumer), 0, NULL);
    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 先注销槽位 0 时，槽位 1 的 out_consumer 指针仍指向原对象
 */
ZTEST(test_data_bus, test_consumer_slot_stable) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t* ch = NULL;
    int                 ret = data_bus_channel_create("slot_stable_ch", &ch);
    zassert_equal(ret, 0, NULL);

    data_bus_consumer_t*    cons_a = NULL;
    data_bus_consumer_t*    cons_b = NULL;
    data_bus_consumer_cfg_t cfg_a = {
        .name = "consumer_a",
        .manual_release = false,
        .callback = auto_consumer_cb,
        .user_data = NULL,
    };
    data_bus_consumer_cfg_t cfg_b = {
        .name = "consumer_b",
        .manual_release = false,
        .callback = auto_consumer_cb,
        .user_data = NULL,
    };

    ret = data_bus_consumer_register(ch, &cfg_a, &cons_a);
    zassert_equal(ret, 0, NULL);
    ret = data_bus_consumer_register(ch, &cfg_b, &cons_b);
    zassert_equal(ret, 0, NULL);
    zassert_not_equal(cons_a, cons_b, "两个消费者应为不同槽位");

    data_bus_consumer_t* cons_b_saved = cons_b;

    ret = data_bus_consumer_unregister(cons_a);
    zassert_equal(ret, 0, NULL);

    /* cons_b 地址必须不变，且仍可正常注销 */
    zassert_equal(cons_b, cons_b_saved, "注销其他槽位后指针不应被数组压缩覆盖");
    ret = data_bus_consumer_unregister(cons_b);
    zassert_equal(ret, 0, NULL);

    /* 已注销的 cons_a 不可再次注销 */
    ret = data_bus_consumer_unregister(cons_a);
    zassert_equal(ret, -EINVAL, NULL);

    zassert_equal(data_bus_channel_destroy(ch), 0, NULL);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 测试 publish_block（零拷贝）
 */
ZTEST(test_data_bus, test_publish_block) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t*  ch = NULL;
    data_bus_consumer_t* cons = NULL;
    int                  ret = data_bus_channel_create("block_ch", &ch);
    zassert_equal(ret, 0, "通道创建失败");

    /* 注册自动释放消费者 */
    data_bus_consumer_cfg_t cfg = {
        .name = "block_consumer",
        .manual_release = false,
        .callback = auto_consumer_cb,
        .user_data = NULL,
    };
    ret = data_bus_consumer_register(ch, &cfg, &cons);
    zassert_equal(ret, 0, "消费者注册失败");

    /* 分配块并填充数据 */
    const char*       msg = "zero-copy message";
    size_t            msg_len = strlen(msg) + 1;
    data_bus_block_t* block = data_bus_mem_alloc(msg_len);
    zassert_not_null(block, "mem_alloc 失败");

    memcpy(block->ptr, msg, msg_len);
    block->len = msg_len;

    /* 发布块 */
    ret = data_bus_publish_block(ch, block);
    zassert_equal(ret, 0, "publish_block 失败");

    /* 等待消费者回调 */
    ret = k_sem_take(&g_test_sem, K_MSEC(100));
    zassert_equal(ret, 0, "消费者回调超时");
    zassert_equal(atomic_get(&g_call_count), 1, "消费者应被调用一次");

    /* destroy 要求先注销全部消费者 */
    zassert_equal(data_bus_consumer_unregister(cons), 0, NULL);
    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 测试从 ISR 上下文发布数据
 */
ZTEST(test_data_bus, test_publish_from_isr) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t*  ch = NULL;
    data_bus_consumer_t* cons = NULL;
    int                  ret = data_bus_channel_create("isr_ch", &ch);
    zassert_equal(ret, 0, "通道创建失败");

    /* 注册消费者 */
    data_bus_consumer_cfg_t cfg = {
        .name = "isr_consumer",
        .manual_release = false,
        .callback = auto_consumer_cb,
        .user_data = NULL,
    };
    ret = data_bus_consumer_register(ch, &cfg, &cons);
    zassert_equal(ret, 0, "消费者注册失败");

    /* 设置 ISR 发布目标 */
    g_isr_ch = ch;
    k_timer_init(&g_isr_timer, isr_timer_handler, NULL);
    k_timer_start(&g_isr_timer, K_MSEC(10), K_NO_WAIT);

    /* 等待 ISR 发布的数据被消费 */
    ret = k_sem_take(&g_test_sem, K_MSEC(200));
    zassert_equal(ret, 0, "ISR 发布消费者超时");
    zassert_equal(atomic_get(&g_call_count), 1, "消费者应被调用一次");

    k_timer_stop(&g_isr_timer);
    g_isr_ch = NULL;

    /* destroy 要求先注销全部消费者 */
    zassert_equal(data_bus_consumer_unregister(cons), 0, NULL);
    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 测试统计获取与重置
 */
ZTEST(test_data_bus, test_reset_stats) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t*  ch = NULL;
    data_bus_consumer_t* cons = NULL;
    int                  ret = data_bus_channel_create("stats_ch", &ch);
    zassert_equal(ret, 0, "通道创建失败");

    /* 注册消费者 */
    data_bus_consumer_cfg_t cfg = {
        .name = "stats_consumer",
        .manual_release = false,
        .callback = auto_consumer_cb,
        .user_data = NULL,
    };
    ret = data_bus_consumer_register(ch, &cfg, &cons);
    zassert_equal(ret, 0, "消费者注册失败");

    /* 发布数据 */
    const uint8_t test_data[] = {0x01, 0x02};
    ret = data_bus_publish(ch, test_data, sizeof(test_data));
    zassert_equal(ret, 0, "发布失败");

    ret = k_sem_take(&g_test_sem, K_MSEC(100));
    zassert_equal(ret, 0, "消费者超时");

    /* 验证统计非零 */
    data_bus_stats_t stats;
    data_bus_channel_get_stats(ch, &stats);
    zassert_equal(stats.publish_count, 1, "publish_count 应为 1");
    zassert_equal(stats.consumer_count, 1, "consumer_count 应为 1");

    /* 重置统计 */
    data_bus_reset_stats(ch);
    data_bus_channel_get_stats(ch, &stats);
    zassert_equal(stats.publish_count, 0, "publish_count 应为 0");
    zassert_equal(stats.drop_count, 0, "drop_count 应为 0");
    zassert_equal(stats.queue_full_count, 0, "queue_full_count 应为 0");
    zassert_equal(stats.alloc_fail_count, 0, "alloc_fail_count 应为 0");
    zassert_equal(stats.malloc_fallback_count, 0, "malloc_fallback_count 应为 0");
    zassert_equal(stats.slab_exhausted_count, 0, "slab_exhausted_count 应为 0");
    zassert_equal(stats.consumer_count, 1, "consumer_count 应保持 1");
    zassert_equal(stats.peak_queue_usage, 0, "peak_queue_usage 应为 0");

    /* destroy 要求先注销全部消费者 */
    zassert_equal(data_bus_consumer_unregister(cons), 0, NULL);
    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

#if !IS_ENABLED(CONFIG_DATA_BUS_NO_MALLOC)
/**
 * @brief 测试 k_malloc fallback 与 slab 耗尽标记统计
 */
ZTEST(test_data_bus, test_malloc_fallback_stats) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t* ch = NULL;
    zassert_equal(data_bus_channel_create("fallback_ch", &ch), 0, NULL);

    memset(g_large_payload, 0xA5, sizeof(g_large_payload));
    zassert_equal(data_bus_publish(ch, g_large_payload, sizeof(g_large_payload)), 0, NULL);

    data_bus_stats_t stats;
    data_bus_channel_get_stats(ch, &stats);
    zassert_equal(stats.malloc_fallback_count, 1, "large payload 应回退到 k_malloc");
    zassert_equal(stats.slab_exhausted_count, 0, "large payload 无匹配 slab，不应计入 slab 耗尽");

    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);

#if IS_ENABLED(CONFIG_DATA_BUS_SLAB_ENABLE) && (CONFIG_DATA_BUS_SLAB_256_COUNT > 0)
    data_bus_block_t* blocks[CONFIG_DATA_BUS_SLAB_256_COUNT + 1];
    memset(blocks, 0, sizeof(blocks));

    for (int i = 0; i < CONFIG_DATA_BUS_SLAB_256_COUNT + 1; i++) {
        blocks[i] = data_bus_mem_alloc(1);
        zassert_not_null(blocks[i], "block allocation %d failed", i);
    }

    zassert_false(blocks[0]->malloc_fallback, "first small block should use slab");
    zassert_false(blocks[0]->slab_exhausted, "first small block should not mark exhausted");
    zassert_true(blocks[CONFIG_DATA_BUS_SLAB_256_COUNT]->malloc_fallback, "exhausted slab should fall back");
    zassert_true(blocks[CONFIG_DATA_BUS_SLAB_256_COUNT]->slab_exhausted, "matching slab exhaustion should be recorded");

    for (int i = 0; i < CONFIG_DATA_BUS_SLAB_256_COUNT + 1; i++) {
        data_bus_mem_free(blocks[i]);
    }
#endif
}

/**
 * @brief 同一预分配块重试发布时，内存降级统计只记录一次
 */
ZTEST(test_data_bus, test_publish_block_retry_counts_memory_once) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t* ch = NULL;
    zassert_equal(data_bus_channel_create("retry_stats", &ch), 0, NULL);

    data_bus_block_t* block = data_bus_mem_alloc(sizeof(g_large_payload));
    zassert_not_null(block, "大块分配失败");
    zassert_true(block->malloc_fallback, "超过最大 slab 的块应使用 k_malloc");

    k_spinlock_key_t key = k_spin_lock(&ch->lock);
    ch->queue_used = CONFIG_DATA_BUS_CHANNEL_QUEUE_DEPTH;
    k_spin_unlock(&ch->lock, key);

    zassert_equal(data_bus_publish_block(ch, block), -ENOBUFS, NULL);
    zassert_equal(data_bus_publish_block(ch, block), -ENOBUFS, NULL);

    data_bus_stats_t stats;
    data_bus_channel_get_stats(ch, &stats);
    zassert_equal(stats.malloc_fallback_count, 1, "同一块重试不应重复统计内存回退");
    zassert_equal(stats.queue_full_count, 2, "两次入队失败都应计入队列满");

    key = k_spin_lock(&ch->lock);
    ch->queue_used = 0;
    k_spin_unlock(&ch->lock, key);

    data_bus_mem_free(block);
    zassert_equal(data_bus_channel_destroy(ch), 0, NULL);
    zassert_equal(data_bus_deinit(), 0, NULL);
}
#endif

/**
 * @brief publish_block 在 ref_count != 0 时应拒绝
 */
ZTEST(test_data_bus, test_publish_block_rejects_nonzero_ref) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t* ch = NULL;
    int                 ret = data_bus_channel_create("ref_ch", &ch);
    zassert_equal(ret, 0, "通道创建失败");

    data_bus_block_t* block = data_bus_mem_alloc(4);
    zassert_not_null(block, "mem_alloc 失败");

    atomic_set(&block->ref_count, 1);
    ret = data_bus_publish_block(ch, block);
    zassert_equal(ret, -EINVAL, "非零 ref_count 应拒绝入队");

    atomic_set(&block->ref_count, 0);
    data_bus_mem_free(block);
    zassert_equal(data_bus_channel_destroy(ch), 0, NULL);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 单通道连续填满队列深度，验证分发线程能排空（回归旧版每轮仅处理 8 块）
 *
 * 入队数量不得超过 CONFIG_DATA_BUS_CHANNEL_QUEUE_DEPTH，否则返回 -ENOBUFS。
 */
ZTEST(test_data_bus, test_dispatch_burst_drain) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t*  ch = NULL;
    data_bus_consumer_t* cons = NULL;
    int                  ret = data_bus_channel_create("burst_ch", &ch);
    zassert_equal(ret, 0, "通道创建失败");

    data_bus_consumer_cfg_t cfg = {
        .name = "burst_consumer",
        .manual_release = false,
        .callback = auto_consumer_cb,
        .user_data = NULL,
    };
    ret = data_bus_consumer_register(ch, &cfg, &cons);
    zassert_equal(ret, 0, "消费者注册失败");

    const int burst_count = CONFIG_DATA_BUS_CHANNEL_QUEUE_DEPTH;

    for (int round = 0; round < 2; round++) {
        atomic_set(&g_call_count, 0);

        for (int i = 0; i < burst_count; i++) {
            const uint8_t payload[] = {(uint8_t) (round * 16 + i), 0xEE};
            ret = data_bus_publish(ch, payload, sizeof(payload));
            zassert_equal(ret, 0, "第 %d 轮发布 %d 失败", round, i);
        }

        for (int i = 0; i < burst_count; i++) {
            ret = k_sem_take(&g_test_sem, K_MSEC(500));
            zassert_equal(ret, 0, "第 %d 轮投递 %d 超时", round, i);
        }

        zassert_equal(atomic_get(&g_call_count), burst_count, "第 %d 轮应投递全部 %d 块", round, burst_count);
    }

    /* destroy 要求先注销全部消费者 */
    zassert_equal(data_bus_consumer_unregister(cons), 0, NULL);
    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 非 active 通道应拒绝 publish（与 destroy 前置 active=0 语义一致）
 */
ZTEST(test_data_bus, test_inactive_channel_rejects_publish) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t* ch = NULL;
    int                 ret = data_bus_channel_create("inactive_ch", &ch);
    zassert_equal(ret, 0, "通道创建失败");

    atomic_set(&ch->active, 0);

    const uint8_t payload[] = {0x01, 0x02};
    ret = data_bus_publish(ch, payload, sizeof(payload));
    zassert_equal(ret, -ESHUTDOWN, "非 active 通道应返回 -ESHUTDOWN");

    atomic_set(&ch->active, 1);
    ret = data_bus_channel_destroy(ch);
    zassert_equal(ret, 0, "通道销毁失败");

    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 未初始化时创建通道应失败
 */
ZTEST(test_data_bus, test_requires_init) {
    data_bus_test_setup();

    zassert_equal(data_bus_deinit(), 0, NULL);

    data_bus_channel_t* ch = NULL;
    int                 ret = data_bus_channel_create("no_init_ch", &ch);
    zassert_equal(ret, -ENODEV, "未初始化应返回 -ENODEV");
    zassert_is_null(ch, "不应输出通道指针");
}

/**
 * @brief 指针队列：destroy 前 drain 排空（PR-1 回归）
 */
ZTEST(test_data_bus, test_queue_drain_before_destroy) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t* ch = NULL;
    zassert_equal(data_bus_channel_create("drain_ch", &ch), 0, NULL);

    for (int i = 0; i < 3; i++) {
        data_bus_block_t* block = data_bus_mem_alloc(1U);
        zassert_not_null(block, "block allocation %d failed", i);
        data_bus_block_acquire(block);

        k_spinlock_key_t key = k_spin_lock(&ch->lock);
        ch->queue[ch->queue_tail] = block;
        ch->queue_tail = (ch->queue_tail + 1U) % CONFIG_DATA_BUS_CHANNEL_QUEUE_DEPTH;
        ch->queue_used++;
        k_spin_unlock(&ch->lock, key);
    }

    k_spinlock_key_t key = k_spin_lock(&ch->lock);
    zassert_equal(ch->queue_used, 3U, "queue should hold 3 blocks before drain");
    k_spin_unlock(&ch->lock, key);

    data_bus_channel_drain_pending(ch, false);

    key = k_spin_lock(&ch->lock);
    zassert_equal(ch->queue_used, 0U, "drain should empty queue");
    k_spin_unlock(&ch->lock, key);

    zassert_equal(data_bus_channel_destroy(ch), 0, NULL);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

static void overwrite_seq_consumer_cb(data_bus_channel_t* ch, data_bus_block_t* block, void* user_data) {
    ARG_UNUSED(ch);
    ARG_UNUSED(user_data);

    atomic_inc(&g_call_count);
    g_recv_seq = block->seq;
    k_sem_give(&g_test_sem);
}

/**
 * @brief OVERWRITE 通道：连续 publish 后消费者收到最新 seq
 */
ZTEST(test_data_bus, test_overwrite_latest_seq) {
    data_bus_channel_t*     ch = NULL;
    data_bus_consumer_cfg_t cfg = {
        .name = "ow_cons",
        .manual_release = false,
        .callback = overwrite_seq_consumer_cb,
        .user_data = NULL,
    };
    const data_bus_channel_cfg_t ch_cfg = {.flags = DATA_BUS_CHANNEL_OVERWRITE};

    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);
    zassert_equal(data_bus_channel_create_ex("ow_ch", &ch_cfg, &ch), 0, NULL);

    const uint8_t payload[] = {0x01};
    const int     publish_count = OVERWRITE_BURST_PUBLISH_COUNT;

    for (int i = 0; i < publish_count; i++) {
        zassert_equal(data_bus_publish(ch, payload, sizeof(payload)), 0, "publish %d", i);
    }

    data_bus_test_wait_channel_quiescent(ch);

    data_bus_stats_t stats;
    data_bus_channel_get_stats(ch, &stats);
    zassert_true(stats.peak_queue_usage <= 1U, "overwrite peak_queue_usage must be <= 1");

    data_bus_consumer_t* cons = NULL;
    zassert_equal(data_bus_consumer_register(ch, &cfg, &cons), 0, NULL);
    zassert_equal(data_bus_publish(ch, payload, sizeof(payload)), 0, NULL);
    zassert_equal(k_sem_take(&g_test_sem, K_MSEC(500)), 0, NULL);
    zassert_equal(g_recv_seq, (uint32_t) publish_count, "consumer should see post-burst seq");

    zassert_equal(data_bus_consumer_unregister(cons), 0, NULL);
    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief OVERWRITE 无消费者时 peak_queue_usage <= 1
 */
ZTEST(test_data_bus, test_overwrite_peak_queue_no_consumer) {
    data_bus_channel_t*          ch = NULL;
    const data_bus_channel_cfg_t ch_cfg = {.flags = DATA_BUS_CHANNEL_OVERWRITE};

    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);
    zassert_equal(data_bus_channel_create_ex("ow_peak", &ch_cfg, &ch), 0, NULL);

    const uint8_t payload[] = {0x02};
    for (int i = 0; i < CONFIG_DATA_BUS_CHANNEL_QUEUE_DEPTH + 10; i++) {
        zassert_equal(data_bus_publish(ch, payload, sizeof(payload)), 0, NULL);
    }

    data_bus_stats_t stats;
    data_bus_channel_get_stats(ch, &stats);
    zassert_true(stats.peak_queue_usage <= 1U, NULL);
    zassert_equal(stats.publish_count, (uint32_t) (CONFIG_DATA_BUS_CHANNEL_QUEUE_DEPTH + 10), NULL);

    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief OVERWRITE + retain：队列中旧块 ref_count!=1 时丢弃新数据（无 UAF）
 */
ZTEST(test_data_bus, test_overwrite_retain_drops_new) {
    data_bus_channel_t*          ch = NULL;
    const data_bus_channel_cfg_t ch_cfg = {.flags = DATA_BUS_CHANNEL_OVERWRITE};

    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);
    zassert_equal(data_bus_channel_create_ex("ow_retain_ch", &ch_cfg, &ch), 0, NULL);

    data_bus_block_t* queued = data_bus_mem_alloc(1U);
    zassert_not_null(queued, NULL);
    data_bus_block_acquire(queued);
    zassert_not_null(data_bus_block_retain(queued), NULL);

    k_spinlock_key_t key = k_spin_lock(&ch->lock);
    ch->queue[ch->queue_tail] = queued;
    ch->queue_tail = (ch->queue_tail + 1U) % CONFIG_DATA_BUS_CHANNEL_QUEUE_DEPTH;
    ch->queue_used = 1U;
    k_spin_unlock(&ch->lock, key);

    const uint8_t p2[] = {0x02};
    zassert_equal(data_bus_publish(ch, p2, sizeof(p2)), -ENOBUFS, "new data dropped while queued block retained");

    data_bus_stats_t stats;
    data_bus_channel_get_stats(ch, &stats);
    zassert_equal(stats.drop_count, 1U, NULL);

    key = k_spin_lock(&ch->lock);
    queued = ch->queue[ch->queue_head];
    ch->queue_head = (ch->queue_head + 1U) % CONFIG_DATA_BUS_CHANNEL_QUEUE_DEPTH;
    ch->queue_used--;
    k_spin_unlock(&ch->lock, key);
    data_bus_block_release(queued);
    data_bus_block_release(queued);

    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

#if CONFIG_DATA_BUS_SLAB_64_COUNT > 0
ZTEST(test_data_bus, test_slab_64_allocation) {
    data_bus_test_setup();

    data_bus_block_t* block = data_bus_mem_alloc(48);
    zassert_not_null(block, NULL);
    zassert_false(block->malloc_fallback, "48B should use 64B slab");
    zassert_not_null(block->slab, "should be slab-backed");
    data_bus_mem_free(block);
}
#endif

#if CONFIG_DATA_BUS_SLAB_128_COUNT > 0
ZTEST(test_data_bus, test_slab_128_allocation) {
    data_bus_test_setup();

    data_bus_block_t* block = data_bus_mem_alloc(100);
    zassert_not_null(block, NULL);
    zassert_false(block->malloc_fallback, NULL);
    data_bus_mem_free(block);
}
#endif

#if CONFIG_DATA_BUS_SLAB_512_COUNT > 0
ZTEST(test_data_bus, test_slab_512_allocation) {
    data_bus_test_setup();

    data_bus_block_t* block = data_bus_mem_alloc(400);
    zassert_not_null(block, NULL);
    zassert_false(block->malloc_fallback, NULL);
    data_bus_mem_free(block);
}
#endif

#if IS_ENABLED(CONFIG_DATA_BUS_NO_MALLOC)
ZTEST(test_data_bus, test_no_malloc_exhaustion) {
    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);

    data_bus_channel_t* ch = NULL;
    zassert_equal(data_bus_channel_create("no_malloc_ch", &ch), 0, NULL);

#if IS_ENABLED(CONFIG_DATA_BUS_SLAB_ENABLE) && (CONFIG_DATA_BUS_SLAB_256_COUNT > 0)
    data_bus_block_t* blocks[CONFIG_DATA_BUS_SLAB_256_COUNT + 1];
    memset(blocks, 0, sizeof(blocks));

    for (int i = 0; i < CONFIG_DATA_BUS_SLAB_256_COUNT + 1; i++) {
        blocks[i] = data_bus_mem_alloc(200);
        if (i < CONFIG_DATA_BUS_SLAB_256_COUNT) {
            zassert_not_null(blocks[i], "block %d", i);
            zassert_false(blocks[i]->malloc_fallback, NULL);
        } else {
            zassert_is_null(blocks[i], "NO_MALLOC must not fall back to k_malloc");
        }
    }

    for (int i = 0; i < CONFIG_DATA_BUS_SLAB_256_COUNT; i++) {
        data_bus_mem_free(blocks[i]);
    }
#endif

    data_bus_stats_t stats;
    data_bus_channel_get_stats(ch, &stats);
    zassert_equal(stats.malloc_fallback_count, 0U, NULL);

    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}
#endif

static int inplace_fill_ok(void* buf, size_t len, void* user_data) {
    const char* msg = (const char*) user_data;

    if (msg == NULL) {
        return -EINVAL;
    }
    size_t msg_len = strlen(msg) + 1U;
    if (msg_len > len) {
        return -EINVAL;
    }
    memcpy(buf, msg, msg_len);
    return 0;
}

static int inplace_fill_fail(void* buf, size_t len, void* user_data) {
    ARG_UNUSED(buf);
    ARG_UNUSED(len);
    ARG_UNUSED(user_data);
    return -EIO;
}

/**
 * @brief publish_inplace 与 publish_block 语义一致
 */
ZTEST(test_data_bus, test_publish_inplace) {
    data_bus_channel_t*     ch = NULL;
    data_bus_consumer_t*    cons = NULL;
    data_bus_consumer_cfg_t cfg = {
        .name = "inplace_cons",
        .manual_release = false,
        .callback = auto_consumer_cb,
        .user_data = NULL,
    };
    const char* msg = "inplace zero-copy";

    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);
    zassert_equal(data_bus_channel_create("inplace_ch", &ch), 0, NULL);
    zassert_equal(data_bus_consumer_register(ch, &cfg, &cons), 0, NULL);

    size_t msg_len = strlen(msg) + 1U;
    zassert_equal(data_bus_publish_inplace(ch, msg_len, inplace_fill_ok, (void*) msg), 0, NULL);
    zassert_equal(k_sem_take(&g_test_sem, K_MSEC(200)), 0, NULL);
    zassert_equal(atomic_get(&g_call_count), 1, NULL);

    zassert_equal(data_bus_publish_inplace(ch, msg_len, inplace_fill_fail, NULL), -EIO, NULL);

    zassert_equal(data_bus_consumer_unregister(cons), 0, NULL);
    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

static void slow_consumer_cb(data_bus_channel_t* ch, data_bus_block_t* block, void* user_data) {
    ARG_UNUSED(ch);
    ARG_UNUSED(block);
    ARG_UNUSED(user_data);

    k_sleep(K_MSEC(5));
    atomic_inc(&g_call_count);
    k_sem_give(&g_test_sem);
}

static int inplace_fill_imu(void* buf, size_t len, void* user_data) {
    if (user_data == NULL || buf == NULL) {
        return -EINVAL;
    }
    memcpy(buf, user_data, len);
    return 0;
}

/**
 * @brief 基准场景 B：慢消费者导致队列满
 */
ZTEST(test_data_bus, test_benchmark_slow_consumer_queue_full) {
    data_bus_channel_t*     ch = NULL;
    data_bus_consumer_t*    cons = NULL;
    data_bus_consumer_cfg_t cfg = {
        .name = "slow_cons",
        .manual_release = false,
        .callback = slow_consumer_cb,
        .user_data = NULL,
    };

    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);
    zassert_equal(data_bus_channel_create("slow_q", &ch), 0, NULL);
    zassert_equal(data_bus_consumer_register(ch, &cfg, &cons), 0, NULL);

    const uint8_t payload[256] = {0xAB};
    int           drops = 0;

    for (int i = 0; i < CONFIG_DATA_BUS_CHANNEL_QUEUE_DEPTH + 8; i++) {
        int ret = data_bus_publish(ch, payload, sizeof(payload));
        if (ret == -ENOBUFS) {
            drops++;
        } else {
            zassert_equal(ret, 0, "publish %d", i);
        }
    }

    zassert_true(drops > 0, "slow consumer should cause queue pressure");

    data_bus_stats_t stats;
    data_bus_channel_get_stats(ch, &stats);
    LOG_INF("benchmark slow consumer: publish=%u drop=%u queue_full=%u peak_q=%u", stats.publish_count,
            stats.drop_count, stats.queue_full_count, stats.peak_queue_usage);

    data_bus_test_wait_channel_quiescent(ch);
    zassert_equal(data_bus_consumer_unregister(cons), 0, NULL);

    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/**
 * @brief 基准场景 A：IMU 模拟（OVERWRITE + 高频 publish）
 */
ZTEST(test_data_bus, test_benchmark_imu_overwrite) {
    data_bus_channel_t*          ch = NULL;
    const data_bus_channel_cfg_t ch_cfg = {.flags = DATA_BUS_CHANNEL_OVERWRITE};
    data_bus_consumer_t*         cons = NULL;
    data_bus_consumer_cfg_t      cfg = {
             .name = "imu_cons",
             .manual_release = false,
             .callback = auto_consumer_cb,
             .user_data = NULL,
    };

    data_bus_test_setup();
    zassert_equal(data_bus_init(), 0, NULL);
    zassert_equal(data_bus_channel_create_ex("imu_bench", &ch_cfg, &ch), 0, NULL);
    zassert_equal(data_bus_consumer_register(ch, &cfg, &cons), 0, NULL);

    uint8_t imu_payload[64];
    for (int round = 0; round < IMU_BENCH_ROUND_COUNT; round++) {
        imu_payload[0] = (uint8_t) round;
        zassert_equal(data_bus_publish_inplace(ch, sizeof(imu_payload), inplace_fill_imu, imu_payload), 0, NULL);
    }

    data_bus_stats_t stats;
    data_bus_channel_get_stats(ch, &stats);
    zassert_true(stats.peak_queue_usage <= 1U, NULL);
    zassert_equal(stats.alloc_fail_count, 0U, NULL);
    LOG_INF("benchmark IMU overwrite: publish=%u drop=%u peak_q=%u malloc_fb=%u", stats.publish_count, stats.drop_count,
            stats.peak_queue_usage, stats.malloc_fallback_count);

    data_bus_test_wait_channel_quiescent(ch);
    zassert_equal(data_bus_consumer_unregister(cons), 0, NULL);

    data_bus_test_destroy_channel(ch);
    zassert_equal(data_bus_deinit(), 0, NULL);
}

/* ============================================================================
 * 测试套件
 * ============================================================================ */

static void data_bus_test_after(void* fixture) {
    ARG_UNUSED(fixture);
    data_bus_test_force_reset();
}

ZTEST_SUITE(test_data_bus, NULL, NULL, NULL, data_bus_test_after, NULL);
