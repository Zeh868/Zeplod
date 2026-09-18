/**
 * @file ipc_service_dispatcher.c
 * @brief IPC 服务响应分发线程
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-05-28
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-05-28       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            补修改日志段
 */

#include "ipc_service_internal.h"

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(thread_ipc_svc, CONFIG_THREAD_IPC_SERVICE_LOG_LEVEL);

void ipc_service_dispatcher_thread(void* p1, void* p2, void* p3) {
    ipc_service_t*     service = (ipc_service_t*) p1;
    ipc_response_msg_t response_msg;

    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_DBG("IPC service '%s' dispatcher started", service->name);

    for (;;) {
        int ret = k_msgq_get(&service->response_queue, &response_msg, K_MSEC(IPC_SERVICE_MSGQ_TIMEOUT_MS));

        if (ret != 0) {
            if (atomic_get(&service->shutdown) != 0) {
                LOG_DBG("Dispatcher thread exiting on shutdown signal");
                break;
            }
            continue;
        }

        if (atomic_get(&service->shutdown) != 0) {
            LOG_DBG("Dispatcher thread detected shutdown");
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
            if (response_msg.shm_handle != 0) {
                ipc_shm_release(service, response_msg.shm_handle);
            }
#endif
            break;
        }

        if (response_msg.request_id == 0U) {
            LOG_DBG("Received dummy response, ignoring");
            continue;
        }

        ipc_service_pending_lock(service);

        ipc_pending_request_t* entry = ipc_pending_table_find(service, response_msg.request_id);

        if (entry != NULL) {
            if (entry->canceled) {
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                if (response_msg.shm_handle != 0) {
                    ipc_shm_release(service, response_msg.shm_handle);
                }
#endif
                ipc_service_pending_unlock(service);
                continue;
            }

            entry->result = response_msg.result;
            entry->response_data = response_msg.data;
            entry->response_data_size = response_msg.data_size;
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
            entry->shm_handle = response_msg.shm_handle;
            response_msg.shm_handle = 0;
            ipc_shm_handle_t shm_handle = entry->shm_handle;
#endif

            if (entry->callback != NULL) {
                ipc_async_callback_t cb = entry->callback;
                void*                ud = entry->callback_user_data;
                ipc_request_id_t     rid = entry->request_id;
                int                  res = entry->result;
                const void*          rdata = entry->response_data;
                size_t               rsize = entry->response_data_size;

#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                entry->shm_handle = 0;
#endif
                ipc_pending_table_release(service, entry);
                ipc_service_pending_unlock(service);

                /* 进入本分支的前提即 entry->callback != NULL，cb 必非空。 */
                cb(rid, res, rdata, rsize, ud);

#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                if (shm_handle != 0) {
                    LOG_DBG("ASYNC callback done, releasing shm handle %u", shm_handle);
                    ipc_shm_release(service, shm_handle);
                }
#endif
                continue;
            }

            if (entry->future != NULL) {
                entry->future->result = entry->result;
                entry->future->data = entry->response_data;
                entry->future->data_size = entry->response_data_size;
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                entry->future->shm_handle = entry->shm_handle;
                entry->shm_handle = 0;
#endif
                atomic_set(&entry->future->completed, 1);
                k_sem_give(&entry->future->semaphore);
                entry->future = NULL;
                ipc_pending_table_release(service, entry);
            } else {
                entry->completed = true;
                k_sem_give(&entry->response_sem);
            }
        } else {
            LOG_DBG("No pending entry for response %u", response_msg.request_id);
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
            if (response_msg.shm_handle != 0) {
                LOG_DBG("Releasing orphan shm handle %u for response %u", response_msg.shm_handle,
                        response_msg.request_id);
                ipc_shm_release(service, response_msg.shm_handle);
            }
#endif
        }

        ipc_service_pending_unlock(service);
    }

    LOG_DBG("IPC service '%s' dispatcher stopped", service->name);
}
