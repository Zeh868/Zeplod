/**
 * @file recovery_policy.h
 * @brief 恢复策略模块头文件
 *
 * 可选模块（CONFIG_RECOVERY_POLICY_MODULE）。监听 module_manager 的
 * MODULE_MGR_EVENT_ERROR，按 Kconfig 执行模块重启、全量重启或暖复位。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 2 初始版本
 * 2026-09-18       1.1            zeh            模块接口 7 件套补全 Doxygen 注释
 *
 */

#ifndef RECOVERY_POLICY_H
#define RECOVERY_POLICY_H

#include <zeplod/event_system.h>
#include <zeplod/module_base.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 类型定义
 * ============================================================================= */

/**
 * @brief 恢复动作（与 Kconfig RECOVERY_POLICY_DEFAULT_ACTION 对应）
 */
typedef enum {
    RECOVERY_ACTION_NONE = 0,       /**< 仅记录日志 */
    RECOVERY_ACTION_RESTART_MODULE, /**< stop + clear_error + start 失败模块 */
    RECOVERY_ACTION_RESTART_ALL,    /**< stop_all + start_all */
    RECOVERY_ACTION_REBOOT,         /**< sys_reboot(WARM) */
} recovery_action_t;

/** recovery_policy_control 命令：重置全部重启计数 */
#define RECOVERY_POLICY_CMD_RESET_COUNTS 1

/* =============================================================================
 * 模块接口（module_interface_t，由 module_manager 生命周期驱动）
 * ============================================================================= */

/**
 * @brief 初始化恢复策略模块（清零重启计数）
 * @param config 模块配置指针；本模块未使用，允许为 NULL
 * @return 0 成功；重复初始化幂等返回 0
 */
int recovery_policy_init(void* config);

/**
 * @brief 启动恢复策略（登记 module_manager 错误回调及 OTA/看门狗钩子）
 * @return 0 成功；APP_ERR_INIT 未初始化；重复启动幂等返回 0
 */
int recovery_policy_start(void);

/**
 * @brief 停止恢复策略（撤销看门狗 pre-expire 回调，保留错误回调登记）
 * @return 0 成功；未运行时亦返回 0（幂等）
 */
int recovery_policy_stop(void);

/**
 * @brief 关闭恢复策略（撤销全部回调与订阅，状态回落 UNINITIALIZED）
 * @return 0 成功
 */
int recovery_policy_shutdown(void);

/**
 * @brief 事件回调占位（恢复动作经 module_manager 回调与 OTA 事件订阅触发）
 * @param event 事件指针（非 NULL，由事件分发器保证）
 * @param user_data 订阅时传入的用户数据（本模块未使用）
 */
void recovery_policy_on_event(const event_t* event, void* user_data);

/**
 * @brief 查询模块生命周期状态
 * @return MODULE_STATUS_*（UNINITIALIZED/INITIALIZED/RUNNING/STOPPED）
 */
module_status_t recovery_policy_get_status(void);

/**
 * @brief 模块控制命令入口
 * @param cmd 命令字；仅支持 RECOVERY_POLICY_CMD_RESET_COUNTS
 * @param arg 命令参数；RESET_COUNTS 未使用，须为 NULL
 * @return 0 成功；-ENOTSUP 不支持的命令
 */
int recovery_policy_control(int cmd, void* arg);

/* =============================================================================
 * 模块专用 API
 * ============================================================================= */

/**
 * @brief 处理指定模块 ERROR（与 module_manager 回调路径相同）
 *
 * @param module_id 模块 ID
 * @return 0 已执行恢复动作；-EINVAL 参数或策略未运行
 */
int recovery_policy_on_module_error(uint32_t module_id);

/**
 * @brief 查询模块累计自动重启次数
 *
 * @param module_id 模块 ID
 * @return 次数；未知 ID 返回 0
 */
uint32_t recovery_policy_get_restart_count(uint32_t module_id);

/**
 * @brief 重置全部重启计数（测试用）
 */
void recovery_policy_reset_restart_counts(void);

#ifdef __cplusplus
}
#endif

#endif /* RECOVERY_POLICY_H */
