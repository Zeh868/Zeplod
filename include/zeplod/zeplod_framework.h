/**
 * @file zeplod_framework.h
 * @brief Zeplod 核心框架伞头文件（事件系统、状态机、锁序）
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-12
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-12       1.0            zeh            统一对外头文件入口
 * 2026-09-27       1.1            zeh            伞头纳入延迟/周期发布定时器池 API（event_timer.h）
 */

#ifndef ZEPLOD_FRAMEWORK_H
#define ZEPLOD_FRAMEWORK_H

#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include <zeplod/event_system_compat.h>
#include <zeplod/event_timer.h>
#include <zeplod/lock_order.h>
#include <zeplod/state_machine.h>
#include <zeplod/zepl_thread_service.h>
#include <zeplod/zeplod_api_defs.h>

#endif /* ZEPLOD_FRAMEWORK_H */
