# Zeplod — Zephyr event-driven application
# Copyright (c) 2026
# SPDX-License-Identifier: GPL-3.0
#
# This is a freestanding Zephyr application (application and Zephyr are in separate directories)
# See: https://docs.zephyrproject.org/latest/application/index.html#zephyr-freestanding-app

cmake_minimum_required(VERSION 3.20.0)

# =============================================================================
# Load Zephyr Configuration
# =============================================================================

# Check for ZEPHYR_BASE environment variable
# For freestanding applications, ZEPHYR_BASE must point to the Zephyr repository
if(NOT DEFINED ENV{ZEPHYR_BASE})
    # Try to load from zephyr_config.env file
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/zephyr_config.env")
        message(STATUS "Loading Zephyr configuration from zephyr_config.env...")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/zephyr_config.env" CONFIG_LINES)
        foreach(LINE ${CONFIG_LINES})
            # Skip comments and empty lines
            if(NOT LINE MATCHES "^#" AND NOT LINE MATCHES "^$")
                string(REGEX MATCH "^([^=]+)=(.*)$" MATCH ${LINE})
                if(MATCH)
                    set(ENV{${CMAKE_MATCH_1}} "${CMAKE_MATCH_2}")
                endif()
            endif()
        endforeach()
    else()
        message(FATAL_ERROR
            "ZEPHYR_BASE not set and zephyr_config.env not found.\n"
            "Please either:\n"
            "  1. Set ZEPHYR_BASE environment variable, or\n"
            "  2. Copy zephyr_config.env.template to zephyr_config.env and edit paths,\n"
            "  3. Run scripts/setup_env.bat (Windows) or source scripts/setup_env.sh (Linux/macOS)"
        )
    endif()
endif()

message(STATUS "ZEPHYR_BASE: $ENV{ZEPHYR_BASE}")
message(STATUS "ZEPHYR_SDK_INSTALL_DIR: $ENV{ZEPHYR_SDK_INSTALL_DIR}")

# =============================================================================
# Application version (before find_package(Zephyr))
#
# Do NOT use a file named VERSION at repo root: Zephyr's find_package loads
# cmake/modules/version.cmake which reads VERSION and expects kernel-style fields
# (VERSION_MAJOR etc.); a semver line like 1.0.0 breaks configuration.
# =============================================================================

