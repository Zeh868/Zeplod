# Zeplod

A high-performance, real-time, event-driven application foundation based on Zephyr RTOS. Zeplod uses the publish-subscribe pattern, providing a solid base for building scalable and modular embedded applications.

> **Language**: [中文版](README_CN.md) | English

> **Video Tutorials**: Follow the WeChat Official Account **硬核嵌入式 (Hardcore Embedded)** for detailed tutorials on this framework!

---

## Quick Start for Newcomers

| I want to... | Read | Time |
|--------------|------|------|
| **Try it in 5 minutes** | [docs/en/00-getting-started/01-quick-start.md](docs/en/00-getting-started/01-quick-start.md) | 5 min |
| **Set up the environment** | [docs/en/10-environment-build/11-environment-setup.md](docs/en/10-environment-build/11-environment-setup.md) | 1-2 hrs |
| **Start coding** | [docs/en/00-getting-started/04-developer-guide.md](docs/en/00-getting-started/04-developer-guide.md) | 1 hr |
| **Control hardware (LED)** | [docs/en/00-getting-started/04-developer-guide.md](docs/en/00-getting-started/04-developer-guide.md) | 30 min |
| **Look up terminology** | [docs/en/00-getting-started/03-glossary.md](docs/en/00-getting-started/03-glossary.md) | Anytime |
| **Browse all docs** | [docs/en/00-getting-started/02-doc-index.md](docs/en/00-getting-started/02-doc-index.md) | - |

> **Tip**: First time here? Start with **[5-Minute Quick Start](docs/en/00-getting-started/01-quick-start.md)** — no dev board needed, runs on PC!

---

## Features

- **Event-Driven Architecture**: Core event system based on publish-subscribe pattern
- **Modular Design**: Dynamic module registration and lifecycle management
- **Real-Time Performance**: Priority-based scheduling with configurable event dispatching
- **System Services**: Integrated logging, memory management, watchdog, and timer services
- **Scalability**: Easy-to-extend module interfaces for adding business logic
- **Thread Safety**: Full thread-safe operations with proper synchronization
- **Shell Commands**: Built-in shell commands for debugging and monitoring
- **Version Tracking**: Complete software version tracking with Git info and build time
- **Thread IPC (In-App)**: Optional worker + dispatcher threads, request/response queues, and event system bridge (see `docs/en/30-core-modules/33-thread-ipc-service-guide.md`)
- **Data Bus**: Named-channel, reference-counted streaming data sharing with zero-copy multi-consumer dispatch, ISR/thread unified publish, auto-release with explicit retain, optional event system bridge (see `docs/en/30-core-modules/37-data-bus-guide.md`)

## Use Cases

This framework, based on event-driven architecture and modular design, is particularly suitable for the following embedded application scenarios:

### Industrial Control & Automation

| Scenario | Description |
|----------|-------------|
| **Industrial Gateway** | Multi-protocol data acquisition, protocol conversion, edge computing; event system naturally supports decoupled processing of heterogeneous data streams |
| **PLC/DCS Controller** | Real-time control logic, I/O module management; modular architecture supports hot-pluggable feature expansion |
| **Data Acquisition Terminal** | Sensor data aggregation, local storage, remote transmission; event queues provide buffering |
| **Industrial IoT Node** | Device status monitoring, predictive maintenance data reporting |

### IoT Devices

| Scenario | Description |
|----------|-------------|
| **Smart Sensor Node** | Multi-sensor data fusion, event-triggered reporting; low-power design supports battery operation |
| **Edge Computing Device** | Local data processing, AI inference pre-processing; IPC service supports multi-threaded parallel computation |
| **IoT Gateway** | Protocol conversion, device management, cloud communication; modular design eases protocol extension |
| **Environmental Monitor** | Weather, water quality, air quality monitoring; timed sampling + event-driven alerts |

### Smart Home & Building

| Scenario | Description |
|----------|-------------|
| **Smart Hub/Gateway** | Multi-device coordination, scene linkage; event system enables decoupled device communication |
| **Smart Switch/Panel** | Touch interaction, lighting control, scene modes; modularity supports different feature combinations |
| **Security Alarm Host** | Sensor linkage, local alarm, remote notification; real-time event dispatch ensures response speed |
| **HVAC Controller** | Temperature/humidity control, scheduled tasks, energy management |

### Automotive Electronics

| Scenario | Description |
|----------|-------------|
| **Vehicle Sensor Node** | Tire pressure monitoring, temperature sensing, door control; event-driven architecture reduces polling overhead |
| **Body Control Module (BCM)** | Lighting, windows, door locks; modular design supports feature trimming |
| **Dashboard Controller** | Signal processing, display driving, alarm logic; priority events guarantee real-time critical alerts |
| **Vehicle Diagnostic Device** | OBD data acquisition, fault code parsing, data upload |

