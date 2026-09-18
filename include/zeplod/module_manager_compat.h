/**
 * @file module_manager_compat.h
 * @brief 模块管理器兼容层 - 标准版统一入口
 *
 * 提供模块管理器的薄封装，使应用代码统一使用 module_compat_* API。
 * 商业版（PRO）模块已移出本框架，本头文件仅保留标准版映射。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.3
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-09-18       1.3            zeh            补 6 个公开函数 Doxygen；无效配置/恒 0 统计字段逐字段标注
 *
 */

#ifndef MODULE_MANAGER_COMPAT_H
#define MODULE_MANAGER_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

#include <zeplod/module_base.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 配置与统计（统一格式）
 * ============================================================================= */

/**
 * @brief 模块管理器配置（传入 compat_init）
 *
 * @note 兼容层豁免：以下字段在标准版均为无效字段（实现不读取），保留仅为兼容既有
 *       调用方代码；计划 2.0 版本移除。
 */
typedef struct {
    uint16_t max_modules;            /**< 无效字段：标准版槽位数由 CONFIG_MAX_MODULES 决定；计划 2.0 移除 */
    uint16_t max_dependencies;       /**< 无效字段：标准版不限制依赖数；计划 2.0 移除 */
    bool     enable_auto_deps;       /**< 无效字段：依赖规划由 Kconfig 开关控制；计划 2.0 移除 */
    bool     enable_hotplug;         /**< 无效字段：标准版无热插拔；计划 2.0 移除 */
    bool     enable_lifecycle_hooks; /**< 无效字段：标准版无生命周期钩子开关；计划 2.0 移除 */
    bool     enable_health_monitor;  /**< 无效字段：标准版无健康监测；计划 2.0 移除 */
} module_compat_config_t;

/**
 * @brief 模块管理器统计信息（统一格式）
 *
 * @note 兼容层豁免：hotplug_events / dependency_resolutions / health_check_cycles
 *       三个字段在标准版恒为 0，保留仅为格式兼容；计划 2.0 版本移除。
 */
typedef struct {
    uint32_t total_modules;          /**< 当前注册模块总数 */
    uint32_t active_modules;         /**< 运行中模块数 */
    uint32_t error_modules;          /**< 错误状态模块数 */
    uint32_t events_processed;       /**< 已处理事件数 */
    uint32_t events_dropped;         /**< 已丢弃事件数 */
    uint32_t hotplug_events;         /**< 恒为 0：标准版无热插拔；计划 2.0 移除 */
    uint32_t dependency_resolutions; /**< 恒为 0：依赖解析计数未实现；计划 2.0 移除 */
    uint32_t health_check_cycles;    /**< 恒为 0：健康监测未实现；计划 2.0 移除 */
} module_compat_stats_t;

/* =============================================================================
 * 统一初始化 / 统计接口
 * ============================================================================= */

/**
 * @brief 初始化模块管理器（标准版薄封装，透传 module_manager_init）
 *
 * @param config 兼容层配置；允许为 NULL——标准版不读取任何配置字段，
 *               传 NULL 与传全零配置等价
 * @return 0 成功，负值错误码失败（透传 module_manager_init 返回值）
 */
int module_compat_init(const module_compat_config_t* config);

/**
 * @brief 启动模块管理器（透传 module_manager_start）
 *
 * @return 0 成功，负值错误码失败
 */
int module_compat_start(void);

/**
 * @brief 停止模块管理器（透传 module_manager_stop）
 *
 * @return 0 成功，负值错误码失败
 */
int module_compat_stop(void);

/**
 * @brief 关闭模块管理器：注销全部模块并复位（透传 module_manager_shutdown）
 *
 * @return 0 成功，负值错误码失败
 */
int module_compat_shutdown(void);

/**
 * @brief 读取统计信息快照（统一格式）
 *
 * @param stats 输出：统计结构指针，不能为 NULL（为 NULL 时静默忽略）
 * @note 事件计数与模块计数字段透传 module_manager_get_stats；热插拔/依赖解析/
 *       健康检查计数器恒为 0（见结构体注释）
 */
void module_compat_get_stats(module_compat_stats_t* stats);

/**
 * @brief 复位统计信息（透传 module_manager_reset_stats：仅清零事件计数器）
 */
void module_compat_reset_stats(void);

/* =============================================================================
 * 模块 API：标准版宏映射
 * ============================================================================= */

#include <zeplod/module_manager.h>

#define module_compat_register(interface, config, module_id) module_manager_register((interface), (config), (module_id))
#define module_compat_unregister(module_id)                  module_manager_unregister((module_id))
#define module_compat_get_module_info(module_id, out)        module_manager_get_module_info((module_id), (out))
#define module_compat_get_id_by_name(name)                   module_manager_get_id_by_name((name))
#define module_compat_foreach(callback, user_data)           module_manager_foreach((callback), (user_data))
#define module_compat_start_module(module_id)                module_manager_start_module((module_id))
#define module_compat_stop_module(module_id)                 module_manager_stop_module((module_id))
#define module_compat_start_all()                            module_manager_start_all()
#define module_compat_stop_all()                             module_manager_stop_all()
#define module_compat_suspend_module(module_id)              module_manager_suspend_module((module_id))
#define module_compat_resume_module(module_id)               module_manager_resume_module((module_id))
#define module_compat_subscribe(module_id, event_type)       module_manager_subscribe((module_id), (event_type))
#define module_compat_unsubscribe(module_id, event_type)     module_manager_unsubscribe((module_id), (event_type))
#define module_compat_send_to_module(module_id, event)       module_manager_send_to_module((module_id), (event))
#define module_compat_broadcast(event)                       module_manager_broadcast((event))
#define module_compat_dump_info()                            module_manager_dump_info()
#define module_compat_set_callback(callback, user_data)      module_manager_set_callback((callback), (user_data))

#ifdef __cplusplus
}
#endif

#endif /* MODULE_MANAGER_COMPAT_H */
