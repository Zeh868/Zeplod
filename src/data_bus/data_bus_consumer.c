/**
 * @file data_bus_consumer.c
 * @brief Data Bus 消费者管理 - 注册/注销/分发
 *
 * 分发核心逻辑：
 * 1. 快照活跃消费者列表
 * 2. atomic_add(ref_count, active_count) 拆分引用
 * 3. 逐个调用消费者回调
 * 4. 非 manual_release 模式下，回调后框架自动 release
 * @author zeh (china_qzh@163.com)
 * @version 2.3
 * @date 2026-05-15
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-05-15       2.0            zeh            重构：删除 COPY 模式，实现 +N 引用拆分
 * 2026-05-20       2.1            zeh            消费者固定槽位：注销不压缩数组，保证 out_consumer 地址稳定
 * 2026-09-18       2.2            zeh            审查整改：注销等待 callback_hold 改有界超时
 * 2026-09-18       2.3            zeh            复审返工：注册路径名字格式化真正移出自旋锁临界区（锁前格式化、锁内 memcpy）
 *                                                 （超时 -EBUSY）；spinlock 内日志拼接改锁内
 *                                                 memcpy 快照、锁外输出
 *
 */

#include "data_bus_consumer.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>
#include "data_bus_internal.h"
#include "data_bus_memory.h"

LOG_MODULE_REGISTER(data_bus_consumer, CONFIG_DATA_BUS_LOG_LEVEL);

/** 注销时等待回调退出的上限（毫秒）；超时说明回调异常滞留，放弃注销避免挂死调用方 */
#define DATA_BUS_UNREGISTER_TIMEOUT_MS 1000U

typedef struct {
    data_bus_consumer_t*  consumer; /* 消费者对象指针（固定槽位地址） */
    bool                  active;
    bool                  manual_release;
    data_bus_consume_fn_t callback;
    void*                 user_data;
    uint32_t              generation;
} data_bus_consumer_snap_t;

static bool channel_in_table_locked(const data_bus_channel_t* ch) {
    for (uint32_t i = 0; i < g_channel_count; i++) {
        if (g_channels[i] == ch) {
            return true;
        }
    }

    return false;
}

/* ============================================================================
 * 公共 API: 消费者注册
 * ============================================================================ */

