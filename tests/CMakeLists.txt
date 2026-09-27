# 测试 CMakeLists.txt
# 用于构建和运行单元测试（与主应用共享 src/ 下的实现）

cmake_minimum_required(VERSION 3.20.0)

# nucleo_l4r5zi：显式合并 640KiB SRAM（避免 DTC_OVERLAY_FILE 空缓存导致 overlay 永不生效）
if(BOARD STREQUAL "nucleo_l4r5zi")
	set(_zeplod_test_dts_overlay "${CMAKE_CURRENT_LIST_DIR}/boards/nucleo_l4r5zi.overlay")
	if(EXISTS "${_zeplod_test_dts_overlay}")
		list(APPEND DTC_OVERLAY_FILE "${_zeplod_test_dts_overlay}")
	endif()
	unset(_zeplod_test_dts_overlay)
endif()

# 与主工程一致：支持从仓库根目录的 zephyr_config.env 加载 ZEPHYR_BASE
if(NOT DEFINED ENV{ZEPHYR_BASE})
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../zephyr_config.env")
		message(STATUS "Loading Zephyr configuration from ../zephyr_config.env...")
		file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../zephyr_config.env" CONFIG_LINES)
		foreach(LINE ${CONFIG_LINES})
			if(NOT LINE MATCHES "^#" AND NOT LINE MATCHES "^$")
				string(REGEX MATCH "^([^=]+)=(.*)$" MATCH ${LINE})
				if(MATCH)
					set(ENV{${CMAKE_MATCH_1}} "${CMAKE_MATCH_2}")
				endif()
			endif()
		endforeach()
	endif()
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(zeplod_tests)

enable_testing()

