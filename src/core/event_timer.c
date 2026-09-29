/**
 * @file event_timer.c
 * @brief 延迟发布与周期发布（事件定时器池）实现（CONFIG_EVENT_TIMER_POOL）
 *
 * 静态 k_timer 槽池承载两类定时投递：延迟发布（负载深拷贝入槽，到期回调以
 * 槽内事件壳零分配入队）与周期发布（母本入槽，每轮到期经纯 slab 路径生成
 * 全新副本后入队）。不新增线程：到期回调运行于系统时钟 ISR 上下文。
 *
 * 并发契约：
 * - 池状态由 g_timer_pool_lock 单一保护（TABLE 层级），仅线程侧提交/取消/
 *   诊断持锁；到期回调（ISR）不可持锁，仅以槽状态机原子 CAS + 世代号仲裁：
 *   CAS(ARMED→FIRING) 成功即取得本槽投递权，取消方 CAS 失败即让位（返回
 *   NOT_FOUND 不干扰）。
 * - 槽回收统一走 gen 递增：一次性槽由到期回调自回收，取消/shutdown 由线程
 *   侧回收；gen 写入方只有「当前持有该槽（ARMED 或 FIRING）的一方」。
 * - SMP 下「读 gen → CAS → 复核 gen」防槽取消后立即复用导致的 ABA 误投；
 *   单核到期回调与线程代码不会交错，该窗口天然不存在。
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-09-29
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-09-27       1.0            zeh            初始版本（B3：延迟发布 + 周期发布共池交付）
 * 2026-09-27       1.1            zeh            零长负载提交清槽内残留 len（M-1，含领槽防御）；cancel 等在飞超时改返回
 * TIMEOUT 2026-09-29       1.2            zeh            k_timer 只在池初始化时接线一次，领槽不再重入 init
 *
 */

#include "event_timer.h"
#include <zephyr/logging/log.h>
#include <string.h>
#include <zeplod/event_timer.h>
#include "event_memory.h"
#include "event_system_internal.h"

#if defined(CONFIG_EVENT_TIMER_POOL)

LOG_MODULE_DECLARE(event_system, CONFIG_SYS_LOG_LEVEL);

/** 负载长度上界（字节）：与最大数据 slab 块（4KB）对齐，保证到期投递零 k_malloc */
#define EVENT_TIMER_PAYLOAD_MAX       4096U

/** delay_ms / initial_delay_ms 上界（毫秒）：为内部换算保留符号位 */
#define EVENT_TIMER_DELAY_MAX_MS      0x7FFFFFFFU

/** 世代号保留值（0xFFFF），不参与分配 */
#define EVENT_TIMER_GEN_INVALID       0xFFFFU

/** cancel_all 等待在飞到期回调的迭代上限（每次 k_busy_wait(50) 微秒） */
#define EVENT_TIMER_FIRING_WAIT_LOOPS 2000U

/** 静态槽池：延迟与周期共享（静态约 130B/槽，32 位目标） */
static event_timer_slot_t g_timer_slots[CONFIG_EVENT_TIMER_SLOTS];

/** 槽池互斥锁（TABLE 层级；单锁不嵌套，到期回调不持锁） */
static K_MUTEX_DEFINE(g_timer_pool_lock);

/** 全局投递失败累计（自上次 shutdown 起累计；原子读，ISR 安全） */
static atomic_t g_timer_miss_total;

/** k_timer 只初始化一次。到期回调返回前不能再 k_timer_init 同一对象 */
static bool g_timer_pool_inited;

/* =============================================================================
 * 内部辅助
 * ============================================================================= */

static void timer_lock_acquire(void) {
    zepl_lock_enter(ZEP_LOCK_LEVEL_TABLE, (uintptr_t) &g_timer_pool_lock);
    k_mutex_lock(&g_timer_pool_lock, K_FOREVER);
}

static void timer_lock_release(void) {
    k_mutex_unlock(&g_timer_pool_lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_TABLE, (uintptr_t) &g_timer_pool_lock);
}