int data_bus_consumer_register(data_bus_channel_t* ch, const data_bus_consumer_cfg_t* cfg,
                               data_bus_consumer_t** out_consumer) {
    int ready = data_bus_require_initialized();

    if (ready != 0) {
        return ready;
    }
    if (ch == NULL || cfg == NULL) {
        return -EINVAL;
    }
    if (cfg->callback == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&g_channels_lock, K_FOREVER);
    if (!channel_in_table_locked(ch)) {
        k_mutex_unlock(&g_channels_lock);
        return -EINVAL;
    }
    if (atomic_get(&g_shutting_down)) {
        k_mutex_unlock(&g_channels_lock);
        return -ESHUTDOWN;
    }

    /* 名字格式化在自旋锁临界区外完成（cfg 为调用方数据，g_channels_lock 下可读），
     * 锁内只做定长 memcpy，缩短关中断时间 */
    char name_scratch[CONFIG_DATA_BUS_CHANNEL_NAME_MAX]; /* 与 consumer->name_storage 同尺寸 */
    bool name_valid = false;
    int  name_len = 0;
    if (cfg->name != NULL) {
        int name_ret = snprintf(name_scratch, sizeof(name_scratch), "%s", cfg->name);
        name_valid = (name_ret >= 0) && ((size_t) name_ret < sizeof(name_scratch));
        if (!name_valid) {
            k_mutex_unlock(&g_channels_lock);
            return -EINVAL;
        }
        name_len = name_ret;
    }

    k_spinlock_key_t key = k_spin_lock(&ch->lock);

    if (!atomic_get(&ch->active)) {
        k_spin_unlock(&ch->lock, key);
        k_mutex_unlock(&g_channels_lock);
        return -ESHUTDOWN;
    }

    if (ch->consumer_count >= CONFIG_DATA_BUS_MAX_CONSUMERS_PER_CHANNEL) {
        /* 通道名先快照，日志输出留到解锁后（ch 在放锁后可能被并发销毁） */
        char full_log_name[CONFIG_DATA_BUS_CHANNEL_NAME_MAX];
        memcpy(full_log_name, ch->name_storage, sizeof(full_log_name));
        k_spin_unlock(&ch->lock, key);
        k_mutex_unlock(&g_channels_lock);
        LOG_ERR("Channel '%s' consumer table full (max=%u)", full_log_name,
                CONFIG_DATA_BUS_MAX_CONSUMERS_PER_CHANNEL);
        return -ENOMEM;
    }

    uint32_t slot = CONFIG_DATA_BUS_MAX_CONSUMERS_PER_CHANNEL;
    for (uint32_t i = 0; i < CONFIG_DATA_BUS_MAX_CONSUMERS_PER_CHANNEL; i++) {
        if (!ch->consumer_slot_in_use[i]) {
            slot = i;
            break;
        }
    }
    if (slot >= CONFIG_DATA_BUS_MAX_CONSUMERS_PER_CHANNEL) {
        k_spin_unlock(&ch->lock, key);
        k_mutex_unlock(&g_channels_lock);
        return -ENOMEM;
    }

    data_bus_consumer_t* consumer = &ch->consumers[slot];

    /* memset 已清零（无名即空串）；按精确长度拷贝避免带入栈上未初始化尾部字节 */
    memset(consumer, 0, sizeof(*consumer));
    if (name_valid) {
        memcpy(consumer->name_storage, name_scratch, (size_t) name_len + 1U);
    }
    consumer->name = consumer->name_storage;
    consumer->channel = ch;
    consumer->manual_release = cfg->manual_release;
    consumer->callback = cfg->callback;
    consumer->user_data = cfg->user_data;
    consumer->last_seq = 0;
    consumer->generation = ++ch->next_consumer_generation;
    atomic_set(&consumer->callback_hold, 0);
    atomic_set(&consumer->active, 1);

    ch->consumer_slot_in_use[slot] = true;
    ch->consumer_count++;

    uint32_t total = ch->consumer_count;
    char     log_consumer[sizeof(consumer->name_storage)];
    char     log_channel[CONFIG_DATA_BUS_CHANNEL_NAME_MAX];

    /* 锁内仅 memcpy 快照字符串（均为 NUL 结尾的内部存储），日志输出留到锁外，缩短临界区 */
    memcpy(log_consumer, consumer->name_storage, sizeof(log_consumer));
    memcpy(log_channel, ch->name_storage, sizeof(log_channel));

    k_spin_unlock(&ch->lock, key);

    if (out_consumer != NULL) {
        *out_consumer = consumer;
    }

    k_mutex_unlock(&g_channels_lock);

    LOG_DBG("Consumer '%s' registered on '%s' (total=%u/%u)", log_consumer, log_channel, total,
            CONFIG_DATA_BUS_MAX_CONSUMERS_PER_CHANNEL);

    return 0;
}

