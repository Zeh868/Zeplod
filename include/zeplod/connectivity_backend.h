/**
 * @file connectivity_backend.h
 * @brief 连接后端抽象（可插拔 vtable）
 *
 * Phase 3 提供 null 后端（ztest / native_sim）；Phase 4 追加 Wi-Fi 原生后端（ESP32-C6 等）。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 3 初始版本
 * 2026-07-09       1.1            zeh            新增 Wi-Fi 后端声明
 *
 */

#ifndef CONNECTIVITY_BACKEND_H
#define CONNECTIVITY_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 后端 vtable
 * ============================================================================= */

typedef struct connectivity_backend_ops connectivity_backend_ops_t;

struct connectivity_backend_ops {
    void* ctx;
    int (*init)(connectivity_backend_ops_t* ops);
    int (*connect)(connectivity_backend_ops_t* ops);
    int (*disconnect)(connectivity_backend_ops_t* ops);
    bool (*is_link_up)(const connectivity_backend_ops_t* ops);
};

/* =============================================================================
 * 内置后端
 * ============================================================================= */

const connectivity_backend_ops_t* connectivity_backend_null_get(void);

/**
 * @brief 获取 Wi-Fi 后端单例（CONFIG_CONNECTIVITY_BACKEND_WIFI）
 *
 * 基于 Zephyr net_mgmt/wifi_mgmt：connect 从 provisioning 模块读取已保存的
 * SSID/PSK 并发起连接，有界等待 net_mgmt 事件回调确认结果后再返回。
 */
const connectivity_backend_ops_t* connectivity_backend_wifi_get(void);

#ifdef __cplusplus
}
#endif

#endif /* CONNECTIVITY_BACKEND_H */
