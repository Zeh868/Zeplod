/**
 * @file feature_gate.h
 * @brief 功能开关模块头文件（非安全 DRM）
 *
 * 可选模块（CONFIG_FEATURE_GATE_MODULE）。按 Kconfig 槽位 + 运行时 license
 * 字符串解锁高级能力；事件 ID 72。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 5 初始版本
 * 2026-09-18       1.1            zeh            模块接口 7 件套补全 Doxygen 注释；
 *                                                 license_valid/is_enabled 补全参数与返回说明
 *
 */

#ifndef FEATURE_GATE_H
#define FEATURE_GATE_H

#include <zeplod/event_system.h>
#include <zeplod/module_base.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** license 变更事件（载荷 bool：应用后 license 是否有效） */
#define EVENT_FEATURE_GATE_LICENSE_CHANGED ((event_type_t) 72)

/** 功能名：始终可用 */
#define FEATURE_GATE_NAME_CORE             "core"

/** 功能名：需有效 license（CONFIG_FEATURE_GATE_SLOT_CLOUD） */
#define FEATURE_GATE_NAME_CLOUD            "cloud"

/** 功能名：需有效 license（CONFIG_FEATURE_GATE_SLOT_REMOTE） */
#define FEATURE_GATE_NAME_REMOTE           "remote"

/** 模块状态快照 */
typedef struct {
    bool license_valid; /**< 当前 license 是否有效 */
} feature_gate_status_t;

/* =============================================================================
 * 模块接口（module_interface_t，由 module_manager 生命周期驱动）
 * ============================================================================= */

/**
 * @brief 初始化功能开关模块（按 Kconfig 启动 license 决定初始有效状态）
 * @param config 模块配置指针；本模块未使用，允许为 NULL
 * @return 0 成功；APP_ERR_FEATURE_GATE 事件类型注册失败；重复初始化幂等返回 0
 */
int feature_gate_init(void* config);

/**
 * @brief 启动功能开关模块
 * @return 0 成功；APP_ERR_INIT 未初始化；重复启动幂等返回 0
 */
int feature_gate_start(void);

/**
 * @brief 停止功能开关模块
 * @return 0 成功；未运行时亦返回 0（幂等）
 */
int feature_gate_stop(void);

/**
 * @brief 关闭功能开关模块（license 置为无效，状态回落 UNINITIALIZED）
 * @return 0 成功
 */
int feature_gate_shutdown(void);

/**
 * @brief 事件回调占位（本模块仅发布 license 变更事件，不消费外部事件）
 * @param event 事件指针（非 NULL，由事件分发器保证）
 * @param user_data 订阅时传入的用户数据（本模块未使用）
 */
void feature_gate_on_event(const event_t* event, void* user_data);

/**
 * @brief 查询模块生命周期状态
 * @return MODULE_STATUS_*（UNINITIALIZED/INITIALIZED/RUNNING/STOPPED）
 */
module_status_t feature_gate_get_status(void);

/**
 * @brief 模块控制命令入口
 * @param cmd 命令字（本模块未实现任何命令）
 * @param arg 命令参数（未使用）
 * @return -ENOTSUP 始终返回
 */
int feature_gate_control(int cmd, void* arg);

/* =============================================================================
 * 模块专用 API
 * ============================================================================= */

/**
 * @brief 应用 license 令牌（与 Kconfig 期望值比对）
 *
 * 返回值 0 仅表示调用成功；token 是否有效请用 feature_gate_license_valid()。
 *
 * @param token license 令牌字符串，须非 NULL（NULL 视为无效参数）
 * @return 0 成功（无论令牌对错）；APP_ERR_INIT 模块未初始化；APP_ERR_INVALID_PARAM
 *         token 为 NULL
 */
int feature_gate_apply_license(const char* token);

/**
 * @brief 当前 license 是否有效
 * @return true license 已应用且与期望值匹配；false 无效或模块未初始化
 */
bool feature_gate_license_valid(void);

/**
 * @brief 查询命名功能是否启用
 *
 * 功能未编译进固件（Kconfig 槽位关闭）时返回 false；core 始终启用；其余功能
 * 需要有效 license。
 *
 * @param feature_name 功能名（FEATURE_GATE_NAME_*）；NULL 按未编译功能处理返回 false
 * @return true 功能启用；false 功能未编译或 license 无效或模块未初始化
 */
bool feature_gate_is_enabled(const char* feature_name);

/**
 * @brief 查询模块状态快照
 * @param out 输出快照指针，须非 NULL
 * @return 0 成功；APP_ERR_INIT 模块未初始化；APP_ERR_INVALID_PARAM out 为 NULL
 */
int feature_gate_get_status_snapshot(feature_gate_status_t* out);

#ifdef __cplusplus
}
#endif

#endif /* FEATURE_GATE_H */
