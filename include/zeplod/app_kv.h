/**
 * @file app_kv.h
 * @brief 应用键值配置（字符串 key / value，默认 RAM）
 *
 * 可选 CONFIG_APP_KV_PERSIST：整表序列化为单条 Settings blob 写入 flash（需分区与 overlay）。
 * 线程安全。
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            审查整改：补全 set_int32/get_int32 Doxygen
 *
 */

#ifndef APP_KV_H
#define APP_KV_H

#include <zeplod/app_config.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化键值表与互斥锁（由 SYS_INIT 调用；可重复调用，无副作用）
 */
void app_kv_init(void);

/**
 * @brief 写入或覆盖键值（拷贝字符串，key/value 须含结尾 0）
 * @return APP_OK，或 APP_ERR_* / APP_ERR_KV_FULL
 */
int app_kv_set(const char* key, const char* value);

/**
 * @brief 读取键对应值，写入 out（含 NUL，至多 out_len-1 字符）
 * @return APP_OK；未找到为 APP_ERR_NOT_FOUND。value 长于 out_len-1 时仍会截断并返回
 * APP_OK（调用方可用更长缓冲或先估长）
 */
int app_kv_get(const char* key, char* out, size_t out_len);

/**
 * @brief 是否存在键
 */
bool app_kv_has(const char* key);

/**
 * @brief 删除键
 * @return APP_OK；不存在为 APP_ERR_NOT_FOUND
 */
int app_kv_remove(const char* key);

/** 清空全部条目 */
void app_kv_clear(void);

/** 当前占用槽位数 */
size_t app_kv_count(void);

typedef int (*app_kv_visit_fn)(const char* key, const char* value, void* user);

/**
 * @brief 遍历有效键值；回调返回非 0 时中止并返回该值
 * @note 回调内勿再调用会抢占同一把互斥锁的 app_kv_*（如 set/get/remove/clear/save/load/foreach），以免死锁
 */
int app_kv_foreach(app_kv_visit_fn fn, void* user);

/**
 * @brief 以十进制文本写入 int32 值（等价于 app_kv_set 的整数便捷封装）
 * @param key 键名（非空，长度 < APP_KV_KEY_MAX_LEN；NULL 或空串为参数错误）
 * @param v 待写入的 32 位有符号整数
 * @return APP_OK 成功；否则为 APP_ERR_INVALID_PARAM / APP_ERR_KV_FULL 等（见 app_kv_set）
 */
int app_kv_set_int32(const char* key, int32_t v);

/**
 * @brief 读取 int32 值（app_kv_get + strtol 解析，含范围校验）
 * @param key 键名（非空；NULL 为参数错误）
 * @param out 输出：解析后的值；NULL 为参数错误
 * @return APP_OK 成功；APP_ERR_NOT_FOUND 键不存在；
 *         APP_ERR_INVALID_PARAM 参数错误、内容非合法整数或超出 int32 范围
 */
int app_kv_get_int32(const char* key, int32_t* out);

/**
 * @brief 将当前 RAM 表写入 flash（需 CONFIG_APP_KV_PERSIST）
 * @return APP_OK；未编译持久化或未启用 Kconfig 时为 APP_ERR_DISABLED；失败为 APP_ERR_IO 等
 */
int app_kv_save(void);

/**
 * @brief 从 flash 重新加载并替换当前 RAM 表（需 CONFIG_APP_KV_PERSIST）
 */
int app_kv_load(void);

/* =============================================================================
 * Schema 版本与升级迁移（OTA / 配置结构变更）
 * ============================================================================= */

/** 单模块可注册的迁移步骤上限 */
#ifndef APP_KV_MIGRATE_MAX_STEPS
#define APP_KV_MIGRATE_MAX_STEPS 4
#endif

/**
 * @brief KV schema 迁移回调
 * @param from_ver 当前 schema 版本（步骤起点）
 * @param to_ver 目标 schema 版本（步骤终点）
 * @param user_data 注册时传入的用户数据
 * @return 0 成功；负 errno 风格错误码中止迁移链
 */
typedef int (*app_kv_migrate_fn)(uint32_t from_ver, uint32_t to_ver, void* user_data);

/**
 * @brief 注册单步 schema 迁移（from_ver → to_ver）
 * @return APP_OK；槽位满或参数无效时返回 APP_ERR_*
 * @note 须满足 from_ver < to_ver；同一步骤重复注册返回 APP_ERR_ALREADY_EXISTS
 */
int app_kv_register_migrate(uint32_t from_ver, uint32_t to_ver, app_kv_migrate_fn fn, void* user_data);

#if defined(UNIT_TEST)
/** 测试专用：清空迁移注册表（仅 ztest 用例间隔离使用，产品构建不可见） */
void app_kv_test_reset_migrations(void);
#endif

/** 设置当前 schema 版本（RAM，不自动持久化） */
int app_kv_set_schema_version(uint32_t version);

/** 读取当前 schema 版本 */
uint32_t app_kv_get_schema_version(void);

/**
 * @brief 按已注册步骤将 schema 从当前版本逐步迁移到可达的最高版本
 * @return APP_OK；迁移回调失败时返回其错误码；检测到环时返回 APP_ERR_INVALID_PARAM
 */
int app_kv_run_migrations(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_KV_H */