/** @brief 记一次投递失败：槽计数 + 全局计数（到期回调/取消路径共用，无日志） */
static void timer_count_miss(event_timer_slot_t* slot) {
    atomic_inc(&slot->miss_count);
    (void) atomic_inc(&g_timer_miss_total);
}

#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE
/** @brief 数据 slab 池指针映射到 EVENT_FLAG_SLAB_* 档位标记 */
static uint8_t timer_slab_flag(struct k_mem_slab* slab) {
    if (slab == NULL) {
        return 0U;
    }
#if EVENT_SLAB_256_AVAILABLE
    if (slab == &event_slab_data_256) {
        return EVENT_FLAG_SLAB_256;
    }
#endif
#if EVENT_SLAB_1K_AVAILABLE
    if (slab == &event_slab_data_1k) {
        return EVENT_FLAG_SLAB_1K;
    }
#endif
#if EVENT_SLAB_4K_AVAILABLE
    if (slab == &event_slab_data_4k) {
        return EVENT_FLAG_SLAB_4K;
    }
#endif
    return 0U;
}
#endif /* EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE */

/**
 * @brief 释放负载私有副本（provenance 记录在 shell.flags，与 event_free_data 同规则）
 *
 * 可在到期回调（ISR）上下文调用：k_mem_slab_free / k_free 均为 ISR 安全。
 */
static void timer_payload_free(void* payload, uint8_t flags) {
    if (payload == NULL) {
        return;
    }
    event_debug_untrack_alloc(payload);
#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE
    if (flags & EVENT_FLAG_DATA_FROM_SLAB) {
        struct k_mem_slab* slab = event_memory_data_slab_from_flag(flags & EVENT_FLAG_SLAB_MASK);

        if (slab == NULL) {
            slab = event_memory_resolve_data_slab_for_ptr(payload);
        }
        if (slab != NULL) {
            k_mem_slab_free(slab, payload);
            return;
        }
        if (!k_is_in_isr()) {
            LOG_ERR("event_timer: cannot resolve slab for payload %p; memory may leak", payload);
        }
        return;
    }
#endif
    k_free(payload);
}

/**
 * @brief 为超内联负载分配私有副本：优先数据 slab，失败回退 k_malloc
 *
 * 仅提交时刻（线程上下文）调用；到期投递路径不再分配。
 *
 * @return 副本指针（*flags_out 含 provenance 标记），分配失败返回 NULL
 */
static void* timer_payload_alloc(const void* data, uint32_t len, uint8_t* flags_out) {
    void*   copy = NULL;
    uint8_t flags = EVENT_FLAG_DATA_DYNAMIC;

#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE
    {
        struct k_mem_slab* slab = event_memory_select_data_slab(len);
        uint8_t            slab_flag = timer_slab_flag(slab);

        if (slab_flag != 0U && k_mem_slab_alloc(slab, &copy, K_NO_WAIT) == 0) {
            flags |= EVENT_FLAG_DATA_FROM_SLAB | slab_flag;
        } else {
            copy = NULL;
        }
    }
#endif
    if (copy == NULL) {
        copy = k_malloc(len);
        if (copy == NULL) {
            return NULL;
        }
        event_memory_inc_fallback_count();
    }

    memcpy(copy, data, len);
    *flags_out = flags;
    event_debug_track_alloc(copy, len, EVENT_PRIORITY_NORMAL);
    return copy;
}

/** @brief 释放槽内负载指针并清空（按 shell.flags 记录的来源） */
static void timer_release_payload(event_timer_slot_t* slot) {
    if (slot->payload == NULL) {
        return;
    }
    timer_payload_free(slot->payload, slot->shell.flags);
    slot->payload = NULL;
}

/** @brief 槽回收：世代号递增（跳过保留值）并原子置 FREE；调用方须已持有该槽投递权 */
static void timer_slot_recycle(event_timer_slot_t* slot) {
    uint16_t next = (uint16_t) (slot->gen + 1U);

    if (next == EVENT_TIMER_GEN_INVALID) {
        next = 0U;
    }
    slot->gen = next;
    atomic_set(&slot->state, EVENT_TIMER_SLOT_FREE);
}

