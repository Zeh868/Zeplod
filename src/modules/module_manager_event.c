/**
 * @file module_manager_event.c
 * @brief 模块事件订阅与分发
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-06
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-06-06       1.1            zeh            引入稳定订阅 cookie 与 draining 协议
 * 2026-09-18       1.2            zeh            订阅竞态回退路径检查 event_unsubscribe 返回值并告警
 */

#include <zephyr/logging/log.h>
#include <string.h>
#include <zeplod/event_system.h>
#include "module_manager_internal.h"

LOG_MODULE_DECLARE(module_manager, CONFIG_SYS_LOG_LEVEL);

static void module_event_handler(const event_t* event, void* user_data);

/* 订阅竞态回退：订阅已在事件系统生效但槽位校验失败，必须退订以免订阅泄漏。
 * 退订失败仅告警不改变原错误返回——此时残留的订阅会因 cookie 找不到槽位而被
 * module_event_handler 安全丢弃。 */
static void rollback_event_subscription(event_type_t event_type, uint32_t subscriber_id) {
    const event_status_t status = event_unsubscribe(event_type, subscriber_id);
    if (status != EVENT_OK) {
        LOG_WRN("rollback event_unsubscribe(type=%d, sub_id=%u) failed: %d", (int) event_type,
                (unsigned int) subscriber_id, (int) status);
    }
}

uint8_t module_event_detach_locked(module_info_t* info, event_type_t* types_out, uint32_t* ids_out, uint8_t max_out) {
    if (info == NULL || types_out == NULL || ids_out == NULL || max_out == 0U) {
        return 0U;
    }

    uint8_t n = info->event_subscription_count;
    if (n > max_out) {
        n = max_out;
    }

    for (uint8_t i = 0U; i < n; i++) {
        types_out[i] = info->event_subscriptions[i].type;
        ids_out[i] = info->event_subscriptions[i].subscriber_id;
    }

    info->event_subscription_count = 0U;
    (void) memset(info->event_subscriptions, 0, sizeof(info->event_subscriptions));
    return n;
}

int module_event_unsubscribe_batch(const event_type_t* types, const uint32_t* ids, uint8_t count) {
    if (types == NULL || ids == NULL) {
        return MODULE_ERR_INVALID_ARG;
    }

    int result = MODULE_OK;

    for (uint8_t i = 0U; i < count; i++) {
        const event_status_t status = event_unsubscribe(types[i], ids[i]);
        if (status != EVENT_OK) {
            LOG_WRN("event_unsubscribe(type=%d, sub_id=%u) failed: status=%d", (int) types[i], (unsigned int) ids[i],
                    (int) status);
            result = event_status_to_module_error(status);
        }
    }
    return result;
}

int find_event_sub_index(const module_info_t* info, event_type_t type) {
    for (uint8_t i = 0; i < info->event_subscription_count; i++) {
        if (info->event_subscriptions[i].type == type) {
            return (int) i;
        }
    }
    return -1;
}

static int find_event_cookie_locked(uint32_t cookie, module_info_t** out_info) {
    if (cookie == 0U || out_info == NULL) {
        return -1;
    }

    for (int mi = 0; mi < CONFIG_MAX_MODULES; mi++) {
        module_info_t* info = &g_module_mgr.modules[mi];

        for (uint8_t si = 0U; si < info->event_subscription_count; si++) {
            if (info->event_subscriptions[si].cookie == cookie) {
                *out_info = info;
                return (int) si;
            }
        }
    }
    return -1;
}

