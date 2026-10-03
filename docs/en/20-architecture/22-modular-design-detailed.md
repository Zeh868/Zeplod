> Language: [中文](../../zh-CN/20-架构设计/22-模块化软件设计的详细方法.md) | **English**

# Detailed Modular Software Design Methods

## Table of Contents

- [Overview](#overview)
- [Core Principles](#core-principles)
- [Module Partitioning Methods](#module-partitioning-methods)
- [Interface Design Specifications](#interface-design-specifications)
- [Dependency Management Strategy](#dependency-management-strategy)
- [Lifecycle Management](#lifecycle-management)
- [Inter-Module Communication Mechanisms](#inter-module-communication-mechanisms)
- [Layered Architecture Design](#layered-architecture-design)
- [Error Handling and Fault Tolerance](#error-handling-and-fault-tolerance)
- [Testing Strategy](#testing-strategy)
- [Best Practices and Anti-Patterns](#best-practices-and-anti-patterns)
- [Case Studies](#case-studies)

---

## Overview

Modular software design is a software architecture method that decomposes complex systems into independent, replaceable, reusable modules. Good modular design significantly improves code maintainability, testability, and extensibility.

### What is a Module

A **module** is a software unit with clearly defined interfaces that encapsulates specific functionality implementations, provides services externally, and hides internal details.

```
┌─────────────────────────────────────────────────────┐
│                      Module                           │
│  ┌───────────────────────────────────────────────┐  │
│  │           Public Interface                     │  │
│  │   - init()  - start()  - stop()  - control()   │  │
│  └───────────────────────────────────────────────┘  │
│                                                      │
│  ┌───────────────────────────────────────────────┐  │
│  │           Internal Implementation               │  │
│  │   - Private data structures                   │  │
│  │   - Internal functions                         │  │
│  │   - State machine                             │  │
│  │   - Resource management                       │  │
│  └───────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
```

### Core Values of Modularization

| Value | Description |
|-------|-------------|
| **Encapsulation** | Hide implementation details, expose only necessary interfaces |
| **Independence** | Modules can be independently developed, tested, deployed |
| **Replaceability** | Modules with same interface can replace each other |
| **Reusability** | Modules can be reused in different projects |
| **Maintainability** | Modifications don't affect other modules |

---

## Core Principles

### 1. Single Responsibility Principle (SRP)

Each module should have only one reason for change, i.e., be responsible for only one thing.

```
❌ Wrong Example: Responsibilities mixed

┌─────────────────────────────────┐
│        DeviceManager            │
│  - Device initialization        │
│  - Data collection              │
│  - Data storage                 │
│  - Network communication        │
│  - UI display                   │
└─────────────────────────────────┘


✅ Correct Example: Responsibilities separated

┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│ DeviceDriver│  │ DataCollector│  │ DataStorage │
│  - init     │  │  - collect  │  │  - store    │
│  - config   │  │  - filter   │  │  - read     │
└─────────────┘  └─────────────┘  └─────────────┘
       │                │                │
       └────────────────┼────────────────┘
                        │
              ┌─────────▼─────────┐
              │   NetworkManager  │
              │    - transmit    │
              └───────────────────┘
```

### 2. High Cohesion + Low Coupling

**High Cohesion**: Elements within a module are tightly related, working together to complete a single function.

**Low Coupling**: Dependency between modules is minimized, interfaces are simple and clear.

```
Coupling levels (from low to high):

1. No Coupling      ───  Modules completely independent
2. Data Coupling    ───  Simple data parameter interaction (recommended)
3. Stamp Coupling   ───  Shared complex data structures
4. Control Coupling ───  Pass control information
5. External Coupling ───  Shared external devices/protocols
6. Common Coupling  ───  Shared global data
7. Content Coupling ───  Directly access each other's internals (prohibited)
```

### 3. Interface Segregation Principle (ISP)

Modules should not be forced to depend on methods they don't use. Large interfaces should be split into multiple smaller ones.

```c
// ❌ Violating ISP: Bloated interface
typedef struct {
    int (*init)(void);
    int (*read)(void* buf, size_t len);
    int (*write)(const void* buf, size_t len);
    int (*seek)(long offset);
    int (*ioctl)(int cmd, void* arg);
    int (*flush)(void);
    int (*truncate)(size_t len);
} file_interface_t;

// ✅ Complying with ISP: Interface segregation
typedef struct {
    int (*init)(void);
    int (*read)(void* buf, size_t len);
} reader_interface_t;

typedef struct {
    int (*init)(void);
    int (*write)(const void* buf, size_t len);
    int (*flush)(void);
} writer_interface_t;

typedef struct {
    int (*seek)(long offset);
    int (*truncate)(size_t len);
} seekable_interface_t;
```

### 4. Dependency Inversion Principle (DIP)

High-level modules should not depend on low-level modules; both should depend on abstractions.

```
❌ Direct dependency:

┌───────────────┐
│ Application   │ ──────depends on─────▶ ┌─────────────┐
└───────────────┘                        │ SensorDriver │
                                        └─────────────┘


✅ Dependency inversion:

┌───────────────┐                      ┌───────────────┐
│ Application   │ ───depends on──▶ ┌─────▶│ SensorDriver  │
└───────────────┘           │        └───────────────┘
                            │
                    ┌───────▼────────┐
                    │ ISensor        │  (Abstract interface)
                    │ - read()       │
                    │ - configure()  │
                    └───────┬────────┘
                            │
                    ┌───────▼────────┐
                    │ TemperatureSensor │
                    └──────────────────┘
```

### 5. Open-Closed Principle (OCP)

Modules should be open for extension but closed for modification.

```c
// ✅ Extend through interfaces without modifying existing code

// Define sensor interface
typedef struct {
    int (*read)(float* value);
    int (*configure)(const void* config);
} sensor_interface_t;

// Temperature sensor implementation
static int temp_sensor_read(float* value) { /* ... */ }
static int temp_sensor_configure(const void* config) { /* ... */ }

const sensor_interface_t temp_sensor = {
    .read = temp_sensor_read,
    .configure = temp_sensor_configure
};

// Humidity sensor implementation (added, no need to modify original code)
static int humidity_sensor_read(float* value) { /* ... */ }
static int humidity_sensor_configure(const void* config) { /* ... */ }

const sensor_interface_t humidity_sensor = {
    .read = humidity_sensor_read,
    .configure = humidity_sensor_configure
};
```

---

## Module Partitioning Methods

### Partition by Functional Responsibility

The most common partitioning method, determining module boundaries based on functional boundaries.

```
Smart Home System Example:

├── core/                    # Core layer
│   ├── event_system/       # Event system
│   └── module_manager/     # Module manager
│
├── services/               # System service layer
│   ├── sys_log/           # Logging service
│   ├── sys_timer/         # Timer service
│   └── sys_watchdog/      # Watchdog service
│
├── modules/               # Business module layer
│   ├── sensor_module/     # Sensor module
│   ├── network_module/    # Network module
│   ├── storage_module/    # Storage module
│   └── display_module/    # Display module
│
└── app/                   # Application layer
    ├── app_main.c         # Main program
    └── app_config.h       # Configuration management
```

### Partition by Business Domain

Suitable for complex business applications, organized by domain models.

```
E-commerce System Example:

├── order/                  # Order domain
│   ├── order_manager.c
│   ├── order_validator.c
│   └── order_repository.c
│
├── inventory/             # Inventory domain
│   ├── stock_manager.c
│   └── warehouse_adapter.c
│
├── payment/               # Payment domain
│   ├── payment_processor.c
│   └── payment_gateway.c
│
└── shipping/              # Shipping domain
    ├── shipping_calculator.c
    └── carrier_adapter.c
```

### Partition by Layer

Classic layered architecture, each layer only depends on the layer below.

```
┌─────────────────────────────────────────────────┐
│            Presentation Layer                   │
│          UI, API controllers, protocol parsing    │
├─────────────────────────────────────────────────┤
│            Application Layer                    │
│        Use case orchestration, transaction       │
│        management, event publishing              │
├─────────────────────────────────────────────────┤
│              Domain Layer                       │
│     Business entities, domain services,         │
│     business rule validation                    │
├─────────────────────────────────────────────────┤
│           Infrastructure Layer                  │
│    Data persistence, external service           │
│    integration, message queues                  │
└─────────────────────────────────────────────────┘
```

### Module Granularity Control

```
Too coarse:
┌─────────────────────────────────┐
│          Application            │
│   (All functions together)       │
│   10,000+ lines of code         │
└─────────────────────────────────┘
Problem: Difficult to understand, test, maintain

Too fine:
┌───────┐ ┌───────┐ ┌───────┐ ┌───────┐
│UtilA  │ │UtilB  │ │UtilC  │ │UtilD  │
│ 50LOC │ │ 30LOC │ │ 40LOC │ │ 20LOC │
└───────┘ └───────┘ └───────┘ └───────┘
Problem: Module explosion, complex dependencies, high call overhead

Just right:
┌─────────────┐ ┌─────────────┐ ┌─────────────┐
│ SensorModule│ │ NetModule   │ │ StoreModule │
│   300 LOC   │ │   400 LOC   │ │   350 LOC   │
└─────────────┘ └─────────────┘ └─────────────┘
Recommended: 200-800 lines per module
```

---

## Interface Design Specifications

### Interface Definition Principles

#### 1. Interface and Implementation Separation

```c
// sensor_interface.h - Public interface header
#ifndef SENSOR_INTERFACE_H
#define SENSOR_INTERFACE_H

typedef struct sensor_interface sensor_interface_t;

struct sensor_interface {
    int (*init)(const sensor_interface_t* self, void* config);
    int (*read)(const sensor_interface_t* self, float* value);
    int (*configure)(const sensor_interface_t* self, const void* config);
    void (*destroy)(const sensor_interface_t* self);
};

// Factory functions
const sensor_interface_t* temperature_sensor_create(void);
const sensor_interface_t* humidity_sensor_create(void);

#endif

// temperature_sensor.c - Implementation file
#include "sensor_interface.h"

typedef struct {
    float calibration_offset;
    int sample_count;
} temp_sensor_ctx_t;

static int temp_init(const sensor_interface_t* self, void* config) {
    // Implementation details...
}

static int temp_read(const sensor_interface_t* self, float* value) {
    // Implementation details...
}

static const sensor_interface_t temp_sensor_vtable = {
    .init = temp_init,
    .read = temp_read,
    // ...
};
```

#### 2. Minimal Interface Principle

Only expose necessary interfaces, hide internal implementation.

```c
// ✅ Minimal interface
typedef struct {
    int (*connect)(const char* address, uint16_t port);
    int (*send)(const void* data, size_t len);
    int (*receive)(void* buffer, size_t max_len);
    void (*disconnect)(void);
} network_interface_t;

// ❌ Exposing too many details
typedef struct {
    int (*connect)(const char* address, uint16_t port);
    int (*send)(const void* data, size_t len);
    int (*receive)(void* buffer, size_t max_len);
    void (*disconnect)(void);
    // Internal state exposed
    int socket_fd;
    bool is_connected;
    uint8_t* internal_buffer;
    void (*internal_helper)(void);  // Internal functions shouldn't be exposed
} network_interface_bad_t;
```

#### 3. Interface Version Management

```c
// Version encoding
#define MODULE_VERSION(major, minor, patch) \
    (((major) << 16) | ((minor) << 8) | (patch))

#define MODULE_VERSION_MAJOR(v)  (((v) >> 16) & 0xFF)
#define MODULE_VERSION_MINOR(v)  (((v) >> 8) & 0xFF)
#define MODULE_VERSION_PATCH(v)  ((v) & 0xFF)

// Interface definition
typedef struct {
    const char* name;
    uint32_t version;  // Module version
    uint32_t api_version;  // API compatibility version

    // Function pointers...
} module_interface_t;

// Version compatibility check
static bool check_api_compatibility(uint32_t api_version) {
    // Compatible if major version number is the same
    return MODULE_VERSION_MAJOR(api_version) ==
           MODULE_VERSION_MAJOR(CURRENT_API_VERSION);
}
```

### Module Interface Template

Standard module interface used in this project:

```c
/**
 * @brief Module interface structure (virtual function table)
 */
typedef struct {
    const char*       name;      // Module name
    uint32_t          version;   // Version number
    module_priority_t priority;  // Priority
    const char* const* depends_on; // Dependency list

    // Lifecycle functions
    int (*init)(void* config);    // Initialize
    int (*start)(void);           // Start
    int (*stop)(void);            // Stop
    int (*shutdown)(void);        // Destroy

    // Events and control
    module_event_handler_t on_event;     // Event handling
    module_status_t (*get_status)(void); // Status query
    int (*control)(int cmd, void* arg);  // Control commands
} module_interface_t;
```

---

## Dependency Management Strategy

### Dependency Types

```
1. Compile-time dependency
   - #include header files
   - Link library files

2. Runtime dependency
   - Module A needs Module B to start first
   - Declared through depends_on

3. Configuration-time dependency
   - select/depends on in Kconfig
   - Determines whether to compile a module
```

### Dependency Declaration Methods

#### Kconfig Configuration Dependencies

```kconfig
# Module A depends on networking functionality
config MODULE_A
    bool "Enable Module A"
    depends on NETWORKING
    select EVENT_SYSTEM
    help
      Module A provides advanced features.
      It requires networking support.
```

#### Runtime Dependency Declaration

```c
// Declare dependency array
static const char* const sensor_module_deps[] = {
    "network_module",   // Depends on network module
    "storage_module",   // Depends on storage module
    NULL                // Must end with NULL
};

// Use macro to declare module interface with dependencies
DECLARE_MODULE_INTERFACE_WITH_DEPS(sensor_module, sensor_module_deps);
```

### Dependency Resolution Algorithm

Use topological sorting to determine module startup order:

```
Dependency graph example:
  Application module
      │
      ├──▶ Data processing module
      │        │
      │        ├──▶ Sensor module
      │        │
      │        └──▶ Storage module
      │
      └──▶ Network module
               │
               └──▶ Driver module

Topological sort result:
Startup order: Driver module → Storage module → Sensor module → Network module → Data processing module → Application module
Shutdown order: Application module → Data processing module → Network module → Sensor module → Storage module → Driver module
```

Kahn's algorithm implementation:

```c
/**
 * @brief Topological sort to determine startup order
 */
static int topological_sort(module_context_t** sorted, int* count) {
    int in_degree[MAX_MODULES] = {0};
    int queue[MAX_MODULES];
    int q_head = 0, q_tail = 0;

    // 1. Calculate in-degree
    for (int i = 0; i < module_count; i++) {
        if (modules[i].depends_on) {
            for (int j = 0; modules[i].depends_on[j]; j++) {
                int dep_idx = find_module_by_name(modules[i].depends_on[j]);
                if (dep_idx >= 0) {
                    in_degree[i]++;
                }
            }
        }
    }

    // 2. Enqueue nodes with in-degree of 0
    for (int i = 0; i < module_count; i++) {
        if (in_degree[i] == 0) {
            queue[q_tail++] = i;
        }
    }

    // 3. Process queue
    *count = 0;
    while (q_head < q_tail) {
        int idx = queue[q_head++];
        sorted[(*count)++] = &modules[idx];

        // Reduce in-degree of other modules depending on this one
        for (int i = 0; i < module_count; i++) {
            if (is_dependent(i, modules[idx].name)) {
                if (--in_degree[i] == 0) {
                    queue[q_tail++] = i;
                }
            }
        }
    }

    // 4. Detect circular dependencies
    return (*count == module_count) ? 0 : -1;
}
```

### Circular Dependency Detection and Resolution

```
Circular dependency example:
  A → B → C → A  (closed loop)

Detection methods:
1. Depth-First Search (DFS)
2. Record visited path
3. Repeated visit indicates a cycle

Solutions:
1. Refactor: Extract common parts into independent modules
2. Callbacks: Use dependency injection instead of direct dependencies
3. Mediator: Introduce mediator module for coordination
```

```c
// Use callback functions to break circular dependencies

// ❌ Circular dependency: A and B call each other
// module_a.c
#include "module_b.h"
void module_a_do_work(void) {
    module_b_process();
}

// module_b.c
#include "module_a.h"
void module_b_process(void) {
    module_a_callback();
}

// ✅ Use callbacks to break circular dependency
// module_a.h
typedef void (*a_callback_t)(void* data);
void module_a_set_callback(a_callback_t cb, void* user_data);

// module_b.c
void module_b_init(void) {
    module_a_set_callback(on_a_event, NULL);
}
```

---

## Lifecycle Management

### Module State Machine

```
                    ┌──────────────────┐
                    │ UNINITIALIZED   │
                    └────────┬─────────┘
                             │ register()
                             ▼
                    ┌──────────────────┐
                    │ INITIALIZING    │
                    └────────┬─────────┘
                             │ init() success
                             ▼
                    ┌──────────────────┐
              ┌─────│ INITIALIZED     │─────┐
              │     └────────┬─────────┘     │
              │              │ start()       │ stop()
              │              ▼               │
              │     ┌──────────────────┐     │
              │     │ RUNNING         │◄────┤
              │     │ (running)        │     │
              │     └────────┬─────────┘     │
              │              │ suspend()     │
              │              ▼               │
              │     ┌──────────────────┐     │
              └────►│ SUSPENDED       │─────┘
                    └────────┬─────────┘
                             │ error
                             ▼
                    ┌──────────────────┐
                    │ ERROR           │
                    └──────────────────┘
```

### Lifecycle Callback Implementation

```c
/**
 * @brief Module lifecycle function responsibility division
 */

// init(): Initialization phase
// - Validate configuration parameters
// - Initialize static data structures
// - Don't start threads/timers
// - Don't allocate dynamic memory (recommended for embedded)
int my_module_init(void* config)
{
    // Parameter validation
    if (config == NULL) {
        return -EINVAL;
    }

    // Copy configuration
    memcpy(&g_config, config, sizeof(g_config));

    // Initialize data structures
    g_status = MODULE_STATUS_INITIALIZED;

    return 0;
}

// start(): Startup phase
// - Start worker threads
// - Start timers
// - Register interrupt handlers
// - Begin data processing
int my_module_start(void)
{
    // Start timer
    k_timer_start(&g_work_timer, K_MSEC(100), K_MSEC(100));

    // Start worker thread
    k_thread_start(&g_work_thread);

    g_status = MODULE_STATUS_RUNNING;
    return 0;
}

// stop(): Stop phase
// - Stop data processing
// - Stop timers/threads
// - Keep allocated resources
int my_module_stop(void)
{
    k_timer_stop(&g_work_timer);
    k_thread_suspend(&g_work_thread);

    g_status = MODULE_STATUS_STOPPED;
    return 0;
}

// shutdown(): Destruction phase
// - Release all resources
// - Clean up state
// - Can reinitialize
int my_module_shutdown(void)
{
    // Stop all activities
    my_module_stop();

    // Cleanup resources
    // ...

    g_status = MODULE_STATUS_UNINITIALIZED;
    return 0;
}
```

### Initialization Order Control

Use Zephyr SYS_INIT mechanism to control initialization order:

```c
// app_config.h - Define initialization priorities

// Initialization phase constants (smaller values execute first)
#define APP_INIT_PRIO_CORE         0    // Core initialization
#define APP_INIT_PRIO_SYSTEM       20   // System services
#define APP_INIT_PRIO_MODULE_MGR   40   // Module manager
#define APP_INIT_PRIO_MODULE_BASE  60   // Base modules
#define APP_INIT_PRIO_MODULE_APP   80   // Application modules
#define APP_INIT_PRIO_APP_FINAL    100  // Application completion

// Module auto-registration
static int my_module_auto_register(const struct device* dev)
{
    my_module_config_t config = { /* ... */ };
    uint32_t module_id;

    return module_manager_register(&my_module_interface, &config, &module_id);
}

// Auto-register in POST_KERNEL phase
SYS_INIT(my_module_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_APP);
```

---

## Inter-Module Communication Mechanisms

### Communication Pattern Comparison

| Pattern | Coupling | Real-time | Use Case |
|---------|----------|-----------|----------|
| Direct function call | High | High | Synchronous operations |
| Callback function | Medium | High | Event notification |
| Event system | Low | Medium | Async decoupling |
| Message queue | Low | Medium | Buffered communication |
| Shared memory | High | High | Large data volumes |

### Event-Driven Communication

This project's recommended communication method:

```c
// Publish-subscribe pattern

// 1. Define event types
typedef enum {
    EVENT_TYPE_SENSOR_DATA = 1,
    EVENT_TYPE_NETWORK_STATUS,
    EVENT_TYPE_STORAGE_ERROR,
} event_type_t;

// 2. Define event structure
typedef struct {
    event_type_t type;
    uint8_t priority;
    uint32_t source_id;
    void* data;
    size_t data_len;
} event_t;

// 3. Module subscribes to events
void sensor_module_init(void)
{
    uint32_t module_id = module_manager_get_id_by_name("sensor_module");

    // Subscribe to events of interest
    module_manager_subscribe(module_id, EVENT_TYPE_NETWORK_STATUS);
    module_manager_subscribe(module_id, EVENT_TYPE_STORAGE_ERROR);
}

// 4. Module handles events
void sensor_module_on_event(const event_t* event, void* user_data)
{
    switch (event->type) {
    case EVENT_TYPE_NETWORK_STATUS:
        // Handle network status change
        handle_network_change(event->data);
        break;

    case EVENT_TYPE_STORAGE_ERROR:
        // Handle storage error
        handle_storage_error(event->data);
        break;
    }
}

// 5. Publish events
void network_module_notify_status(bool connected)
{
    event_t event = {
        .type = EVENT_TYPE_NETWORK_STATUS,
        .priority = EVENT_PRIORITY_HIGH,
        .reserved = 0,
        .data.ptr = &connected,
        .data_len = sizeof(connected)
    };

    module_manager_broadcast(&event);
}
```

### Callback Mechanism

```c
// Define callback types
typedef void (*data_callback_t)(const void* data, size_t len, void* user_data);

// Module registers callback
typedef struct {
    data_callback_t on_data;
    void* user_data;
} callback_entry_t;

static callback_entry_t g_callbacks[MAX_CALLBACKS];

int register_data_callback(data_callback_t cb, void* user_data)
{
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (g_callbacks[i].on_data == NULL) {
            g_callbacks[i].on_data = cb;
            g_callbacks[i].user_data = user_data;
            return i;
        }
    }
    return -ENOMEM;
}

// Trigger callback
void notify_data_received(const void* data, size_t len)
{
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        if (g_callbacks[i].on_data) {
            // Note: Call outside lock to avoid deadlock
            g_callbacks[i].on_data(data, len, g_callbacks[i].user_data);
        }
    }
}
```

### Shared Data Pattern

```c
// Use read-write lock to protect shared data

typedef struct {
    struct k_mutex lock;
    sensor_data_t data;
    bool valid;
} shared_sensor_data_t;

static shared_sensor_data_t g_shared_data;

// Write (producer)
int write_sensor_data(const sensor_data_t* data)
{
    k_mutex_lock(&g_shared_data.lock, K_FOREVER);
    memcpy(&g_shared_data.data, data, sizeof(sensor_data_t));
    g_shared_data.valid = true;
    k_mutex_unlock(&g_shared_data.lock);
    return 0;
}

// Read (consumer)
int read_sensor_data(sensor_data_t* data)
{
    k_mutex_lock(&g_shared_data.lock, K_FOREVER);
    if (g_shared_data.valid) {
        memcpy(data, &g_shared_data.data, sizeof(sensor_data_t));
    }
    k_mutex_unlock(&g_shared_data.lock);
    return g_shared_data.valid ? 0 : -1;
}
```

---

## Layered Architecture Design

### Standard Layered Model

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                         │
│           Business logic orchestration, user interaction     │
│                                                              │
│  Modules: app_main, app_config, app_version                │
└────────────────────────────┬────────────────────────────────┘
                             │
┌────────────────────────────▼────────────────────────────────┐
│                   Business Modules Layer                    │
│              Independent business function units             │
│                                                              │
│  Modules: sensor_module, network_module, storage_module     │
└────────────────────────────┬────────────────────────────────┘
                             │
┌────────────────────────────▼────────────────────────────────┐
│                   System Services Layer                      │
│              General system services, infrastructure         │
│                                                              │
│  Modules: sys_log, sys_timer, sys_memory, sys_watchdog     │
└────────────────────────────┬────────────────────────────────┘
                             │
┌────────────────────────────▼────────────────────────────────┐
│                    Core Framework Layer                      │
│              Module management, event system, core           │
│              abstractions                                   │
│                                                              │
│  Modules: module_manager, event_system, event_dispatcher   │
└────────────────────────────┬────────────────────────────────┘
                             │
┌────────────────────────────▼────────────────────────────────┐
│                    Hardware Abstraction Layer (HAL/RTOS)    │
│              OS interfaces, hardware drivers                 │
│                                                              │
│  Zephyr RTOS, Device Drivers, Hardware Peripherals           │
└─────────────────────────────────────────────────────────────┘
```

### Directory Structure Specification

```
project/
├── src/
│   ├── app/                    # Application layer
│   │   ├── app_main.c         # Main entry
│   │   ├── app_config.h       # Configuration definitions
│   │   └── app_version.h      # Version management
│   │
│   ├── modules/               # Business modules layer
│   │   ├── module_base.h      # Module base interface
│   │   ├── module_manager.c   # Module manager
│   │   ├── sensor_module/     # Sensor module
│   │   │   ├── sensor_module.h
│   │   │   └── sensor_module.c
│   │   └── network_module/    # Network module
│   │       ├── network_module.h
│   │       └── network_module.c
│   │
│   ├── services/              # System services layer
│   │   ├── sys_log.c         # Logging service
│   │   ├── sys_timer.c       # Timer service
│   │   └── sys_memory.c      # Memory management
│   │
│   └── core/                  # Core framework layer
│       ├── event_system.c     # Event system
│       └── event_dispatcher.c # Event dispatcher
│
├── include/                   # Public header files
├── tests/                     # Test code
├── docs/                      # Documentation
└── scripts/                   # Build scripts
```

---

## Error Handling and Fault Tolerance

### Error Code Definitions

```c
// Unified error code definitions
typedef enum {
    MODULE_OK = 0,
    MODULE_ERR_INVALID_PARAM = -1,
    MODULE_ERR_NOT_FOUND = -2,
    MODULE_ERR_NOT_INITIALIZED = -3,
    MODULE_ERR_ALREADY_RUNNING = -4,
    MODULE_ERR_TIMEOUT = -5,
    MODULE_ERR_NO_MEMORY = -6,
    MODULE_ERR_BUSY = -7,
    MODULE_ERR_NOT_SUPPORTED = -8,
} module_error_t;

// Error code to string conversion
static const char* module_error_str(int err)
{
    switch (err) {
    case MODULE_OK:                return "OK";
    case MODULE_ERR_INVALID_PARAM: return "Invalid parameter";
    case MODULE_ERR_NOT_FOUND:      return "Not found";
    case MODULE_ERR_NOT_INITIALIZED: return "Not initialized";
    case MODULE_ERR_ALREADY_RUNNING: return "Already running";
    case MODULE_ERR_TIMEOUT:       return "Timeout";
    case MODULE_ERR_NO_MEMORY:     return "No memory";
    case MODULE_ERR_BUSY:          return "Busy";
    case MODULE_ERR_NOT_SUPPORTED: return "Not supported";
    default:                       return "Unknown error";
    }
}
```

### Health Check Mechanism

```c
// Health check configuration
typedef struct {
    bool enable_health_check;       // Enable health check
    uint32_t check_interval_ms;     // Check interval
    bool enable_auto_recovery;      // Auto recovery
    uint32_t max_restart_count;     // Max restart count
} health_check_config_t;

// Health check process
static void health_check_handler(struct k_timer* timer)
{
    for (int i = 0; i < module_count; i++) {
        module_context_t* mod = &modules[i];

        if (mod->status != MODULE_STATUS_RUNNING) {
            continue;
        }

        // Call module's get_status or custom health check
        bool healthy = check_module_health(mod);

        if (!healthy) {
            LOG_WRN("Module %s unhealthy", mod->interface->name);

            if (mod->config.enable_auto_recovery) {
                attempt_recovery(mod);
            } else {
                mod->status = MODULE_STATUS_ERROR;
            }
        }
    }
}

// Auto recovery
static void attempt_recovery(module_context_t* mod)
{
    if (mod->restart_count >= mod->config.max_restart_count) {
        LOG_ERR("Module %s exceeded max restart count", mod->interface->name);
        mod->status = MODULE_STATUS_ERROR;
        return;
    }

    mod->restart_count++;
    LOG_INF("Attempting recovery for module %s (attempt %u)",
            mod->interface->name, mod->restart_count);

    // Stop and restart
    mod->interface->stop();
    int ret = mod->interface->start();

    if (ret == 0) {
        LOG_INF("Module %s recovered successfully", mod->interface->name);
        mod->status = MODULE_STATUS_RUNNING;
    } else {
        LOG_ERR("Module %s recovery failed", mod->interface->name);
        mod->status = MODULE_STATUS_ERROR;
    }
}
```

### Defensive Programming

```c
// Parameter validation
int module_process_data(const void* data, size_t len)
{
    // Null pointer check
    if (data == NULL) {
        LOG_ERR("NULL data pointer");
        return -EINVAL;
    }

    // Length check
    if (len == 0 || len > MAX_DATA_LEN) {
        LOG_ERR("Invalid data length: %zu", len);
        return -EINVAL;
    }

    // Status check
    if (g_status != MODULE_STATUS_RUNNING) {
        LOG_ERR("Module not running");
        return -EPERM;
    }

    // Actual processing...
    return do_process(data, len);
}

// Boundary check
int buffer_write(uint8_t* buf, size_t buf_size, const void* data, size_t data_len)
{
    if (data_len > buf_size) {
        LOG_ERR("Buffer overflow: %zu > %zu", data_len, buf_size);
        return -EOVERFLOW;
    }
    memcpy(buf, data, data_len);
    return 0;
}
```

---

## Testing Strategy

### Unit Testing

```c
// tests/test_sensor_module.c

#include <zephyr/ztest.h>
#include "sensor_module.h"

// Test setup
static void test_setup(void* data)
{
    // Initialize test environment
    module_manager_init();
}

static void test_teardown(void* data)
{
    // Cleanup test environment
    module_manager_shutdown();
}

ZTEST_SUITE(sensor_module_tests, NULL, NULL, test_setup, NULL, test_teardown);

// Test case: Normal initialization
ZTEST(sensor_module_tests, test_init_success)
{
    sensor_module_config_t config = {
        .sample_rate = 100,
        .mode = SENSOR_MODE_NORMAL
    };

    uint32_t module_id;
    int ret = module_manager_register(&sensor_module_interface, &config, &module_id);

    zassert_equal(ret, 0, "Registration should succeed");
    zassert_not_equal(module_id, 0, "Module ID should be assigned");
}

// Test case: Parameter validation
ZTEST(sensor_module_tests, test_init_null_config)
{
    uint32_t module_id;
    int ret = module_manager_register(&sensor_module_interface, NULL, &module_id);

    zassert_not_equal(ret, 0, "Should fail with NULL config");
}

// Test case: Lifecycle
ZTEST(sensor_module_tests, test_lifecycle)
{
    // Register
    uint32_t module_id;
    register_test_module(&module_id);

    // Check initial state
    module_info_t info;
    module_manager_get_module_info(module_id, &info);
    zassert_equal(info.status, MODULE_STATUS_INITIALIZED);

    // Start
    module_manager_start_module(module_id);
    module_manager_get_module_info(module_id, &info);
    zassert_equal(info.status, MODULE_STATUS_RUNNING);

    // Stop
    module_manager_stop_module(module_id);
    module_manager_get_module_info(module_id, &info);
    zassert_equal(info.status, MODULE_STATUS_STOPPED);
}
```

### Mock Testing

```c
// Use mock objects to test module interactions

// Mock dependency module
static int mock_storage_write(const void* data, size_t len)
{
    // Record call
    mock_storage_write_called = true;
    mock_storage_write_data = data;
    mock_storage_write_len = len;
    return 0;
}

// Inject mock
static storage_interface_t mock_storage = {
    .write = mock_storage_write,
};

ZTEST(data_processor_tests, test_data_saved_to_storage)
{
    // Inject mock storage
    data_processor_set_storage(&mock_storage);

    // Trigger data processing
    process_sample_data();

    // Verify storage was called
    zassert_true(mock_storage_write_called);
}
```

---

## Best Practices and Anti-Patterns

### Best Practices

#### 1. Module Naming Conventions

```c
// Recommended naming conventions

// Module name: lowercase with underscores
// sensor_module, network_manager, data_processor

// Function name: module_name_verb_noun
// sensor_module_read_data()
// network_manager_connect()
// data_processor_start()

// Type name: module_name_noun_t
// sensor_module_config_t
// network_manager_state_t

// Macro constants: module_name_UPPERCASE
// SENSOR_MODULE_MAX_SAMPLES
// NETWORK_MANAGER_TIMEOUT_MS
```

#### 2. Configuration Structure Design

```c
// Provide reasonable default values
typedef struct {
    uint32_t sample_rate;       // Sample rate, default 100
    uint32_t buffer_size;       // Buffer size, default 256
    bool enable_filter;         // Enable filter, default true
    const char* device_name;    // Device name, must specify
} sensor_module_config_t;

// Provide default configuration macro
#define SENSOR_MODULE_CONFIG_DEFAULT { \
    .sample_rate = 100, \
    .buffer_size = 256, \
    .enable_filter = true, \
    .device_name = NULL \
}

// Usage example
static sensor_module_config_t g_config = SENSOR_MODULE_CONFIG_DEFAULT;
g_config.device_name = "SENSOR0";
```

#### 3. Resource Management

```c
// Resource Acquisition Is Initialization (RAII) pattern

// Acquire resources during initialization
int module_init(void* config)
{
    g_config = *(module_config_t*)config;

    // Initialize mutex
    k_mutex_init(&g_mutex);

    // Initialize semaphore
    k_sem_init(&g_sem, 0, 1);

    // Initialize timer
    k_timer_init(&g_timer, timer_handler, NULL);

    return 0;
}

// Release resources during destruction
int module_shutdown(void)
{
    // Stop timer
    k_timer_stop(&g_timer);

    // Ensure no threads hold resources
    k_mutex_lock(&g_mutex, K_FOREVER);
    // Cleanup resources...
    k_mutex_unlock(&g_mutex);

    return 0;
}
```

#### 4. Logging

```c
// Level-based logging

// Initialization phase: info level
LOG_INF("Initializing sensor module with rate=%u", config->sample_rate);

// Running phase: debug level
LOG_DBG("Processing sample %u", sample_count);

// Error situations: error level
LOG_ERR("Failed to read sensor: %d", ret);

// Critical path: use LOG_DBG to avoid performance impact
// Problem investigation: temporarily increase log level
```

### Common Anti-Patterns

#### 1. God Modules

```c
// ❌ Anti-pattern: One module does everything
typedef struct {
    // Sensor related
    // Network related
    // Storage related
    // Display related
    // ... 50+ function pointers
} god_module_interface_t;

// ✅ Correct: Split by responsibility
sensor_interface_t sensor;
network_interface_t network;
storage_interface_t storage;
display_interface_t display;
```

#### 2. Circular Dependencies

```c
// ❌ Anti-pattern: A depends on B, B depends on A
// module_a.h
#include "module_b.h"

// module_b.h
#include "module_a.h"

// ✅ Correct: Extract common interface
// common_interface.h
// module_a.h (only includes common_interface.h)
// module_b.h (only includes common_interface.h)
```

#### 3. Excessive Coupling

```c
// ❌ Anti-pattern: Directly access other module's internal data
extern sensor_module_data_t g_sensor_data;  // Expose internal data

void process_data(void)
{
    // Directly access external module data
    float value = g_sensor_data.values[0];  // High coupling
}

// ✅ Correct: Access through interface
void process_data(void)
{
    float value;
    sensor_module_read(&value);  // Low coupling
}
```

#### 4. Global State Abuse

```c
// ❌ Anti-pattern: Too many global variables
static int g_count;
static float g_value;
static bool g_flag;
static void* g_ptr;
// ... 20+ global variables

// ✅ Correct: Encapsulate into struct
typedef struct {
    int count;
    float value;
    bool flag;
    void* ptr;
} module_state_t;

static module_state_t g_state;
```

---

## Case Studies

### Case 1: Sensor Data Processing Module

Complete module design example:

```c
// sensor_processor.h
#ifndef SENSOR_PROCESSOR_H
#define SENSOR_PROCESSOR_H

#include <zeplod/module_base.h>

// Configuration structure
typedef struct {
    uint32_t sample_rate_ms;    // Sample interval
    uint32_t buffer_size;       // Buffer size
    float threshold;            // Alarm threshold
    bool enable_filter;         // Enable filter
} sensor_processor_config_t;

// Event types
#define EVENT_TYPE_SENSOR_ALARM  100
#define EVENT_TYPE_SENSOR_DATA   101

// Public APIs
int sensor_processor_get_latest(float* value);
int sensor_processor_get_statistics(float* avg, float* max, float* min);
int sensor_processor_set_threshold(float threshold);

// Module interface (generated by macro)
extern const module_interface_t sensor_processor_interface;

#endif

// sensor_processor.c
#include "sensor_processor.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sensor_processor, LOG_LEVEL_INF);

// Internal data structure
typedef struct {
    float* buffer;
    uint32_t buffer_size;
    uint32_t write_index;
    float threshold;
    bool filter_enabled;
    float last_value;
    uint32_t sample_count;
} sensor_processor_ctx_t;

// Static instances
static sensor_processor_ctx_t g_ctx;
static float g_buffer[CONFIG_SENSOR_BUFFER_SIZE];
static sensor_processor_config_t g_config;
static module_status_t g_status = MODULE_STATUS_UNINITIALIZED;

// Initialization
static int sensor_processor_init(void* config)
{
    if (config == NULL) {
        return -EINVAL;
    }

    memcpy(&g_config, config, sizeof(g_config));
    memset(&g_ctx, 0, sizeof(g_ctx));

    g_ctx.buffer = g_buffer;
    g_ctx.buffer_size = g_config.buffer_size;
    g_ctx.threshold = g_config.threshold;
    g_ctx.filter_enabled = g_config.enable_filter;

    g_status = MODULE_STATUS_INITIALIZED;
    LOG_INF("Sensor processor initialized: rate=%ums, threshold=%.2f",
            g_config.sample_rate_ms, g_config.threshold);

    return 0;
}

// Startup
static int sensor_processor_start(void)
{
    g_status = MODULE_STATUS_RUNNING;
    LOG_INF("Sensor processor started");
    return 0;
}

// Stop
static int sensor_processor_stop(void)
{
    g_status = MODULE_STATUS_STOPPED;
    LOG_INF("Sensor processor stopped");
    return 0;
}

// Shutdown
static int sensor_processor_shutdown(void)
{
    g_status = MODULE_STATUS_UNINITIALIZED;
    memset(&g_ctx, 0, sizeof(g_ctx));
    return 0;
}

// Event handling
static void sensor_processor_on_event(const event_t* event, void* user_data)
{
    if (event == NULL || g_status != MODULE_STATUS_RUNNING) {
        return;
    }

    switch (event->type) {
    case EVENT_TYPE_SENSOR_DATA:
        if (event->data && event->data_len == sizeof(float)) {
            float value = *(float*)event->data;
            process_sample(value);
        }
        break;
    }
}

// Status query
static module_status_t sensor_processor_get_status(void)
{
    return g_status;
}

// Control commands
static int sensor_processor_control(int cmd, void* arg)
{
    switch (cmd) {
    case 0:  // Set threshold
        if (arg) {
            g_ctx.threshold = *(float*)arg;
            return 0;
        }
        return -EINVAL;

    case 1:  // Get statistics
        // ...
        return 0;

    default:
        return -ENOTSUP;
    }
}

// Internal functions
static void process_sample(float value)
{
    // Filter processing
    if (g_ctx.filter_enabled) {
        value = value * 0.8f + g_ctx.last_value * 0.2f;
    }
    g_ctx.last_value = value;

    // Store in buffer
    g_ctx.buffer[g_ctx.write_index] = value;
    g_ctx.write_index = (g_ctx.write_index + 1) % g_ctx.buffer_size;
    g_ctx.sample_count++;

    // Threshold detection
    if (value > g_ctx.threshold) {
        trigger_alarm(value);
    }
}

static void trigger_alarm(float value)
{
    LOG_WRN("Sensor alarm: value=%.2f exceeds threshold=%.2f",
            value, g_ctx.threshold);

    event_t event = {
        .type = EVENT_TYPE_SENSOR_ALARM,
        .priority = EVENT_PRIORITY_HIGH,
        .reserved = 0,
        .data.ptr = &value,
        .data_len = sizeof(value)
    };

    module_manager_broadcast(&event);
}

// Public API implementation
int sensor_processor_get_latest(float* value)
{
    if (g_status != MODULE_STATUS_RUNNING || value == NULL) {
        return -EINVAL;
    }

    uint32_t idx = (g_ctx.write_index + g_ctx.buffer_size - 1) % g_ctx.buffer_size;
    *value = g_ctx.buffer[idx];
    return 0;
}

// Use macro to declare module interface
DECLARE_MODULE_INTERFACE(sensor_processor);
```

### Case 2: Module Dependency Configuration

```c
// Application configuration example

// Define dependency relationships
static const char* const data_processor_deps[] = {
    "sensor_module",
    "storage_module",
    NULL
};

// Module registration
static int register_all_modules(void)
{
    uint32_t module_id;

    // 1. Driver layer modules (no dependencies)
    sensor_module_config_t sensor_cfg = {
        .sample_rate = 100
    };
    module_manager_register(&sensor_module_interface, &sensor_cfg, &module_id);

    storage_module_config_t storage_cfg = {
        .partition = "storage"
    };
    module_manager_register(&storage_module_interface, &storage_cfg, &module_id);

    // 2. Business layer modules (with dependencies)
    data_processor_config_t processor_cfg = {
        .buffer_size = 256
    };
    module_manager_register(&data_processor_interface, &processor_cfg, &module_id);

    // 3. Application layer modules
    application_config_t app_cfg = { /* ... */ };
    module_manager_register(&application_interface, &app_cfg, &module_id);

    return 0;
}
```

---

## Appendix

### References

- [Detailed Module System Usage Guide](../30-core-modules/32-module-system-guide.md)
- [Detailed Event System Usage Guide](../30-core-modules/31-event-system-guide.md)
- [Developer Getting Started Guide](../00-getting-started/04-developer-guide.md)
- [Zephyr RTOS Official Documentation](https://docs.zephyrproject.org/)

### Glossary

| Term | English | Definition |
|------|---------|------------|
| Module | Module | Software unit with clearly defined interfaces |
| Interface | Interface | Service contract provided by module to outside |
| Dependency | Dependency | Relationship between modules, A depends on B means A needs B |
| Coupling | Coupling | Degree of association between modules |
| Cohesion | Cohesion | Degree of association between elements within a module |
| Encapsulation | Encapsulation | Hide implementation details |
| Abstraction | Abstraction | Ignore details, focus on essence |

---

**Document Version: 1.0.0**
**Last Updated: 2026-04-14**
