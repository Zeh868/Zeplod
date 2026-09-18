/**
 * @file event_system_memory.c
 * @brief 事件创建、释放与负载内存管理
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-05-28
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-05-28       1.1            zeh            收敛 event 壳初始化与 inline/slab 负载附着 helper（P4.1）
 * 2026-09-18       1.2            zeh            提取 EVENT_DATA_LEN_MAX 具名常量；inline 负载附着改为 void 返回
 *
 */

#include <zephyr/logging/log.h>
#include <string.h>
#include "event_memory.h"
#include "event_system_internal.h"

LOG_MODULE_DECLARE(event_system, CONFIG_SYS_LOG_LEVEL);

/**
 * 事件负载数据长度上限（字节）：64KB-1。
 * 沿用初版实现的保守上限（对齐 16 位长度字段的常见协议约束）；event_t.data_len
 * 声明为 uint32_t，负载校验层仍限制在 64KB 内，超出按参数错误拒绝。
 */
#define EVENT_DATA_LEN_MAX 65535U

#define EVENT_SYSTEM_CHECK_MAGIC_FREE_VOID()                                                                           \
    do {                                                                                                               \
        if (g_event_system.magic == EVENT_SYSTEM_MAGIC_IDLE) {                                                         \
            return;                                                                                                    \
        }                                                                                                              \
        if (g_event_system.magic != EVENT_SYSTEM_MAGIC) {                                                              \
            if (!k_is_in_isr()) {                                                                                      \
                LOG_ERR("Event system magic corruption detected");                                                     \
            }                                                                                                          \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

#if EVENT_SLAB_ENABLED
static const char* event_slab_name_for_priority(event_priority_t priority) {
    switch (priority) {
#if EVENT_SLAB_CRITICAL_AVAILABLE
    case EVENT_PRIORITY_CRITICAL:
        return "event_slab_critical";
#endif
#if EVENT_SLAB_HIGH_AVAILABLE
    case EVENT_PRIORITY_HIGH:
        return "event_slab_high";
#endif
    case EVENT_PRIORITY_NORMAL:
    case EVENT_PRIORITY_LOW:
    default:
        return "event_slab_normal";
    }
}
#endif

static void event_object_init(event_t* event, event_type_t type, event_priority_t priority, uint8_t flags) {
    event->type = type;
    event->priority = priority;
    event->timestamp = k_uptime_get_32();
    event->source_id = 0;
    event->data_len = 0;
    event->flags = flags;
    event->reserved = 0;
    memset(event->data.inline_data, 0, CONFIG_EVENT_INLINE_DATA_SIZE);
}

static bool event_validate_data_len(size_t data_len) {
    if (data_len > EVENT_DATA_LEN_MAX) {
        if (!k_is_in_isr()) {
            LOG_ERR("Event data length %zu exceeds maximum 64KB", data_len);
        }
        return false;
    }
    return true;
}

/* 将负载内联附着到事件壳（调用方保证参数合法：data_len ≤ INLINE_DATA_SIZE，恒成功） */
static void event_attach_inline_payload(event_t* event, const void* data, size_t data_len) {
    __ASSERT_NO_MSG(event != NULL);
    __ASSERT_NO_MSG(data != NULL);
    __ASSERT_NO_MSG(data_len <= CONFIG_EVENT_INLINE_DATA_SIZE);
    memcpy(event->data.inline_data, data, data_len);
    event->data_len = (uint32_t) data_len;
    event->flags |= EVENT_FLAG_DATA_INLINE;
}

#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE
static bool event_attach_slab_data(event_t* event, struct k_mem_slab* slab, const void* data, size_t data_len) {
    if (event == NULL || slab == NULL || data == NULL) {
        return false;
    }

    if (k_mem_slab_alloc(slab, &event->data.ptr, K_NO_WAIT) != 0) {
        return false;
    }

    event->flags |= EVENT_FLAG_DATA_DYNAMIC | EVENT_FLAG_DATA_FROM_SLAB;
    if (!event_memory_data_slab_set_flag(event, slab)) {
        k_mem_slab_free(slab, event->data.ptr);
        event->data.ptr = NULL;
        event->flags &= ~(EVENT_FLAG_DATA_DYNAMIC | EVENT_FLAG_DATA_FROM_SLAB | EVENT_FLAG_SLAB_MASK);
        return false;
    }

    memcpy(event->data.ptr, data, data_len);
    event->data_len = (uint32_t) data_len;
    event_debug_track_alloc(event->data.ptr, data_len, event->priority);
    return true;
}

/**
 * @brief 为大负载尝试 slab（含 fallback slab）；仅 RT/ISR 路径使用，失败由调用方释放 event
 */
static bool event_attach_large_payload_slab_only(event_t* event, const void* data, size_t data_len,
                                                 event_priority_t priority) {
    struct k_mem_slab* data_slab = event_memory_select_data_slab(data_len);

    if (data_slab != NULL && event_attach_slab_data(event, data_slab, data, data_len)) {
        return true;
    }
    if (data_slab != NULL) {
        event_memory_notify_slab_exhausted(priority, "event_slab_data");
    }

    data_slab = event_memory_select_data_slab_with_fallback(data_len);
    if (data_slab != NULL && event_attach_slab_data(event, data_slab, data, data_len)) {
        return true;
    }

    event_memory_notify_slab_exhausted(priority, "event_slab_data");
    if (!k_is_in_isr()) {
        LOG_WRN("All data slabs exhausted for size %zu", data_len);
    }
    return false;
}

/**
 * @brief 为大负载尝试 slab（含 fallback）；调用方须已分配 event 壳
 */
static bool event_attach_large_payload_slab_first(event_t* event, const void* data, size_t data_len,
                                                  event_priority_t priority) {
    struct k_mem_slab* data_slab = event_memory_select_data_slab(data_len);

    if (data_slab == NULL) {
        return false;
    }

    if (event_attach_slab_data(event, data_slab, data, data_len)) {
        return true;
    }

    event_memory_notify_slab_exhausted(priority, "event_slab_data");
    struct k_mem_slab* fallback_slab = event_memory_select_data_slab_with_fallback(data_len);

    if (fallback_slab != NULL && event_attach_slab_data(event, fallback_slab, data, data_len)) {
        return true;
    }

    return false;
}
#endif /* EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE */

event_t* event_create_rt(event_type_t type, event_priority_t priority) {
    EVENT_SYSTEM_CHECK_MAGIC_ALLOC();
    event_t* event = NULL;

#if EVENT_SLAB_ENABLED
    struct k_mem_slab* slab = event_memory_select_event_slab(priority);
    int                ret = k_mem_slab_alloc(slab, (void**) &event, K_NO_WAIT);

    if (ret != 0) {
        event_memory_notify_slab_exhausted(priority, event_slab_name_for_priority(priority));
        if (!k_is_in_isr()) {
            LOG_WRN("Event slab exhausted for priority %d", priority);
        }
        return NULL;
    }

    event_object_init(event, type, priority, EVENT_FLAG_FROM_SLAB);
#else
    if (!k_is_in_isr()) {
        LOG_DBG("event_create_rt: slab not enabled, returning NULL");
    }
    return NULL;
#endif

    event_debug_track_alloc(event, sizeof(event_t), priority);

    return event;
}

event_t* event_create_with_data_rt(event_type_t type, event_priority_t priority, const void* data, size_t data_len) {
    EVENT_SYSTEM_CHECK_MAGIC_ALLOC();
    if (data == NULL || data_len == 0) {
        return event_create_rt(type, priority);
    }

    if (!event_validate_data_len(data_len)) {
        return NULL;
    }

    event_t* event = event_create_rt(type, priority);
    if (event == NULL) {
        return NULL;
    }

    if (data_len <= CONFIG_EVENT_INLINE_DATA_SIZE) {
        event_attach_inline_payload(event, data, data_len);
        return event;
    }

#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE
    if (event_attach_large_payload_slab_only(event, data, data_len, priority)) {
        return event;
    }
    event_free(event);
    return NULL;
#else
    event_free(event);
    if (!k_is_in_isr()) {
        LOG_WRN("Large data requested but no slab configured");
    }
    return NULL;
#endif
}

event_t* event_create_from_isr(event_type_t type, event_priority_t priority, const void* data, size_t data_len) {
    if (g_event_system.magic == EVENT_SYSTEM_MAGIC_IDLE) {
        return NULL;
    }
    if (g_event_system.magic != EVENT_SYSTEM_MAGIC) {
        return NULL;
    }
    return event_create_with_data_rt(type, priority, data, data_len);
}

event_t* event_create(event_type_t type, event_priority_t priority) {
    EVENT_SYSTEM_CHECK_MAGIC_ALLOC();
    event_t* event = event_create_rt(type, priority);
    if (event != NULL) {
        return event;
    }

    event = k_malloc(sizeof(event_t));
    if (event == NULL) {
        LOG_ERR("k_malloc failed for event_t");
        return NULL;
    }

    event_object_init(event, type, priority, 0);

    event_debug_track_alloc(event, sizeof(event_t), priority);

    return event;
}

event_t* event_create_with_data(event_type_t type, event_priority_t priority, const void* data, size_t data_len) {
    EVENT_SYSTEM_CHECK_MAGIC_ALLOC();
    if (data == NULL || data_len == 0) {
        return event_create(type, priority);
    }

    if (!event_validate_data_len(data_len)) {
        return NULL;
    }

    if (data_len <= CONFIG_EVENT_INLINE_DATA_SIZE) {
        event_t* event = event_create(type, priority);

        if (event == NULL) {
            return NULL;
        }
        event_attach_inline_payload(event, data, data_len);
        return event;
    }

#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE
    {
        struct k_mem_slab* data_slab = event_memory_select_data_slab(data_len);

        if (data_slab != NULL) {
            event_t* event = event_create(type, priority);

            if (event != NULL) {
                if (event_attach_large_payload_slab_first(event, data, data_len, priority)) {
                    return event;
                }
                event_free(event);
            }
        }
    }
#endif

    event_t* event = k_malloc(sizeof(event_t));
    if (event == NULL) {
        return NULL;
    }

    event_object_init(event, type, priority, EVENT_FLAG_DATA_DYNAMIC);

    event->data.ptr = k_malloc(data_len);
    if (event->data.ptr == NULL) {
        k_free(event);
        return NULL;
    }

    event_memory_inc_fallback_count();

    event->data_len = (uint32_t) data_len;
    memcpy(event->data.ptr, data, data_len);

    event_debug_track_alloc(event, sizeof(event_t), priority);
    event_debug_track_alloc(event->data.ptr, data_len, priority);

    return event;
}

void event_free_data(event_t* event) {
    if (event == NULL) {
        return;
    }

    if ((event->flags & EVENT_FLAG_DATA_DYNAMIC) && event->data.ptr != NULL) {
        event_debug_untrack_alloc(event->data.ptr);
#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE
        if (event->flags & EVENT_FLAG_DATA_FROM_SLAB) {
            struct k_mem_slab* slab = event_memory_data_slab_from_flag(event->flags & EVENT_FLAG_SLAB_MASK);

            if (slab == NULL) {
                LOG_ERR("Unknown slab marker for ptr %p (flags=0x%02x)", event->data.ptr, event->flags);
                slab = event_memory_resolve_data_slab_for_ptr(event->data.ptr);
                if (slab == NULL) {
                    LOG_ERR("Cannot resolve slab pool for ptr %p; memory may leak", event->data.ptr);
                }
            }
            if (slab != NULL) {
                k_mem_slab_free(slab, event->data.ptr);
            }
        } else
#endif
        {
            k_free(event->data.ptr);
        }
        event->data.ptr = NULL;
        event->flags &= ~(EVENT_FLAG_DATA_DYNAMIC | EVENT_FLAG_DATA_FROM_SLAB | EVENT_FLAG_SLAB_MASK);
    }
}

void event_free(event_t* event) {
    if (event == NULL) {
        return;
    }

    event_free_data(event);

    event_debug_untrack_alloc(event);

    if (event->flags & EVENT_FLAG_FROM_SLAB) {
#if EVENT_SLAB_ENABLED
        struct k_mem_slab* slab = event_memory_resolve_event_slab_for_ptr(event);
        if (slab == NULL) {
            LOG_ERR("Cannot resolve event slab for ptr %p; memory may leak", event);
            return;
        }
        k_mem_slab_free(slab, (void*) event);
#else
        LOG_ERR("Event %p has FROM_SLAB flag but slab is disabled; falling back to k_free", event);
        k_free(event);
#endif
    } else {
        k_free(event);
    }
}