### Medical Electronics

| Scenario | Description |
|----------|-------------|
| **Portable Monitor** | Heart rate, blood oxygen, body temperature monitoring; event-triggered abnormal alarms |
| **Medical Data Collector** | Multi-parameter monitor data aggregation, local storage, network upload |
| **Rehabilitation Device** | Motion data acquisition, feedback control, training records |
| **Medical IoT Node** | Device status monitoring, consumable management, remote diagnosis support |

### Robotics & Automation

| Scenario | Description |
|----------|-------------|
| **Service Robot Controller** | Sensor fusion, behavior decision, motion control; event system supports asynchronous sensor data processing |
| **AGV/AMR Navigation** | Path planning, obstacle avoidance, task scheduling; IPC service supports multi-threaded collaboration |
| **Robotic Arm Node** | Joint control, force feedback processing, safety monitoring |
| **UAV Flight Controller** | Attitude sensing, motor control, communication link; high-priority event handling for emergencies |

### Consumer Electronics

| Scenario | Description |
|----------|-------------|
| **Wearable Device** | Motion data acquisition, health monitoring, message alerts; low-power event-driven reduces CPU wake-ups |
| **Smart Speaker** | Voice wake-up, audio processing, cloud interaction; modular architecture separates front-end/back-end logic |
| **E-Reader/E-Label** | Display refresh, button response, wireless updates |
| **Gaming Peripheral** | Gamepad, steering wheel, flight stick; low-latency event processing ensures response speed |

### Communication Devices

| Scenario | Description |
|----------|-------------|
| **Wireless Sensor Network** | Data acquisition, multi-hop routing, low-power sleep |
| **Serial Server/CAN Gateway** | Protocol conversion, data forwarding, device management |
| **Modbus/Profibus Device** | Slave device, protocol stack implementation; modularity eases porting to different protocols |
| **LoRa/NB-IoT Terminal** | Remote meter reading, asset tracking, environmental monitoring |

### Education & Learning

| Scenario | Description |
|----------|-------------|
| **RTOS Teaching Platform** | Learn event-driven, multi-threading, modular design and other modern embedded software engineering practices |
| **Embedded Course Project** | Complete project structure, code standards, test framework as reference template |
| **Graduation Project/Research** | Rapid prototyping, focus on business logic rather than underlying architecture |
| **Enterprise Training** | Unified development standards, code style, CI/CD workflow |

### Prototype & Rapid Validation

| Scenario | Description |
|----------|-------------|
| **Product Prototype Development** | Rapid concept validation, native_sim supports PC simulation testing |
| **Technical Evaluation** | Performance testing, resource evaluation, feasibility analysis |
| **Driver/Protocol Development** | Isolate hardware-related code for easy porting and testing |
| **Multi-Platform Product Line** | Unified architecture, differentiated modules, reduced maintenance cost |

### Framework Selection Guide

This framework is **especially suitable** for projects with these characteristics:

- Modular, scalable architecture needed
- Multi-sensor/multi-peripheral collaboration
- Real-time requirements (event priority support)
- Thread-safe inter-module communication needed
- Multiple product variants, feature trimming required
- Team collaboration, unified standards needed
- **32KB SRAM extreme scenarios** (use `conf/profiles/tiny.conf` extreme config)

This framework is **less suitable** for:

- Extremely simple MCU projects (resource-constrained, e.g. 8-bit MCU)
- Pure front-end/back-end or super-loop architecture suffices
- Products extremely sensitive to Flash/RAM footprint
- Standalone functional devices without inter-module communication

> **32KB SRAM Solution**: Using `conf/profiles/tiny.conf` extreme config, framework footprint is **< 10KB**, leaving **> 22KB** for APP modules. See [Configuration Comparison Guide](docs/en/40-app-development/43-config-comparison-guide.md) and [conf/README.md](conf/README.md).

### Memory Configuration Options

| Configuration | Target SRAM | Framework | APP Available | Config Files |
|---------------|-------------|-----------|---------------|--------------|
| Standard | >= 256KB | ~190KB | ~66KB+ | Default `west build` (CMake merges standard + features) |
| Balanced | 64-128KB | ~40KB | ~24-88KB | `-DEXTRA_CONF_FILE=conf/profiles/balanced.conf` |
| Minimal | 32-64KB | ~18KB | ~14-46KB | `-DEXTRA_CONF_FILE=conf/profiles/minimal.conf` |
| **Extreme** | **<= 32KB** | **< 10KB** | **> 22KB** | `-DCONF_FILE="prj.conf;conf/profiles/tiny.conf"` |

