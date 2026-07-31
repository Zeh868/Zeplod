> Language: [中文](../../zh-CN/00-入门/04-开发者入门指南.md) | **English**

# Developer Getting Started Guide

This guide covers how to develop with **Zeplod**: directory structure, daily development workflow, testing and debugging entry points. **If this is your first time encountering this repo**, please first read **[02-doc-index.md](02-doc-index.md)** in the "Where Should Beginners Start?" section, then proceed in order through environment setup, freestanding app build, and other documentation.

## Table of Contents

0. [Documentation Index (Recommended Reading Order)](#0-documentation-index-recommended-reading-order)
1. [Checklist After Copying from Template](#1-checklist-after-copying-from-template)
2. [Quick Start](#2-quick-start)
3. [Project Structure](#3-project-structure)
4. [Development Workflow](#4-development-workflow)
5. [Code Style](#5-code-style)
6. [Testing](#6-testing)
7. [Debugging](#7-debugging)
8. [Release](#8-release)
9. [Config Options Index](#9-config-options-index)
10. [Zephyr Devicetree & Memory](#10-zephyr-devicetree--memory)

---

## 0. Documentation Index (Recommended Reading Order)

For the complete manual list, three learning paths (build / business / release), terminology table, and **old filename reference**, see **[02-doc-index.md](02-doc-index.md)**. The following chapters assume you can already successfully execute **`west build`**; if not, first complete **[11-environment-setup.md](../10-environment-build/11-environment-setup.md)** and **[12-freestanding-app-build.md](../10-environment-build/12-freestanding-app-build.md)**.

---

## 1. Checklist After Copying from Template

When using this repo as a **new product engineering template**, it is recommended to check the following items to avoid path, version, and CI drift over time.

1. **west.yml**: `revision` matches the Zephyr version used by your team (default **v3.6.0**, aligned with **[72-zephyr-version-ci.md](../70-release-productization/72-zephyr-version-ci.md)** and CI mirrors); if using a company internal mirror, modify the `url`.
2. **zephyr_config.env**: Copy from **`zephyr_config.env.template`**, fill in **`ZEPHYR_BASE`**, **`ZEPHYR_SDK_INSTALL_DIR`**; **do not commit** to Git (repo **`.gitignore`** already includes this file). If it was accidentally committed historically, remove from index: `git rm --cached zephyr_config.env` (then commit again).
3. **CMake project name**: In root **`CMakeLists.txt`**, change **`project(...)`** to your product name.
4. **Version and documentation**: Update root **`APP_VERSION`**, **`README.md`** title and product description as needed.
5. **Board and CI**: **`prj.conf`**, **`.github/workflows/ci.yml`** (and if using GitLab: **`.gitlab-ci.yml`**) have ARM build matrix **`board`** aligned with target hardware or CI strategy; example board names in docs (like `nucleo_l4r5zi`) are for illustration only, **use your actual `BOARD` and CI as the standard**. For step-by-step CI platform enablement see **[52-ci-platform-setup.md](../50-testing-ci/52-ci-platform-setup.md)**.
6. **Example modules**: **`src/modules_examples/example_*`** can be deleted or converted to business modules (public headers under **`include/zeplod/`**); synchronize **`CMakeLists.txt`**, Kconfig, each module **`.c`** **`SYS_INIT`** registration, and **`APP_INIT_PRIO_*`** in **`include/zeplod/app_config.h`** (no longer need to modify **`app_main.c`** registration list centrally).
7. **Optional hooks**: Installing **`pre-commit`** locally (see **[81-contributing-code-style.md](../80-contributing/81-contributing-code-style.md)**) can align with CI **`pre-commit run --all-files`** behavior.

---

## 2. Quick Start

### 1. Environment Setup

```bash
# Windows PowerShell
.\scripts\setup_env.ps1

# Linux/macOS
source scripts/setup_env.sh
```

### 2. Build Project

```bash
# Build native_posix (for testing)
west build -b native_posix .

# Build for target development board (example: replace with your board name)
west build -b nucleo_l4r5zi .
```

### 3. Flash and Monitor

```bash
# Flash
west flash

# Serial (example: pip install pyserial required; COM port and baud rate per your setup)
python -m serial.tools.miniterm COM3 115200
```

For complete steps and Windows/Linux differences see **[61-flash-debug-quickstart.md](../60-debugging/61-flash-debug-quickstart.md)**; for troubleshooting see **[62-troubleshooting.md](../60-debugging/62-troubleshooting.md)**.

---

## 3. Project Structure

```
zeplod/
├── src/
│   ├── app/            # Application
│   ├── core/           # Core Event System
│   ├── modules/        # Business modules
│   ├── modules_examples/ # Example modules
│   └── services/       # System services
├── tests/              # Unit tests
├── docs/               # Documentation
├── scripts/            # Utility scripts
├── boards/             # Board-level support
├── .github/workflows/  # CI/CD config
└── .vscode/           # VSCode config
```

---

## 4. Development Workflow

### Adding a New Module

1. Create public header `include/zeplod/my_module.h` and implementation `src/modules/my_module.c`
2. Implement module interface (`#include <zeplod/module_base.h>`)
3. Add `.c` source files in **`CMakeLists.txt`**
4. Add **`APP_INIT_PRIO_MODULE_*`** in **`include/zeplod/app_config.h`** (between **`APP_INIT_PRIO_MODULE_MGR`** and **`APP_INIT_PRIO_APP_FINAL`**)
5. Use **`SYS_INIT(..., POST_KERNEL, APP_INIT_PRIO_MODULE_*)`** to call **`module_manager_register()`** at the end of **`my_module.c`** (reference **`example_module_a.c`**)

Optional: If module startup order needs dependency arrangement, see "Application Startup and Initialization Order (Zephyr SYS_INIT)" and "Runtime Dependencies" in [32-module-system-guide.md](../30-core-modules/32-module-system-guide.md); for multi-dependency examples see `src/modules_examples/example_module_multi_dep.c`.

### Adding an Event Type

```c
// Define in module header file
#define EVENT_TYPE_MY_EVENT  100

// Register event type
event_register_type(EVENT_TYPE_MY_EVENT, "my_event");

// Subscribe to event
event_subscribe(EVENT_TYPE_MY_EVENT, my_callback, user_data, &id);

// Publish event
event_publish_copy(EVENT_TYPE_MY_EVENT, EVENT_PRIORITY_NORMAL, &data, sizeof(data));
```

**Event system usage contract (required reading)**

| Scenario | Practice |
|----------|----------|
| Publish | Register the type first; prefer `event_publish_copy` / `event_create_with_data`. Do not hand-build inconsistent `flags` and `data_len` (`event_publish` validates and returns `EVENT_ERR_INVALID_ARG`) |
| Module teardown | `event_system_stop()` → `event_unsubscribe_all(subscriber_id)` → then free `user_data` (callbacks may still run briefly after unsubscribe due to dispatcher snapshots) |
| Manual queue drain | Use `event_dispatcher_process_one` / `process_all` only after **init** and **before the first** `event_dispatcher_start()`; after start→stop, or in parallel with a running dispatcher thread, calls are rejected |
| Queue full `DROP_LOWEST` | Priority eviction applies only to thread-side `event_publish`; ISR still uses drop-newest when the queue is full |

For the full API reference, see [31-event-system-guide.md](../30-core-modules/31-event-system-guide.md).

### State machine and lock ordering (core modules)

The framework provides **`state_machine`** (lifecycle) and **`lock_order`** (multi-lock ordering) under `src/core/`. They are integrated in **`event_system`**, **`module_manager`**, and **`ipc_service`**.

| Topic | Notes |
|-------|--------|
| State machine API | `zepl_state_machine_init`, `zepl_state_machine_try_transition`, `zepl_state_machine_get` (see `include/zeplod/state_machine.h`) |
| Lock order API | Pair `zepl_lock_enter` / `zepl_lock_exit` with `k_mutex`; levels GLOBAL → STATE → TABLE → ENTRY → RESOURCE |
| New modules | Use `zepl_state_machine_t` for init/start/stop; register lock order on every mutex; no external callbacks while holding locks |
| Caveats | `event_system` does **not** use `ZEP_STATE_ERROR`; `module_manager` has a single global lock; IPC pending + shm release nests TABLE → ENTRY |

Implementation plan and intentional design choices: **[2026-05-23-state-machine-lock-ordering-implementation-plan.md](../../superpowers/plan/2026-05-23-state-machine-lock-ordering-implementation-plan.md)** (under `docs/superpowers/plan/`).

### Modifying Configuration

- `prj.conf` - Zephyr kernel and app merged config (`CONFIG_*`)
- `Kconfig` - App extension menu item definitions (root and `src/modules/ipc_service/Kconfig`)
- `zephyr_config.env` - Local path config
- `include/zeplod/app_config.h` - App feature toggles (`APP_CONFIG_*` and other macros, not menuconfig)

**For config option meanings**: See **[42-config-options.md](../40-app-development/42-config-options.md)** (Event System, Module Manager, System Services, Thread IPC, `app_config` supplementary notes).

---

## 5. Code Style

### Formatting Code

```bash
# Use clang-format (root directory .clang-format)
clang-format -i src/**/*.c src/**/*.h

For example:
    clang-format -i src\app\*.c src\app\*.h
    clang-format -i src\core\*.c src\core\*.h
    clang-format -i src\modules\ipc_service\*.c src\modules\ipc_service\*.h
    clang-format -i src\modules\*.c src\modules\*.h
    clang-format -i src\services\*.c src\services\*.h
```

### pre-commit (Optional)

```bash
pip install pre-commit
pre-commit install
pre-commit run --all-files
```

Configuration is in root directory `.pre-commit-config.yaml` (includes trailing whitespace, YAML check, clang-format).

### Static Analysis (clang-tidy)

The repo provides `.clang-tidy` (can be tightened or loosened per project). First configure Zephyr build to generate `compile_commands.json`, for example:

```bash
west build -b native_posix . -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
clang-tidy -p build src/core/event_system.c
```

Without `compile_commands.json`, you can only use `clang-tidy ... -- -I...` to manually pass headers, which easily becomes inconsistent with Zephyr; using `-p build` is recommended.

### Naming Conventions

| Type | Style | Example |
|------|-------|---------|
| File | snake_case | `event_system.c` |
| Function | snake_case | `event_publish()` |
| Type | snake_case_t | `event_t`, `event_status_t` |
| Enum value | UPPER_CASE | `EVENT_OK`, `EVENT_ERR` |
| Macro | UPPER_CASE | `EVENT_MAX_SUBSCRIBERS` |
| Variable | lower_case | `subscriber_id` |
| Module name | snake_case | `module_manager` |

---

## 6. Testing

### Running Unit Tests

```bash
# Build (native_posix)
west build -b native_posix tests/ --build-dir build_tests

# Run ztest executable (Linux/macOS)
./build_tests/zephyr/zephyr

# Or one-step
west build -t run --build-dir build_tests
```

Requires `ZEPHYR_BASE` set or `zephyr_config.env` in repo root.

### Adding New Tests

Create test file in `tests/` directory:

```c
#include <zephyr/ztest.h>
#include <module_under_test.h>

ZTEST(test_module, test_feature)
{
    // Test code
    zassert_equal(expected, actual, "Error message");
}

ZTEST_SUITE(test_module, NULL, NULL, NULL, NULL, NULL);
```

---

## 7. Debugging

### VSCode Debugging

1. Press `F5` to start debugging
2. Select debug configuration (QEMU/OpenOCD/J-Link)
3. Use breakpoints and variable windows

### Log Debugging

```c
// Add logs in code
LOG_INF("Info log: %d", value);
LOG_DBG("Debug log: %s", string);
LOG_WRN("Warning log");
LOG_ERR("Error log: %d", error_code);

// Change log level (prj.conf)
CONFIG_LOG_DEFAULT_LEVEL=4
```

### Shell Commands

```bash
# Application status
app status

# Module information
app modules

# Event statistics
app events

# Memory statistics
app memory

# Log dump
app log 3
```

---

## 8. Release

### Version Update

1. Update version:
   - `CMakeLists.txt`
   - `README.md`
   - `Doxyfile`
   - `CHANGELOG.md`

2. Commit changes:
```bash
git add .
git commit -m "chore: prepare release v1.0.0"
```

3. Create tag:
```bash
git tag -a v1.0.0 -m "Release v1.0.0"
git push origin v1.0.0
```

### Packaging Firmware

```bash
# Package release
scripts/package_release.sh -v 1.0.0

# Batch build
scripts/build_all.sh -c
```

**Windows note**: The above **`*.sh`** are **Shell** scripts. In **PowerShell / CMD**, they typically need to be executed via **Git Bash**, **MSYS2**, or **WSL**; if the repo **`scripts/`** provides same-named **`.ps1` / `.bat`** (like **`build_all.bat`**), those can be used preferentially. See **[63-scripts-and-tools.md](../60-debugging/63-scripts-and-tools.md)** for details.

---

## 9. Utility Scripts

For script purposes, paths, and precautions see **[63-scripts-and-tools.md](../60-debugging/63-scripts-and-tools.md)**. Summary table:

| Script | Description |
|--------|-------------|
| `setup_env.ps1/sh` | Set up environment |
| `build_all.sh/bat` | Batch build |
| `package_release.sh` | Package release |
| `generate_docs.sh` | Generate API documentation (Doxygen) |

---

## 10. Common Issues

### Q: How to change target development board?

A: Modify the `-b` parameter in the build command:
```bash
west build -b nucleo_l4r5zi . -p always
```

### Q: How to add a new Kconfig option?

A: Add in `Kconfig` file:
```kconfig
config MY_FEATURE
    bool "Enable my feature"
    default y
    help
      Enable this feature to...
```

Then enable in `prj.conf`:
```
CONFIG_MY_FEATURE=y
```

### Q: How to debug memory issues?

A: Use memory statistics and leak detection:
```c
sys_mem_dump_allocations(SYS_MEM_POOL_GENERAL);
uint32_t leaks = sys_mem_check_leaks(SYS_MEM_POOL_GENERAL);
```

---

## 11. Config Options Index

For centralized explanation of Kconfig and `app_config.h` macros extended by this repo see **[42-config-options.md](../40-app-development/42-config-options.md)**.

---

## 12. Zephyr Devicetree & Memory

For Devicetree Overlay auto-discovery order, difference between `app.overlay` and `boards/*.overlay`, main RAM (`zephyr,sram`) and linker scripts, and typical approaches for multiple discontinuous memory blocks, see **[44-devicetree-memory-config.md](../40-app-development/44-devicetree-memory-config.md)**.

---

## Reference Resources

- [Zephyr Official Documentation](https://docs.zephyrproject.org/)
- [Zephyr App Development & Services Guide](../40-app-development/41-zephyr-app-development.md) (threads, synchronization, device model, service writing patterns)
- [Devicetree & Memory Configuration Manual](../40-app-development/44-devicetree-memory-config.md)
- [Documentation Index](02-doc-index.md)
- [Project README](../../../README.md)
- [Environment Setup & Configuration Guide](../10-environment-build/11-environment-setup.md)
- [API Documentation](see scripts/ tools documentation for generation)
