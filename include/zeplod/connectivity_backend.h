/**
 * @file connectivity_backend.h
 * @brief 连接后端抽象（可插拔 vtable）
 *
 * Phase 3 提供 null 后端（ztest / native_sim）；Phase 4 追加 Wi-Fi 原生后端（ESP32-C6 等）；
 * Phase 5 引入优先级注册表 + failover，vtable 追加可选的 is_available，并新增以太网、蜂窝后端。
 *
 * @par 后端实现约定
 * - init/connect/disconnect 允许阻塞（如等待 net_mgmt 回调、DHCP 绑定），
 *   connectivity_module 保证调用方在**模块锁外**调用这三个接口。
 * - is_link_up/is_available 必须是**非阻塞**的快速状态查询（仅读取后端内部缓存的状态位，
 *   不得执行阻塞 I/O 或长时间等待），因此调用方可能在持有模块锁的情况下调用它们。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.3
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 3 初始版本
 * 2026-07-09       1.1            zeh            新增 Wi-Fi 后端声明
 * 2026-07-09       1.2            zeh            vtable 追加 is_available；新增以太网/蜂窝后端声明
 * 2026-09-18       1.3            zeh            vtable 逐字段标注必需/可选及 NULL 语义
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
    /** 后端私有上下文（必需）：各回调以此回取自身状态，不得为 NULL */
    void* ctx;
    /**
     * @brief （可选）初始化后端资源
     * @return 0 成功；非 0 时 connectivity_module 将该后端从注册表剔除
     * @note 为 NULL 时视为无需初始化（直接成功）
     */
    int (*init)(connectivity_backend_ops_t* ops);
    /**
     * @brief （必需）发起连接
     * @return 0 成功；非 0 表示失败（connectivity_module 会对该后端调用 disconnect 收尾）
     * @note 允许阻塞（等待 net_mgmt 回调等）；connectivity_module 保证在模块锁外调用
     */
    int (*connect)(connectivity_backend_ops_t* ops);
    /**
     * @brief （可选）断开连接
     * @note 为 NULL 时表示后端无需显式断开（如仅清缓存的实现）；允许阻塞，锁外调用
     */
    int (*disconnect)(connectivity_backend_ops_t* ops);
    /**
     * @brief （必需）查询链路是否已 up
     * @note 须非阻塞；为 NULL 时该后端永远不会被判定为已连接（无法成为活动后端）
     */
    bool (*is_link_up)(const connectivity_backend_ops_t* ops);
    /**
     * @brief （可选）查询后端对应硬件当前是否可用（存在且有载波/信号）
     *
     * 供 connectivity_module 的优先级择优连接与 failover 管理器判断某后端此刻是否值得
     * 尝试 connect()。为 NULL 时视为“始终可用”（null 桩后端即如此，不设该字段）。
     * @note 须非阻塞，仅读取后端内部缓存状态，不做实际探测 I/O。
     */
    bool (*is_available)(const connectivity_backend_ops_t* ops);
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

/**
 * @brief 获取以太网后端单例（CONFIG_CONNECTIVITY_BACKEND_ETHERNET）
 *
 * 基于 Zephyr net_mgmt：监听 NET_EVENT_IF_UP/DOWN 与 NET_EVENT_IPV4_ADDR_ADD 维护链路
 * 缓存；connect 确保接口 up 并有界等待 DHCP 绑定到 IPv4 地址。
 */
const connectivity_backend_ops_t* connectivity_backend_ethernet_get(void);

/**
 * @brief 获取蜂窝后端单例（CONFIG_CONNECTIVITY_BACKEND_CELLULAR）
 *
 * 面向通用 Zephyr modem 子系统的骨架实现：监听 NET_EVENT_L4_CONNECTED/DISCONNECTED，
 * connect 有界等待 L4 连接建立。真实可用需在设备树绑定具体 modem 驱动并配置 APN，
 * 本仓库无蜂窝硬件，仅保证编译通过。
 */
const connectivity_backend_ops_t* connectivity_backend_cellular_get(void);

#ifdef __cplusplus
}
#endif

#endif /* CONNECTIVITY_BACKEND_H */
