/**
 * @file event_system_internal.h
 * @brief 事件系统 core 内部共享（不对外公开）
 * @author zeh (china_qzh@163.com)
 * @version 1.6
 * @date 2026-09-29
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-05-28       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            收编 event_system_inc_dropped_count 原型，消除 event_queue.c 裸 extern
 * 2026-09-27       1.2            zeh            新增订阅级过滤跳过计数原子变量声明（dispatcher 统计镜像）
 * 2026-09-27       1.3            zeh            新增 sticky 更新/重放/全量释放内部接口声明
 * 2026-09-27       1.4            zeh            新增事件定时器池 cancel_all 内部接口声明（B3 延迟/周期发布）
 * 2026-09-27       1.5            zeh            sticky 锁存改两段式：stage/commit/discard 暂存区接口取代入队后 update
 * 钩子（C-1） 2026-09-29       1.6            zeh            定时器池增加 event_timer_pool_init 声明
 */

#ifndef EVENT_SYSTEM_INTERNAL_H
#define EVENT_SYSTEM_INTERNAL_H

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zeplod/event_system.h>
#include <zeplod/lock_order.h>
#include <zeplod/state_machine.h>

/** 最大支持的事件类型数量（从 Kconfig 获取） */
#define MAX_EVENT_TYPES         CONFIG_EVENT_MAX_TYPES
#define MAX_EVENT_TYPE_ID       (MAX_EVENT_TYPES - 1U)

/** 魔术字，用于验证控制块有效性 ("EVNT") */
#define EVENT_SYSTEM_MAGIC      0x45564E54U

/** 未初始化或已完成 shutdown（与 BSS 初值一致） */
#define EVENT_SYSTEM_MAGIC_IDLE 0U

/** SIL-2: 验证事件系统魔术字（返回 event_status_t 版本） */
#define EVENT_SYSTEM_VALIDATE()                                                                                        \
    do {                                                                                                               \
        if (g_event_system.magic == EVENT_SYSTEM_MAGIC_IDLE) {                                                         \
            return EVENT_ERR_INVALID_ARG;                                                                              \
        }                                                                                                              \
        if (g_event_system.magic != EVENT_SYSTEM_MAGIC) {                                                              \
            LOG_ERR("Event system magic corruption detected: 0x%08x", g_event_system.magic);                           \
            return EVENT_ERR_INVALID_ARG;                                                                              \
        }                                                                                                              \
    } while (0)

