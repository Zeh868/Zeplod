/**
 * @file event_system_publish.c
 * @brief 事件发布与入队
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-05-28
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-05-28       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            补充文件头修改日志段（审查整改）
 */

#include <zephyr/logging/log.h>
#include "event_memory.h"
#include "event_queue.h"
#include "event_system_internal.h"

LOG_MODULE_DECLARE(event_system, CONFIG_SYS_LOG_LEVEL);

#if defined(CONFIG_EVENT_QUEUE_OVERFLOW_DROP_LOWEST)
#define EVENT_PUBLISH_QUEUE_OVERFLOW_POLICY     QUEUE_OVERFLOW_DROP_LOWEST
#define EVENT_PUBLISH_ISR_QUEUE_OVERFLOW_POLICY QUEUE_OVERFLOW_DROP_NEWEST
#elif defined(CONFIG_EVENT_QUEUE_OVERFLOW_BLOCK)
#define EVENT_PUBLISH_QUEUE_OVERFLOW_POLICY     QUEUE_OVERFLOW_BLOCK
#define EVENT_PUBLISH_ISR_QUEUE_OVERFLOW_POLICY QUEUE_OVERFLOW_BLOCK
#else
#define EVENT_PUBLISH_QUEUE_OVERFLOW_POLICY     QUEUE_OVERFLOW_DROP_NEWEST
#define EVENT_PUBLISH_ISR_QUEUE_OVERFLOW_POLICY QUEUE_OVERFLOW_DROP_NEWEST
#endif

#if defined(CONFIG_EVENT_QUEUE_OVERFLOW_BLOCK)
#define EVENT_PUBLISH_ENQUEUE_TIMEOUT K_FOREVER
#else
#define EVENT_PUBLISH_ENQUEUE_TIMEOUT K_NO_WAIT
#endif

static event_status_t event_validate_for_publish(const event_t* event, bool log_failures) {
    if (event == NULL) {
        return EVENT_ERR_INVALID_ARG;
    }

    const uint32_t len = event->data_len;
    const uint8_t  flags = event->flags;
    const uint8_t  storage =
        flags & (EVENT_FLAG_DATA_INLINE | EVENT_FLAG_DATA_DYNAMIC | EVENT_FLAG_DATA_FROM_SLAB | EVENT_FLAG_SLAB_MASK);

    if (len == 0U) {
        if (storage != 0U) {
            if (log_failures) {
                LOG_WRN("publish: data_len=0 but storage flags 0x%02x", storage);
            }
            return EVENT_ERR_INVALID_ARG;
        }
        return EVENT_OK;
    }

    if ((flags & EVENT_FLAG_DATA_INLINE) && (flags & EVENT_FLAG_DATA_DYNAMIC)) {
        if (log_failures) {
            LOG_WRN("publish: both INLINE and DYNAMIC flags set");
        }
        return EVENT_ERR_INVALID_ARG;
    }

    if (flags & EVENT_FLAG_DATA_INLINE) {
        if ((flags & (EVENT_FLAG_DATA_FROM_SLAB | EVENT_FLAG_SLAB_MASK)) != 0U) {
            if (log_failures) {
                LOG_WRN("publish: INLINE data carries slab flags");
            }
            return EVENT_ERR_INVALID_ARG;
        }
        if (len > CONFIG_EVENT_INLINE_DATA_SIZE) {
            if (log_failures) {
                LOG_WRN("publish: INLINE data_len %u exceeds %u", len, CONFIG_EVENT_INLINE_DATA_SIZE);
            }
            return EVENT_ERR_INVALID_ARG;
        }
        return EVENT_OK;
    }

    if (flags & EVENT_FLAG_DATA_DYNAMIC) {
        if (event->data.ptr == NULL) {
            if (log_failures) {
                LOG_WRN("publish: DYNAMIC with NULL data.ptr");
            }
            return EVENT_ERR_INVALID_ARG;
        }
        const uint8_t slab_flags = flags & EVENT_FLAG_SLAB_MASK;
        if ((flags & EVENT_FLAG_DATA_FROM_SLAB) == 0U && slab_flags != 0U) {
            if (log_failures) {
                LOG_WRN("publish: SLAB_MASK without DATA_FROM_SLAB");
            }
            return EVENT_ERR_INVALID_ARG;
        }
        if (flags & EVENT_FLAG_DATA_FROM_SLAB) {
#if EVENT_SLAB_ENABLED && EVENT_SLAB_LARGE_AVAILABLE
            struct k_mem_slab* marked_slab = event_memory_data_slab_from_flag(slab_flags);
            struct k_mem_slab* actual_slab = event_memory_resolve_data_slab_for_ptr(event->data.ptr);

            if (marked_slab == NULL || actual_slab != marked_slab) {
                if (log_failures) {
                    LOG_WRN("publish: invalid slab ownership for ptr %p (flags=0x%02x)", event->data.ptr, flags);
                }
                return EVENT_ERR_INVALID_ARG;
            }
#else
            if (log_failures) {
                LOG_WRN("publish: DATA_FROM_SLAB while data slabs are disabled");
            }
            return EVENT_ERR_INVALID_ARG;
#endif
        }
        return EVENT_OK;
    }

    if (log_failures) {
        LOG_WRN("publish: data_len=%u without INLINE/DYNAMIC flags", len);
    }
    return EVENT_ERR_INVALID_ARG;
}

