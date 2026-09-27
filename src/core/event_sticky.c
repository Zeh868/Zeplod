/**
 * @file event_sticky.c
 * @brief Sticky 事件锁存与订阅重放（CONFIG_EVENT_STICKY）
 *
 * 对显式启用（event_sticky_enable）的事件类型锁存最近一条线程侧发布沿：
 * 发布路径在入队前（发布者仍持负载所有权时）拷贝私有副本并拍沿快照，
 * 入队成功后把副本提交进静态槽池；新订阅者订阅成功后在订阅线程
 * 上下文同步重放一次该沿。ISR 发布不更新锁存沿；锁存为 best-effort，
 * 拷贝失败不影响发布结果，仅计 miss。
 *
 * 并发契约：
 * - 锁存采用两段式（修复入队后拷贝的 use-after-free）：
 *   stage 在 event_queue_enqueue 之前执行——发布者仍持原负载所有权，
 *   此刻完成私有副本分配与沿快照拷贝；commit 在入队成功后把副本提交
 *   进槽（释放槽内旧沿）；discard 在入队失败时丢弃副本（发布本就失败，
 *   不计 sticky miss）。副本所有权流转：stage 分配 → commit 移交槽内
 *   或 discard 释放，任一路径均不悬空。
 * - 槽池状态由 g_sticky_lock 单一保护（TABLE 层级）。切换槽位与类型索引时，
 *   固定按 sticky 锁（TABLE）→ entry 锁（ENTRY）顺序持有，保证映射原子一致。
 * - stage 与 commit 之间槽可能被 disable（窄窗竞态）：commit 按 entry 当前
 *   索引重新解析槽，槽已易主则丢弃副本（允许该窗口内 best-effort 丢失
 *   一次锁存，不计 miss）；同类型并发发布的 commit 顺序理论上可与入队
 *   顺序交错，最坏槽内沿为较早入队的一条，属 best-effort 允许范围。
 * - 重放回调在锁外执行：锁内拍快照并递增 replay_readers，回调期间
 *   stage/commit/clear/disable 遇 readers>0 时计 miss 或推迟收尾（pending），
 *   回调返回后才释放旧负载指针，保证回调读到的 payload 恒有效。
 * - 每线程重放深度计数防御回调内重入订阅导致的递归重放。
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-09-27
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-09-27       1.0            zeh            初始版本（B2：sticky 锁存/重放/统计/生命周期收尾）
 * 2026-09-27       1.1            zeh            锁存改两段式：入队前拷贝副本修 use-after-free（C-1）；gate 关闭返回 NOT_RUNNING
 *
 */

#include <zephyr/logging/log.h>
#include <string.h>
#include "event_memory.h"
#include "event_system_internal.h"

LOG_MODULE_DECLARE(event_system, CONFIG_SYS_LOG_LEVEL);

/** 锁存负载长度上界（字节）：与最大数据 slab 块（4KB）对齐，超出拒锁存计 miss */
#define EVENT_STICKY_PAYLOAD_MAX 4096U

/** 槽空闲标记（事件类型 ID 0-255 均合法，故以 0xFF 表示空闲） */
#define EVENT_STICKY_OWNER_NONE 0xFFU

/** 重放深度跟踪表容量：并发处于重放回调中的线程数上界，表满时防御退化为放行 */
#define EVENT_STICKY_REPLAY_TRACKS 8

/** 重放排空后的收尾动作（仅 sticky 锁内读写；DISABLE 优先级高于 CLEAR，不降级） */
typedef enum {
    EVENT_STICKY_PENDING_NONE = 0, /**< 无待收尾动作 */
    EVENT_STICKY_PENDING_CLEAR,    /**< 重放排空后仅清锁存沿，保留槽与能力 */
    EVENT_STICKY_PENDING_DISABLE,  /**< 重放排空后释放负载并归还槽位 */
} sticky_pending_t;

/**
 * @brief sticky 槽：锁存沿的静态壳 + 私有负载副本
 *
 * shell 持元数据与内联字节；超内联负载由 payload 指针单独持有，
 * provenance 记录在 shell.flags（DYNAMIC/FROM_SLAB/SLAB 档位），
 * 释放路径与 event_free_data 同规则。除 latched/owner_type 被
 * is_latched 无锁读外，其余字段仅在持有 g_sticky_lock 时读写。
 */
