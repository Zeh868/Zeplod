> Language: [中文](../../zh-CN/00-入门/02-文档索引.md) | **English**

# Documentation Index and Reading Guide

This page is the **master index** of all documentation under `docs/`. For **task-based lookup**, see **[05-task-navigation.md](05-task-navigation.md)**. Code mapping: **[06-code-doc-map.md](06-code-doc-map.md)**.

Documents are organized by topic, with prefix numbers indicating recommended learning order:

```
docs/
├── zh-CN/
│   ├── 00-入门/
│   │   ├── 01-5分钟快速体验.md
│   │   ├── 02-文档索引.md
│   │   ├── 03-术语速查卡片.md
│   │   ├── 04-开发者入门指南.md
│   │   ├── 05-任务导航.md
│   │   └── 06-代码与文档映射.md
│   ├── 10-环境与构建/
│   │   ├── 11-环境搭建与配置指南.md
│   │   ├── 12-Freestanding应用与构建基础.md
│   │   ├── 13-板型迁移指南.md
│   │   ├── 14-QEMU仿真运行指南.md
│   │   └── 15-新建APP开发指南.md
│   ├── 20-架构设计/
│   │   ├── 21-模块化软件设计方法论.md
│   │   ├── 22-模块化软件设计的详细方法.md
│   │   ├── 23-框架核心技术实现细节.md
│   │   ├── 26-稳定性与可维护性优化建议.md
│   │   └── 27-核心架构重构路线图.md
│   ├── 30-核心模块/
│   │   ├── 31-事件系统详细使用说明.md
│   │   ├── 32-模块系统详细使用说明.md
│   │   ├── 33-线程IPC服务使用说明.md
│   │   ├── 34-Thread_IPC模块集成指南.md
│   │   ├── 35-IPC服务扩展特性规划.md
│   │   ├── 36-系统服务使用说明.md
│   │   ├── 37-数据总线使用说明.md
│   │   └── 38-线程服务生命周期约定.md
│   ├── 40-应用开发/
│   │   ├── 41-Zephyr应用开发与服务指南.md
│   │   ├── 42-项目配置项说明.md
│   │   ├── 43-配置方案对比指南.md
│   │   └── 44-设备树与内存配置手册.md
│   ├── 50-测试与CI/
│   │   ├── 51-单元测试与持续集成说明.md
│   │   ├── 52-CI平台配置保姆级手册.md
│   │   ├── 53-硬件测试运行指南.md
│   │   └── 54-看门狗测试指南.md
│   ├── 60-调试与排错/
│   │   ├── 61-烧录与调试快速指南.md
│   │   ├── 62-常见问题与故障排除.md
│   │   └── 63-脚本与工具说明.md
│   ├── 70-发布与产品化/
│   │   ├── 71-版本管理.md
│   │   ├── 72-Zephyr版本与CI说明.md
│   │   ├── 73-发布检查清单.md
│   │   ├── 74-OTA与存储扩展指南.md
│   │   └── 75-安全与密钥管理说明.md
│   └── 80-贡献与维护/
│       ├── 81-参与贡献与代码规范.md
│       └── 82-文档改进建议.md
└── en/                              ← English canonical subset (zh authoritative)
    ├── 00-getting-started/
    ├── 10-environment-build/
    ├── 20-architecture/
    ├── 30-core-modules/
    │   └── 37-data-bus-guide.md
    ├── 40-app-development/
    ├── 50-testing-ci/
    ├── 60-debugging/
    ├── 70-release-productization/
    └── 80-contributing/
```

---

## Language Switch

This is the **English** documentation tree. For **Chinese** documentation, switch to:
- [docs/zh-CN/00-入门/02-文档索引.md](../../zh-CN/00-入门/02-文档索引.md)

---

## Where Should Beginners Start?

The five paths below are organized by common use cases. **You don't need to read every document** — skip anything irrelevant to your current task.

### Path A: First Compile of This Project (Zero Basics)