static void event_publish_transfer_data_ownership(event_t* event) {
    if (event == NULL) {
        return;
    }
    event->flags &= ~(EVENT_FLAG_DATA_DYNAMIC | EVENT_FLAG_DATA_FROM_SLAB | EVENT_FLAG_SLAB_MASK);
}

static event_status_t event_publish_common(event_t* event, queue_overflow_policy_t policy, k_timeout_t timeout,
                                           bool log_failures) {
    event_status_t status = EVENT_OK;

    if (!event_system_op_enter()) {
        return EVENT_ERR_NOT_RUNNING;
    }

    if (g_event_system.magic != EVENT_SYSTEM_MAGIC) {
        status = EVENT_ERR_INVALID_ARG;
        goto out;
    }
    if (!g_event_system.initialized || event == NULL) {
        status = EVENT_ERR_INVALID_ARG;
        goto out;
    }

    if (atomic_get(&g_event_system.running) == 0) {
#ifndef CONFIG_EVENT_SYSTEM_LOG_MINIMAL
        if (log_failures) {
            LOG_WRN("Event system not running, event dropped");
        }
#endif
        status = EVENT_ERR_NOT_RUNNING;
        goto out;
    }

    if ((uint32_t) event->type > MAX_EVENT_TYPE_ID) {
        status = EVENT_ERR_INVALID_ARG;
#ifndef CONFIG_EVENT_SYSTEM_LOG_MINIMAL
        if (log_failures) {
            LOG_WRN("Invalid event type id %u (max %u)", (unsigned int) event->type, (unsigned int) MAX_EVENT_TYPE_ID);
        }
#endif
        goto out;
    }

    status = event_validate_for_publish(event, log_failures);
    if (status != EVENT_OK) {
        goto out;
    }

    if (!event_system_type_is_registered(event->type)) {
        status = EVENT_ERR_NOT_FOUND;
#ifndef CONFIG_EVENT_SYSTEM_LOG_MINIMAL
        if (log_failures) {
            LOG_WRN("Publishing to unregistered event type: %d", event->type);
        }
#endif
        goto out;
    }

    status = event_queue_enqueue(g_event_system.event_queue, event, policy, timeout);
    if (status == EVENT_OK) {
        event_publish_transfer_data_ownership(event);
    }

out:
    event_system_op_exit();
    return status;
}

event_status_t event_publish(event_t* event) {
    return event_publish_common(event, EVENT_PUBLISH_QUEUE_OVERFLOW_POLICY, EVENT_PUBLISH_ENQUEUE_TIMEOUT, true);
}

event_status_t event_publish_from_isr(event_t* event) {
    return event_publish_common(event, EVENT_PUBLISH_ISR_QUEUE_OVERFLOW_POLICY, K_NO_WAIT, false);
}

event_status_t event_publish_copy(event_type_t type, event_priority_t priority, const void* data, size_t data_len) {
    EVENT_SYSTEM_VALIDATE();
    event_t* event = event_create_with_data(type, priority, data, data_len);
    if (event == NULL) {
        return EVENT_ERR_NO_MEM;
    }

    event_status_t status = event_publish(event);

    event_free(event);
    return status;
}

event_status_t event_publish_copy_rt(event_type_t type, event_priority_t priority, const void* data, size_t data_len) {
    EVENT_SYSTEM_VALIDATE();
    event_t* event = event_create_with_data_rt(type, priority, data, data_len);
    if (event == NULL) {
        return EVENT_ERR_NO_MEM;
    }

    event_status_t status = event_publish(event);

    event_free(event);
    return status;
}