## Project Structure

```
zeplod/
├── APP_VERSION                       # App semantic version (do NOT name it VERSION — conflicts with Zephyr)
├── CMakeLists.txt                    # Build config (freestanding app needs ZEPHYR_BASE or zephyr_config.env)
├── Kconfig                           # Application Kconfig (events/modules/IPC etc.)
├── Kconfig.zephyr                    # Zephyr top-level Kconfig entry
├── prj.conf                          # Common baseline (framework core switches)
├── conf/                             # Overlay fragments (see conf/README.md)
│   ├── profiles/                     # standard (default) / balanced / minimal / tiny
│   ├── features/                     # data_bus, thread_ipc (default) + optional
│   ├── targets/                      # qemu
│   └── examples/                     # gpio_uart, module_ipc
├── app.overlay                       # Common device tree overlay
├── west.yml                          # West manifest (default 4.3.0-rc3)
├── zephyr_config.env                 # Local paths (copied from template, do NOT commit secrets)
├── zephyr_config.env.template
├── Doxyfile                          # API documentation generation config
├── .clang-format                     # Code formatting config
├── .clang-tidy                       # Static analysis config
├── .pre-commit-config.yaml           # pre-commit hooks config
├── boards/
│   ├── overlay.dts                   # Common device tree overlay
│   ├── nucleo_l4r5zi.overlay         # STM32 Nucleo L4R5ZI board overlay
│   └── mimxrt1050_fire_mimxrt1052_qspi.overlay
├── scripts/                          # Environment scripts, packaging, versioning
│   ├── setup_env.{sh,bat,ps1}        # Environment variable setup
│   ├── build_all.{sh,bat}            # Batch build scripts
│   ├── analyze_map.{sh,bat,ps1}      # MAP file analysis
│   ├── package_release.{sh,ps1}      # Release packaging
│   ├── bump_version.py               # Version sync
│   └── module_config.py              # Module config tool
├── tests/                            # ztest unit tests (native_sim)
│   ├── CMakeLists.txt
│   ├── Kconfig                       # rsource to reuse root Kconfig
│   ├── prj.conf
│   ├── prj_native_sim.conf           # native_sim platform overlay
│   ├── prj_test_watchdog.conf        # Watchdog test overlay
│   ├── README.md
│   ├── test_event_system.c
│   ├── test_event_queue.c
│   ├── test_event_dispatcher.c
│   ├── test_event_memory.c
│   ├── test_module_manager.c
│   ├── test_ipc_service.c
│   ├── test_sys_log.c
│   ├── test_sys_memory.c
│   ├── test_sys_timer.c
│   ├── test_sys_watchdog.c
│   ├── test_example_module_a.c
│   ├── test_example_module_b.c
│   ├── test_example_module_gpio.c
│   ├── test_example_module_uart.c
│   └── test_example_module_multi_dep.c
├── docs/                             # Documentation (index at docs/en/00-getting-started/02-doc-index.md)
│   ├── 00-入门/                       # Quick start, doc index, glossary, dev guide
│   ├── 10-环境与构建/                 # Environment setup, freestanding build
│   ├── 20-架构设计/                   # Modular design methodology, core implementation
│   ├── 30-核心模块/                   # Event system, module system, IPC, system services
│   ├── 40-应用开发/                   # App development, config items, comparison, devicetree
│   ├── 50-测试与CI/                   # Unit tests, CI config
│   ├── 60-调试与排错/                 # Flashing, debugging, troubleshooting
│   ├── 70-发布与产品化/               # Versioning, release checklist, OTA, security
│   ├── 80-贡献与维护/                 # Contribution guide, code standards
│   └── 90-学习资源/                   # Embedded AI, career development, project review
└── src/
    ├── core/                          # Core event system
    │   ├── event_system.{c,h}         # Publish-subscribe, type management
    │   ├── event_queue.{c,h}          # Event queues (priority, overflow handling)
    │   ├── event_dispatcher.{c,h}     # Event dispatcher (dedicated thread, stats, pause/resume)
    │   ├── event_dispatcher_autoinit.c # SYS_INIT auto-initialization
    │   ├── event_memory.{c,h}         # Slab memory management (priority-tiered pools, large-block pools)
    │   └── event_system_compat.{c,h}  # Event system compatibility layer
    ├── data_bus/                      # Data Bus: named-channel streaming data sharing
    │   ├── data_bus.{c,h}             # Core: init/deinit, dispatcher thread, stats
    │   ├── data_bus_channel.{c,h}     # Channel management (create/find/destroy/publish)
    │   ├── data_bus_consumer.{c,h}    # Consumer registration and dispatch
    │   ├── data_bus_memory.{c,h}      # Two-tier Slab pools + reference-counted lifecycle
    │   ├── data_bus_event_bridge.c    # Event system bridge (optional)
    │   ├── data_bus_internal.h        # Internal shared declarations
    │   └── Kconfig                    # Data Bus Kconfig options
    ├── include/zeplod/                # Public API headers (#include <zeplod/...>)
    │   ├── zeplod_framework.h         # Umbrella: event system + state machine + lock order
    │   ├── event_system.h
    │   ├── module_manager.h
    │   ├── app_config.h
    │   └── ...
    ├── services/                      # System services (implementation)
    │   ├── sys_log.c
    │   ├── sys_memory.c
    │   ├── sys_watchdog.c
    │   └── sys_timer.c
    ├── modules/                       # Module manager & Thread IPC (implementation)
    │   ├── module_manager.c
    │   ├── module_manager_compat.c
    │   └── ipc_service/               # Thread IPC service (Kconfig: THREAD_IPC_SERVICE)
    │       ├── ipc_service.c
    │       ├── ipc_service_event.c
    │       ├── ipc_shared_mem.c
    │       ├── ipc_service_example.c
    │       ├── CMakeLists.txt
    │       └── Kconfig
    ├── modules_examples/              # Example module .c (headers in include/zeplod/)
    │   ├── example_module_a.c
    │   ├── example_module_b.c
    │   ├── example_module_gpio.c
    │   ├── example_module_uart.c
    │   ├── example_module_ipc.c
    │   └── example_module_multi_dep.c
    ├── app/                           # Application layer (implementation)
    │   ├── app_main.c
    │   ├── app_version.c              # Version API (macros injected via -D from root APP_VERSION)
    │   └── app_kv.c                   # Application key-value store (optional persist on power loss)
```

