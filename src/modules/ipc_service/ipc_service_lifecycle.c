/**
 * @file ipc_service_lifecycle.c
 * @brief IPC 服务生命周期（init/start/stop）
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-05-28
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-05-28       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            start 回滚不再持锁 join；stop 按阶段拆分；init 增加 pending 防御
 */

#include <zephyr/logging/log.h>
#include <string.h>

#include "ipc_service_internal.h"

LOG_MODULE_DECLARE(thread_ipc_svc, CONFIG_THREAD_IPC_SERVICE_LOG_LEVEL);

bool ipc_service_is_accepting_requests(ipc_service_t* service) {
    if (service == NULL || !service->initialized) {
        return false;
    }

    ipc_service_state_lock(service);
    bool accepting = service->running && atomic_get(&service->shutdown) == 0;
    ipc_service_state_unlock(service);

    return accepting;
}

int ipc_put_msgq_until_shutdown(struct k_msgq* queue, const void* msg, const atomic_t* shutdown) {
    while (atomic_get(shutdown) == 0) {
        int ret = k_msgq_put(queue, msg, K_MSEC(IPC_SERVICE_MSGQ_TIMEOUT_MS));

        if (ret == 0) {
            return 0;
        }

        if (ret != -EAGAIN) {
            return ret;
        }
    }

    return -ECANCELED;
}

void ipc_drain_queued_messages(ipc_service_t* service) {
    ipc_request_msg_t request_msg;
    while (k_msgq_get(&service->request_queue, &request_msg, K_NO_WAIT) == 0) {
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
        if (request_msg.shm_handle != 0) {
            ipc_shm_release(service, request_msg.shm_handle);
        }
#endif
    }

    ipc_response_msg_t response_msg;
    while (k_msgq_get(&service->response_queue, &response_msg, K_NO_WAIT) == 0) {
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
        if (response_msg.shm_handle != 0) {
            ipc_shm_release(service, response_msg.shm_handle);
        }
#endif
    }
}

int ipc_service_init(ipc_service_t* service, const char* name, ipc_service_func_t service_func, int priority) {
    if (service == NULL || name == NULL || service_func == NULL) {
        return -EINVAL;
    }

    if (service->initialized) {
        ipc_service_state_lock(service);
        zepl_state_t state = ipc_service_lifecycle_state_locked(service);
        ipc_service_state_unlock(service);

        if (state == ZEP_STATE_RUNNING || state == ZEP_STATE_STARTING || state == ZEP_STATE_STOPPING ||
            state == ZEP_STATE_ERROR) {
            return -EBUSY;
        }

        /* 防御：仍有未收尾的 pending 请求时拒绝 re-init，避免下方 memset 抹掉调用方
         * 仍在等待的信号量。尽力检查（与后续 memset 间存在 TOCTOU 窗口），硬性前置
         * 条件见 ipc_service_init 头文件 @note。 */
        if (ipc_pending_table_count(service) != 0U) {
            LOG_WRN("IPC service '%s' re-init rejected: pending requests outstanding", name);
            return -EBUSY;
        }
    }

    memset(service, 0, sizeof(ipc_service_t));
    zepl_state_machine_init(&service->lifecycle, ZEP_STATE_UNINIT);

    service->name = name;
    service->service_func = service_func;
    service->priority = priority;
    service->initialized = true;
    service->running = false;
    atomic_set(&service->shutdown, 0);
    atomic_set(&service->stop_in_progress, 0);
    k_mutex_init(&service->state_lock);

    k_msgq_init(&service->request_queue, (char*) service->request_queue_buf, sizeof(ipc_request_msg_t),
                CONFIG_THREAD_IPC_SERVICE_REQUEST_QUEUE_SIZE);

    k_msgq_init(&service->response_queue, (char*) service->response_queue_buf, sizeof(ipc_response_msg_t),
                CONFIG_THREAD_IPC_SERVICE_RESPONSE_QUEUE_SIZE);

    k_mutex_init(&service->pending_lock);

    for (int i = 0; i < CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS; i++) {
        service->pending_requests[i].in_use = false;
    }

    service->free_futures = NULL;
    for (int i = 0; i < CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS; i++) {
        service->futures[i].request_id = 0;
        atomic_set(&service->futures[i].completed, 0);
        service->futures[i].next = service->free_futures;
        k_sem_init(&service->futures[i].semaphore, 0, 1);
        service->free_futures = &service->futures[i];
    }

#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
    int shm_ret = ipc_shm_init(service);
    if (shm_ret != 0) {
        LOG_ERR("Failed to init shared memory pool: %d", shm_ret);
        service->initialized = false;
        return shm_ret;
    }
#endif

    (void) zepl_state_machine_try_transition(&service->lifecycle, ZEP_STATE_INITED);

    LOG_DBG("IPC service '%s' initialized", name);

    return 0;
}

