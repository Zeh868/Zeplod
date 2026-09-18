/**
 * @file example_module_ipc.h
 * @brief 示例：在业务模块中集成 Thread IPC Service（与 module_manager 配合）
 *
 * 由 Kconfig CONFIG_EXAMPLE_MODULE_THREAD_IPC 控制是否编译与注册。
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            审查整改：删除无实际用途的 reserved
 *                                                 字段（模块无可配置项，配置结构体一并
 *                                                 移除，init/config 传 NULL 即可）
 *
 */

#ifndef EXAMPLE_MODULE_IPC_H
#define EXAMPLE_MODULE_IPC_H

#include <zeplod/module_base.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 演示用：事件桥中的 source_id（与 thread_ipc_event_publish_result 一致） */
#define EXAMPLE_MODULE_IPC_EVENT_SOURCE_ID 42U

int             example_module_ipc_init(void* config);
int             example_module_ipc_start(void);
int             example_module_ipc_stop(void);
int             example_module_ipc_shutdown(void);
void            example_module_ipc_on_event(const event_t* event, void* user_data);
module_status_t example_module_ipc_get_status(void);
int             example_module_ipc_control(int cmd, void* arg);

const module_interface_t* example_module_ipc_get_interface(void);

/* =============================================================================
 * 可选：供调试或上层显式调用（演示 IPC 通路）
 * ============================================================================= */

/**
 * @brief 向本模块 IPC 服务发起一次同步调用（演示 payload 为以 '\\0' 结尾的字符串）
 * @return 与 ipc_call_sync 相同
 */
int example_module_ipc_demo_call_sync(const char* msg, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* EXAMPLE_MODULE_IPC_H */
