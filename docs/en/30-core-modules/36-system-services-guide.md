> Language: [中文](../../zh-CN/30-核心模块/36-系统服务使用说明.md) | **English**

# System Services Usage Guide (sys_*)

This document describes the System Services under **`src/services/`**: **Logging** (`sys_log`), **Memory Pool** (`sys_memory`), **Software Timer** (`sys_timer`), **Watchdog** (`sys_watchdog`), and optional **Diagnostics** (`sys_diag`). They sit **between Zephyr kernel and business modules**, providing unified capabilities for the Event System, Module Manager, etc.

**Related**: **[Zephyr Application Development and Services Guide.md](../40-app-development/41-zephyr-app-development.md)** (Zephyr general kernel and service patterns) · **[Project Configuration Options.md](../40-app-development/42-config-options.md)** §3 · **[Developer Getting Started Guide.md](../00-getting-started/04-developer-guide.md)** · Doxygen **`docs/api/html`** (generation method in **[Scripts and Tools Guide.md](../60-debugging/63-scripts-and-tools.md)**)

---

## 1. Overview

| Module | Header File | Main Responsibility |
|--------|-------------|---------------------|
| Logging | `sys_log.h` / `sys_log.c` | Leveled logging; **`sys_log_dest_mask_t`** bitmask selects console printk, memory ring, (optional) SEGGER RTT, UART (typically shares printk path with console) |
| Memory | `sys_memory.h` / `sys_memory.c` | Multi-pool allocation, statistics, optional allocation tracking; works with **`CONFIG_SYS_MEMORY_POOL_SIZE`** etc. |
| Timer | `sys_timer.h` / `sys_timer.c` | One-shot/periodic callback wrapper based on kernel timer |
| Watchdog | `sys_watchdog.h` / `sys_watchdog.c` | Soft/hardware watchdog abstraction, feeding and pre-timeout callbacks |
| Diagnostics | `sys_diag.h` / `sys_diag.c` | Optional (`CONFIG_SYS_DIAG_ENABLE`); health snapshot aggregation and `diag dump` Shell |

**Initialization order**: In main firmware, each **`sys_*_init()`** is called before **`main()`** by multiple **`SYS_INIT(POST_KERNEL, APP_INIT_PRIO_*)`** in **`app_main.c`** (priority definitions in **`include/zeplod/app_config.h`**); **`app_start()`** in **`main()`** then starts threads and modules. If a service is not initialized, corresponding API behavior follows implementation (may return error or undefined).

---

## 1.1 Minimal Example (equivalent to `app_main.c` init steps)

The following snippets only demonstrate **typical calling order**; macros, configuration, and **`APP_CONFIG_*`** switches follow project source code. Complete API in **`docs/api/html`**.

### Logging `sys_log`

```c
#include <zeplod/sys_log.h>

void demo_sys_log(void)
{
	const sys_log_config_t cfg = {
		.default_level = SYS_LOG_LEVEL_INF,
		.destinations = SYS_LOG_DEST_CONSOLE | SYS_LOG_DEST_MEMORY,
		.enable_timestamp = true,
		.enable_colors = true,
		.enable_module_name = true,
		.memory_buffer_size = 1024,
	};

	(void)sys_log_init(&cfg);
	sys_log_print(SYS_LOG_LEVEL_INF, "demo", "hello %d", 1);
	/* Or use convenience macro from sys_log.h: SYS_LOG_I("demo", "hello %d", 1); */
}
```

### Memory `sys_memory`

```c
#include <zeplod/sys_memory.h>

void demo_sys_mem(void)
{
	const sys_mem_config_t cfg = {
		.pool_sizes = {
			[SYS_MEM_POOL_GENERAL] = 8192,
			[SYS_MEM_POOL_EVENT] = 4096,
			[SYS_MEM_POOL_MODULE] = 4096,
			[SYS_MEM_POOL_DMA] = 0,
		},
		.enable_tracking = true,
		.enable_defrag = false,
		.max_allocations = 256,
	};

	(void)sys_mem_init(&cfg);
	void *p = sys_mem_alloc(SYS_MEM_POOL_GENERAL, 32);

	if (p != NULL) {
		sys_mem_free(SYS_MEM_POOL_GENERAL, p);
	}
}
```

### Timer `sys_timer`

