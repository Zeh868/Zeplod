#
# Copyright (c) 2026
# SPDX-License-Identifier: GPL-3.0
#

if(CONFIG_CONNECTIVITY_MODULE)
    target_sources(app PRIVATE connectivity_module.c)
    if(CONFIG_CONNECTIVITY_BACKEND_NULL)
        target_sources(app PRIVATE connectivity_backend_null.c)
    endif()
    if(CONFIG_CONNECTIVITY_BACKEND_WIFI)
        target_sources(app PRIVATE connectivity_backend_wifi.c)
    endif()
    if(CONFIG_CONNECTIVITY_BACKEND_ETHERNET)
        target_sources(app PRIVATE connectivity_backend_ethernet.c)
    endif()
    if(CONFIG_CONNECTIVITY_BACKEND_CELLULAR)
        target_sources(app PRIVATE connectivity_backend_cellular.c)
    endif()
    if(CONFIG_CONNECTIVITY_SHELL)
        target_sources(app PRIVATE connectivity_shell.c)
    endif()
    target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
