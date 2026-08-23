/**
 * @file event_system_stats.c
 * @brief 事件系统统计与丢弃计数
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-05-28
 */

#include <zephyr/logging/log.h>
#include <zeplod/event_dispatcher.h>
#include "event_queue.h"
#include "event_system_internal.h"

LOG_MODULE_DECLARE(event_system, CONFIG_SYS_LOG_LEVEL);

extern void event_dispatcher_stats_inc_dropped(void);

void event_system_inc_dropped_count(void) {
    atomic_inc(&g_event_dropped_count);
    event_dispatcher_stats_inc_dropped();
}

void event_get_statistics(uint32_t* total_events, uint32_t* queue_depth, uint32_t* dropped_events) {
    if (total_events != NULL) {
        *total_events = 0U;
    }
    if (queue_depth != NULL) {
        *queue_depth = 0U;
    }
    if (dropped_events != NULL) {
        *dropped_events = 0U;
    }

    if (!event_system_op_enter()) {
        return;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC || !g_event_system.initialized) {
        event_system_op_exit();
        return;
    }

    /* total_events 为原子计数：热路径 atomic_inc 无锁，此处原子快照即可 */
    if (total_events != NULL) {
        *total_events = (uint32_t) atomic_get(&g_event_system.total_events);
    }
    if (queue_depth != NULL && g_event_system.event_queue != NULL) {
        *queue_depth = k_msgq_num_used_get(g_event_system.event_queue);
    }
    if (dropped_events != NULL) {
        *dropped_events = (uint32_t) atomic_get(&g_event_dropped_count);
    }

    event_system_op_exit();
}

void event_system_reset_statistics(void) {
    if (!event_system_op_enter()) {
        return;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC || !g_event_system.initialized) {
        event_system_op_exit();
        return;
    }

    atomic_set(&g_event_system.total_events, 0);
    atomic_set(&g_event_dropped_count, 0);

    if (g_event_system.event_queue != NULL) {
        event_queue_reset_stats(g_event_system.event_queue);
    }
    event_dispatcher_reset_stats();

    LOG_DBG("Event system statistics reset");
    event_system_op_exit();
}
