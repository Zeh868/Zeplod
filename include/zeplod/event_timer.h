/**
 * @file event_timer.h
 * @brief 延迟发布与周期发布（事件定时器池）公共 API
 *
 * 基于静态 k_timer 槽池为事件系统提供两类定时投递：
 * - 延迟发布：event_publish_delayed_copy() 将负载深拷贝入槽，delay_ms 后自动投递一次；
 * - 周期发布：event_periodic_start() 以 period_ms 为周期自动重发，显式 cancel 停止。
 *
 * 两特性共享同一静态槽池（CONFIG_EVENT_TIMER_SLOTS）；Kconfig 默认关闭，
 * 关闭时零链接残留、行为零变化。「绝对时刻发布」不在本模块语义内。
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-09-27
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-09-27       1.0            zeh            初始版本（B3：延迟发布 + 周期发布共池交付）
 * 2026-09-27       1.1            zeh            cancel 族新增 EVENT_ERR_TIMEOUT 返回语义（在飞到期回调护栏耗尽，m-6）
 *
 */

#ifndef ZEPLOD_EVENT_TIMER_H
#define ZEPLOD_EVENT_TIMER_H

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zeplod/event_system.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 定时发布槽句柄
 *
 * slot 与 gen 合体寻址：gen 为槽世代号，槽每次释放递增，用于防 ABA——
 * 旧句柄在槽被取消/到期回收并复用后自动失效（cancel/get_stats 返回 EVENT_ERR_NOT_FOUND）。
 */
typedef struct {
    uint16_t slot; /**< 槽下标（0..CONFIG_EVENT_TIMER_SLOTS-1）；0xFFFF 表示无效 */
    uint16_t gen;  /**< 世代号；0xFFFF 保留为无效标记，不参与分配 */
} event_timer_handle_t;

/** 无效句柄常量（slot/gen 均为保留值 0xFFFF） */
#define EVENT_TIMER_HANDLE_INVALID ((event_timer_handle_t){.slot = 0xFFFFU, .gen = 0xFFFFU})

#if defined(CONFIG_EVENT_TIMER_POOL) || defined(__DOXYGEN__)

/* =============================================================================
 * 延迟发布（CONFIG_EVENT_TIMER_POOL）
 * ============================================================================= */

/**
 * @brief 延迟发布：深拷贝负载入槽，delay_ms 后自动投递一次
 *
 * 调用时把负载完整拷贝进静态槽（内联或私有副本），到期回调（时钟 ISR 上下文）
 * 直接以槽内事件壳入队——到期投递路径零内存分配。投递为尽力而为：到期时
 * 队列满或系统未 running 则丢弃该次并计 miss，槽随即回收，不重试不补发。
 *
 * @param type 事件类型 ID（0-255，须已 register_type）
 * @param priority 事件优先级（EVENT_PRIORITY_LOW/NORMAL/HIGH/CRITICAL）
 * @param data 负载指针；NULL 时 data_len 必须为 0
 * @param data_len 负载长度（字节，0-4096；超过 4096 返回 EVENT_ERR_INVALID_ARG，
 *                 上界保证到期投递零 k_malloc）
 * @param delay_ms 相对延迟（毫秒，1-0x7FFFFFFF）；0 按 1 处理
 * @param handle_out 输出：槽句柄（可为 NULL，表示无需后续取消/查询）
 * @return EVENT_OK 成功入槽
 * @return EVENT_ERR_INVALID_ARG 参数非法（类型越界/data 与 data_len 不一致/
 *         data_len 超 4096/delay_ms 超 0x7FFFFFFF/系统未初始化/ISR 上下文调用）
 * @return EVENT_ERR_NOT_FOUND 事件类型未注册
 * @return EVENT_ERR_NO_MEM 槽池已满，或超内联负载拷贝失败
 * @return EVENT_ERR_NOT_RUNNING 事件系统未处于 running（stop 后提交被拒）
 *
 * @note 仅线程上下文；ISR 中请直接 event_publish_from_isr()
 * @note 返回成功后调用方缓冲即可释放/复用（负载已深拷贝）
 * @note event_system_stop() 不取消已入槽的延迟发布：到期时投递被拒计 miss，
 *       槽回收；event_system_shutdown() 取消全部槽并释放负载
 * @note 到期投递的元数据（timestamp）在到期时刻刷新
 */
event_status_t event_publish_delayed_copy(event_type_t type, event_priority_t priority, const void* data,
                                          size_t data_len, uint32_t delay_ms, event_timer_handle_t* handle_out);

