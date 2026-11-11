> Language: [中文](../../zh-CN/20-架构设计/21-模块化软件设计方法论.md) | **English**

# Modular Software Design Methodology

> **Comprehensive Step-by-Step Guide** — From concept to practice, hands-on guide to designing high-quality modular embedded systems

---

## Table of Contents

1. [Modular Design Basics: What and Why?](#1-modular-design-basics-what-and-why)
2. [Deep Dive into SOLID Principles](#2-deep-dive-into-solid-principles)
3. [Module Quality Assessment: Cohesion and Coupling](#3-module-quality-assessment-cohesion-and-coupling)
4. [Seven-Step Module Design Method](#4-seven-step-module-design-method)
5. [Module Partitioning Practice: How to Identify Module Boundaries](#5-module-partitioning-practice-how-to-identify-module-boundaries)
6. [Interface Design Details](#6-interface-design-details)
7. [Dependency Management: Keeping Your System Clean](#7-dependency-management-keeping-your-system-clean)
8. [Module Lifecycle Management](#8-module-lifecycle-management)
9. [Event-Driven Architecture: How Modules Communicate](#9-event-driven-architecture-how-modules-communicate)
10. [Modular Architecture Patterns and Selection](#10-modular-architecture-patterns-and-selection)
11. [Code Quality Assurance](#11-code-quality-assurance)
12. [Common Problems and Solutions](#12-common-problems-and-solutions)
13. [This Framework's Modular Implementation Guide](#13-this-frameworks-modular-implementation-guide)
14. [Summary and Checklist](#14-summary-and-checklist)

---

## 1. Modular Design Basics: What and Why?

### 1.1 What is a Module?

A **module** is the smallest unit in a software system with independent responsibilities, testable individually, and reusable. In embedded C language, a module typically corresponds to:

- One `.c` source file + one `.h` header file
- A set of closely related functions and data structures
- Clearly defined interfaces exposed to the outside

**Module Examples in This Framework:**

```
src/modules/
├── example_module_gpio.c/h    ← GPIO driver module
├── example_module_uart.c/h    ← UART communication module
├── example_module_ipc.c/h     ← IPC functionality module
```

### 1.2 What is Modular Design?

Modular design is a software development method that **decomposes a system into independent modules**, where each module:
- Encapsulates specific responsibilities
- Has clear interface boundaries
- Can be independently developed, tested, and reused

### 1.3 Why Modularize?

| Consequences of Not Modularizing | Benefits of Modularization |
|----------------------------------|---------------------------|
| Code duplication, copy-paste everywhere | Module reuse, reduced duplicate code |
| Modifying one thing affects many | Local changes don't affect the whole |
| Hard to test, one change affects all | Modules can be independently unit tested |
| Hard for newcomers, must understand entire system | Clear module boundaries, quick location |
| Team collaboration conflicts, merge hell | Separate module development, fewer conflicts |

### 1.4 Core Goals of Modular Design

```
┌─────────────────────────────────────────────────────────────┐
│              Four Goals of Modular Design                   │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│   ┌─────────────┐    ┌─────────────┐    ┌─────────────┐    │
│   │  Reduce     │ →  │  Increase   │ →  │  Easier to  │    │
│   │  Complexity │    │  Reuse      │    │  Test       │    │
│   └─────────────┘    └─────────────┘    └─────────────┘    │
│          │                                    │             │
│          └──────────────→  Easy Maintenance  ←───────────────┘│
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

### 1.5 Modular vs Object-Oriented

| Dimension | Object-Oriented (OOP) | Modular (This Framework) |
|------------|----------------------|--------------------------|
| Basic Unit | Class | Module |
| Encapsulation | Class members + methods | C files + interface functions |
| Inheritance | Supports polymorphism | Simulated through interfaces |
| Communication | Method calls | Event publish/subscribe |
| Runtime polymorphism | Supported | Determined at compile time |

**This framework's modularization** draws on OOP interface concepts but uses C language implementation, more suitable for resource-constrained embedded environments.

---

## 2. Deep Dive into SOLID Principles

### 2.0 SOLID Quick Overview

| Principle | One-Line Description | Core Question |
|-----------|---------------------|---------------|
| **S** - Single Responsibility | Each module does one thing | "Would this module change for multiple reasons?" |
| **O** - Open-Closed | Open for extension, closed for modification | "Do I need to modify existing code to add new features?" |
| **L** - Liskov Substitution | Subclasses can replace parent classes | "Can all subclasses be used interchangeably?" |
| **I** - Interface Segregation | Clients only depend on needed methods | "Does the module force me to call unnecessary interfaces?" |
| **D** - Dependency Inversion | Depend on abstractions, not concretions | "Do high-level modules directly call low-level modules?" |

---

### 2.1 Single Responsibility Principle (SRP)

#### What is a "Responsibility"?

A **responsibility** is a reason for a module to change. If a module can be modified for multiple reasons, it violates SRP.

#### How to Judge: Ask Yourself Two Questions

```
Question 1: For what reasons would this module change?
Question 2: Is there more than one answer?
```

#### Example Violating SRP

```c
// ❌ Violating SRP: One module handles multiple responsibilities
// src/modules/bad_module.c
typedef struct {
    uint32_t temperature;      // May change due to sensor
    uint32_t humidity;         // May change due to sensor
    uint32_t display_mode;     // May change due to UI
    uint32_t log_level;        // May change due to configuration
} sensor_config_t;

int sensor_init(void) {
    // Sensor initialization
}

int sensor_read(uint32_t *temp, uint32_t *hum) {
    // Read sensor
}

void sensor_display(void) {
    // Display data - UI responsibility mixed in
}

void sensor_log(void) {
    // Log data - logging responsibility mixed in
}
```

**Problem Analysis:**

| Reason for Change | Affected Functions |
|------------------|-------------------|
| Sensor driver replacement | `sensor_init()`, `sensor_read()` |
| UI display logic change | `sensor_display()` |
| Log format change | `sensor_log()` |

#### Refactoring to Comply with SRP

```c
// ✅ Complying with SRP: Separation of responsibilities
// src/modules/sensor_driver.c/h - Sensor driver
typedef struct {
    uint32_t temperature;
    uint32_t humidity;
} sensor_data_t;

int sensor_driver_init(void);
int sensor_read_data(sensor_data_t *data);

// src/modules/sensor_display.c/h - Display module
void sensor_display_update(const sensor_data_t *data);

// src/app/sensor_logger.c/h - Logger module
void sensor_log_data(const sensor_data_t *data);
```

#### SRP Practice in This Framework

```c
// src/core/event_system.c - Event system only does event distribution
int event_publish(event_type_t type, const void *data, size_t len);
int event_subscribe(event_type_t type, event_callback_t cb, void *user_data);
int event_dispatch(const event_t *event);

// Never mix: logging functionality (should be handled by sys_log)
// Never mix: data storage (should be handled by other modules)
```

---

### 2.2 Open-Closed Principle (OCP)

#### What are "Open" and "Closed"?

| State | Meaning |
|--------|---------|
| **Open for Extension** | Can add new functionality (new modules, new event types) |
| **Closed for Modification** | Existing code doesn't need modification |

#### Example Violating OCP

```c
// ❌ Violating OCP: Adding new events requires modifying event_publish
typedef enum {
    EVENT_TYPE_A,
    EVENT_TYPE_B,
    EVENT_TYPE_C,
    // Adding new events requires modifying here
} event_type_t;

int event_publish(event_type_t type, const void *data, size_t len) {
    switch (type) {
        case EVENT_TYPE_A:
            return handle_event_a(data);
        case EVENT_TYPE_B:
            return handle_event_b(data);
        case EVENT_TYPE_C:
            return handle_event_c(data);
        // Every time a new event is added, this needs to change!
        default:
            return -EINVAL;
    }
}
```

#### Refactoring to Comply with OCP

```c
// ✅ Complying with OCP: Adding new event types doesn't require modifying event_publish
// event_system.c - Core code doesn't need to change
int event_publish(event_type_t type, const void *data, size_t len) {
    // Generic publish logic, only operates on queue
    return event_queue_push(type, data, len);
}

// src/modules/sensor_module.c - Adding new modules doesn't require modifying core
event_subscribe(EVENT_TYPE_SENSOR_DATA, sensor_event_handler, NULL, &sub_id);

// src/modules/network_module.c - Adding new modules doesn't require modifying core
event_subscribe(EVENT_TYPE_NETWORK_READY, network_event_handler, NULL, &sub_id);
```

#### OCP Practice in This Framework

```c
// Core event system treats all event types equally
// Adding new event types = adding new subscribers = no core modifications needed

// Define new event type (in app_config.h)
#define EVENT_TYPE_SENSOR_DATA    10
#define EVENT_TYPE_NETWORK_READY  11

// New module subscribes (no need to modify event_system.c)
event_subscribe(EVENT_TYPE_SENSOR_DATA, my_handler, NULL, &my_sub_id);
```

---

### 2.3 Liskov Substitution Principle (LSP)

#### What is Liskov Substitution?

**Instances of subclasses can replace instances of parent classes without affecting program correctness.**

#### Example Violating LSP

```c
// ❌ Violating LSP: Subclass narrows parent's behavior
typedef struct {
    int (*init)(void);
    int (*read)(uint8_t *buf, size_t len);  // Base class: can read any length
} sensor_interface_t;

// Wrong implementation: UART sensor can only read fixed length
int uart_read(uint8_t *buf, size_t len) {
    if (len != 4) {  // Violation: narrows valid range of len
        return -EINVAL;
    }
    // ...
}

// Caller uses parent class interface
int generic_read(sensor_interface_t *sensor, uint8_t *buf, size_t len) {
    return sensor->read(buf, len);  // Assumes any len works
}

// But when passing UART sensor, len != 4 fails!
```

#### Refactoring to Comply with LSP

```c
// ✅ Complying with LSP: Interface contract is clear, all implementations consistent
typedef struct {
    const char *name;
    int (*init)(void);
    // read returns actual bytes read, caller must check return value
    int (*read)(uint8_t *buf, size_t len);
} sensor_interface_t;

// UART implementation
int uart_read(uint8_t *buf, size_t len) {
    if (len < 4) {
        return -EINVAL;  // Clear error code
    }
    // Read data
    return 4;  // Return actual bytes read
}

// I2C implementation
int i2c_read(uint8_t *buf, size_t len) {
    if (len < 2) {
        return -EINVAL;
    }
    return i2c_transfer(buf, len);
}

// Caller correctly checks return value
int generic_read(sensor_interface_t *sensor, uint8_t *buf, size_t len) {
    int ret = sensor->read(buf, len);
    if (ret < 0) {
        return ret;
    }
    // Process read data
    return 0;
}
```

#### LSP Practice in This Framework

```c
// All modules implement unified module_interface_t
typedef struct {
    const char *name;
    int (*init)(void *config);
    int (*start)(void);
    int (*stop)(void);
    int (*shutdown)(void);
    module_event_handler_t on_event;
    module_status_t (*get_status)(void);
    int (*control)(int cmd, void *arg);
} module_interface_t;

// Module manager calls uniformly, no need to care about specific module types
int module_manager_register(module_interface_t *interface, void *config, uint32_t *id) {
    // All modules can register through this interface
    // No need to know if it's GPIO module or UART module
}
```

---

### 2.4 Interface Segregation Principle (ISP)

#### What is Interface Segregation?

**Clients should not be forced to depend on interfaces they don't use.** In other words, don't force modules to use methods they don't need.

#### Example Violating ISP

```c
// ❌ Violating ISP: Huge bloated interface
typedef struct {
    int (*init)(void);
    int (*start)(void);
    int (*stop)(void);
    int (*shutdown)(void);
    int (*read)(uint8_t *buf, size_t len);
    int (*write)(const uint8_t *buf, size_t len);
    int (*flush)(void);
    int (*reset)(void);
    int (*sleep)(void);
    int (*wakeup)(void);
    // ... more methods
} mega_driver_interface_t;

// Simple module also forced to implement all methods
typedef struct {
    int (*init)(void);
    int (*start)(void);
    int (*stop)(void);
    int (*shutdown)(void);
    int (*read)(uint8_t *buf, size_t len);
    int (*write)(const uint8_t *buf, size_t len);
    int (*flush)(void);      // Simple module doesn't need, but must implement empty function
    int (*reset)(void);      // Simple module doesn't need
    int (*sleep)(void);      // Simple module doesn't need
    int (*wakeup)(void);     // Simple module doesn't need
} simple_uart_interface_t;
```

#### Refactoring to Comply with ISP: Separate Interfaces

```c
// ✅ Complying with ISP: Minimal interfaces, combined as needed
// Base interface (all modules need)
typedef struct {
    int (*init)(void *config);
    int (*start)(void);
    int (*stop)(void);
    int (*shutdown)(void);
} module_base_interface_t;

// Event handling interface (optional)
typedef struct {
    void (*on_event)(const event_t *event, void *user_data);
} module_event_interface_t;

// Data I/O interface (only for modules that need read/write)
typedef struct {
    int (*read)(uint8_t *buf, size_t len);
    int (*write)(const uint8_t *buf, size_t len);
} module_io_interface_t;

// Simple module only needs to implement base interface
static int simple_init(void *config) { /* ... */ }
static int simple_start(void) { /* ... */ }
static int simple_stop(void) { /* ... */ }
static int simple_shutdown(void) { /* ... */ }

static module_base_interface_t simple_ops = {
    .init = simple_init,
    .start = simple_start,
    .stop = simple_stop,
    .shutdown = simple_shutdown,
};
```

#### ISP Practice in This Framework

```c
// module_base.h - Core interface stays minimal
typedef struct {
    const char *name;
    uint32_t version;
    module_priority_t priority;
    const char *const *depends_on;  // Dependency declaration
    int (*init)(void *config);
    int (*start)(void);
    int (*stop)(void);
    int (*shutdown)(void);
    module_event_handler_t on_event;  // Event handling (optional)
    module_status_t (*get_status)(void);
    int (*control)(int cmd, void *arg);  // Control commands (optional)
} module_interface_t;

// Modules that only need basic functionality set on_event to NULL
// Modules that only need status queries set control to NULL
```

---

### 2.5 Dependency Inversion Principle (DIP)

#### What is Dependency Inversion?

| Traditional Way | After Dependency Inversion |
|-----------------|---------------------------|
| High-level module → Low-level module | High-level module → Abstract interface ← Low-level module |

#### Example Violating DIP

```c
// ❌ Violating DIP: High-level module directly depends on low-level module
// src/app/data_process.c
#include "sensor_driver.h"  // Directly depends on concrete implementation

int process_sensor_data(void) {
    sensor_data_t data;

    // Directly call low-level module
    if (sensor_driver_read(&data) != 0) {
        return -EIO;
    }

    // Process data
    return process(&data);
}
```

**Dependency Diagram:**
```
┌─────────────────┐
│ data_process    │  ← High-level module
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ sensor_driver   │  ← Low-level module
└─────────────────┘
```

#### Refactoring to Comply with DIP

```c
// ✅ Complying with DIP: Depend on abstract interface
// src/core/sensor_interface.h - Define abstract interface
#ifndef SENSOR_INTERFACE_H
#define SENSOR_INTERFACE_H

#include <stdint.h>

typedef struct {
    const char *name;
    int (*init)(void);
    int (*read)(uint32_t *data);
    int (*deinit)(void);
} sensor_interface_t;

#endif

// src/app/data_process.c - Depends on abstraction
#include "sensor_interface.h"

int process_sensor_data(const sensor_interface_t *sensor) {
    uint32_t data;

    // Call through abstract interface, don't care about concrete implementation
    if (sensor->read(&data) != 0) {
        return -EIO;
    }

    return process(data);
}

// src/drivers/ds18b20.c - Implement interface
#include "sensor_interface.h"

static int ds18b20_read(uint32_t *data) {
    // Read temperature sensor
}

static sensor_interface_t ds18b20_ops = {
    .name = "ds18b20",
    .init = ds18b20_init,
    .read = ds18b20_read,
    .deinit = ds18b20_deinit,
};
```

**Dependency Diagram:**
```
┌─────────────────┐
│ data_process    │  ← High-level module
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ sensor_interface│  ← Abstract interface
└────────┬────────┘
         │
    ┌────┴────┐
    ▼         ▼
┌────────┐  ┌────────┐
│ ds18b20│  │ bme280│  ← Low-level modules (implement interface)
└────────┘  └────────┘
```

#### DIP Practice in This Framework

```c
// High-level: Event system
int event_subscribe(event_type_t type, event_callback_t cb, void *user_data);
// Doesn't depend on specific business modules, only depends on callback function signature

// Low-level: Business modules implement callbacks
void my_module_on_event(const event_t *event, void *user_data) {
    // Handle events, don't care who published them
}

// Decoupled through events, achieving dependency inversion
```

---

## 3. Module Quality Assessment: Cohesion and Coupling

### 3.1 Cohesion - How tightly connected are internal module elements?

#### Cohesion Level Table

| Level | Type | Description | Code Characteristics | Recommendation |
|-------|------|-------------|----------------------|----------------|
| 7 🟢 | **Functional Cohesion** | Module completes single function | All code serves one purpose | ✅✅✅ Best |
| 6 🟢 | **Sequential Cohesion** | Previous output is next input | Data processing pipeline | ✅✅ Good |
| 5 🟡 | **Communication Cohesion** | Operations on same data | Different operations on same data structure | ✅ Acceptable |
| 4 🟡 | **Procedural Cohesion** | Execute in specific order | Steps clear but functions differ | ⚠️ Acceptable |
| 3 🔴 | **Temporal Cohesion** | Operations executed together | `init()` functions grouped | ❌ Avoid |
| 2 🔴 | **Logical Cohesion** | Logically related but different functions | `misc()` functions | ❌ Avoid |
| 1 🔴 | **Coincidental Cohesion** | Unrelated code grouped together | Random grouping | ❌ Forbidden |

#### Cohesion Visualization

```
Functional Cohesion (Best)      Coincidental Cohesion (Worst)
┌───────────────┐               ┌───────────────┐
│ ██████████████│               │ ██  █  ██    │
│ ██████████████│               │  █ ██ █  █   │
│ ██████████████│               │ █  ██  ██   │
│ ██████████████│               │  █ ██ █  █   │
└───────────────┘               └───────────────┘
   Highly unified                 Chaotic
```

#### Cohesion Judgment Practice

**Scenario**: Analyze cohesion of the following module

```c
// ❌ Coincidental cohesion module
// src/utils/misc.c
int calculate_checksum(const uint8_t *data, size_t len) {
    // Checksum calculation
}

void print_debug(const char *fmt, ...) {
    // Debug printing
}

double square_root(double x) {
    // Math calculation
}

int string_utils_split(const char *str, char delim) {
    // String splitting
}
// Problem: These functions have no relationship, just "miscellaneous"

---

// ✅ Functional cohesion module
// src/utils/crc.c
uint32_t crc32_init(void);
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len);
uint32_t crc32_final(uint32_t crc);
int crc32_verify(const uint8_t *data, size_t len, uint32_t expected);

// Problem: All functions serve CRC checksum one purpose
```

#### Cohesion Practice in This Framework

```c
// ✅ Functional cohesion: Event queue only handles queue operations
// src/core/event_queue.c
int event_queue_init(event_queue_t *queue, size_t capacity);
int event_queue_push(event_queue_t *queue, const event_t *event);
int event_queue_pop(event_queue_t *queue, event_t *event);
size_t event_queue_size(const event_queue_t *queue);
bool event_queue_is_empty(const event_queue_t *queue);

// ❌ This code would never appear:
// int event_queue_push(event_queue_t *queue, const event_t *event) {
//     // ... event enqueue ...
//     sys_log_print("Event pushed");  // Mixed logging - temporal cohesion
//     save_to_flash(event);           // Mixed storage - coincidental cohesion
// }
```

---

### 3.2 Coupling - How tightly are modules interdependent?

#### Coupling Level Table

| Level | Type | Description | Dependency Method | Recommendation |
|-------|------|-------------|------------------|-----------------|
| 1 🟢 | **No Coupling** | Completely independent | None | ✅✅✅ Ideal |
| 2 🟢 | **Data Coupling** | Pass through parameters | `func(a, b)` | ✅✅✅ Best |
| 3 🟢 | **Stamp Coupling** | Pass through data structures | `func(data_t *d)` | ✅✅ Good |
| 4 🟡 | **Control Coupling** | Pass control flags | `func(cmd, flag)` | ✅ Acceptable |
| 5 🔴 | **External Coupling** | Share global data | Global variables | ❌ Avoid |
| 6 🔴 | **Content Coupling** | Directly access internals | `a.b = x` | ❌ Forbidden |

#### Coupling Visualization

```
No Coupling (Best)               Content Coupling (Worst)

    ┌───┐                      ┌───┐
    │ A │                      │ A │──────┐
    └───┘                      └─┬─┘      │
                                │        │
    ┌───┐                      ┌──▼──┐    │
    │ B │                      │  B  │◄────┘
    └───┘                      └──┬──┘
                                │
    ┌───┐                      ┌──▼──┐
    │ C │                      │  C  │
    └───┘                      └────┘
```

#### Coupling Judgment Practice

```c
// ✅ Data coupling: Modules exchange data through parameters
// Module A
int sensor_get_data(sensor_data_t *out_data) {
    out_data->temp = read_temp();
    out_data->hum = read_hum();
    return 0;
}

// Module B calls A
sensor_data_t data;
sensor_get_data(&data);  // Only passes data, not behavior

// ❌ External coupling: Shared global variables
// sensor.c
sensor_data_t g_sensor_data;  // Global variable

// user.c
extern sensor_data_t g_sensor_data;  // Depends on global variable
if (g_sensor_data.temp > 30) { ... }

// ❌ Content coupling: Directly access internal structure
// user.c - Wrong example
extern sensor_data_t g_sensor_data;
g_sensor_data.internal_state = 0xFF;  // Directly modify internal state
```

#### Coupling Practice in This Framework

```c
// ✅ Modules decoupled through events - no direct coupling
// Module A publishes event
event_publish_copy(EVENT_TYPE_DATA_READY, EVENT_PRIORITY_NORMAL,
                   &sensor_data, sizeof(sensor_data));

// Module B subscribes to event
event_subscribe(EVENT_TYPE_DATA_READY, module_b_handler, NULL, &sub_id);

// A and B don't know each other exist, only communicate through event system
```

---

### 3.3 Relationship Between Cohesion and Coupling

#### Ideal State: High Cohesion + Low Coupling

```
                     High Cohesion
                       ↑
                       │
      ┌─────────────────┼─────────────────┐
      │                 │                 │
      │   🟢 Ideal      │   ⚠️ Gray       │ ← Medium cohesion + Low coupling
      │  High cohesion  │  Procedural     │
      │  Low coupling   │  Data coupling  │
      │                 │                 │
Low ────────────────────────────────────→ High Coupling
Coupling │                 │                 │
      │   ⚠️ Gray       │   🔴 Dangerous   │
      │  Logical        │  Coincidental    │
      │  High coupling  │  Content coupling│
      │                 │                 │
      └─────────────────┼─────────────────┘
                       │
                       ↓
                     Low Cohesion
```

#### This Framework's Practice: High Cohesion + Low Coupling

```c
// Event system: High cohesion (only event distribution) + Low coupling (doesn't depend on any business module)
// src/core/event_system.c
static event_queue_t g_event_queue;
static subscriber_t g_subscribers[EVENT_MAX_SUBSCRIBERS];

int event_publish(event_type_t type, const void *data, size_t len) {
    event_t event = {
        .type = type,
        .reserved = 0,
        .data.ptr = (void *)data,
        .data_len = len,
    };
    return event_queue_push(&g_event_queue, &event);
}

// No direct dependencies between modules, communicate through events
```

---

## 4. Seven-Step Module Design Method

### 4.0 Process Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                   Seven-Step Module Design Method                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Step 1: Identify Responsibilities                               │
│     │                                                           │
│     ▼                                                           │
│  Step 2: Define Interfaces                                       │
│     │                                                           │
│     ▼                                                           │
│  Step 3: Determine Dependencies                                  │
│     │                                                           │
│     ▼                                                           │
│  Step 4: Design Data Structures                                 │
│     │                                                           │
│     ▼                                                           │
│  Step 5: Implement Module                                       │
│     │                                                           │
│     ▼                                                           │
│  Step 6: Write Tests                                            │
│     │                                                           │
│     ▼                                                           │
│  Step 7: Write Documentation                                    │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

### Step 1: Identify Responsibilities

#### 1.1 What is a Responsibility?

A **responsibility** is a **single, clear role or task** that a module undertakes in the system.

#### 1.2 Methods to Identify Responsibilities

**Method 1: Verb Analysis**

Extract verbs from requirements; verbs correspond to actions, actions correspond to responsibilities:

```
Requirement description:
"System needs to collect temperature data, display through display module, alert when temperature exceeds threshold"

Verb analysis:
- Collect → Sensor driver responsibility
- Display → Display module responsibility
- Alert → Alert module responsibility
```

**Method 2: Change Point Analysis**

Find parts of requirements that **may change independently**:

```
Change points in requirements:
- Sensor type (DS18B20, BME280, SHT30...) → Sensor driver module
- Display method (LCD, OLED, serial...) → Display module
- Alert method (buzzer, LED, relay...) → Alert module

Stable points (unchanging parts):
- Temperature threshold judgment logic
- Data collection process
```

#### 1.3 Responsibility Identification Checklist

```
□ What is this module responsible for?
□ Does the module's name clearly express its responsibility?
□ Can you describe this module's responsibility in one sentence?
□ How many different reasons would this module change for?
□ If this module were deleted, could the system still work?
```

---

### Step 2: Define Interfaces

#### 2.1 Interface Design Principles

| Principle | Description | Practice |
|-----------|-------------|----------|
| **Minimal** | Only expose necessary methods | Don't expose unneeded methods |
| **Self-describing** | Method names express intent | `sensor_read()` not `sensor_op()` |
| **Consistent** | Uniform interface style | Consistent parameter order, return value style |
| **Stable** | Interfaces should remain unchanged once published | Consider extensibility |

#### 2.2 Interface Definition Template

```c
/**
 * @file <module_name>.h
 * @brief <Module responsibility brief description>
 *
 * <Detailed description including purpose, usage scenarios, dependencies>
 */

#ifndef <MODULE_NAME>_H
#define <MODULE_NAME>_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//==============================================================================
// Type Definitions
//==============================================================================

/**
 * @brief Module configuration parameters
 */
typedef struct {
    uint32_t param1;         /**< Parameter 1 description */
    uint32_t param2;         /**< Parameter 2 description */
} <module_name>_config_t;

/**
 * @brief Module status
 */
typedef enum {
    <MODULE_NAME>_STATUS_NONE = 0,
    <MODULE_NAME>_STATUS_INIT,
    <MODULE_NAME>_STATUS_RUNNING,
    <MODULE_NAME>_STATUS_ERROR,
} <module_name>_status_t;

//==============================================================================
// Error Code Definitions
//==============================================================================

#define <MODULE_NAME>_OK           0
#define <MODULE_NAME>_ERR_INVALID  (-EINVAL)   /**< Invalid parameter */
#define <MODULE_NAME>_ERR_NO_MEM   (-ENOMEM)    /**< Insufficient memory */
#define <MODULE_NAME>_ERR_HW       (-EIO)       /**< Hardware error */
#define <MODULE_NAME>_ERR_BUSY     (-EBUSY)     /**< Device busy */

//==============================================================================
// Interface Function Declarations
//==============================================================================

/**
 * @brief Initialize module
 * @param config Configuration parameters, NULL uses default
 * @return 0 success, negative error code
 */
int <module_name>_init(<module_name>_config_t *config);

/**
 * @brief Start module
 * @return 0 success, negative error code
 */
int <module_name>_start(void);

/**
 * @brief Stop module
 * @return 0 success, negative error code
 */
int <module_name>_stop(void);

/**
 * @brief Get module status
 * @return Current module status
 */
<module_name>_status_t <module_name>_get_status(void);

#ifdef __cplusplus
}
#endif

#endif /* <MODULE_NAME>_H */
```

#### 2.3 Interface Definition Example

```c
/**
 * @file sensor_driver.h
 * @brief Sensor driver interface
 */
#ifndef SENSOR_DRIVER_H
#define SENSOR_DRIVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//==============================================================================
// Type Definitions
//==============================================================================

typedef struct {
    uint32_t sample_rate;       /**< Sample rate (Hz) */
    uint32_t threshold_high;    /**< High threshold */
    uint32_t threshold_low;     /**< Low threshold */
} sensor_driver_config_t;

typedef enum {
    SENSOR_DRIVER_STATUS_NONE = 0,
    SENSOR_DRIVER_STATUS_INIT,
    SENSOR_DRIVER_STATUS_RUNNING,
    SENSOR_DRIVER_STATUS_ERROR,
} sensor_driver_status_t;

typedef struct {
    uint32_t temperature;       /**< Temperature value */
    uint32_t humidity;          /**< Humidity value */
    uint32_t timestamp;         /**< Timestamp */
} sensor_driver_data_t;

//==============================================================================
// Error Codes
//==============================================================================

#define SENSOR_DRIVER_OK          0
#define SENSOR_DRIVER_ERR_INVALID (-EINVAL)
#define SENSOR_DRIVER_ERR_NO_MEM  (-ENOMEM)
#define SENSOR_DRIVER_ERR_HW      (-EIO)

//==============================================================================
// Interface Functions
//==============================================================================

/**
 * @brief Initialize sensor driver
 * @param config Configuration parameters, NULL uses default
 * @return 0 success, negative error code
 */
int sensor_driver_init(sensor_driver_config_t *config);

/**
 * @brief Start sensor
 * @return 0 success, negative error code
 */
int sensor_driver_start(void);

/**
 * @brief Stop sensor
 * @return 0 success, negative error code
 */
int sensor_driver_stop(void);

/**
 * @brief Read sensor data
 * @param data Output data
 * @return 0 success, negative error code
 */
int sensor_driver_read(sensor_driver_data_t *data);

/**
 * @brief Get module status
 * @return Current status
 */
sensor_driver_status_t sensor_driver_get_status(void);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_DRIVER_H */
```

---

### Step 3: Determine Dependencies

#### 3.1 Dependency Analysis Process

```
1. List all external resources needed by the module
2. Identify what each resource is (hardware, software, data)
3. Determine how to obtain resources (direct access, interface call, event subscription)
4. Draw dependency diagram
5. Check for circular dependencies
```

#### 3.2 Dependency Types

| Type | Description | Example |
|------|-------------|---------|
| **Hardware Dependency** | Needs specific hardware peripheral | GPIO, UART, I2C, SPI |
| **System Service Dependency** | Needs system to provide services | Logging, memory, timer |
| **Data Dependency** | Needs specific data format | Sensor data, network packets |
| **Functional Dependency** | Needs other module functions | Call other module interfaces |

#### 3.3 Dependency Diagram Drawing

```c
// Analyze module dependencies
// example_module_gpio.c

#include <zeplod/example_module_gpio.h>  // Own header
#include <zeplod/app_config.h>           // Application config
#include <zephyr/kernel.h>           // Zephyr kernel

// Dependency analysis:
// 1. Hardware: GPIO peripheral (through Zephyr API)
// 2. System services: Logging (sys_log)
// 3. Event system: Publish events (event_system)
// 4. Memory: k_malloc/k_free (Zephyr)

// Dependency diagram:
/*
                    ┌─────────────┐
                    │  Zephyr Kernel │
                    └──────┬──────┘
                           │
          ┌─────────────────┼─────────────────┐
          │                 │                 │
          ▼                 ▼                 ▼
    ┌───────────┐    ┌───────────┐    ┌───────────┐
    │   GPIO    │    │  sys_log  │    │   Event   │
    │  Driver   │    │   Service │    │   System  │
    └───────────┘    └───────────┘    └───────────┘
          │                 │                 │
          └─────────────────┼─────────────────┘
                           │
                           ▼
                    ┌───────────────┐
                    │ gpio_module  │
                    │    (self)    │
                    └───────────────┘
*/
```

#### 3.4 Circular Dependency Detection

**Circular Dependency Example (Wrong):**

```c
// ❌ Circular dependency: A → B → C → A
// module_a.c
#include "module_b.h"  // Depends on B
int a_func(void) {
    b_func();  // Call B
}

// module_b.c
#include "module_c.h"  // Depends on C
int b_func(void) {
    c_func();  // Call C
}

// module_c.c
#include "module_a.h"  // Depends on A - Circular!
int c_func(void) {
    a_func();  // Call A
}
```

**Solution: Event Decoupling**

```c
// ✅ Solution: Break circular dependency through event decoupling
// module_a.c - Publish events, don't call directly
event_publish_copy(EVENT_TYPE_A_DONE, ...);

// module_c.c - Subscribe to events, don't call directly
event_subscribe(EVENT_TYPE_A_DONE, c_handler, ...);
```

#### 3.5 This Framework's Dependency Configuration

```c
// Declare dependencies in module interface
static const char *module_gpio_depends[] = {
    "sys_log",        // Needs logging service
    NULL              // Must end with NULL
};

static module_interface_t gpio_module_interface = {
    .name = "gpio_module",
    .depends_on = module_gpio_depends,
    .init = gpio_module_init,
    .start = gpio_module_start,
    // ...
};
```

---

### Step 4: Design Data Structures

#### 4.1 Data Structure Design Principles

| Principle | Description | Example |
|-----------|-------------|---------|
| **Cohesive** | Data and functions operating on it together | `sensor_data_t` and `sensor_driver.c` |
| **Minimal** | Only include necessary fields | Sensor module doesn't need network config |
| **Encapsulated** | Hide internal details | Header only exposes necessary types |
| **Extensible** | Consider future changes | Reserve version field |

#### 4.2 Data Structure Design Example

```c
// ❌ Bad data structure: Unclear responsibilities
typedef struct {
    uint32_t temp;           // Sensor data
    uint32_t hum;
    char device_name[32];    // Device info
    uint8_t mac_addr[6];     // Network info - unrelated
    uint32_t log_level;      // Log config - unrelated
} bad_sensor_t;

// ✅ Good data structure: Single responsibility
typedef struct {
    uint32_t temp;           // Temperature value
    uint32_t hum;            // Humidity value
    uint32_t timestamp;      // Sample timestamp
    uint16_t status;         // Sensor status flags
} sensor_data_t;

typedef struct {
    char name[32];           // Device name
    uint8_t addr;            // I2C address
    uint32_t sample_rate;    // Sample rate
} sensor_config_t;
```

#### 4.3 This Framework's Data Structure Practice

```c
// include/zeplod/event_system.h
typedef struct {
    event_type_t type;           /**< Event type */
    event_priority_t priority;   /**< Event priority */
    const void *data;            /**< Event data pointer */
    size_t len;                   /**< Data length */
    uint32_t timestamp;           /**< Event generation timestamp */
} event_t;

// event_type_t defined in app_config.h
typedef uint8_t event_type_t;
typedef uint8_t event_priority_t;

// Event type enum (defined in app_config.h)
typedef enum {
    EVENT_TYPE_INIT = 0,
    EVENT_TYPE_START,
    EVENT_TYPE_STOP,
    EVENT_TYPE_GPIO_STATE_CHANGED,
    EVENT_TYPE_UART_DATA_RECEIVED,
    EVENT_TYPE_SENSOR_DATA_READY,
    // ... extensible
    EVENT_TYPE_MAX
} event_type_enum_t;
```

---

### Step 5: Implement Module

#### 5.1 Module Implementation Template

```c
/**
 * @file <module_name>.c
 * @brief <Module brief description>
 *
 * <Detailed description including design decisions, notes, etc.>
 *
 * @author <Author>
 * @version X.Y
 * @date YYYY-MM-DD
 */

#include "<module_name>.h"
#include <zeplod/app_config.h>

#ifdef CONFIG_<MODULE_NAME_UPPER>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(<module_name>, LOG_LEVEL_DBG);

//==============================================================================
// Private Definitions
//==============================================================================

#define MODULE_NAME_MAX_RETRIES  3

static <module_name>_config_t g_config;
static <module_name>_status_t g_status = <MODULE_NAME>_STATUS_NONE;

//==============================================================================
// Private Function Declarations
//==============================================================================

static int private_function(int param);
static void event_handler(const event_t *event, void *user_data);

//==============================================================================
// Public Interface Implementation
//==============================================================================

int <module_name>_init(<module_name>_config_t *config) {
    if (g_status != <MODULE_NAME>_STATUS_NONE) {
        LOG_WRN("Module already initialized");
        return <MODULE_NAME>_OK;
    }

    if (config != NULL) {
        g_config = *config;
    } else {
        // Use default config
        memset(&g_config, 0, sizeof(g_config));
    }

    // Module-specific initialization
    int ret = private_function(0);
    if (ret != 0) {
        LOG_ERR("Init failed: %d", ret);
        g_status = <MODULE_NAME>_STATUS_ERROR;
        return ret;
    }

    g_status = <MODULE_NAME>_STATUS_INIT;
    LOG_INF("Module initialized");
    return <MODULE_NAME>_OK;
}

int <module_name>_start(void) {
    if (g_status == <MODULE_NAME>_STATUS_ERROR) {
        return <MODULE_NAME>_ERR_HW;
    }

    if (g_status == <MODULE_NAME>_STATUS_RUNNING) {
        return <MODULE_NAME>_OK;
    }

    // Start logic
    g_status = <MODULE_NAME>_STATUS_RUNNING;
    LOG_INF("Module started");
    return <MODULE_NAME>_OK;
}

int <module_name>_stop(void) {
    if (g_status == <MODULE_NAME>_STATUS_NONE) {
        return <MODULE_NAME>_OK;
    }

    // Stop logic
    g_status = <MODULE_NAME>_STATUS_NONE;
    LOG_INF("Module stopped");
    return <MODULE_NAME>_OK;
}

<module_name>_status_t <module_name>_get_status(void) {
    return g_status;
}

//==============================================================================
// Private Function Implementation
//==============================================================================

static int private_function(int param) {
    // Private implementation
    return 0;
}

static void event_handler(const event_t *event, void *user_data) {
    switch (event->type) {
        case EVENT_TYPE_<OTHER>_CHANGED:
            // Handle event
            break;
        default:
            break;
    }
}

//==============================================================================
// Module Auto-Registration (Zephyr SYS_INIT)
//==============================================================================

static int <module_name>_auto_register(void) {
    uint32_t id;
    int ret = module_manager_register(&<module_name>_interface, &g_config, &id);
    if (ret != 0) {
        LOG_ERR("Register failed: %d", ret);
        return -EIO;
    }
    return 0;
}

SYS_INIT(<module_name>_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_<NAME>);

#endif /* CONFIG_<MODULE_NAME_UPPER> */
```

---

### Step 6: Write Tests

#### 6.1 Testing Strategy

| Test Type | Test Content | Test Timing |
|-----------|-------------|-------------|
| **Unit Test** | Module internal logic | During development |
| **Integration Test** | Module interactions | During integration |
| **Regression Test** | Modifications don't break existing functionality | Every commit |

#### 6.2 Unit Test Example

```c
// tests/test_sensor_driver.c
#include <zephyr/ztest.h>
#include "sensor_driver.h"

static void test_sensor_driver_init(void) {
    sensor_driver_config_t config = {
        .sample_rate = 10,
        .threshold_high = 100,
        .threshold_low = 0,
    };

    int ret = sensor_driver_init(&config);
    zassert_equal(ret, SENSOR_DRIVER_OK, "Init failed");
}

static void test_sensor_driver_read(void) {
    sensor_driver_data_t data;
    int ret = sensor_driver_read(&data);
    zassert_equal(ret, SENSOR_DRIVER_OK, "Read failed");
    zassert_true(data.timestamp > 0, "Invalid timestamp");
}

ZTEST(sensor_driver, test_init) {
    test_sensor_driver_init();
}

ZTEST(sensor_driver, test_read) {
    test_sensor_driver_read();
}
```

---

### Step 7: Write Documentation

#### 7.1 Documentation Content Checklist

```
□ Module Overview (Purpose)
□ Interface Description (API Reference)
□ Usage Example (Usage Example)
□ Configuration Description (Configuration)
□ Notes/Caveats
□ Error Code Description (Error Codes)
□ Version History (Changelog)
```

#### 7.2 Documentation Template

```c
/**
 * @defgroup sensor_driver Sensor Driver Module
 *
 * @brief Sensor driver module, responsible for collecting temperature and humidity data
 *
 * ## Overview
 *
 * This module encapsulates the BME280 sensor driver, providing a unified sensor data collection interface.
 *
 * ## Features
 *
 * - Supports temperature, humidity, pressure collection
 * - Configurable sample rate
 * - Threshold alarm support
 *
 * ## Dependencies
 *
 * - Zephyr I2C driver
 * - sys_log system logging service
 *
 * ## Usage Example
 *
 * @code
 * sensor_driver_config_t config = {
 *     .sample_rate = 10,
 *     .threshold_high = 80,
 *     .threshold_low = 20,
 * };
 *
 * sensor_driver_init(&config);
 * sensor_driver_start();
 *
 * sensor_driver_data_t data;
 * sensor_driver_read(&data);
 * LOG_INF("Temp: %d, Hum: %d", data.temp, data.hum);
 * @endcode
 *
 * @{
 */

/**
 * @brief Initialize sensor driver
 *
 * Initialize I2C bus and configure sensor parameters.
 *
 * @param config Configuration parameters, NULL uses default
 * @return 0 success, negative error code
 */
int sensor_driver_init(sensor_driver_config_t *config);

// ... More interface documentation
```

---

## 5. Module Partitioning Practice: How to Identify Module Boundaries

### 5.1 Module Boundary Identification Methods

#### Method 1: Single Responsibility Check

```
Question: What is this module's responsibility?
Answer: (If answer exceeds one sentence, may need to split)
```

#### Method 2: Change Point Analysis

```
Find parts of requirements that change independently; each change point may be a module:

Requirement: "Users can connect to device via Bluetooth or WiFi, after connection can view sensor data or control relay"

Change points:
- Communication method (Bluetooth vs WiFi) → Communication module
- Sensor data reading → Sensor module
- Relay control → Control module

Boundaries: Communication method and control method are independent
```

#### Method 3: Domain Division

```
Common embedded system domains:
├── Drivers Layer
│   ├── GPIO
│   ├── UART
│   ├── I2C
│   ├── SPI
│   └── ADC
├── Protocol Layer
│   ├── MQTT
│   ├── CoAP
│   └── Modbus
├── Business Layer
│   ├── Data collection
│   ├── Alarm processing
│   └── User interaction
└── System Layer
    ├── Logging
    ├── Configuration
    └── Upgrades
```

### 5.2 Module Boundary Decision Tree

```
Start
  │
  ▼
Is this function a single, complete responsibility?
  │
  ├─ Yes → Can it be independently tested?
  │         │
  │         ├─ Yes → Consider as independent module
  │         │
  │         └─ No → Merge into related module
  │
  └─ No → Split into multiple single responsibilities
          │
          ▼
        Do these responsibilities change together?
          │
          ├─ Yes → Consider merging
          │
          └─ No → Keep separate
```

### 5.3 This Framework's Module Partitioning Example

```
src/
├── core/                    ← Core domain (framework core)
│   ├── event_system.c/h    ← Event system: publish/subscribe/dispatch
│   ├── event_queue.c/h     ← Event queue: queue operations
│   └── event_dispatcher.c/h← Event dispatcher: dispatcher thread
│
├── services/                ← Support domain (system services)
│   ├── sys_log.c/h         ← Logging service
│   ├── sys_memory.c/h      ← Memory management
│   ├── sys_timer.c/h       ← Timer service
│   └── sys_watchdog.c/h    ← Watchdog service
│
├── modules/                 ← Business domain (business modules)
│   ├── example_module_gpio.c/h    ← GPIO example
│   ├── example_module_uart.c/h    ← UART example
│   ├── example_module_ipc.c/h     ← IPC example
│   └── example_module_multi_dep.c/h ← Multi-dependency example
│
└── app/                     ← Application layer
    ├── app_main.c/h         ← Application entry
    ├── app_config.h         ← Application config
    └── app_version.c/h      ← Version info
```

**Partitioning Basis:**

| Module | Responsibility | Change Points |
|--------|---------------|---------------|
| `event_system` | Event publish/subscribe | Won't change (framework core) |
| `sys_log` | Log output | May change output target (serial/RTT/file) |
| `example_module_gpio` | GPIO control | Needs modification when hardware platform changes |

---

## 6. Interface Design Details

### 6.1 Characteristics of Good Interfaces

| Characteristic | Description | Example |
|---------------|-------------|---------|
| **Self-describing name** | Know purpose from name | `sensor_read()` vs `sensor_op()` |
| **Reasonable parameters** | Neither too many nor too few | Reading data only needs `buf, len` |
| **Clear return values** | Success/error clear | 0=success, negative=error code |
| **Consistent error handling** | Unified error code style | `-EINVAL`, `-ENOMEM` |
| **No side effects** | Calling doesn't change global state | Don't print logs internally |

### 6.2 Function Signature Design

#### 6.2.1 Input/Output Parameters

```c
// ✅ Good: Input params const, output params pointer
int sensor_driver_read(const sensor_driver_config_t *config,
                       sensor_driver_data_t *data);

// ❌ Bad: Can't distinguish input from output
int sensor_driver_read(sensor_driver_data_t *data);

// ✅ Good: Multiple related data use struct
int network_send(const network_packet_t *packet);

// ❌ Bad: Parameter list too long
int network_send(uint8_t *data, size_t len, uint8_t type,
                 uint8_t flags, uint32_t timeout);
```

#### 6.2.2 Return Value Design

```c
// ✅ Good: Return error code, result through output parameter
int sensor_driver_read(sensor_driver_data_t *data) {
    if (data == NULL) {
        return -EINVAL;
    }
    // Read data
    data->temp = read_temp();
    return 0;  // Success
}

// ❌ Bad: Negative means error, but success returns data
int sensor_driver_read(uint32_t *temp) {
    if (temp == NULL) {
        return -EINVAL;
    }
    if (error) {
        return -EIO;
    }
    *temp = actual_temp;
    return actual_temp;  // Confusing: success returns temperature, failure returns negative
}
```

### 6.3 Interface Version Management

```c
// Version info added to interface
#define SENSOR_DRIVER_API_VERSION  1

typedef struct {
    uint32_t api_version;        // Must equal SENSOR_DRIVER_API_VERSION
    // Other interface functions...
    int (*init)(void *config);
    int (*read)(sensor_driver_data_t *data);
} sensor_driver_interface_t;

// Compatibility check
int sensor_driver_check_version(const sensor_driver_interface_t *iface) {
    if (iface->api_version != SENSOR_DRIVER_API_VERSION) {
        return -EPROTONOSUPPORT;
    }
    return 0;
}
```

---

## 7. Dependency Management: Keeping Your System Clean

### 7.1 Dependency Principles

| Principle | Description | Practice |
|-----------|-------------|----------|
| **One-way dependency** | A → B, not B → A | Use events to break circular dependencies |
| **Dependency depth ≤ 3** | A → B → C → D too deep | Flatten design |
| **Depend on interfaces** | Depend on abstraction, not implementation | Define interface header files |
| **Explicit declaration** | All dependencies must be declared | `depends_on` array |

### 7.2 Dependency Depth Control

```
Deep dependency (bad)             Shallow dependency (good)

A → B → C → D                    A → B
                               A → C
                               A → D

Depth 4                          Depth 1-2
```

### 7.3 Three Ways to Break Circular Dependencies

#### First Method: Event Decoupling

```c
// ❌ Circular dependency
// module_a.c → module_b.c → module_a.c

// ✅ Break it: Event decoupling
// module_a.c only publishes events
event_publish_copy(EVENT_TYPE_A_REQ, ...);

// module_b.c only subscribes to events
event_subscribe(EVENT_TYPE_A_REQ, b_handle_a_req, ...);

// module_a.c subscribes to response events
event_subscribe(EVENT_TYPE_B_RESP, a_handle_b_resp, ...);
```

#### Second Method: Dependency Injection

```c
// ✅ Use callbacks to break circular dependency

// module_a.h
typedef void (*a_callback_t)(void* data);
void module_a_set_callback(a_callback_t cb, void* user_data);

// module_b.c
void module_b_init(void) {
    module_a_set_callback(on_a_event, NULL);
}
```

#### Third Method: Shared Interface

```c
// ✅ Extract common interface
// common_interface.h
// module_a.h (only includes common_interface.h)
// module_b.h (only includes common_interface.h)
```

---

## 8. Module Lifecycle Management

### 8.1 Lifecycle States

```
┌─────────────────────────────────────────────────────────────┐
│                      Module Lifecycle                        │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌───────────┐    init()     ┌───────────┐    start()      │
│  │  NONE     │ ───────────▶  │   INIT    │ ───────────▶    │
│  └───────────┘               └───────────┘                 │
│                                  │                           │
│                           ┌──────┴──────┐                    │
│                           ▼             ▼                    │
│                    ┌───────────┐   ┌───────────┐             │
│                    │  ERROR    │   │ RUNNING   │             │
│                    └───────────┘   └─────┬─────┘             │
│                                         │                    │
│                                    stop()│                    │
│                                         ▼                    │
│                                  ┌───────────┐               │
│                                  │  STOPPED  │               │
│                                  └───────────┘               │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

### 8.2 Lifecycle Implementation

```c
// init() - Initialization phase
// - Validate configuration parameters
// - Initialize static data structures
// - Don't start threads/timers
// - Don't allocate dynamic memory (recommended for embedded)
int my_module_init(void *config) {
    if (config == NULL) {
        return -EINVAL;
    }
    memcpy(&g_config, config, sizeof(g_config));
    g_status = MODULE_STATUS_INITIALIZED;
    return 0;
}

// start() - Start phase
// - Start worker threads
// - Start timers
// - Register interrupt handlers
// - Begin data processing
int my_module_start(void) {
    k_timer_start(&g_work_timer, K_MSEC(100), K_MSEC(100));
    k_thread_start(&g_work_thread);
    g_status = MODULE_STATUS_RUNNING;
    return 0;
}

// stop() - Stop phase
// - Stop data processing
// - Stop timers/threads
// - Keep allocated resources
int my_module_stop(void) {
    k_timer_stop(&g_work_timer);
    k_thread_suspend(&g_work_thread);
    g_status = MODULE_STATUS_STOPPED;
    return 0;
}

// shutdown() - Destruction phase
// - Release all resources
// - Clean up state
// - Can reinitialize
int my_module_shutdown(void) {
    my_module_stop();
    // Cleanup resources...
    g_status = MODULE_STATUS_NONE;
    return 0;
}
```

---

## 9. Event-Driven Architecture: How Modules Communicate

### 9.1 Communication Patterns

| Pattern | Coupling | Real-time | Use Case |
|---------|----------|-----------|----------|
| Direct function call | High | High | Synchronous operations |
| Callback function | Medium | High | Event notification |
| Event system | Low | Medium | Async decoupling |
| Message queue | Low | Medium | Buffered communication |
| Shared memory | High | High | Large data volumes |

### 9.2 Event-Driven Communication in This Framework

```c
// Publish-subscribe pattern

// 1. Define event type
typedef enum {
    EVENT_TYPE_SENSOR_DATA = 1,
    EVENT_TYPE_NETWORK_STATUS,
    EVENT_TYPE_ALARM,
} event_type_t;

// 2. Module subscribes to events
void module_init(void) {
    event_subscribe(EVENT_TYPE_SENSOR_DATA, sensor_handler, NULL, &sub_id);
    event_subscribe(EVENT_TYPE_NETWORK_STATUS, network_handler, NULL, &sub_id);
}

// 3. Module handles events
void sensor_handler(const event_t *event, void *user_data) {
    if (event->type == EVENT_TYPE_SENSOR_DATA) {
        sensor_data_t *data = (sensor_data_t *)event->data.ptr;
        // Process data
    }
}

// 4. Another module publishes events
void publish_sensor_data(sensor_data_t *data) {
    event_publish_copy(EVENT_TYPE_SENSOR_DATA, EVENT_PRIORITY_NORMAL,
                       data, sizeof(sensor_data_t));
}
```

---

## 10. Modular Architecture Patterns and Selection

### 10.1 Common Patterns

| Pattern | Description | Suitable For |
|---------|-------------|--------------|
| **Layered** | Clear layer-by-layer dependency | General embedded systems |
| **Event-Driven** | Modules communicate via events | Loosely coupled systems |
| **Modular** | Independent replaceable modules | Large-scale systems |
| **Monolithic** | All in one module | Simple tiny systems |

### 10.2 Pattern Selection Guide

```
                    System Scale
                         │
         ┌───────────────┼───────────────┐
         │               │               │
        Small          Medium          Large
         │               │               │
         ▼               ▼               ▼
    ┌─────────┐    ┌───────────┐    ┌───────────┐
    │Monolithic│   │ Event-    │    │  Modular  │
    │         │    │ Driven    │    │  + Layered│
    └─────────┘    └───────────┘    └───────────┘
```

---

## 11. Code Quality Assurance

### 11.1 Code Standards

- Follow this framework's coding standards (see AGENTS.md)
- Use meaningful naming
- Write comments for complex logic
- Keep functions short (recommended < 50 lines)

### 11.2 Static Analysis

```bash
# Run clang-tidy for static analysis
clang-tidy -p build src/modules/*.c

# Run clang-format for formatting
clang-format -i src/modules/*.c
```

### 11.3 Testing Requirements

- All public APIs must have unit tests
- Integration tests for module interactions
- Regression tests for bug fixes

---

## 12. Common Problems and Solutions

### Problem 1: God Modules

**Symptom**: One module does everything

**Solution**: Split by responsibility following SRP

### Problem 2: Circular Dependencies

**Symptom**: A → B → C → A

**Solution**: Use event decoupling, dependency injection

### Problem 3: High Coupling

**Symptom**: Modules directly access each other's internals

**Solution**: Use interfaces, event communication

### Problem 4: Low Cohesion

**Symptom**: Unrelated functions in one module

**Solution**: Split into focused modules by function

---

## 13. This Framework's Modular Implementation Guide

### 13.1 Module Creation Steps

1. **Define module priority** (in `include/zeplod/app_config.h`):
```c
#define APP_INIT_PRIO_MODULE_MINE  60  // Between MODULE_MGR(54) and APP_FINAL(99)
```

2. **Create module header** (`include/zeplod/my_module.h`):
```c
#ifndef MY_MODULE_H
#define MY_MODULE_H

#include <zeplod/module_base.h>

typedef struct {
    uint32_t param1;
    uint32_t param2;
} my_module_config_t;

int my_module_init(void *config);
int my_module_start(void);
int my_module_stop(void);
int my_module_shutdown(void);
void my_module_on_event(const event_t *event, void *user_data);
module_status_t my_module_get_status(void);
int my_module_control(int cmd, void *arg);

DECLARE_MODULE_INTERFACE(my_module);

#endif
```

3. **Implement module** (`src/modules/my_module.c`):
```c
#include "my_module.h"
#include <zeplod/app_config.h>

static my_module_config_t g_config;

int my_module_init(void *config) {
    if (config != NULL) {
        g_config = *(my_module_config_t *)config;
    }
    return 0;
}

// ... implement other lifecycle functions ...

DECLARE_MODULE_INTERFACE(my_module);

static int my_module_auto_register(void) {
    uint32_t id;
    return module_manager_register(&my_module_interface, &g_config, &id) ? -EIO : 0;
}
SYS_INIT(my_module_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_MINE);
```

4. **Add to CMakeLists.txt** (add source file in alphabetical order)

### 13.2 Event System Usage Patterns

```c
// Subscribe to event
event_subscribe(EVENT_TYPE_SENSOR_DATA, my_callback, user_data, &subscriber_id);

// Publish event
event_publish_copy(EVENT_TYPE_SENSOR_DATA, EVENT_PRIORITY_NORMAL, &data, sizeof(data));

// Publish from ISR
event_publish_from_isr(&my_event);
```

---

## 14. Summary and Checklist

### 14.1 Design Checklist

```
□ Does each module have a single, clear responsibility?
□ Is the interface minimal and self-describing?
□ Are dependencies declared explicitly?
□ Is coupling low (preferably data coupling)?
□ Is cohesion high (preferably functional cohesion)?
□ Can modules be independently tested?
□ Is the lifecycle clearly defined (init/start/stop/shutdown)?
□ Are error codes returned consistently?
```

### 14.2 Implementation Checklist

```
□ Follows interface definition template?
□ All public functions have documentation comments?
□ Error codes follow framework conventions?
□ Registered via SYS_INIT?
□ Subscribes to needed events in init()?
□ Publishes events instead of direct calls?
```

### 14.3 Review Checklist

```
□ Code follows naming conventions?
□ No global variables (except module state)?
□ All return values checked?
□ Thread safety considered?
□ No circular dependencies?
□ Documentation complete?
```

---

**Document Version: 1.0.0**
**Last Updated: 2026-04-14**
