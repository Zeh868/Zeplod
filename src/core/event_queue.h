/**
 * @file event_queue.h
 * @brief 事件队列管理头文件
 *
 * 提供基于优先级的队列管理和溢出处理功能。
 *
 * 主要特性：
 * - 基于 Zephyr k_msgq 的高效队列实现
 * - 多种溢出处理策略
 * - 详细的队列统计信息
 * - 水位线监控
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-27       1.1            zeh            新增优先级批出队 dequeue_priority_batch 内部 API（B4）
 *
 */

#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>
#include <zeplod/event_system.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 配置宏
 * ============================================================================= */

/**
 * @brief 队列高水位线阈值
 * @note 当队列深度超过此值时，可触发告警或降级处理
 */
#ifndef CONFIG_EVENT_QUEUE_HIGH_WATERMARK
#define CONFIG_EVENT_QUEUE_HIGH_WATERMARK (CONFIG_EVENT_QUEUE_SIZE * 3 / 4)
#endif

/* =============================================================================
 * 类型定义
 * ============================================================================= */

/**
 * @brief 队列溢出处理策略枚举
 */
typedef enum {
    QUEUE_OVERFLOW_DROP_LOWEST, /**< 丢弃最低优先级的事件 */
    QUEUE_OVERFLOW_DROP_NEWEST, /**< 丢弃新到达的事件 */
    QUEUE_OVERFLOW_BLOCK        /**< 阻塞等待（不推荐用于实时系统） */
} queue_overflow_policy_t;

/**
 * @brief 队列统计信息结构
 */
typedef struct {
    uint32_t enqueue_count;  /**< 入队操作次数 */
    uint32_t dequeue_count;  /**< 出队操作次数 */
    uint32_t overflow_count; /**< 溢出发生次数 */
    uint32_t drop_count;     /**< 事件丢弃次数 */
    uint32_t high_watermark; /**< 队列深度历史最大值 */
} queue_stats_t;

/* =============================================================================
 * 队列 API
 * ============================================================================= */

/**
 * @brief 初始化事件队列
 *
 * @param queue 指向队列结构的指针
 * @param buffer 队列存储缓冲区
 * @param capacity 队列最大容量
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 */
event_status_t event_queue_init(struct k_msgq* queue, void* buffer, size_t capacity);

/**
 * @brief 入队操作
 *
 * @param queue 队列实例
 * @param event 要入队的事件
 * @param policy 溢出处理策略
 * @param timeout 等待超时时间
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 *
 * @note 调用前需先对该 queue 执行 event_queue_init()，否则返回 EVENT_ERR_INVALID_ARG
 * @note ISR 路径为避免在中断上下文持互斥锁，会进行无锁控制块查询；因此要求
 *       queue 仅在 event_queue_init() 完成后、event_queue_deinit() 开始前调用；
 *       不得与 init/deinit 并发，推荐先停分发/发布再 deinit。
 * @note 优先级批出队期间 ISR 仍可在队列未满时入队；仅 DROP_LOWEST 的排空/回灌
 *       窗口会关闭 ISR 准入。该窗口内的拒收按 EVENT_ERR_QUEUE_FULL 计入 overflow_count。
 * @note QUEUE_OVERFLOW_DROP_LOWEST：在队列满时排空并丢弃 priority 数值最大的一条（同值则丢弃 FIFO
 * 最旧）； 若新事件比队列中最差事件还低，则丢弃新事件。需线程上下文，不可在 ISR 中使用。
 *       启用时线程侧入队/出队由 reorder_lock 串行化，排空/回灌期间屏蔽 ISR 入队。
 *       请避免对同一 k_msgq 并发使用裸 k_msgq_* 与 event_queue API 混用。
 * @note QUEUE_OVERFLOW_BLOCK：event_publish 在启用 CONFIG_EVENT_QUEUE_OVERFLOW_BLOCK
 *       时使用 K_FOREVER 分段阻塞；event_system_stop() 后返回 EVENT_ERR_NOT_RUNNING。
 *       勿在 ISR 中使用。对 BLOCK 传入 K_NO_WAIT 在满队列时返回 EVENT_ERR_QUEUE_FULL。
 */
event_status_t event_queue_enqueue(struct k_msgq* queue, const event_t* event, queue_overflow_policy_t policy,
                                   k_timeout_t timeout);