```c
#include <zeplod/sys_timer.h>

static void on_tick(sys_timer_handle_t t, void *user_data)
{
	(void)t;
	(void)user_data;
	/* Lightweight logic; for heavy work, post event or work */
}

void demo_sys_timer(void)
{
	(void)sys_timer_init();

	const sys_timer_config_t cfg = {
		.mode = SYS_TIMER_PERIODIC,
		.delay_ms = 500,
		.period_ms = 500,
		.callback = on_tick,
		.user_data = NULL,
		.name = "demo",
		.priority = 5,
	};

	sys_timer_handle_t h = sys_timer_create(&cfg);

	if (h != NULL) {
		(void)sys_timer_start(h);
	}
}
```

### Watchdog `sys_watchdog`

```c
#include <zeplod/sys_watchdog.h>

void demo_sys_wdt(void)
{
	const wdt_config_t cfg = {
		.mode = WDT_MODE_SOFTWARE,
		.timeout_ms = 5000,
		.feed_margin_ms = 1000,
		.pre_expire_callback = NULL,
		.callback_user_data = NULL,
		.reset_on_expire = false,
		.name = "demo",
	};

	(void)sys_wdt_init(&cfg);
	(void)sys_wdt_start();
	(void)sys_wdt_feed();
}
```

---

## 2. Logging Service `sys_log`

- **Levels**: `sys_log_level_t` (`OFF` / `ERR` / `WRN` / `INF` / `DBG`).
- **Output destinations**: `sys_log_dest_mask_t` (bitwise OR: `SYS_LOG_DEST_CONSOLE` / `SYS_LOG_DEST_MEMORY` / `SYS_LOG_DEST_RTT` / `SYS_LOG_DEST_UART`; when mask is 0, initializes to console + memory ring).
- **Configuration**: `sys_log_config_t` (default level, whether with timestamp/color/module name, memory buffer size, etc.).

### sys_log API

| Function | Description |
|----------|-------------|
| `int sys_log_init(const sys_log_config_t *config)` | Initialize logging system |
| `void sys_log_set_level(const char *module, sys_log_level_t level)` | Set module log level |
| `sys_log_level_t sys_log_get_level(const char *module)` | Get module log level |
| `void sys_log_set_destination(sys_log_dest_mask_t dest, bool enable)` | Set log output destination |
| `void sys_log_print(sys_log_level_t level, const char *module, const char *format, ...)` | Print log |
| `void sys_log_print_ts(sys_log_level_t level, const char *module, const char *format, ...)` | Print log with timestamp |
| `void sys_log_hexdump(sys_log_level_t level, const char *module, const void *data, size_t len, bool ascii)` | Hexadecimal dump |
| `uint32_t sys_log_get_entries(sys_log_entry_t *entries, uint32_t count, bool oldest_first)` | Get memory ring logs |
| `void sys_log_clear_buffer(void)` | Clear memory buffer |
| `uint32_t sys_log_get_count(void)` | Get recorded message count |
| `void sys_log_dump(sys_log_level_t level_filter)` | Dump logs to console |

### sys_log Macros

| Macro | Description |
|-------|-------------|
| `SYS_LOG_E(module, fmt, ...)` | Error level log |
| `SYS_LOG_W(module, fmt, ...)` | Warning level log |
| `SYS_LOG_I(module, fmt, ...)` | Info level log |
| `SYS_LOG_D(module, fmt, ...)` | Debug level log |
| `SYS_LOG_HEXDUMP_E(module, data, len)` | Hex dump error log |
| `SYS_LOG_HEXDUMP_I(module, data, len)` | Hex dump info log |

**Usage Recommendations**:

- Business and modules preferably use macros or wrapper functions provided by this service for unified format and easy backend switching.
- Align with **`CONFIG_SYS_LOG_LEVEL`** (see project configuration options); release builds can appropriately raise threshold to reduce log volume.

---

## 3. Memory Service `sys_memory`

- **Pool types**: `sys_mem_pool_type_t` (`GENERAL`, `EVENT`, `MODULE`, `DMA`, etc.), used for **logical isolation** of different allocation purposes, facilitating statistics and rate limiting.
- **Statistics**: `sys_mem_stats_t` (usage, peak, failure count, fragmentation, etc.).
- **Debugging**: Optional allocation tracking (`sys_mem_alloc_info_t`), for leak and double-free analysis.