/**
 * @brief 取消一个延迟/周期发布槽
 *
 * 以 gen 世代号仲裁与到期回调的竞态：若回调已通过状态机校验（正在投递），
 * 返回 EVENT_ERR_NOT_FOUND 且不干扰该次投递；否则停止定时器、释放槽内负载、
 * 槽立即回收（gen 递增，句柄失效）。
 *
 * @param handle event_publish_delayed_copy()/event_periodic_start() 返回的句柄
 * @return EVENT_OK 取消成功，槽已回收
 * @return EVENT_ERR_INVALID_ARG 句柄格式非法（slot 越界/gen 为保留值）或系统未初始化
 * @return EVENT_ERR_NOT_FOUND 句柄已失效（槽已到期回收/已取消/gen 不匹配），
 *         或到期回调已通过校验正在投递（不干扰）
 * @return EVENT_ERR_TIMEOUT 在飞到期回调未在护栏窗口内收尾（SMP 长尾）：
 *         语义为「到期回调仍在投递中」而非「已取消」，槽与负载由回调收尾
 *
 * @note 仅线程上下文；对同一句柄重复取消返回 EVENT_ERR_NOT_FOUND
 * @note 对周期槽调用本函数等价 event_periodic_cancel()
 */
event_status_t event_delayed_cancel(event_timer_handle_t handle);

/* =============================================================================
 * 诊断查询（CONFIG_EVENT_TIMER_POOL）
 * ============================================================================= */

/**
 * @brief 当前已入槽（ARMED，等待到期）的槽数
 *
 * @return 已武装槽数（延迟 + 周期合计）；恰在投递中的槽（FIRING 窗口）不计
 *
 * @note 仅线程上下文（内部短暂持有池锁）
 */
uint32_t event_timer_pending_count(void);

/**
 * @brief 累计投递失败（miss）次数（自上次 shutdown 起累计）
 *
 * 到期投递遇队列满/未 running/副本分配失败/周期轮次跳过均 +1。
 *
 * @return 累计 miss 次数（原子读，ISR 安全）
 */
uint32_t event_timer_miss_count(void);

/**
 * @brief 查询指定槽的投递统计
 *
 * @param handle 槽句柄
 * @param fire_count 输出：该槽累计成功入队次数（可为 NULL）
 * @param miss_count 输出：该槽累计投递失败次数（可为 NULL）
 * @return EVENT_OK 查询成功
 * @return EVENT_ERR_INVALID_ARG 句柄格式非法
 * @return EVENT_ERR_NOT_FOUND 句柄已失效（槽已回收/gen 不匹配）
 *
 * @note 仅线程上下文；一次性槽投递完成后句柄即失效（gen 已递增）
 */
event_status_t event_timer_get_stats(event_timer_handle_t handle, uint32_t* fire_count, uint32_t* miss_count);

#if defined(CONFIG_EVENT_PERIODIC_PUBLISH) || defined(__DOXYGEN__)

/* =============================================================================
 * 周期发布（须 CONFIG_EVENT_PERIODIC_PUBLISH=y）
 * ============================================================================= */

/**
 * @brief 周期发布：每 period_ms 自动投递一条负载副本，直至显式取消
 *
 * 调用时深拷贝一份母本入槽；每轮到期回调（时钟 ISR 上下文）用母本经
 * event_create_with_data_rt() 生成全新副本（纯 slab 路径，绝不 k_malloc）
 * 后入队。slab 耗尽的轮次跳过并计 miss，不影响后续轮次；投递遇队列满/
 * 未 running 同样计 miss（「停止期间轮次丢弃计 miss，不补发」，恢复后继续）。
 *
 * @param type 事件类型 ID（0-255，须已 register_type）
 * @param priority 事件优先级
 * @param data 母本负载指针；NULL 时 data_len 必须为 0
 * @param data_len 母本负载长度（字节，0-4096；超过 4096 返回 EVENT_ERR_INVALID_ARG）
 * @param period_ms 周期（毫秒，1-0x7FFFFFFF）
 * @param initial_delay_ms 首次投递延迟（毫秒，0-0x7FFFFFFF）；0 表示首次投递
 *                         发生在 period_ms 时刻（k_timer 原生语义）
 * @param handle_out 输出：槽句柄（可为 NULL，表示无需后续取消/查询）
 * @return EVENT_OK 成功入槽并开始周期
 * @return EVENT_ERR_INVALID_ARG 参数非法（period_ms 为 0 或超界/类型越界/
 *         data 与 data_len 不一致/系统未初始化/ISR 上下文调用）
 * @return EVENT_ERR_NOT_FOUND 事件类型未注册
 * @return EVENT_ERR_NO_MEM 槽池已满，或母本拷贝失败
 * @return EVENT_ERR_NOT_RUNNING 事件系统未处于 running
 *
 * @note 仅线程上下文
 * @note 占用的槽与母本在 event_periodic_cancel()/event_delayed_cancel() 或
 *       event_system_shutdown() 时释放
 * @note event_system_stop() 不取消周期槽：停止期间到期轮次计 miss，重新
 *       start 后自动恢复投递
 * @note 本 API 须 CONFIG_EVENT_PERIODIC_PUBLISH=y；未启用时调用返回
 *       EVENT_ERR_INVALID_ARG
 */
