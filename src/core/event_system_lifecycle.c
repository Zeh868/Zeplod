/**
 * @file event_system_lifecycle.c
 * @brief 事件系统生命周期（init/start/stop/shutdown）
 * @author zeh (china_qzh@163.com)
 * @version 1.3
 * @date 2026-05-28
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-05-28       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            拆分 init/stop/shutdown 超长函数（门控/资源准备/停止流程提取为辅助函数）
 * 2026-09-27       1.2            zeh            init 复位 sticky 索引、shutdown 清理 sticky 槽与锁存负载（stop 不动锁存）
 * 2026-09-27       1.3            zeh            shutdown 在队列销毁前取消全部定时器池槽并释放负载（B3）
 */

#include <zephyr/logging/log.h>
#include <string.h>
#include <zeplod/event_dispatcher.h>
#include "event_queue.h"
#include "event_system_internal.h"

LOG_MODULE_DECLARE(event_system, CONFIG_SYS_LOG_LEVEL);

void event_system_lifecycle_lock_wait(void) {
    /* k_yield 只让给同优先级就绪线程：若高优先级调用者自旋等待低优先级持有者，
     * 单核下持有者永远得不到 CPU（活锁直至看门狗复位）。改用 1ms 睡眠让出 CPU。 */
    while (atomic_test_and_set_bit(&g_event_system_init_lock, 0)) {
        k_sleep(K_MSEC(1));
    }
}

bool event_system_lifecycle_try_lock(void) {
    return !atomic_test_and_set_bit(&g_event_system_init_lock, 0);
}

void event_system_lifecycle_unlock(void) {
    atomic_clear_bit(&g_event_system_init_lock, 0);
}

zepl_state_t event_system_lifecycle_state(void) {
    return zepl_state_machine_get(&g_event_system.lifecycle);
}

bool event_system_op_enter(void) {
    atomic_val_t epoch = atomic_get(&g_event_ops_epoch);

    if (atomic_get(&g_event_ops_accepting) == 0) {
        return false;
    }

    /* epoch 重新检查用于拒绝那些观察到旧的开放 gate、在 shutdown 和重新 init 之后才恢复执行的调用者 */
    (void) atomic_inc(&g_event_ops_in_flight);
    if (atomic_get(&g_event_ops_accepting) == 0 || atomic_get(&g_event_ops_epoch) != epoch) {
        atomic_dec(&g_event_ops_in_flight);
        return false;
    }

    return true;
}

void event_system_op_exit(void) {
    atomic_dec(&g_event_ops_in_flight);
}

void event_system_ops_close(void) {
    atomic_set(&g_event_ops_accepting, 0);
    /* 在等待前先推进 epoch，防止上一个生命周期的迟到者在 gate 重新打开后通过 */
    (void) atomic_inc(&g_event_ops_epoch);
}

void event_system_ops_open(void) {
    atomic_set(&g_event_ops_accepting, 1);
}

event_status_t event_system_ops_wait_zero(void) {
    int64_t deadline = k_uptime_get() + (int64_t) EVENT_SYSTEM_OP_WAIT_TIMEOUT_MS;
    while (atomic_get(&g_event_ops_in_flight) != 0) {
        if (k_uptime_get() >= deadline) {
            LOG_ERR("Timeout waiting event operations to drain: %d", (int) atomic_get(&g_event_ops_in_flight));
            return EVENT_ERR_TIMEOUT;
        }
        k_sleep(K_MSEC(1));
    }
    return EVENT_OK;
}

void event_system_cleanup_event_types(void) {
    for (int type = 0; type < MAX_EVENT_TYPES; type++) {
        event_type_entry_t* entry = &g_event_system.event_types[type];
        event_system_entry_lock(entry);
        atomic_set(&entry->registered, 0);
        entry->name = NULL;
        entry->name_storage[0] = '\0';
        entry->subscriber_count = 0;
        memset(entry->subscribers, 0, sizeof(entry->subscribers));
#if defined(CONFIG_EVENT_STICKY)
        entry->sticky_slot = EVENT_STICKY_IDX_NONE;
#endif
        event_system_entry_unlock(entry);
    }

#if defined(CONFIG_EVENT_STICKY)
    /* shutdown 释放全部锁存负载与槽位；此时 ops gate 已关闭且在途操作排空 */
    event_sticky_release_all();
#endif
}

void event_system_reset_control_block(void) {
    (void) zepl_state_machine_try_transition(&g_event_system.lifecycle, ZEP_STATE_UNINIT);
    g_event_system.initialized = false;
    atomic_set(&g_event_system.total_events, 0);
    atomic_set(&g_event_system.next_subscriber_id, 1);
    g_event_system.subscriber_id_wrapped = false;
    g_event_system.event_queue = NULL;
    g_event_system.magic = EVENT_SYSTEM_MAGIC_IDLE;
}