### sys_memory API

| Function | Description |
|----------|-------------|
| `int sys_mem_init(const sys_mem_config_t *config)` | Initialize memory system (NULL uses default) |
| `void *sys_mem_alloc(sys_mem_pool_type_t type, size_t size)` | Allocate memory |
| `void *sys_mem_calloc(sys_mem_pool_type_t type, size_t size)` | Allocate and zero memory |
| `void sys_mem_free(sys_mem_pool_type_t type, void *ptr)` | Free memory |
| `void *sys_mem_realloc(sys_mem_pool_type_t type, void *ptr, size_t size)` | Reallocate |
| `void sys_mem_get_stats(sys_mem_pool_type_t type, sys_mem_stats_t *stats)` | Get pool statistics |
| `void sys_mem_reset_stats(sys_mem_pool_type_t type)` | Reset statistics |
| `uint32_t sys_mem_get_active_allocations(sys_mem_pool_type_t type)` | Get active allocation count |
| `void sys_mem_dump_allocations(sys_mem_pool_type_t type)` | Dump allocation info |
| `uint32_t sys_mem_check_leaks(sys_mem_pool_type_t type)` | Check leaks |
| `size_t sys_mem_defrag(sys_mem_pool_type_t type)` | Defragmentation |
| `size_t sys_mem_get_heap_size(void)` | Get total heap size |
| `size_t sys_mem_get_free_size(void)` | Get available heap size |
| `size_t sys_mem_get_min_free_size(void)` | Get historical minimum available |
| `void *sys_mem_alloc_with_info(sys_mem_pool_type_t type, size_t size, const char *module, uint32_t line)` | Allocate with info (debug) |

### sys_memory Macros

| Macro | Description |
|-------|-------------|
| `SYS_MEM_ALLOC_GENERAL(size)` | Allocate from general pool |
| `SYS_MEM_FREE_GENERAL(ptr)` | Free to general pool |
| `SYS_MEM_ALLOC_EVENT(size)` | Allocate from event pool |
| `SYS_MEM_FREE_EVENT(ptr)` | Free to event pool |

**Usage Recommendations**:

- Event payloads, module private data, etc., try to allocate from corresponding pools, avoid mixing with general pool to prevent fragmentation making analysis difficult.
- **`CONFIG_SYS_MEMORY_DEBUG`** enables info-carrying allocation (filename/line number).
- **`CONFIG_SYS_MEMORY_POOL_SIZE`** and Zephyr **`CONFIG_HEAP_MEM_POOL_SIZE`** (`k_malloc`) are different levels: former is this service's managed pool, latter is kernel heap; combined total must not exceed chip RAM budget (see **[Devicetree and Memory Configuration Manual.md](../40-app-development/44-devicetree-memory-config.md)**).

---

## 4. Timer Service `sys_timer`

- **Modes**: One-shot (`SYS_TIMER_ONESHOT`) and periodic (`SYS_TIMER_PERIODIC`).
- **Callback**: Executes in **work context** (specific thread/interrupt level follows implementation); callbacks should **avoid long blocking** or deadlock with event dispatcher.
- **Handle**: `sys_timer_handle_t` is an opaque pointer; create/start/stop/destroy need to be used in pairs.
- **Status**: `sys_timer_status_t` (`SYS_TIMER_STOPPED` / `SYS_TIMER_RUNNING` / `SYS_TIMER_PAUSED` / `SYS_TIMER_EXPIRED`).

### sys_timer API

