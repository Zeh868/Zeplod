> Language: [中文](../../zh-CN/40-应用开发/42-项目配置项说明.md) | **English**

# Project Configuration Options Guide

This document documents the meaning of each configuration option in this repository's **application-side Kconfig** (root directory `Kconfig` and `src/modules/ipc_service/Kconfig`). After build, Zephyr generates `autoconf.h`, used in source code via `CONFIG_<symbol_name>` (e.g., `CONFIG_EVENT_SYSTEM`).

- **How to set**: Write `CONFIG_xxx=y` or `CONFIG_xxx=value` in `prj.conf`, or use `west build -t menuconfig` / `guiconfig`.
- **Symbol prefix**: "Configuration macro" column below is the macro name in C code (consistent with `CONFIG_`).
- **With board/kernel options**: SOC, drivers, kernel heap etc. see Zephyr and board `defconfig`; this table only covers **application extension** menu items.

---

## Table of Contents

1. [Event System Configuration](#1-event-system-configuration)
2. [Module Manager Configuration](#2-module-manager-configuration)
3. [System Services Configuration](#3-system-services-configuration)
4. [Thread IPC Service (in-app)](#4-thread-ipc-service-in-app)
5. [Application KV Store (optional persistent)](#5-application-kv-store-optional-persistent)
6. [Feature Switches in Application Header Files (non-Kconfig)](#6-feature-switches-in-application-header-files-non-kconfig)
7. [Common Zephyr Options (working with this application)](#7-common-zephyr-options-working-with-this-application)

---

## 1. Event System Configuration

| Configuration Macro | Type | Default | Range | Description |
|--------------------|------|---------|-------|-------------|
| `CONFIG_EVENT_SYSTEM` | bool | y | - | Enable event system: publish/subscribe, event queue and dispatch thread. Features depending on event system unavailable when disabled. |
| `CONFIG_EVENT_MAX_TYPES` | int | 128 | 4-256 | Maximum event-type ID space. Kconfig estimates about 228 bytes per type with eight subscribers. Product features need IDs through 72; tests using higher IDs set 256 explicitly. |
| `CONFIG_EVENT_QUEUE_SIZE` | int | 64 | 4-512 | Maximum events in the global queue. Larger values use more RAM; smaller values can drop events. |
| `CONFIG_EVENT_MAX_SUBSCRIBERS` | int | 16 | 1-64 | Maximum subscribers **per event type**. |
| `CONFIG_EVENT_DISPATCHER_STACK_SIZE` | int | 2048 | 256-8192 | Event dispatch thread stack size (bytes). Increase when callbacks are deep or local variables large. Minimum can be 256. |
| `CONFIG_EVENT_DISPATCHER_PRIORITY` | int | 5 | 1-15 | Dispatch thread priority (range 1-15, **smaller value = higher priority**). |

### 1.1 Event Struct and Slab Memory Configuration

| Configuration Macro | Type | Default | Range | Description |
|--------------------|------|---------|-------|-------------|
| `CONFIG_EVENT_STRUCT_SIZE` | int | 64 | 32/64/128 | Event struct size (bytes), determines inline_data array length. |
| `CONFIG_EVENT_INLINE_DATA_SIZE` | int | 48 | 4-128 | Inline data size threshold. Data smaller than this stored in inline_data, zero extra allocation. |
| `CONFIG_EVENT_SLAB_ENABLE` | bool | n | - | Enable Slab memory pool for real-time safe allocation. `_rt` suffixed APIs entirely allocate from Slab, O(1) deterministic time. |
| `CONFIG_EVENT_SLAB_CRITICAL_COUNT` | int | 8 | 0-64 | CRITICAL priority event Slab block count (optional). |
| `CONFIG_EVENT_SLAB_HIGH_COUNT` | int | 8 | 0-64 | HIGH priority event Slab block count (optional). |
| `CONFIG_EVENT_SLAB_NORMAL_COUNT` | int | 16 | ≥4 | NORMAL/LOW priority event Slab block count (must be ≥4). |
| `CONFIG_EVENT_SLAB_LARGE_ENABLE` | bool | n | - | Enable large data block Slab pool (256B/1KB/4KB). |
| `CONFIG_EVENT_SLAB_LARGE_256_COUNT` | int | 4 | 0-64 | 256-byte data block Slab count. |
| `CONFIG_EVENT_SLAB_LARGE_1K_COUNT` | int | 2 | 0-64 | 1KB data block Slab count. |
| `CONFIG_EVENT_SLAB_LARGE_4K_COUNT` | int | 1 | 0-32 | 4KB data block Slab count. |
| `CONFIG_EVENT_RUNTIME_STATUS` | bool | n | - | Enable Slab runtime status query APIs (`event_slab_available`, `event_get_slab_stats`). |
| `CONFIG_EVENT_SLAB_EXHAUSTED_CB` | bool | n | - | Enable Slab exhaustion callback registration. |
| `CONFIG_EVENT_DEBUG_MEM` | bool | n | - | Enable event system memory debugging (leak detection). |

**Data Storage Strategy**:
- `data_len ≤ INLINE_DATA_SIZE`: Inline storage in `event_t.inline_data`, no extra allocation
- `data_len ≤ 256`: Allocated from 256B data Slab pool
- `data_len ≤ 1024`: Allocated from 1KB data Slab pool
- `data_len ≤ 4096`: Allocated from 4KB data Slab pool
- `data_len > 4096`: Fallback to k_malloc (not real-time safe)

**Memory Usage Estimation**:
```
Event system static memory ≈ CONFIG_EVENT_MAX_TYPES × (16 + CONFIG_EVENT_MAX_SUBSCRIBERS × 16) bytes
+ CONFIG_EVENT_QUEUE_SIZE × CONFIG_EVENT_STRUCT_SIZE
+ (if Slab enabled) Total Slab pool size
```

**Examples** (code defaults):
| Config | Event Types | Subscribers/Type | Queue Depth | Struct Size | Slab Pool | Memory | Target Scenario |
|--------|-------------|------------------|-------------|-------------|-----------|--------|-----------------|
| Standard | 256 | 8 | 32 | 64B | Enabled | ~30 KB | SRAM ≥ 256KB |
| Balanced | 128 | 4 | 16 | 32B | Disabled | ~8 KB | SRAM 64-128KB |
| Minimal | 64 | 2 | 8 | 32B | Disabled | ~2 KB | SRAM 32-64KB |
| **Tiny** | **8** | **1** | **4** | **32B** | **Disabled** | **~0.5 KB** | **SRAM ≤ 32KB** |

**Note**: For event type registration and subscription APIs, see the [Event System Detailed Usage Guide.md](../30-core-modules/31-event-system-guide.md).

---

## 2. Module Manager Configuration

| Configuration Macro | Type | Default | Dependencies | Description |
|--------------------|------|---------|--------------|-------------|
| `CONFIG_MODULE_MANAGER` | bool | y | — | Enable module manager: register/unregister, `init/start/stop` lifecycle, integration with event subscription. |
| `CONFIG_MAX_MODULES` | int | 16 | `MODULE_MANAGER` | Upper limit of **slots** for simultaneously registered business modules (range 4-32). |
| `CONFIG_MODULE_INIT_TIMEOUT_MS` | int | 1000 | `MODULE_MANAGER` | Maximum allowed time (ms, range 100-10000) for a single `register()` call to module `init()`. Timeout causes registration failure; 0 means no check (the code is authoritative; see `module_manager.h`). |
| `CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES` | bool | n | `MODULE_MANAGER` | When enabled: `module_manager_start_all()` performs topological sorting based on each module's `depends_on` (NULL-terminated module name string array); `module_manager_stop_all()` stops in reverse dependency order on RUNNING set. Illegal dependencies eliminated via fixed-point; cycles or anomalies fallback to priority-only sorting. |
| `CONFIG_MODULE_MANAGER_DEPENDS_LIST_MAX` | int | 16 | `MODULE_MANAGER_RUNTIME_DEPENDENCIES` | Maximum dependency names to scan in `depends_on` for a **single module** (excluding trailing NULL), prevents infinite loop from incorrectly NULL-terminated list. **Not** total system module count, **not** dependency chain depth. Range 4-64. |
| `CONFIG_MODULE_MANAGER_START_ALL_ABORT_ON_FAILURE` | bool | n | `MODULE_MANAGER` | When y: `module_manager_start_all()` stops starting subsequent modules after first `start` returns non-0. Recommended with runtime dependencies to prevent dependents from starting when their dependencies fail. |
| `CONFIG_EXAMPLE_MODULE_MULTI_DEP` | bool | n | `MODULE_MANAGER` | Compile **`example_module_multi_dep.c`**; registers via **`SYS_INIT`** (`depends_on` points to `example_module_a` / `example_module_b`). To observe topological order, also enable `RUNTIME_DEPENDENCIES` and A/B (`APP_CONFIG_ENABLE_MODULE_A`/`B`). |
| `CONFIG_EXAMPLE_MODULE_GPIO` | bool | n | `MODULE_MANAGER` | Compile **`example_module_gpio.c`**; registers via **`SYS_INIT`** (requires board-level DTS to provide **`led0`**; optional **`sw0`** user button). |
| `CONFIG_EXAMPLE_MODULE_UART` | bool | n | `MODULE_MANAGER` | Compile **`example_module_uart.c`**; registers via **`SYS_INIT`** (default bound to **`zephyr_console`** corresponding UART, may share with Shell). |
| `CONFIG_EXAMPLE_MODULE_UART_USE_ZEPHYR_CONSOLE` | bool | y | `EXAMPLE_MODULE_UART` | When y: use `DEVICE_DT_GET(DT_CHOSEN(zephyr_console))`; when n: use `device_get_binding(CONFIG_EXAMPLE_MODULE_UART_DEVICE_NAME)`. |
| `CONFIG_EXAMPLE_MODULE_UART_DEVICE_NAME` | string | `UART_0` | `EXAMPLE_MODULE_UART` | Device name string when above is n. |

**Note**: For `module_interface_t.depends_on`, `DECLARE_MODULE_*` macros, and examples, see the [Module System Detailed Usage Guide.md](../30-core-modules/32-module-system-guide.md). GPIO/UART overlay examples are in **`conf/examples/gpio_uart.conf`**.

---

## 3. System Services Configuration

For each **`sys_*`** module's responsibilities and usage notes, see the **[System Services Usage Guide.md](../30-core-modules/36-system-services-guide.md)**. The options below cover application-side services such as logging, memory pools, and watchdogs; source code remains authoritative.

**`sys_log` destination**: controlled by `sys_log_config_t.destinations` (**`sys_log_dest_mask_t`**) bitwise selecting `SYS_LOG_DEST_CONSOLE`, `SYS_LOG_DEST_MEMORY`, `SYS_LOG_DEST_RTT` (requires **`CONFIG_SEGGER_RTT`**), `SYS_LOG_DEST_UART`; mask 0 falls back to "console + memory ring" during init.

| Configuration Macro | Type | Default | Description |
|--------------------|------|---------|-------------|
| `CONFIG_SYS_LOG_LEVEL` | int | 3 | System/application log level: 0=off, 1=ERROR, 2=WARNING, 3=INFO, 4=DEBUG. |
| `CONFIG_SYS_MEMORY_POOL_SIZE` | int | 8192 | System memory pool size (bytes, range 1024-65536), for event allocation scenarios needs coordination with `CONFIG_HEAP_MEM_POOL_SIZE` etc. |
| `CONFIG_SYS_WATCHDOG_ENABLE` | bool | y | Enable watchdog related logic (specific feeding implementation see `sys_watchdog` etc.). |
| `CONFIG_SYS_WATCHDOG_TIMEOUT_MS` | int | 5000 | Watchdog timeout (ms, range 1000-30000). |

---

## 4. Thread IPC Service (in-app)

Corresponds to menu **"Thread IPC service (in-app)"**, defined in `src/modules/ipc_service/Kconfig`. Note: This is **in-app dual-thread + queue** IPC, **not** Zephyr upstream subsystem `IPC_SERVICE` (RPMSG/icmsg).

| Configuration Macro | Type | Default | Dependencies | Description |
|--------------------|------|---------|--------------|-------------|
| `CONFIG_THREAD_IPC_SERVICE` | bool | n | — | Master switch: enable Thread IPC service (worker thread + dispatch thread, request/response queues, etc.). |
| `CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS` | int | 8 | `THREAD_IPC_SERVICE` | Upper limit of incomplete requests that can be tracked simultaneously (range 2-64). |
| `CONFIG_THREAD_IPC_SERVICE_REQUEST_QUEUE_SIZE` | int | 4 | `THREAD_IPC_SERVICE` | Request queue depth on worker thread side (static buffer, range 2-32). |
| `CONFIG_THREAD_IPC_SERVICE_RESPONSE_QUEUE_SIZE` | int | 4 | `THREAD_IPC_SERVICE` | Response queue depth on dispatch thread side (range 2-32). |
| `CONFIG_THREAD_IPC_SERVICE_STACK_SIZE` | int | 1024 | `THREAD_IPC_SERVICE` | Stack size (bytes, range 512-8192) for **each** internal thread (worker / dispatcher). |
| `CONFIG_THREAD_IPC_SERVICE_PRIORITY` | int | 5 | `THREAD_IPC_SERVICE` | Both threads' priority (range -16-15, per Zephyr convention). |
| `CONFIG_THREAD_IPC_SERVICE_LOG_LEVEL` | int | 1 | `THREAD_IPC_SERVICE` | This module's log level: 0-4, production recommend 0 or 1. |
| `CONFIG_THREAD_IPC_SERVICE_EXAMPLE` | bool | n | `THREAD_IPC_SERVICE` | Whether to compile built-in SYS_INIT demo code (can save RAM vs `example_module_ipc`). |
| `CONFIG_THREAD_IPC_SERVICE_EVENT_BRIDGE` | bool | n | `THREAD_IPC_SERVICE` + `EVENT_SYSTEM` | Enable bridging IPC results to event system (`ipc_service_event`). |
| `CONFIG_THREAD_IPC_SERVICE_SHARED_MEM` | bool | y | `THREAD_IPC_SERVICE` | Enable shared memory pool (reference count management). |
| `CONFIG_THREAD_IPC_SERVICE_SHARED_MEM_POOL_SIZE` | int | 4 | `THREAD_IPC_SERVICE_SHARED_MEM` | Number of shared memory blocks (range 2-128). |
| `CONFIG_THREAD_IPC_SERVICE_SHARED_MEM_BLOCK_SIZE` | int | 256 | `THREAD_IPC_SERVICE_SHARED_MEM` | Size per block (bytes, range 64-4096). |
| `CONFIG_EXAMPLE_MODULE_THREAD_IPC` | bool | n | `THREAD_IPC_SERVICE` + `MODULE_MANAGER` | Compile **`example_module_ipc.c`**: registered with `module_manager` via **`SYS_INIT`** and integrated with Thread IPC. |

**Memory usage reference**:
- Default config (64KB SRAM): ~4.7 KB
- Tiny config (20KB SRAM): ~2.1 KB

**Note**: For architecture and APIs, see the **[Thread IPC Service Usage Guide.md](../30-core-modules/33-thread-ipc-service-guide.md)** and **[Thread IPC Module Integration Guide.md](../30-core-modules/34-thread-ipc-integration-guide.md)**.

---

## 5. Application KV Store (optional persistent)

The **"Application KV store"** menu is in the root **`Kconfig`**. **`CONFIG_APP_KV_PERSIST`** depends on **`CONFIG_FLASH=y`** (including **`FLASH_PAGE_LAYOUT`** / **`FLASH_HAS_DRIVER_ENABLED`**), plus **`CONFIG_SETTINGS`**, **`CONFIG_SETTINGS_NVS`**, and related options. NVS is an internal Zephyr **choice** backend, so application **Kconfig cannot use `select`**; enable the options explicitly and in order in **`prj.conf` / fragments**. See **`conf/features/app_kv_persist.conf`** for a typical combination. Targets without Flash (for example **`native_sim`**) should not merge the persistence fragment.

If **`Deprecated symbol FLASH_CODE_PARTITION_ADDRESS_INVALID`** appears: on ARM + XIP with non-zero Flash base, if **`/chosen` does not set `zephyr,code-partition`**, Kconfig treats partition address as 0 triggering this deprecated symbol. This repository's **`boards/nucleo_l4r5zi.overlay`** already supplements **`&flash0` / `partitions` `ranges`**, adding **`slot0_partition`** and **`zephyr,code-partition`** to eliminate this warning. If also wanting **link stage** to not place firmware in `storage` partition, can additionally specify **`CONFIG_USE_DT_CODE_PARTITION=y`** (depends on above chosen). Other boards follow [Zephyr 4.4 Migration Guide](https://docs.zephyrproject.org/latest/releases/migration-guide-4.4.html) to supplement `ranges` for Flash and `fixed-partitions`. Upstream overview at [zephyr#104862](https://github.com/zephyrproject-rtos/zephyr/issues/104862).

| Configuration Macro | Type | Default | Description |
|--------------------|------|---------|-------------|
| `CONFIG_APP_KV_PERSIST` | bool | n | Serialize entire **`app_kv`** table as single Settings record to flash; **`app_kv_init()`** auto-loads at boot. Requires **`SETTINGS`** + **`SETTINGS_NVS`** and devicetree **`zephyr,settings-partition`**. |
| `CONFIG_APP_KV_PERSIST_AUTOSAVE` | bool | n | Write flash after every **`app_kv_set`** / `remove` / `clear`**; **high wear**, generally use **`app_kv_save()`** or Shell **`app kv save`**. |

**Board-level**: `nucleo_l4r5zi` can use repository **`boards/nucleo_l4r5zi.overlay`** (board-level dts already has **`storage_partition`**). Other boards must specify partition in chosen. Merge config example: **`conf/features/app_kv_persist.conf`**.

API: **`app_kv_save()`**, **`app_kv_load()`** (see **`app_kv.h`**); error code **`APP_ERR_IO`**.

---

## 6. Feature Switches in Application Header Files (non-Kconfig)

**`APP_CONFIG_*`** etc. in `include/zeplod/app_config.h` are **compile-time macros**, not in `menuconfig`, common items below (as per file):

| Macro | Description |
|-------|-------------|
| `APP_CONFIG_ENABLE_MODULE_A` / `B` | Whether to compile **`SYS_INIT`** auto-registration for example modules A/B (see corresponding **`example_module_*.c`**). |
| `APP_INIT_PRIO_*` | **`SYS_INIT(POST_KERNEL, prio)`** priority: same stage **smaller value runs earlier**; subsystem and module order based on this (don't confuse with thread `priority` "small value = high priority", here only represents **startup call order**). |
| `APP_CONFIG_ENABLE_APP_KV` | Whether to enable RAM string key-value table **`app_kv_*`** (**`APP_INIT_PRIO_APP_KV`** stage initialization; default pre-populated **`build.target`**). |
| `APP_KV_MAX_ENTRIES` / `APP_KV_KEY_MAX_LEN` / `APP_KV_VALUE_MAX_LEN` | Key-value table capacity and max length per key/value (characters before trailing `\0`). |
| `APP_CONFIG_ENABLE_LOGGING` etc. | Whether to enable application-layer features like logging, watchdog, memory manager, timer service, etc. |
| `APP_CONFIG_ENABLE_STATS` | When `1`: event dispatcher enables runtime statistics, Shell **`app events`** additionally prints `event_dispatcher_get_stats()`; when `0` only event system global statistics. |
| `APP_CONFIG_ENABLE_LOG_DUMP` | When `1` Shell **`app log`** calls `sys_log_dump()`; when `0` prompts disabled (can still use via API). |

Runtime dependency ordering is independent of the switches above; the multi-dependency example requires Kconfig to enable `EXAMPLE_MODULE_MULTI_DEP` and usually keep A/B enabled. For the full startup order, see "Application Startup and Initialization Order (Zephyr SYS_INIT)" in the [Module System Detailed Usage Guide.md](../30-core-modules/32-module-system-guide.md).

---

## 7. Common Zephyr Options (working with this application)

These are **not** in this repository's `Kconfig` menu, but frequently need modification in `prj.conf`:

| Configuration Macro | Description |
|--------------------|-------------|
| `CONFIG_HEAP_MEM_POOL_SIZE` | Kernel heap size. Must be **non-zero** when event system `event_create` / `event_publish_copy` etc. use `k_malloc`. |
| `CONFIG_MAIN_STACK_SIZE` | Main thread stack; increase when business is heavy. |

For board-level peripherals, networking, filesystem etc. refer to corresponding Zephyr/Kconfig documentation.

---

## 8. Optional productization modules (factory_mode)

Defined in `src/modules/factory/Kconfig`. The implementation and public headers are the current sources of truth. Overlays: **`conf/features/factory_mode.conf`** (production line), **`conf/targets/production.conf`** (factory off by default).

| Configuration Macro | Type | Default | Depends | Description |
|--------------------|------|---------|---------|-------------|
| `CONFIG_FACTORY_MODE_MODULE` | bool | n | `EVENT_MAX_TYPES > 70` | Factory test module; publishes event **70**. |
| `CONFIG_FACTORY_MODE_MODULE_AUTOINIT` | bool | y | `FACTORY_MODE_MODULE` | Auto-register via `SYS_INIT`. |
| `CONFIG_FACTORY_MODE_SHELL` | bool | y | `FACTORY_MODE_MODULE` + `SHELL` | Register `factory` shell commands. |
| `CONFIG_FACTORY_MODE_GPIO_STUB` | bool | y | `FACTORY_MODE_MODULE` | GPIO loopback stub (always pass in Phase 4). |
| `CONFIG_FACTORY_MODE_CAL_MAX_ENTRIES` | int | 8 | `FACTORY_MODE_MODULE` | Max staged calibration entries (1–32). |
| `CONFIG_FACTORY_MODE_CAL_KEY_MAX_LEN` | int | 24 | `FACTORY_MODE_MODULE` | Max calibration key length (8–64). |
| `CONFIG_FACTORY_MODE_CAL_VALUE_MAX_LEN` | int | 64 | `FACTORY_MODE_MODULE` | Max calibration value length (16–256). |

**Note:** `finalize_pass` requires **`CONFIG_APP_KV_ENABLE=y`**; without app_kv it returns **`APP_ERR_DISABLED`**.

---

## Document Maintenance

- When Kconfig source changes, please sync update this page's tables.
- Details strongly related to modules/events still subject to thematic documents:
  [Module System Detailed Usage Guide.md](../30-core-modules/32-module-system-guide.md) · [Event System Detailed Usage Guide.md](../30-core-modules/31-event-system-guide.md) · [Documentation Index.md](../00-getting-started/02-doc-index.md)
