/**
 * @file factory_mode.h
 * @brief 工厂产测模式模块头文件
 *
 * 可选模块（CONFIG_FACTORY_MODE_MODULE）。产线进入/退出、GPIO 环回（stub）、
 * 校准数据写入与提交，发布事件 70。
 * 配置要求：CONFIG_EVENT_MAX_TYPES > 70。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 4 初始版本
 * 2026-09-18       1.1            zeh            校准项 key/value 超长错误码统一为
 *                                                 APP_ERR_INVALID_PARAM（实现对齐）；
 *                                                 模块接口 7 件套补全 Doxygen 注释
 *
 */

#ifndef FACTORY_MODE_H
#define FACTORY_MODE_H

#include <zeplod/event_system.h>
#include <zeplod/module_base.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 事件类型
 * ============================================================================= */

/** 工厂模式状态变化（负载 factory_status_t） */
#define EVENT_FACTORY_STATE_CHANGED ((event_type_t) 70)

/* =============================================================================
 * 类型定义
 * ============================================================================= */

typedef enum {
    FACTORY_STATE_INACTIVE = 0,
    FACTORY_STATE_ACTIVE,
    FACTORY_STATE_PASSED,
    FACTORY_STATE_FAILED,
} factory_state_t;

typedef struct {
    factory_state_t state;
    int             error_code;
    bool            gpio_passed;
} factory_status_t;

/** factory_mode_control 命令：重置内部状态（测试用） */
#define FACTORY_MODE_CMD_RESET 1

/* =============================================================================
 * 模块接口（module_interface_t，由 module_manager 生命周期驱动）
 * ============================================================================= */

/**
 * @brief 初始化工厂模式模块
 * @param config 模块配置指针；本模块未使用，允许为 NULL
 * @return 0 成功；重复初始化幂等返回 0
 */
int factory_mode_init(void* config);

/**
 * @brief 启动工厂模式模块
 * @return 0 成功；APP_ERR_INIT 未初始化；重复启动幂等返回 0
 */
int factory_mode_start(void);

/**
 * @brief 停止工厂模式模块
 * @return 0 成功；未运行时亦返回 0（幂等）
 */
int factory_mode_stop(void);

/**
 * @brief 关闭工厂模式模块（先退出产测态，状态回落 UNINITIALIZED）
 * @return 0 成功
 */
int factory_mode_shutdown(void);

/**
 * @brief 事件回调占位（本模块仅发布状态事件，不消费外部事件）
 * @param event 事件指针（非 NULL，由事件分发器保证）
 * @param user_data 订阅时传入的用户数据（本模块未使用）
 */
void factory_mode_on_event(const event_t* event, void* user_data);

/**
 * @brief 查询模块生命周期状态
 * @return MODULE_STATUS_*（UNINITIALIZED/INITIALIZED/RUNNING/STOPPED）
 */
module_status_t factory_mode_get_status(void);

/**
 * @brief 模块控制命令入口
 * @param cmd 命令字；仅支持 FACTORY_MODE_CMD_RESET
 * @param arg 命令参数；RESET 未使用，须为 NULL
 * @return 0 成功；-ENOTSUP 不支持的命令
 */
int factory_mode_control(int cmd, void* arg);

/* =============================================================================
 * 模块专用 API
 * ============================================================================= */

/**
 * @brief 进入产测模式（仅 INACTIVE → ACTIVE）
 * @return 0 成功；APP_ERR_FACTORY 非 INACTIVE 状态；APP_ERR_INIT 模块未初始化
 */
int factory_mode_enter(void);

/**
 * @brief 退出产测模式（→ INACTIVE，清除临时校准表）
 * @return 0 成功（已处于 INACTIVE 时幂等返回 0）
 */
int factory_mode_exit(void);

/**
 * @brief 查询工厂模式状态
 *
 * @param out 输出状态指针，须非 NULL；`error_code` 在 FAILED 时为最近一次失败码，否则为 0
 * @return 0 成功；APP_ERR_INVALID_PARAM out 为 NULL
 */
int factory_mode_get_state(factory_status_t* out);

/**
 * @brief 运行 GPIO 环回自检（Phase 4 stub 同步成功）
 * @return 0 成功；APP_ERR_FACTORY 未处于 ACTIVE；-ENOTSUP 未使能 GPIO 桩
 */
int factory_mode_run_gpio_loopback(void);

/**
 * @brief 写入临时校准项（仅 ACTIVE，存于 RAM 表，finalize 时才落盘）
 *
 * @param key 校准项键名，非 NULL；长度须在 [1, CONFIG_FACTORY_MODE_CAL_KEY_MAX_LEN)
 * @param value 校准项值，非 NULL；长度须在 [0, CONFIG_FACTORY_MODE_CAL_VALUE_MAX_LEN)
 * @return 0 成功；APP_ERR_INVALID_PARAM key/value 为 NULL、空/超长；
 *         APP_ERR_FACTORY 未处于 ACTIVE；-ENOMEM 校准表已满
 */
int factory_mode_set_calibration(const char* key, const char* value);

/**
 * @brief 读取临时校准项
 *
 * @param key 校准项键名，须非 NULL
 * @param out 输出缓冲区，须非 NULL
 * @param out_len 缓冲区长度（须 > 0，含结尾 NUL）
 * @return 0 成功；APP_ERR_INVALID_PARAM 参数非法；APP_ERR_NOT_FOUND 键不存在；
 *         -ENOMEM 缓冲不足
 */
int factory_mode_get_calibration(const char* key, char* out, size_t out_len);

/**
 * @brief 提交校准到 app_kv（键前缀 factory.cal.）并标记 PASSED
 *
 * 须已完成 GPIO 环回且处于 ACTIVE。
 * @return 0 成功；APP_ERR_FACTORY 状态不满足；APP_ERR_DISABLED 无 app_kv；
 *         其余为 app_kv_set 失败的错误码
 */
int factory_mode_finalize_pass(void);

#ifdef __cplusplus
}
#endif

#endif /* FACTORY_MODE_H */
