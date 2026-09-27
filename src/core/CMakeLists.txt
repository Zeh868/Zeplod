#
# Copyright (c) 2026
#
# SPDX-License-Identifier: GPL-3.0
#

target_sources(app PRIVATE
    event_system.c
    event_system_lifecycle.c
    event_system_memory.c
    event_system_pubsub.c
    event_system_publish.c
    event_system_stats.c
    event_queue.c
    event_queue.h
    event_dispatcher.c
    event_dispatcher_autoinit.c
    event_system_compat.c
    event_memory.c
    event_memory.h
    lock_order.c
    state_machine.c
)

# Sticky 事件锁存（CONFIG_EVENT_STICKY=n 时零链接残留）
if(CONFIG_EVENT_STICKY)
    target_sources(app PRIVATE event_sticky.c)
endif()

# 延迟/周期发布定时器池（CONFIG_EVENT_TIMER_POOL=n 时零链接残留）
if(CONFIG_EVENT_TIMER_POOL)
    target_sources(app PRIVATE event_timer.c)
endif()

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