/** @brief 清空槽内计数（提交领槽时复位，避免上一任使用者的统计串入新句柄） */
static void timer_slot_reset_stats(event_timer_slot_t* slot) {
    atomic_set(&slot->fire_count, 0);
    atomic_set(&slot->miss_count, 0);
}

/* =============================================================================
 * 到期回调（系统时钟 ISR 上下文，禁止持锁/日志/阻塞）
 * ============================================================================= */

/** @brief 周期轮投递：母本经纯 slab 路径生成全新副本后入队；失败轮次计 miss */
static void timer_expiry_periodic(event_timer_slot_t* slot) {
    const void* src = (slot->payload != NULL) ? slot->payload : slot->shell.data.inline_data;

    event_t* copy =
        event_create_with_data_rt(slot->shell.type, (event_priority_t) slot->shell.priority, src, slot->len);
    if (copy == NULL) {
        /* slab 耗尽：本轮跳过计 miss，不影响后续轮次 */
        timer_count_miss(slot);
        return;
    }
    if (event_publish_from_isr(copy) == EVENT_OK) {
        atomic_inc(&slot->fire_count);
    } else {
        timer_count_miss(slot);
    }
    /* 与 event_publish_copy_rt 同模式：成功入队后动态负载标记已被清除，
     * event_free 仅归还事件壳（负载随队列副本由分发器释放）；
     * 失败时 flags 完整，壳与负载一并释放 */
    event_free(copy);
}

/** @brief 一次性投递：槽内事件壳零分配入队；无论成败槽随即回收 */
static void timer_expiry_oneshot(event_timer_slot_t* slot) {
    bool published = (event_publish_from_isr(&slot->shell) == EVENT_OK);

    if (published) {
        atomic_inc(&slot->fire_count);
        /* 成功后 shell 的动态负载标记已被清除：所有权随队列副本转移，槽不再释放 */
        slot->payload = NULL;
    } else {
        /* 队列满/未运行/类型失效：负载未转移，就地释放后计 miss（尽力而为，不重试） */
        timer_release_payload(slot);
        timer_count_miss(slot);
    }
    timer_slot_recycle(slot);
}

/** @brief k_timer 到期回调：gen 仲裁 → CAS 取投递权 → 按模式投递 */
static void event_timer_expiry_fn(struct k_timer* timer) {
    event_timer_slot_t* slot = CONTAINER_OF(timer, event_timer_slot_t, timer);

    /* 先读 gen 再 CAS：两步之间槽可能已被取消并复用（SMP 窗口），CAS 成功后复核 */
    uint16_t gen_seen = slot->gen;

    if (!atomic_cas(&slot->state, EVENT_TIMER_SLOT_ARMED, EVENT_TIMER_SLOT_FIRING)) {
        return; /* 已被取消/回收：陈旧到期，放弃本次 */
    }
    if (slot->gen != gen_seen) {
        /* ABA：读到旧 gen 后槽已被取消并重新武装，恢复现场交还新主人 */
        (void) atomic_cas(&slot->state, EVENT_TIMER_SLOT_FIRING, EVENT_TIMER_SLOT_ARMED);
        return;
    }

    slot->shell.timestamp = k_uptime_get_32();

    if (slot->mode == EVENT_TIMER_MODE_PERIODIC) {
        timer_expiry_periodic(slot);
        /* 周期槽保持 ARMED：k_timer 已按 period 自动重武装，等待 cancel/shutdown 回收 */
        (void) atomic_cas(&slot->state, EVENT_TIMER_SLOT_FIRING, EVENT_TIMER_SLOT_ARMED);
        return;
    }
    timer_expiry_oneshot(slot);
}

/* =============================================================================
 * 提交路径（线程上下文）
 * ============================================================================= */

