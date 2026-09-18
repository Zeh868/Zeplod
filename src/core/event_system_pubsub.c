/**
 * @file event_system_pubsub.c
 * @brief 事件类型注册、订阅与订阅者通知
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-05-28
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-05-28       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            拆分 register_type/subscribe 超长函数；标注槽满复用 EVENT_ERR_QUEUE_FULL 的延期计划
 */

#include <zephyr/logging/log.h>
#include <stdint.h>
#include <string.h>
#include "event_system_internal.h"

LOG_MODULE_DECLARE(event_system, CONFIG_SYS_LOG_LEVEL);

static K_MUTEX_DEFINE(g_subscriber_id_lock);

#define EVENT_OP_RETURN(value)                                                                                         \
    do {                                                                                                               \
        event_system_op_exit();                                                                                        \
        return (value);                                                                                                \
    } while (0)

#define EVENT_OP_RETURN_VOID()                                                                                         \
    do {                                                                                                               \
        event_system_op_exit();                                                                                        \
        return;                                                                                                        \
    } while (0)

void event_system_subscriber_id_lock(void) {
    zepl_lock_enter(ZEP_LOCK_LEVEL_TABLE, (uintptr_t) &g_subscriber_id_lock);
    k_mutex_lock(&g_subscriber_id_lock, K_FOREVER);
}

void event_system_subscriber_id_unlock(void) {
    k_mutex_unlock(&g_subscriber_id_lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_TABLE, (uintptr_t) &g_subscriber_id_lock);
}

void event_system_entry_lock(event_type_entry_t* entry) {
    zepl_lock_enter(ZEP_LOCK_LEVEL_ENTRY, (uintptr_t) &entry->lock);
    k_mutex_lock(&entry->lock, K_FOREVER);
}

void event_system_entry_unlock(event_type_entry_t* entry) {
    k_mutex_unlock(&entry->lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_ENTRY, (uintptr_t) &entry->lock);
}

static subscriber_entry_t* find_subscriber(event_type_entry_t* entry, uint32_t subscriber_id) {
    for (uint32_t i = 0; i < CONFIG_EVENT_MAX_SUBSCRIBERS; i++) {
        if (entry->subscribers[i].callback != NULL && entry->subscribers[i].subscriber_id == subscriber_id) {
            return &entry->subscribers[i];
        }
    }
    return NULL;
}

static bool subscriber_id_in_use(uint32_t id) {
    for (int t = 0; t < MAX_EVENT_TYPES; t++) {
        event_type_entry_t* entry = &g_event_system.event_types[t];
        event_system_entry_lock(entry);
        for (uint32_t i = 0; i < CONFIG_EVENT_MAX_SUBSCRIBERS; i++) {
            if (entry->subscribers[i].callback != NULL && entry->subscribers[i].subscriber_id == id) {
                event_system_entry_unlock(entry);
                return true;
            }
        }
        event_system_entry_unlock(entry);
    }
    return false;
}

bool event_system_type_is_registered(event_type_t type) {
    if ((uint32_t) type > MAX_EVENT_TYPE_ID) {
        return false;
    }
    return atomic_get(&g_event_system.event_types[type].registered) != 0;
}

/**
 * @brief 校验注册参数：类型 ID 范围、名称非空与长度
 *
 * @return EVENT_OK 合法，EVENT_ERR_INVALID_ARG 非法
 */
static event_status_t event_register_type_validate(event_type_t type, const char* name) {
    if ((uint32_t) type > MAX_EVENT_TYPE_ID) {
        LOG_ERR("Invalid event type: %d", type);
        return EVENT_ERR_INVALID_ARG;
    }

    if (name == NULL) {
        LOG_ERR("event_register_type: name cannot be NULL");
        return EVENT_ERR_INVALID_ARG;
    }

    size_t name_len = strlen(name);
    if (name_len == 0U || name_len >= CONFIG_EVENT_TYPE_NAME_MAX) {
        LOG_ERR("Event type name invalid length %zu (max %d)", name_len, CONFIG_EVENT_TYPE_NAME_MAX - 1);
        return EVENT_ERR_INVALID_ARG;
    }

    return EVENT_OK;
}

/**
 * @brief 持 entry 锁执行注册：幂等重注册放行，名称冲突拒绝，否则落表
 *
 * @pre 调用方已持 entry 锁；本函数负责在返回前释放该锁
 */
