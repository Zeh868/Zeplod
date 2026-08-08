/**
 * @file data_bus.c
 * @brief Data Bus 核心 - 分发线程、初始化/反初始化、统计
 *
 * 分发线程从各通道队列中取出数据块，调用 data_bus_consumer_dispatch()
 * 将数据分发给所有注册的消费者。
 * @author zeh (china_qzh@163.com)
 * @version 2.0
 * @date 2026-05-15
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-05-15       2.0            zeh            重构：适配统一 auto_release 分发模型
 * 2026-05-15       2.1            zeh            分发线程：queue 非空时无消费者也出队并 release，避免块泄漏
 * 2026-05-19       2.2            zeh            init 校验分发线程；排空快照释放全局锁
 * 2026-05-20       2.3            zeh            init/deinit 互斥锁消除并发初始化竞态
 * 2026-05-28       2.4            zeh            deinit：有界 join，失败保留状态供重试（P2 生命周期对齐）
 * 2026-05-28       2.5            zeh            dispatcher 仅处理就绪队列中的 channel（P3.1）
 *
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <zeplod/app_config.h>
#include <zeplod/data_bus.h>
#include "data_bus_channel.h"
#include "data_bus_consumer.h"
#include "data_bus_internal.h"
#include "data_bus_memory.h"
#include "data_bus_ready.h"

LOG_MODULE_REGISTER(data_bus, CONFIG_DATA_BUS_LOG_LEVEL);

/* ============================================================================
 * 全局状态
 * ============================================================================ */

struct k_sem        g_dispatcher_sem;
data_bus_channel_t* g_channels[CONFIG_DATA_BUS_MAX_CHANNELS];
uint32_t            g_channel_count;
struct k_mutex      g_channels_lock;
atomic_t            g_initialized;
atomic_t            g_shutting_down;
static atomic_t     g_dispatcher_joined;

K_MUTEX_DEFINE(g_init_lock);

int data_bus_require_initialized(void) {
    if (!atomic_get(&g_initialized)) {
        return -ENODEV;
    }
    if (atomic_get(&g_shutting_down)) {
        return -ESHUTDOWN;
    }
    return 0;
}

static void data_bus_drain_all_channels(bool run_dispatch) {
    data_bus_channel_t* snap[CONFIG_DATA_BUS_MAX_CHANNELS];
    uint32_t            n;

    /* 快照后释放全局锁，避免回调内 create/unregister 与 g_channels_lock 死锁 */
    k_mutex_lock(&g_channels_lock, K_FOREVER);
    n = g_channel_count;
    for (uint32_t i = 0; i < n; i++) {
        snap[i] = g_channels[i];
        if (snap[i] != NULL) {
            (void) atomic_inc(&snap[i]->dispatch_hold);
        }
    }
    k_mutex_unlock(&g_channels_lock);

    for (uint32_t i = 0; i < n; i++) {
        if (snap[i] != NULL) {
            data_bus_channel_drain_pending(snap[i], run_dispatch);
            (void) atomic_dec(&snap[i]->dispatch_hold);
        }
    }
}

struct k_thread g_dispatcher_thread_data;
K_THREAD_STACK_DEFINE(g_dispatcher_stack, CONFIG_DATA_BUS_DISPATCHER_STACK_SIZE);

/* ============================================================================
 * 分发线程
 * ============================================================================ */

static void data_bus_dispatcher_thread(void* arg1, void* arg2, void* arg3) {
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    while (1) {
        if (atomic_get(&g_shutting_down)) {
            data_bus_drain_all_channels(false);
            break;
        }

        k_sem_take(&g_dispatcher_sem, K_FOREVER);

        if (atomic_get(&g_shutting_down)) {
            data_bus_drain_all_channels(false);
            break;
        }

        /*
         * 处理就绪通道，并在排空多余信号量后以就绪环（真相来源）重新判定。
         * k_sem_give 与 ready_ring_push 非原子：若仅在处理后排空信号量，
         * 可能把对应「已入环但尚未处理」就绪项的信号量一并吃掉，
         * 导致该通道数据滞留至下一次 publish 才被分发。
         * 故排空后通过 data_bus_ready_pending() 复查：仍有待处理工作则继续，
         * 否则才回到外层 k_sem_take(K_FOREVER) 阻塞等待。
         */
        do {
            while (true) {
                data_bus_channel_t* ch = data_bus_ready_pop();

                if (ch == NULL) {
                    break;
                }

                if (!data_bus_ready_claim(ch)) {
                    continue;
                }

                data_bus_ready_finish(ch);
            }

            (void) data_bus_ready_consume_fallback();

            /* 消费 publish 累积的多余信号量，避免每轮空转多次 k_sem_take */
            while (k_sem_take(&g_dispatcher_sem, K_NO_WAIT) == 0) {
                ;
            }
        } while (data_bus_ready_pending());
    }
}