/** @brief 提交前置门控：拒绝 ISR 与未初始化/未运行状态（失败已执行 op_exit） */
static event_status_t timer_submit_gate(void) {
    if (k_is_in_isr()) {
        return EVENT_ERR_INVALID_ARG;
    }
    if (!event_system_op_enter()) {
        /* 生命周期切换中（stop/shutdown 正排空在途操作）：按未运行拒绝 */
        return EVENT_ERR_NOT_RUNNING;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC || !g_event_system.initialized) {
        event_system_op_exit();
        return EVENT_ERR_INVALID_ARG;
    }
    if (atomic_get(&g_event_system.running) == 0) {
        event_system_op_exit();
        return EVENT_ERR_NOT_RUNNING;
    }
    return EVENT_OK;
}

/** @brief 负载参数校验：data/len 配对与 4KB 上界（上界保证到期投递零 k_malloc） */
static bool timer_payload_args_valid(const void* data, size_t data_len) {
    if (data == NULL && data_len != 0U) {
        return false;
    }
    if (data_len > EVENT_TIMER_PAYLOAD_MAX) {
        return false;
    }
    return true;
}

/** @brief 池锁内领取空闲槽（CAS FREE→ARMED 独占并接线到期回调；无空闲返回 -1） */
static int32_t timer_claim_slot(void) {
    for (uint32_t i = 0; i < CONFIG_EVENT_TIMER_SLOTS; i++) {
        event_timer_slot_t* slot = &g_timer_slots[i];

        if (atomic_cas(&slot->state, EVENT_TIMER_SLOT_FREE, EVENT_TIMER_SLOT_ARMED)) {
            timer_slot_reset_stats(slot);
            /* M-1 防御：领槽即清残留长度，任何 fill 路径遗漏都不会外溢成错误投递长度 */
            slot->len = 0U;
            return (int32_t) i;
        }
    }
    return -1;
}

/** @brief 复位事件壳元数据（负载由调用方按长度填充） */
static void timer_build_shell(event_timer_slot_t* slot, event_type_t type, event_priority_t priority) {
    memset(&slot->shell, 0, sizeof(slot->shell));
    slot->shell.type = type;
    slot->shell.priority = (uint8_t) priority;
    slot->shell.timestamp = k_uptime_get_32();
}

/**
 * @brief 填充槽内负载：内联直存 / 超内联深拷贝（slab 优先，k_malloc 回退）
 *
 * @return EVENT_OK 成功，EVENT_ERR_NO_MEM 拷贝分配失败（失败时槽由调用方回收）
 */
static event_status_t timer_fill_payload(event_timer_slot_t* slot, const void* data, size_t data_len) {
    slot->shell.data_len = (uint32_t) data_len;
    if (data_len == 0U) {
        slot->shell.flags = 0U;
        /* M-1：槽为复用资源，len 必须随本次提交重定义，不得残留上一任的长度
         * （否则零长周期发布每轮按残留 len 投递，读越界 garbage） */
        slot->len = 0U;
        return EVENT_OK;
    }
    if (data_len <= CONFIG_EVENT_INLINE_DATA_SIZE) {
        slot->shell.flags = EVENT_FLAG_DATA_INLINE;
        memcpy(slot->shell.data.inline_data, data, data_len);
        slot->len = (uint32_t) data_len;
        return EVENT_OK;
    }

    uint8_t flags = 0U;
    void*   copy = timer_payload_alloc(data, (uint32_t) data_len, &flags);

    if (copy == NULL) {
        return EVENT_ERR_NO_MEM;
    }
    slot->payload = copy;
    slot->len = (uint32_t) data_len;
    slot->shell.flags = flags;
    slot->shell.data.ptr = copy;
    return EVENT_OK;
}

/* =============================================================================
 * 公共 API：延迟发布
 * ============================================================================= */

