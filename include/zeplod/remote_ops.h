/**
 * @file remote_ops.h
 * @brief 远程运维钩子模块头文件
 *
 * 可选模块（CONFIG_REMOTE_OPS_MODULE）。Phase 5 通过可插拔后端导出
 * `sys_diag` JSON；mcumgr 真实传输留后续 target 叠加。事件 ID 71。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 5 初始版本
 * 2026-09-18       1.1            zeh            模块接口 7 件套补全 Doxygen 注释
 *
 */

#ifndef REMOTE_OPS_H
#define REMOTE_OPS_H

#include <zeplod/event_system.h>
#include <zeplod/module_base.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 诊断导出完成事件（载荷 uint32_t：本次导出 JSON 字节长度） */
#define EVENT_REMOTE_OPS_DIAG_EXPORTED ((event_type_t) 71)

/** 模块统计：导出次数与最近一次载荷长度 */
typedef struct {
    uint32_t export_count;     /**< 累计成功导出次数 */
    uint32_t last_payload_len; /**< 最近一次导出的 JSON 字节长度（不含 NUL） */
} remote_ops_status_t;

/* =============================================================================
 * 模块接口（module_interface_t，由 module_manager 生命周期驱动）
 * ============================================================================= */

/**
 * @brief 初始化远程运维模块（选定并初始化导出后端）
 * @param config 模块配置指针；本模块未使用，允许为 NULL
 * @return 0 成功；APP_ERR_REMOTE_OPS 无可用后端或后端 init 失败；重复初始化幂等返回 0
 */
int remote_ops_init(void* config);

/**
 * @brief 启动远程运维模块
 * @return 0 成功；APP_ERR_INIT 未初始化；重复启动幂等返回 0
 */
int remote_ops_start(void);

/**
 * @brief 停止远程运维模块
 * @return 0 成功；未运行时亦返回 0（幂等）
 */
int remote_ops_stop(void);

/**
 * @brief 关闭远程运维模块（调用后端 deinit，状态回落 UNINITIALIZED）
 * @return 0 成功
 */
int remote_ops_shutdown(void);

/**
 * @brief 事件回调占位（本模块不消费外部事件）
 * @param event 事件指针（非 NULL，由事件分发器保证）
 * @param user_data 订阅时传入的用户数据（本模块未使用）
 */
void remote_ops_on_event(const event_t* event, void* user_data);

/**
 * @brief 查询模块生命周期状态
 * @return MODULE_STATUS_*（UNINITIALIZED/INITIALIZED/RUNNING/STOPPED）
 */
module_status_t remote_ops_get_status(void);

/**
 * @brief 模块控制命令入口
 * @param cmd 命令字（本模块未实现任何命令）
 * @param arg 命令参数（未使用）
 * @return -ENOTSUP 始终返回
 */
int remote_ops_control(int cmd, void* arg);

/* =============================================================================
 * 模块专用 API
 * ============================================================================= */

/**
 * @brief 采集诊断快照并以 JSON 经后端导出
 * @return 0 成功；APP_ERR_INIT（未 init 或未 RUNNING）；APP_ERR_REMOTE_OPS
 *         （无后端、sys_diag 导出失败或后端接收失败）
 */
int remote_ops_export_diag(void);

/**
 * @brief 查询模块统计
 * @param out 输出统计指针，须非 NULL
 * @return 0 成功；APP_ERR_INIT（未 init）；APP_ERR_INVALID_PARAM out 为 NULL
 */
int remote_ops_get_stats(remote_ops_status_t* out);

/**
 * @brief 读回最近一次导出的 JSON（需后端实现 get_last_export）
 * @param out 输出缓冲区，须非 NULL
 * @param out_len 缓冲区长度（须 > 0，含结尾 NUL）
 * @return 0 成功；APP_ERR_INIT（未 init）；APP_ERR_REMOTE_OPS（后端不支持读回）；
 *         APP_ERR_INVALID_PARAM 参数非法；-ENOENT 尚无导出；-ENOMEM 缓冲不足
 */
int remote_ops_get_last_export(char* out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* REMOTE_OPS_H */
