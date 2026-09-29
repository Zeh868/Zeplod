/**
 * @file event_timer.h
 * @brief 事件定时器池内部共享定义（不对外公开）
 *
 * 声明定时槽结构与池级内部接口，供 event_timer.c 实现与生命周期收尾使用。
 * 槽状态机（FREE/ARMED/FIRING）以原子变量承载：线程侧提交/取消持池锁，
 * 到期回调（时钟 ISR 上下文）无锁，仅靠原子 CAS 与世代号仲裁竞态。
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-09-29
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-09-27       1.0            zeh            初始版本（B3：延迟发布 + 周期发布共池交付）
 * 2026-09-29       1.1            zeh            增加 event_timer_pool_init：k_timer 只接线一次
 *
 */

#ifndef EVENT_TIMER_H
#define EVENT_TIMER_H

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <zeplod/event_system.h>

#if defined(CONFIG_EVENT_TIMER_POOL)

/** 槽状态：空闲（BSS 初值一致） */
#define EVENT_TIMER_SLOT_FREE     0
/** 槽状态：已入槽等待到期 */
#define EVENT_TIMER_SLOT_ARMED    1
/** 槽状态：到期回调已通过校验、正在投递 */
#define EVENT_TIMER_SLOT_FIRING   2

/** 槽模式：一次性（延迟发布，到期即回收） */
#define EVENT_TIMER_MODE_ONESHOT  0U
/** 槽模式：周期（到期后由 k_timer 周期自动重武装） */
#define EVENT_TIMER_MODE_PERIODIC 1U

/**
 * @brief 定时槽：k_timer + 事件壳 + 负载副本 + 世代号 + 状态机
 *
 * shell 持元数据与内联负载；超内联负载由 payload 单独持有：
 * - ONESHOT：payload 为待投递私有副本，成功入队即随队列副本转移所有权，
 *   失败则由到期回调就地释放（到期路径零分配）；
 * - PERIODIC：payload 为母本（仅超内联时独立持有），每轮由到期回调经
 *   event_create_with_data_rt() 生成全新副本。
 * state/fire_count/miss_count 为原子字段供无锁到期路径读写；
 * 其余字段仅在池锁保护下或到期回调持有 FIRING 状态期间访问。
 */
typedef struct {
    struct k_timer timer;      /**< Zephyr 软定时器（到期回调运行于时钟 ISR 上下文） */
    event_t        shell;      /**< 事件壳（元数据 + 内联负载），到期时整体入队 */
    void*          payload;    /**< 超内联负载指针（ONESHOT 待投递副本 / PERIODIC 母本） */
    uint32_t       len;        /**< 负载长度（字节） */
    uint32_t       period_ms;  /**< 周期（毫秒，仅 PERIODIC 有效） */
    uint16_t       gen;        /**< 世代号：槽每次释放 +1（跳过保留值 0xFFFF），防句柄 ABA */
    uint8_t        mode;       /**< EVENT_TIMER_MODE_ONESHOT / EVENT_TIMER_MODE_PERIODIC */
    atomic_t       state;      /**< 槽状态机 EVENT_TIMER_SLOT_FREE/ARMED/FIRING */
    atomic_t       fire_count; /**< 该槽累计成功入队次数 */
    atomic_t       miss_count; /**< 该槽累计投递失败次数 */
} event_timer_slot_t;

/**
 * @brief 初始化定时槽的 k_timer（event_system_init 路径，只执行一次）
 *
 * 到期回调返回后 Zephyr 仍会再拿定时器锁收尾。领槽时若再次 k_timer_init，
 * 会和这段收尾重叠。因此整池只在首次 init 接线，之后只 stop/start。
 *
 * @note 仅线程上下文；重复调用无效果
 */
void event_timer_pool_init(void);

/**
 * @brief 取消全部定时槽并释放负载（shutdown 收尾）
 *
 * 逐槽 k_timer_stop、等待在飞到期回调退出（FIRING 状态由回调自行收尾）、
 * 释放槽内负载并复位世代号；随后清零全局 miss 统计。与在飞 expiry 的竞态
 * 由槽状态机 + gen 校验兜底（单核与 SMP 同此兜底）。
 *
 * @note 仅线程上下文（event_system_shutdown() 路径调用）；幂等，可重复调用
 */
void event_timer_pool_cancel_all(void);

#endif /* CONFIG_EVENT_TIMER_POOL */

#endif /* EVENT_TIMER_H */
