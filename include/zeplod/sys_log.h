/**
 * @file sys_log.h
 * @brief 系统日志服务头文件
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 *
 */
#ifndef SYS_LOG_H
#define SYS_LOG_H

#include <zephyr/logging/log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 日志级别定义
 * ============================================================================= */

typedef enum {
    SYS_LOG_LEVEL_OFF = 0,
    SYS_LOG_LEVEL_ERR = 1,
    SYS_LOG_LEVEL_WRN = 2,
    SYS_LOG_LEVEL_INF = 3,
    SYS_LOG_LEVEL_DBG = 4,
    SYS_LOG_LEVEL_MAX = 5
} sys_log_level_t;

/* =============================================================================
 * 日志目标（位掩码；可 OR 组合）
 * ============================================================================= */

typedef uint32_t sys_log_dest_mask_t;

#define SYS_LOG_DEST_CONSOLE (1U << 0)
#define SYS_LOG_DEST_MEMORY  (1U << 1)
#define SYS_LOG_DEST_RTT     (1U << 2) /* SEGGER RTT（当 CONFIG_SEGGER_RTT 时）*/
#define SYS_LOG_DEST_UART    (1U << 3) /* 典型开发板上与控制台共用 printk 路径 */
#define SYS_LOG_DEST_ALL     0xFFu

/* =============================================================================
 * 日志条目结构
 * ============================================================================= */

#define SYS_LOG_MSG_MAX_LEN  128

typedef struct {
    uint32_t        timestamp;
    sys_log_level_t level;
    char            module_name[32]; /* SIL-2：存储模块名称副本而非指针 */
    char            message[SYS_LOG_MSG_MAX_LEN];
} sys_log_entry_t;

/* =============================================================================
 * 配置
 * ============================================================================= */

typedef struct {
    sys_log_level_t     default_level;
    sys_log_dest_mask_t destinations;
    bool                enable_timestamp;
    bool                enable_colors;
    bool                enable_module_name;
    /** 环形内存日志区字节数，用于推导实际使用的条目数（不超过静态缓冲上限） */
    uint32_t memory_buffer_size;
} sys_log_config_t;

/* =============================================================================
 * API 函数
 * ============================================================================= */

/**
 * @brief 初始化日志系统
 * @param config 配置结构体
 * @return 成功返回 0，失败返回负错误码
 */
int sys_log_init(const sys_log_config_t* config);

/**
 * @brief 设置模块日志级别
 * @param module 模块名称
 * @param level 日志级别
 *
 * @note module 为 NULL 或空串时设置默认级别，并会同时清空全部按模块覆盖表
 *       （即所有模块恢复跟随默认级别），这是一次性重置语义而非仅改默认值。
 */
void sys_log_set_level(const char* module, sys_log_level_t level);

/**
 * @brief 获取当前日志级别
 * @param module 模块名称
 * @return 当前日志级别
 */
sys_log_level_t sys_log_get_level(const char* module);

/**
 * @brief 启用/禁用日志目标
 * @param dest 位掩码（一个或多个 SYS_LOG_DEST_* 位，或 SYS_LOG_DEST_ALL）
 * @param enable true 启用，false 禁用
 */
void sys_log_set_destination(sys_log_dest_mask_t dest, bool enable);

/**
 * @brief 记录日志消息
 * @param level 日志级别
 * @param module 模块名称
 * @param format printf 风格格式字符串
 * @param ... 格式参数
 *
 * @note 时间戳由内部自动附加（k_uptime），无需调用方提供。
 */
void sys_log_print(sys_log_level_t level, const char* module, const char* format, ...);

/**
 * @brief 记录二进制数据
 * @param level 日志级别
 * @param module 模块名称
 * @param data 数据指针
 * @param len 数据长度
 * @param ascii 显示 ASCII 表示
 */
void sys_log_hexdump(sys_log_level_t level, const char* module, const void* data, size_t len, bool ascii);

/**
 * @brief 从内存缓冲区获取日志条目
 * @param entries 输出条目缓冲区
 * @param count 要获取的条目数
 * @param oldest_first true 表示先获取最旧的条目
 * @return 获取的条目数
 */
uint32_t sys_log_get_entries(sys_log_entry_t* entries, uint32_t count, bool oldest_first);

/**
 * @brief 清空日志内存缓冲区
 */
void sys_log_clear_buffer(void);

/**
 * @brief 获取当前环形缓冲中的日志条数（非累计写入次数）
 *
 * @return 当前环内有效条目数（与 sys_log_clear_buffer 后归零的 count 一致）
 */
uint32_t sys_log_get_count(void);

/**
 * @brief 将日志转储到控制台（需要启用 CONSOLE 和/或 UART 目标）
 * @param level_filter 最高输出级别（与 sys_log_print 一致：数值越小越严重；
 *                     输出 level <= level_filter 的条目；OFF 表示全部输出）
 */
void sys_log_dump(sys_log_level_t level_filter);

/* =============================================================================
 * 便捷宏
 * ============================================================================= */

#define SYS_LOG_E(module, fmt, ...)          sys_log_print(SYS_LOG_LEVEL_ERR, module, fmt, ##__VA_ARGS__)

#define SYS_LOG_W(module, fmt, ...)          sys_log_print(SYS_LOG_LEVEL_WRN, module, fmt, ##__VA_ARGS__)

#define SYS_LOG_I(module, fmt, ...)          sys_log_print(SYS_LOG_LEVEL_INF, module, fmt, ##__VA_ARGS__)

#define SYS_LOG_D(module, fmt, ...)          sys_log_print(SYS_LOG_LEVEL_DBG, module, fmt, ##__VA_ARGS__)

#define SYS_LOG_HEXDUMP_E(module, data, len) sys_log_hexdump(SYS_LOG_LEVEL_ERR, module, data, len, true)

#define SYS_LOG_HEXDUMP_I(module, data, len) sys_log_hexdump(SYS_LOG_LEVEL_INF, module, data, len, true)

#ifdef __cplusplus
}
#endif

#endif /* SYS_LOG_H */