/* ============================================================================
 * 公共 API: 生命周期
 * ============================================================================ */

int data_bus_init(void) {
    int ret = 0;

    k_mutex_lock(&g_init_lock, K_FOREVER);

    if (atomic_get(&g_initialized)) {
        ret = atomic_get(&g_shutting_down) ? -ESHUTDOWN : 0;
        k_mutex_unlock(&g_init_lock);
        return ret;
    }

    /* 初始化全局信号量 */
    k_sem_init(&g_dispatcher_sem, 0, K_SEM_MAX_LIMIT);
    data_bus_ready_init();

    /* 初始化通道表锁 */
    k_mutex_init(&g_channels_lock);

    /* 清空通道表 */
    memset(g_channels, 0, sizeof(g_channels));
    g_channel_count = 0;

    atomic_set(&g_shutting_down, 0);
    atomic_set(&g_dispatcher_joined, 0);

    /* 创建分发线程 */
    k_tid_t tid = k_thread_create(&g_dispatcher_thread_data, g_dispatcher_stack,
                                  K_THREAD_STACK_SIZEOF(g_dispatcher_stack), data_bus_dispatcher_thread, NULL, NULL,
                                  NULL, CONFIG_DATA_BUS_DISPATCHER_PRIORITY, 0, K_NO_WAIT);

    if (tid == NULL) {
        LOG_ERR("Failed to create data bus dispatcher thread");
        ret = -ENOMEM;
    } else {
        k_thread_name_set(tid, "data_bus_disp");
        atomic_set(&g_initialized, 1);
        LOG_DBG("Data bus initialized (disp stack=%d prio=%d)", CONFIG_DATA_BUS_DISPATCHER_STACK_SIZE,
                CONFIG_DATA_BUS_DISPATCHER_PRIORITY);
    }

    k_mutex_unlock(&g_init_lock);
    return ret;
}

int data_bus_deinit(void) {
    k_mutex_lock(&g_init_lock, K_FOREVER);

    if (!atomic_get(&g_initialized)) {
        k_mutex_unlock(&g_init_lock);
        return 0;
    }

    /* 发出关闭信号 */
    if (k_current_get() == &g_dispatcher_thread_data) {
        k_mutex_unlock(&g_init_lock);
        return -EINVAL;
    }

    k_mutex_lock(&g_channels_lock, K_FOREVER);
    atomic_set(&g_shutting_down, 1);
    k_mutex_unlock(&g_channels_lock);

    if (!atomic_get(&g_dispatcher_joined)) {
        /* 唤醒分发线程使其退出 */
        k_sem_give(&g_dispatcher_sem);

        int jret = k_thread_join(&g_dispatcher_thread_data, K_MSEC(DATA_BUS_DISPATCHER_JOIN_TIMEOUT_MS));

        if (jret != 0) {
            LOG_ERR("Data bus dispatcher join timed out: %d", jret);
            k_mutex_unlock(&g_init_lock);
            return -EIO;
        }
        atomic_set(&g_dispatcher_joined, 1);
    }

    data_bus_ready_reset();

    int64_t drain_deadline = k_uptime_get() + DATA_BUS_DEINIT_DRAIN_TIMEOUT_MS;

    while (true) {
        bool busy = false;

        k_mutex_lock(&g_channels_lock, K_FOREVER);
        for (uint32_t i = 0; i < g_channel_count; i++) {
            data_bus_channel_t* ch = g_channels[i];

            if (ch != NULL && (atomic_get(&ch->publish_hold) != 0 || atomic_get(&ch->dispatch_hold) != 0 ||
                               atomic_get(&ch->lifecycle_hold) != 0)) {
                busy = true;
                break;
            }
        }
        k_mutex_unlock(&g_channels_lock);

        if (!busy) {
            break;
        }
        if (k_uptime_get() > drain_deadline) {
            LOG_ERR("Data bus deinit drain timeout (channel operations still active)");
            k_mutex_unlock(&g_init_lock);
            return -EBUSY;
        }
        k_sleep(K_MSEC(1));
    }

    /* 锁定通道表 */
    k_mutex_lock(&g_channels_lock, K_FOREVER);

    /* 销毁所有通道（排空队列，释放块） */
    while (g_channel_count > 0) {
        data_bus_channel_t* ch = g_channels[0];
        if (ch != NULL) {
            /* obj_reset 内部会 drain_pending */
            data_bus_channel_obj_reset(ch);
            k_mem_slab_free(&data_bus_channel_slab, ch);
        }

        /* 从表中移除并压缩 */
        for (uint32_t j = 0; j < g_channel_count - 1; j++) {
            g_channels[j] = g_channels[j + 1];
        }
        g_channels[--g_channel_count] = NULL;
    }

    k_mutex_unlock(&g_channels_lock);

    atomic_set(&g_initialized, 0);
    atomic_set(&g_shutting_down, 0);
    atomic_set(&g_dispatcher_joined, 0);
    LOG_DBG("Data bus deinitialized");

    k_mutex_unlock(&g_init_lock);
    return 0;
}

