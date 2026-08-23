/**
 * @file provisioning_module.h
 * @brief 配网模块头文件
 *
 * 可选模块（CONFIG_PROVISIONING_MODULE）。设备身份与配网状态机，发布事件 61。
 * 配置要求：CONFIG_EVENT_MAX_TYPES > 61。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 3 初始版本
 *
 */

#ifndef PROVISIONING_MODULE_H
#define PROVISIONING_MODULE_H

#include <zeplod/event_system.h>
#include <zeplod/module_base.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 事件类型
 * ============================================================================= */

/** 配网状态变化（负载 provisioning_status_t） */
#define EVENT_PROVISIONING_STATE_CHANGED ((event_type_t) 61)

/* =============================================================================
 * 类型定义
 * ============================================================================= */

typedef enum {
    PROVISIONING_STATE_UNPROVISIONED = 0, /**< 尚未配网 */
    PROVISIONING_STATE_IN_PROGRESS,       /**< 配网进行中 */
    PROVISIONING_STATE_PROVISIONED,       /**< 已完成配网 */
    PROVISIONING_STATE_ERROR,             /**< 配网失败（预留） */
} provisioning_state_t;

typedef struct {
    provisioning_state_t state;
    int                  error_code;
} provisioning_status_t;

typedef struct {
    const char* ssid;
    const char* psk;
} provisioning_credentials_t;

/* =============================================================================
 * Wi-Fi 凭据长度上限
 * ============================================================================= */

/** SSID 缓冲区长度上限（Zephyr WIFI_SSID_MAX_LEN=32 + 结尾 NUL） */
#define PROVISIONING_WIFI_SSID_MAX_LEN 33U

/** PSK 缓冲区长度上限（Zephyr WIFI_PSK_MAX_LEN=64 + 结尾 NUL） */
#define PROVISIONING_WIFI_PSK_MAX_LEN 65U

/* =============================================================================
 * 模块接口
 * ============================================================================= */

int             provisioning_module_init(void* config);
int             provisioning_module_start(void);
int             provisioning_module_stop(void);
int             provisioning_module_shutdown(void);
void            provisioning_module_on_event(const event_t* event, void* user_data);
module_status_t provisioning_module_get_status(void);
int             provisioning_module_control(int cmd, void* arg);

/* =============================================================================
 * 模块专用 API
 * ============================================================================= */

/**
 * @brief 开始配网（须已 start）
 *
 * creds 非空时会先调用 provisioning_module_set_credentials() 落盘凭据，
 * 成功后状态机才推进 IN_PROGRESS → PROVISIONED；落盘失败则置 ERROR 并返回错误码。
 *
 * @param creds 凭据（可为 NULL，仅推进状态机，不写入凭据）
 * @return 0 成功；APP_ERR_PROVISIONING / APP_ERR_INVALID_PARAM / APP_ERR_INIT
 */
int provisioning_module_begin(const provisioning_credentials_t* creds);

/**
 * @brief 重置为未配网
 * @return 0
 */
int provisioning_module_reset(void);

/**
 * @brief 查询配网状态
 */
int provisioning_module_get_state(provisioning_state_t* out_state);

/**
 * @brief 读取设备 ID 字符串（来自 Kconfig 或测试默认值）
 * @return 0 成功；-EINVAL / -ENOMEM
 */
int provisioning_module_get_device_id(char* out, size_t out_len);

/**
 * @brief 写入 Wi-Fi 凭据（SSID/PSK），经 app_kv 持久化到 NVS
 *
 * key 固定为 "wifi.ssid" / "wifi.psk"；写入 RAM 表后若编译了 CONFIG_APP_KV_PERSIST，
 * 会调用 app_kv_save() 落盘。落盘失败仅记日志（RAM 缓存已生效），不视为本函数失败。
 *
 * @param creds 凭据，ssid/psk 均须非空且不超过 PROVISIONING_WIFI_SSID_MAX_LEN /
 * PROVISIONING_WIFI_PSK_MAX_LEN（含结尾 0）
 * @return 0 成功；APP_ERR_INVALID_PARAM 参数非法；APP_ERR_INIT 模块未运行；
 * 其余为 app_kv_set 失败时的错误码（如 APP_ERR_KV_FULL）
 * @warning PSK 以明文经 app_kv 写入并在 CONFIG_APP_KV_PERSIST 下明文落 NVS/flash。
 *          sys_secure_kv 当前为非密码学混淆（见其头文件警告），接入真正的加密后端
 *          前请评估设备物理读取 flash 的威胁模型。
 */
int provisioning_module_set_credentials(const provisioning_credentials_t* creds);

/**
 * @brief 读取已持久化的 Wi-Fi 凭据（SSID/PSK）
 * @param ssid 输出 SSID 缓冲区
 * @param ssid_len 缓冲区长度
 * @param psk 输出 PSK 缓冲区
 * @param psk_len 缓冲区长度
 * @return 0 成功；APP_ERR_INVALID_PARAM 参数非法；APP_ERR_NOT_FOUND 尚未配置凭据
 */
int provisioning_module_get_credentials(char* ssid, size_t ssid_len, char* psk, size_t psk_len);

#ifdef __cplusplus
}
#endif

#endif /* PROVISIONING_MODULE_H */
