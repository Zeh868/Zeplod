/**
 * @file event_dispatcher.h
 * @brief 事件分发器头文件
 *
 * 高性能事件分发器，支持优先级调度和统计功能。
 *
 * 主要特性：
 * - 可配置的分发器线程参数
 * - 事件过滤功能
 * - 详细的统计信息（处理数量、延迟等）
 * - 支持暂停/恢复功能
 * - 支持手动批处理模式
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-05-09       1.0            zeh            修正文档与注释
 * 2026-09-18       1.1            zeh            get_current_latency 兼容别名注释改中文并引导新代码使用 get_idle_time_us
 *
 */

#ifndef EVENT_DISPATCHER_H
#define EVENT_DISPATCHER_H

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>
#include <zeplod/event_system.h>
#include <zeplod/zepl_thread_service.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 配置验证宏
 * ============================================================================= */

/** 最小合理栈大小（256 字节） */
#ifndef EVENT_DISPATCHER_MIN_STACK_SIZE
#define EVENT_DISPATCHER_MIN_STACK_SIZE 256U
#endif

/** 最大合理栈大小 (64 KB) */
#ifndef EVENT_DISPATCHER_MAX_STACK_SIZE
#define EVENT_DISPATCHER_MAX_STACK_SIZE 65536U
#endif

/** 优先级数值下界（Zephyr 约定：数值越小优先级越高，0 为最高） */
#ifndef EVENT_DISPATCHER_MIN_PRIORITY
#define EVENT_DISPATCHER_MIN_PRIORITY 0
#endif

/** 优先级数值上界（数值越大优先级越低） */
#ifndef EVENT_DISPATCHER_MAX_PRIORITY
#define EVENT_DISPATCHER_MAX_PRIORITY 15
#endif

/** 每个周期最大处理事件数上限 */
#ifndef EVENT_DISPATCHER_MAX_EVENTS_PER_CYCLE
#define EVENT_DISPATCHER_MAX_EVENTS_PER_CYCLE 10000U
#endif

/** 线程超时退出的最大等待时间 (毫秒) */
#ifndef EVENT_DISPATCHER_THREAD_JOIN_TIMEOUT_MS
#define EVENT_DISPATCHER_THREAD_JOIN_TIMEOUT_MS ZEPL_THREAD_SERVICE_JOIN_TIMEOUT_MS
#endif

/** PAUSED 状态下休眠时间 (毫秒) */
#ifndef EVENT_DISPATCHER_PAUSE_SLEEP_MS
#define EVENT_DISPATCHER_PAUSE_SLEEP_MS 10U
#endif

/** 空闲时阻塞等待新事件的超时时间 (毫秒) */
#ifndef EVENT_DISPATCHER_IDLE_TIMEOUT_MS
#define EVENT_DISPATCHER_IDLE_TIMEOUT_MS 10U
#endif

/* =============================================================================
 * 类型定义
 * ============================================================================= */

/**
 * @brief 分发器状态枚举
 */
typedef enum {
    DISPATCHER_STOPPED = 0, /**< 分发器已停止 */
    DISPATCHER_RUNNING,     /**< 分发器正在运行 */
    DISPATCHER_PAUSED       /**< 分发器已暂停 */
} dispatcher_state_t;

/**
 * @brief 分发器配置结构
 *
 * 用于初始化分发器时的配置参数。
 */
typedef struct {
    uint32_t    stack_size;           /**< 分发器线程栈大小（字节） */
    int         priority;             /**< 分发器线程优先级 */
    const char* thread_name;          /**< 分发器线程名称 */
    bool        enable_stats;         /**< 是否启用统计信息 */
    uint32_t    max_events_per_cycle; /**< 每个周期最大处理事件数 */
} dispatcher_config_t;

/**
 * @brief 分发器统计信息结构
 *
 * 包含分发器的运行时统计数据。
 */
