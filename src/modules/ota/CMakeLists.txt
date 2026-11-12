#
# Copyright (c) 2026
#
# SPDX-License-Identifier: GPL-3.0
#

if(CONFIG_OTA_MODULE)
    target_sources(app PRIVATE
        ota_module.c
        ota_state_machine.c
    )
    if(CONFIG_OTA_TRANSPORT_NULL)
        target_sources(app PRIVATE ota_transport_null.c)
    endif()
    if(CONFIG_OTA_TRANSPORT_ACTIVE)
        target_sources(app PRIVATE ota_transport_mcuboot.c)
    endif()
    if(CONFIG_OTA_TRANSPORT_MCUMGR_SMP)
        target_sources(app PRIVATE ota_transport_mcumgr_smp.c)
        # img_mgmt.h 会拉进 bootutil/image.h，后者再包含 mcuboot_config。
        # 这两个目录都在 MCUBOOT_BOOTUTIL 接口库上，应用目标需要自己链接它。
        target_link_libraries(app PRIVATE MCUBOOT_BOOTUTIL)
    endif()
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()