set(APP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# =============================================================================
# 核心源码（始终包含）
# =============================================================================
add_subdirectory(${APP_SRC_DIR}/core ${CMAKE_CURRENT_BINARY_DIR}/core)
add_subdirectory(${APP_SRC_DIR}/services ${CMAKE_CURRENT_BINARY_DIR}/services)
add_subdirectory(${APP_SRC_DIR}/modules ${CMAKE_CURRENT_BINARY_DIR}/modules)

# =============================================================================
# 核心测试（始终包含）
# =============================================================================
target_sources(app PRIVATE
	test_event_system.c
	test_event_system_compat.c
	test_event_queue.c
	test_event_dispatcher.c
	test_event_memory.c
	test_core_coverage.c
	test_core_coverage_extended.c
	test_module_manager.c
	test_sys_log.c
	test_lock_order.c
	test_state_machine.c
)

# 系统内存管理器（可选）
if(CONFIG_SYS_MEMORY_ENABLE)
	target_sources(app PRIVATE test_sys_memory.c)
endif()

# =============================================================================
# 可选服务测试
# =============================================================================

# 看门狗测试
if(CONFIG_SYS_WATCHDOG_ENABLE)
	target_sources(app PRIVATE test_sys_watchdog.c)
endif()

# 定时器测试
if(CONFIG_SYS_TIMER_ENABLE)
	target_sources(app PRIVATE test_sys_timer.c)
endif()

# =============================================================================
# 示例模块测试（按配置条件包含）
# =============================================================================

add_subdirectory(${APP_SRC_DIR}/modules_examples ${CMAKE_CURRENT_BINARY_DIR}/modules_examples)

if(CONFIG_EXAMPLE_MODULE_A_ENABLE)
	target_sources(app PRIVATE test_example_module_a.c)
endif()

if(CONFIG_EXAMPLE_MODULE_B_ENABLE)
	target_sources(app PRIVATE test_example_module_b.c)
endif()

if(CONFIG_EXAMPLE_MODULE_GPIO)
	target_sources(app PRIVATE test_example_module_gpio.c)
endif()

if(CONFIG_EXAMPLE_MODULE_UART)
	target_sources(app PRIVATE test_example_module_uart.c)
endif()

if(CONFIG_EXAMPLE_MODULE_MULTI_DEP)
	target_sources(app PRIVATE test_example_module_multi_dep.c)
endif()

# =============================================================================
# IPC 服务测试
# =============================================================================
if(CONFIG_THREAD_IPC_SERVICE)
	add_subdirectory(${APP_SRC_DIR}/modules/ipc_service ${CMAKE_CURRENT_BINARY_DIR}/ipc_service)
	target_sources(app PRIVATE test_ipc_service.c)
endif()

# =============================================================================
# 并发压力测试（静态线程栈，默认关闭以适配硬件 RAM）
# =============================================================================
if(CONFIG_ZTEST_CONCURRENCY_STRESS)
	target_sources(app PRIVATE test_concurrency_stress.c)
endif()

# =============================================================================
# 订阅级过滤测试（需 CONFIG_EVENT_SUBSCRIBER_FILTER）
# =============================================================================
if(CONFIG_EVENT_SUBSCRIBER_FILTER)
	target_sources(app PRIVATE test_event_filter.c)
endif()

# =============================================================================
# Sticky 事件测试（需 CONFIG_EVENT_STICKY）
# =============================================================================
if(CONFIG_EVENT_STICKY)
	target_sources(app PRIVATE test_event_sticky.c)
endif()

# =============================================================================
# 延迟/周期发布定时器池测试（需 CONFIG_EVENT_TIMER_POOL）
# =============================================================================
if(CONFIG_EVENT_TIMER_POOL)
	target_sources(app PRIVATE test_event_timer.c)
endif()

# =============================================================================
# 优先级有序分发测试（需 CONFIG_EVENT_PRIORITY_DISPATCH）
# =============================================================================
if(CONFIG_EVENT_PRIORITY_DISPATCH)
	target_sources(app PRIVATE test_event_priority.c)
endif()

# =============================================================================
# Data Bus 测试
# =============================================================================
if(CONFIG_DATA_BUS)
	add_subdirectory(${APP_SRC_DIR}/data_bus ${CMAKE_CURRENT_BINARY_DIR}/data_bus)
	target_sources(app PRIVATE test_data_bus.c)
endif()

# =============================================================================
# OTA module tests
# =============================================================================
if(CONFIG_OTA_MODULE)
	add_subdirectory(${APP_SRC_DIR}/modules/ota ${CMAKE_CURRENT_BINARY_DIR}/ota)
	target_sources(app PRIVATE test_ota_module.c)
endif()

# =============================================================================
# Recovery policy tests
# =============================================================================
if(CONFIG_RECOVERY_POLICY_MODULE)
	add_subdirectory(${APP_SRC_DIR}/modules/recovery ${CMAKE_CURRENT_BINARY_DIR}/recovery)
	target_sources(app PRIVATE test_recovery_policy.c)
endif()

# =============================================================================
# System diagnostics tests
# =============================================================================
if(CONFIG_SYS_DIAG_ENABLE)
	target_sources(app PRIVATE test_sys_diag.c)
endif()

# =============================================================================
# Secure KV tests
# =============================================================================
if(CONFIG_SYS_SECURE_KV_ENABLE)
	target_sources(app PRIVATE test_sys_secure_kv.c)
endif()

# =============================================================================
# sys_time tests
# =============================================================================
if(CONFIG_SYS_TIME_ENABLE)
	target_sources(app PRIVATE test_sys_time.c)
endif()

# =============================================================================
# Connectivity module tests
# =============================================================================
if(CONFIG_CONNECTIVITY_MODULE)
	add_subdirectory(${APP_SRC_DIR}/modules/connectivity ${CMAKE_CURRENT_BINARY_DIR}/connectivity)
	target_sources(app PRIVATE test_connectivity_module.c)
endif()

# =============================================================================
# Provisioning module tests
# =============================================================================
if(CONFIG_PROVISIONING_MODULE)
	add_subdirectory(${APP_SRC_DIR}/modules/provisioning ${CMAKE_CURRENT_BINARY_DIR}/provisioning)
	target_sources(app PRIVATE test_provisioning_module.c)
endif()

# =============================================================================
# Factory mode tests
# =============================================================================
if(CONFIG_FACTORY_MODE_MODULE)
	add_subdirectory(${APP_SRC_DIR}/modules/factory ${CMAKE_CURRENT_BINARY_DIR}/factory)
	if(CONFIG_APP_KV_ENABLE)
		target_sources(app PRIVATE test_factory_mode.c)
	else()
		target_sources(app PRIVATE test_factory_mode_no_kv.c)
	endif()
endif()

# =============================================================================
# Remote ops tests
# =============================================================================
if(CONFIG_REMOTE_OPS_MODULE)
	add_subdirectory(${APP_SRC_DIR}/modules/remote_ops ${CMAKE_CURRENT_BINARY_DIR}/remote_ops)
	target_sources(app PRIVATE test_remote_ops.c)
endif()

# =============================================================================
# Feature gate tests
# =============================================================================
if(CONFIG_FEATURE_GATE_MODULE)
	add_subdirectory(${APP_SRC_DIR}/modules/feature_gate ${CMAKE_CURRENT_BINARY_DIR}/feature_gate)
	target_sources(app PRIVATE test_feature_gate.c)
endif()

# =============================================================================
# Fault dump tests
# =============================================================================
if(CONFIG_SYS_FAULT_DUMP_ENABLE)
	target_sources(app PRIVATE test_sys_fault_dump.c)
endif()

# =============================================================================
# Application KV tests
# =============================================================================
if(CONFIG_APP_KV_ENABLE)
	target_sources(app PRIVATE
		${APP_SRC_DIR}/app/app_kv.c
		test_app_kv_migrate.c
	)
endif()

# =============================================================================
# 包含路径
# =============================================================================
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../include
	${APP_SRC_DIR}/core
	${APP_SRC_DIR}/modules
	${APP_SRC_DIR}/modules/ipc_service
	${APP_SRC_DIR}/modules/ota
	${APP_SRC_DIR}/modules/recovery
	${APP_SRC_DIR}/modules/connectivity
	${APP_SRC_DIR}/modules/provisioning
	${APP_SRC_DIR}/modules/factory
	${APP_SRC_DIR}/modules/remote_ops
	${APP_SRC_DIR}/modules/feature_gate
	${APP_SRC_DIR}/data_bus
)

target_compile_definitions(app PRIVATE
	UNIT_TEST=1
)