/** SIL-2: 验证事件系统魔术字（返回 void 版本） */
#define EVENT_SYSTEM_VALIDATE_VOID()                                                                                   \
    do {                                                                                                               \
        if (g_event_system.magic == EVENT_SYSTEM_MAGIC_IDLE) {                                                         \
            return;                                                                                                    \
        }                                                                                                              \
        if (g_event_system.magic != EVENT_SYSTEM_MAGIC) {                                                              \
            LOG_ERR("Event system magic corruption detected: 0x%08x", g_event_system.magic);                           \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

/** 分配类 API：空闲态静默失败，非法 magic 记录损坏 */
#define EVENT_SYSTEM_CHECK_MAGIC_ALLOC()                                                                               \
    do {                                                                                                               \
        if (g_event_system.magic == EVENT_SYSTEM_MAGIC_IDLE) {                                                         \
            return NULL;                                                                                               \
        }                                                                                                              \
        if (g_event_system.magic != EVENT_SYSTEM_MAGIC) {                                                              \
            if (!k_is_in_isr()) {                                                                                      \
                LOG_ERR("Event system magic corruption detected: 0x%08x", g_event_system.magic);                       \
            }                                                                                                          \
            return NULL;                                                                                               \
        }                                                                                                              \
    } while (0)

/** stop/shutdown 等待已准入事件 API 退出的最大时间（毫秒） */
#define EVENT_SYSTEM_OP_WAIT_TIMEOUT_MS 5000U

/**
 * @brief 事件系统控制块
 */
typedef struct {
    uint32_t             magic;
    bool                 initialized;
    atomic_t             running;
    zepl_state_machine_t lifecycle;
    struct k_msgq*       event_queue;
    event_type_entry_t   event_types[MAX_EVENT_TYPES];
    atomic_t             total_events; /**< 已分发事件计数（分发线程单写，原子读） */
    atomic_t             next_subscriber_id;
    bool                 subscriber_id_wrapped;
} event_system_cb_t;

extern event_system_cb_t g_event_system;
extern struct k_msgq     g_event_msgq;
extern char              g_event_msgq_buffer[];
extern atomic_t          g_event_system_init_lock;
extern atomic_t          g_restart_dispatcher_on_start;
extern atomic_t          g_event_dropped_count;
extern atomic_t          g_event_ops_accepting;
extern atomic_t          g_event_ops_epoch;
extern atomic_t          g_event_ops_in_flight;
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
extern atomic_t g_event_filter_skip_count; /**< 订阅级过滤跳过计数（event_system_pubsub.c 定义） */
#endif

void         event_system_lifecycle_lock_wait(void);
bool         event_system_lifecycle_try_lock(void);
void         event_system_lifecycle_unlock(void);
zepl_state_t event_system_lifecycle_state(void);

void event_system_subscriber_id_lock(void);
void event_system_subscriber_id_unlock(void);
void event_system_entry_lock(event_type_entry_t* entry);
void event_system_entry_unlock(event_type_entry_t* entry);

bool           event_system_op_enter(void);
void           event_system_op_exit(void);
void           event_system_ops_close(void);
void           event_system_ops_open(void);
event_status_t event_system_ops_wait_zero(void);
void           event_system_init_rollback(void);
void           event_system_cleanup_event_types(void);
void           event_system_reset_control_block(void);

bool event_system_type_is_registered(event_type_t type);

/**
 * @brief 全局丢弃计数 +1（定义于 event_system_stats.c）
 *
 * 队列溢出/丢弃路径调用；同时联动分发器的 events_dropped 统计。
 * ISR 安全（内部仅原子操作）。
 */
void event_system_inc_dropped_count(void);

#if defined(CONFIG_EVENT_STICKY)
/**
 * @brief sticky 两段式锁存暂存区（发布路径栈上持有，生命周期限单次发布调用内）
 *
 * stage 段在入队前执行：发布者仍持负载所有权时拍沿快照并备好私有副本；
 * commit 段在入队成功后把副本提交进槽（槽内旧沿释放）；discard 段在入队
 * 失败时丢弃副本（发布本就失败，不计 sticky miss）。字段布局由
 * event_sticky.c 私有约定，发布路径仅透传。
 */
typedef struct {
    bool         active;  /**< stage 已命中 sticky 类型并备好副本；false 时 commit/discard 直返 */
    event_type_t type;    /**< 暂存锁存沿的事件类型 */
    event_t      shell;   /**< 入队前拍下的沿快照（提交时 flags 覆写为副本 provenance） */
    void*        payload; /**< 超内联负载私有副本；NULL=内联于 shell 或空负载 */
    uint8_t      flags;   /**< 副本 provenance 标记（DYNAMIC/FROM_SLAB/SLAB 档位） */
    uint32_t     len;     /**< 锁存负载长度（字节） */
} event_sticky_staged_t;

/**
 * @brief sticky 锁存 stage 段（定义于 event_sticky.c）
 *
 * 仅线程侧调用（ISR 跳过）；非 sticky 类型入口处一次字节比较即返回。
 * 必须在 event_queue_enqueue 之前调用：此刻发布者仍持负载所有权，
 * 私有副本拷贝读到的内存保证有效（入队成功后原负载归队列副本所有，
 * 分发线程可并发释放，不可再读）。锁存 best-effort：超上界/拷贝失败仅计
 * miss 并 LOG_WRN，不影响发布结果；重放占用中让位计 miss。
 *
 * @param event 待发布事件（调用期间其负载缓冲有效且发布者持所有权）
 * @param staged 输出：暂存区（active=false 表示本次发布无锁存动作）
 */
void event_sticky_stage_copy(const event_t* event, event_sticky_staged_t* staged);

/**
 * @brief sticky 锁存 commit 段（定义于 event_sticky.c）
 *
 * 入队成功后调用：把 staged 内私有副本提交进槽并释放槽内旧沿。
 * stage 与 commit 之间槽被 disable（窄窗竞态）时副本丢弃、不计 miss
 * （best-effort 契约允许该窗口丢失一次锁存）；重放占用中让位计 miss。
 *
 * @param staged stage 段产出的暂存区；副本所有权随之移交槽内或就地释放
 */
void event_sticky_commit(event_sticky_staged_t* staged);

/**
 * @brief sticky 锁存 discard 段（定义于 event_sticky.c）
 *
 * 入队失败后调用：丢弃暂存的私有副本（发布本就失败，不计 sticky miss）。
 *
 * @param staged stage 段产出的暂存区
 */
void event_sticky_discard(event_sticky_staged_t* staged);

/**
 * @brief 订阅成功后的 sticky 同步重放（定义于 event_sticky.c）
 *
 * 须在订阅的两把锁（subscriber_id/entry）释放后、仍持有 ops gate 时调用；
 * 该类型持有锁存沿时以栈上快照同步调用一次新订阅者回调（锁外执行）。
 *
 * @param type 事件类型 ID
 * @param callback 新订阅者的回调（不能为 NULL）
 * @param user_data 新订阅者的用户数据
 */
void event_sticky_replay_on_subscribe(event_type_t type, event_callback_t callback, void* user_data);

/**
 * @brief 将 sticky 槽池复位为空闲状态（定义于 event_sticky.c）
 *
 * init 与 shutdown 路径在 ops gate 已关闭、在途操作已排空后调用；覆盖 BSS 冷启动，
 * 确保空槽 owner 使用非零的空闲标记。
 */
void event_sticky_init_all(void);

/**
 * @brief 释放全部 sticky 槽与锁存负载（定义于 event_sticky.c）
 *
 * shutdown/cleanup 路径调用：调用前须已通过 ops gate 排空在途操作，
 * 保证无并发重放/更新持有槽内指针。
 */
void event_sticky_release_all(void);
#endif /* CONFIG_EVENT_STICKY */

#if defined(CONFIG_EVENT_TIMER_POOL)
/**
 * @brief 初始化定时槽的 k_timer（定义于 event_timer.c，只执行一次）
 */
void event_timer_pool_init(void);

/**
 * @brief 取消全部定时槽并释放槽内负载（定义于 event_timer.c）
 *
 * shutdown 路径在 event_queue_deinit 之前调用：逐槽 k_timer_stop 并释放
 * 一次性待投递副本/周期母本，防止 deinit 后到期回调触碰已销毁队列；
 * 与在飞到期回调的竞态由槽状态机 + gen 校验兜底（单核与 SMP 同此兜底）。
 */
void event_timer_pool_cancel_all(void);
#endif /* CONFIG_EVENT_TIMER_POOL */
#endif /* EVENT_SYSTEM_INTERNAL_H */
