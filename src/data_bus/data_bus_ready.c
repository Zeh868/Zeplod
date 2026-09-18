/**
 * @file data_bus_ready.c
 * @brief Data Bus 就绪通道环形队列
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-05-28
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-05-28       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            审查整改：就绪环满告警日志移出关中断
 *                                                 临界区（先快照标志、解锁后打印）
 *
 */

#include "data_bus_ready.h"

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include "data_bus_channel.h"
#include "data_bus_internal.h"

LOG_MODULE_DECLARE(data_bus, CONFIG_DATA_BUS_LOG_LEVEL);

#ifndef DATA_BUS_READY_QUEUE_CAPACITY
#define DATA_BUS_READY_QUEUE_CAPACITY (CONFIG_DATA_BUS_MAX_CHANNELS * 2U)
#endif

BUILD_ASSERT(DATA_BUS_READY_QUEUE_CAPACITY >= CONFIG_DATA_BUS_MAX_CHANNELS);

static struct {
    struct k_spinlock   lock;
    data_bus_channel_t* ring[DATA_BUS_READY_QUEUE_CAPACITY];
    uint32_t            head;
    uint32_t            tail;
    uint32_t            count;
} g_ready;

static atomic_t g_ready_fallback_pending;

static bool ready_ring_push(data_bus_channel_t* ch) {
    /* 队列满仅返回失败标志，由调用方在临界区外打印日志（锁内不做日志 IO） */
    if (g_ready.count >= DATA_BUS_READY_QUEUE_CAPACITY) {
        return false;
    }

    g_ready.ring[g_ready.tail] = ch;
    g_ready.tail = (g_ready.tail + 1U) % DATA_BUS_READY_QUEUE_CAPACITY;
    g_ready.count++;
    return true;
}

static data_bus_channel_t* ready_ring_pop(void) {
    if (g_ready.count == 0U) {
        return NULL;
    }

    data_bus_channel_t* ch = g_ready.ring[g_ready.head];

    g_ready.head = (g_ready.head + 1U) % DATA_BUS_READY_QUEUE_CAPACITY;
    g_ready.count--;
    return ch;
}

void data_bus_ready_init(void) {
    g_ready.head = 0U;
    g_ready.tail = 0U;
    g_ready.count = 0U;
    atomic_set(&g_ready_fallback_pending, 0);
}

void data_bus_ready_reset(void) {
    k_spinlock_key_t key = k_spin_lock(&g_ready.lock);

    g_ready.head = 0U;
    g_ready.tail = 0U;
    g_ready.count = 0U;
    atomic_set(&g_ready_fallback_pending, 0);
    k_spin_unlock(&g_ready.lock, key);
}

void data_bus_ready_resync(data_bus_channel_t* ch) {
    if (ch == NULL) {
        return;
    }

    k_spinlock_key_t skey = k_spin_lock(&ch->lock);
    bool             pending = ch->queue_used > 0U;
    k_spin_unlock(&ch->lock, skey);

    if (pending) {
        data_bus_ready_signal(ch);
    }
}

void data_bus_ready_signal(data_bus_channel_t* ch) {
    if (ch == NULL) {
        return;
    }

    bool wake = false;
    bool queue_overflow = false;

    k_spinlock_key_t key = k_spin_lock(&g_ready.lock);

    if (atomic_cas(&ch->dispatch_ready, 0, 1)) {
        if (ready_ring_push(ch)) {
            wake = true;
        } else {
            (void) atomic_cas(&ch->dispatch_ready, 1, 0);
            atomic_set(&g_ready_fallback_pending, 1);
            wake = true;
            queue_overflow = true;
        }
    } else {
        wake = true;
    }

    k_spin_unlock(&g_ready.lock, key);

    /* 先在临界区内快照溢出标志，解锁后再打印，缩短关中断时间 */
    if (queue_overflow) {
        LOG_WRN("Data bus ready queue full for '%s'", ch->name);
    }

    if (wake) {
        k_sem_give(&g_dispatcher_sem);
    }
}