## Checklist After Forking or Copying This Repo

After copying or forking this repo, complete the following steps in order for easy deployment on any product (more details in **[docs/en/00-getting-started/04-developer-guide.md](docs/en/00-getting-started/04-developer-guide.md#从模板复制后的检查清单)**).

| Step | Content |
|------|---------|
| 1 | **west.yml**: Align `revision` with your team's Zephyr version (default **4.3.0-rc3**, same as CI); private mirrors can change `url`. |
| 2 | **zephyr_config.env**: Copy from `zephyr_config.env.template` and fill in paths; **do NOT commit** (already in `.gitignore`). |
| 3 | **CMake**: In root `CMakeLists.txt`, change `project(...)` name to your product project name. |
| 4 | **Version & Description**: Modify `APP_VERSION`, README title and product description as needed. |
| 5 | **Board & CI**: In `prj.conf`, **`.github/workflows/ci.yml`** / **`.gitlab-ci.yml`**, make ARM matrix `board` match target hardware or trim as needed. |
| 6 | **Example Code**: `src/modules_examples/example_*` can be deleted or replaced (headers under `include/zeplod/`); sync `CMakeLists.txt`, Kconfig, each module **`.c` `SYS_INIT` registration**, and **`include/zeplod/app_config.h` `APP_INIT_PRIO_*`**. |

> **Board examples in docs and CI**: Getting started docs may mention `nucleo_l4r5zi` and other example boards; CI is currently fixed to some Nucleo/Disco boards. **Use your actual `BOARD` and CI matrix as the source of truth**; for RAM/link issues, see **[docs/en/40-app-development/44-devicetree-memory-config.md](docs/en/40-app-development/44-devicetree-memory-config.md)**.

## Quick Start

### Prerequisites

- Zephyr SDK (0.16.x or later)
- CMake (3.20.0 or later)
- Python 3.8+
- West (Zephyr build tool)

CI (`.github/workflows/ci.yml`) currently uses Zephyr **4.3.0-rc3** build image; locally, use the same or compatible Zephyr version to reduce configuration differences.

### Project Type: Freestanding Application

This project is a **Zephyr freestanding application** — the application and Zephyr source code are in separate directories.

```
<home>/
├── zephyrproject/          # Zephyr workspace
│   ├── .west/
│   ├── zephyr/             # Zephyr source code (ZEPHYR_BASE)
│   ├── modules/            # Zephyr modules
│   └── ...
└── zeplod/       # Application directory (this directory)
    ├── CMakeLists.txt
    ├── prj.conf
    ├── src/
    └── ...
```

**Important**: Build requires `ZEPHYR_BASE` environment variable to point to the Zephyr source directory.

### Configuration (Local Paths)

This project uses local Zephyr SDK and source code paths. Configure before building:

#### Method 1: Edit Config File

1. Copy `zephyr_config.env.template` to `zephyr_config.env`
2. Edit paths in `zephyr_config.env`:

```bash
# Edit this file with your local paths
ZEPHYR_SDK_INSTALL_DIR=C:/zephyr-sdk
ZEPHYR_BASE=D:/zephyrproject/zephyr
```

#### Method 2: Run Setup Script

```bash
# Windows (PowerShell)
.\scripts\setup_env.ps1

# Windows (CMD)
scripts\setup_env.bat

# Linux/macOS
source scripts/setup_env.sh
```

#### Method 3: Set Environment Variables

```bash
# Windows (PowerShell)
$env:ZEPHYR_BASE="D:/zephyrproject/zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR="C:/zephyr-sdk"

# Linux/macOS
export ZEPHYR_BASE=/path/to/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk
```

### Build

```bash
# Build for target board
west build -b <your_board> .

# Build for native_sim (for host testing)
west build -b native_sim .

# Use specific config files (can merge multiple)
west build -b <your_board> . -- -DCONF_FILE="prj.conf;conf/examples/module_ipc.conf"

# Extreme memory mode build (framework < 10KB)
west build -b <your_board> . -- -DCONF_FILE="prj.conf;conf/profiles/tiny.conf"

# Clean and rebuild
west build -t pristine
west build -b <your_board> .
```

### Flash

```bash
west flash
```

### Monitor Output

```bash
west console
```

### Using Custom Boards

This project supports adding custom boards in the `boards/` directory without modifying Zephyr source code.

**Board directory structure**:

```
boards/
└── vendor/
    └── board_name/
        ├── board_name_defconfig
        ├── board_name.dts
        ├── board_name.yaml
        ├── board.cmake
        ├── Kconfig.defconfig
        └── ...
```

**Build command**:

```bash
# After uncommenting BOARD_ROOT in root CMakeLists.txt, custom boards can be placed under boards/
# list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/boards)
west build -b vendor/board_name .
```

**Device tree overlays**:

- Common overlay: `boards/overlay.dts`
- Board-specific overlay: `boards/<board_name>.overlay`
- Using FILE_SUFFIX: `boards/<board_name>_<suffix>.overlay`

## Architecture Overview

### Event System

The core event system provides:
- **Event Types**: 256 unique event types (0-255)
- **Event Priorities**: Low, Normal, High, Critical (lower numeric value = higher priority)
- **Subscribers**: Up to 16 subscribers per event type
- **Queue**: Configurable event queue (default 32 events)
- **Slab Memory Pools**: Priority-tiered (CRITICAL/HIGH/NORMAL) and large-block (256B/1KB/4KB)
- **Real-Time Safe APIs**: `_rt` suffix APIs allocate entirely from Slab pool, O(1) deterministic time

```c
// Subscribe to an event
event_subscribe(EVENT_TYPE_SENSOR_DATA, my_callback, user_data, &subscriber_id);

// Publish event (recommended, automatic memory management)
event_publish_copy(EVENT_TYPE_SENSOR_DATA, EVENT_PRIORITY_NORMAL, &data, sizeof(data));

// Use in ISR/real-time tasks (real-time safe, Slab pool allocation)
event_publish_copy_rt(EVENT_TYPE_SENSOR_DATA, EVENT_PRIORITY_HIGH, &data, sizeof(data));
```

### Module System

Modules are independent business logic units:
- **Lifecycle**: init -> start -> run -> stop -> shutdown
- **Registration**: Dynamically registered via module manager; firmware auto-registers in **`SYS_INIT(POST_KERNEL, APP_INIT_PRIO_*)`** (see `include/zeplod/app_config.h` and each `example_module_*.c`)
- **Event Handling**: Automatically routes events to subscribed modules
- **Isolation**: Each module has its own state and configuration

```c
// Define module interface
DECLARE_MODULE_INTERFACE(my_module);

// Typical: auto-register with Zephyr initialization in module .c (define APP_INIT_PRIO_MODULE_* in include/zeplod/app_config.h)
static int my_module_auto_register(void) {
    uint32_t id;
    return module_manager_register(my_module_get_interface(), &config, &id) ? -EIO : 0;
}
SYS_INIT(my_module_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_MINE);
```

### System Services

| Service | Description |
|---------|-------------|
| `sys_log` | Unified logging: levels, memory ring, console printk; optional UART shared with console output; writes RTT when `CONFIG_SEGGER_RTT` is enabled |
| `sys_memory` | Memory pool management with allocation tracking |
| `sys_watchdog` | Hardware/software watchdog for improved reliability |
| `sys_timer` | High-resolution one-shot and periodic timers |

### Data Bus

The Data Bus provides named-channel, reference-counted streaming data sharing, completely independent of the event system:

- **Named Channels**: Globally unique names, dynamically created/looked up at runtime
- **Zero-Copy Sharing**: Data is copied once into a slab block; multiple consumers share the same memory
- **ISR/Thread Unified Publish**: `data_bus_publish()` automatically detects context and adapts
- **Auto-Release**: Framework automatically `release`s after consumer callback returns (default)
- **Explicit Retain**: Call `data_bus_block_retain()` inside callback for asynchronous hold
- **Event Bridge**: Optional bridge to event system for lightweight notifications
- **Memory Determinism**: All from pre-allocated slabs; ISR path does not depend on `k_malloc`

```c
// Create channel and register consumer
data_bus_channel_t *ch;
data_bus_channel_create("imu", &ch);

data_bus_consumer_cfg_t cfg = {
    .name     = "imu_fusion",
    .callback = imu_consumer_cb,
};
data_bus_consumer_register(ch, &cfg, NULL);

// Publish from ISR or thread
data_bus_publish(ch, &imu_data, sizeof(imu_data));
```

> **Data Bus vs Event System**: Data Bus is for streaming data blocks (arbitrary size, reference-counted); Event System is for discrete events (fixed size, fire-and-forget). Use Data Bus for sensor streams and large payload sharing; use Event System for control commands and state notifications.

## Configuration

### Kconfig Options

```kconfig
# Event System
CONFIG_EVENT_SYSTEM=y
CONFIG_EVENT_QUEUE_SIZE=64
CONFIG_EVENT_MAX_SUBSCRIBERS=16
CONFIG_EVENT_DISPATCHER_STACK_SIZE=2048
CONFIG_EVENT_DISPATCHER_PRIORITY=5

# Module Manager
CONFIG_MODULE_MANAGER=y
CONFIG_MAX_MODULES=16
# Optional: runtime topological start / reverse-order stop by depends_on (see docs/en/30-core-modules/32-module-system-guide.md)
# CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES=y
# CONFIG_MODULE_MANAGER_DEPENDS_LIST_MAX=16
# CONFIG_MODULE_MANAGER_START_ALL_ABORT_ON_FAILURE=y
# CONFIG_EXAMPLE_MODULE_MULTI_DEP=y

# System Services
CONFIG_SYS_LOG_LEVEL=3          # 0=Off, 1=Error, 2=Warning, 3=Info, 4=Debug
CONFIG_SYS_MEMORY_POOL_SIZE=8192
CONFIG_SYS_WATCHDOG_ENABLE=y
CONFIG_SYS_WATCHDOG_TIMEOUT_MS=5000
```

### Application Configuration

Edit `include/zeplod/app_config.h` to customize:
- Feature flags (enable/disable modules and services)
- Task priorities and stack sizes
- Timing configuration
- Event type definitions

## Shell Commands

The application provides the following shell commands for debugging:

```bash
# Application status
app status

# Module info
app modules

# Event statistics
app events

# Memory statistics
app memory

# Log dump
app log [level]

# Application key-value (string key/value; persist-on-power-loss: see conf/features/app_kv_persist.conf + boards/nucleo_l4r5zi.overlay)
app kv list
app kv set mykey hello world
app kv get mykey
app kv del mykey
app kv clear
app kv save
app kv load

# Help
app help
```

## Creating Custom Modules

1. Create module header file (`include/zeplod/my_module.h`):

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

2. Implement the module (`src/modules/my_module.c`):

```c
#include <zeplod/my_module.h>

static my_module_cb_t g_my_module;

int my_module_init(void *config) {
    // Initialize module
    return 0;
}

int my_module_start(void) {
    // Start module operations
    return 0;
}

// ... implement other interface functions

DECLARE_MODULE_INTERFACE(my_module);
```

3. Register with **`SYS_INIT`** in **`my_module.c`** (add **`APP_INIT_PRIO_MODULE_MINE`** in **`include/zeplod/app_config.h`**, value between **`APP_INIT_PRIO_MODULE_MGR`** and **`APP_INIT_PRIO_APP_FINAL`**; if depending on other registered modules, use a **larger** priority number to execute later):

```c
#include <zeplod/app_config.h>
#include <zephyr/init.h>
#include <errno.h>

static int my_module_auto_register(void) {
    uint32_t id;
    return module_manager_register(&my_module_interface, &config, &id) ? -EIO : 0;
}
SYS_INIT(my_module_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_MINE);
```

For multi-module dependencies, `depends_on` syntax, and Kconfig switch descriptions, see **docs/en/30-core-modules/32-module-system-guide.md** "Application Boot and Init Order (Zephyr SYS_INIT)", "Runtime Dependencies" and "Configuration Options"; multi-dependency example source is `src/modules_examples/example_module_multi_dep.c`.

## Event Flow

```
+-------------+     +---------------+     +-------------+
|  Module A   |---->| Event System  |---->|  Module B   |
|  (Publisher)|     | (Dispatcher)  |     | (Subscriber)|
+-------------+     +---------------+     +-------------+
                           |
                           v
                    +-------------+
                    | Event Queue |
                    +-------------+
```

## Best Practices

1. **Event Design**: Keep events small and focused. Use event data pointers for large payloads.
2. **Module Isolation**: Modules should not directly call other modules. Use events for inter-module communication.
3. **Priority Assignment**: Use appropriate priorities for time-critical events.
4. **Memory Management**: Always release dynamically allocated event data.
5. **Error Handling**: Implement proper error handling in all module callbacks.
6. **Watchdog**: Feed the watchdog regularly during long-running operations.

## Debugging

### Enable Debug Logging

```kconfig
CONFIG_LOG_DEFAULT_LEVEL=4
CONFIG_SYS_LOG_LEVEL=4
```

### Check Statistics

```c
// Event system statistics
uint32_t total, depth, dropped;
event_get_statistics(&total, &depth, &dropped);

// Module manager statistics
module_mgr_stats_t stats;
module_manager_get_stats(&stats);
```

### Memory Leak Detection

```c
// Check for leaks
uint32_t leaks = sys_mem_check_leaks(SYS_MEM_POOL_GENERAL);
if (leaks > 0) {
    sys_mem_dump_allocations(SYS_MEM_POOL_GENERAL);
}
```

## Unit Tests

Uses ztest under `tests/`, sharing `src/` implementation with main app (does not link `app_main` or example business modules); **enabled** `CONFIG_THREAD_IPC_SERVICE` by default and includes `ipc_service` for smoke testing; heap size increased (see `tests/prj.conf`).

```bash
# native_sim platform
west build -b native_sim tests/ --build-dir build_tests
west build -t run --build-dir build_tests

# native_sim platform
west build -b native_sim tests/ --build-dir build_tests_sim
west build -t run --build-dir build_tests_sim
```

Requires `ZEPHYR_BASE` or root directory `zephyr_config.env`. See tests/README.md for details.

## License

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)** - see the LICENSE file.

### Dual License Model

This project adopts a **dual license** model:

- **GPL v3** (Free): Free for personal learning, research, and open-source projects, but **you must open-source your derivative code**
- **Commercial License** (Paid): Required for enterprise users and closed-source commercial products

For commercial license inquiries, email: [china_qzh@163.com](mailto:china_qzh@163.com)

Detailed terms: See [LICENSE_COMMERCIAL.md](LICENSE_COMMERCIAL.md)

> Using this code in violation of GPL terms carries legal risk. Purchasing a commercial license enables lawful closed-source usage.

## Contributing

### Submitting Code (Pull Request)

1. Fork the repository
2. Create a feature branch (e.g. `feature/my-feature` or `fix/issue-123`)
3. Make changes, ensure build and tests pass
4. **Use conventional commit format** (see below)
5. Submit Pull Request

**Commit Format**: Follows Conventional Commits specification

```
<type>(<scope>): <subject>
```

**Common types**:
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation update
- `style`: Code style (no logic change)
- `refactor`: Refactoring
- `perf`: Performance optimization
- `test`: Testing related
- `build`: Build system
- `ci`: CI/CD
- `chore`: Other chores

**Examples**:
```bash
git commit -m "feat(sys_memory): add heap leak detection"
git commit -m "fix(event): fix event queue statistics error"
git commit -m "docs(ci): update CI board config documentation"
```

**Avoid vague commit messages**:
```bash
git commit -m "modify"          # Too vague
git commit -m "update code"     # No information
git commit -m "fix bug"         # Doesn't specify what was fixed
```

More details in **[docs/en/80-contributing/81-contributing-code-style.md](docs/en/80-contributing/81-contributing-code-style.md)**.

## Support

For questions and feature requests, please submit an Issue in the project repository.

---

**Version**: 1.0.0 (Single source: root `APP_VERSION`; before release run `python scripts/bump_version.py X.Y.Z` to sync Doxygen/README)
**Build Type**: Release/Debug
**Target**: Generic / Zephyr-supported boards

## Documentation Index

**Main Index (recommended reading order, glossary, old filename cross-reference)**: [docs/en/00-getting-started/02-doc-index.md](docs/en/00-getting-started/02-doc-index.md)

| Document | Description |
|----------|-------------|
| [docs/en/00-getting-started/02-doc-index.md](docs/en/00-getting-started/02-doc-index.md) | **Main Entry**: Learning path, full manual list |
| [docs/en/10-environment-build/11-environment-setup.md](docs/en/10-environment-build/11-environment-setup.md) | Toolchain, paths, verification build |
| [docs/en/10-environment-build/12-freestanding-app-build.md](docs/en/10-environment-build/12-freestanding-app-build.md) | Freestanding app, `ZEPHYR_BASE`, overlay |
| [docs/en/00-getting-started/04-developer-guide.md](docs/en/00-getting-started/04-developer-guide.md) | Daily development, testing, debugging |
| [docs/en/40-app-development/41-zephyr-app-development.md](docs/en/40-app-development/41-zephyr-app-development.md) | Zephyr general technology and service development guide |
| [docs/en/40-app-development/44-devicetree-memory-config.md](docs/en/40-app-development/44-devicetree-memory-config.md) | Devicetree, SRAM, `app.overlay` |
| [docs/en/40-app-development/42-config-options.md](docs/en/40-app-development/42-config-options.md) | **Kconfig and application config items** |
| [docs/en/30-core-modules/31-event-system-guide.md](docs/en/30-core-modules/31-event-system-guide.md) | Event API and usage |
| [docs/en/30-core-modules/32-module-system-guide.md](docs/en/30-core-modules/32-module-system-guide.md) | Module lifecycle, runtime dependencies |
| [docs/en/30-core-modules/33-thread-ipc-service-guide.md](docs/en/30-core-modules/33-thread-ipc-service-guide.md) | Thread IPC service |
| [docs/en/30-core-modules/34-thread-ipc-integration-guide.md](docs/en/30-core-modules/34-thread-ipc-integration-guide.md) | Integrating IPC in modules |
| [docs/en/30-core-modules/37-data-bus-guide.md](docs/en/30-core-modules/37-data-bus-guide.md) | Data Bus: named-channel streaming data sharing |
| [docs/en/70-release-productization/74-ota-storage-guide.md](docs/en/70-release-productization/74-ota-storage-guide.md) | OTA, NVS, low power (optional) |
| [docs/en/70-release-productization/71-version-management.md](docs/en/70-release-productization/71-version-management.md) | Version number and build info |
| [docs/en/70-release-productization/72-zephyr-version-ci.md](docs/en/70-release-productization/72-zephyr-version-ci.md) | Aligning with CI image version |
| [docs/en/70-release-productization/73-release-checklist.md](docs/en/70-release-productization/73-release-checklist.md) | Pre-release checklist |
| [docs/en/60-debugging/62-troubleshooting.md](docs/en/60-debugging/62-troubleshooting.md) | Build and environment troubleshooting |
| [docs/en/60-debugging/61-flash-debug-quickstart.md](docs/en/60-debugging/61-flash-debug-quickstart.md) | Flashing, serial, debugging |
| [docs/en/60-debugging/63-scripts-and-tools.md](docs/en/60-debugging/63-scripts-and-tools.md) | `scripts/` tool descriptions |
| [docs/en/50-testing-ci/51-unit-testing-ci.md](docs/en/50-testing-ci/51-unit-testing-ci.md) | ztest and CI overview |
| [docs/en/50-testing-ci/52-ci-platform-setup.md](docs/en/50-testing-ci/52-ci-platform-setup.md) | **GitHub / GitLab** CI enablement and maintenance |
| [docs/en/30-core-modules/36-system-services-guide.md](docs/en/30-core-modules/36-system-services-guide.md) | sys_log / sys_memory / sys_timer / sys_watchdog |
| [docs/en/80-contributing/81-contributing-code-style.md](docs/en/80-contributing/81-contributing-code-style.md) | PR, code style and CI |
| [docs/en/70-release-productization/75-security-key-management.md](docs/en/70-release-productization/75-security-key-management.md) | Keys, secrets, OTA signing notes |
| [tests/README.md](tests/README.md) | Unit tests (detailed) |
| [LICENSE](LICENSE) | Full GPL v3 text |
| [LICENSE_COMMERCIAL.md](LICENSE_COMMERCIAL.md) | Commercial license terms |
