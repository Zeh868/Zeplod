/**
 * @file ipc_pending_table.h
 * @brief IPC pending 表与 future 对象池（内部 API）
 *
 * 除 ipc_pending_table_count() 外，调用方须在持有 service->pending_lock 时使用
 * find/alloc/init/release；future 池操作同理。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-05-28
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-05-28       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            补修改日志段
 */

#ifndef IPC_PENDING_TABLE_H
#define IPC_PENDING_TABLE_H

#include <zeplod/ipc_service.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

ipc_pending_request_t* ipc_pending_table_find(ipc_service_t* service, ipc_request_id_t request_id);
ipc_pending_request_t* ipc_pending_table_alloc(ipc_service_t* service);
void ipc_pending_table_init_entry(ipc_service_t* service, ipc_pending_request_t* entry, ipc_request_id_t request_id,
                                  struct k_thread* caller_thread, ipc_async_callback_t callback,
                                  void* callback_user_data, ipc_future_t* future);
void ipc_pending_table_release(ipc_service_t* service, ipc_pending_request_t* entry);
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
void ipc_pending_table_release_shm(ipc_service_t* service, ipc_pending_request_t* entry);
#endif

ipc_future_t* ipc_pending_table_alloc_future(ipc_service_t* service);
void          ipc_pending_table_release_future(ipc_service_t* service, ipc_future_t* future);
bool          ipc_pending_table_future_belongs(const ipc_service_t* service, const ipc_future_t* future);
bool          ipc_pending_table_future_in_free_list(const ipc_service_t* service, const ipc_future_t* future);

/** 统计 in_use 条目数（内部获取 pending_lock） */
size_t ipc_pending_table_count(ipc_service_t* service);

/** 公开 API 实现：ipc_service_get_pending_count() */
size_t ipc_service_get_pending_count(ipc_service_t* service);

#ifdef __cplusplus
}
#endif

#endif /* IPC_PENDING_TABLE_H */