typedef struct {
    uint64_t events_processed;  /**< 已处理的事件总数 */
    uint64_t events_dropped;    /**< 队列满等导致的丢弃数 */
    uint64_t events_filtered;   /**< 被过滤器拒绝、未分发给订阅者的出队事件数 */
    uint32_t max_latency_us;    /**< 最大处理延迟（微秒） */
    uint32_t avg_latency_us;    /**< 平均处理延迟（微秒） */
    uint32_t processing_errors; /**< 处理错误次数 */
} dispatcher_stats_t;

/**
 * @brief 事件过滤函数类型
 *
 * 用于过滤事件，决定哪些事件需要被处理。
 *
 * @param event 指向事件的指针
 * @param user_data 用户数据
 * @return true 处理此事件，false 跳过此事件
 */
typedef bool (*event_filter_t)(const event_t* event, void* user_data);

/* =============================================================================
 * 分发器控制 API
 * ============================================================================= */

/**
 * @brief 初始化事件分发器
 *
 * @param config 分发器配置结构指针，NULL 使用默认配置
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 *
 * @note 必须先调用 event_system_init() 初始化事件系统
 * @note 默认配置：
 *   - stack_size: CONFIG_EVENT_DISPATCHER_STACK_SIZE
 *   - priority: CONFIG_EVENT_DISPATCHER_PRIORITY
 *   - thread_name: "event_disp"
 *   - enable_stats: true
 *   - max_events_per_cycle: CONFIG_EVENT_DISPATCHER_MAX_EVENTS_PER_CYCLE
 */
event_status_t event_dispatcher_init(const dispatcher_config_t* config);

/**
 * @brief 启动分发器
 *
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 */
event_status_t event_dispatcher_start(void);

/**
 * @brief 停止分发器
 *
 * @return EVENT_OK 成功；join 正在进行或超时时可能返回 EVENT_ERR_TIMEOUT
 *         （罕见，见实现注释）
 *
 * @note 不得从分发器线程自身调用；否则无法 join，返回 EVENT_ERR_INVALID_ARG
 */
event_status_t event_dispatcher_stop(void);

/**
 * @brief 反初始化分发器（停止线程并清除 init 状态）
 *
 * 幂等：未初始化时返回 EVENT_OK。
 *
 * @return EVENT_OK 成功；stop/join 失败时返回对应错误码（与 event_dispatcher_stop 相同）
 *
 * @note 须在 event_system_shutdown() 之后或与之配对使用；stop 仅停线程，deinit 才清除 is_initialized
 * @note 不得从分发器线程内部调用
 */
event_status_t event_dispatcher_deinit(void);

/**
 * @brief 暂停事件处理
 *
 * 暂停后，分发器线程仍存在但不处理事件。
 *
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 */
event_status_t event_dispatcher_pause(void);

/**
 * @brief 恢复事件处理
 *
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 */
event_status_t event_dispatcher_resume(void);

/**
 * @brief 获取分发器当前状态
 *
 * @return 当前分发器状态（原子读）；未初始化时返回 DISPATCHER_STOPPED
 */
dispatcher_state_t event_dispatcher_get_state(void);

/**
 * @brief 检查分发器是否已完成初始化
 *
 * @return true 已调用 event_dispatcher_init() 且成功，false 尚未初始化
 */
bool event_dispatcher_is_initialized(void);

/**
 * @brief 检查当前线程是否为分发器线程
 *
 * @return true 当前线程是分发器线程，false 不是或未初始化
 *
 * @note 用于避免从分发器线程内部调用 shutdown 等危险操作
 */
bool event_dispatcher_is_current_thread(void);

/* =============================================================================
 * 事件处理 API
 * ============================================================================= */

/**
 * @brief 设置事件过滤器
 *
 * 过滤器用于决定哪些事件需要被处理。
 * @note stop->init 序列会保留 filter/user_data；须确保 user_data 在该序列期间
 * 持续有效，或在重新 init 前调用 event_dispatcher_clear_filter()。
 * @param filter 过滤函数指针
 * @param user_data 用户数据，将传入过滤函数
 */