static event_status_t event_register_type_locked(event_type_entry_t* entry, event_type_t type, const char* name) {
    if (entry->name_storage[0] != '\0') {
        if (strcmp(entry->name_storage, name) == 0) {
            entry->name = entry->name_storage;
            atomic_set(&entry->registered, 1);
            event_system_entry_unlock(entry);
            return EVENT_OK;
        }

        /* 仅在名称不一致（异常路径）时才拷贝出快照用于日志，避免幂等重复注册的常见路径做无谓拷贝 */
        char registered_name[CONFIG_EVENT_TYPE_NAME_MAX];

        (void) strncpy(registered_name, entry->name_storage, sizeof(registered_name) - 1U);
        registered_name[sizeof(registered_name) - 1U] = '\0';
        event_system_entry_unlock(entry);
        LOG_WRN("Event type %d already registered with different name '%s' (new '%s')", type, registered_name, name);
        return EVENT_ERR_INVALID_ARG;
    }

    (void) strncpy(entry->name_storage, name, sizeof(entry->name_storage) - 1U);
    entry->name_storage[sizeof(entry->name_storage) - 1U] = '\0';
    entry->name = entry->name_storage;
    atomic_set(&entry->registered, 1);
    entry->subscriber_count = 0;
    memset(entry->subscribers, 0, sizeof(entry->subscribers));

    event_system_entry_unlock(entry);

    LOG_DBG("Registered event type: %s (%d)", name, type);
    return EVENT_OK;
}

event_status_t event_register_type(event_type_t type, const char* name) {
    if (!event_system_op_enter()) {
        return EVENT_ERR_INVALID_ARG;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC || !g_event_system.initialized) {
        EVENT_OP_RETURN(EVENT_ERR_INVALID_ARG);
    }

    event_status_t vret = event_register_type_validate(type, name);
    if (vret != EVENT_OK) {
        EVENT_OP_RETURN(vret);
    }

    event_type_entry_t* entry = &g_event_system.event_types[type];

    event_system_entry_lock(entry);
    event_status_t ret = event_register_type_locked(entry, type, name);
    EVENT_OP_RETURN(ret);
}

event_status_t event_unregister_type(event_type_t type) {
    if (!event_system_op_enter()) {
        return EVENT_ERR_INVALID_ARG;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC || !g_event_system.initialized) {
        EVENT_OP_RETURN(EVENT_ERR_INVALID_ARG);
    }

    if ((uint32_t) type > MAX_EVENT_TYPE_ID) {
        EVENT_OP_RETURN(EVENT_ERR_INVALID_ARG);
    }

    event_type_entry_t* entry = &g_event_system.event_types[type];

    event_system_entry_lock(entry);

    if (atomic_get(&entry->registered) == 0) {
        event_system_entry_unlock(entry);
        EVENT_OP_RETURN(EVENT_ERR_NOT_FOUND);
    }

    if (entry->subscriber_count > 0) {
        event_system_entry_unlock(entry);
        LOG_WRN("Cannot unregister type %d with active subscribers", type);
        EVENT_OP_RETURN(EVENT_ERR_NO_SUBSCRIBER);
    }

    atomic_set(&entry->registered, 0);
    entry->name = NULL;
    /* 清空名称存储：register_type 以 name_storage[0] 作为占用判据，
     * 若此处不清，注销后以不同名字重注册会被误拒为 EVENT_ERR_INVALID_ARG。 */
    entry->name_storage[0] = '\0';

    event_system_entry_unlock(entry);

    LOG_DBG("Unregistered event type: %d", type);
    EVENT_OP_RETURN(EVENT_OK);
}

/**
 * @brief 分配未冲突的订阅者 ID（持 subscriber_id 锁调用，锁由调用方释放）
 *
 * ID 由全局原子计数递增生成，INVALID 值跳过；回绕后（wrapped）须全表查重，
 * 避免与仍存活的订阅冲突。连续尝试超过 UINT16_MAX 次视为 ID 空间耗尽。
 *
 * @param[out] new_id_out 分配成功的 ID
 * @return true 分配成功，false ID 空间耗尽
 */
