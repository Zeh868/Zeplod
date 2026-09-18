/**
 * @file event_system.c
 * @brief 事件系统全局状态与队列访问
 *
 * 实现按职责拆分至：
 * - event_system_lifecycle.c  生命周期
 * - event_system_pubsub.c     注册/订阅/通知
 * - event_system_publish.c    发布入队
 * - event_system_memory.c     创建/释放
 * - event_system_stats.c      统计
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            补充文件头修改日志段（审查整改）
 */

#include <zephyr/logging/log.h>
#include <zeplod/event_system.h>
#include "event_system_internal.h"

LOG_MODULE_REGISTER(event_system, CONFIG_SYS_LOG_LEVEL);

event_system_cb_t g_event_system;

struct k_msgq g_event_msgq;

char g_event_msgq_buffer[CONFIG_EVENT_QUEUE_SIZE * sizeof(event_t)] __aligned(__alignof__(event_t));

atomic_t g_event_dropped_count;

atomic_t g_event_ops_accepting;

atomic_t g_event_ops_epoch;

atomic_t g_event_ops_in_flight;

atomic_t g_event_system_init_lock = ATOMIC_INIT(0);

atomic_t g_restart_dispatcher_on_start;

struct k_msgq* event_system_get_queue(void) {
    if (!event_system_op_enter()) {
        return NULL;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC || !g_event_system.initialized) {
        event_system_op_exit();
        return NULL;
    }

    struct k_msgq* queue = g_event_system.event_queue;
    event_system_op_exit();
    return queue;
}

/*
 * SIL-2: 标准版事件系统的初始化由 event_system_compat.c 中的
 * event_compat_auto_init() 统一处理。该函数调用 event_compat_init()
 * -> event_system_init() 完成初始化，优先级为 APP_INIT_PRIO_EVENT_SYS。
 *
 * 为避免双重 SYS_INIT 竞态（CRIT-2 修复），event_system.c 中不再注册
 * 独立的 SYS_INIT。event_system_init() 的幂等性保证多次调用安全。
 */