event_status_t event_periodic_start(event_type_t type, event_priority_t priority, const void* data, size_t data_len,
                                    uint32_t period_ms, uint32_t initial_delay_ms,
                                    event_timer_handle_t* handle_out);

/**
 * @brief 取消周期发布并释放母本与槽位
 *
 * @param handle event_periodic_start() 返回的句柄
 * @return EVENT_OK 取消成功，槽与母本已释放
 * @return EVENT_ERR_INVALID_ARG 句柄格式非法或系统未初始化
 * @return EVENT_ERR_NOT_FOUND 句柄已失效，或到期回调已通过校验正在投递（不干扰）
 * @return EVENT_ERR_TIMEOUT 在飞到期回调未在护栏窗口内收尾（与 event_delayed_cancel 同口径）
 *
 * @note 仅线程上下文；与 event_delayed_cancel() 等价（句柄语义统一）
 */
event_status_t event_periodic_cancel(event_timer_handle_t handle);

/**
 * @brief 当前活跃（已入槽未取消）的周期发布槽数
 *
 * @return 活跃周期槽数
 *
 * @note 仅线程上下文（内部短暂持有池锁）
 */
uint32_t event_periodic_active_count(void);

#else /* !CONFIG_EVENT_PERIODIC_PUBLISH */

/* 周期特性未启用时提供可编译桩：调用即返回参数错误，保持上层代码无需条件编译 */
static inline event_status_t event_periodic_start(event_type_t type, event_priority_t priority, const void* data,
                                                  size_t data_len, uint32_t period_ms, uint32_t initial_delay_ms,
                                                  event_timer_handle_t* handle_out) {
    ARG_UNUSED(type);
    ARG_UNUSED(priority);
    ARG_UNUSED(data);
    ARG_UNUSED(data_len);
    ARG_UNUSED(period_ms);
    ARG_UNUSED(initial_delay_ms);
    ARG_UNUSED(handle_out);
    return EVENT_ERR_INVALID_ARG;
}

static inline event_status_t event_periodic_cancel(event_timer_handle_t handle) {
    ARG_UNUSED(handle);
    return EVENT_ERR_INVALID_ARG;
}

static inline uint32_t event_periodic_active_count(void) {
    return 0U;
}

#endif /* CONFIG_EVENT_PERIODIC_PUBLISH */

#else /* !CONFIG_EVENT_TIMER_POOL */

/* 定时器池未启用时提供可编译桩：调用即返回参数错误/零值，保持上层代码无需条件编译 */
static inline event_status_t event_publish_delayed_copy(event_type_t type, event_priority_t priority,
                                                        const void* data, size_t data_len, uint32_t delay_ms,
                                                        event_timer_handle_t* handle_out) {
    ARG_UNUSED(type);
    ARG_UNUSED(priority);
    ARG_UNUSED(data);
    ARG_UNUSED(data_len);
    ARG_UNUSED(delay_ms);
    ARG_UNUSED(handle_out);
    return EVENT_ERR_INVALID_ARG;
}

static inline event_status_t event_delayed_cancel(event_timer_handle_t handle) {
    ARG_UNUSED(handle);
    return EVENT_ERR_INVALID_ARG;
}

static inline uint32_t event_timer_pending_count(void) {
    return 0U;
}

static inline uint32_t event_timer_miss_count(void) {
    return 0U;
}

static inline event_status_t event_timer_get_stats(event_timer_handle_t handle, uint32_t* fire_count,
                                                   uint32_t* miss_count) {
    ARG_UNUSED(handle);
    ARG_UNUSED(fire_count);
    ARG_UNUSED(miss_count);
    return EVENT_ERR_INVALID_ARG;
}

static inline event_status_t event_periodic_start(event_type_t type, event_priority_t priority, const void* data,
                                                  size_t data_len, uint32_t period_ms, uint32_t initial_delay_ms,
                                                  event_timer_handle_t* handle_out) {
    ARG_UNUSED(type);
    ARG_UNUSED(priority);
    ARG_UNUSED(data);
    ARG_UNUSED(data_len);
    ARG_UNUSED(period_ms);
    ARG_UNUSED(initial_delay_ms);
    ARG_UNUSED(handle_out);
    return EVENT_ERR_INVALID_ARG;
}

static inline event_status_t event_periodic_cancel(event_timer_handle_t handle) {
    ARG_UNUSED(handle);
    return EVENT_ERR_INVALID_ARG;
}

static inline uint32_t event_periodic_active_count(void) {
    return 0U;
}

#endif /* CONFIG_EVENT_TIMER_POOL */

#ifdef __cplusplus
}
#endif

#endif /* ZEPLOD_EVENT_TIMER_H */