void event_system_init_rollback(void) {
    event_queue_deinit(&g_event_msgq);
    memset(&g_event_system, 0, sizeof(g_event_system));
    atomic_clear(&g_restart_dispatcher_on_start);
}

/**
 * @brief init 前置门控：拒绝 ISR 与分发器线程，其余调用者阻塞取得生命周期锁
 *
 * 分发器线程内禁止阻塞等待（k_sleep 语义非法），仅尝试一次。
 *
 * @return EVENT_OK 已持锁，EVENT_ERR_INVALID_ARG 调用上下文被拒绝
 */
static event_status_t event_system_init_gate(void) {
    /* 生命周期锁在 ISR 中不可获取（k_sleep 语义非法），拒绝 ISR 上下文调用 */
    if (k_is_in_isr()) {
        return EVENT_ERR_INVALID_ARG;
    }

    if (event_dispatcher_is_current_thread()) {
        if (!event_system_lifecycle_try_lock()) {
            LOG_ERR("Cannot init event system from dispatcher thread during lifecycle transition");
            return EVENT_ERR_INVALID_ARG;
        }
        return EVENT_OK;
    }

    event_system_lifecycle_lock_wait();
    return EVENT_OK;
}

/**
 * @brief 复位控制块、初始化事件类型表并创建队列（持生命周期锁调用）
 *
 * 关闭 ops gate 并等待在途事件操作排空后，才允许触碰共享状态。
 *
 * @return EVENT_OK 成功，其他错误码见 event_status_t（失败时已执行回滚，锁仍由调用方持有）
 */
static event_status_t event_system_init_resources(void) {
    event_system_ops_close();
    event_status_t wait_ret = event_system_ops_wait_zero();
    if (wait_ret != EVENT_OK) {
        return wait_ret;
    }

    memset(&g_event_system, 0, sizeof(g_event_system));
    zepl_state_machine_init(&g_event_system.lifecycle, ZEP_STATE_UNINIT);

    for (int i = 0; i < MAX_EVENT_TYPES; i++) {
        g_event_system.event_types[i].type = i;
#if defined(CONFIG_EVENT_STICKY)
        g_event_system.event_types[i].sticky_slot = EVENT_STICKY_IDX_NONE;
#endif
        k_mutex_init(&g_event_system.event_types[i].lock);
    }

#if defined(CONFIG_EVENT_STICKY)
    /* BSS 冷启动时 owner_type 为 0，必须显式复位为空闲标记。 */
    event_sticky_init_all();
#endif

    g_event_system.event_queue = &g_event_msgq;
    atomic_clear(&g_restart_dispatcher_on_start);

    event_status_t qret = event_queue_init(&g_event_msgq, g_event_msgq_buffer, CONFIG_EVENT_QUEUE_SIZE);
    if (qret != EVENT_OK) {
        event_system_init_rollback();
        LOG_ERR("event_queue_init failed: %d", qret);
        return qret;
    }

    return EVENT_OK;
}

event_status_t event_system_init(void) {
    LOG_DBG("Initializing event system...");

    event_status_t ret = event_system_init_gate();
    if (ret != EVENT_OK) {
        return ret;
    }

    if (g_event_system.initialized) {
        if (g_event_system.magic != EVENT_SYSTEM_MAGIC) {
            LOG_ERR("Event system initialized flag set but magic is invalid: 0x%08x", g_event_system.magic);
            event_system_lifecycle_unlock();
            return EVENT_ERR_INVALID_ARG;
        }
        event_system_lifecycle_unlock();
        LOG_WRN("Event system already initialized");
        return EVENT_OK;
    }

    if (g_event_system.magic != EVENT_SYSTEM_MAGIC_IDLE) {
        LOG_ERR("Event system magic corruption detected before init: 0x%08x", g_event_system.magic);
        event_system_lifecycle_unlock();
        return EVENT_ERR_INVALID_ARG;
    }

    ret = event_system_init_resources();
    if (ret != EVENT_OK) {
        event_system_lifecycle_unlock();
        return ret;
    }

    g_event_system.magic = EVENT_SYSTEM_MAGIC;
    atomic_set(&g_event_system.next_subscriber_id, 1);
    g_event_system.subscriber_id_wrapped = false;
    atomic_set(&g_event_system.running, 0);
    g_event_system.initialized = true;
    if (zepl_state_machine_try_transition(&g_event_system.lifecycle, ZEP_STATE_INITED) != 0) {
        event_system_init_rollback();
        event_system_lifecycle_unlock();
        LOG_ERR("Failed to transition event system to INITED");
        return EVENT_ERR_INVALID_ARG;
    }

    event_system_ops_open();
    event_system_lifecycle_unlock();
    LOG_DBG("Event system initialized successfully");
    return EVENT_OK;
}

