/**
 * @file remote_ops_backend.h
 * @brief 远程运维后端抽象
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 5 初始版本
 * 2026-09-18       1.1            zeh            vtable 逐字段标注必需/可选及 NULL 语义
 *
 */

#ifndef REMOTE_OPS_BACKEND_H
#define REMOTE_OPS_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct remote_ops_backend_ops remote_ops_backend_ops_t;

struct remote_ops_backend_ops {
    /** 后端私有上下文（必需）：各回调以此回取自身状态，不得为 NULL */
    void* ctx;
    /**
     * @brief （可选）初始化后端资源
     * @return 0 成功；非 0 时 remote_ops 初始化失败
     * @note 为 NULL 时视为无需初始化（直接成功）
     */
    int (*init)(remote_ops_backend_ops_t* ops);
    /**
     * @brief （可选）释放后端资源
     * @note 为 NULL 时视为无需清理；remote_ops_shutdown 在模块锁外调用
     */
    int (*deinit)(remote_ops_backend_ops_t* ops);
    /**
     * @brief （必需）接收导出的诊断 JSON 载荷
     * @param json JSON 字符串（非 NULL，生命周期仅覆盖本次调用，后端须自行拷贝）
     * @param json_len JSON 字节长度（不含 NUL）
     * @return 0 成功；非 0 表示后端接收失败（如缓冲不足）
     * @note 可能在 remote_ops 的 export_lock 持有区间被调用，应避免长时间阻塞
     */
    int (*export_diag)(remote_ops_backend_ops_t* ops, const char* json, size_t json_len);
    /**
     * @brief （可选）读回最近一次 export_diag 载荷
     * @return 0 成功；-ENOENT 尚无导出；-EINVAL / -ENOMEM
     * @note mcumgr 等无 RAM 缓存后端可置 NULL，remote_ops 对此返回 APP_ERR_REMOTE_OPS
     */
    int (*get_last_export)(remote_ops_backend_ops_t* ops, char* out, size_t out_len);
};

/** 获取 null 后端单例（RAM 缓存最近一次导出的 JSON，供测试/读回验证） */
const remote_ops_backend_ops_t* remote_ops_backend_null_get(void);

#ifdef __cplusplus
}
#endif

#endif /* REMOTE_OPS_BACKEND_H */