int data_bus_consumer_unregister(data_bus_consumer_t* consumer) {
    if (consumer == NULL) {
        return -EINVAL;
    }
    if (k_current_get() == &g_dispatcher_thread_data) {
        return -EDEADLK;
    }

    k_mutex_lock(&g_channels_lock, K_FOREVER);

    data_bus_channel_t* found_ch = consumer->channel;

    if (found_ch == NULL) {
        k_mutex_unlock(&g_channels_lock);
        return -EINVAL;
    }

    if (!channel_in_table_locked(found_ch)) {
        k_mutex_unlock(&g_channels_lock);
        return -EINVAL;
    }
    if (atomic_get(&g_shutting_down)) {
        k_mutex_unlock(&g_channels_lock);
        return -ESHUTDOWN;
    }
    if (!atomic_get(&found_ch->active)) {
        k_mutex_unlock(&g_channels_lock);
        return -EINVAL;
    }

    k_spinlock_key_t key = k_spin_lock(&found_ch->lock);
    char             log_channel[CONFIG_DATA_BUS_CHANNEL_NAME_MAX];

    /* 锁内仅 memcpy 快照，避免 spinlock 临界区内执行格式化 */
    memcpy(log_channel, found_ch->name_storage, sizeof(log_channel));

    uint32_t found_idx = CONFIG_DATA_BUS_MAX_CONSUMERS_PER_CHANNEL;
    for (uint32_t j = 0; j < CONFIG_DATA_BUS_MAX_CONSUMERS_PER_CHANNEL; j++) {
        if (found_ch->consumer_slot_in_use[j] && &found_ch->consumers[j] == consumer) {
            found_idx = j;
            break;
        }
    }

    if (found_idx >= CONFIG_DATA_BUS_MAX_CONSUMERS_PER_CHANNEL) {
        k_spin_unlock(&found_ch->lock, key);
        k_mutex_unlock(&g_channels_lock);
        LOG_WRN("Consumer unregister failed: not found on channel '%s'", log_channel);
        return -EINVAL;
    }

    char log_name[sizeof(consumer->name_storage)];

    /* 锁内仅 memcpy 快照，避免 spinlock 临界区内执行格式化 */
    memcpy(log_name, consumer->name_storage, sizeof(log_name));

    uint32_t recorded_generation = consumer->generation;
    atomic_set(&consumer->active, 0);
    (void) atomic_inc(&found_ch->lifecycle_hold);
    k_spin_unlock(&found_ch->lock, key);

    /* 释放全局锁后再等待 callback_hold，避免与回调内调用 data_bus_publish() 死锁 */
    k_mutex_unlock(&g_channels_lock);

    /*
     * 有界等待回调退出：无界自旋会在回调长期不返回（回调缺陷）时挂死注销方。
     * 超时放弃本次注销并返回 -EBUSY；消费者已置 active=0，后续派发不再命中，
     * 槽位保持占用，待通道销毁时统一回收（不可对同一 consumer 重试注销）。
     */
    uint32_t waited_ms = 0U;
    while (atomic_get(&consumer->callback_hold) != 0) {
        if (waited_ms >= DATA_BUS_UNREGISTER_TIMEOUT_MS) {
            /* 超时路径无锁递减 lifecycle_hold（原子量），避免与滞留回调争抢通道锁；
             * 递减保证 destroy 等待 lifecycle_hold 归零的路径不被永久阻塞 */
            (void) atomic_dec(&found_ch->lifecycle_hold);
            LOG_WRN("Consumer '%s' unregister timeout: callback busy after %u ms", log_name, waited_ms);
            return -EBUSY;
        }
        k_sleep(K_MSEC(1));
        waited_ms++;
    }

    k_mutex_lock(&g_channels_lock, K_FOREVER);

    if (!channel_in_table_locked(found_ch)) {
        (void) atomic_dec(&found_ch->lifecycle_hold);
        k_mutex_unlock(&g_channels_lock);
        LOG_WRN("Consumer unregister failed: channel removed during wait");
        return -EINVAL;
    }

    key = k_spin_lock(&found_ch->lock);

    if (found_idx >= CONFIG_DATA_BUS_MAX_CONSUMERS_PER_CHANNEL || !found_ch->consumer_slot_in_use[found_idx] ||
        &found_ch->consumers[found_idx] != consumer ||
        found_ch->consumers[found_idx].generation != recorded_generation) {
        (void) atomic_dec(&found_ch->lifecycle_hold);
        k_spin_unlock(&found_ch->lock, key);
        k_mutex_unlock(&g_channels_lock);
        LOG_WRN("Consumer unregister failed: slot changed on channel '%s'", log_channel);
        return -EINVAL;
    }

    consumer->channel = NULL;
    memset(consumer, 0, sizeof(*consumer));
    found_ch->consumer_slot_in_use[found_idx] = false;
    found_ch->consumer_count--;
    (void) atomic_dec(&found_ch->lifecycle_hold);

    uint32_t remain = found_ch->consumer_count;

    k_spin_unlock(&found_ch->lock, key);
    k_mutex_unlock(&g_channels_lock);

    LOG_DBG("Consumer '%s' unregistered from '%s' (remain=%u)", log_name, log_channel, remain);

    return 0;
}