static bool event_subscribe_alloc_id(uint32_t* new_id_out) {
    uint32_t attempts = 0;

    while (true) {
        uint32_t new_id = (uint32_t) atomic_inc(&g_event_system.next_subscriber_id);

        if (++attempts > UINT16_MAX) {
            LOG_ERR("Subscriber ID space exhausted after %u attempts", attempts);
            return false;
        }
        if (new_id == EVENT_SUBSCRIBER_ID_INVALID) {
            g_event_system.subscriber_id_wrapped = true;
            continue;
        }
        if (!g_event_system.subscriber_id_wrapped || !subscriber_id_in_use(new_id)) {
            *new_id_out = new_id;
            return true;
        }
    }
}

/**
 * @brief 查找空闲订阅槽
 *
 * @param entry 事件类型条目
 * @return 空闲槽下标，-1 表示槽满
 */
static int32_t event_subscribe_find_free_slot(const event_type_entry_t* entry) {
    for (uint32_t i = 0; i < CONFIG_EVENT_MAX_SUBSCRIBERS; i++) {
        if (entry->subscribers[i].callback == NULL) {
            return (int32_t) i;
        }
    }
    return -1;
}

event_status_t event_subscribe(event_type_t type, event_callback_t callback, void* user_data, uint32_t* subscriber_id) {
    if (!event_system_op_enter()) {
        return EVENT_ERR_INVALID_ARG;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC || !g_event_system.initialized) {
        EVENT_OP_RETURN(EVENT_ERR_INVALID_ARG);
    }

    if ((uint32_t) type > MAX_EVENT_TYPE_ID || callback == NULL || subscriber_id == NULL) {
        EVENT_OP_RETURN(EVENT_ERR_INVALID_ARG);
    }

    event_type_entry_t* entry = &g_event_system.event_types[type];

    event_system_subscriber_id_lock();

    uint32_t new_id;
    if (!event_subscribe_alloc_id(&new_id)) {
        event_system_subscriber_id_unlock();
        EVENT_OP_RETURN(EVENT_ERR_NO_MEM);
    }

    event_system_entry_lock(entry);
    if (atomic_get(&entry->registered) == 0) {
        event_system_entry_unlock(entry);
        event_system_subscriber_id_unlock();
        EVENT_OP_RETURN(EVENT_ERR_NOT_FOUND);
    }

    int32_t free_slot = event_subscribe_find_free_slot(entry);

    /* 槽满复用 EVENT_ERR_QUEUE_FULL：tests 依赖该现值，暂不改码；
     * 未来版本拟引入 EVENT_ERR_NO_RESOURCE 区分（已在 event_system.h @note 登记延期） */
    if (free_slot < 0) {
        event_system_entry_unlock(entry);
        event_system_subscriber_id_unlock();
        LOG_ERR("No room for more subscribers on event type %d", type);
        EVENT_OP_RETURN(EVENT_ERR_QUEUE_FULL);
    }

    entry->subscribers[free_slot].callback = callback;
    entry->subscribers[free_slot].user_data = user_data;
    entry->subscribers[free_slot].subscriber_id = new_id;
    entry->subscriber_count++;
    *subscriber_id = new_id;

    event_system_entry_unlock(entry);
    event_system_subscriber_id_unlock();
    LOG_DBG("Subscriber %d registered for event type %d", new_id, type);
    EVENT_OP_RETURN(EVENT_OK);
}

event_status_t event_unsubscribe(event_type_t type, uint32_t subscriber_id) {
    if (!event_system_op_enter()) {
        return EVENT_ERR_INVALID_ARG;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC || !g_event_system.initialized) {
        EVENT_OP_RETURN(EVENT_ERR_INVALID_ARG);
    }

    if ((uint32_t) type > MAX_EVENT_TYPE_ID || subscriber_id == EVENT_SUBSCRIBER_ID_INVALID) {
        EVENT_OP_RETURN(EVENT_ERR_INVALID_ARG);
    }

    event_type_entry_t* entry = &g_event_system.event_types[type];

    event_system_subscriber_id_lock();
    event_system_entry_lock(entry);

    subscriber_entry_t* sub = find_subscriber(entry, subscriber_id);
    if (sub == NULL) {
        event_system_entry_unlock(entry);
        event_system_subscriber_id_unlock();
        EVENT_OP_RETURN(EVENT_ERR_NOT_FOUND);
    }

    sub->callback = NULL;
    sub->user_data = NULL;
    entry->subscriber_count--;

    event_system_entry_unlock(entry);
    event_system_subscriber_id_unlock();
    LOG_DBG("Subscriber %d removed from event type %d", subscriber_id, type);
    EVENT_OP_RETURN(EVENT_OK);
}

