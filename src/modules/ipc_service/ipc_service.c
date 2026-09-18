/**
 * @file ipc_service.c
 * @brief IPC 服务核心：请求 ID、锁、构建期校验
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-05-28
 *
 * @par 修改日志:
 * Date       Version Author Description
 * 2026-04-01 1.0     zeh    初始版本
 * 2026-05-28 1.1     zeh    按职责拆分到多源文件
 * 2026-09-18 1.2     zeh    修正 include 顺序（zephyr → 标准库 → zeplod → 私有头）
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <errno.h>

#include <zeplod/ipc_service.h>
#include <zeplod/lock_order.h>

#include "ipc_service_internal.h"

LOG_MODULE_REGISTER(thread_ipc_svc, CONFIG_THREAD_IPC_SERVICE_LOG_LEVEL);

BUILD_ASSERT(CONFIG_THREAD_IPC_SERVICE_REQUEST_QUEUE_SIZE >= 2);
BUILD_ASSERT(CONFIG_THREAD_IPC_SERVICE_REQUEST_QUEUE_SIZE <= IPC_SERVICE_MAX_QUEUE_SIZE);
BUILD_ASSERT(CONFIG_THREAD_IPC_SERVICE_RESPONSE_QUEUE_SIZE >= 2);
BUILD_ASSERT(CONFIG_THREAD_IPC_SERVICE_RESPONSE_QUEUE_SIZE <= IPC_SERVICE_MAX_QUEUE_SIZE);
BUILD_ASSERT(CONFIG_THREAD_IPC_SERVICE_STACK_SIZE >= IPC_SERVICE_MIN_STACK_SIZE);

static atomic_t s_request_id_counter = ATOMIC_INIT(1);

ipc_request_id_t ipc_generate_request_id(void) {
    ipc_request_id_t id;

    do {
        id = (ipc_request_id_t) atomic_inc(&s_request_id_counter);
        if (id == 0U) {
            id = (ipc_request_id_t) atomic_inc(&s_request_id_counter);
        }
    } while (id == 0U);

    return id;
}

void ipc_service_state_lock(ipc_service_t* service) {
    zepl_lock_enter(ZEP_LOCK_LEVEL_STATE, (uintptr_t) &service->state_lock);
    k_mutex_lock(&service->state_lock, K_FOREVER);
}

void ipc_service_state_unlock(ipc_service_t* service) {
    k_mutex_unlock(&service->state_lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_STATE, (uintptr_t) &service->state_lock);
}

void ipc_service_pending_lock(ipc_service_t* service) {
    zepl_lock_enter(ZEP_LOCK_LEVEL_TABLE, (uintptr_t) &service->pending_lock);
    k_mutex_lock(&service->pending_lock, K_FOREVER);
}

void ipc_service_pending_unlock(ipc_service_t* service) {
    k_mutex_unlock(&service->pending_lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_TABLE, (uintptr_t) &service->pending_lock);
}

zepl_state_t ipc_service_lifecycle_state_locked(const ipc_service_t* service) {
    if (service == NULL) {
        return ZEP_STATE_ERROR;
    }

    return zepl_state_machine_get(&service->lifecycle);
}
