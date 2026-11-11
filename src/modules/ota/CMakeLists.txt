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
        # img_mgmt.h 直接包含 bootutil/image.h。该路径只进了 MCUmgr 库，
        # 应用目标编译时看不到。
        target_include_directories(app PRIVATE
            ${ZEPHYR_MCUBOOT_MODULE_DIR}/boot/bootutil/include
        )
    endif()
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()