int event_status_to_module_error(event_status_t status) {
    switch (status) {
    case EVENT_OK:
        return MODULE_OK;
    case EVENT_ERR_NO_MEM:
        return MODULE_ERR_NO_MEM;
    case EVENT_ERR_INVALID_ARG:
        return MODULE_ERR_INVALID_ARG;
    case EVENT_ERR_NOT_FOUND:
        return MODULE_ERR_NOT_FOUND;
    case EVENT_ERR_NO_SUBSCRIBER:
        return MODULE_ERR_NOT_FOUND;
    case EVENT_ERR_TIMEOUT:
        return MODULE_ERR_TIMEOUT;
    case EVENT_ERR_NOT_RUNNING:
        return MODULE_ERR_NOT_RUNNING;
    case EVENT_ERR_QUEUE_FULL:
    case EVENT_ERR_QUEUE_EMPTY:
    default:
        return MODULE_ERR_IO;
    }
}

int module_manager_subscribe(uint32_t module_id, event_type_t event_type) {
    if (!atomic_get(&g_module_mgr_initialized)) {
        return MODULE_ERR_NOT_INITIALIZED;
    }

    module_manager_lock();

    module_info_t* info = find_module_by_id_locked(module_id);

    if (info == NULL || info->interface == NULL || info->interface->on_event == NULL) {
        module_manager_unlock();
        return MODULE_ERR_NOT_FOUND;
    }

    /* 门控：manager 处于 STOPPING / ERROR 时拒绝新订阅（#6）。 */
    const zepl_state_t mgr_state = module_manager_lifecycle_state_locked();
    if (!module_manager_lifecycle_allows_op(mgr_state)) {
        module_manager_unlock();
        return MODULE_ERR_INVALID_STATE;
    }

    if (atomic_get(&g_module_mgr_shutting_down) != 0 || info->unregistering || atomic_get(&info->draining) != 0) {
        module_manager_unlock();
        return MODULE_ERR_BUSY;
    }

    if (find_event_sub_index(info, event_type) >= 0) {
        module_manager_unlock();
        return MODULE_ERR_BUSY;
    }

    if (info->event_subscription_count >= CONFIG_MODULE_MAX_EVENT_SUBSCRIPTIONS) {
        module_manager_unlock();
        return MODULE_ERR_NO_MEM;
    }

    const uint16_t slot_index = (uint16_t) (info - g_module_mgr.modules);
    const uint32_t slot_gen = info->generation;
    const uint32_t cookie = allocate_event_cookie_locked();
    if (cookie == 0U) {
        module_manager_unlock();
        return MODULE_ERR_NO_MEM;
    }

    module_manager_unlock();

    uint32_t             subscriber_id = 0U;
    const event_status_t status =
        event_subscribe(event_type, module_event_handler, mm_event_token_make(cookie), &subscriber_id);

    if (status != EVENT_OK) {
        return event_status_to_module_error(status);
    }

    module_manager_lock();

    info = find_module_by_id_locked(module_id);
    if (info == NULL || (uint16_t) (info - g_module_mgr.modules) != slot_index || info->generation != slot_gen ||
        info->unregistering || atomic_get(&info->draining) != 0) {
        module_manager_unlock();
        rollback_event_subscription(event_type, subscriber_id);
        return MODULE_ERR_NOT_FOUND;
    }

    if (info->event_subscription_count >= CONFIG_MODULE_MAX_EVENT_SUBSCRIPTIONS ||
        find_event_sub_index(info, event_type) >= 0) {
        module_manager_unlock();
        rollback_event_subscription(event_type, subscriber_id);
        return MODULE_ERR_BUSY;
    }

    const uint8_t idx = info->event_subscription_count;

    info->event_subscriptions[idx].type = event_type;
    info->event_subscriptions[idx].subscriber_id = subscriber_id;
    info->event_subscriptions[idx].cookie = cookie;
    info->event_subscriptions[idx].removing = false;
    info->event_subscription_count++;

    module_manager_unlock();
    return MODULE_OK;
}