event_status_t event_publish_delayed_copy(event_type_t type, event_priority_t priority, const void* data,
                                          size_t data_len, uint32_t delay_ms, event_timer_handle_t* handle_out) {
    if (delay_ms == 0U) {
        delay_ms = 1U; /* 0 按 1 处理：k_timer 不接受 0 时长 */
    }
    if (delay_ms > EVENT_TIMER_DELAY_MAX_MS) {
        return EVENT_ERR_INVALID_ARG;
    }

    event_status_t ret = timer_submit_gate();

    if (ret != EVENT_OK) {
        return ret;
    }
    if ((uint32_t) type > MAX_EVENT_TYPE_ID || !timer_payload_args_valid(data, data_len)) {
        event_system_op_exit();
        return EVENT_ERR_INVALID_ARG;
    }
    if (!event_system_type_is_registered(type)) {
        event_system_op_exit();
        return EVENT_ERR_NOT_FOUND;
    }

    timer_lock_acquire();
    int32_t idx = timer_claim_slot();

    if (idx < 0) {
        timer_lock_release();
        event_system_op_exit();
        LOG_WRN("event_timer: no free slot (pool=%u)", (unsigned int) CONFIG_EVENT_TIMER_SLOTS);
        return EVENT_ERR_NO_MEM;
    }

    event_timer_slot_t* slot = &g_timer_slots[idx];

    timer_build_shell(slot, type, priority);
    slot->payload = NULL;
    slot->mode = EVENT_TIMER_MODE_ONESHOT;
    slot->period_ms = 0U;
    ret = timer_fill_payload(slot, data, data_len);
    if (ret != EVENT_OK) {
        timer_slot_recycle(slot); /* 拷贝失败：归还槽位 */
        timer_lock_release();
        event_system_op_exit();
        return ret;
    }

    k_timer_start(&slot->timer, K_MSEC((uint32_t) delay_ms), K_NO_WAIT);
    if (handle_out != NULL) {
        handle_out->slot = (uint16_t) idx;
        handle_out->gen = slot->gen;
    }
    timer_lock_release();
    event_system_op_exit();
    return EVENT_OK;
}

/* =============================================================================
 * 公共 API：取消（延迟/周期共用一套句柄语义）
 * ============================================================================= */

/** @brief 取消公共主体：gen 仲裁 → 停定时器 → 等在飞回调让位 → 回收槽 */
static event_status_t timer_cancel_common(event_timer_handle_t handle) {
    if (k_is_in_isr() || handle.slot >= (uint16_t) CONFIG_EVENT_TIMER_SLOTS || handle.gen == EVENT_TIMER_GEN_INVALID) {
        return EVENT_ERR_INVALID_ARG;
    }
    if (!event_system_op_enter()) {
        return EVENT_ERR_NOT_RUNNING;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC || !g_event_system.initialized) {
        event_system_op_exit();
        return EVENT_ERR_INVALID_ARG;
    }

    event_timer_slot_t* slot = &g_timer_slots[handle.slot];

    timer_lock_acquire();
    if (slot->gen != handle.gen || atomic_get(&slot->state) == EVENT_TIMER_SLOT_FREE) {
        timer_lock_release();
        event_system_op_exit();
        return EVENT_ERR_NOT_FOUND;
    }

    k_timer_stop(&slot->timer);

    /* 在飞到期回调（SMP 窗口）持有 FIRING 期间不干扰，等其自行收尾后再仲裁 */
    uint32_t guard = 0U;

    while (atomic_get(&slot->state) == EVENT_TIMER_SLOT_FIRING && guard < EVENT_TIMER_FIRING_WAIT_LOOPS) {
        k_busy_wait(50);
        guard++;
    }

    if (atomic_get(&slot->state) == EVENT_TIMER_SLOT_FIRING) {
        /* 护栏耗尽仍在飞（SMP 长尾）：让位并按超时报告——语义是「到期回调
         * 未收尾」，与「已取消/已消费」的 NOT_FOUND 区分（m-6）；
         * 负载由到期回调自行收尾，此处不得触碰 */
        LOG_WRN("event_timer: cancel timed out waiting in-flight expiry (slot=%u gen=%u)", (unsigned int) handle.slot,
                (unsigned int) handle.gen);
        timer_lock_release();
        event_system_op_exit();
        return EVENT_ERR_TIMEOUT;
    }

    if (!atomic_cas(&slot->state, EVENT_TIMER_SLOT_ARMED, EVENT_TIMER_SLOT_FREE)) {
        /* 到期回调已通过校验（一次性槽已消费回收）：让位，按未找到处理 */
        timer_lock_release();
        event_system_op_exit();
        return EVENT_ERR_NOT_FOUND;
    }

    timer_release_payload(slot); /* 一次性待投递副本 / 周期母本 */
    slot->len = 0U;
    slot->period_ms = 0U;
    slot->mode = EVENT_TIMER_MODE_ONESHOT;
    timer_slot_recycle(slot);
    timer_lock_release();
    event_system_op_exit();
    return EVENT_OK;
}

