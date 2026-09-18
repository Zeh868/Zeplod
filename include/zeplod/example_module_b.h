/**
 * @file example_module_b.h
 * @brief 示例模块 B 头文件
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.2            zeh            复审整改：删除全工程无读取的死配置结构体（模块无运行参数）
 *
 */

#ifndef EXAMPLE_MODULE_B_H
#define EXAMPLE_MODULE_B_H

#include <zeplod/module_base.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 模块接口
 * ============================================================================= */

int             example_module_b_init(void* config);
int             example_module_b_start(void);
int             example_module_b_stop(void);
int             example_module_b_shutdown(void);
void            example_module_b_on_event(const event_t* event, void* user_data);
module_status_t example_module_b_get_status(void);
int             example_module_b_control(int cmd, void* arg);

/* 获取模块接口 */
const module_interface_t* example_module_b_get_interface(void);

/* =============================================================================
 * 模块专用 API
 * ============================================================================= */

/**
 * @brief 通过通信通道发送数据
 * @param data 要发送的数据
 * @param len 数据长度
 * @return 发送的字节数
 */
int example_module_b_send(const void* data, size_t len);

/**
 * @brief 从通信通道接收数据
 * @param data 输出缓冲区
 * @param len 缓冲区长度
 * @return 接收的字节数
 */
int example_module_b_receive(void* data, size_t len);

/**
 * @brief 获取通信统计信息
 * @param tx_count 输出：发送字节数
 * @param rx_count 输出：接收字节数
 * @param errors 输出：错误计数
 */
void example_module_b_get_stats(uint32_t* tx_count, uint32_t* rx_count, uint32_t* errors);

#ifdef __cplusplus
}
#endif

#endif /* EXAMPLE_MODULE_B_H */
