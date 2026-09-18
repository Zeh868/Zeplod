/**
 * @file module_dependency_planner.h
 * @brief 模块批量启停依赖规划（拓扑 + 优先级）— 公开类型
 *
 * 本头仅暴露数据结构（mm_dep_order_entry_t / start_order_entry_t），
 * 供应用层在调用 module_manager_start_all / module_manager_stop_all 之前
 * 收集条目时使用。具体的排序与拓扑构建函数已迁移到内部头
 * module_manager_planner_internal.h，不再对应用层公开。
 *
 * @note 依赖版本下限按 MODULE_VERSION(major, minor, patch) 打包整数比较，
 *       不解析 semver 字符串。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-06
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-06       1.1            zeh            排序与拓扑函数迁移至内部头，本头仅保留公开类型
 * 2026-09-18       1.2            zeh            修复 @author 断行导致的 Doxygen 解析破碎，补修改日志
 *
 */

#ifndef MODULE_DEPENDENCY_PLANNER_H
#define MODULE_DEPENDENCY_PLANNER_H

#include <zeplod/module_manager.h>

#ifdef __cplusplus
extern "C" {
#endif

#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
typedef struct {
    uint32_t           id;
    module_priority_t  priority;
    const char* const* depends_on;
    const uint32_t*    depends_version_min;
} mm_dep_order_entry_t;
#else
typedef struct {
    uint32_t          id;
    module_priority_t priority;
} mm_dep_order_entry_t;
#endif

/** 与历史 start_order_entry_t 布局相同 */
typedef mm_dep_order_entry_t start_order_entry_t;

#ifdef __cplusplus
}
#endif

#endif /* MODULE_DEPENDENCY_PLANNER_H */