int ipc_service_start(ipc_service_t* service) {
    if (service == NULL || !service->initialized || !service->service_func) {
        return -EINVAL;
    }

    ipc_service_state_lock(service);

    zepl_state_t state = ipc_service_lifecycle_state_locked(service);

    if (state == ZEP_STATE_RUNNING) {
        ipc_service_state_unlock(service);
        return -EALREADY;
    }

    if (state == ZEP_STATE_UNINIT || state == ZEP_STATE_ERROR || state == ZEP_STATE_STOPPING) {
        ipc_service_state_unlock(service);
        return -EINVAL;
    }

    if (zepl_state_machine_try_transition(&service->lifecycle, ZEP_STATE_STARTING) != 0) {
        ipc_service_state_unlock(service);
        return -EINVAL;
    }

    atomic_set(&service->shutdown, 0);

    k_tid_t worker_tid = k_thread_create(&service->thread, service->worker_stack,
                                         K_KERNEL_STACK_SIZEOF(service->worker_stack), ipc_service_worker_thread,
                                         service, NULL, NULL, service->priority, 0, K_NO_WAIT);
    if (worker_tid == NULL) {
        LOG_ERR("Failed to create IPC worker thread for '%s'", service->name);
        atomic_set(&service->shutdown, 0);
        (void) zepl_state_machine_try_transition(&service->lifecycle, ZEP_STATE_STOPPING);
        (void) zepl_state_machine_try_transition(&service->lifecycle, ZEP_STATE_STOPPED);
        ipc_service_state_unlock(service);
        return -ENOMEM;
    }
#if IS_ENABLED(CONFIG_THREAD_NAME)
    k_thread_name_set(worker_tid, service->name);
#endif

    k_tid_t disp_tid = k_thread_create(&service->response_thread, service->dispatcher_stack,
                                       K_KERNEL_STACK_SIZEOF(service->dispatcher_stack), ipc_service_dispatcher_thread,
                                       service, NULL, NULL, service->priority, 0, K_NO_WAIT);
    if (disp_tid == NULL) {
        LOG_ERR("Failed to create IPC dispatcher thread for '%s'", service->name);
        /* 回滚决定（锁内）：置 shutdown 并投递空消息唤醒已创建的 worker
         * （其 msgq 轮询会看到标志后退出）。join 会阻塞，不得持有 state_lock 执行，
         * 因此先解锁再等待，之后重新拿锁提交最终状态。 */
        atomic_set(&service->shutdown, 1);
        ipc_request_msg_t dummy;
        memset(&dummy, 0, sizeof(dummy));
        (void) k_msgq_put(&service->request_queue, &dummy, K_NO_WAIT);
        ipc_service_state_unlock(service);

        const bool worker_exited = (k_thread_join(&service->thread, K_MSEC(IPC_SERVICE_THREAD_JOIN_TIMEOUT_MS)) == 0);

        ipc_service_state_lock(service);
        if (worker_exited) {
            atomic_set(&service->shutdown, 0);
            (void) zepl_state_machine_try_transition(&service->lifecycle, ZEP_STATE_STOPPING);
            (void) zepl_state_machine_try_transition(&service->lifecycle, ZEP_STATE_STOPPED);
        } else {
            /* worker 未能在超时内退出（异常）：保持 shutdown=1 让其稍后自行退出并置
             * ERROR 故障态；线程控制块仍被占用，不得再重试 start/init。 */
            LOG_ERR("IPC worker thread for '%s' failed to exit during rollback; service faulted", service->name);
            (void) zepl_state_machine_try_transition(&service->lifecycle, ZEP_STATE_STOPPING);
            (void) zepl_state_machine_try_transition(&service->lifecycle, ZEP_STATE_ERROR);
        }
        ipc_service_state_unlock(service);
        return -ENOMEM;
    }
#if IS_ENABLED(CONFIG_THREAD_NAME)
    k_thread_name_set(disp_tid, "ipc_disp");
#endif

    service->running = true;
    (void) zepl_state_machine_try_transition(&service->lifecycle, ZEP_STATE_RUNNING);
    ipc_service_state_unlock(service);

    LOG_DBG("IPC service '%s' started", service->name);

    return 0;
}

/* stop 状态门控（持锁→解锁）。返回：
 *  1  可以继续 stop 流程（已置 shutdown 并进入/保持 STOPPING，返回时已解锁）
 *  0  服务本就未运行，stop 幂等成功（已解锁）
 *  <0 失败错误码（已解锁） */
static int ipc_stop_begin(ipc_service_t* service) {
    ipc_service_state_lock(service);
    zepl_state_t state = ipc_service_lifecycle_state_locked(service);

    if (state == ZEP_STATE_UNINIT) {
        ipc_service_state_unlock(service);
        return -EINVAL;
    }

    if (!service->running || state == ZEP_STATE_INITED || state == ZEP_STATE_STOPPED) {
        ipc_service_state_unlock(service);
        return 0;
    }

    if (state == ZEP_STATE_ERROR) {
        ipc_service_state_unlock(service);
        return -EINVAL;
    }

    if (k_current_get() == &service->thread || k_current_get() == &service->response_thread) {
        ipc_service_state_unlock(service);
        return -EDEADLK;
    }

    if (state != ZEP_STATE_STOPPING &&
        zepl_state_machine_try_transition(&service->lifecycle, ZEP_STATE_STOPPING) != 0) {
        ipc_service_state_unlock(service);
        return -EINVAL;
    }

    atomic_set(&service->shutdown, 1);
    ipc_service_state_unlock(service);
    return 1;
}

