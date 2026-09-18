/**
 * @file ota_transport.h
 * @brief OTA 传输层抽象（可插拔 vtable）
 *
 * Phase 1 提供 null 传输（RAM 模拟镜像，供 native_sim 测试）；
 * 产品默认 MCUmgr SMP 被动接入；可选 OTA_TRANSPORT_ACTIVE 自建 write_chunk 扩展。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.4
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            初始版本
 * 2026-06-13       1.1            zeh            增加 ota_transport_mcuboot_get
 * 2026-06-13       1.2            zeh            增加 ota_transport_mcumgr_smp_get
 * 2026-06-13       1.3            zeh            MCUmgr SMP 命名；双 ingest 模型
 * 2026-09-18       1.4            zeh            vtable 逐字段标注必需/可选及 NULL 语义
 *
 */

#ifndef OTA_TRANSPORT_H
#define OTA_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 传输层 vtable
 * ============================================================================= */

typedef struct ota_transport_ops ota_transport_ops_t;

/**
 * @brief OTA 传输操作表
 *
 * 各回调在 ota_module 调用线程上下文中执行（Phase 1 为调用者线程同步调用）。
 * write_chunk/verify 等慢速 IO 由 ota_module 在 xfer_lock 序列化域内、模块锁外调用。
 */
struct ota_transport_ops {
    /** 传输私有上下文（必需）：各回调以此回取自身状态，不得为 NULL */
    void* ctx;
    /**
     * @brief （必需，主动路径）建立传输会话
     * @return 0 成功；非 0 时 ota_module_begin_update 以 APP_ERR_OTA_TRANSPORT 失败返回
     * @note MCUmgr SMP 被动路径中语义为"arm 回调钩子"，在 ota_module_start 调用
     */
    int (*open)(ota_transport_ops_t* ops);
    /**
     * @brief （必需，主动路径）写入镜像数据块
     * @param offset 镜像内偏移（字节）；flash_img 类后端要求严格顺序写入
     * @param data 数据指针（非 NULL）；@param len 数据长度（> 0）
     * @return 0 成功；负 errno 表示写入失败，ota_module 将转入 ERROR 并 abort 会话
     */
    int (*write_chunk)(ota_transport_ops_t* ops, size_t offset, const uint8_t* data, size_t len);
    /**
     * @brief （必需，主动路径）完成写入并校验镜像（刷缓冲、整体校验等慢速 IO）
     * @return 0 成功；负 errno 校验失败（如 -ENODATA 未写入任何数据）
     */
    int (*verify)(ota_transport_ops_t* ops);
    /**
     * @brief （可选）正常收尾会话（如提交 boot_request_upgrade）
     * @return 0 成功；非 0 仅记日志，不影响 READY_REBOOT 状态推进
     * @note 为 NULL 时 ota_module 跳过收尾调用
     */
    int (*close)(ota_transport_ops_t* ops);
    /**
     * @brief （可选，建议实现）中止会话并清理半包数据（如擦除 upload slot）
     * @note 为 NULL 时 abort 为空操作；返回值无意义（void）
     */
    void (*abort)(ota_transport_ops_t* ops);
};

/* =============================================================================
 * 内置传输后端
 * ============================================================================= */

/**
 * @brief 获取 null 传输实例（RAM 模拟镜像）
 * @return 静态 vtable 指针，生命周期为整个进程
 */
const ota_transport_ops_t* ota_transport_null_get(void);

#if defined(CONFIG_OTA_TRANSPORT_ACTIVE) || defined(__DOXYGEN__)
/**
 * @brief 获取 MCUboot flash_img 主动传输实例（自建协议扩展）
 * @return 静态 vtable 指针；须 CONFIG_OTA_TRANSPORT_ACTIVE=y
 */
const ota_transport_ops_t* ota_transport_mcuboot_get(void);
#endif

#if defined(CONFIG_OTA_TRANSPORT_MCUMGR_SMP) || defined(__DOXYGEN__)
/**
 * @brief 获取 MCUmgr SMP 被动接入实例
 * @return 静态 vtable 指针；须 CONFIG_OTA_TRANSPORT_MCUMGR_SMP=y
 */
const ota_transport_ops_t* ota_transport_mcumgr_smp_get(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* OTA_TRANSPORT_H */