event_status_t event_system_start(void) {
    if (k_is_in_isr()) {
        return EVENT_ERR_INVALID_ARG;
    }

    if (!event_system_lifecycle_try_lock()) {
        LOG_WRN("Event system lifecycle transition in progress; start rejected");
        return EVENT_ERR_TIMEOUT;
    }

    if (g_event_system.magic == EVENT_SYSTEM_MAGIC_IDLE) {
        event_system_lifecycle_unlock();
        return EVENT_ERR_INVALID_ARG;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC) {
        LOG_ERR("Event system magic corruption detected: 0x%08x", g_event_system.magic);
        event_system_lifecycle_unlock();
        return EVENT_ERR_INVALID_ARG;
    }
    if (!g_event_system.initialized) {
        LOG_ERR("Event system not initialized");
        event_system_lifecycle_unlock();
        return EVENT_ERR_INVALID_ARG;
    }

    {
        const zepl_state_t lc_state = event_system_lifecycle_state();

        if (lc_state == ZEP_STATE_RUNNING && atomic_get(&g_event_system.running) != 0) {
            LOG_WRN("Event system already running");
            event_system_lifecycle_unlock();
            return EVENT_OK;
        }

        if (lc_state != ZEP_STATE_INITED && lc_state != ZEP_STATE_STOPPED) {
            LOG_ERR("Event system not in a startable state: %s", zepl_state_name(lc_state));
            event_system_lifecycle_unlock();
            return EVENT_ERR_INVALID_ARG;
        }

        if (zepl_state_machine_try_transition(&g_event_system.lifecycle, ZEP_STATE_STARTING) != 0) {
            LOG_ERR("Failed to transition event system to STARTING from %s", zepl_state_name(lc_state));
            event_system_lifecycle_unlock();
            return EVENT_ERR_INVALID_ARG;
        }

        if (zepl_state_machine_try_transition(&g_event_system.lifecycle, ZEP_STATE_RUNNING) != 0) {
            (void) zepl_state_machine_try_transition(&g_event_system.lifecycle, ZEP_STATE_STOPPING);
            (void) zepl_state_machine_try_transition(&g_event_system.lifecycle, ZEP_STATE_STOPPED);
            LOG_ERR("Failed to transition event system to RUNNING from STARTING; rolled back to STOPPED");
            event_system_lifecycle_unlock();
            return EVENT_ERR_INVALID_ARG;
        }
    }

    if (atomic_cas(&g_restart_dispatcher_on_start, 1, 0)) {
        if (event_dispatcher_is_initialized()) {
            event_status_t dret = event_dispatcher_start();
            if (dret != EVENT_OK) {
                LOG_ERR("event_dispatcher_start during event_system_start failed: %d", dret);
                atomic_set(&g_restart_dispatcher_on_start, 1);
                (void) zepl_state_machine_try_transition(&g_event_system.lifecycle, ZEP_STATE_STOPPING);
                (void) zepl_state_machine_try_transition(&g_event_system.lifecycle, ZEP_STATE_STOPPED);
                event_system_lifecycle_unlock();
                return dret;
            }
        }
    }

    atomic_set(&g_event_system.running, 1);
    event_system_lifecycle_unlock();
    LOG_DBG("Event system started");
    return EVENT_OK;
}

/**
 * @brief RUNNING 时迁移到 STOPPING（持生命周期锁调用；stop/shutdown 共用）
 *
 * @return EVENT_OK 已处于停止流程（含非 RUNNING 直接通过），
 *         EVENT_ERR_INVALID_ARG 迁移失败（已解锁）
 */
static event_status_t event_system_begin_stopping(void) {
    if (event_system_lifecycle_state() != ZEP_STATE_RUNNING) {
        return EVENT_OK;
    }
    if (zepl_state_machine_try_transition(&g_event_system.lifecycle, ZEP_STATE_STOPPING) != 0) {
        LOG_ERR("Failed to transition event system to STOPPING");
        event_system_lifecycle_unlock();
        return EVENT_ERR_INVALID_ARG;
    }
    return EVENT_OK;
}

/**
 * @brief stop 路径停止分发器：RUNNING/PAUSED 时登记 start 重启标记
 *
 * @return EVENT_OK 成功（含分发器未初始化），其他错误码：失败时已重新打开 ops gate
 */
static event_status_t event_system_stop_dispatcher(void) {
    if (!event_dispatcher_is_initialized()) {
        return EVENT_OK;
    }

    dispatcher_state_t disp_st = event_dispatcher_get_state();
    if (disp_st == DISPATCHER_RUNNING || disp_st == DISPATCHER_PAUSED) {
        atomic_set(&g_restart_dispatcher_on_start, 1);
    }

    event_status_t dret = event_dispatcher_stop();
    if (dret != EVENT_OK) {
        LOG_WRN("event_dispatcher_stop during event_system_stop: %d", dret);
        /* 分发器线程仍在运行，重新开放事件入队 gate 以保持系统可用 */
        event_system_ops_open();
        return dret;
    }

    return EVENT_OK;
}

