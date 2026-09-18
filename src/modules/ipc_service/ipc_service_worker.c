/**
 * @file ipc_service_worker.c
 * @brief IPC 服务工作线程
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-05-28
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-05-28       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            补输入句柄 acquire+release×2 引用计数契约注释
 */

#include "ipc_service_internal.h"

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(thread_ipc_svc, CONFIG_THREAD_IPC_SERVICE_LOG_LEVEL);

void ipc_service_worker_thread(void* p1, void* p2, void* p3) {
    ipc_service_t*     service = (ipc_service_t*) p1;
    ipc_request_msg_t  request_msg;
    ipc_response_msg_t response_msg;

    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_DBG("IPC service '%s' worker started", service->name);

    for (;;) {
        int ret = k_msgq_get(&service->request_queue, &request_msg, K_MSEC(IPC_SERVICE_MSGQ_TIMEOUT_MS));

        if (ret != 0) {
            if (atomic_get(&service->shutdown) != 0) {
                LOG_DBG("Worker thread exiting on shutdown signal");
                break;
            }
            continue;
        }

        if (atomic_get(&service->shutdown) != 0) {
            LOG_DBG("Worker thread detected shutdown after receiving request");
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
            if (request_msg.shm_handle != 0) {
                ipc_shm_release(service, request_msg.shm_handle);
            }
#endif
            break;
        }

        if (request_msg.request_id == 0U) {
            LOG_DBG("Received dummy request, ignoring");
            continue;
        }

        if (service->service_func == NULL) {
            LOG_ERR("Service function is NULL, dropping request %u", request_msg.request_id);
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
            if (request_msg.shm_handle != 0) {
                ipc_shm_release(service, request_msg.shm_handle);
            }
#endif
            continue;
        }

        ipc_service_pending_lock(service);
        ipc_pending_request_t* tracked = ipc_pending_table_find(service, request_msg.request_id);
        bool                   still_tracked = (tracked != NULL && tracked->in_use && !tracked->canceled);
        ipc_service_pending_unlock(service);

        if (!still_tracked) {
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
            if (request_msg.shm_handle != 0) {
                ipc_shm_release(service, request_msg.shm_handle);
            }
#endif
            LOG_DBG("Skipping request %u (pending slot released)", request_msg.request_id);
            continue;
        }

        void*  out_data = NULL;
        size_t out_data_size = 0;

        LOG_DBG("Processing request %u", request_msg.request_id);

#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
        bool request_shm_acquired = false;

        if (request_msg.shm_handle != 0) {
            if (ipc_shm_acquire(service, request_msg.shm_handle) != 0) {
                LOG_ERR("Request %u: invalid shm handle %u", request_msg.request_id, request_msg.shm_handle);
                response_msg.request_id = request_msg.request_id;
                response_msg.result = -EINVAL;
                response_msg.data = NULL;
                response_msg.data_size = 0;
                response_msg.caller_thread = request_msg.caller_thread;
                response_msg.shm_handle = 0;
                (void) ipc_put_msgq_until_shutdown(&service->response_queue, &response_msg, &service->shutdown);
                continue;
            }
            request_shm_acquired = true;
            LOG_DBG("Request %u: acquired shm handle %u", request_msg.request_id, request_msg.shm_handle);
        }
#endif

        int result = service->service_func(request_msg.request_id, request_msg.data, request_msg.data_size, &out_data,
                                           &out_data_size);

        response_msg.request_id = request_msg.request_id;
        response_msg.result = result;
        response_msg.data = out_data;
        response_msg.data_size = out_data_size;
        response_msg.caller_thread = request_msg.caller_thread;
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
        response_msg.shm_handle = 0;
        if (out_data != NULL) {
            ipc_shm_handle_t out_handle = ipc_shm_lookup_handle_by_ptr(service, out_data);

            if (out_handle != IPC_SHM_HANDLE_INVALID) {
                response_msg.shm_handle = out_handle;
            }
        }
#endif

        int put_ret = ipc_put_msgq_until_shutdown(&service->response_queue, &response_msg, &service->shutdown);
        if (put_ret != 0) {
            LOG_ERR("Failed to send response for request %u: %d", request_msg.request_id, put_ret);
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
            if (response_msg.shm_handle != 0) {
                ipc_shm_release(service, response_msg.shm_handle);
            }
#endif
        }

#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
        /* 输入句柄引用计数平衡：调用方随请求移交 1 份引用（所有权归 worker），
         * worker 处理前又显式 acquire +1（供响应透传时对冲）。因此收尾需 release 两次：
         * 1) 释放 worker 自己 acquire 的那份；
         * 2) 若响应未透传原句柄（response_msg.shm_handle != 请求句柄），再释放随请求
         *    移交的那份。透传场景下该份引用由 dispatcher（async 回调后）或调用方
         *    收尾（sync_shm 拿到句柄后自行 release）负责释放。 */
        if (request_shm_acquired) {
            ipc_shm_release(service, request_msg.shm_handle);
        }
        if (request_msg.shm_handle != 0 && response_msg.shm_handle != request_msg.shm_handle) {
            ipc_shm_release(service, request_msg.shm_handle);
        }
#endif
    }

    LOG_DBG("IPC service '%s' worker stopped", service->name);
}