typedef struct {
    event_t          shell;          /**< 锁存沿元数据壳（超内联负载时 data.ptr 以 payload 为准） */
    void*            payload;        /**< 超内联负载私有副本；NULL=内联于 shell 或空负载 */
    uint32_t         len;            /**< 锁存负载长度（字节） */
    uint8_t          owner_type;     /**< 持有本槽的事件类型；EVENT_STICKY_OWNER_NONE=槽空闲 */
    bool             latched;        /**< true=当前持有有效锁存沿（is_latched 无锁读） */
    uint32_t         replay_readers; /**< 正以本槽快照执行回调的重放者数 */
    sticky_pending_t pending;        /**< 重放排空后的收尾动作 */
} sticky_slot_t;

/** 静态槽池：静态约 sizeof(sticky_slot_t)（约 88B/槽，64 位） */
static sticky_slot_t g_sticky_slots[CONFIG_EVENT_STICKY_MAX_TYPES];

/** 槽池互斥锁（TABLE 层级；嵌套时先于 ENTRY 层级获取） */
static K_MUTEX_DEFINE(g_sticky_lock);

/** 锁存失败（拷贝失败/超上界/重放让位）累计计数 */
static atomic_t g_sticky_miss_count;

/** 每线程重放深度跟踪（防回调内重入订阅递归）；自线程写自身槽，k_spinlock 保护扫描 */
static struct {
    k_tid_t  thread;
    uint32_t depth;
} g_sticky_replay_tracks[EVENT_STICKY_REPLAY_TRACKS];

static struct k_spinlock g_sticky_track_lock;

/* =============================================================================
 * 内部辅助
 * ============================================================================= */

static void sticky_lock_acquire(void) {
    zepl_lock_enter(ZEP_LOCK_LEVEL_TABLE, (uintptr_t) &g_sticky_lock);
    k_mutex_lock(&g_sticky_lock, K_FOREVER);
}

static void sticky_lock_release(void) {
    k_mutex_unlock(&g_sticky_lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_TABLE, (uintptr_t) &g_sticky_lock);
}

/** @brief 记一次锁存失败（miss 计数 + 警告日志；须持 sticky 锁或处于无害竞争路径） */
static void sticky_count_miss(const char* reason, event_type_t type, uint32_t len) {
    atomic_inc(&g_sticky_miss_count);
    LOG_WRN("sticky latch miss: type=%u len=%u reason=%s", (unsigned int) type, (unsigned int) len, reason);
}

/** @brief 校验 entry 索引并确认槽仍归 type 所有，失效返回 NULL */
static sticky_slot_t* sticky_slot_resolve(const event_type_entry_t* entry, event_type_t type) {
    uint8_t idx = entry->sticky_slot;

    if (idx >= CONFIG_EVENT_STICKY_MAX_TYPES) {
        return NULL;
    }
    sticky_slot_t* slot = &g_sticky_slots[idx];

    if (slot->owner_type != type) {
        return NULL;
    }
    return slot;
}

/** @brief 释放负载私有副本（provenance 记录在 shell.flags，与 event_free_data 同规则） */
static void sticky_payload_free(void* payload, uint8_t flags) {
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
        LOG_ERR("sticky: cannot resolve slab for payload %p; memory may leak", payload);
        return;
    }
#endif
    k_free(payload);
}

/** @brief 槽内锁存负载的释放与沿复位（保留槽位与 owner） */
static void sticky_slot_drop_edge(sticky_slot_t* slot) {
    uint8_t flags = slot->shell.flags;

    sticky_payload_free(slot->payload, flags);
    slot->payload = NULL;
    slot->len = 0U;
    slot->latched = false;
}

/** @brief 槽整体复位为空闲（含 owner；仅 shutdown 收尾或 disable 收尾路径调用） */
static void sticky_slot_reset(sticky_slot_t* slot) {
    sticky_slot_drop_edge(slot);
    slot->owner_type = EVENT_STICKY_OWNER_NONE;
    slot->replay_readers = 0U;
    slot->pending = EVENT_STICKY_PENDING_NONE;
    memset(&slot->shell, 0, sizeof(slot->shell));
}

