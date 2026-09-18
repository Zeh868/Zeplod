/**
 * @file example_module_a.h
 * @brief 示例模块 A 头文件
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            删除全工程无读取方的 buffer_size 配置
 *                                                 字段；补全模块接口 Doxygen
 *
 */
#ifndef EXAMPLE_MODULE_A_H
#define EXAMPLE_MODULE_A_H

#include <zeplod/module_base.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 模块配置
 * ============================================================================= */

typedef struct {
    uint32_t sample_rate_ms;   /* 采样周期（毫秒），须 > 0 */
    bool     enable_filtering; /* 是否启用滤波（演示占位，当前仅透传保存） */
} example_module_a_config_t;

/* =============================================================================
 * 模块接口（在 .c 文件中实现）
 * ============================================================================= */

/**
 * @brief 初始化示例模块 A（由模块管理器调用）
 * @param config 指向 example_module_a_config_t 的指针；NULL 时使用默认配置
 *               （sample_rate_ms=100，enable_filtering=true）
 * @return 0 成功；-EIO 事件类型注册失败
 */
int example_module_a_init(void* config);

/**
 * @brief 启动模块：创建数据采集线程并订阅配置事件
 * @return 0 成功；-1 状态非法；-EIO 事件订阅失败
 */
int example_module_a_start(void);

/**
 * @brief 停止模块：置停止标志、有界汇合采集线程并取消订阅
 * @return 0 成功（含未运行时直接返回）
 */
int example_module_a_stop(void);

/**
 * @brief 关停模块：先 stop，再将状态置为 UNINITIALIZED
 * @return 恒为 0
 */
int example_module_a_shutdown(void);

/**
 * @brief 事件处理器：处理 EVENT_TYPE_SENSOR_CONFIG 更新采样率
 * @param event 事件指针；NULL 时直接返回
 * @param user_data 必须指向本模块控制块（start 订阅时传入 &g_module_a）；
 *                  NULL 时直接返回
 */
void example_module_a_on_event(const event_t* event, void* user_data);

/**
 * @brief 获取模块当前生命周期状态
 * @return module_status_t 当前状态
 */
module_status_t example_module_a_get_status(void);

/**
 * @brief 模块控制命令入口
 * @param cmd 内部命令（CMD_SET_RATE/CMD_GET_RATE/CMD_RESET 对应 1/2/3）
 * @param arg SET_RATE/GET_RATE 时须指向 uint32_t（采样率毫秒），NULL 返回 -1；
 *            RESET 忽略
 * @return 0 成功；-1 参数非法或命令不支持
 */
int example_module_a_control(int cmd, void* arg);

/**
 * @brief 获取模块接口（供模块管理器注册）
 * @return 指向模块接口的只读指针
 */
const module_interface_t* example_module_a_get_interface(void);

/* =============================================================================
 * 模块专用 API
 * ============================================================================= */

/**
 * @brief 获取最新传感器数据
 * @param data 输出缓冲区
 * @param len 缓冲区长度
 * @return 读取的字节数
 */
int example_module_a_get_data(void* data, size_t len);

/**
 * @brief 设置采样率
 * @param rate_ms 采样率（毫秒）
 * @return 成功返回 0，失败返回负错误码
 */
int example_module_a_set_rate(uint32_t rate_ms);

#ifdef __cplusplus
}
#endif

#endif /* EXAMPLE_MODULE_A_H */
