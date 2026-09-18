/**
 * @file test_event_stubs.h
 * @brief 事件系统测试用可调用 stub 回调（替代伪地址 0x1000）
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-05-28
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-05-28       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            补全文件头修改日志段
 */

#ifndef TEST_EVENT_STUBS_H
#define TEST_EVENT_STUBS_H

#include <zeplod/event_system.h>

/**
 * @brief 空操作订阅回调，仅用于占位订阅槽位
 */
static inline void test_event_noop_callback(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
}
#endif /* TEST_EVENT_STUBS_H */