event_status_t event_delayed_cancel(event_timer_handle_t handle) {
    return timer_cancel_common(handle);
}

#if defined(CONFIG_EVENT_PERIODIC_PUBLISH)

event_status_t event_periodic_cancel(event_timer_handle_t handle) {
    return timer_cancel_common(handle);
}

uint32_t event_periodic_active_count(void) {
    uint32_t count = 0U;

    timer_lock_acquire();
    for (uint32_t i = 0; i < CONFIG_EVENT_TIMER_SLOTS; i++) {
        const event_timer_slot_t* slot = &g_timer_slots[i];

        if (slot->mode == EVENT_TIMER_MODE_PERIODIC && atomic_get(&slot->state) != EVENT_TIMER_SLOT_FREE) {
            count++;
        }
    }
    timer_lock_release();
    return count;
}

#endif /* CONFIG_EVENT_PERIODIC_PUBLISH */

/* =============================================================================
 * 公共 API：周期发布（须 CONFIG_EVENT_PERIODIC_PUBLISH）
 * ============================================================================= */

#if defined(CONFIG_EVENT_PERIODIC_PUBLISH)

event_status_t event_periodic_start(event_type_t type, event_priority_t priority, const void* data, size_t data_len,
                                    uint32_t period_ms, uint32_t initial_delay_ms, event_timer_handle_t* handle_out) {
    if (period_ms == 0U || period_ms > EVENT_TIMER_DELAY_MAX_MS || initial_delay_ms > EVENT_TIMER_DELAY_MAX_MS) {
        return EVENT_ERR_INVALID_ARG;
    }

    event_status_t ret = timer_submit_gate();

    if (ret != EVENT_OK) {
        return ret;
    }
    if ((uint32_t) type > MAX_EVENT_TYPE_ID || !timer_payload_args_valid(data, data_len)) {
        event_system_op_exit();
        return EVENT_ERR_INVALID_ARG;
    }
    if (!event_system_type_is_registered(type)) {
        event_system_op_exit();
        return EVENT_ERR_NOT_FOUND;
    }

    timer_lock_acquire();
    int32_t idx = timer_claim_slot();

    if (idx < 0) {
        timer_lock_release();
        event_system_op_exit();
        LOG_WRN("event_timer: no free slot (pool=%u)", (unsigned int) CONFIG_EVENT_TIMER_SLOTS);
        return EVENT_ERR_NO_MEM;
    }

    event_timer_slot_t* slot = &g_timer_slots[idx];

    timer_build_shell(slot, type, priority);
    slot->payload = NULL;
    slot->mode = EVENT_TIMER_MODE_PERIODIC;
    slot->period_ms = period_ms;
    ret = timer_fill_payload(slot, data, data_len);
    if (ret != EVENT_OK) {
        timer_slot_recycle(slot);
        timer_lock_release();
        event_system_op_exit();
        return ret;
    }

    /* initial_delay_ms=0 表示首投在 period_ms 时刻（k_timer 原生语义） */
    uint32_t first_delay_ms = (initial_delay_ms != 0U) ? initial_delay_ms : period_ms;

    k_timer_start(&slot->timer, K_MSEC(first_delay_ms), K_MSEC(period_ms));
    if (handle_out != NULL) {
        handle_out->slot = (uint16_t) idx;
        handle_out->gen = slot->gen;
    }
    timer_lock_release();
    event_system_op_exit();
    return EVENT_OK;
}

