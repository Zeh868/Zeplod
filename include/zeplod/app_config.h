/**
 * @file app_config.h
 * @brief 应用层 Kconfig 映射、SYS_INIT 优先级、事件类型与错误码
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

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* Zephyr Kconfig 生成的配置 */
#include <zephyr/autoconf.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 功能开关（映射到 Kconfig CONFIG_*）
 * 注意：这些宏现在从 Kconfig 读取，不再在此硬编码
 *
 * 使用 #define 映射，使 #if APP_CONFIG_ENABLE_* 能正确工作
 * ============================================================================= */

/* 示例模块开关 */
#ifdef CONFIG_EXAMPLE_MODULE_A_ENABLE
#define APP_CONFIG_ENABLE_MODULE_A 1
#else
#define APP_CONFIG_ENABLE_MODULE_A 0
#endif

#ifdef CONFIG_EXAMPLE_MODULE_B_ENABLE
#define APP_CONFIG_ENABLE_MODULE_B 1
#else
#define APP_CONFIG_ENABLE_MODULE_B 0
#endif

/* 系统服务开关 */
#ifdef CONFIG_SYS_LOG_ENABLE
#define APP_CONFIG_ENABLE_LOGGING 1
#else
#define APP_CONFIG_ENABLE_LOGGING 0
#endif

#ifdef CONFIG_SYS_WATCHDOG_ENABLE
#define APP_CONFIG_ENABLE_WATCHDOG 1
#else
#define APP_CONFIG_ENABLE_WATCHDOG 0
#endif

#ifdef CONFIG_SYS_MEMORY_ENABLE
#define APP_CONFIG_ENABLE_MEMORY_MGR 1
#else
#define APP_CONFIG_ENABLE_MEMORY_MGR 0
#endif

#ifdef CONFIG_SYS_TIMER_ENABLE
#define APP_CONFIG_ENABLE_TIMER_SVC 1
#else
#define APP_CONFIG_ENABLE_TIMER_SVC 0
#endif

/* 应用功能开关 */
#ifdef CONFIG_APP_ENABLE_SHELL
#define APP_CONFIG_ENABLE_SHELL 1
#else
#define APP_CONFIG_ENABLE_SHELL 0
#endif

#ifdef CONFIG_APP_ENABLE_STATS
#define APP_CONFIG_ENABLE_STATS 1
#else
#define APP_CONFIG_ENABLE_STATS 0
#endif

#ifdef CONFIG_APP_ENABLE_LOG_DUMP
#define APP_CONFIG_ENABLE_LOG_DUMP 1
#else
#define APP_CONFIG_ENABLE_LOG_DUMP 0
#endif

#ifdef CONFIG_APP_ENABLE_TOP
#define APP_CONFIG_ENABLE_TOP 1
#else
#define APP_CONFIG_ENABLE_TOP 0
#endif

#ifdef CONFIG_APP_ENABLE_BANNER
#define APP_CONFIG_ENABLE_BANNER 1
#else
#define APP_CONFIG_ENABLE_BANNER 0
#endif

#ifdef CONFIG_APP_BOOT_VERBOSE
#define APP_CONFIG_BOOT_VERBOSE 1
#else
#define APP_CONFIG_BOOT_VERBOSE 0
#endif

#ifdef CONFIG_APP_KV_ENABLE
#define APP_CONFIG_ENABLE_APP_KV 1
#else
#define APP_CONFIG_ENABLE_APP_KV 0
#endif

/* =============================================================================
 * KV 存储配置（映射到 Kconfig）
 * ============================================================================= */

#define APP_KV_MAX_ENTRIES                CONFIG_APP_KV_MAX_ENTRIES
#define APP_KV_KEY_MAX_LEN                CONFIG_APP_KV_KEY_MAX_LEN
#define APP_KV_VALUE_MAX_LEN              CONFIG_APP_KV_VALUE_MAX_LEN

/** 单条 KV 在 persist blob 中的编码上界（klen(1) + vlen(2) 字节 + key NUL + value NUL） */
#define APP_KV_PERSIST_ENTRY_MAX          (3U + (unsigned) APP_KV_KEY_MAX_LEN + (unsigned) APP_KV_VALUE_MAX_LEN)

/** Settings 中单条 blob 上限（魔数+头+各槽位序列化，需 ≥ 实际编码长度） */
#define APP_KV_PERSIST_BLOB_MAX           (8U + (unsigned) APP_KV_MAX_ENTRIES * APP_KV_PERSIST_ENTRY_MAX)

/* =============================================================================
 * Zephyr SYS_INIT priorities (POST_KERNEL, same level: lower value runs earlier)
 * 注意：这些是应用层的启动顺序常量，保留在 C 头文件中
 * ============================================================================= */