/* ============================================================================
 * 公共 API: 统计
 * ============================================================================ */

void data_bus_channel_get_stats(const data_bus_channel_t* ch, data_bus_stats_t* stats) {
    if (stats == NULL) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

    if (ch == NULL || k_is_in_isr() || data_bus_require_initialized() != 0) {
        return;
    }

    k_mutex_lock(&g_channels_lock, K_FOREVER);

    data_bus_channel_t* ch_rw = (data_bus_channel_t*) ch;
    bool                found = false;

    for (uint32_t i = 0; i < g_channel_count; i++) {
        if (g_channels[i] == ch_rw) {
            found = true;
            break;
        }
    }

    if (!found || !atomic_get(&ch_rw->active)) {
        k_mutex_unlock(&g_channels_lock);
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&ch_rw->lock);
    stats->publish_count = ch->publish_count;
    stats->drop_count = ch->drop_count;
    stats->queue_full_count = ch->queue_full_count;
    stats->alloc_fail_count = ch->alloc_fail_count;
    stats->malloc_fallback_count = ch->malloc_fallback_count;
    stats->slab_exhausted_count = ch->slab_exhausted_count;
    stats->consumer_count = ch->consumer_count;
    stats->peak_queue_usage = ch->peak_queue_usage;
    k_spin_unlock(&ch_rw->lock, key);
    k_mutex_unlock(&g_channels_lock);
}

void data_bus_reset_stats(data_bus_channel_t* ch) {
    if (ch == NULL || k_is_in_isr() || data_bus_require_initialized() != 0) {
        return;
    }

    k_mutex_lock(&g_channels_lock, K_FOREVER);

    bool found = false;

    for (uint32_t i = 0; i < g_channel_count; i++) {
        if (g_channels[i] == ch) {
            found = true;
            break;
        }
    }

    if (!found || !atomic_get(&ch->active)) {
        k_mutex_unlock(&g_channels_lock);
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&ch->lock);
    ch->publish_count = 0;
    ch->drop_count = 0;
    ch->queue_full_count = 0;
    ch->alloc_fail_count = 0;
    ch->malloc_fallback_count = 0;
    ch->slab_exhausted_count = 0;
    ch->peak_queue_usage = 0;
    k_spin_unlock(&ch->lock, key);
    k_mutex_unlock(&g_channels_lock);
}

void data_bus_get_overview(data_bus_overview_t* overview) {
    if (overview == NULL) {
        return;
    }

    memset(overview, 0, sizeof(*overview));

    /* g_channels_lock 为互斥锁，ISR 中不可获取 */
    if (k_is_in_isr() || data_bus_require_initialized() != 0) {
        return;
    }

    k_mutex_lock(&g_channels_lock, K_FOREVER);
    overview->channel_count = g_channel_count;

    for (uint32_t i = 0; i < g_channel_count; i++) {
        data_bus_channel_t* ch = g_channels[i];
        if (ch == NULL) {
            continue;
        }

        k_spinlock_key_t key = k_spin_lock(&ch->lock);
        if (atomic_get(&ch->active)) {
            overview->active_channel_count++;
        }
        overview->total_queue_used += ch->queue_used;
        overview->total_publish_count += ch->publish_count;
        overview->total_drop_count += ch->drop_count;
        overview->total_alloc_fail_count += ch->alloc_fail_count;
        overview->total_malloc_fallback_count += ch->malloc_fallback_count;
        overview->total_slab_exhausted_count += ch->slab_exhausted_count;
        if (ch->peak_queue_usage > overview->peak_queue_usage) {
            overview->peak_queue_usage = ch->peak_queue_usage;
        }
        k_spin_unlock(&ch->lock, key);
    }

    k_mutex_unlock(&g_channels_lock);
}

/* ============================================================================
 * 自动初始化
 * ============================================================================ */

static int data_bus_auto_init(void) {
    return data_bus_init();
}

SYS_INIT(data_bus_auto_init, POST_KERNEL, APP_INIT_PRIO_DATA_BUS);
