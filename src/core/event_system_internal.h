/**
 * @file event_system_internal.h
 * @brief 事件系统 core 内部共享（不对外公开）
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-05-28
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
#endif /* EVENT_SYSTEM_INTERNAL_H */
