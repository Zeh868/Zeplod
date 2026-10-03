> Language: [中文](../../zh-CN/00-入门/03-术语速查卡片.md) | **English**

# Glossary Quick Reference

**Purpose**: Quick lookup for unfamiliar terms  
**Audience**: Newcomers, cross-domain developers

---

## Build & Tools

| Term | One-Line Explanation | Typical Scenario | More Info |
|------|----------------------|------------------|-----------|
| **West** | Zephyr's "package manager + build tool" | `west build`, `west flash` | [Environment Setup Guide](../10-environment-build/11-environment-setup.md) |
| **ZEPHYR_BASE** | Environment variable for Zephyr source directory | Build fails without it | [Freestanding App Build](../10-environment-build/12-freestanding-app-build.md) |
| **Zephyr SDK** | Zephyr's "compiler toolchain package" | Required for compilation | [Environment Setup Guide](../10-environment-build/11-environment-setup.md) |
| **overlay** | "Patch file" for development board Devicetree | Extend SRAM, add peripherals | [Devicetree & Memory Config](../40-app-development/44-devicetree-memory-config.md) |
| **prj.conf** | Project "config file" (enables features) | Like menuconfig | [Config Options](../40-app-development/42-config-options.md) |
| **Kconfig** | "Menu definition" for config options | Defines what prj.conf can set | [Config Options](../40-app-development/42-config-options.md) |
| **native_sim** | Run Zephyr simulation on Linux/WSL | Test logic without hardware | [Quick Start](01-quick-start.md) |
| **CMake** | Cross-platform build system | Zephyr uses it to generate build files | - |
| **Devicetree** | "Description file" for hardware configuration | Tells system what hardware exists | [Devicetree & Memory Config](../40-app-development/44-devicetree-memory-config.md) |
| **DTS** | Devicetree Source (hardware tree source) | `.dts` or `.overlay` files | [Devicetree & Memory Config](../40-app-development/44-devicetree-memory-config.md) |

---

## Code & API

| Term | One-Line Explanation | Typical Scenario | More Info |
|------|----------------------|------------------|-----------|
| **SYS_INIT** | Zephyr's "auto-initialization" macro | Module registration without manual calls | [Module System Guide](../30-core-modules/32-module-system-guide.md) |
| **POST_KERNEL** | SYS_INIT initialization phase parameter | Executes after kernel init | [Module System Guide](../30-core-modules/32-module-system-guide.md) |
| **event_publish** | API to "send events" | Inter-module communication | [Event System Guide](../30-core-modules/31-event-system-guide.md) |
| **event_subscribe** | API to "subscribe to events" | Receive messages from other modules | [Event System Guide](../30-core-modules/31-event-system-guide.md) |
| **module_manager_register** | Register module with Module Manager | Makes system aware of your module | [Module System Guide](../30-core-modules/32-module-system-guide.md) |
| **LOG_INF** | Info-level logging macro | `LOG_INF("Started successfully")` | [System Services Guide](../30-core-modules/36-system-services-guide.md) |
| **LOG_DBG** | Debug-level logging macro | `LOG_DBG("Debug info: %d", value)` | [System Services Guide](../30-core-modules/36-system-services-guide.md) |
| **k_thread** | Zephyr thread object | Create new threads | [Zephyr App Development Guide](../40-app-development/41-zephyr-app-development.md) |
| **k_mutex** | Zephyr mutex | Protect shared resources | [Zephyr App Development Guide](../40-app-development/41-zephyr-app-development.md) |
| **DEVICE_DT_GET** | Get device handle from Devicetree | Get GPIO, UART and other devices | [Zephyr App Development Guide](../40-app-development/41-zephyr-app-development.md) |

---

## Hardware & Board-Level

| Term | One-Line Explanation | Typical Scenario | More Info |
|------|----------------------|------------------|-----------|
| **Nucleo** | STMicroelectronics development board series | Common test boards (e.g., nucleo_l4r5zi) | [Flash & Debug Guide](../60-debugging/61-flash-debug-quickstart.md) |
| **UART** | Serial communication | Print logs to computer | [Flash & Debug Guide](../60-debugging/61-flash-debug-quickstart.md) |
| **GPIO** | General Purpose Input/Output | Control LEDs, read buttons | [Zephyr Application Development Guide](../40-app-development/41-zephyr-app-development.md) |
| **SRAM** | Static Random Access Memory | Runtime memory for programs | [Devicetree & Memory Config](../40-app-development/44-devicetree-memory-config.md) |
| **Flash** | Flash storage (program code storage) | Firmware storage location | [OTA & Storage Guide](../70-release-productization/74-ota-storage-guide.md) |
| **RAM overflow** | Memory insufficient error | Linker error | [Troubleshooting](../60-debugging/62-troubleshooting.md) |
| **Board** | Development board (Zephyr term) | `west build -b nucleo_l4r5zi` | [Board Porting Guide](../10-environment-build/13-board-porting-guide.md) |
| **Shield** | Arduino-compatible expansion board | Some shields supported by Zephyr | - |

