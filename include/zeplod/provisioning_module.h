/**
 * @file provisioning_module.h
 * @brief 配网模块头文件
 *
 * 可选模块（CONFIG_PROVISIONING_MODULE）。设备身份与配网状态机，发布事件 61。
 * 配置要求：CONFIG_EVENT_MAX_TYPES > 61。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 3 初始版本
 * 2026-09-18       1.1            zeh            reset() 更新为同时删除持久化凭据；
 *                                                 模块接口 7 件套补全 Doxygen 注释
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
 * 模块接口（module_interface_t，由 module_manager 生命周期驱动）
 * ============================================================================= */

/**
 * @brief 初始化配网模块（探测已持久化凭据，存在则直接进入 PROVISIONED）
 * @param config 模块配置指针；本模块未使用，允许为 NULL
 * @return 0 成功；重复初始化幂等返回 0
 */
int provisioning_module_init(void* config);

/**
 * @brief 启动配网模块
 * @return 0 成功；APP_ERR_INIT 未初始化；重复启动幂等返回 0
 */
int provisioning_module_start(void);

/**
 * @brief 停止配网模块
 * @return 0 成功；未运行时亦返回 0（幂等）
 */
int provisioning_module_stop(void);

/**
 * @brief 关闭配网模块（状态回落 UNINITIALIZED）
 * @return 0 成功
 */
int provisioning_module_shutdown(void);

/**
 * @brief 事件回调占位（本模块不消费外部事件，预留供 connectivity 等联动）
 * @param event 事件指针（非 NULL，由事件分发器保证）
 * @param user_data 订阅时传入的用户数据（本模块未使用）
 */
void provisioning_module_on_event(const event_t* event, void* user_data);

/**
 * @brief 查询模块生命周期状态
 * @return MODULE_STATUS_*（UNINITIALIZED/INITIALIZED/RUNNING/STOPPED）
 */
module_status_t provisioning_module_get_status(void);

/**
 * @brief 模块控制命令入口
 * @param cmd 命令字（本模块未实现任何命令）
 * @param arg 命令参数（未使用）
 * @return -ENOTSUP 始终返回
 */
int provisioning_module_control(int cmd, void* arg);

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
 * @return 0 成功；APP_ERR_PROVISIONING 已配网拒绝重复；APP_ERR_INIT 模块未运行；
 *         其余为 set_credentials 落盘失败的错误码
 */
int provisioning_module_begin(const provisioning_credentials_t* creds);

/**
 * @brief 重置为未配网并删除已持久化的 Wi-Fi 凭据
 *
 * 状态机回到 UNPROVISIONED，同时删除 app_kv 中的 wifi.ssid / wifi.psk 两键；
 * 编译 CONFIG_APP_KV_PERSIST 时再调用 app_kv_save() 落盘，确保重启后不会
 * 因 flash 残留凭据而被重新判定为 PROVISIONED。
 *
 * @return 0（凭据删除/落盘失败仅记日志，不改变返回值）
 */
int provisioning_module_reset(void);

/**
 * @brief 查询配网状态
 * @param out_state 输出状态指针，须非 NULL
 * @return 0 成功；APP_ERR_INVALID_PARAM out_state 为 NULL
 */
int provisioning_module_get_state(provisioning_state_t* out_state);

/**
 * @brief 读取设备 ID 字符串（来自 Kconfig 或测试默认值）
 * @param out 输出缓冲区，须非 NULL
 * @param out_len 缓冲区长度（须 > 0，含结尾 NUL）
 * @return 0 成功；APP_ERR_INVALID_PARAM 参数非法；-ENOMEM 缓冲不足
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