data_bus_channel_t* data_bus_ready_pop(void) {
    k_spinlock_key_t    key = k_spin_lock(&g_ready.lock);
    data_bus_channel_t* ch = ready_ring_pop();

    k_spin_unlock(&g_ready.lock, key);
    return ch;
}

bool data_bus_ready_claim(data_bus_channel_t* ch) {
    if (ch == NULL) {
        return false;
    }

    k_mutex_lock(&g_channels_lock, K_FOREVER);

    bool ok = false;

    if (atomic_get(&g_shutting_down)) {
        k_mutex_unlock(&g_channels_lock);
        (void) atomic_set(&ch->dispatch_ready, 0);
        return false;
    }

    for (uint32_t i = 0; i < g_channel_count; i++) {
        if (g_channels[i] == ch && atomic_get(&ch->active)) {
            (void) atomic_inc(&ch->dispatch_hold);
            ok = true;
            break;
        }
    }

    k_mutex_unlock(&g_channels_lock);

    if (!ok) {
        /*
         * 不变量：通道一旦入就绪环即 queue_used>0，且仅 dispatcher 排空队列；
         * destroy 在 queue_used>0 时直接 -EAGAIN，deinit 先 join dispatcher 再释放通道。
         * 故 dispatcher 持有已 pop 的通道期间 ch 不会被释放，此处写入 ch 安全。
         */
        (void) atomic_set(&ch->dispatch_ready, 0);
    }

    return ok;
}

void data_bus_ready_finish(data_bus_channel_t* ch) {
    if (ch == NULL) {
        return;
    }

    for (;;) {
        data_bus_channel_drain_pending(ch, true);

        k_spinlock_key_t skey = k_spin_lock(&ch->lock);
        if (ch->queue_used > 0) {
            k_spin_unlock(&ch->lock, skey);
            continue;
        }
        if (atomic_cas(&ch->dispatch_ready, 1, 0)) {
            k_spin_unlock(&ch->lock, skey);
            break;
        }
        /* destroy 等路径已清除 dispatch_ready 时避免 CAS 空转 */
        if (atomic_get(&ch->dispatch_ready) == 0) {
            k_spin_unlock(&ch->lock, skey);
            break;
        }
        k_spin_unlock(&ch->lock, skey);
    }

    (void) atomic_dec(&ch->dispatch_hold);
}

static void data_bus_ready_run_fallback(void) {
    data_bus_channel_t* snap[CONFIG_DATA_BUS_MAX_CHANNELS];
    uint32_t            n = 0U;

    k_mutex_lock(&g_channels_lock, K_FOREVER);
    for (uint32_t i = 0; i < g_channel_count; i++) {
        data_bus_channel_t* ch = g_channels[i];

        if (ch == NULL || !atomic_get(&ch->active)) {
            continue;
        }

        k_spinlock_key_t skey = k_spin_lock(&ch->lock);
        bool             pending = ch->queue_used > 0U;

        if (pending) {
            (void) atomic_inc(&ch->dispatch_hold);
            snap[n++] = ch;
        }
        k_spin_unlock(&ch->lock, skey);
    }
    k_mutex_unlock(&g_channels_lock);

    for (uint32_t i = 0; i < n; i++) {
        data_bus_channel_t* ch = snap[i];

        data_bus_ready_finish(ch);
    }
}

bool data_bus_ready_consume_fallback(void) {
    if (atomic_cas(&g_ready_fallback_pending, 1, 0)) {
        data_bus_ready_run_fallback();
        return true;
    }
    return false;
}

bool data_bus_ready_pending(void) {
    k_spinlock_key_t key = k_spin_lock(&g_ready.lock);
    bool             pending = (g_ready.count > 0U) || (atomic_get(&g_ready_fallback_pending) != 0);

    k_spin_unlock(&g_ready.lock, key);
    return pending;
}
