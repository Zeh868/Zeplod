/**
 * @file data_bus_ready.h
 * @brief Data Bus 就绪通道队列（dispatcher 仅处理有数据的 channel）
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-05-28
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-05-28       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            审查整改：补全文件头修改日志段
 *
 */

#ifndef DATA_BUS_READY_H
#define DATA_BUS_READY_H

#include <zeplod/data_bus.h>

#ifdef __cplusplus
extern "C" {
#endif

void data_bus_ready_init(void);
void data_bus_ready_reset(void);

/**
 * @brief 发布入队后通知分发器（线程或 ISR 均可调用）
 *
 * 使用 dispatch_ready 避免同一 channel 重复入队；仍会通过信号量唤醒 dispatcher。
 */
void data_bus_ready_signal(data_bus_channel_t* ch);

/** destroy 回滚 active 后，若队列仍有数据则重新入就绪环 */
void data_bus_ready_resync(data_bus_channel_t* ch);

/** 就绪环满等异常路径：扫描仍有 pending 数据的 channel（dispatcher 调用） */
bool data_bus_ready_consume_fallback(void);

/**
 * @brief 是否仍有待处理工作（就绪环非空或 fallback 待处理）
 *
 * 以就绪环作为真相来源；dispatcher 排空信号量后据此判定是否需继续处理，
 * 避免把对应未处理就绪项的信号量一并吃掉而造成延迟唤醒。
 */
bool data_bus_ready_pending(void);

/**
 * @brief 弹出一个待分发 channel（dispatcher 线程调用）
 * @return channel 指针，队列为空返回 NULL
 */
data_bus_channel_t* data_bus_ready_pop(void);

/**
 * @brief 校验 channel 仍注册且活跃，并增加 dispatch_hold
 * @return true 可安全分发
 */
bool data_bus_ready_claim(data_bus_channel_t* ch);

/**
 * @brief 完成分发后释放 dispatch_hold，并在队列空时清除 dispatch_ready
 */
void data_bus_ready_finish(data_bus_channel_t* ch);

#ifdef __cplusplus
}
#endif

#endif /* DATA_BUS_READY_H */
