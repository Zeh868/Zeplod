/**
 * @file ipc_pending_table.c
 * @brief IPC pending 表与 future 对象池实现
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-05-28
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-05-28       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            补修改日志段
 */

#include "ipc_pending_table.h"
#include "ipc_service_internal.h"

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(thread_ipc_svc, CONFIG_THREAD_IPC_SERVICE_LOG_LEVEL);

ipc_pending_request_t* ipc_pending_table_find(ipc_service_t* service, ipc_request_id_t request_id) {
    for (int i = 0; i < CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS; i++) {
        if (service->pending_requests[i].in_use && service->pending_requests[i].request_id == request_id) {
            return &service->pending_requests[i];
        }
    }
    return NULL;
}

ipc_pending_request_t* ipc_pending_table_alloc(ipc_service_t* service) {
    for (int i = 0; i < CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS; i++) {
        if (!service->pending_requests[i].in_use) {
            return &service->pending_requests[i];
        }
    }
    return NULL;
}

void ipc_pending_table_init_entry(ipc_service_t* service, ipc_pending_request_t* entry, ipc_request_id_t request_id,
                                  struct k_thread* caller_thread, ipc_async_callback_t callback,
                                  void* callback_user_data, ipc_future_t* future) {
    ARG_UNUSED(service);
    entry->request_id = request_id;
    entry->caller_thread = caller_thread;
    entry->callback = callback;
    entry->callback_user_data = callback_user_data;
    entry->future = future;
    entry->result = 0;
    entry->response_data = NULL;
    entry->response_data_size = 0;
    entry->in_use = true;
    entry->completed = false;
    entry->canceled = false;
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
    entry->shm_handle = 0;
#endif
    k_sem_init(&entry->response_sem, 0, 1);
}

#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
void ipc_pending_table_release_shm(ipc_service_t* service, ipc_pending_request_t* entry) {
    if (entry->shm_handle != 0) {
        LOG_DBG("ipc_pending_table_release: shm_handle %u", entry->shm_handle);
        ipc_shm_release(service, entry->shm_handle);
        entry->shm_handle = 0;
    }
}
#endif

void ipc_pending_table_release(ipc_service_t* service, ipc_pending_request_t* entry) {
    ARG_UNUSED(service);
    entry->in_use = false;
    entry->completed = false;
    entry->canceled = false;
    entry->callback = NULL;
    entry->future = NULL;
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
    ipc_pending_table_release_shm(service, entry);
#endif
}

ipc_future_t* ipc_pending_table_alloc_future(ipc_service_t* service) {
    if (service->free_futures == NULL) {
        return NULL;
    }

    ipc_future_t* future = service->free_futures;

    service->free_futures = future->next;
    future->next = NULL;

    return future;
}

void ipc_pending_table_release_future(ipc_service_t* service, ipc_future_t* future) {
    future->request_id = 0U;
    atomic_set(&future->completed, 0);
    future->result = 0;
    future->data = NULL;
    future->data_size = 0;
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
    if (future->shm_handle != 0) {
        LOG_DBG("ipc_pending_table_release_future: shm_handle %u", future->shm_handle);
        ipc_shm_release(service, future->shm_handle);
        future->shm_handle = 0;
    }
#endif
    future->next = service->free_futures;
    service->free_futures = future;
}

bool ipc_pending_table_future_belongs(const ipc_service_t* service, const ipc_future_t* future) {
    for (int i = 0; i < CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS; i++) {
        if (future == &service->futures[i]) {
            return true;
        }
    }
    return false;
}

bool ipc_pending_table_future_in_free_list(const ipc_service_t* service, const ipc_future_t* future) {
    const ipc_future_t* it = service->free_futures;

    while (it != NULL) {
        if (it == future) {
            return true;
        }
        it = it->next;
    }

    return false;
}

size_t ipc_pending_table_count(ipc_service_t* service) {
    if (service == NULL) {
        return 0;
    }

    size_t count = 0;

    ipc_service_pending_lock(service);

    for (int i = 0; i < CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS; i++) {
        if (service->pending_requests[i].in_use) {
            count++;
        }
    }

    ipc_service_pending_unlock(service);

    return count;
}

size_t ipc_service_get_pending_count(ipc_service_t* service) {
    return ipc_pending_table_count(service);
}
