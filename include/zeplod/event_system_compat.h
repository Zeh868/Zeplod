/**
 * @file event_system_compat.h
 * @brief 标准事件系统应用适配层
 *
 * 保留应用层使用的初始化、停止和统计入口。商业事件系统支持已移除，
 * 事件注册、订阅、发布等操作直接使用 event_system.h 的标准 API。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-06
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-06       1.1            zeh            移除商业事件系统适配，仅保留标准实现
 * 2026-09-18       1.2            zeh            补全 7 个公开函数 Doxygen；标注兼容字段无效/恒 0 与移除计划
 */

#ifndef EVENT_SYSTEM_COMPAT_H
#define EVENT_SYSTEM_COMPAT_H

#include <stdbool.h>
#include <stdint.h>
#include <zeplod/event_system.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 兼容保留的初始化配置
 *
 * 标准事件系统使用 Kconfig 配置，以下字段均为无效字段（不会改变任何运行参数，
 * 既有豁免：仅为避免破坏已有应用调用而保留），计划于 2.0 移除；
 * 新代码请通过 prj.conf / Kconfig 配置事件系统。
 */
typedef struct {
    uint16_t high_priority_queue_size;  /**< 无效字段（Kconfig 接管），计划于 2.0 移除 */
    uint16_t normal_priority_queue_size; /**< 无效字段（Kconfig 接管），计划于 2.0 移除 */
    uint16_t low_priority_queue_size;   /**< 无效字段（Kconfig 接管），计划于 2.0 移除 */
    bool     enable_playback;           /**< 无效字段（未实现回放），计划于 2.0 移除 */
    bool     enable_statistics;         /**< 无效字段（统计恒开启），计划于 2.0 移除 */
    bool     enable_rate_limit;         /**< 无效字段（未实现限流），计划于 2.0 移除 */
    bool     enable_batch;              /**< 无效字段（批处理由分发器负责），计划于 2.0 移除 */
    bool     enable_persist;            /**< 无效字段（未实现持久化），计划于 2.0 移除 */
    bool     enable_profiling;          /**< 无效字段（未实现剖析），计划于 2.0 移除 */
    bool     enable_security;           /**< 无效字段（未实现安全策略），计划于 2.0 移除 */
} event_compat_config_t;

/**
 * @brief 事件系统统计信息
 *
 * 前三个字段由 event_get_statistics 填充；后三个历史扩展字段恒为 0
 * （既有豁免：兼容已有 Shell 和应用代码），计划于 2.0 移除。
 */
typedef struct {
    uint32_t total_events;           /**< 已处理的事件总数 */
    uint32_t queue_depth;            /**< 当前队列深度 */
    uint32_t dropped_events;         /**< 被丢弃的事件数量 */
    uint32_t high_priority_processed; /**< 恒为 0（历史扩展字段），计划于 2.0 移除 */
    uint32_t batch_operations;       /**< 恒为 0（历史扩展字段），计划于 2.0 移除 */
    uint32_t rate_limited_events;    /**< 恒为 0（历史扩展字段），计划于 2.0 移除 */
} event_compat_stats_t;

/**
 * @brief 初始化事件系统（兼容入口）
 *
 * @param config 兼容配置，允许为 NULL；当前实现忽略其内容（字段均无效，见 event_compat_config_t）
 * @return 0 成功；-ENOMEM 资源不足，-EINVAL 参数/状态错误，-ETIMEDOUT 超时，
 *         -ECANCELED 系统未运行，-EIO 其他内部错误
 *
 * @note 幂等：事件系统已初始化时直接返回 0
 */
int event_compat_init(const event_compat_config_t* config);

/**
 * @brief 启动事件系统投递（兼容入口）
 *
 * @return 0 成功（含已处于运行态的幂等成功）；-EINVAL 未初始化或状态错误，
 *         -ETIMEDOUT 生命周期迁移冲突，其他负值见 event_compat_init
 *
 * @note 须先调用 event_compat_init()
 */
int event_compat_start(void);

/**
 * @brief 停止事件系统投递（兼容入口）
 *
 * @return 0 成功（含未初始化时的幂等成功）；-EINVAL 从分发器线程调用，
 *         -ETIMEDOUT 在途操作排空或线程 join 超时
 *
 * @note stop 不释放队列与订阅表，可再次 start 恢复投递
 */
int event_compat_stop(void);

/**
 * @brief 检查事件系统是否正在运行（兼容入口）
 *
 * @return true 正在运行，false 已停止或未初始化
 */
bool event_compat_is_running(void);

/**
 * @brief 关闭事件系统并释放全部资源（兼容入口）
 *
 * @return 0 成功；-EINVAL 从分发器线程调用，-ETIMEDOUT 线程 join 超时
 *         （此时 dispatcher 线程可能仍存活，建议系统复位），其他负值见 event_compat_init
 *
 * @note 调用后需重新 event_compat_init() 才能再次使用
 */
int event_compat_shutdown(void);

/**
 * @brief 获取事件系统统计信息（兼容入口）
 *
 * @param stats 输出：统计结构指针；允许为 NULL（此时不执行任何操作）。
 *              非 NULL 时函数先整体清零再填充，后三个历史字段恒为 0
 */
void event_compat_get_statistics(event_compat_stats_t* stats);

/**
 * @brief 重置事件系统统计信息（兼容入口）
 *
 * 清零全局计数并联动重置事件队列与分发器统计。
 */
void event_compat_reset_statistics(void);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_SYSTEM_COMPAT_H */
