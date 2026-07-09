/**
 * @file connectivity_module.h
 * @brief 连接管理模块头文件
 *
 * 可选模块（CONFIG_CONNECTIVITY_MODULE）。抽象链路 up/down，发布事件 60。
 * 配置要求：CONFIG_EVENT_MAX_TYPES > 60。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 3 初始版本
 * 2026-07-09       1.1            zeh            优先级注册表 + failover：新增 CELLULAR 链路类型与 connect_auto()
 *
 */

#ifndef CONNECTIVITY_MODULE_H
#define CONNECTIVITY_MODULE_H

#include <zeplod/event_system.h>
#include <zeplod/module_base.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 事件类型
 * ============================================================================= */

/** 连接状态变化（负载 connectivity_status_t） */
#define EVENT_CONNECTIVITY_STATE_CHANGED ((event_type_t) 60)

/* =============================================================================
 * 类型定义
 * ============================================================================= */

typedef enum {
    CONNECTIVITY_LINK_NONE = 0,
    CONNECTIVITY_LINK_WIFI,
    CONNECTIVITY_LINK_ETHERNET,
    CONNECTIVITY_LINK_BLE,
    /** 蜂窝（追加于枚举末尾，避免变更既有取值的二进制/持久化语义） */
    CONNECTIVITY_LINK_CELLULAR,
} connectivity_link_type_t;

typedef enum {
    CONNECTIVITY_STATE_DOWN = 0,
    CONNECTIVITY_STATE_CONNECTING,
    CONNECTIVITY_STATE_UP,
    CONNECTIVITY_STATE_ERROR,
} connectivity_state_t;

typedef struct {
    connectivity_state_t     state;
    connectivity_link_type_t link_type;
    int                      error_code;
} connectivity_status_t;

/* =============================================================================
 * 模块接口（module_interface_t）
 * ============================================================================= */

int             connectivity_module_init(void* config);
int             connectivity_module_start(void);
int             connectivity_module_stop(void);
int             connectivity_module_shutdown(void);
void            connectivity_module_on_event(const event_t* event, void* user_data);
module_status_t connectivity_module_get_status(void);
int             connectivity_module_control(int cmd, void* arg);

/* =============================================================================
 * 模块专用 API
 * ============================================================================= */

/**
 * @brief 发起连接（须已 start；异步完成由后端决定；null 后端同步 UP）
 *
 * 在已注册的后端中查找 link_type 精确匹配者并只连它（“钉住指定链路”语义）；若无精确
 * 匹配，退化到注册表中的通配后端（目前仅 null 桩后端具备该属性），以兼容仅注册 null
 * 后端时调用方传入任意 link_type 标签的既有用法。
 *
 * @return 0 成功；APP_ERR_CONNECTIVITY / APP_ERR_INIT
 */
int connectivity_module_connect(connectivity_link_type_t link_type);

/**
 * @brief 按注册表优先级自动择优连接
 *
 * 从高优先级到低优先级遍历已注册后端，跳过 is_available() 为 false 者，对候选依次尝试
 * connect()，首个成功且 is_link_up() 为真者成为活动后端；全部失败则状态置 ERROR。
 * 需要 CONFIG_CONNECTIVITY_FAILOVER 时通常无需手动调用——failover 管理器会在活动链路
 * 掉线时自动重新调用本函数；也可在应用层主动调用以立即触发一次择优连接。
 *
 * @return 0 成功；APP_ERR_CONNECTIVITY 全部候选失败；APP_ERR_INIT 模块未 RUNNING
 */
int connectivity_module_connect_auto(void);

/**
 * @brief 断开连接
 * @return 0 成功
 */
int connectivity_module_disconnect(void);

/**
 * @brief 查询当前连接状态
 * @param out 输出状态
 * @return 0 成功；-EINVAL
 */
int connectivity_module_get_state(connectivity_status_t* out);

#ifdef __cplusplus
}
#endif

#endif /* CONNECTIVITY_MODULE_H */