/**
 * @brief 出队操作
 *
 * @param queue 队列实例
 * @param event 输出：出队的事件
 * @param timeout 等待超时时间
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 *
 * @note DROP_LOWEST scratch 启用后，阻塞等待通过 k_poll 监听 msgq 数据通知；
 *       等待期间不持有 reorder_lock。
 */
event_status_t event_queue_dequeue(struct k_msgq* queue, event_t* event, k_timeout_t timeout);

#if defined(CONFIG_EVENT_PRIORITY_DISPATCH) || defined(__DOXYGEN__)
/**
 * @brief 优先级批出队：排空队列，按「有效优先级」选出至多 max_out 条最优事件
 *
 * 有效优先级 = max(priority - age_steps, 0)，其中
 * age_steps = (now_ms - timestamp) / CONFIG_EVENT_PRIORITY_AGING_STEP_MS；
 * 有效优先级数值最小者最先出队（数值小=优先级高），平局取 FIFO 最旧。
 * 其余事件按原 FIFO 相对次序回灌队列。
 *
 * @param queue 队列实例（须经 event_queue_init()）
 * @param out 输出事件数组，容量不小于 min(max_out, 队列容量)
 * @param max_out 最多取出的事件数（须 > 0）
 * @param remaining 输出：本次出队后队列剩余事件数（可为 NULL）
 * @return 实际写入 out 的事件数；0 表示队列为空或参数非法
 *
 * @note 仅线程上下文调用。内部持有 reorder_lock 并与线程侧入队、DROP_LOWEST 挤占
 *       串行；ISR 入队保持可用，期间新到事件留给下一批排序。若它们占满容量导致
 *       本批剩余事件回灌失败，失败事件按真实溢出释放并计入统计。
 * @note 事件 timestamp 须为创建时刻（event_create 系列与 event_publish_copy
 *       已满足）；手搓 event_t 必须设置 timestamp，否则按「严重老化」处理
 *       （有效优先级归 0），退化为 FIFO 平局次序——不会乱序，但失去优先级语义
 * @note 须 CONFIG_EVENT_PRIORITY_DISPATCH=y
 */
uint32_t event_queue_dequeue_priority_batch(struct k_msgq* queue, event_t* out, uint32_t max_out, uint32_t* remaining);
#endif /* CONFIG_EVENT_PRIORITY_DISPATCH */

/**
 * @brief 检查队列是否为空
 *
 * @param queue 队列实例
 * @return true 队列为空，false 队列非空
 */
bool event_queue_is_empty(const struct k_msgq* queue);

/**
 * @brief 检查队列是否已满
 *
 * @param queue 队列实例
 * @return true 队列已满，false 队列未满
 */
bool event_queue_is_full(const struct k_msgq* queue);

/**
 * @brief 获取队列深度（已用槽位数）
 *
 * @param queue 队列实例
 * @return 队列中的事件数量
 */
uint32_t event_queue_depth(const struct k_msgq* queue);

/**
 * @brief 获取队列容量
 *
 * @param queue 队列实例
 * @return 队列最大容量
 */
uint32_t event_queue_capacity(const struct k_msgq* queue);

/**
 * @brief 清空队列中的所有事件
 *
 * @param queue 队列实例
 * @note 若队列中事件包含动态负载（EVENT_FLAG_DATA_DYNAMIC），会在清空时自动释放 data.ptr
 */
void event_queue_purge(struct k_msgq* queue);

/**
 * @brief 获取队列统计信息
 *
 * @param queue 队列实例
 * @param stats 输出：统计信息结构
 */
void event_queue_get_stats(const struct k_msgq* queue, queue_stats_t* stats);

/**
 * @brief 重置队列统计信息
 *
 * @param queue 队列实例
 */
void event_queue_reset_stats(struct k_msgq* queue);

/**
 * @brief 反初始化事件队列
 *
 * 释放队列相关的所有动态分配资源，包括 DROP_LOWEST scratch 缓冲区。
 * 调用后队列控制块回到未初始化状态。
 *
 * @param queue 队列实例
 * @note 调用前需确保队列中所有事件已被消费或清空
 * @note 重复调用是安全的（幂等）
 */
void event_queue_deinit(struct k_msgq* queue);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_QUEUE_H */
