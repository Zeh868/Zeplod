> Language: [中文](../../zh-CN/30-核心模块/32-模块系统详细使用说明.md) | **English**

# Module System Detailed Usage Guide

## Table of Contents

- [Overview](#overview)
- [Application Startup and Initialization Order (Zephyr SYS_INIT)](#application-startup-and-initialization-order-zephyr-sys_init)
- [Core Concepts](#core-concepts)
- [System Architecture](#system-architecture)
- [Configuration Options](#configuration-options)
- [Module Configuration Management Tool](#module-configuration-management-tool)
- [Runtime Dependencies (Optional)](#runtime-dependencies-optional)
- [Complete Kconfig Index](#complete-kconfig-index)
- [API Reference](#api-reference)
- [Usage Guide](#usage-guide)
- [Best Practices](#best-practices)
- [Troubleshooting](#troubleshooting)

---

## Overview

The Module System provides a dynamic, extensible component management framework supporting module registration, Lifecycle management, and Event-driven communication. This system is closely integrated with the Event System to provide modular architecture support for applications.

### Key Features

| Feature | Description |
|---------|-------------|
| Max Modules | 16 (configurable) |
| Max Event Subscriptions Per Module | 8 (configurable) |
| Initialization Timeout | 1000ms (configurable) |
| Priority Scheduling | Supported (4 priorities) |
| Dynamic Registration/Unregistration | Supported |
| Module State Management | Complete state machine |
| Runtime Dependency Ordering (Optional) | `depends_on` + Kconfig, see below |

### Module State Machine

```
                     ┌──────────────────┐
                     │ UNINITIALIZED    │
                     │                  │
                     └────────┬─────────┘
                              │ register()
                              ▼
                     ┌──────────────────┐
                     │ INITIALIZING     │
                     │                  │
                     └────────┬─────────┘
                              │ init() complete
                              ▼
                     ┌──────────────────┐
               ┌─────│ INITIALIZED      │─────┐
               │     │                  │     │
               │     └────────┬─────────┘     │
               │              │ start()       │ stop()
               │              ▼               │
               │     ┌──────────────────┐     │
               │     │ RUNNING          │◄────┤
               │     │                  │     │
               │     └────────┬─────────┘     │
               │              │ suspend()      │
               │              ▼               │
               │     ┌──────────────────┐     │
               └────►│ SUSPENDED        │─────┘
                     │                  │
                     └────────┬─────────┘
                              │ error
                              ▼
                     ┌──────────────────┐
                     │ ERROR            │
                     │                  │
                     └──────────────────┘
```

**Suspend semantics**

- `module_manager_suspend_module` only sets manager state to `SUSPENDED` and blocks manager-routed `send_to_module`, `broadcast`, and `subscribe` delivery; it does **not** call the module `stop()`.
- Module-owned hardware (interrupts, DMA, timers, etc.) and direct calls outside the manager may still run.
- To invoke business `stop()` for full shutdown: call `module_manager_resume_module` first, then `module_manager_stop_module`; calling `stop_module` while `SUSPENDED` returns 0 idempotently without calling `stop()`.
- `module_manager_stop_all` stops only `RUNNING` slots, not `SUSPENDED` ones.

---

## Application Startup and Initialization Order (Zephyr SYS_INIT)

This template completes application and subsystem initialization **before `main()` enters** through Zephyr **`SYS_INIT(fn, POST_KERNEL, prio)`**; **within the same `POST_KERNEL` stage, smaller `prio` values execute earlier** (consistent with **`APP_INIT_PRIO_*`** macros in `include/zeplod/app_config.h`).

### Startup Stage (`main` before)

Typical order:

1. **`app_init_apply_cb`**: Print version, zero application control block, write default `app_config_t` (from `app_config.h` macros).
2. **`sys_log_init` / `sys_mem_init`** (controlled by `APP_CONFIG_ENABLE_*` macros).
3. **`event_system_init`** → **`event_dispatcher_init`** → **`sys_timer_init`** → **`sys_wdt_init`** (按宏开关).
4. **`module_manager_init`**.
5. **Business modules** call **`SYS_INIT(..., APP_INIT_PRIO_MODULE_*)`** within their **`.c`** files to call **`module_manager_register()`** (examples in `example_module_a.c`, `example_module_ipc.c`, etc.). Modules with dependencies should use **larger** `APP_INIT_PRIO_*` values (registered later), e.g., `example_module_multi_dep` after A/B.
6. **`app_init_finalize`**: Set "application initialized" flag, record startup timestamp.

On failure (e.g., `event_dispatcher_init` returns non-`EVENT_OK`), the corresponding init callback returns non-zero, and Zephyr startup path reports error; specific behavior depends on Zephyr version.

### `app_init()` / `app_start()` (in `main`)

- **`app_init(config)`**: Returns **`APP_OK`** after all SYS_INIT succeed; **`config` pointer currently does not participate** in SYS_INIT path (reserved for API compatibility). If runtime config override is needed, use another hook (e.g., weak symbol default table or separate initialization module).
- **`app_start()`**: Still called by **`main()`**: Start Event System, dispatcher thread, Module Manager and registered modules, watchdog and heartbeat timer, etc. (consistent with pre-modification behavior).

### How to Register a New Business Module

1. Add **`SYS_INIT(your_module_auto_register, POST_KERNEL, APP_INIT_PRIO_…)`** at the end of the module **`.c`** file, and in the callback prepare **`config`** and call **`module_manager_register(your_module_get_interface(), &cfg, &id)`**.
2. Add **`APP_INIT_PRIO_MODULE_YOURS`** in **`app_config.h`**, with a value between **`APP_INIT_PRIO_MODULE_MGR`** and **`APP_INIT_PRIO_APP_FINAL`**; if depending on other modules, **value should be larger** than the depended module's priority (executes later).
3. **No need** to modify **`app_main.c`**'s centralized registration list ( **`app_register_modules()``** has been removed).

Unit test project (**`tests/`**) does not link **`app_main.c`**, and can still explicitly call **`module_manager_init()`** / **`module_manager_register()`** in test cases.

---

## Core Concepts

### Module Interface

The standard interface that all modules must implement:

```c
typedef struct {
    const char *name;                     // Module name
    uint32_t version;                     // Version number (MAJOR.MINOR.PATCH)
    module_priority_t priority;           // Priority
    const char *const *depends_on;        // Runtime dependency name list (NULL-terminated), optional; see below
    int (*init)(void *config);            // Initialization function
    int (*start)(void);                   // Startup function
    int (*stop)(void);                    // Stop function
    int (*shutdown)(void);                // Destruction function
    module_event_handler_t on_event;      // Event handling callback
    module_status_t (*get_status)(void);  // Get status
    int (*control)(int cmd, void *arg);   // Control command
} module_interface_t;
```

For the complete definition, refer to **`module_base.h`** in the repository.

### Module Status

```c
typedef enum {
    MODULE_STATUS_UNINITIALIZED = 0,  // Uninitialized
    MODULE_STATUS_INITIALIZING,       // Initializing
    MODULE_STATUS_INITIALIZED,        // Initialized
    MODULE_STATUS_RUNNING,            // Running
    MODULE_STATUS_STOPPED,            // Stopped
    MODULE_STATUS_ERROR,              // Error
    MODULE_STATUS_SUSPENDED           // Suspended
} module_status_t;
```

### Module Priority

```c
typedef enum {
    MODULE_PRIORITY_LOW      = 10,   // Low priority (starts last)
    MODULE_PRIORITY_NORMAL   = 5,    // Normal priority
    MODULE_PRIORITY_HIGH     = 2,    // High priority
    MODULE_PRIORITY_CRITICAL = 0     // Critical priority (starts first)
} module_priority_t;
```

### Module Info Structure

```c
typedef struct {
    const module_interface_t *interface;      // Module interface
    void *config;                             // Configuration data
    void *internal_data;                      // Internal data
    module_status_t status;                   // Current status
    uint32_t id;                              // Module ID
    module_event_subscription_t event_subscriptions[CONFIG_MODULE_MAX_EVENT_SUBSCRIPTIONS];  // Event subscriptions (default 8)
    uint8_t event_subscription_count;         // Subscription count
} module_info_t;
```

---

## System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Application Layer                        │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────────────────────────┐   │
│  │              Module Manager                           │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  │   │
│  │  │  Module A   │  │  Module B   │  │  Module C   │  │   │
│  │  │  (GPIO)     │  │  (UART)     │  │  (Sensor)   │  │   │
│  │  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  │   │
│  │         │                │                │          │   │
│  │         └────────────────┼────────────────┘          │   │
│  │                          │                           │   │
│  │         ┌────────────────▼────────────────┐          │   │
│  │         │     Event System Integration     │          │   │
│  │         └────────────────┬────────────────┘          │   │
│  └──────────────────────────┼───────────────────────────┘   │
│                             │                               │
│                             ▼                               │
│                    ┌─────────────────┐                      │
│                    │   Event System  │                      │
│                    └─────────────────┘                      │
└─────────────────────────────────────────────────────────────┘
```

---

## Configuration Options

Kconfig macros directly related to the Module Manager are explained in the table below; **all project configuration items** (including Event System, Thread IPC, etc.) are centrally documented in **[Project Configuration Options.md](../40-app-development/42-config-options.md)**.

| Config Macro | Summary Meaning |
|--------------|-----------------|
| `CONFIG_MODULE_MANAGER` | Whether to enable Module Manager |
| `CONFIG_MAX_MODULES` | Maximum registered module slot count |
| `CONFIG_MODULE_INIT_TIMEOUT_MS` | `init()` timeout (registration stage) |
| `CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES` | Whether to order by `depends_on` topology for start / reverse order for stop |
| `CONFIG_MODULE_MANAGER_DEPENDS_LIST_MAX` | Single module `depends_on` scan limit (prevents dead loops) |
| `CONFIG_MODULE_MANAGER_START_ALL_ABORT_ON_FAILURE` | Whether `start_all` aborts on first `start` failure |
| `CONFIG_EXAMPLE_MODULE_MULTI_DEP` | Whether to compile multi-dependency example module |

`CONFIG_MODULE_MAX_EVENT_SUBSCRIPTIONS` can be overridden via macro in `module_base.h` (default 8).

`prj.conf` example:

```kconfig
CONFIG_MODULE_MANAGER=y
CONFIG_MAX_MODULES=16
CONFIG_MODULE_INIT_TIMEOUT_MS=1000
# CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES=y
```

---

## Module Configuration Management Tool

The project provides a Python script for convenient management of module enable/disable configuration.

### Usage

**Script location**: `scripts/module_config.py`

**Basic commands**:

```bash
# View all available modules
python scripts/module_config.py list

# View current module status
python scripts/module_config.py status

# Enable an example module
python scripts/module_config.py enable EXAMPLE_MODULE_GPIO

# Disable an example module
python scripts/module_config.py disable EXAMPLE_MODULE_GPIO

# Generate custom configuration file
python scripts/module_config.py generate --output prj_custom.conf
```

### Workflow Examples

**Scenario 1: Enable an Optional Module**

```bash
# 1. View current status
python scripts/module_config.py status

# 2. Enable the required module
python scripts/module_config.py enable EXAMPLE_MODULE_GPIO

# 3. Confirm configuration has been updated
python scripts/module_config.py status

# 4. Rebuild
west build -b nucleo_l4r5zi
```

**Scenario 2: Create Custom Configuration**

```bash
# 1. Enable/disable modules
python scripts/module_config.py enable EXAMPLE_MODULE_UART
python scripts/module_config.py disable EXAMPLE_MODULE_GPIO

# 2. Generate custom configuration file
python scripts/module_config.py generate --output prj_my_project.conf

# 3. Build with custom configuration
west build -b nucleo_l4r5zi . -- -DCONF_FILE="prj.conf;prj_my_project.conf"
```

### Configuration Notes

- The script directly modifies `CONFIG_xxx=y/n` configurations in `prj.conf`
- All modules **are independent of each other**; enabling one module does not affect others
- Generated `prj_custom.conf` can be used for different project configuration combinations

### Notes

1. After modifying configuration, need to **re-run CMake configuration** (clean build directory or `west build --pristine`)

---

## Runtime Dependencies (Optional)

When **`CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES=y`**, the manager performs topological sorting on `module_manager_start_all()` based on each module's `depends_on` pointer array (**NULL-terminated**, each item being another module's `interface->name`), and reverse order for `module_manager_stop_all()`; can be mixed with dependency-free modules (`depends_on == NULL`) in the same batch.

| Kconfig | Meaning |
|---------|---------|
| `MODULE_MANAGER_RUNTIME_DEPENDENCIES` | Enable runtime dependency ordering |
| `MODULE_MANAGER_DEPENDS_LIST_MAX` | **Per module** maximum names to scan in `depends_on` (not including trailing NULL), prevents dead loops when not properly terminated; **not** total system module count, **not** dependency chain depth |
| `MODULE_MANAGER_START_ALL_ABORT_ON_FAILURE` | Whether to not start subsequent modules after any `start` failure (beneficial for dependency semantics) |
| `EXAMPLE_MODULE_MULTI_DEP` | Compile and register multi-dependency example `example_module_multi_dep` (depends on `example_module_a` / `example_module_b`) |

Implementation notes (consistent with `module_manager.c` / `module_base.h`):

- Illegal dependencies or cycles fall back to priority-only ordering (stop side: priority descending rollback).
- Macro parameter name is **`mod_name`** (not `name`), to avoid conflict with struct field **`.name`** during macro expansion.
- Multi-dependency example: `src/modules/example_module_multi_dep.c`.

---

## API Reference

### Core API

#### Initialization and Shutdown

```c
/**
 * @brief Initialize the Module Manager
 * @return 0 on success, negative value is error code
 */
int module_manager_init(void);

/**
 * @brief Start the Module Manager
 * @return 0 on success, negative value is error code
 */
int module_manager_start(void);

/**
 * @brief Stop the Module Manager
 * @return 0 on success, negative value is error code
 */
int module_manager_stop(void);

/**
 * @brief Shutdown the Module Manager (will stop and destroy all modules)
 * @return 0 on success, negative value is error code
 */
int module_manager_shutdown(void);
```

### Module Registration API

#### Registration and Unregistration

```c
/**
 * @brief Register a module
 * @param interface Module interface pointer
 * @param config Module configuration data
 * @param module_id Output: assigned module ID
 * @return 0 on success, negative value is error code
 *
 * @note init() is called outside the Module Manager mutex - to avoid
 *       deadlock when user code needs to call manager API. If init exceeds
 *       CONFIG_MODULE_INIT_TIMEOUT_MS, registration will fail.
 */
int module_manager_register(const module_interface_t *interface,
                            void *config,
                            uint32_t *module_id);

/**
 * @brief Unregister a module
 * @param module_id Module ID
 * @return 0 on success, negative value is error code
 */
int module_manager_unregister(uint32_t module_id);
```

#### Module Query

```c
/**
 * @brief Get module information (thread-safe snapshot)
 * @param module_id Module ID
 * @param out Output structure (must be non-NULL)
 * @return 0 on success, -1 if not found or invalid parameter
 */
int module_manager_get_module_info(uint32_t module_id, module_info_t *out);

/**
 * @brief Get module ID by name
 * @param name Module name
 * @return Module ID, 0 if not found
 */
uint32_t module_manager_get_id_by_name(const char *name);

/**
 * @brief Iterate through all modules
 * @param callback Callback function
 * @param user_data User data
 *
 * @note Callback is executed outside the manager lock, can call module_manager_* API without reentry deadlock
 */
void module_manager_foreach(void (*callback)(module_info_t *, void *),
                            void *user_data);
```

### Module Lifecycle API

```c
/**
 * @brief Start a single module
 * @param module_id Module ID
 * @return 0 on success, negative value is error code
 */
int module_manager_start_module(uint32_t module_id);

/**
 * @brief Stop a single module
 * @param module_id Module ID
 * @return 0 on success (including idempotent calls in non-RUNNING state), negative value is error code
 * @note Repeated stop is safe - if module status is not RUNNING (including SUSPENDED), returns 0
 *       without calling business stop; after suspend, resume then stop to invoke business stop()
 */
int module_manager_stop_module(uint32_t module_id);

/**
 * @brief Start all modules (ordered by priority)
 * @return Number of modules successfully started
 */
int module_manager_start_all(void);

/**
 * @brief Stop all modules (RUNNING only, not SUSPENDED)
 * @return Number of modules successfully stopped
 */
int module_manager_stop_all(void);

/**
 * @brief Suspend a module (pause manager-side event delivery only, no stop())
 * @param module_id Module ID
 * @return 0 on success, negative value is error code
 * @note Does not call stop(); hardware IRQ/DMA may remain active. For full shutdown, resume then stop
 */
int module_manager_suspend_module(uint32_t module_id);

/**
 * @brief Resume a suspended module
 * @param module_id Module ID
 * @return 0 on success, negative value is error code
 */
int module_manager_resume_module(uint32_t module_id);
```

### Event Handling API

```c
/**
 * @brief Module subscribes to an event type
 * @param module_id Module ID
 * @param event_type Event type
 * @return 0 on success, negative value is error code
 */
int module_manager_subscribe(uint32_t module_id, event_type_t event_type);

/**
 * @brief Module unsubscribes from an event type
 * @param module_id Module ID
 * @param event_type Event type
 * @return 0 on success, negative value is error code
 */
int module_manager_unsubscribe(uint32_t module_id, event_type_t event_type);

/**
 * @brief Send an event to a specified module
 * @param module_id Module ID
 * @param event Event pointer
 * @return 0 on success, negative value is error code
 */
int module_manager_send_to_module(uint32_t module_id, const event_t *event);

/**
 * @brief Broadcast an event to all modules
 * @param event Event pointer
 * @return Number of modules that received the event
 */
int module_manager_broadcast(const event_t *event);
```

### Statistics and Debug API

```c
/**
 * @brief Get Module Manager statistics
 * @param stats Output statistics structure
 */
void module_manager_get_stats(module_mgr_stats_t *stats);

/**
 * @brief Reset statistics
 */
void module_manager_reset_stats(void);

/**
 * @brief Print module information to console
 */
void module_manager_dump_info(void);

/**
 * @brief Register module event callback
 * @param callback Callback function
 * @param user_data User data
 */
void module_manager_set_callback(module_mgr_callback_t callback,
                                  void *user_data);
```

### Compatibility Layer API

`module_manager_compat.h` provides a thin `module_compat_*` wrapper that maps to the standard `module_manager_*` API (historical PRO backends are outside this repository; this header no longer switches to closed-source implementations).

```c
#include <zeplod/module_manager_compat.h>

module_compat_config_t cfg = {
    .max_modules = 16,
    .max_dependencies = 4,
    .enable_auto_deps = false,
    .enable_hotplug = false,
    .enable_lifecycle_hooks = false,
    .enable_health_monitor = false,
};

module_compat_init(&cfg);
module_compat_start();
```

See `include/zeplod/module_manager_compat.h` for the full API list.

### Helper Macros

Consistent with implementation in **`module_base.h`**; parameter name is **`mod_name`** (module prefix), **must not** use `name`, otherwise conflicts with struct field `.name` during macro expansion.

```c
// Version number encoding/decoding
#define MODULE_VERSION(major, minor, patch) \
    (((major) << 16) | ((minor) << 8) | (patch))

#define MODULE_VERSION_MAJOR(v)  (((v) >> 16) & 0xFF)
#define MODULE_VERSION_MINOR(v)  (((v) >> 8) & 0xFF)
#define MODULE_VERSION_PATCH(v)  ((v) & 0xFF)

// Declare complete module interface (requires all callback functions)
#define DECLARE_MODULE_INTERFACE(mod_name) \
    extern const module_interface_t mod_name##_interface; \
    const module_interface_t mod_name##_interface = { \
        .name = #mod_name, \
        .version = MODULE_VERSION(1, 0, 0), \
        .priority = MODULE_PRIORITY_NORMAL, \
        .depends_on = NULL, \
        .init = mod_name##_init, \
        .start = mod_name##_start, \
        .stop = mod_name##_stop, \
        .shutdown = mod_name##_shutdown, \
        .on_event = mod_name##_on_event, \
        .get_status = mod_name##_get_status, \
        .control = mod_name##_control \
    }

#define DECLARE_MODULE_INTERFACE_WITH_DEPS(mod_name, deps_array) \
    extern const module_interface_t mod_name##_interface; \
    const module_interface_t mod_name##_interface = { \
        .name = #mod_name, \
        .version = MODULE_VERSION(1, 0, 0), \
        .priority = MODULE_PRIORITY_NORMAL, \
        .depends_on = (deps_array), \
        .init = mod_name##_init, \
        .start = mod_name##_start, \
        .stop = mod_name##_stop, \
        .shutdown = mod_name##_shutdown, \
        .on_event = mod_name##_on_event, \
        .get_status = mod_name##_get_status, \
        .control = mod_name##_control \
    }

// Declare minimal module interface (optional callbacks can be NULL)
#define DECLARE_MODULE_INTERFACE_MINIMAL(mod_name) \
    extern const module_interface_t mod_name##_interface; \
    const module_interface_t mod_name##_interface = { \
        .name = #mod_name, \
        .version = MODULE_VERSION(1, 0, 0), \
        .priority = MODULE_PRIORITY_NORMAL, \
        .depends_on = NULL, \
        .init = mod_name##_init, \
        .start = mod_name##_start, \
        .stop = mod_name##_stop, \
        .shutdown = NULL, \
        .on_event = mod_name##_on_event, \
        .get_status = NULL, \
        .control = NULL \
    }

#define DECLARE_MODULE_INTERFACE_MINIMAL_WITH_DEPS(mod_name, deps_array) \
    extern const module_interface_t mod_name##_interface; \
    const module_interface_t mod_name##_interface = { \
        .name = #mod_name, \
        .version = MODULE_VERSION(1, 0, 0), \
        .priority = MODULE_PRIORITY_NORMAL, \
        .depends_on = (deps_array), \
        .init = mod_name##_init, \
        .start = mod_name##_start, \
        .stop = mod_name##_stop, \
        .shutdown = NULL, \
        .on_event = mod_name##_on_event, \
        .get_status = NULL, \
        .control = NULL \
    }
```

Usage remains the same, e.g., `DECLARE_MODULE_INTERFACE(my_module);`, `DECLARE_MODULE_INTERFACE_MINIMAL_WITH_DEPS(foo, foo_deps);`.

---

## Usage Guide

### Quick Start

#### 1. Create a Module

**Module Header (my_module.h)**:

```c
#ifndef MY_MODULE_H
#define MY_MODULE_H

#include <zeplod/module_base.h>

// Module configuration structure
typedef struct {
    int param1;
    int param2;
} my_module_config_t;

// Module interface function declarations
int my_module_init(void *config);
int my_module_start(void);
int my_module_stop(void);
int my_module_shutdown(void);
void my_module_on_event(const event_t *event, void *user_data);
module_status_t my_module_get_status(void);
int my_module_control(int cmd, void *arg);

// Module-specific API
int my_module_do_something(void);

#endif /* MY_MODULE_H */
```

**Module Implementation (my_module.c)**:

```c
#include <zeplod/my_module.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(my_module, LOG_LEVEL_INF);

// Module static data
static my_module_config_t g_config;
static module_status_t g_status = MODULE_STATUS_UNINITIALIZED;

// Initialization
int my_module_init(void *config)
{
    LOG_INF("Initializing my_module");

    if (config == NULL) {
        return -1;
    }

    // Copy configuration
    memcpy(&g_config, config, sizeof(g_config));

    // Perform initialization operations
    // ...

    g_status = MODULE_STATUS_INITIALIZED;
    LOG_INF("my_module initialized with param1=%d, param2=%d",
            g_config.param1, g_config.param2);

    return 0;
}

// Startup
int my_module_start(void)
{
    LOG_INF("Starting my_module");

    // Startup operations
    // ...

    g_status = MODULE_STATUS_RUNNING;
    LOG_INF("my_module started");
    return 0;
}

// Stop
int my_module_stop(void)
{
    LOG_INF("Stopping my_module");

    // Stop operations
    // ...

    g_status = MODULE_STATUS_STOPPED;
    LOG_INF("my_module stopped");
    return 0;
}

// Shutdown
int my_module_shutdown(void)
{
    LOG_INF("Shutting down my_module");

    // Cleanup resources
    // ...

    g_status = MODULE_STATUS_UNINITIALIZED;
    LOG_INF("my_module shutdown complete");
    return 0;
}

// Event handling
void my_module_on_event(const event_t *event, void *user_data)
{
    if (event == NULL) {
        return;
    }

    LOG_INF("my_module received event: type=%d", event->type);

    // Handle based on event type
    switch (event->type) {
    case 100:
        // Handle specific event
        break;
    default:
        break;
    }
}

// Get status
module_status_t my_module_get_status(void)
{
    return g_status;
}

// Control command
int my_module_control(int cmd, void *arg)
{
    switch (cmd) {
    case 0:
        // Command 0 handling
        break;
    default:
        return -1;
    }
    return 0;
}

// Module-specific functionality
int my_module_do_something(void)
{
    if (g_status != MODULE_STATUS_RUNNING) {
        return -1;
    }
    // Perform functionality
    return 0;
}

// Declare module interface
DECLARE_MODULE_INTERFACE(my_module);
```

#### 2. Register and Use the Module

**Main firmware**: Calls to `module_manager_init()` and **`module_manager_register()`** occur during **`SYS_INIT(POST_KERNEL, …)`** stage (see "Application Startup and Initialization Order" above); the following code demonstrates **manual order**, suitable for understanding APIs or **`tests/`** scenarios that don't link `app_main.c`.

```c
#include <zeplod/module_manager.h>
#include <zeplod/my_module.h>

void app_main(void)
{
    // 1. Initialize Module Manager
    int ret = module_manager_init();
    if (ret != 0) {
        LOG_ERR("Failed to init module manager");
        return;
    }

    // 2. Prepare module configuration
    my_module_config_t config = {
        .param1 = 10,
        .param2 = 20
    };

    // 3. Register module
    uint32_t module_id;
    ret = module_manager_register(&my_module_interface,
                                   &config,
                                   &module_id);
    if (ret != 0) {
        LOG_ERR("Failed to register module");
        return;
    }
    LOG_INF("Module registered with ID: %u", module_id);

    // 4. Start Module Manager
    ret = module_manager_start();
    if (ret != 0) {
        LOG_ERR("Failed to start module manager");
        return;
    }

    // 5. Start module (can start individually or start all)
    ret = module_manager_start_module(module_id);
    // Or: module_manager_start_all();

    // 6. Subscribe to events
    ret = module_manager_subscribe(module_id, EVENT_TYPE_SENSOR_DATA);
    if (ret != 0) {
        LOG_WRN("Failed to subscribe to event");
    }

    // 7. Use module functionality
    my_module_do_something();

    // 8. Send event to module
    event_t event = {
        .type = EVENT_TYPE_GENERIC,
        .priority = EVENT_PRIORITY_NORMAL,
        .data = NULL,
        .data_len = 0
    };
    module_manager_send_to_module(module_id, &event);

    // 9. Broadcast event
    module_manager_broadcast(&event);
}
```

### Complete Example: GPIO Module

#### Header File (example_module_gpio.h)

```c
#ifndef EXAMPLE_MODULE_GPIO_H
#define EXAMPLE_MODULE_GPIO_H

#include <zeplod/module_base.h>
#include <zephyr/drivers/gpio.h>
#include <stdint.h>
#include <stdbool.h>

// GPIO module configuration
typedef struct {
    const struct device *led_port;
    gpio_pin_t led_pin;
    const struct device *button_port;
    gpio_pin_t button_pin;
    uint32_t blink_interval_ms;
} example_module_gpio_config_t;

// Module interface functions
int example_module_gpio_init(void *config);
int example_module_gpio_start(void);
int example_module_gpio_stop(void);
int example_module_gpio_shutdown(void);
void example_module_gpio_on_event(const event_t *event, void *user_data);
module_status_t example_module_gpio_get_status(void);
int example_module_gpio_control(int cmd, void *arg);

// GPIO module-specific API
int example_module_gpio_set_led(bool on);
bool example_module_gpio_toggle_led(void);
bool example_module_gpio_get_button(void);
int example_module_gpio_set_blink_interval(uint32_t interval_ms);

// Control command definitions
#define GPIO_CMD_SET_BLINK_INTERVAL  0
#define GPIO_CMD_GET_BLINK_INTERVAL  1

#endif /* EXAMPLE_MODULE_GPIO_H */
```

#### Implementation File (example_module_gpio.c)

```c
#include <zeplod/example_module_gpio.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(example_module_gpio, LOG_LEVEL_INF);

// Module private data
typedef struct {
    example_module_gpio_config_t config;
    module_status_t status;
    bool led_state;
    struct k_timer blink_timer;
    uint32_t blink_count;
} gpio_module_data_t;

static gpio_module_data_t g_gpio_data;

// Timer callback
static void blink_timer_handler(struct k_timer *timer)
{
    example_module_gpio_toggle_led();
    g_gpio_data.blink_count++;
}

// Initialization
int example_module_gpio_init(void *config)
{
    LOG_INF("Initializing GPIO module");

    if (config == NULL) {
        return -EINVAL;
    }

    memcpy(&g_gpio_data.config, config, sizeof(g_gpio_data.config));
    g_gpio_data.status = MODULE_STATUS_INITIALIZING;
    g_gpio_data.led_state = false;
    g_gpio_data.blink_count = 0;

    // Initialize LED pin
    if (g_gpio_data.config.led_port != NULL) {
        int ret = gpio_pin_configure(g_gpio_data.config.led_port,
                                      g_gpio_data.config.led_pin,
                                      GPIO_OUTPUT_ACTIVE);
        if (ret < 0) {
            LOG_ERR("Failed to configure LED pin");
            g_gpio_data.status = MODULE_STATUS_ERROR;
            return ret;
        }
    }

    // Initialize button pin
    if (g_gpio_data.config.button_port != NULL) {
        int ret = gpio_pin_configure(g_gpio_data.config.button_port,
                                      g_gpio_data.config.button_pin,
                                      GPIO_INPUT | GPIO_PULL_UP);
        if (ret < 0) {
            LOG_ERR("Failed to configure button pin");
            g_gpio_data.status = MODULE_STATUS_ERROR;
            return ret;
        }
    }

    // Initialize timer
    k_timer_init(&g_gpio_data.blink_timer, blink_timer_handler, NULL);

    g_gpio_data.status = MODULE_STATUS_INITIALIZED;
    LOG_INF("GPIO module initialized");
    return 0;
}

// Startup
int example_module_gpio_start(void)
{
    LOG_INF("Starting GPIO module");

    if (g_gpio_data.config.blink_interval_ms > 0) {
        k_timer_start(&g_gpio_data.blink_timer,
                      K_MSEC(g_gpio_data.config.blink_interval_ms),
                      K_MSEC(g_gpio_data.config.blink_interval_ms));
    }

    g_gpio_data.status = MODULE_STATUS_RUNNING;
    LOG_INF("GPIO module started");
    return 0;
}

// Stop
int example_module_gpio_stop(void)
{
    LOG_INF("Stopping GPIO module");

    k_timer_stop(&g_gpio_data.blink_timer);

    g_gpio_data.status = MODULE_STATUS_STOPPED;
    LOG_INF("GPIO module stopped");
    return 0;
}

// Shutdown
int example_module_gpio_shutdown(void)
{
    LOG_INF("Shutting down GPIO module");

    k_timer_stop(&g_gpio_data.blink_timer);

    // Turn off LED
    if (g_gpio_data.config.led_port != NULL) {
        gpio_pin_set(g_gpio_data.config.led_port,
                     g_gpio_data.config.led_pin, 0);
    }

    g_gpio_data.status = MODULE_STATUS_UNINITIALIZED;
    LOG_INF("GPIO module shutdown complete");
    return 0;
}

// Event handling
void example_module_gpio_on_event(const event_t *event, void *user_data)
{
    if (event == NULL) {
        return;
    }

    LOG_DBG("GPIO module received event: type=%d", event->type);

    switch (event->type) {
    case EVENT_TYPE_SENSOR_CONFIG:
        // Handle configuration event
        if (event->data != NULL && event->data_len > 0) {
            uint8_t *data = (uint8_t *)event->data;
            // Parse and apply configuration
        }
        break;

    default:
        break;
    }
}

// Get status
module_status_t example_module_gpio_get_status(void)
{
    return g_gpio_data.status;
}

// Control command
int example_module_gpio_control(int cmd, void *arg)
{
    switch (cmd) {
    case GPIO_CMD_SET_BLINK_INTERVAL:
        if (arg == NULL) {
            return -EINVAL;
        }
        g_gpio_data.config.blink_interval_ms = *(uint32_t *)arg;
        return 0;

    case GPIO_CMD_GET_BLINK_INTERVAL:
        if (arg == NULL) {
            return -EINVAL;
        }
        *(uint32_t *)arg = g_gpio_data.config.blink_interval_ms;
        return 0;

    default:
        return -ENOTSUP;
    }
}

// LED control
int example_module_gpio_set_led(bool on)
{
    if (g_gpio_data.status != MODULE_STATUS_RUNNING) {
        return -EPERM;
    }

    if (g_gpio_data.config.led_port != NULL) {
        gpio_pin_set(g_gpio_data.config.led_port,
                     g_gpio_data.config.led_pin, on ? 1 : 0);
        g_gpio_data.led_state = on;
        return 0;
    }
    return -ENODEV;
}

// Toggle LED state
bool example_module_gpio_toggle_led(void)
{
    bool new_state = !g_gpio_data.led_state;
    example_module_gpio_set_led(new_state);
    return new_state;
}

// Read button
bool example_module_gpio_get_button(void)
{
    if (g_gpio_data.config.button_port != NULL) {
        return gpio_pin_get(g_gpio_data.config.button_port,
                            g_gpio_data.config.button_pin);
    }
    return false;
}

// Set blink interval
int example_module_gpio_set_blink_interval(uint32_t interval_ms)
{
    g_gpio_data.config.blink_interval_ms = interval_ms;

    if (g_gpio_data.status == MODULE_STATUS_RUNNING) {
        if (interval_ms > 0) {
            k_timer_start(&g_gpio_data.blink_timer,
                          K_MSEC(interval_ms),
                          K_MSEC(interval_ms));
        } else {
            k_timer_stop(&g_gpio_data.blink_timer);
        }
    }
    return 0;
}

// Declare module interface
DECLARE_MODULE_INTERFACE(example_module_gpio);
```

#### Usage Example

```c
#include <zeplod/module_manager.h>
#include <zeplod/example_module_gpio.h>

void gpio_module_example(void)
{
    // Initialize Module Manager
    module_manager_init();
    module_manager_start();

    // Configure GPIO module
    example_module_gpio_config_t gpio_config = {
        .led_port = DEVICE_DT_GET(DT_NODELABEL(led0)),
        .led_pin = 0,
        .button_port = DEVICE_DT_GET(DT_NODELABEL(sw0)),
        .button_pin = 0,
        .blink_interval_ms = 500
    };

    // Register module
    uint32_t gpio_module_id;
    module_manager_register(&example_module_gpio_interface,
                            &gpio_config,
                            &gpio_module_id);

    // Start module
    module_manager_start_module(gpio_module_id);

    // Subscribe to events
    module_manager_subscribe(gpio_module_id, EVENT_TYPE_SENSOR_DATA);

    // Use module API
    example_module_gpio_set_led(true);
    example_module_gpio_toggle_led();

    // Modify configuration via control command
    uint32_t new_interval = 1000;
    example_module_gpio_control(GPIO_CMD_SET_BLINK_INTERVAL, &new_interval);

    // Get module information
    module_info_t info;
    module_manager_get_module_info(gpio_module_id, &info);
    LOG_INF("Module: %s, Status: %d", info.interface->name, info.status);

    // Print all module information
    module_manager_dump_info();

    // Get statistics
    module_mgr_stats_t stats;
    module_manager_get_stats(&stats);
    LOG_INF("Total modules: %u, Active: %u",
            stats.total_modules, stats.active_modules);
}
```

### Inter-Module Communication

```c
// Module A sends event to Module B
void module_a_send_to_module_b(void)
{
    uint32_t module_b_id = module_manager_get_id_by_name("module_b");

    event_t event = {
        .type = EVENT_TYPE_GENERIC,
        .priority = EVENT_PRIORITY_NORMAL,
        .source_id = g_my_module_id,
        .reserved = 0,
        .data.ptr = &some_data,
        .data_len = sizeof(some_data)
    };

    module_manager_send_to_module(module_b_id, &event);
}

// Broadcast event to all modules
void broadcast_to_all_modules(void)
{
    event_t event = {
        .type = EVENT_TYPE_SENSOR_DATA,
        .priority = EVENT_PRIORITY_HIGH,
        .reserved = 0,
        .data.ptr = &sensor_data,
        .data_len = sizeof(sensor_data)
    };

    int count = module_manager_broadcast(&event);
    LOG_INF("Event sent to %d modules", count);
}
```

### Module Iteration

```c
// Callback function
void print_module_info(module_info_t *info, void *user_data)
{
    LOG_INF("Module: %s, ID: %u, Status: %d",
            info->interface->name, info->id, info->status);
}

// Iterate through all modules
void list_all_modules(void)
{
    module_manager_foreach(print_module_info, NULL);
}
```

---

## Best Practices

### 1. Module Design Principles

```c
// ✅ Recommended: Modular design, single responsibility
typedef struct {
    // Only necessary configuration
    int param;
} simple_module_config_t;

// ❌ Avoid: Module too large
typedef struct {
    // Too many configuration items
    int param1, param2, ..., param50;
} complex_module_config_t;
```

### 2. Initialization Notes

```c
// ✅ Recommended: Quick initialization, delayed startup
int my_module_init(void *config)
{
    // Only do necessary initialization
    // Do not start threads, timers, etc.
    return 0;
}

int my_module_start(void)
{
    // Start threads, timers, etc. here
}

// ❌ Avoid: Doing too much in init
int bad_init(void *config)
{
    k_thread_create(...);  // Should be in start
    k_timer_start(...);    // Should be in start
    return 0;
}
```

### 3. Event Handling Best Practices

```c
// ✅ Recommended: Fast event processing
void my_module_on_event(const event_t *event, void *user_data)
{
    // Quickly copy data
    memcpy(&local_data, event->data, MIN(event->data_len, sizeof(local_data)));

    // Trigger semaphore, let worker thread handle
    k_sem_give(&work_sem);
}

// ❌ Avoid: Executing time-consuming operations in event callbacks
void bad_on_event(const event_t *event, void *user_data)
{
    k_msleep(1000);  // Blocks event dispatch
    complex_calculation();  // Time-consuming calculation
}
```

### 4. Error Handling

```c
// ✅ Recommended: Complete error handling
int my_module_init(void *config)
{
    if (config == NULL) {
        g_status = MODULE_STATUS_ERROR;
        return -EINVAL;
    }

    int ret = some_init_operation();
    if (ret < 0) {
        g_status = MODULE_STATUS_ERROR;
        LOG_ERR("Init failed: %d", ret);
        return ret;
    }

    g_status = MODULE_STATUS_INITIALIZED;
    return 0;
}

// ❌ Avoid: Ignoring errors
int bad_init(void *config)
{
    some_init_operation();  // Not checking return value
    g_status = MODULE_STATUS_INITIALIZED;
    return 0;
}
```

### 5. Resource Management

```c
// ✅ Recommended: Complete resource cleanup
int my_module_shutdown(void)
{
    // Stop all activities
    k_timer_stop(&timer);
    k_thread_abort(&thread);

    // Release resources
    k_free(dynamic_memory);

    // Reset status
    g_status = MODULE_STATUS_UNINITIALIZED;
    return 0;
}
```

### 6. Module Priority Settings

```c
// Critical module (starts first)
const module_interface_t sensor_interface = {
    .name = "sensor",
    .priority = MODULE_PRIORITY_CRITICAL,  // Sensor starts first
    // ...
};

// Normal module
const module_interface_t logger_interface = {
    .name = "logger",
    .priority = MODULE_PRIORITY_NORMAL,
    // ...
};

// Low priority module (starts last)
const module_interface_t stats_interface = {
    .name = "statistics",
    .priority = MODULE_PRIORITY_LOW,
    // ...
};
```

---

## Troubleshooting

### Common Issues

#### 1. Module Registration Failure

**Symptom**: `module_manager_register` returns error

**Checklist**:
- [ ] Is the module interface correctly defined?
- [ ] Is the maximum module count exceeded?
- [ ] Did the init function timeout?

```c
// Debug steps
void debug_register_failure(void)
{
    module_mgr_stats_t stats;
    module_manager_get_stats(&stats);

    LOG_INF("Total: %u, Max: %d",
            stats.total_modules, CONFIG_MAX_MODULES);

    if (stats.error_modules > 0) {
        LOG_ERR("Some modules failed to initialize");
    }
}
```

#### 2. Module Startup Failure

**Symptom**: `module_manager_start_module` returns error

**Possible causes**:
- Module status is not INITIALIZED or STOPPED
- start function returned an error

```c
// Check module status
void check_module_state(uint32_t module_id)
{
    module_info_t info;
    if (module_manager_get_module_info(module_id, &info) == 0) {
        LOG_INF("Module %s status: %d", info.interface->name, info.status);
    }
}
```

#### 3. Event Not Delivered to Module

**Symptom**: Module did not receive subscribed event

**Checklist**:
- [ ] Has the module started (RUNNING status)?
- [ ] Is it correctly subscribed to the event type?
- [ ] Is the on_event callback non-NULL?

```c
// Check subscription status
void check_subscription(uint32_t module_id, event_type_t event_type)
{
    module_info_t info;
    module_manager_get_module_info(module_id, &info);

    LOG_INF("Module %s subscriptions: %u",
            info.interface->name, info.event_subscription_count);

    for (uint8_t i = 0; i < info.event_subscription_count; i++) {
        if (info.event_subscriptions[i].type == event_type) {
            LOG_INF("Subscribed to event type %u", event_type);
            return;
        }
    }
    LOG_WRN("Not subscribed to event type %u", event_type);
}
```

#### 4. Module System Pitfalls

**Symptom**: Abnormal system behavior (hang, status error, crash)

**Pitfall 1: State Machine Trap**

```c
// ❌ Wrong example - Illegal state machine transition
int bad_init(void *config)
{
    // Calling stop in init triggers illegal state transition
    module_manager_stop(my_module_id);
    return 0;
}

// ✅ Correct example
int good_init(void *config)
{
    // Only do initialization, do not operate on state machine
    return 0;
}
```

**Pitfall 2: Dependency Trap**

If Module A depends on Module B, but runtime dependencies are not correctly declared, stopping Module B alone will leave dependent downstream modules dangling:

```c
// Correct approach: Declare dependency relationship
static const char * const my_deps[] = {"other_module", NULL};
DECLARE_MODULE_INTERFACE_WITH_DEPS(my_module, my_deps);
```

**Pitfall 3: Callback Reentry Trap**

Calling `module_manager_register` or subscribing to new events within a module event callback chain may trigger internal list capacity expansion (realloc), causing iterator invalidation:

```c
// ❌ Dangerous example
static void my_module_on_event(const event_t *event, void *user_data)
{
    if (some_condition) {
        // Dangerous: may be called during list expansion
        event_subscribe(OTHER_EVENT, another_callback, NULL, &sub_id);
    }
}
```

### Error Code Reference

| Error Code | Description |
|------------|-------------|
| 0 | Success |
| -1 | General error/not found |
| -EINVAL | Invalid argument |
| -EPERM | Permission error (incorrect status) |
| -ENOTSUP | Unsupported operation |
| -ENODEV | Device does not exist |
| -ENOMEM | Out of memory |

### Debug Tips

```c
// Print module information
void debug_modules(void)
{
    module_manager_dump_info();
}

// Print statistics
void debug_stats(void)
{
    module_mgr_stats_t stats;
    module_manager_get_stats(&stats);

    LOG_INF("=== Module Manager Stats ===");
    LOG_INF("Total modules: %u", stats.total_modules);
    LOG_INF("Active modules: %u", stats.active_modules);
    LOG_INF("Error modules: %u", stats.error_modules);
    LOG_INF("Events processed: %u", stats.events_processed);
    LOG_INF("Events dropped: %u", stats.events_dropped);
}
```

---

## Appendix

### Header File Includes

```c
#include <zeplod/module_manager.h>  // Module Manager API
#include <zeplod/module_base.h>     // Module base interface and macros
```

### Example Modules

The project includes the following example modules:
- `example_module_gpio` - GPIO control module
- `example_module_uart` - UART communication module
- `example_module_a` - General purpose example module
- `example_module_b` - General purpose example module

### Related Documentation

- [Event System Detailed Usage Guide.md](31-event-system-guide.md)
- [Developer Getting Started Guide.md](../00-getting-started/04-developer-guide.md)
- [Zephyr RTOS Official Documentation](https://docs.zephyrproject.org/)
