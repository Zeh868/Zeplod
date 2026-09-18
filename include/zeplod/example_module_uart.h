/**
 * @file example_module_uart.h
 * @brief UART 通信示例模块
 *
 * 演示如何使用 Zephyr UART API 进行串口通信。
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            删除从未生效的 baudrate 字段（实际波特率
 *                                                 来自 DTS/驱动默认）；删除无发布方的
 *                                                 TX_COMPLETE/ERROR 事件宏；缓冲区字段注明
 *                                                 取值范围；补全模块接口 Doxygen
 *
 */

#ifndef EXAMPLE_MODULE_UART_H
#define EXAMPLE_MODULE_UART_H

#include <stddef.h>
#include <zeplod/module_base.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 模块配置
 * ============================================================================= */

typedef struct {
    const char* device_name;      /* UART 设备名称，如 "UART_0" */
    size_t      rx_buffer_size;   /* 接收环形缓冲大小（字节），取值 1..256，须 <= 模块内部静态缓冲 */
    size_t      tx_buffer_size;   /* 发送缓冲大小（字节），取值 1..256，须 <= 模块内部静态缓冲 */
    bool        enable_interrupt; /* 是否启用中断接收 */
} example_module_uart_config_t;

/* =============================================================================
 * 模块接口
 * ============================================================================= */

/**
 * @brief 初始化 UART 模块（由模块管理器调用）
 *
 * @param config 指向 example_module_uart_config_t 的指针；NULL 时使用默认配置。
 *               非 NULL 时 rx_buffer_size/tx_buffer_size 须在 1..256，
 *               越界返回 -EINVAL
 * @return 0 成功；-EINVAL 配置非法；-EIO 事件类型注册失败
 */
int example_module_uart_init(void* config);

/**
 * @brief 启动 UART 模块：绑定设备、使能中断接收并创建接收线程
 * @return 0 成功；-1 状态非法、设备缺失或未就绪
 */
int example_module_uart_start(void);

/**
 * @brief 停止 UART 模块：关闭中断接收并有界汇合接收线程
 * @return 0 成功（含未运行时直接返回）
 */
int example_module_uart_stop(void);

/**
 * @brief 关停模块：先 stop，再将状态置为 UNINITIALIZED
 * @return 恒为 0
 */
int example_module_uart_shutdown(void);

/**
 * @brief 模块事件回调（模块管理器框架调用）
 * @param event 事件指针；NULL 时直接返回
 * @param user_data 订阅时的用户数据（本模块未使用，可为 NULL）
 */
void example_module_uart_on_event(const event_t* event, void* user_data);

/**
 * @brief 获取模块当前生命周期状态
 * @return module_status_t 当前状态
 */
module_status_t example_module_uart_get_status(void);

/**
 * @brief 模块控制命令入口
 * @param cmd UART_CMD_* 之一（SEND/GET_RX_COUNT/CLEAR_RX/GET_STATS）
 * @param arg cmd 对应的参数指针：SEND 传 example_module_uart_tx_req_t*，
 *            GET_RX_COUNT 传 size_t*，GET_STATS 传 uint32_t[3]；
 *            CLEAR_RX 忽略；需传参的命令为 NULL 时返回 -1
 * @return 0 成功；-1 参数非法或命令不支持
 */
int example_module_uart_control(int cmd, void* arg);

/**
 * @brief 获取模块接口（供模块管理器注册）
 * @return 指向模块接口的只读指针
 */
const module_interface_t* example_module_uart_get_interface(void);

/* =============================================================================
 * 模块特定 API
 * ============================================================================= */

/**
 * @brief 发送数据
 * @param data 数据指针
 * @param len 数据长度
 * @return 实际发送字节数
 */
int example_module_uart_send(const void* data, size_t len);

/**
 * @brief 发送字符串
 * @param str 字符串指针
 * @return 实际发送字节数
 */
int example_module_uart_send_string(const char* str);

/**
 * @brief 接收数据
 * @param data 输出缓冲区
 * @param len 缓冲区长度
 * @return 实际接收字节数
 */
int example_module_uart_receive(void* data, size_t len);

/**
 * @brief 获取接收到的数据长度
 * @return 接收缓冲区中的数据字节数
 */
size_t example_module_uart_get_rx_count(void);

/**
 * @brief 清空接收缓冲区
 */
void example_module_uart_clear_rx_buffer(void);

/**
 * @brief 获取统计信息
 * @param tx_count 输出：发送字节数
 * @param rx_count 输出：接收字节数
 * @param errors 输出：错误计数
 */
void example_module_uart_get_stats(uint32_t* tx_count, uint32_t* rx_count, uint32_t* errors);

/* =============================================================================
 * 事件类型定义
 * ============================================================================= */

#define EVENT_TYPE_UART_RX_DATA 40 /* 接收到数据（本模块 rx_thread 发布） */

/* 原 EVENT_TYPE_UART_TX_COMPLETE(41)/EVENT_TYPE_UART_ERROR(42) 已删除：
 * 全工程无发布方，属死定义（2026-09-18 审查整改）。 */

/* =============================================================================
 * 控制命令
 * ============================================================================= */

#define UART_CMD_SEND               1
#define UART_CMD_GET_RX_COUNT       2
#define UART_CMD_CLEAR_RX           3
#define UART_CMD_GET_STATS          4

/** UART_CMD_SEND 时传入的 arg 指向本结构 */
typedef struct {
    const void* data;
    size_t      len;
} example_module_uart_tx_req_t;

#ifdef __cplusplus
}
#endif

#endif /* EXAMPLE_MODULE_UART_H */