---

## Project Architecture

| Term | One-Line Explanation | Typical Scenario | More Info |
|------|----------------------|------------------|-----------|
| **Freestanding** | App and Zephyr source in different directories | This project's organization | [Freestanding App Build](../10-environment-build/12-freestanding-app-build.md) |
| **Event-Driven** | Inter-module communication via events | Publish-Subscribe pattern | [Event System Guide](../30-core-modules/31-event-system-guide.md) |
| **Module** | Independent business logic unit | e.g., `example_module_gpio` | [Module System Guide](../30-core-modules/32-module-system-guide.md) |
| **Thread IPC** | Multi-thread request/response service within an app | Synchronous inter-module communication | [Thread IPC Service Guide](../30-core-modules/33-thread-ipc-service-guide.md) |
| **Publish-Subscribe** | Event System working pattern | Publisher doesn't care who receives | [Event System Guide](../30-core-modules/31-event-system-guide.md) |
| **SYS_INIT chain** | Zephyr's automatic initialization mechanism | Multiple modules initialize in priority order | [Module System Guide](../30-core-modules/32-module-system-guide.md) |

---

## CI/CD & Versioning

| Term | One-Line Explanation | Typical Scenario | More Info |
|------|----------------------|------------------|-----------|
| **CI** | Continuous Integration | Auto build and test | [CI Platform Setup](../50-testing-ci/52-ci-platform-setup.md) |
| **GitHub Actions** | GitHub's CI service | `.github/workflows/ci.yml` | [CI Platform Setup](../50-testing-ci/52-ci-platform-setup.md) |
| **GitLab CI** | GitLab's CI service | `.gitlab-ci.yml` | [CI Platform Setup](../50-testing-ci/52-ci-platform-setup.md) |
| **PR (Pull Request)** | Code merge request | Submit changes for review | [Contributing & Code Style](../80-contributing/81-contributing-code-style.md) |
| **pre-commit** | Pre-commit auto-check tool | Format and check code | [Contributing & Code Style](../80-contributing/81-contributing-code-style.md) |
| **clang-format** | Code formatting tool | Unified code style | [Contributing & Code Style](../80-contributing/81-contributing-code-style.md) |
| **APP_VERSION** | Application semantic version number | `APP_VERSION` file in root | [Version Management](../70-release-productization/71-version-management.md) |

---

## Config File Quick Reference

| File | Purpose | Git Commit |
|------|---------|------------|
| `prj.conf` | Zephyr config (`CONFIG_*`) | Yes |
| `Kconfig` | Config menu definitions | Yes |
| `app.overlay` | Devicetree overlay (common) | Yes |
| `zephyr_config.env` | Local path config | No (copied from template) |
| `APP_VERSION` | App version number | Yes |
| `CMakeLists.txt` | Build config | Yes |

---

## Common Abbreviations

| Abbreviation | Full Name | Meaning |
|--------------|-----------|---------|
| **RTOS** | Real-Time Operating System | Real-time operating system |
| **MCU** | Microcontroller Unit | Microcontroller |
| **SoC** | System on Chip | System on Chip |
| **GPIO** | General Purpose Input/Output | General Purpose Input/Output |
| **UART** | Universal Asynchronous Receiver/Transmitter | Universal Asynchronous Receiver/Transmitter |
| **SPI** | Serial Peripheral Interface | Serial Peripheral Interface |
| **I2C** | Inter-Integrated Circuit | Inter-Integrated Circuit |
| **RAM** | Random Access Memory | Random Access Memory |
| **ROM** | Read-Only Memory | Read-Only Memory |
| **OTA** | Over-The-Air | Over-The-Air (firmware update) |
| **NVS** | Non-Volatile Storage | Non-Volatile Storage |
| **IPC** | Inter-Process Communication | Inter-Process Communication |
| **ISR** | Interrupt Service Routine | Interrupt Service Routine |
| **DTS** | Devicetree Source | Devicetree Source |
| **DTB** | Devicetree Blob | Compiled Devicetree |
| **YAML** | YAML Ain't Markup Language | Config file format (e.g., `.github/workflows/ci.yml`) |

---

## Quick Links

| I want to learn about... | Read this |
|--------------------------|-----------|
| **Complete Documentation Index** | [docs/en/00-getting-started/02-doc-index.md](02-doc-index.md) |
| **5-Minute Quick Start** | [docs/en/00-getting-started/01-quick-start.md](01-quick-start.md) |
| **Environment Setup** | [docs/en/10-environment-build/11-environment-setup.md](../10-environment-build/11-environment-setup.md) |
| **Getting Started with Development** | [docs/en/00-getting-started/04-developer-guide.md](04-developer-guide.md) |
| **Common Issues** | [docs/en/60-debugging/62-troubleshooting.md](../60-debugging/62-troubleshooting.md) |

---

*Note: This document is a quick reference card. For detailed explanations, refer to the corresponding topic documentation.*