int module_manager_unsubscribe(uint32_t module_id, event_type_t event_type) {
    if (!atomic_get(&g_module_mgr_initialized)) {
        return MODULE_ERR_NOT_INITIALIZED;
    }

    module_manager_lock();

    module_info_t* info = find_module_by_id_locked(module_id);

    if (info == NULL) {
        module_manager_unlock();
        return MODULE_ERR_NOT_FOUND;
    }

    if (info->unregistering || atomic_get(&info->draining) != 0) {
        module_manager_unlock();
        return MODULE_ERR_BUSY;
    }

    const int idx = find_event_sub_index(info, event_type);

    if (idx < 0) {
        module_manager_unlock();
        return MODULE_ERR_NOT_FOUND;
    }

    if (info->event_subscriptions[idx].removing) {
        module_manager_unlock();
        return MODULE_ERR_BUSY;
    }

    const uint32_t sub_id = info->event_subscriptions[idx].subscriber_id;
    const uint32_t cookie = info->event_subscriptions[idx].cookie;
    info->event_subscriptions[idx].removing = true;

    module_manager_unlock();

    const event_status_t status = event_unsubscribe(event_type, sub_id);
    if (status != EVENT_OK) {
        LOG_WRN("event_unsubscribe(type=%d, sub_id=%u) failed: status=%d", (int) event_type, (unsigned int) sub_id,
                (int) status);
        module_manager_lock();
        info = find_module_by_id_locked(module_id);
        if (info != NULL) {
            for (uint8_t i = 0U; i < info->event_subscription_count; i++) {
                if (info->event_subscriptions[i].cookie == cookie) {
                    info->event_subscriptions[i].removing = false;
                    break;
                }
            }
        }
        module_manager_unlock();
        return event_status_to_module_error(status);
    }

    module_manager_lock();
    info = find_module_by_id_locked(module_id);
    if (info == NULL) {
        module_manager_unlock();
        return MODULE_ERR_NOT_FOUND;
    }

    int committed_idx = -1;
    for (uint8_t i = 0U; i < info->event_subscription_count; i++) {
        if (info->event_subscriptions[i].cookie == cookie) {
            committed_idx = (int) i;
            break;
        }
    }
    if (committed_idx < 0) {
        module_manager_unlock();
        return MODULE_ERR_NOT_FOUND;
    }

    const uint8_t last = (uint8_t) (info->event_subscription_count - 1U);
    if ((uint8_t) committed_idx != last) {
        info->event_subscriptions[committed_idx] = info->event_subscriptions[last];
    }
    (void) memset(&info->event_subscriptions[last], 0, sizeof(info->event_subscriptions[last]));
    info->event_subscription_count = last;
    module_manager_unlock();

    return MODULE_OK;
}

int module_manager_send_to_module(uint32_t module_id, const event_t* event) {
    if (!atomic_get(&g_module_mgr_initialized)) {
        return MODULE_ERR_NOT_INITIALIZED;
    }

    if (event == NULL) {
        module_manager_lock();
        g_module_mgr.stats.events_dropped++;
        module_manager_unlock();
        return MODULE_ERR_INVALID_ARG;
    }

    module_manager_lock();

    module_info_t* info = find_module_by_id_locked(module_id);

    if (info == NULL || info->interface == NULL) {
        g_module_mgr.stats.events_dropped++;
        module_manager_unlock();
        return MODULE_ERR_NOT_FOUND;
    }

    /* 修复 #3：draining 标记后新分发必须跳过，防止注销与分发竞争。 */
    if (atomic_get(&info->draining) != 0 || info->status != MODULE_STATUS_RUNNING) {
        g_module_mgr.stats.events_dropped++;
        module_manager_unlock();
        return MODULE_ERR_NOT_RUNNING;
    }

    if (info->interface->on_event == NULL) {
        g_module_mgr.stats.events_dropped++;
        module_manager_unlock();
        return MODULE_ERR_INVALID_ARG;
    }

    module_event_handler_t handler = info->interface->on_event;
    void*                  idata = info->internal_data;
    const uint32_t         slot_gen = info->generation;

    g_module_mgr.stats.events_processed++;
    atomic_inc(&info->in_flight);
    module_manager_unlock();

    handler(event, idata);

    /* 锁外回调已返回。注销路径会等 in_flight 归零后才能调 shutdown；
     * 此处 dec 必须持锁以保证对清槽可见。 */
    module_manager_lock();
    if (info->interface != NULL && info->generation == slot_gen) {
        atomic_dec(&info->in_flight);
    }
    module_manager_unlock();

    return MODULE_OK;
}