#endif /* CONFIG_EVENT_PERIODIC_PUBLISH */

/* =============================================================================
 * 公共 API：诊断
 * ============================================================================= */

uint32_t event_timer_pending_count(void) {
    uint32_t count = 0U;

    timer_lock_acquire();
    for (uint32_t i = 0; i < CONFIG_EVENT_TIMER_SLOTS; i++) {
        if (atomic_get(&g_timer_slots[i].state) == EVENT_TIMER_SLOT_ARMED) {
            count++;
        }
    }
    timer_lock_release();
    return count;
}

uint32_t event_timer_miss_count(void) {
    return (uint32_t) atomic_get(&g_timer_miss_total);
}

event_status_t event_timer_get_stats(event_timer_handle_t handle, uint32_t* fire_count, uint32_t* miss_count) {
    if (handle.slot >= (uint16_t) CONFIG_EVENT_TIMER_SLOTS || handle.gen == EVENT_TIMER_GEN_INVALID) {
        return EVENT_ERR_INVALID_ARG;
    }

    event_timer_slot_t* slot = &g_timer_slots[handle.slot];

    timer_lock_acquire();
    if (slot->gen != handle.gen || atomic_get(&slot->state) == EVENT_TIMER_SLOT_FREE) {
        timer_lock_release();
        return EVENT_ERR_NOT_FOUND;
    }
    if (fire_count != NULL) {
        *fire_count = (uint32_t) atomic_get(&slot->fire_count);
    }
    if (miss_count != NULL) {
        *miss_count = (uint32_t) atomic_get(&slot->miss_count);
    }
    timer_lock_release();
    return EVENT_OK;
}

/* =============================================================================
 * 生命周期收尾（event_system_shutdown 调用，线程上下文）
 * ============================================================================= */

void event_timer_pool_init(void) {
    if (g_timer_pool_inited) {
        return;
    }
    for (uint32_t i = 0; i < CONFIG_EVENT_TIMER_SLOTS; i++) {
        event_timer_slot_t* slot = &g_timer_slots[i];

        k_timer_init(&slot->timer, event_timer_expiry_fn, NULL);
        atomic_set(&slot->state, EVENT_TIMER_SLOT_FREE);
        slot->gen = 0U;
        slot->len = 0U;
        slot->period_ms = 0U;
        slot->mode = EVENT_TIMER_MODE_ONESHOT;
        slot->payload = NULL;
    }
    g_timer_pool_inited = true;
}

void event_timer_pool_cancel_all(void) {
    timer_lock_acquire();
    for (uint32_t i = 0; i < CONFIG_EVENT_TIMER_SLOTS; i++) {
        event_timer_slot_t* slot = &g_timer_slots[i];

        k_timer_stop(&slot->timer);

        /* 等待在飞到期回调退出（SMP 窗口）；回调收尾不持池锁，此处必然可观测 */
        uint32_t guard = 0U;

        while (atomic_get(&slot->state) == EVENT_TIMER_SLOT_FIRING && guard < EVENT_TIMER_FIRING_WAIT_LOOPS) {
            k_busy_wait(50);
            guard++;
        }

        if (atomic_cas(&slot->state, EVENT_TIMER_SLOT_ARMED, EVENT_TIMER_SLOT_FREE)) {
            timer_release_payload(slot); /* 一次性待投递副本 / 周期母本 */
            slot->len = 0U;
            slot->period_ms = 0U;
            slot->mode = EVENT_TIMER_MODE_ONESHOT;
            timer_slot_recycle(slot);
        }
    }
    atomic_set(&g_timer_miss_total, 0);
    timer_lock_release();
}

#endif /* CONFIG_EVENT_TIMER_POOL */