void event_unsubscribe_all(uint32_t subscriber_id) {
    if (!event_system_op_enter()) {
        return;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC || !g_event_system.initialized ||
        subscriber_id == EVENT_SUBSCRIBER_ID_INVALID) {
        EVENT_OP_RETURN_VOID();
    }

    event_system_subscriber_id_lock();

    for (int type = 0; type < MAX_EVENT_TYPES; type++) {
        event_type_entry_t* entry = &g_event_system.event_types[type];

        if (atomic_get(&entry->registered) == 0) {
            continue;
        }

        event_system_entry_lock(entry);

        subscriber_entry_t* sub = find_subscriber(entry, subscriber_id);
        if (sub != NULL) {
            sub->callback = NULL;
            sub->user_data = NULL;
            entry->subscriber_count--;
        }

        event_system_entry_unlock(entry);
    }

    event_system_subscriber_id_unlock();

    LOG_DBG("Subscriber %d removed from all event types", subscriber_id);
    EVENT_OP_RETURN_VOID();
}

const char* event_get_type_name(event_type_t type) {
    if (!event_system_op_enter()) {
        return "UNKNOWN";
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC || !g_event_system.initialized ||
        (uint32_t) type > MAX_EVENT_TYPE_ID) {
        EVENT_OP_RETURN("UNKNOWN");
    }

    event_type_entry_t* entry = &g_event_system.event_types[type];
    event_system_entry_lock(entry);
    const char* name = atomic_get(&entry->registered) != 0 ? entry->name_storage : "UNREGISTERED";
    event_system_entry_unlock(entry);
    EVENT_OP_RETURN(name);
}

uint32_t event_get_subscriber_count(event_type_t type) {
    if (!event_system_op_enter()) {
        return 0;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC || !g_event_system.initialized ||
        (uint32_t) type > MAX_EVENT_TYPE_ID) {
        EVENT_OP_RETURN(0);
    }

    event_type_entry_t* entry = &g_event_system.event_types[type];
    event_system_entry_lock(entry);
    uint32_t count = entry->subscriber_count;
    event_system_entry_unlock(entry);

    EVENT_OP_RETURN(count);
}

event_status_t event_notify_subscribers(const event_t* event) {
    if (!event_system_op_enter()) {
        return EVENT_ERR_INVALID_ARG;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC) {
        LOG_ERR("Event system magic corruption detected");
        EVENT_OP_RETURN(EVENT_ERR_INVALID_ARG);
    }
    if (event == NULL || (uint32_t) event->type > MAX_EVENT_TYPE_ID) {
        EVENT_OP_RETURN(EVENT_ERR_INVALID_ARG);
    }

    event_type_entry_t* entry = &g_event_system.event_types[event->type];

    typedef struct {
        event_callback_t cb;
        void*            ud;
    } sub_snap_t;

    sub_snap_t snap[CONFIG_EVENT_MAX_SUBSCRIBERS];
    uint32_t   n = 0U;

    event_system_entry_lock(entry);

    if (entry->subscriber_count == 0) {
        event_system_entry_unlock(entry);
        atomic_inc(&g_event_system.total_events);
        EVENT_OP_RETURN(EVENT_ERR_NO_SUBSCRIBER);
    }

    for (uint32_t i = 0; i < CONFIG_EVENT_MAX_SUBSCRIBERS; i++) {
        subscriber_entry_t* sub = &entry->subscribers[i];

        if (sub->callback != NULL) {
            snap[n].cb = sub->callback;
            snap[n].ud = sub->user_data;
            n++;
        }
    }

    event_system_entry_unlock(entry);

    for (uint32_t i = 0; i < n; i++) {
        if (snap[i].cb != NULL) {
            snap[i].cb(event, snap[i].ud);
        } else {
            LOG_ERR("NULL callback in subscriber snapshot at index %u", i);
        }
    }

    atomic_inc(&g_event_system.total_events);

    EVENT_OP_RETURN(EVENT_OK);
}

#undef EVENT_OP_RETURN_VOID
#undef EVENT_OP_RETURN