event_status_t event_system_stop(void) {
    if (k_is_in_isr()) {
        return EVENT_ERR_INVALID_ARG;
    }

    if (event_dispatcher_is_current_thread()) {
        LOG_ERR("Cannot stop event system from dispatcher thread");
        return EVENT_ERR_INVALID_ARG;
    }

    event_system_lifecycle_lock_wait();

    if (g_event_system.magic == EVENT_SYSTEM_MAGIC_IDLE || !g_event_system.initialized) {
        event_system_lifecycle_unlock();
        return EVENT_OK;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC) {
        LOG_ERR("Event system magic corruption detected: 0x%08x", g_event_system.magic);
        event_system_lifecycle_unlock();
        return EVENT_ERR_INVALID_ARG;
    }

    /* 未运行且不在 STOPPING 流程中：空闲态幂等返回 */
    if (atomic_get(&g_event_system.running) == 0 && event_system_lifecycle_state() != ZEP_STATE_STOPPING) {
        event_system_lifecycle_unlock();
        return EVENT_OK;
    }

    event_status_t ret = event_system_begin_stopping();
    if (ret != EVENT_OK) {
        return ret;
    }

    atomic_set(&g_event_system.running, 0);
    event_system_ops_close();

    ret = event_system_ops_wait_zero();
    if (ret != EVENT_OK) {
        event_system_lifecycle_unlock();
        return ret;
    }

    ret = event_system_stop_dispatcher();
    if (ret != EVENT_OK) {
        event_system_lifecycle_unlock();
        return ret;
    }

    /* 清空未处理事件（动态负载在此释放）；stop 不销毁队列/订阅表，可再次 start */
    event_queue_purge(g_event_system.event_queue);

    (void) zepl_state_machine_try_transition(&g_event_system.lifecycle, ZEP_STATE_STOPPED);

    event_system_ops_open();
    event_system_lifecycle_unlock();
    LOG_DBG("Event system stopped");
    return EVENT_OK;
}

event_status_t event_system_shutdown(void) {
    if (k_is_in_isr()) {
        return EVENT_ERR_INVALID_ARG;
    }

    if (event_dispatcher_is_current_thread()) {
        LOG_ERR("Cannot shutdown event system from dispatcher thread");
        return EVENT_ERR_INVALID_ARG;
    }

    LOG_DBG("Shutting down event system...");

    event_system_lifecycle_lock_wait();

    if (g_event_system.magic == EVENT_SYSTEM_MAGIC_IDLE || !g_event_system.initialized) {
        event_system_lifecycle_unlock();
        return EVENT_OK;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC) {
        LOG_ERR("Event system magic corruption detected: 0x%08x", g_event_system.magic);
        event_system_lifecycle_unlock();
        return EVENT_ERR_INVALID_ARG;
    }

    event_status_t ret = event_system_begin_stopping();
    if (ret != EVENT_OK) {
        return ret;
    }

    atomic_set(&g_event_system.running, 0);
    event_system_ops_close();

    ret = event_system_ops_wait_zero();
    if (ret != EVENT_OK) {
        event_system_lifecycle_unlock();
        return ret;
    }

    if (event_dispatcher_is_initialized()) {
        event_status_t dret = event_dispatcher_deinit();
        if (dret != EVENT_OK) {
            LOG_ERR("Failed to deinit dispatcher during shutdown: %d", dret);
            event_system_lifecycle_unlock();
            return dret;
        }
    }

    if (event_system_lifecycle_state() == ZEP_STATE_STOPPING) {
        (void) zepl_state_machine_try_transition(&g_event_system.lifecycle, ZEP_STATE_STOPPED);
    }

#if defined(CONFIG_EVENT_TIMER_POOL)
    /* B3：先取消全部定时槽（k_timer_stop + 释放负载），防止队列销毁后到期回调
     * 触碰已 deinit 的队列；与在飞 expiry 的竞态由槽状态机 + gen 校验兜底（单核与 SMP 同） */
    event_timer_pool_cancel_all();
#endif

    /* 顺序约束：须在 magic 置 IDLE 前完成队列清理，否则 event_free_data 可能跳过释放 */
    event_queue_deinit(g_event_system.event_queue);

    event_system_cleanup_event_types();

    event_system_reset_control_block();
    atomic_clear(&g_restart_dispatcher_on_start);

    event_system_lifecycle_unlock();
    LOG_DBG("Event system shutdown complete");
    return EVENT_OK;
}

bool event_system_is_running(void) {
    return atomic_get(&g_event_system.running) != 0;
}