int module_manager_broadcast(const event_t* event) {
    if (!atomic_get(&g_module_mgr_initialized)) {
        return MODULE_ERR_NOT_INITIALIZED;
    }

    if (event == NULL) {
        return MODULE_ERR_INVALID_ARG;
    }

    /* 修复 #3：批量分发时 per-slot 维护 in_flight，注销路径据此等待回调结束。
     * 流程：锁内收集 (handler, idata, gen) 并对每个目标 inc；锁外逐个调用；
     * 再次持锁对每个目标 dec，generation 不匹配的槽位（已被清/复用）跳过。 */
    module_event_handler_t handlers[CONFIG_MAX_MODULES];
    void*                  datas[CONFIG_MAX_MODULES];
    uint32_t               gens[CONFIG_MAX_MODULES];
    uint16_t               indices[CONFIG_MAX_MODULES];
    int                    n = 0;

    module_manager_lock();
    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        module_info_t* info = &g_module_mgr.modules[i];
        if (atomic_get(&info->draining) != 0) {
            continue;
        }
        if (info->status != MODULE_STATUS_RUNNING || info->interface == NULL || info->interface->on_event == NULL) {
            continue;
        }
        handlers[n] = info->interface->on_event;
        datas[n] = info->internal_data;
        gens[n] = info->generation;
        indices[n] = (uint16_t) i;
        atomic_inc(&info->in_flight);
        n++;
    }
    g_module_mgr.stats.events_processed += (uint32_t) n;
    module_manager_unlock();

    for (int i = 0; i < n; i++) {
        handlers[i](event, datas[i]);
    }

    module_manager_lock();
    for (int i = 0; i < n; i++) {
        module_info_t* info = &g_module_mgr.modules[indices[i]];
        if (info->generation == gens[i]) {
            atomic_dec(&info->in_flight);
        }
        /* generation 不匹配说明槽位已被清空（在 unregister 路径中
         * clear_module_slot_locked 会把 in_flight 复位为 0），无需 dec。 */
    }
    module_manager_unlock();

    return n;
}

static void module_event_handler(const event_t* event, void* user_data) {
    if (event == NULL || user_data == NULL) {
        return;
    }

    uint32_t cookie = 0U;
    if (!mm_event_token_decode(user_data, &cookie)) {
        return;
    }

    module_manager_lock();

    module_info_t* info = NULL;
    const int      sub_idx = find_event_cookie_locked(cookie, &info);
    if (sub_idx < 0 || info == NULL || info->event_subscriptions[sub_idx].removing) {
        g_module_mgr.stats.events_dropped++;
        module_manager_unlock();
        return;
    }
    if (atomic_get(&info->draining) != 0 || info->status != MODULE_STATUS_RUNNING || info->interface == NULL ||
        info->interface->on_event == NULL) {
        g_module_mgr.stats.events_dropped++;
        module_manager_unlock();
        return;
    }

    module_event_handler_t handler = info->interface->on_event;
    void*                  idata = info->internal_data;
    const uint32_t         slot_gen = info->generation;

    g_module_mgr.stats.events_processed++;
    atomic_inc(&info->in_flight);
    module_manager_unlock();

    handler(event, idata);

    /* 修复 #3：注销路径在 in_flight 归零后才调 shutdown；此处持锁 dec 保证对清槽可见。 */
    module_manager_lock();
    if (info->interface != NULL && info->generation == slot_gen) {
        atomic_dec(&info->in_flight);
    }
    module_manager_unlock();
}
