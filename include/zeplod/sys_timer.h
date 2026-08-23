/**
 * @file sys_timer.h
 * @brief 系统定时器服务头文件
 * @author zeh (china_qzh@163.com)
 * @version 2.0
 * @date 2026-08-23
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-08-23       2.0            zeh            单服务线程模型（RAM 优化）
 *
 * @note v2.0：所有回调在唯一的定时器服务线程（CONFIG_SYS_TIMER_PRIORITY）
 *       中执行。回调应尽量短小；耗时操作请发布事件交由相应模块处理，
 *       长回调会推迟其他定时器的触发。回调耗时超过周期时，错过的触发
 *       会被跳过（miss_count 不含此类跳过，仅统计到期队列打满的丢弃）。
 *
 */

#ifndef SYS_TIMER_H
#define SYS_TIMER_H

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 类型定义
 * ============================================================================= */

/**
 * @brief 定时器句柄（不透明）
 */
typedef struct sys_timer* sys_timer_handle_t;

/**
 * @brief 定时器模式
 */
typedef enum {
    SYS_TIMER_ONESHOT = 0, /* 触发一次 */
    SYS_TIMER_PERIODIC     /* 重复触发 */
} sys_timer_mode_t;

/**
 * @brief 定时器状态
 */
typedef enum { SYS_TIMER_STOPPED = 0, SYS_TIMER_RUNNING, SYS_TIMER_PAUSED, SYS_TIMER_EXPIRED } sys_timer_status_t;

/**
 * @brief 定时器回调函数
 * @param timer 定时器句柄
 * @param user_data 用户数据
 */
typedef void (*sys_timer_callback_t)(sys_timer_handle_t timer, void* user_data);

/**
 * @brief 定时器配置
 */
typedef struct {
    sys_timer_mode_t     mode;
    uint32_t             delay_ms;  /* 初始延迟（或单次时间） */
    uint32_t             period_ms; /* 周期定时器的周期 */
    sys_timer_callback_t callback;
    void*                user_data;
    const char*          name;
    int priority; /* v2 兼容保留：所有回调共享服务线程优先级，此字段被忽略 */
} sys_timer_config_t;

/**
 * @brief 定时器统计
 */
typedef struct {
    uint32_t fire_count;
    uint32_t miss_count;
    uint32_t last_fire_time_ms;
    uint32_t avg_latency_us;
    uint32_t max_latency_us;
} sys_timer_stats_t;

/* =============================================================================
 * 核心 API
 * ============================================================================= */

/**
 * @brief 初始化定时器系统
 * @return 成功返回 0，失败返回负错误码
 */
int sys_timer_init(void);

/**
 * @brief 创建新定时器
 * @param config 定时器配置
 * @return 定时器句柄，失败返回 NULL
 */
sys_timer_handle_t sys_timer_create(const sys_timer_config_t* config);

/**
 * @brief 删除定时器
 *
 * 回调执行期间调用删除会等待回调结束后释放槽位；回调超过
 * SYS_TIMER_DELETE_WAIT_MS 未结束时返回 -EIO，可重试。
 * @note 从定时器回调内部删除自身返回 -EDEADLK（请在回调外删除）
 * @param timer 定时器句柄
 * @return 成功返回 0，失败返回负错误码
 */
int sys_timer_delete(sys_timer_handle_t timer);

/**
 * @brief 启动定时器
 * @param timer 定时器句柄
 * @return 成功返回 0，失败返回负错误码
 */
int sys_timer_start(sys_timer_handle_t timer);

/**
 * @brief 停止定时器
 * @param timer 定时器句柄
 * @return 成功返回 0，失败返回负错误码
 */
int sys_timer_stop(sys_timer_handle_t timer);

/**
 * @brief 重启定时器（停止后使用新延迟重新开始；工作线程保持挂起，不 join）
 * @param timer 定时器句柄
 * @return 成功返回 0，失败返回负错误码
 */
int sys_timer_restart(sys_timer_handle_t timer);

/**
 * @brief 暂停定时器
 * @param timer 定时器句柄
 * @return 成功返回 0，失败返回负错误码
 */
int sys_timer_pause(sys_timer_handle_t timer);

/**
 * @brief 恢复暂停的定时器
 * @param timer 定时器句柄
 * @return 成功返回 0，失败返回负错误码
 */
int sys_timer_resume(sys_timer_handle_t timer);

/**
 * @brief 获取定时器状态
 * @param timer 定时器句柄
 * @return 当前定时器状态
 */
sys_timer_status_t sys_timer_get_status(sys_timer_handle_t timer);

/**
 * @brief 修改定时器周期（用于周期定时器）
 * @param timer 定时器句柄
 * @param period_ms 新周期（毫秒）
 * @return 成功返回 0，失败返回负错误码
 */
int sys_timer_set_period(sys_timer_handle_t timer, uint32_t period_ms);

/**
 * @brief 获取距下次到期的剩余时间
 * @param timer 定时器句柄
 * @return 时间（毫秒，未运行时返回 0）
 */
uint32_t sys_timer_get_time_until_expiry(sys_timer_handle_t timer);

/* =============================================================================
 * 统计 API
 * ============================================================================= */

/**
 * @brief 获取定时器统计
 * @param timer 定时器句柄
 * @param stats 输出：统计信息结构体
 * @return 成功返回 0，失败返回负错误码
 */
int sys_timer_get_stats(sys_timer_handle_t timer, sys_timer_stats_t* stats);

/**
 * @brief 重置定时器统计
 * @param timer 定时器句柄
 * @return 成功返回 0，失败返回负错误码
 */
int sys_timer_reset_stats(sys_timer_handle_t timer);

/* =============================================================================
 * 便捷函数
 * ============================================================================= */

/**
 * @brief 创建并启动单次定时器
 * @param delay_ms 延迟（毫秒）
 * @param callback 回调函数
 * @param user_data 回调用户数据
 * @return 定时器句柄，失败返回 NULL
 */
sys_timer_handle_t sys_timer_oneshot(uint32_t delay_ms, sys_timer_callback_t callback, void* user_data);

/**
 * @brief 创建并启动周期定时器
 * @param period_ms 周期（毫秒）
 * @param callback 回调函数
 * @param user_data 回调用户数据
 * @return 定时器句柄，失败返回 NULL
 */
sys_timer_handle_t sys_timer_periodic(uint32_t period_ms, sys_timer_callback_t callback, void* user_data);

/**
 * @brief 睡眠指定时间（便捷包装器）
 * @param ms 睡眠毫秒数
 */
void sys_timer_sleep(uint32_t ms);

/**
 * @brief 获取系统运行时间（毫秒）
 * @return 运行时间（毫秒）
 */
uint32_t sys_timer_get_uptime(void);

#ifdef __cplusplus
}
#endif

#endif /* SYS_TIMER_H */
