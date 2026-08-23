/**
 * @file sys_secure_kv.h
 * @brief 安全 KV 服务头文件
 *
 * 可选服务（CONFIG_SYS_SECURE_KV_ENABLE）。在 RAM 中存储敏感二进制值，
 * Phase 2 使用软件 keystream 加解密；量产请换 PSA / AES-GCM 后端。
 *
 * @warning 当前实现的机密性限制（量产前必须知悉）：
 *          1. keystream 为 FNV-1a 哈希派生的异或流，无认证标签（密文被篡改无检测），
 *             已知明文即可逐位置恢复 keystream，不构成密码学安全；
 *          2. 密钥来自 CONFIG_SYS_SECURE_KV_KEY_HEX，编译进固件镜像，可被固件提取；
 *          3. 仅存 RAM，重启即失，无持久化。
 *          因此本服务现阶段只适合作为「防随手读取的混淆存储」；任何真正的机密
 *          （令牌、证书、密钥）请在接入 PSA Crypto / AES-GCM 后端后再使用本 API。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 2 初始版本
 *
 */

#ifndef SYS_SECURE_KV_H
#define SYS_SECURE_KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 核心 API
 * ============================================================================= */

/**
 * @brief 初始化 secure KV（解析 CONFIG_SYS_SECURE_KV_KEY_HEX）
 *
 * 幂等；由 SYS_INIT 在 POST_KERNEL 阶段自动调用。
 *
 * @return 成功返回 0；-EINVAL 密钥格式无效
 */
int sys_secure_kv_init(void);

/**
 * @brief 写入加密值（覆盖同名键）
 *
 * @param key 键名（NUL 结尾）
 * @param plain 明文数据
 * @param plain_len 明文长度（字节）
 * @return 成功返回 0；-EINVAL / -ENOMEM
 */
int sys_secure_kv_set(const char* key, const uint8_t* plain, size_t plain_len);

/**
 * @brief 读取并解密
 *
 * @param key 键名
 * @param out 输出缓冲
 * @param out_len 缓冲容量
 * @param out_written 实际写入字节数（可为 NULL）
 * @return 成功返回 0；-ENOENT / -ENOMEM / -EINVAL
 */
int sys_secure_kv_get(const char* key, uint8_t* out, size_t out_len, size_t* out_written);

/**
 * @brief 删除键
 *
 * @param key 键名
 * @return 成功返回 0；-ENOENT / -EINVAL
 */
int sys_secure_kv_remove(const char* key);

/**
 * @brief 键是否存在
 *
 * @param key 键名
 * @return true 存在
 */
bool sys_secure_kv_has(const char* key);

/**
 * @brief 清空全部条目
 */
void sys_secure_kv_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* SYS_SECURE_KV_H */