void event_dispatcher_set_filter(event_filter_t filter, void* user_data);

/**
 * @brief 清除事件过滤器
 *
 * 清除后，所有事件都将被处理。
 */
void event_dispatcher_clear_filter(void);

/**
 * @brief 处理单个事件（用于手动分发模式）
 *
 * @param timeout 等待超时时间
 * @return EVENT_OK 成功处理了事件，其他错误码见 event_status_t
 *
 * @note 分发器线程 RUNNING 时，仅允许分发器线程调用；其他线程返回 EVENT_ERR_INVALID_ARG
 * @note 仅 init、从未 event_dispatcher_start() 时，可从任意线程手动消费（测试/无 autoinit）
 * @note 曾 start 后再 stop：外部线程不得调用，返回 EVENT_ERR_INVALID_ARG
 * @note DISPATCHER_PAUSED 返回 EVENT_ERR_INVALID_ARG
 * @note 返回 EVENT_ERR_QUEUE_EMPTY 表示队列中无事件
 * @note 返回 EVENT_ERR_TIMEOUT 表示在 timeout 内未等到事件
 * @warning 外部线程手动消费时请使用有限 timeout：阻塞期间会持有分发器生命周期门闩，
 *          传入 K_FOREVER 会在无事件到达前长期阻塞 init/start/stop/deinit。
 */
event_status_t event_dispatcher_process_one(k_timeout_t timeout);

/**
 * @brief 处理所有待处理事件
 *
 * @param max_events 最大处理事件数，0 表示使用配置中的默认上限
 * @return 已处理的事件数量
 *
 * @note 此函数会一直处理直到队列为空或达到 max_events 限制
 * @note 返回值包含被过滤器跳过的事件（process_one 对过滤事件亦返回 EVENT_OK）
 * @note 与 process_one 相同的手动消费约束；不得与 RUNNING 的分发器线程并行调用
 */
uint32_t event_dispatcher_process_all(uint32_t max_events);

/* =============================================================================
 * 统计 API
 * ============================================================================= */

/**
 * @brief 获取分发器统计信息
 *
 * @param stats 输出：统计信息结构指针
 */
void event_dispatcher_get_stats(dispatcher_stats_t* stats);

/**
 * @brief 重置分发器统计信息
 *
 * 将所有统计计数器清零。
 */
void event_dispatcher_reset_stats(void);

/**
 * @brief 递增分发器「队列丢弃」统计（供 event_queue 溢出路径调用）
 *
 * @note ISR 安全：使用原子计数，可在 event_publish_from_isr 满队列路径调用
 */
void event_dispatcher_stats_inc_dropped(void);

/**
 * @brief 获取距上次事件处理完成后的空闲时间（微秒）
 *
 * 实现语义为「自上一次 process_event 结束以来经过的时间」，不是
 * 单条事件从创建到分发的排队延迟；后者应使用 event->timestamp 等字段。
 *
 * @return 空闲时间（微秒），与 dispatcher_stats 中基于处理耗时的延迟统计含义不同。
 *         内部基于 k_uptime_get()，有效分辨率约为毫秒级，不宜用于亚毫秒测量。
 *
 * @note 保留 API 名称 event_dispatcher_get_current_latency 以保持向后兼容
 */
uint32_t event_dispatcher_get_idle_time_us(void);

/**
 * @brief event_dispatcher_get_idle_time_us() 的向后兼容别名
 *
 * 兼容豁免：旧 API 名称，语义与 event_dispatcher_get_idle_time_us() 完全一致；
 * 新代码请直接使用 event_dispatcher_get_idle_time_us()。
 *
 * @return 与 event_dispatcher_get_idle_time_us() 相同的空闲时间（微秒）
 */
uint32_t event_dispatcher_get_current_latency(void);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_DISPATCHER_H */
