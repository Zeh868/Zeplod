/**
 * @file module_manager_planner_internal.h
 * @brief 依赖规划器内部声明（仅供 module_manager 内部使用）
 *
 * 这些函数假定调用方不持有 module_manager_lock；函数自身会短暂获取
 * module_manager 锁以读取模块状态。该头不再对应用层公开。
 *
 * @note 应用层不应直接 include 本头。如需获取启动/停止顺序，
 *       请使用 module_manager_start_all / module_manager_stop_all。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-06
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-06-06       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            补 @version 与修改日志段
 */

#ifndef MODULE_MANAGER_PLANNER_INTERNAL_H
#define MODULE_MANAGER_PLANNER_INTERNAL_H

#include <zeplod/module_dependency_planner.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 按 priority 升序（数值小优先启动） */
void mm_dep_planner_sort_priority_asc(mm_dep_order_entry_t* entries, int n);

#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
/** 按 priority 降序（停止时作 fallback） */
void mm_dep_planner_sort_priority_desc(mm_dep_order_entry_t* entries, int n);

/**
 * @brief 校验批次内依赖并拓扑排序（启动顺序）
 * @pre 调用方不得持有 module_manager_lock
 * @return 有效条目数（可能小于 n）
 */
int mm_dep_planner_build_start_order(mm_dep_order_entry_t* entries, int n);

/**
 * @brief 拓扑排序后反转为停止顺序
 * @pre 调用方不得持有 module_manager_lock
 */
int mm_dep_planner_build_stop_order(mm_dep_order_entry_t* entries, int n);
#endif /* CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES */

#ifdef __cplusplus
}
#endif

#endif /* MODULE_MANAGER_PLANNER_INTERNAL_H */