#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE
/** @brief 数据 slab 池指针映射到 EVENT_FLAG_SLAB_* 档位标记 */
static uint8_t sticky_slab_flag(struct k_mem_slab* slab) {
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
 * @brief 为超内联负载分配私有副本：优先数据 slab，失败回退 k_malloc
 *
 * @return 副本指针（*flags_out 含 provenance 标记），分配失败返回 NULL
 */
static void* sticky_payload_alloc(const event_t* event, uint32_t len, uint8_t* flags_out) {
    void*   copy = NULL;
    uint8_t flags = EVENT_FLAG_DATA_DYNAMIC;

#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE
    {
        struct k_mem_slab* slab = event_memory_select_data_slab(len);
        uint8_t            slab_flag = sticky_slab_flag(slab);

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

    memcpy(copy, (const void*) event->data.ptr, len);
    *flags_out = flags;
    event_debug_track_alloc(copy, len, event->priority);
    return copy;
}

/**
 * @brief 为锁存备料：拍沿快照并为超内联负载分配私有副本（stage 段）
 *
 * 必须在 event_queue_enqueue 之前调用——此刻发布者仍持原负载所有权，
 * 拷贝读到的内存保证有效；入队成功后原负载即归队列副本所有，分发线程
 * 可并发释放，不可再读（C-1 use-after-free 的修复关键窗口）。
 * 超 4KB 上界/拷贝失败在此计 miss 并放弃锁存（保留槽内旧沿）。
 */
static void sticky_stage_copy(const event_t* event, event_sticky_staged_t* staged) {
    staged->active = false;
    staged->payload = NULL;

    if (event == NULL || k_is_in_isr() || (uint32_t) event->type > MAX_EVENT_TYPE_ID) {
        return;
    }

    event_type_entry_t* entry = &g_event_system.event_types[event->type];

    /* 非 sticky 类型热路径：锁外一次字节比较即返回 */
    if (entry->sticky_slot == EVENT_STICKY_IDX_NONE) {
        return;
    }

    uint32_t len = event->data_len;
    void*    payload = NULL;
    uint8_t  flags = (len > 0U) ? EVENT_FLAG_DATA_INLINE : 0U;

    if (len > 0U && (event->flags & EVENT_FLAG_DATA_INLINE) == 0U) {
        if (len > EVENT_STICKY_PAYLOAD_MAX) {
            sticky_count_miss("too-large", event->type, len);
            return;
        }
        payload = sticky_payload_alloc(event, len, &flags);
        if (payload == NULL) {
            sticky_count_miss("alloc-failed", event->type, len);
            return;
        }
    }

    sticky_lock_acquire();
    sticky_slot_t* slot = sticky_slot_resolve(entry, event->type);

    if (slot == NULL || slot->replay_readers > 0U) {
        /* 索引为陈旧快照（disable 进行中）时静默放弃；重放回调正读旧负载时
         * 让位计 miss（旧指针在重放结束前不释放）。两种情形副本均就地释放 */
        if (slot != NULL) {
            sticky_count_miss("replaying", event->type, len);
        }
        sticky_lock_release();
        sticky_payload_free(payload, flags);
        return;
    }

    staged->active = true;
    staged->type = event->type;
    staged->shell = *event;
    staged->payload = payload;
    staged->flags = flags;
    staged->len = len;
    sticky_lock_release();
}

/**
 * @brief 把备好的锁存沿（shell 快照 + 私有副本）提交进槽（须持 sticky 锁）
 *
 * payload 所有权随提交移交槽内：先换新沿再释放旧沿副本，
 * 槽内沿与负载指针在任意时刻都完整有效。
 */
static void sticky_slot_commit_edge(sticky_slot_t* slot, const event_t* shell, void* payload, uint8_t flags,
                                    uint32_t len) {
    /* 旧沿收尾信息先行捕获：提交会覆盖 shell.flags（provenance 载体） */
    void*   old_payload = NULL;
    uint8_t old_flags = 0U;

    if (slot->latched && slot->payload != NULL) {
        old_payload = slot->payload;
        old_flags = slot->shell.flags;
    }

    slot->shell = *shell;
    slot->shell.flags = flags;
    slot->shell.data_len = len;
    if (payload != NULL) {
        slot->shell.data.ptr = payload; /* 壳自洽：重放快照不依赖 data.ptr 覆盖逻辑 */
    }
    slot->payload = payload;
    slot->len = len;
    slot->latched = true;

    /* 新副本已就位，旧副本此时才可释放 */
    sticky_payload_free(old_payload, old_flags);
}

/* =============================================================================
 * 每线程重放深度跟踪
 * ============================================================================= */

/** @brief 当前线程重放深度 +1，返回进入前的深度（表满时放行，锁外告警） */
static uint32_t sticky_replay_depth_enter(void) {
    k_spinlock_key_t key = k_spin_lock(&g_sticky_track_lock);
    k_tid_t          self = k_current_get();
    int32_t          mine = -1;
    int32_t          free_idx = -1;
    uint32_t         depth_before;
    bool             table_full = false;

    for (uint32_t i = 0; i < EVENT_STICKY_REPLAY_TRACKS; i++) {
        if (mine < 0 && g_sticky_replay_tracks[i].thread == self) {
            mine = (int32_t) i;
            continue;
        }
        if (free_idx < 0 && g_sticky_replay_tracks[i].thread == NULL) {
            free_idx = (int32_t) i;
        }
    }

    if (mine >= 0) {
        depth_before = g_sticky_replay_tracks[mine].depth;
        g_sticky_replay_tracks[mine].depth++;
    } else if (free_idx >= 0) {
        g_sticky_replay_tracks[free_idx].thread = self;
        g_sticky_replay_tracks[free_idx].depth = 1U;
        depth_before = 0U;
    } else {
        /* 表满：防御退化放行（回调内重入订阅可能递归重放，深度受订阅槽数约束） */
        table_full = true;
        depth_before = 0U;
    }

    k_spin_unlock(&g_sticky_track_lock, key);

    if (table_full) {
        LOG_WRN("sticky replay track table full; recursion defense disabled for %p", (void*) self);
    }
    return depth_before;
}

/** @brief 当前线程重放深度 -1（与 enter 严格配对） */
static void sticky_replay_depth_exit(void) {
    k_spinlock_key_t key = k_spin_lock(&g_sticky_track_lock);
    k_tid_t          self = k_current_get();

    for (uint32_t i = 0; i < EVENT_STICKY_REPLAY_TRACKS; i++) {
        if (g_sticky_replay_tracks[i].thread == self && g_sticky_replay_tracks[i].depth > 0U) {
            g_sticky_replay_tracks[i].depth--;
            if (g_sticky_replay_tracks[i].depth == 0U) {
                /* 本次重放完成即归还跟踪槽，避免短生命周期线程永久耗尽表项。 */
                g_sticky_replay_tracks[i].thread = NULL;
            }
            break;
        }
    }
    k_spin_unlock(&g_sticky_track_lock, key);
}

/* =============================================================================
 * 发布钩子与订阅重放
 * ============================================================================= */

/* =============================================================================
 * 发布路径两段式锁存（stage → 入队 → commit/discard）
 * ============================================================================= */

void event_sticky_stage_copy(const event_t* event, event_sticky_staged_t* staged) {
    /* 必须在入队前调用：此刻发布者仍持负载所有权，拷贝读到的内存保证有效 */
    sticky_stage_copy(event, staged);
}

void event_sticky_commit(event_sticky_staged_t* staged) {
    if (!staged->active) {
        return;
    }

    event_type_entry_t* entry = &g_event_system.event_types[staged->type];

    sticky_lock_acquire();
    sticky_slot_t* slot = sticky_slot_resolve(entry, staged->type);

    if (slot == NULL) {
        /* stage 与 commit 之间类型被 disable（窄窗竞态）：副本丢弃即可，
         * best-effort 契约允许该窗口内丢失一次锁存，不计 miss */
        sticky_lock_release();
        sticky_payload_free(staged->payload, staged->flags);
        staged->active = false;
        return;
    }
    if (slot->replay_readers > 0U) {
        /* stage 后有新订阅者进入重放：让位计 miss（旧指针在重放结束前不释放） */
        sticky_count_miss("replaying", staged->type, staged->len);
        sticky_lock_release();
        sticky_payload_free(staged->payload, staged->flags);
        staged->active = false;
        return;
    }
    sticky_slot_commit_edge(slot, &staged->shell, staged->payload, staged->flags, staged->len);
    sticky_lock_release();
    staged->active = false;
}

void event_sticky_discard(event_sticky_staged_t* staged) {
    if (!staged->active) {
        return;
    }
    /* 入队失败即发布失败：副本直接丢弃，不计 sticky miss */
    sticky_payload_free(staged->payload, staged->flags);
    staged->active = false;
}

/** @brief 拍快照、锁外回调、收尾 pending 的重放主体（深度防御由调用方负责） */
static void sticky_replay_do(event_type_entry_t* entry, event_type_t type, event_callback_t callback,
                             void* user_data) {
    event_t snap;

    sticky_lock_acquire();
    sticky_slot_t* slot = sticky_slot_resolve(entry, type);

    if (slot == NULL || !slot->latched) {
        sticky_lock_release();
        return;
    }
    snap = slot->shell;
    if (slot->payload != NULL) {
        snap.data.ptr = slot->payload;
    }
    slot->replay_readers++;
    sticky_lock_release();

    /* 锁外执行回调：payload 只读，重放期间 update 让位、clear/disable 转入 pending */
    callback(&snap, user_data);

    sticky_lock_acquire();
    slot->replay_readers--;
    if (slot->replay_readers == 0U && slot->pending != EVENT_STICKY_PENDING_NONE) {
        if (slot->pending == EVENT_STICKY_PENDING_DISABLE) {
            sticky_slot_reset(slot);
        } else {
            sticky_slot_drop_edge(slot);
        }
        slot->pending = EVENT_STICKY_PENDING_NONE;
    }
    sticky_lock_release();
}

void event_sticky_replay_on_subscribe(event_type_t type, event_callback_t callback, void* user_data) {
    if (callback == NULL || (uint32_t) type > MAX_EVENT_TYPE_ID) {
        return;
    }

    event_type_entry_t* entry = &g_event_system.event_types[type];

    if (entry->sticky_slot == EVENT_STICKY_IDX_NONE) {
        return;
    }

    /* 深度防御：回调内重入订阅时跳过重放，防递归 */
    uint32_t depth_before = sticky_replay_depth_enter();

    if (depth_before == 0U) {
        sticky_replay_do(entry, type, callback, user_data);
    }
    sticky_replay_depth_exit();
}

void event_sticky_init_all(void) {
    /* 调用前提：ops gate 已关闭且在途操作排空（init/shutdown 路径），无并发持有槽内指针。 */
    sticky_lock_acquire();
    for (uint32_t i = 0; i < CONFIG_EVENT_STICKY_MAX_TYPES; i++) {
        sticky_slot_reset(&g_sticky_slots[i]);
    }
    sticky_lock_release();
    atomic_set(&g_sticky_miss_count, 0);
}

void event_sticky_release_all(void) {
    event_sticky_init_all();
}

/* =============================================================================
 * 公共 API
 * ============================================================================= */

/** @brief sticky 公共 API 前置校验：ops gate + 魔数 + 初始化（失败已执行 op_exit） */
static event_status_t sticky_api_gate(void) {
    if (!event_system_op_enter()) {
        /* 生命周期切换中（stop/shutdown 正排空在途操作）：与 timer 口径一致按未运行拒绝 */
        return EVENT_ERR_NOT_RUNNING;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC || !g_event_system.initialized) {
        event_system_op_exit();
        return EVENT_ERR_INVALID_ARG;
    }
    return EVENT_OK;
}

event_status_t event_sticky_enable(event_type_t type) {
    event_status_t gate = sticky_api_gate();

    if (gate != EVENT_OK) {
        return gate;
    }
    if ((uint32_t) type > MAX_EVENT_TYPE_ID) {
        event_system_op_exit();
        return EVENT_ERR_INVALID_ARG;
    }
    if (!event_system_type_is_registered(type)) {
        event_system_op_exit();
        return EVENT_ERR_NOT_FOUND;
    }

    event_type_entry_t* entry = &g_event_system.event_types[type];
    int32_t             claimed = -1;

    /* sticky→entry 锁序内分配/复用：同类型已占槽直接复用，否则取空闲槽 */
    sticky_lock_acquire();
    for (uint32_t i = 0; i < CONFIG_EVENT_STICKY_MAX_TYPES; i++) {
        sticky_slot_t* slot = &g_sticky_slots[i];

        if (slot->owner_type == type) {
            if (slot->pending == EVENT_STICKY_PENDING_DISABLE) {
                continue; /* disable 推迟收尾中：该槽即将归还，视同不可用 */
            }
            claimed = (int32_t) i;
            break;
        }
        if (slot->owner_type == EVENT_STICKY_OWNER_NONE && claimed < 0) {
            claimed = (int32_t) i;
        }
    }

    if (claimed < 0) {
        sticky_lock_release();
        event_system_op_exit();
        LOG_WRN("sticky: no free slot for type %u (pool=%u)", (unsigned int) type,
                (unsigned int) CONFIG_EVENT_STICKY_MAX_TYPES);
        return EVENT_ERR_NOT_FOUND;
    }

    if (g_sticky_slots[claimed].owner_type != type) {
        /* 新领槽：复位为干净状态（旧沿不可能残留：空闲槽在释放路径已清） */
        sticky_slot_reset(&g_sticky_slots[claimed]);
        g_sticky_slots[claimed].owner_type = type;
    }
    event_system_entry_lock(entry);
    entry->sticky_slot = (uint8_t) claimed;
    event_system_entry_unlock(entry);
    sticky_lock_release();

    event_system_op_exit();
    LOG_DBG("sticky enabled for type %u (slot %d)", (unsigned int) type, claimed);
    return EVENT_OK;
}

event_status_t event_sticky_clear(event_type_t type) {
    event_status_t gate = sticky_api_gate();

    if (gate != EVENT_OK) {
        return gate;
    }
    if ((uint32_t) type > MAX_EVENT_TYPE_ID) {
        event_system_op_exit();
        return EVENT_ERR_INVALID_ARG;
    }

    event_type_entry_t* entry = &g_event_system.event_types[type];

    sticky_lock_acquire();
    sticky_slot_t* slot = sticky_slot_resolve(entry, type);

    if (slot == NULL) {
        sticky_lock_release();
        event_system_op_exit();
        return EVENT_ERR_NOT_FOUND;
    }
    if (slot->replay_readers > 0U) {
        /* 重放中：立即按已清除对待，负载延迟到重放排空后释放 */
        slot->latched = false;
        if (slot->pending == EVENT_STICKY_PENDING_NONE) {
            slot->pending = EVENT_STICKY_PENDING_CLEAR;
        }
    } else {
        sticky_slot_drop_edge(slot);
    }
    sticky_lock_release();

    event_system_op_exit();
    return EVENT_OK;
}

event_status_t event_sticky_disable(event_type_t type) {
    event_status_t gate = sticky_api_gate();

    if (gate != EVENT_OK) {
        return gate;
    }
    if ((uint32_t) type > MAX_EVENT_TYPE_ID) {
        event_system_op_exit();
        return EVENT_ERR_INVALID_ARG;
    }

    event_type_entry_t* entry = &g_event_system.event_types[type];

    /* 与 enable 使用相同的 sticky → entry 锁序，使索引摘除与槽位归还不可分割。 */
    sticky_lock_acquire();
    event_system_entry_lock(entry);
    uint8_t idx = entry->sticky_slot;

    if (idx >= CONFIG_EVENT_STICKY_MAX_TYPES || g_sticky_slots[idx].owner_type != type) {
        entry->sticky_slot = EVENT_STICKY_IDX_NONE;
        event_system_entry_unlock(entry);
        sticky_lock_release();
        event_system_op_exit();
        return EVENT_ERR_NOT_FOUND;
    }
    sticky_slot_t* slot = &g_sticky_slots[idx];
    entry->sticky_slot = EVENT_STICKY_IDX_NONE;
    event_system_entry_unlock(entry);
    if (slot->replay_readers > 0U) {
        /* 重放中：立即停止后续重放语义，槽与负载延迟到重放排空后释放 */
        slot->latched = false;
        slot->pending = EVENT_STICKY_PENDING_DISABLE;
    } else {
        sticky_slot_reset(slot);
    }
    sticky_lock_release();

    event_system_op_exit();
    LOG_DBG("sticky disabled for type %u", (unsigned int) type);
    return EVENT_OK;
}

bool event_sticky_is_latched(event_type_t type) {
    if ((uint32_t) type > MAX_EVENT_TYPE_ID) {
        return false;
    }

    /* 原子读实现（ISR 安全）：字节索引 + 属主/沿标志的单字读取 */
    event_type_entry_t* entry = &g_event_system.event_types[type];
    uint8_t             idx = entry->sticky_slot;

    if (idx >= CONFIG_EVENT_STICKY_MAX_TYPES) {
        return false;
    }
    const sticky_slot_t* slot = &g_sticky_slots[idx];

    return slot->owner_type == type && slot->latched;
}

void event_sticky_get_stats(uint32_t* latched_types, uint32_t* miss_count) {
    if (latched_types != NULL) {
        *latched_types = 0U;
    }
    if (miss_count != NULL) {
        *miss_count = 0U;
    }

    event_status_t gate = sticky_api_gate();

    if (gate != EVENT_OK) {
        return;
    }

    sticky_lock_acquire();
    uint32_t latched = 0U;

    for (uint32_t i = 0; i < CONFIG_EVENT_STICKY_MAX_TYPES; i++) {
        const sticky_slot_t* slot = &g_sticky_slots[i];

        if (slot->owner_type != EVENT_STICKY_OWNER_NONE && slot->latched) {
            latched++;
        }
    }
    sticky_lock_release();

    if (latched_types != NULL) {
        *latched_types = latched;
    }
    if (miss_count != NULL) {
        *miss_count = (uint32_t) atomic_get(&g_sticky_miss_count);
    }
    event_system_op_exit();
}
