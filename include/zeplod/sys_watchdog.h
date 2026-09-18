/**
 * @file sys_watchdog.h
 * @brief 系统看门狗服务头文件
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            补充 init 并发语义、get_stats
 *                                                参数 NULL 约定
 *
 */

#ifndef SYS_WATCHDOG_H
#define SYS_WATCHDOG_H

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 类型定义
 * ============================================================================= */

/**
 * @brief 看门狗状态
 */
typedef enum {
    WDT_STATUS_STOPPED = 0,
    WDT_STATUS_RUNNING,
    WDT_STATUS_PAUSED,
    WDT_STATUS_EXPIRED,
    WDT_STATUS_ERROR
} wdt_status_t;

/**
 * @brief 看门狗模式
 */
typedef enum {
    WDT_MODE_SOFTWARE = 0, /* 软件看门狗（线程监控）*/
    WDT_MODE_HARDWARE,     /* 硬件看门狗（如可用）*/
    WDT_MODE_DUAL          /* 硬件和软件同时启用 */
} wdt_mode_t;

/**
 * @brief 看门狗用户回调（过期前调用）；与 Zephyr wdt_callback_t 不同
 */
typedef void (*sys_wdt_user_cb_t)(void* user_data);

/**
 * @brief 看门狗配置
 */
typedef struct {
    wdt_mode_t        mode;
    uint32_t          timeout_ms;
    uint32_t          feed_margin_ms; /* 在此前喂狗以避免竞态 */
    sys_wdt_user_cb_t pre_expire_callback;
    void*             callback_user_data;
    bool              reset_on_expire;
    const char*       name;
} wdt_config_t;

/**
 * @brief 看门狗统计
 */
typedef struct {
    uint32_t feed_count;
    uint32_t warning_count;
    uint32_t expire_count;
    uint32_t reset_count;
    uint32_t max_feed_interval_ms;
    uint32_t last_feed_time_ms;
} wdt_stats_t;

/* =============================================================================
 * 核心 API
 * ============================================================================= */

/**
 * @brief 初始化看门狗系统
 * @param config 看门狗配置；NULL 使用默认配置（Kconfig 超时、软件模式）
 * @return 成功返回 0，失败返回负错误码
 * @retval -EBUSY 看门狗仍活跃（RUNNING/PAUSED、监控线程存活或并发操作进行中）
 *
 * @note 并发语义：重复初始化时短超时试锁探测"操作在途"（feed/监控正持锁拒绝），
 *       并检查运行状态与监控线程存活标志；活跃时返回 -EBUSY，不会摧毁正在
 *       使用的互斥锁与线程控制块。非活跃状态（STOPPED/EXPIRED/ERROR）允许重初始化
 */
int sys_wdt_init(const wdt_config_t* config);

/**
 * @brief 启动看门狗
 * @return 成功返回 0，失败返回负错误码
 */
int sys_wdt_start(void);

/**
 * @brief 停止看门狗
 * @return 成功返回 0，失败返回负错误码
 */
int sys_wdt_stop(void);

/**
 * @brief 喂看门狗
 * @return 成功返回 0，失败返回负错误码
 */
int sys_wdt_feed(void);

/**
 * @brief 设置过期前回调（可运行时更新，供 recovery 等注册）
 *
 * @param callback 回调；NULL 清除
 * @param user_data 用户数据
 * @return 0 成功；-EINVAL 看门狗未初始化
 */
int sys_wdt_set_pre_expire_callback(sys_wdt_user_cb_t callback, void* user_data);

/**
 * @brief 暂停看门狗（调试用）
 * @return 成功返回 0，失败返回负错误码
 */
int sys_wdt_pause(void);

/**
 * @brief 恢复看门狗
 * @return 成功返回 0，失败返回负错误码
 */
int sys_wdt_resume(void);

/**
 * @brief 获取看门狗状态
 * @return 当前看门狗状态
 */
wdt_status_t sys_wdt_get_status(void);

/* =============================================================================
 * 线程监控 API（软件看门狗）
 * ============================================================================= */

/**
 * @brief 注册线程以进行监控
 * @param thread_id 线程标识符
 * @param thread_name 线程名称
 * @param max_idle_ms 最大允许空闲时间
 * @return 成功返回 0，失败返回负错误码
 */
int sys_wdt_monitor_thread(k_tid_t thread_id, const char* thread_name, uint32_t max_idle_ms);

/**
 * @brief 注销线程监控
 * @param thread_id 线程标识符
 * @return 成功返回 0，失败返回负错误码
 */
int sys_wdt_unmonitor_thread(k_tid_t thread_id);

/**
 * @brief 标记线程为存活（由被监控线程调用）
 * @param thread_id 线程标识符
 * @return 成功返回 0，失败返回负错误码
 */
int sys_wdt_thread_alive(k_tid_t thread_id);

/* =============================================================================
 * 统计与调试 API
 * ============================================================================= */

/**
 * @brief 获取看门狗统计信息
 * @param stats 输出：统计信息结构体；NULL 时为无操作（不写入、不报错）
 */
void sys_wdt_get_stats(wdt_stats_t* stats);

/**
 * @brief 重置看门狗统计信息
 */
void sys_wdt_reset_stats(void);

/**
 * @brief 获取自上次喂狗以来的时间
 * @return 时间（毫秒）
 */
uint32_t sys_wdt_get_time_since_feed(void);

/**
 * @brief 获取距过期的时间
 * @return 时间（毫秒，已过期则返回 0）
 */
uint32_t sys_wdt_get_time_until_expire(void);

/**
 * @brief 模拟看门狗过期（测试用）
 */
void sys_wdt_simulate_expire(void);

#ifdef __cplusplus
}
#endif

#endif /* SYS_WATCHDOG_H */