/* 向请求/响应队列各投递一条空消息，唤醒可能在 msgq 轮询中阻塞的两个线程。 */
static void ipc_stop_wake_threads(ipc_service_t* service) {
    ipc_request_msg_t dummy_request;
    memset(&dummy_request, 0, sizeof(dummy_request));
    (void) k_msgq_put(&service->request_queue, &dummy_request, K_NO_WAIT);

    ipc_response_msg_t dummy_response;
    memset(&dummy_response, 0, sizeof(dummy_response));
    (void) k_msgq_put(&service->response_queue, &dummy_response, K_NO_WAIT);
}

/* 等待两个服务线程退出。全部退出返回 0；任一超时返回 -EIO（实例保持 STOPPING，
 * 调用方稍后重试 stop）。 */
static int ipc_stop_join_threads(ipc_service_t* service) {
    const int ret1 = k_thread_join(&service->thread, K_MSEC(IPC_SERVICE_THREAD_JOIN_TIMEOUT_MS));
    if (ret1 != 0) {
        LOG_ERR("Worker thread join failed: %d", ret1);
    }

    const int ret2 = k_thread_join(&service->response_thread, K_MSEC(IPC_SERVICE_THREAD_JOIN_TIMEOUT_MS));
    if (ret2 != 0) {
        LOG_ERR("Dispatcher thread join failed: %d", ret2);
    }

    if (ret1 != 0 || ret2 != 0) {
        LOG_ERR("IPC service '%s': thread join timed out (worker_ret=%d, disp_ret=%d)", service->name, ret1, ret2);
        return -EIO;
    }
    return 0;
}

/* 清理全部 pending 请求（持 pending_lock）：按 future / callback / 同步三种形态
 * 分别投递 -ECANCELED 并释放槽位，唤醒仍阻塞的调用方。 */
static void ipc_stop_cancel_pending(ipc_service_t* service) {
    ipc_service_pending_lock(service);
    for (int i = 0; i < CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS; i++) {
        ipc_pending_request_t* entry = &service->pending_requests[i];

        if (!entry->in_use) {
            continue;
        }

        if (entry->future != NULL) {
            entry->future->result = -ECANCELED;
            entry->future->data = NULL;
            entry->future->data_size = 0;
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
            ipc_pending_table_release_shm(service, entry);
#endif
            atomic_set(&entry->future->completed, 1);
            k_sem_give(&entry->future->semaphore);
            entry->future = NULL;
            ipc_pending_table_release(service, entry);
        } else if (entry->callback != NULL) {
            ipc_pending_table_release(service, entry);
        } else if (!entry->completed) {
            entry->canceled = true;
            entry->result = -ECANCELED;
            entry->response_data = NULL;
            entry->response_data_size = 0;
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
            ipc_pending_table_release_shm(service, entry);
#endif
            k_sem_give(&entry->response_sem);
        }
    }
    ipc_service_pending_unlock(service);
}

/* 线程全部退出后的收尾：排空队列、复位状态到 STOPPED、取消 pending 并回收 shm 池。 */
static void ipc_stop_cleanup(ipc_service_t* service) {
    ipc_drain_queued_messages(service);
    k_msgq_purge(&service->request_queue);
    k_msgq_purge(&service->response_queue);

    ipc_service_state_lock(service);
    service->running = false;
    atomic_set(&service->shutdown, 0);
    (void) zepl_state_machine_try_transition(&service->lifecycle, ZEP_STATE_STOPPED);
    ipc_service_state_unlock(service);

    ipc_stop_cancel_pending(service);

#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
    ipc_shm_deinit(service);
#endif
}

int ipc_service_stop(ipc_service_t* service) {
    if (service == NULL || !service->initialized) {
        return -EINVAL;
    }

    if (!atomic_cas(&service->stop_in_progress, 0, 1)) {
        return -EALREADY;
    }

    const int begin_ret = ipc_stop_begin(service);
    if (begin_ret <= 0) {
        /* 0 = 本就未运行（幂等成功）；<0 = 门控失败。两者都不进入停止流程。 */
        atomic_set(&service->stop_in_progress, 0);
        return (begin_ret < 0) ? begin_ret : 0;
    }

    ipc_stop_wake_threads(service);

    const int join_ret = ipc_stop_join_threads(service);
    if (join_ret != 0) {
        atomic_set(&service->stop_in_progress, 0);
        return join_ret;
    }

    ipc_stop_cleanup(service);

    atomic_set(&service->stop_in_progress, 0);

    LOG_DBG("IPC service '%s' stopped", service->name);

    return 0;
}