| Function | Description |
|----------|-------------|
| `int sys_timer_init(void)` | Initialize timer system |
| `sys_timer_handle_t sys_timer_create(const sys_timer_config_t *config)` | Create timer |
| `int sys_timer_delete(sys_timer_handle_t timer)` | Delete timer |
| `int sys_timer_start(sys_timer_handle_t timer)` | Start timer |
| `int sys_timer_stop(sys_timer_handle_t timer)` | Stop timer |
| `int sys_timer_restart(sys_timer_handle_t timer)` | Restart timer |
| `int sys_timer_pause(sys_timer_handle_t timer)` | Pause timer |
| `int sys_timer_resume(sys_timer_handle_t timer)` | Resume timer |
| `sys_timer_status_t sys_timer_get_status(sys_timer_handle_t timer)` | Get status |
| `int sys_timer_set_period(sys_timer_handle_t timer, uint32_t period_ms)` | Modify period |
| `uint32_t sys_timer_get_time_until_expiry(sys_timer_handle_t timer)` | Get time until expiry |
| `int sys_timer_get_stats(sys_timer_handle_t timer, sys_timer_stats_t *stats)` | Get statistics |
| `int sys_timer_reset_stats(sys_timer_handle_t timer)` | Reset statistics |
| `sys_timer_handle_t sys_timer_oneshot(uint32_t delay_ms, sys_timer_callback_t callback, void *user_data)` | Create and start one-shot timer |
| `sys_timer_handle_t sys_timer_periodic(uint32_t period_ms, sys_timer_callback_t callback, void *user_data)` | Create and start periodic timer |
| `void sys_timer_sleep(uint32_t ms)` | Sleep (convenience wrapper) |
| `uint32_t sys_timer_get_uptime(void)` | Get system uptime |

**Usage Recommendations**:

- When integrating with the **Event Bus**, do only lightweight work in the callback and post to the module thread via **`event_publish_*`**.
- Precision and jitter affected by kernel tick and load; hard real-time needs combined hardware timer and peripheral documentation evaluation.

---

## 5. Watchdog Service `sys_watchdog`

- **Modes**: `wdt_mode_t` (`WDT_MODE_SOFTWARE` / `WDT_MODE_HARDWARE` / `WDT_MODE_DUAL`).
- **Configuration**: Timeout, feed margin (`feed_margin_ms`), pre-timeout user callback, etc.
- **Status**: `wdt_status_t` (`WDT_STATUS_STOPPED` / `WDT_STATUS_RUNNING` / `WDT_STATUS_PAUSED` / `WDT_STATUS_EXPIRED` / `WDT_STATUS_ERROR`).
- **Feeding**: Periodically call the feeding interface in the main loop or health task; for deep sleep and low-power scenarios, check the PM notes in the **[OTA and Storage Expansion Guide.md](../70-release-productization/74-ota-storage-guide.md)** to avoid accidental reset.

### sys_watchdog API

| Function | Description |
|----------|-------------|
| `int sys_wdt_init(const wdt_config_t *config)` | Initialize watchdog |
| `int sys_wdt_start(void)` | Start watchdog |
| `int sys_wdt_stop(void)` | Stop watchdog |
| `int sys_wdt_feed(void)` | Feed watchdog |
| `int sys_wdt_pause(void)` | Pause (for debug) |
| `int sys_wdt_resume(void)` | Resume |
| `wdt_status_t sys_wdt_get_status(void)` | Get status |
| `int sys_wdt_monitor_thread(k_tid_t thread_id, const char *thread_name, uint32_t max_idle_ms)` | Register thread monitoring |
| `int sys_wdt_unmonitor_thread(k_tid_t thread_id)` | Unregister thread monitoring |
| `int sys_wdt_thread_alive(k_tid_t thread_id)` | Mark thread alive |
| `void sys_wdt_get_stats(wdt_stats_t *stats)` | Get statistics |
| `void sys_wdt_reset_stats(void)` | Reset statistics |
| `uint32_t sys_wdt_get_time_since_feed(void)` | Get time since last feed |
| `uint32_t sys_wdt_get_time_until_expire(void)` | Get time until expiry |
| `void sys_wdt_simulate_expire(void)` | Simulate expiry (for testing) |

**Usage Recommendations**:

- Align with **`CONFIG_SYS_WATCHDOG_ENABLE`**, **`CONFIG_SYS_WATCHDOG_TIMEOUT_MS`**.
- During debug phase can temporarily disable or increase timeout, **restore reasonable threshold before production**.

---

## 6. Configuration Options Quick Reference

**`CONFIG_SYS_*`** explanations for above services are centralized in **[Project Configuration Options.md](../40-app-development/42-config-options.md) §3 (System Services)**. If behavior is abnormal after modifying `prj.conf`, first check **[Troubleshooting.md](../60-debugging/62-troubleshooting.md)**, then check the **map file** and logs.

---

## 7. API Details

Function-level descriptions, parameters, and return values follow source comments and **Doxygen-generated `docs/api/html`**; the generation command is in the **[Scripts and Tools Guide.md](../60-debugging/63-scripts-and-tools.md)**.
