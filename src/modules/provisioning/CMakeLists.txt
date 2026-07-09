#
# Copyright (c) 2026
# SPDX-License-Identifier: GPL-3.0
#

if(CONFIG_PROVISIONING_MODULE)
    target_sources(app PRIVATE provisioning_module.c)
    if(CONFIG_PROVISIONING_SHELL)
        target_sources(app PRIVATE provisioning_shell.c)
    endif()
    target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