#define APP_INIT_PRIO_APP_CB              10
#define APP_INIT_PRIO_APP_KV              11
#define APP_INIT_PRIO_SYS_LOG             20
#define APP_INIT_PRIO_SYS_MEM             30
#define APP_INIT_PRIO_SYS_DIAG            31
#define APP_INIT_PRIO_SYS_SECURE_KV       32
#define APP_INIT_PRIO_SYS_FAULT_DUMP      33
#define APP_INIT_PRIO_SYS_TIME            34
#define APP_INIT_PRIO_EVENT_SYS           40
#define APP_INIT_PRIO_DISPATCHER          45
#define APP_INIT_PRIO_DATA_BUS            46
#define APP_INIT_PRIO_DATA_BUS_BRIDGE     47
#define APP_INIT_PRIO_SYS_TIMER           50
#define APP_INIT_PRIO_APP_KV_RESTORE      51 /* flash 驱动(≈50)之后、业务模块(≥54)之前恢复持久化 KV */
#define APP_INIT_PRIO_SYS_WDT             52
#define APP_INIT_PRIO_MODULE_MGR          54
#define APP_INIT_PRIO_MODULE_CONNECTIVITY 55
#define APP_INIT_PRIO_MODULE_PROVISIONING 56
#define APP_INIT_PRIO_MODULE_RECOVERY     57
#define APP_INIT_PRIO_MODULE_OTA          58
#define APP_INIT_PRIO_MODULE_FACTORY      59
#define APP_INIT_PRIO_MODULE_REMOTE_OPS   66
#define APP_INIT_PRIO_MODULE_FEATURE_GATE 67
#define APP_INIT_PRIO_MODULE_A            60
#define APP_INIT_PRIO_MODULE_B            61
#define APP_INIT_PRIO_MODULE_GPIO         62
#define APP_INIT_PRIO_MODULE_UART         63
#define APP_INIT_PRIO_MODULE_IPC          64
#define APP_INIT_PRIO_MODULE_MULTI        65
#define APP_INIT_PRIO_APP_FINAL           99

/* =============================================================================
 * 系统配置（映射到 Kconfig 或 Zephyr 配置）
 * ============================================================================= */

/* 任务优先级 */
#define APP_PRIORITY_EVENT_DISPATCHER     CONFIG_EVENT_DISPATCHER_PRIORITY
#define APP_PRIORITY_MODULE_HIGH          2
#define APP_PRIORITY_MODULE_NORMAL        5
#define APP_PRIORITY_MODULE_LOW           8

/* 栈大小（从 Kconfig 或 Zephyr 配置读取） */
#define APP_STACK_MAIN                    CONFIG_MAIN_STACK_SIZE
#define APP_STACK_EVENT_DISPATCHER        CONFIG_EVENT_DISPATCHER_STACK_SIZE
#define APP_STACK_MODULE_DEFAULT          1024

/* 时序配置 */
#define APP_TICK_RATE_HZ                  CONFIG_SYS_CLOCK_TICKS_PER_SEC
#define APP_WATCHDOG_TIMEOUT_MS           CONFIG_SYS_WATCHDOG_TIMEOUT_MS
#define APP_HEARTBEAT_INTERVAL_MS         CONFIG_APP_HEARTBEAT_INTERVAL_MS

/* 内存和事件配置（从 Kconfig 读取） */
#define APP_HEAP_SIZE                     CONFIG_HEAP_MEM_POOL_SIZE
#define APP_EVENT_QUEUE_SIZE              CONFIG_EVENT_QUEUE_SIZE
#define APP_MAX_MODULES                   CONFIG_MAX_MODULES

/* =============================================================================
 * 事件类型定义
 * ============================================================================= */

/* 保留事件类型（0-9 为系统事件）*/
#define APP_EVENT_TYPE_SYSTEM             0
#define APP_EVENT_TYPE_ERROR              1
#define APP_EVENT_TYPE_CONFIG             2

/* 应用事件类型（10+）*/
#define APP_EVENT_TYPE_USER_START         10

/* =============================================================================
 * Error Codes
 * ============================================================================= */

#define APP_OK                            0
#define APP_ERR_INIT                      -1
#define APP_ERR_MEMORY                    -2
#define APP_ERR_TIMEOUT                   -3
#define APP_ERR_INVALID_PARAM             -4
#define APP_ERR_NOT_FOUND                 -5
#define APP_ERR_BUSY                      -6
#define APP_ERR_DISABLED                  -7
#define APP_ERR_KV_FULL                   -8
#define APP_ERR_IO                        -9
#define APP_ERR_ALREADY_EXISTS            -10
#define APP_ERR_OTA_INVALID_STATE         -20
#define APP_ERR_OTA_TRANSPORT             -21
#define APP_ERR_CONNECTIVITY              -30
#define APP_ERR_PROVISIONING              -31
#define APP_ERR_TIME                      -32
#define APP_ERR_FACTORY                   -40
#define APP_ERR_REMOTE_OPS                -50
#define APP_ERR_FEATURE_GATE              -51

/* =============================================================================
 * 构建配置
 * ============================================================================= */

/* 版本与构建类型见 app_version.h */

/* 目标板名：与 CMake BUILD_TARGET / app_version 一致 */
#ifndef BUILD_TARGET
#define BUILD_TARGET "generic"
#endif
#ifndef APP_TARGET_NAME
#define APP_TARGET_NAME BUILD_TARGET
#endif

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