0. (If copying this repo **as a new project**) First read **[README.md](../../../README.md)** for "Initialize New Project from Template (Checklist)" and **[04-developer-guide.md](04-developer-guide.md#checklist-after-copying-from-template)**, complete west, CMake, CI board alignment.
1. **[01-quick-start.md](01-quick-start.md)** — Run on PC in 5 minutes (`native_posix`), no development board needed.
2. **[11-environment-setup.md](../10-environment-build/11-environment-setup.md)** — Install Zephyr SDK, West, CMake, Python; configure **`zephyr_config.env`**; verify **`west build`** passes.
3. **[12-freestanding-app-build.md](../10-environment-build/12-freestanding-app-build.md)** — Understand "Freestanding App" directory structure, **`ZEPHYR_BASE`**, **`west build`** common parameters; custom board and overlay conventions.
4. If **changing development boards**: **[13-board-porting-guide.md](../10-environment-build/13-board-porting-guide.md)** — Complete board porting flow, Devicetree adjustment, memory config, CI updates.
5. **[04-developer-guide.md](04-developer-guide.md)** — Project directory, `prj.conf` / `Kconfig` responsibilities, shortest workflow for adding modules and events.
6. If using **Nucleo L4R5** and encountering **RAM insufficient / linker overflow**, also read **[44-devicetree-memory-config.md](../40-app-development/44-devicetree-memory-config.md)** (includes **`app.overlay`** and multi-segment memory explanation). For **32KB SRAM extreme scenarios**, refer to the "Extreme" config in **[43-config-comparison-guide.md](../40-app-development/43-config-comparison-guide.md)**.
7. After successful build, to **flash and view serial**: **[61-flash-debug-quickstart.md](../60-debugging/61-flash-debug-quickstart.md)**.
8. When stuck, first check **[62-troubleshooting.md](../60-debugging/62-troubleshooting.md)**; for script purposes see **[63-scripts-and-tools.md](../60-debugging/63-scripts-and-tools.md)**.

### Path A2: Create a New APP Repo (framework Submodule)

For a **separate Git repo** with zeplod under `framework/` (e.g. zephyr_gateway):

1. **[15-creating-new-app-guide.md](../10-environment-build/15-creating-new-app-guide.md)** — Layout, CMakeLists template, Kconfig/QEMU/scripts, checklist.
2. **[11-environment-setup.md](../10-environment-build/11-environment-setup.md)** — `framework/zephyr_config.env`.
3. **[14-qemu-simulation-guide.md](../10-environment-build/14-qemu-simulation-guide.md)** — Windows smoke tests without hardware.
4. **[63-scripts-and-tools.md](../60-debugging/63-scripts-and-tools.md)** — `framework/scripts/`.
5. Business development: **[04-developer-guide.md](04-developer-guide.md)** and **30-core-modules** docs.

### Path B: Modify Business Features (Events, Modules, IPC)

0. (Optional but recommended) **[41-zephyr-app-development.md](../40-app-development/41-zephyr-app-development.md)** — Zephyr general techniques: threads, synchronization, timers, memory, `LOG`, device model, ISR notes, service writing patterns.
1. **[42-config-options.md](../40-app-development/42-config-options.md)** — Complete reference for all **`CONFIG_*`** and **`app_config.h`** macros (recommended bookmark).
2. **[31-event-system-guide.md](../30-core-modules/31-event-system-guide.md)** — Publish/subscribe, queues, priority, thread model.
3. **[32-module-system-guide.md](../30-core-modules/32-module-system-guide.md)** — Module registration, lifecycle, **`depends_on`** runtime dependencies.
4. If **Thread IPC** is enabled: **[33-thread-ipc-service-guide.md](../30-core-modules/33-thread-ipc-service-guide.md)** → **[34-thread-ipc-integration-guide.md](../30-core-modules/34-thread-ipc-integration-guide.md)**; for expansion plans see **[35-ipc-service-roadmap.md](../30-core-modules/35-ipc-service-roadmap.md)**.
5. For using or trimming **logging / memory pool / timer / watchdog**: **[36-system-services-guide.md](../30-core-modules/36-system-services-guide.md)** (corresponds to **`CONFIG_SYS_*`**).
6. For **sensor streams, log streams, large payload sharing**: **[37-data-bus-guide.md](../30-core-modules/37-data-bus-guide.md)** — Named channels, zero-copy, reference counting, auto-release and Retain.

### Path C: Release Versions, Align CI, Optional OTA

1. **[71-version-management.md](../70-release-productization/71-version-management.md)** — How `APP_VERSION`, Git info, and build time enter firmware.
2. **[72-zephyr-version-ci.md](../70-release-productization/72-zephyr-version-ci.md)** — How local Zephyr aligns with versions in **`.github/workflows`** CI mirrors.
3. **[73-release-checklist.md](../70-release-productization/73-release-checklist.md)** — Pre-release checklist.
4. If **OTA / persistent storage / low power** is needed: **[74-ota-storage-guide.md](../70-release-productization/74-ota-storage-guide.md)** (strongly SoC-specific, cross-reference with Zephyr official docs).
5. For **signing keys, CI secrets, repo-uncommitted content**: **[75-security-key-management.md](../70-release-productization/75-security-key-management.md)**.

### Path D: Run Unit Tests, Understand CI

1. **`tests/README.md`** — ztest directory, **`native_posix`** run commands, test case templates.
2. **[51-unit-testing-ci.md](../50-testing-ci/51-unit-testing-ci.md)** — Relationship with main app, GitHub Actions responsibilities, version alignment tips.
3. **[52-ci-platform-setup.md](../50-testing-ci/52-ci-platform-setup.md)** — Step-by-step instructions for enabling CI on **GitHub / GitLab**, viewing pipelines, changing versions and boards, troubleshooting failures.
4. Run tests on hardware: **[53-hardware-testing.md](../50-testing-ci/53-hardware-testing.md)**; watchdog special: **[54-watchdog-test-guide.md](../50-testing-ci/54-watchdog-test-guide.md)**.

### Path E: Submit Code and Reviews

1. **[81-contributing-code-style.md](../80-contributing/81-contributing-code-style.md)** — PR workflow, clang-format, pre-commit, CI expectations, **Commit format specification**.
2. When security-related, pair with **[75-security-key-management.md](../70-release-productization/75-security-key-management.md)**.
3. For improving documentation, refer to **[82-doc-improvements.md](../80-contributing/82-doc-improvements.md)**.

### Path F: Architecture Learning, Internal Evolution

1. **[21-modular-design-methodology.md](../20-architecture/21-modular-design-methodology.md)** — Overall methodology of modular design.
2. **[22-modular-design-detailed.md](../20-architecture/22-modular-design-detailed.md)** — Interface design, dependency management, lifecycle, communication mechanisms, layered architecture.
3. **[23-framework-internals.md](../20-architecture/23-framework-internals.md)** — Event System, Module Manager, memory management implementation details.
4. **[26-stability-maintainability.md](../20-architecture/26-stability-maintainability.md)** — Stability and maintainability recommendations (internal).
5. **[27-architecture-roadmap.md](../20-architecture/27-architecture-roadmap.md)** — Core architecture refactoring roadmap (internal).
6. **[38-thread-service-lifecycle.md](../30-core-modules/38-thread-service-lifecycle.md)** — Thread service lifecycle conventions.

---

## Complete Manual Overview (By Directory)

### 00-Getting Started

| Document | Main Content | Typical Reader |
|----------|--------------|----------------|
| **[01-quick-start.md](01-quick-start.md)** | **Ultra-quick start**: Run project on PC in 5 minutes | **Everyone (first stop)** |
| **[02-doc-index.md](02-doc-index.md)** (this page) | Master index, reading paths, terminology | Everyone |
| **[03-glossary.md](03-glossary.md)** | Term, abbreviation, config file quick reference | Everyone (browse anytime) |
| **[04-developer-guide.md](04-developer-guide.md)** | Structure, workflow, testing, debugging, release overview | Daily developers |

### 10-Environment & Build

| Document | Main Content | Typical Reader |
|----------|--------------|----------------|
| **[11-environment-setup.md](../10-environment-build/11-environment-setup.md)** | SDK, West, path config, verify build | New members, new machines |
| **[12-freestanding-app-build.md](../10-environment-build/12-freestanding-app-build.md)** | Freestanding app, `ZEPHYR_BASE`, BOARD_ROOT, overlay rules | Build and integration leads |
| **[13-board-porting-guide.md](../10-environment-build/13-board-porting-guide.md)** | Complete flow for switching boards, Devicetree, memory, CI config | Board-level porting, migration engineers |
| **[14-qemu-simulation-guide.md](../10-environment-build/14-qemu-simulation-guide.md)** | QEMU build/run, `-Board`, **SMP multi-core**, **ztest on QEMU**, `conf/targets/qemu.conf` | Simulation without hardware |
| **[15-creating-new-app-guide.md](../10-environment-build/15-creating-new-app-guide.md)** | Bootstrap APP repo with framework submodule, CMake/Kconfig/overlay/scripts, checklist | New products, standalone business repos |

### 20-Architecture

| Document | Main Content | Typical Reader |
|----------|--------------|----------------|
| **[21-modular-design-methodology.md](../20-architecture/21-modular-design-methodology.md)** | Overall methodology and principles of modular design | Architects, tech leads |
| **[22-modular-design-detailed.md](../20-architecture/22-modular-design-detailed.md)** | Interface design, dependency management, lifecycle, communication, layered architecture | **Architecture design, module developers** |
| **[23-framework-internals.md](../20-architecture/23-framework-internals.md)** | Event System, Module Manager, memory management internals, thread safety, performance tradeoffs, interview Q&A | **Architecture learning, interview prep** |

### 30-Core Modules

| Document | Main Content | Typical Reader |
|----------|--------------|----------------|
| **[31-event-system-guide.md](../30-core-modules/31-event-system-guide.md)** | Event API, design considerations | Business and middleware writers |
| **[32-module-system-guide.md](../30-core-modules/32-module-system-guide.md)** | Module Manager, **`SYS_INIT`** boot order, `APP_INIT_PRIO_*`, dependencies, registration macros | Module and architecture writers |
| **[33-thread-ipc-service-guide.md](../30-core-modules/33-thread-ipc-service-guide.md)** | In-app Thread IPC architecture and API | When IPC is enabled |
| **[34-thread-ipc-integration-guide.md](../30-core-modules/34-thread-ipc-integration-guide.md)** | Register IPC handling logic in modules | Module authors |
| **[35-ipc-service-roadmap.md](../30-core-modules/35-ipc-service-roadmap.md)** | Roadmap for future IPC extensibility | IPC maintainers |
| **[36-system-services-guide.md](../30-core-modules/36-system-services-guide.md)** | `sys_log` / `sys_memory` / `sys_timer` / `sys_watchdog` overview | Shared capabilities between business and drivers |
| **[37-data-bus-guide.md](../30-core-modules/37-data-bus-guide.md)** | Named channels, zero-copy, reference counting, auto-release and Retain, event bridge | Sensor streams, log streams, large payload sharing |

### 40-App Development

| Document | Main Content | Typical Reader |
|----------|--------------|----------------|
| **[41-zephyr-app-development.md](../40-app-development/41-zephyr-app-development.md)** | Zephyr general tech stack and service development patterns (kernel API outline) | App and middleware developers |
| **[42-config-options.md](../40-app-development/42-config-options.md)** | Item-by-item explanation of app-side Kconfig and `app_config` | Must-check when modifying features |
| **[43-config-comparison-guide.md](../40-app-development/43-config-comparison-guide.md)** | Four config schemes (Standard/Balanced/Minimal/Extreme) memory comparison and selection guide | **Must-read when SRAM is constrained** |
| **[44-devicetree-memory-config.md](../40-app-development/44-devicetree-memory-config.md)** | Devicetree overlay, `zephyr,sram`, scattered memory, **32KB SRAM extreme optimization**, config and code examples | Board-level adaptation, memory savings |

### 50-Testing & CI

| Document | Main Content | Typical Reader |
|----------|--------------|----------------|
| **[51-unit-testing-ci.md](../50-testing-ci/51-unit-testing-ci.md)** | ztest and CI overview (details in `tests/README`) | Test and CI maintainers |
| **[52-ci-platform-setup.md](../50-testing-ci/52-ci-platform-setup.md)** | GitHub Actions / GitLab CI enable steps, dual platform comparison, Runner and mirror | First-time CI, platform migration |
| **[53-hardware-testing.md](../50-testing-ci/53-hardware-testing.md)** | Regression test operations on real hardware | On-board integration and regression |
| **[54-watchdog-test-guide.md](../50-testing-ci/54-watchdog-test-guide.md)** | Watchdog special test guide | Watchdog-related verification |

### 60-Debugging

| Document | Main Content | Typical Reader |
|----------|--------------|----------------|
| **[61-flash-debug-quickstart.md](../60-debugging/61-flash-debug-quickstart.md)** | `west flash`, serial, debug entry | Hardware integration |
| **[62-troubleshooting.md](../60-debugging/62-troubleshooting.md)** | High-frequency issues with build/environment/linker/Devicetree | Everyone (self-service) |
| **[63-scripts-and-tools.md](../60-debugging/63-scripts-and-tools.md)** | Scripts under `scripts/` for setup, docs, versioning | New members |

### 70-Release & Productization

| Document | Main Content | Typical Reader |
|----------|--------------|----------------|
| **[71-version-management.md](../70-release-productization/71-version-management.md)** | Version file format and display | Release, version display |
| **[72-zephyr-version-ci.md](../70-release-productization/72-zephyr-version-ci.md)** | Aligning with CI container Zephyr versions | CI maintainers |
| **[73-release-checklist.md](../70-release-productization/73-release-checklist.md)** | Pre-release checklist | Release leads |
| **[74-ota-storage-guide.md](../70-release-productization/74-ota-storage-guide.md)** | MCUboot, NVS/Settings, PM overview | Productization and advanced topics |
| **[75-security-key-management.md](../70-release-productization/75-security-key-management.md)** | Keys not in repo, OTA signing and secrets, breach handling | OTA/CI integration |

### 80-Contributing

| Document | Main Content | Typical Reader |
|----------|--------------|----------------|
| **[81-contributing-code-style.md](../80-contributing/81-contributing-code-style.md)** | Issue/PR, style, CI self-check | Contributors, reviewers |
| **[82-doc-improvements.md](../80-contributing/82-doc-improvements.md)** | Documentation gap analysis and improvement suggestions | Documentation maintainers |

### Other

- **`docs/api/html/index.html`** — Doxygen-generated **C API** documentation (run `scripts/generate_docs.ps1` or `generate_docs.sh` to generate).
- **`tests/README.md`** — **Detailed** explanation of unit tests (`native_posix`) (complements `51-unit-testing-ci.md`).
- Root directory **`README.md`** — Project overview and features.

---

## Terms & Abbreviations (For Beginners)

| Term | Meaning |
|------|---------|
| **Freestanding** | App source and Zephyr source in different directories; build uses **`ZEPHYR_BASE`** to find kernel. |
| **`west`** | Zephyr's meta-tool: clone, build, flash, etc. |
| **`prj.conf`** | Kconfig fragment writing **`CONFIG_*`**, combined with **`Kconfig`** menu to determine final **`autoconf.h`**. |
| **`app.overlay`** | Devicetree overlay file, merged into board-level DTS; this repo uses it to expand SRAM, etc. |
| **Event** | Inter-module asynchronous communication mechanism (publish-subscribe). See Event System docs. |
| **Thread IPC (this repo)** | Multi-thread request/response service within an app, **not** the full capabilities of Zephyr's `IPC_SERVICE` subsystem. |
| **`k_thread` / `k_mutex`** | Zephyr kernel objects; see **41-zephyr-app-development.md** for details. |
| **`DEVICE_DT_GET`** | Device handle macro from Devicetree parsing; used with **`device_is_ready()`**. |

---

## Old Filename Reference (For Bookmarks Pointing to Old Names)

> **2026-05 Bilingual Revamp:** This release moved `docs/<NN-category>/...` to `docs/zh-CN/<NN-category>/...`, with English versions at `docs/en/<NN-English-category>/...` (filled in during P3). If your local bookmarks contain links like `docs/00-入门/01-5分钟快速体验.md`, they are now broken; please prepend `zh-CN/` to the path.

Documents were reorganized by topic in May 2026 into `docs/zh-CN/<NN-category>/NN-filename.md` format. Below is a commonly-used old-to-new path reference:

| Old Path (`docs/`) | New Path |
|--------------------|----------|
| `5-Minute Quick Start.md` | [00-Getting Started/01-quick-start.md](01-quick-start.md) |
| `Documentation Index.md` | [00-Getting Started/02-doc-index.md](02-doc-index.md) |
| `Glossary Quick Reference.md` | [00-Getting Started/03-glossary.md](03-glossary.md) |
| `Developer Guide.md` | [00-Getting Started/04-developer-guide.md](04-developer-guide.md) |
| `Environment Setup Guide.md` | [10-Environment & Build/11-environment-setup.md](../10-environment-build/11-environment-setup.md) |
| `Freestanding App Build.md` | [10-Environment & Build/12-freestanding-app-build.md](../10-environment-build/12-freestanding-app-build.md) |
| `Board Porting Guide.md` | [10-Environment & Build/13-board-porting-guide.md](../10-environment-build/13-board-porting-guide.md) |
| `Modular Design Methodology.md` | [20-Architecture/21-modular-design-methodology.md](../20-architecture/21-modular-design-methodology.md) |
| `Modular Design Detailed.md` | [20-Architecture/22-modular-design-detailed.md](../20-architecture/22-modular-design-detailed.md) |
| `Framework Internals.md` | [20-Architecture/23-framework-internals.md](../20-architecture/23-framework-internals.md) |
| `Event System Guide.md` | [30-Core Modules/31-event-system-guide.md](../30-core-modules/31-event-system-guide.md) |
| `Module System Guide.md` | [30-Core Modules/32-module-system-guide.md](../30-core-modules/32-module-system-guide.md) |
| `Thread IPC Service Guide.md` | [30-Core Modules/33-thread-ipc-service-guide.md](../30-core-modules/33-thread-ipc-service-guide.md) |
| `Thread IPC Integration Guide.md` | [30-Core Modules/34-thread-ipc-integration-guide.md](../30-core-modules/34-thread-ipc-integration-guide.md) |
| `IPC Service Roadmap.md` | [30-Core Modules/35-ipc-service-roadmap.md](../30-core-modules/35-ipc-service-roadmap.md) |
| `System Services Guide.md` | [30-Core Modules/36-system-services-guide.md](../30-core-modules/36-system-services-guide.md) |
| `Zephyr App Development.md` | [40-App Development/41-zephyr-app-development.md](../40-app-development/41-zephyr-app-development.md) |
| `Config Options.md` | [40-App Development/42-config-options.md](../40-app-development/42-config-options.md) |
| `Config Comparison Guide.md` | [40-App Development/43-config-comparison-guide.md](../40-app-development/43-config-comparison-guide.md) |
| `Devicetree & Memory Config.md` | [40-App Development/44-devicetree-memory-config.md](../40-app-development/44-devicetree-memory-config.md) |
| `Unit Testing & CI.md` | [50-Testing & CI/51-unit-testing-ci.md](../50-testing-ci/51-unit-testing-ci.md) |
| `CI Platform Setup.md` | [50-Testing & CI/52-ci-platform-setup.md](../50-testing-ci/52-ci-platform-setup.md) |
| `Hardware Testing.md` | [50-Testing & CI/53-hardware-testing.md](../50-testing-ci/53-hardware-testing.md) |
| `Watchdog Test Guide.md` | [50-Testing & CI/54-watchdog-test-guide.md](../50-testing-ci/54-watchdog-test-guide.md) |
| `Flash & Debug Quickstart.md` | [60-Debugging/61-flash-debug-quickstart.md](../60-debugging/61-flash-debug-quickstart.md) |
| `Troubleshooting.md` | [60-Debugging/62-troubleshooting.md](../60-debugging/62-troubleshooting.md) |
| `Scripts & Tools.md` | [60-Debugging/63-scripts-and-tools.md](../60-debugging/63-scripts-and-tools.md) |
| `Version Management.md` | [70-Release & Productization/71-version-management.md](../70-release-productization/71-version-management.md) |
| `Zephyr Version & CI.md` | [70-Release & Productization/72-zephyr-version-ci.md](../70-release-productization/72-zephyr-version-ci.md) |
| `Release Checklist.md` | [70-Release & Productization/73-release-checklist.md](../70-release-productization/73-release-checklist.md) |
| `OTA & Storage Guide.md` | [70-Release & Productization/74-ota-storage-guide.md](../70-release-productization/74-ota-storage-guide.md) |
| `Security & Key Management.md` | [70-Release & Productization/75-security-key-management.md](../70-release-productization/75-security-key-management.md) |
| `Contributing & Code Style.md` | [80-Contributing/81-contributing-code-style.md](../80-contributing/81-contributing-code-style.md) |
| `Documentation Improvements.md` | [80-Contributing/82-doc-improvements.md](../80-contributing/82-doc-improvements.md) |

Even older filenames (before first organization):

| Former Filename | Current Path |
|-----------------|--------------|
| `FREESTANDING_APP.md` | [10-Environment & Build/12-freestanding-app-build.md](../10-environment-build/12-freestanding-app-build.md) |
| `Zephyr Devicetree & Memory Config Manual.md` | [40-App Development/44-devicetree-memory-config.md](../40-app-development/44-devicetree-memory-config.md) |
| `Developer Guide.md` | [00-Getting Started/04-developer-guide.md](04-developer-guide.md) |
| `Project Expansion.md` | [70-Release & Productization/74-ota-storage-guide.md](../70-release-productization/74-ota-storage-guide.md) |
| `Thread_IPC_Service Module Guide.md` | [30-Core Modules/33-thread-ipc-service-guide.md](../30-core-modules/33-thread-ipc-service-guide.md) |
| `Module Development Integrating Thread_IPC Guide.md` | [30-Core Modules/34-thread-ipc-integration-guide.md](../30-core-modules/34-thread-ipc-integration-guide.md) |
| `ZEPHYR_VERSION.md` | [70-Release & Productization/72-zephyr-version-ci.md](../70-release-productization/72-zephyr-version-ci.md) |
| `RELEASE_CHECKLIST.md` | [70-Release & Productization/73-release-checklist.md](../70-release-productization/73-release-checklist.md) |
| `Environment Setup Guide.md` | [10-Environment & Build/11-environment-setup.md](../10-environment-build/11-environment-setup.md) |

---

*Maintenance note: When adding new `docs/zh-CN/` documents, place them in the appropriate subdirectory with the same prefix number; also update this page's "Complete Manual Overview" and "Old Filename Reference".*
