/**
 * @file sys_fault_dump.h
 * @brief 故障事件环（retained RAM）导出
 *
 * 可选服务（CONFIG_SYS_FAULT_DUMP_ENABLE）。记录最近 N 条紧凑故障记录，
 * 供 recovery / 诊断在复位后读取。Phase 2 使用 .noinit RAM，量产可换 retained_mem。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 2 初始版本
 * 2026-09-18       1.1            zeh            补充线程安全契约说明（k_spinlock）
 *
 * @note 线程安全契约：record/export/clear 均以 k_spinlock 保护 head/count 与字段
 *       写入（fault 上下文可能从 ISR 调用，不使用 k_mutex）。并发 record 同一
 *       槽位时整条记录"最后写入者胜"，head/count 推进保持一致；export 为持锁快照。
 */

#ifndef SYS_FAULT_DUMP_H
#define SYS_FAULT_DUMP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 记录类型
 * ============================================================================= */

typedef enum {
    FAULT_DUMP_KIND_MODULE_ERROR = 1,
    FAULT_DUMP_KIND_OTA_ERROR,
    FAULT_DUMP_KIND_WDT_PRE_EXPIRE,
} fault_dump_kind_t;

/* =============================================================================
 * API
 * ============================================================================= */

/**
 * @brief 初始化故障环（幂等）
 *
 * @return 0 成功
 */
int sys_fault_dump_init(void);

/**
 * @brief 追加一条故障记录
 *
 * @param kind 记录类型
 * @param data 负载（可为 NULL）
 * @param len 负载长度（截断至条目上限）
 * @return 0 成功；-EINVAL
 */
int sys_fault_dump_record(fault_dump_kind_t kind, const void* data, size_t len);

/**
 * @brief 导出原始环缓冲（二进制）
 *
 * @param out 输出缓冲
 * @param out_len 容量
 * @param out_written 实际写入（可为 NULL）
 * @return 0 成功；-EINVAL / -ENOMEM
 */
int sys_fault_dump_export(uint8_t* out, size_t out_len, size_t* out_written);

/**
 * @brief 清空环（保留 magic）
 */
void sys_fault_dump_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* SYS_FAULT_DUMP_H */