# 版本文件：优先顶层应用仓库的 APP_VERSION（add_subdirectory 时 CMAKE_SOURCE_DIR 为应用根）；
# 否则使用本目录（framework）下的 APP_VERSION（独立构建模板时二者相同）。
if(EXISTS "${CMAKE_SOURCE_DIR}/APP_VERSION")
	file(READ "${CMAKE_SOURCE_DIR}/APP_VERSION" PROJECT_VERSION_RAW)
	string(STRIP "${PROJECT_VERSION_RAW}" PROJECT_VERSION_RAW)
	if(PROJECT_VERSION_RAW MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
		set(PROJECT_VERSION_MAJOR ${CMAKE_MATCH_1})
		set(PROJECT_VERSION_MINOR ${CMAKE_MATCH_2})
		set(PROJECT_VERSION_PATCH ${CMAKE_MATCH_3})
		set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")
		message(STATUS "Project version from \${CMAKE_SOURCE_DIR}/APP_VERSION: ${PROJECT_VERSION}")
	else()
		message(FATAL_ERROR "APP_VERSION must contain a single line X.Y.Z, got: '${PROJECT_VERSION_RAW}'")
	endif()
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/APP_VERSION")
	file(READ "${CMAKE_CURRENT_SOURCE_DIR}/APP_VERSION" PROJECT_VERSION_RAW)
	string(STRIP "${PROJECT_VERSION_RAW}" PROJECT_VERSION_RAW)
	if(PROJECT_VERSION_RAW MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
		set(PROJECT_VERSION_MAJOR ${CMAKE_MATCH_1})
		set(PROJECT_VERSION_MINOR ${CMAKE_MATCH_2})
		set(PROJECT_VERSION_PATCH ${CMAKE_MATCH_3})
		set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")
		message(STATUS "Project version from framework/APP_VERSION: ${PROJECT_VERSION}")
	else()
		message(FATAL_ERROR "APP_VERSION must contain a single line X.Y.Z, got: '${PROJECT_VERSION_RAW}'")
	endif()
else()
	set(PROJECT_VERSION_MAJOR 1)
	set(PROJECT_VERSION_MINOR 0)
	set(PROJECT_VERSION_PATCH 0)
	set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")
	message(STATUS "Project version default (no APP_VERSION): ${PROJECT_VERSION}")
endif()

message(STATUS "Project Version: ${PROJECT_VERSION}")

# 保存独立的应用版本变量。
# 下方的 find_package(Zephyr) 与 project() 会用 Zephyr 内核版本（如 4.x.x）重写
# PROJECT_VERSION_* / PROJECT_VERSION，因此其后所有编译宏与生成头必须改用 APP_VERSION_*，
# 切勿再复用 PROJECT_VERSION_*，否则固件会错误地报告 Zephyr 版本。
set(APP_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(APP_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(APP_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(APP_VERSION_STRING "${PROJECT_VERSION}")

# Note: BOARD_ROOT is not needed for standard boards like nucleo_l4r5zi
# Only use BOARD_ROOT if you have custom boards in the boards/ directory
# list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/boards)

# =============================================================================
# 默认 Kconfig 合并链（可被 -DCONF_FILE / -DEXTRA_CONF_FILE 覆盖或追加）
# =============================================================================
#
# prj.conf          公共基线
# standard.conf     标准内存档位
# data_bus.conf     Data Bus 功能
# thread_ipc.conf   Thread IPC 功能
#
# 档位切换：west build ... -- -DEXTRA_CONF_FILE=conf/profiles/balanced.conf
# =============================================================================

if(NOT DEFINED CONF_FILE)
    set(CONF_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/prj.conf"
        "${CMAKE_CURRENT_SOURCE_DIR}/conf/profiles/standard.conf"
        "${CMAKE_CURRENT_SOURCE_DIR}/conf/features/data_bus.conf"
        "${CMAKE_CURRENT_SOURCE_DIR}/conf/features/thread_ipc.conf"
    )
endif()

# Find Zephyr package
# For freestanding applications, explicitly specify ZEPHYR_BASE
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

# shell RTT 通道 1 缓冲移入 DTCM（cmake/zeplod_shell_rtt_dtcm.ld 定义的
# .shell_rtt_dtcm 段）：DTCM 不经 M7 D-Cache，恢复 DCACHE 默认开启后调试器
# 仍能实时读到 shell 输出；缓冲原本随 .bss 落 SDRAM，写回缓存会让 RTT Viewer
# 读到旧数据。SECTIONS 片段注入点先于通用 .bss 规则生效；条件沿用 SoC
# defconfig 的 SEGGER_RTT_SECTION_DTCM（仅存在 zephyr,dtcm 的板子为真）。
zephyr_linker_sources_ifdef(CONFIG_SEGGER_RTT_SECTION_DTCM SECTIONS cmake/zeplod_shell_rtt_dtcm.ld)

project(zeplod VERSION ${APP_VERSION_STRING})

# =============================================================================
# Git Information (Auto-detect)
# =============================================================================

# Find Git
find_package(Git)

if(GIT_FOUND)
    # Get git commit hash
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE GIT_RESULT
        ERROR_QUIET
    )

    if(GIT_RESULT EQUAL 0)
        message(STATUS "Git Commit: ${GIT_COMMIT_HASH}")
    else()
        set(GIT_COMMIT_HASH "unknown")
    endif()

    # Get git branch
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE GIT_RESULT
        ERROR_QUIET
    )

    if(NOT GIT_RESULT EQUAL 0)
        set(GIT_BRANCH "unknown")
    endif()

    # Get git tag (ERROR_QUIET suppresses stderr; avoid shell redirects for portability)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE GIT_RESULT
        ERROR_QUIET
    )

    if(NOT GIT_RESULT EQUAL 0)
        set(GIT_TAG "untagged")
    endif()

    # Check for uncommitted changes
    execute_process(
        COMMAND ${GIT_EXECUTABLE} diff-index --quiet HEAD --
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        RESULT_VARIABLE GIT_DIRTY
        ERROR_QUIET
    )

    if(NOT GIT_DIRTY EQUAL 0)
        set(GIT_DIRTY 1)
        message(STATUS "Git Status: dirty (uncommitted changes)")
    else()
        set(GIT_DIRTY 0)
        message(STATUS "Git Status: clean")
    endif()
else()
    set(GIT_COMMIT_HASH "unknown")
    set(GIT_BRANCH "unknown")
    set(GIT_TAG "unknown")
    set(GIT_DIRTY 0)
    message(STATUS "Git not found, version info will be limited")
endif()

# =============================================================================
# Build Timestamp
# =============================================================================

string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S")
string(TIMESTAMP BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP BUILD_TIME "%H:%M:%S")

message(STATUS "Build Timestamp: ${BUILD_TIMESTAMP}")

# =============================================================================
# Target Board
# =============================================================================

if(BOARD)
    set(BUILD_TARGET ${BOARD})
else()
    set(BUILD_TARGET "unknown")
endif()

message(STATUS "Build Target: ${BUILD_TARGET}")

# =============================================================================
# Project Source Files
# =============================================================================

# Core Event System
add_subdirectory(src/core)

# System Services
add_subdirectory(src/services)

# Application Layer
add_subdirectory(src/app)

# Business Modules (Example - Extendable)
add_subdirectory(src/modules)

# OTA module (Kconfig: CONFIG_OTA_MODULE)
add_subdirectory(src/modules/ota)

# Recovery policy module (Kconfig: CONFIG_RECOVERY_POLICY_MODULE)
add_subdirectory(src/modules/recovery)

# Connectivity module (Kconfig: CONFIG_CONNECTIVITY_MODULE)
add_subdirectory(src/modules/connectivity)

# Provisioning module (Kconfig: CONFIG_PROVISIONING_MODULE)
add_subdirectory(src/modules/provisioning)

# Factory mode module (Kconfig: CONFIG_FACTORY_MODE_MODULE)
add_subdirectory(src/modules/factory)

# Remote ops module (Kconfig: CONFIG_REMOTE_OPS_MODULE)
add_subdirectory(src/modules/remote_ops)

# Feature gate module (Kconfig: CONFIG_FEATURE_GATE_MODULE)
add_subdirectory(src/modules/feature_gate)

# Event benchmark module (Kconfig: CONFIG_EVENT_BENCHMARK)
add_subdirectory(src/modules/event_benchmark)

# Thread IPC service (Kconfig: CONFIG_THREAD_IPC_SERVICE; avoids Zephyr IPC_SERVICE name clash)
if(CONFIG_THREAD_IPC_SERVICE)
    add_subdirectory(src/modules/ipc_service)
endif()

# Data Bus
if(CONFIG_DATA_BUS)
    add_subdirectory(src/data_bus)
endif()

# Example modules (conditionally compiled)
add_subdirectory(src/modules_examples)

# =============================================================================
# Compiler Definitions
# =============================================================================

target_compile_definitions(app PRIVATE
    PROJECT_VERSION_MAJOR=${APP_VERSION_MAJOR}
    PROJECT_VERSION_MINOR=${APP_VERSION_MINOR}
    PROJECT_VERSION_PATCH=${APP_VERSION_PATCH}
    PROJECT_VERSION="${APP_VERSION_STRING}"
    GIT_COMMIT_HASH="${GIT_COMMIT_HASH}"
    GIT_BRANCH="${GIT_BRANCH}"
    GIT_TAG="${GIT_TAG}"
    GIT_DIRTY=${GIT_DIRTY}
    BUILD_TIMESTAMP="${BUILD_TIMESTAMP}"
    BUILD_DATE="${BUILD_DATE}"
    BUILD_TIME="${BUILD_TIME}"
    BUILD_TARGET="${BUILD_TARGET}"
)

# =============================================================================
# Include Directories
# =============================================================================

# 版本信息说明：版本宏（PROJECT_VERSION_* / PROJECT_VERSION / GIT_* / BUILD_*）统一通过
# 上方 target_compile_definitions 的 -D 注入，源头为仓库根 APP_VERSION 文件。
# 不再生成/包含 app_version_config.h（曾为无人 include 的死代码）。
# 对外 API：include/zeplod/*.h；实现细节头文件保留在各 src 子目录
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/modules
    ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/ipc_service
    ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/ota
    ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/recovery
    ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/connectivity
    ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/provisioning
    ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/factory
    ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/remote_ops
    ${CMAKE_CURRENT_SOURCE_DIR}/src/modules/feature_gate
    ${CMAKE_CURRENT_SOURCE_DIR}/src/data_bus
)