/* ============================================================================
 * 内部：将块分发给所有消费者
 * ============================================================================ */

void data_bus_consumer_dispatch(data_bus_channel_t* ch, data_bus_block_t* block) {
    if (ch == NULL || block == NULL) {
        return;
    }

    data_bus_consumer_snap_t snaps[CONFIG_DATA_BUS_MAX_CONSUMERS_PER_CHANNEL];
    uint32_t                 snap_count = 0;

    k_spinlock_key_t key = k_spin_lock(&ch->lock);
    for (uint32_t i = 0; i < CONFIG_DATA_BUS_MAX_CONSUMERS_PER_CHANNEL; i++) {
        if (!ch->consumer_slot_in_use[i]) {
            continue;
        }

        data_bus_consumer_t* c = &ch->consumers[i];

        snaps[snap_count].consumer = c;
        snaps[snap_count].active = atomic_get(&c->active);
        snaps[snap_count].manual_release = c->manual_release;
        snaps[snap_count].callback = c->callback;
        snaps[snap_count].user_data = c->user_data;
        snaps[snap_count].generation = c->generation;
        snap_count++;
    }
    k_spin_unlock(&ch->lock, key);

    /* 统计活跃消费者以拆分引用 */
    uint32_t active_count = 0;
    for (uint32_t i = 0; i < snap_count; i++) {
        if (snaps[i].active && snaps[i].callback != NULL) {
            active_count++;
        }
    }

    if (active_count == 0) {
        return;
    }

    /*
     * 拆分 bus 引用：ref_count 原为 1（bus 持有）。
     * 增加 active_count 使总引用数 = 1 + active_count。
     * 每个消费者获得一份隐式引用。
     */
    LOG_DBG("Dispatch ch='%s' seq=%u ref+%u=%d", ch->name, block->seq, active_count,
            (int) atomic_get(&block->ref_count));

    atomic_add(&block->ref_count, active_count);

    for (uint32_t i = 0; i < snap_count; i++) {
        if (!snaps[i].active || snaps[i].callback == NULL) {
            continue;
        }

        data_bus_consume_fn_t callback = NULL;
        void*                 user_data = NULL;
        bool                  manual_release = false;
        bool                  hold_acquired = false;

        k_spinlock_key_t     lk = k_spin_lock(&ch->lock);
        data_bus_consumer_t* target = snaps[i].consumer;
        if (target != NULL && atomic_get(&target->active) && target->generation == snaps[i].generation) {
            (void) atomic_inc(&target->callback_hold);
            hold_acquired = true;
            callback = target->callback;
            user_data = target->user_data;
            manual_release = target->manual_release;
        }
        k_spin_unlock(&ch->lock, lk);

        if (callback == NULL) {
            data_bus_block_release(block);
            continue;
        }

        uint32_t block_seq = block->seq;

        LOG_DBG("  -> consumer manual_release=%d", manual_release);

        callback(ch, block, user_data);

        /* 框架自动释放隐式引用，除非消费者选择手动释放 */
        if (!manual_release) {
            data_bus_block_release(block);
        }

        lk = k_spin_lock(&ch->lock);
        target = snaps[i].consumer;
        if (target != NULL && atomic_get(&target->active) && target->generation == snaps[i].generation) {
            target->last_seq = block_seq;
        }
        if (hold_acquired) {
            (void) atomic_dec(&snaps[i].consumer->callback_hold);
        }
        k_spin_unlock(&ch->lock, lk);
    }
}
