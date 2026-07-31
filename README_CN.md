# Zeplod

基于 Zephyr RTOS 的高性能、实时、事件驱动应用基础。Zeplod 采用发布 - 订阅模式，为构建可扩展的模块化嵌入式应用提供坚实基础。

> **语言**: 中文版 | [English](README.md)

> 📺 **视频教程**：关注微信公众号 **硬核嵌入式**，正在连载本框架的详细教程！
> 💬 **技术交流**：加微信/QQ群 [待补充]，与 500+ 嵌入式开发者交流

---

## 🚀 新人从这里开始

| 我想... | 阅读文档 | 预计时间 |
|---------|----------|----------|
| **5 分钟看看效果** | [docs/5 分钟快速体验.md](docs/zh-CN/00-入门/01-5分钟快速体验.md) | 5 分钟 |
| **正式搭建环境** | [docs/zh-CN/10-环境与构建/11-环境搭建与配置指南.md](docs/zh-CN/10-环境与构建/11-环境搭建与配置指南.md) | 1-2 小时 |
| **开始写代码** | [docs/zh-CN/00-入门/04-开发者入门指南.md](docs/zh-CN/00-入门/04-开发者入门指南.md) | 1 小时 |
| **控制硬件（LED）** | [docs/zh-CN/00-入门/04-开发者入门指南.md](docs/zh-CN/00-入门/04-开发者入门指南.md) | 30 分钟 |
| **查术语** | [docs/zh-CN/00-入门/03-术语速查卡片.md](docs/zh-CN/00-入门/03-术语速查卡片.md) | 随时 |
| **浏览全部文档** | [docs/zh-CN/00-入门/02-文档索引.md](docs/zh-CN/00-入门/02-文档索引.md) | - |

> 💡 **提示**：第一次接触？从 **[5 分钟快速体验](docs/zh-CN/00-入门/01-5分钟快速体验.md)** 开始，无需开发板即可在 PC 上运行！

---

## 特性

- **事件驱动架构**：基于发布 - 订阅模式的核心事件系统
- **模块化设计**：动态模块注册和生命周期管理
- **实时性能**：基于优先级的调度，可配置的事件分发
- **系统服务**：集成的日志、内存管理、看门狗和定时器服务
- **可扩展性**：易于扩展的模块接口，用于添加业务逻辑
- **线程安全**：完整的线程安全操作和正确的同步机制
- **Shell 命令**：内置调试和监控的 Shell 命令
- **版本管理**：完整的软件版本跟踪，包含 Git 信息和编译时间
- **Thread IPC（应用内）**：可选的工作线程 + 分发线程、请求/响应队列，及与事件系统的桥接（见 `docs/zh-CN/30-核心模块/33-线程IPC服务使用说明.md`）
- **Data Bus（数据总线）**：命名通道、引用计数流数据共享，零拷贝多消费者分发，ISR/线程统一发布，自动释放与显式 Retain，可选事件系统桥接（见 `docs/zh-CN/30-核心模块/37-数据总线使用说明.md`）

## 适用场景

本框架基于事件驱动架构和模块化设计，特别适合以下嵌入式应用场景：

### 🏭 工业控制与自动化

| 场景 | 说明 |
|------|------|
| **工业网关** | 多协议数据采集、协议转换、边缘计算；事件系统天然支持异构数据流的解耦处理 |
| **PLC/DCS控制器** | 实时控制逻辑、I/O模块管理；模块化架构支持热插拔式功能扩展 |
| **数据采集终端** | 传感器数据聚合、本地存储、远程传输；事件队列提供缓冲能力 |
| **工业物联网节点** | 设备状态监控、预测性维护数据上报 |

### 🌐 物联网设备

| 场景 | 说明 |
|------|------|
| **智能传感器节点** | 多传感器数据融合、事件触发上报；低功耗设计支持电池供电 |
| **边缘计算设备** | 本地数据处理、AI推理预处理；IPC服务支持多线程并行计算 |
| **物联网网关** | 协议转换、设备管理、云端通信；模块化设计易于扩展通信协议 |
| **环境监测站** | 气象、水质、空气质量监测；定时采样 + 事件驱动告警 |

### 🏠 智能家居与楼宇

| 场景 | 说明 |
|------|------|
| **智能中控/网关** | 多设备协调控制、场景联动；事件系统实现设备间解耦通信 |
| **智能开关/面板** | 触摸交互、灯光控制、情景模式；模块化支持不同功能组合 |
| **安防报警主机** | 传感器联动、本地报警、远程通知；实时事件分发保证响应速度 |
| **暖通空调控制器** | 温湿度控制、定时任务、能耗管理 |

### 🚗 汽车电子

| 场景 | 说明 |
|------|------|
| **车载传感器节点** | 胎压监测、温度传感、门控模块；事件驱动架构减少轮询开销 |
| **车身控制模块(BCM)** | 车灯、车窗、门锁控制；模块化设计支持功能裁剪 |
| **仪表盘控制器** | 信号处理、显示驱动、报警逻辑；优先级事件保证关键告警实时性 |
| **车载诊断设备** | OBD数据采集、故障码解析、数据上传 |

### ⚕️ 医疗电子

| 场景 | 说明 |
|------|------|
| **便携式监测设备** | 心率、血氧、体温监测；事件触发异常告警 |
| **医疗数据采集器** | 多参数监护仪数据汇聚、本地存储、联网上传 |
| **康复训练设备** | 运动数据采集、反馈控制、训练记录 |
| **医疗物联网节点** | 设备状态监控、耗材管理、远程诊断支持 |

### 🤖 机器人与自动化设备

| 场景 | 说明 |
|------|------|
| **服务机器人控制器** | 传感器融合、行为决策、运动控制；事件系统支持异步传感器数据处理 |
| **AGV/AMR导航控制器** | 路径规划、避障控制、任务调度；IPC服务支持多线程协作 |
| **机械臂控制节点** | 关节控制、力反馈处理、安全监测 |
| **无人机飞控模块** | 姿态传感、电机控制、通信链路；高优先级事件处理紧急情况 |

### 📱 消费电子

| 场景 | 说明 |
|------|------|
| **可穿戴设备** | 运动数据采集、健康监测、消息提醒；低功耗事件驱动减少CPU唤醒 |
| **智能音箱/语音助手** | 语音唤醒、音频处理、云端交互；模块化架构分离前后端逻辑 |
| **电子阅读器/电子标签** | 显示刷新、按键响应、无线更新 |
| **游戏外设** | 手柄、方向盘、飞行摇杆；低延迟事件处理保证响应速度 |

### 📡 通信设备

| 场景 | 说明 |
|------|------|
| **无线传感网络节点** | 数据采集、多跳路由、低功耗休眠 |
| **串口服务器/CAN网关** | 协议转换、数据转发、设备管理 |
| **Modbus/Profibus设备** | 从站设备、协议栈实现；模块化易于移植不同协议 |
| **LoRa/NB-IoT终端** | 远程抄表、资产追踪、环境监测 |

### 🎓 教学与学习

| 场景 | 说明 |
|------|------|
| **RTOS教学平台** | 学习事件驱动、多线程、模块化设计等现代嵌入式软件工程实践 |
| **嵌入式系统课程设计** | 完整的项目结构、代码规范、测试框架可作为参考模板 |
| **毕业设计/科研项目** | 快速搭建原型，专注业务逻辑而非底层架构 |
| **企业内部培训** | 统一的开发规范、代码风格、CI/CD流程 |

### 🛠️ 开发原型与快速验证

| 场景 | 说明 |
|------|------|
| **产品原型开发** | 快速验证概念，native_sim 支持PC端仿真测试 |
| **技术方案评估** | 性能测试、资源评估、可行性分析 |
| **驱动/协议开发** | 隔离硬件相关代码，便于移植和测试 |
| **多平台产品线** | 统一架构、差异化模块，降低维护成本 |

### ✅ 框架选型建议

本框架**特别适合**以下特征的项目：

- ✅ 需要模块化、可扩展的架构
- ✅ 多传感器/多外设协同工作
- ✅ 实时性要求（事件优先级支持）
- ✅ 需要线程安全的模块间通信
- ✅ 产品形态多样、需要功能裁剪
- ✅ 团队协作开发、需要统一规范
- ✅ **32KB SRAM 极限场景**（使用 `conf/profiles/tiny.conf` 极限配置）

本框架**不太适合**以下场景：

- ❌ 极简单片机项目（资源受限，如8位MCU）
- ❌ 纯前后台/超级循环架构即可满足需求
- ❌ 对Flash/RAM占用极度敏感的低成本产品
- ❌ 不需要模块间通信的独立功能设备

> 💡 **32KB SRAM 解决方案**：使用 `conf/profiles/tiny.conf` 极限配置，框架占用 **< 10KB**，预留 **> 22KB** 给 APP 模块。详见 [配置方案对比指南](docs/zh-CN/40-应用开发/43-配置方案对比指南.md) 与 [conf/README.md](conf/README.md)。

### 📊 内存配置方案

| 配置方案 | 目标 SRAM | 框架占用 | APP 可用 | 配置文件 |
|---------|----------|---------|---------|---------|
| 标准版 | ≥ 256KB | ~190KB | ~66KB+ | 默认 `west build`（CMake 自动合并 standard + features） |
| 平衡版 | 64-128KB | ~40KB | ~24-88KB | `-DEXTRA_CONF_FILE=conf/profiles/balanced.conf` |
| 极简版 | 32-64KB | ~18KB | ~14-46KB | `-DEXTRA_CONF_FILE=conf/profiles/minimal.conf` |
| **极限版** | **≤ 32KB** | **< 10KB** | **> 22KB** | `-DCONF_FILE="prj.conf;conf/profiles/tiny.conf"` |

## 项目结构

```
zeplod/
├── APP_VERSION                       # 应用语义化版本（勿用文件名 VERSION，与 Zephyr 冲突）
├── CMakeLists.txt                    # 构建配置（独立应用需 ZEPHYR_BASE 或 zephyr_config.env）
├── Kconfig                           # 应用 Kconfig（含事件/模块/IPC 等）
├── Kconfig.zephyr                    # Zephyr 顶层 Kconfig 入口
├── prj.conf                          # 公共基线（框架核心开关）
├── conf/                             # 叠加配置片段（见 conf/README.md）
│   ├── profiles/                     # standard（默认）/ balanced / minimal / tiny
│   ├── features/                     # data_bus、thread_ipc（默认）及可选功能
│   ├── targets/                      # qemu
│   └── examples/                     # gpio_uart、module_ipc
├── app.overlay                       # 通用设备树覆盖
├── west.yml                          # West 清单（默认 4.3.0-rc3）
├── zephyr_config.env                 # 本地路径（由 template 复制生成，勿提交密钥）
├── zephyr_config.env.template
├── Doxyfile                          # API 文档生成配置
├── .clang-format                     # 代码格式化配置
├── .clang-tidy                       # 静态分析配置
├── .pre-commit-config.yaml           # pre-commit 钩子配置
├── boards/
│   ├── overlay.dts                   # 通用设备树覆盖
│   ├── nucleo_l4r5zi.overlay         # STM32 Nucleo L4R5ZI 板覆盖
│   └── mimxrt1050_fire_mimxrt1052_qspi.overlay
├── scripts/                          # 环境脚本、打包、版本管理
│   ├── setup_env.{sh,bat,ps1}        # 环境变量设置
│   ├── build_all.{sh,bat}            # 批量构建脚本
│   ├── analyze_map.{sh,bat,ps1}      # MAP 文件分析
│   ├── package_release.{sh,ps1}      # 发布打包
│   ├── bump_version.py               # 版本号同步
│   └── module_config.py              # 模块配置工具
├── tests/                            # ztest 单元测试（native_sim）
│   ├── CMakeLists.txt
│   ├── Kconfig                       # rsource 复用根目录 Kconfig
│   ├── prj.conf
│   ├── prj_native_sim.conf           # native_sim 平台叠加配置
│   ├── prj_test_watchdog.conf        # 看门狗测试叠加配置
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
├── docs/                             # 说明文档（总目录见 docs/zh-CN/00-入门/02-文档索引.md）
│   ├── 00-入门/                      # 5 分钟体验、文档索引、术语速查、入门指南
│   ├── 10-环境与构建/                # 环境搭建、独立应用构建
│   ├── 20-架构设计/                  # 模块化设计方法论、核心技术实现
│   ├── 30-核心模块/                  # 事件系统、模块系统、IPC、系统服务
│   ├── 40-应用开发/                  # 应用开发、配置项、配置方案对比、设备树
│   ├── 50-测试与CI/                  # 单元测试、CI 配置
│   ├── 60-调试与排错/                # 烧录调试、故障排除、脚本说明
│   ├── 70-发布与产品化/              # 版本管理、发布检查、OTA、安全
│   ├── 80-贡献与维护/                # 贡献指南、代码规范
│   └── 90-学习资源/                  # 嵌入式 AI、个人发展、项目评审
└── src/
    ├── core/                         # 事件系统核心
    │   ├── event_system.{c,h}        # 发布-订阅、类型管理
    │   ├── event_queue.{c,h}         # 事件队列（优先级、溢出处理）
    │   ├── event_dispatcher.{c,h}    # 事件分发器（独立线程、统计、暂停/恢复）
    │   ├── event_dispatcher_autoinit.c # SYS_INIT 自动初始化
    │   ├── event_memory.{c,h}        # Slab 内存管理（优先级分层池、大数据块池）
    │   └── event_system_compat.{c,h} # 事件系统兼容层
    ├── data_bus/                     # Data Bus：命名通道流数据共享
    │   ├── data_bus.{c,h}            # 核心：初始化/反初始化、分发线程、统计
    │   ├── data_bus_channel.{c,h}    # 通道管理（创建/查找/销毁/发布）
    │   ├── data_bus_consumer.{c,h}   # 消费者注册与分发
    │   ├── data_bus_memory.{c,h}     # 两级 Slab 内存池 + 引用计数生命周期
    │   ├── data_bus_event_bridge.c   # 事件系统桥接（可选）
    │   ├── data_bus_internal.h       # 内部共享声明
    │   └── Kconfig                   # Data Bus Kconfig 配置项
    ├── include/zeplod/               # 对外公开 API 头文件（#include <zeplod/...>）
    │   ├── zeplod_framework.h        # 伞头：事件系统 + 状态机 + 锁序
    │   ├── event_system.h
    │   ├── module_manager.h
    │   ├── app_config.h
    │   └── ...
    ├── services/                     # 系统服务（实现）
    │   ├── sys_log.c
    │   ├── sys_memory.c
    │   ├── sys_watchdog.c
    │   └── sys_timer.c
    ├── modules/                      # 模块管理器与 Thread IPC（实现）
    │   ├── module_manager.c
    │   ├── module_manager_compat.c
    │   └── ipc_service/              # Thread IPC 服务（Kconfig: THREAD_IPC_SERVICE）
    │       ├── ipc_service.c
    │       ├── ipc_service_event.c
    │       ├── ipc_shared_mem.c
    │       ├── ipc_service_example.c
    │       ├── CMakeLists.txt
    │       └── Kconfig
    ├── modules_examples/             # 示例模块 .c（头文件在 include/zeplod/）
    │   ├── example_module_a.c
    │   ├── example_module_b.c
    │   ├── example_module_gpio.c
    │   ├── example_module_uart.c
    │   ├── example_module_ipc.c
    │   └── example_module_multi_dep.c
    ├── app/                          # 应用层（实现）
    │   ├── app_main.c
    │   ├── app_version.c             # 版本 API（版本宏由根目录 APP_VERSION 经 -D 注入）
    │   └── app_kv.c                  # 应用键值存储（可选掉电保存）
```

## 复制为新项目（检查清单）

复制或 fork 本仓库后，建议按顺序完成下列项，便于在任意产品上落地（更细的说明见 **[docs/zh-CN/00-入门/04-开发者入门指南.md](docs/zh-CN/00-入门/04-开发者入门指南.md#从模板复制后的检查清单)**）。

| 步骤 | 内容 |
|------|------|
| 1 | **west.yml**：将 `revision` 与团队 Zephyr 版本对齐（默认 **4.3.0-rc3**，与 CI 一致）；私有镜像可改 `url`。 |
| 2 | **zephyr_config.env**：由 `zephyr_config.env.template` 复制并填写路径；**勿提交**（已在 `.gitignore` 中忽略）。 |
| 3 | **CMake**：根目录 `CMakeLists.txt` 中 `project(...)` 名称改为你的产品工程名。 |
| 4 | **版本与说明**：按需修改 `APP_VERSION`、README 标题与产品描述。 |
| 5 | **板型与 CI**：`prj.conf`、**`.github/workflows/ci.yml`** / **`.gitlab-ci.yml`** 中 ARM 矩阵 `board` 与目标硬件一致或按需裁剪。 |
| 6 | **示例代码**：`src/modules_examples/example_*` 可删除或替换（头文件在 `include/zeplod/`）；同步 `CMakeLists.txt`、Kconfig，各模块 **`.c` 的 `SYS_INIT` 注册**与 **`include/zeplod/app_config.h` 的 `APP_INIT_PRIO_*`**。 |

> **文档与 CI 的板型示例**：入门文档中可能出现 `nucleo_l4r5zi` 等示例板名；CI 当前固定为若干 Nucleo/Disco 板。**以你手头的 `BOARD` 与 CI 矩阵为准**；若遇 RAM/链接问题，见 **[docs/zh-CN/40-应用开发/44-设备树与内存配置手册.md](docs/zh-CN/40-应用开发/44-设备树与内存配置手册.md)**。

## 快速开始

### 前提条件

- Zephyr SDK（0.16.x 或更高版本）
- CMake（3.20.0 或更高版本）
- Python 3.8+
- West（Zephyr 构建工具）

CI（`.github/workflows/ci.yml`）当前使用 Zephyr **4.3.0-rc3** 构建镜像；本地建议使用相同或兼容的 Zephyr 版本，以减少配置差异。

### 项目类型：独立应用程序 (Freestanding Application)

本项目是一个 **Zephyr 独立应用程序**，应用程序和 Zephyr 源代码位于不同的目录中。

```
<home>/
├── zephyrproject/          # Zephyr 工作区
│   ├── .west/
│   ├── zephyr/             # Zephyr 源代码 (ZEPHYR_BASE)
│   ├── modules/            # Zephyr 模块
│   └── ...
└── zeplod/       # 应用程序目录 (本目录)
    ├── CMakeLists.txt
    ├── prj.conf
    ├── src/
    └── ...
```

**重要**：构建时需要设置 `ZEPHYR_BASE` 环境变量指向 Zephyr 源代码目录。

### 配置（本地路径）

本项目使用本地 Zephyr SDK 和源代码路径。构建前请进行配置：

#### 方法 1：编辑配置文件

1. 复制 `zephyr_config.env.template` 到 `zephyr_config.env`
2. 编辑 `zephyr_config.env` 中的路径：

```bash
# 编辑此文件，填入您的本地路径
ZEPHYR_SDK_INSTALL_DIR=C:/zephyr-sdk
ZEPHYR_BASE=D:/zephyrproject/zephyr
```

#### 方法 2：运行设置脚本

```bash
# Windows (PowerShell)
.\scripts\setup_env.ps1

# Windows (CMD)
scripts\setup_env.bat

# Linux/macOS
source scripts/setup_env.sh
```

#### 方法 3：设置环境变量

```bash
# Windows (PowerShell)
$env:ZEPHYR_BASE="D:/zephyrproject/zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR="C:/zephyr-sdk"

# Linux/macOS
export ZEPHYR_BASE=/path/to/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk
```

### 构建

```bash
# 构建目标开发板
west build -b <your_board> .

# 构建 native_sim（用于主机测试）
west build -b native_sim .

# 使用特定配置文件（可合并多个）
west build -b <your_board> . -- -DCONF_FILE="prj.conf;conf/examples/module_ipc.conf"

# 清理并重新构建
west build -t pristine
west build -b <your_board> .
```

### 烧录

```bash
west flash
```

### 监控输出

```bash
west console
```

### 使用自定义开发板

本项目支持在 `boards/` 目录中添加自定义开发板，无需修改 Zephyr 源代码。

**开发板目录结构**：

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

**构建命令**：

```bash
# 在根目录 CMakeLists.txt 中取消注释 BOARD_ROOT 后，可将自定义板放在 boards/ 下
# list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/boards)
west build -b vendor/board_name .
```

**设备树覆盖**：

- 通用覆盖：`boards/overlay.dts`
- 开发板特定覆盖：`boards/<board_name>.overlay`
- 使用 FILE_SUFFIX：`boards/<board_name>_<suffix>.overlay`

## 架构概览

### 事件系统

核心事件系统提供：
- **事件类型**：256 个唯一事件类型（0-255）
- **事件优先级**：低、普通、高、关键（数值越小优先级越高）
- **订阅者**：每个事件类型最多 16 个订阅者
- **队列**：可配置的事件队列（默认 32 个事件）
- **Slab 内存池**：优先级分层（CRITICAL/HIGH/NORMAL）和大数据块（256B/1KB/4KB）
- **实时安全 API**：`_rt` 后缀 API 完全从 Slab 池分配，O(1) 确定时间

```c
// 订阅事件
event_subscribe(EVENT_TYPE_SENSOR_DATA, my_callback, user_data, &subscriber_id);

// 发布事件（推荐，自动内存管理）
event_publish_copy(EVENT_TYPE_SENSOR_DATA, EVENT_PRIORITY_NORMAL, &data, sizeof(data));

// ISR/实时任务使用（实时安全，Slab 池分配）
event_publish_copy_rt(EVENT_TYPE_SENSOR_DATA, EVENT_PRIORITY_HIGH, &data, sizeof(data));
```

### 模块系统

模块是独立的业务逻辑单元：
- **生命周期**：init → start → run → stop → shutdown
- **注册**：通过模块管理器动态注册；主固件在 **`SYS_INIT(POST_KERNEL, APP_INIT_PRIO_*)`** 中自动注册（见 `include/zeplod/app_config.h` 与各 `example_module_*.c`）
- **事件处理**：自动将事件路由到订阅的模块
- **隔离**：每个模块有自己的状态和配置

```c
// 定义模块接口
DECLARE_MODULE_INTERFACE(my_module);

// 典型：在模块 .c 内用 Zephyr 自动初始化注册（需在 app_config.h 定义 APP_INIT_PRIO_MODULE_*）
static int my_module_auto_register(void) {
    uint32_t id;
    return module_manager_register(my_module_get_interface(), &config, &id) ? -EIO : 0;
}
SYS_INIT(my_module_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_MINE);
```

### 系统服务

| 服务 | 描述 |
|------|------|
| `sys_log` | 统一日志：分级、内存环、控制台 printk；可选 UART 位与控制台共用输出路径；启用 `CONFIG_SEGGER_RTT` 时可写 RTT |
| `sys_memory` | 内存池管理，带分配跟踪 |
| `sys_watchdog` | 硬件/软件看门狗，提高可靠性 |
| `sys_timer` | 高分辨率单次和周期定时器 |

### Data Bus 数据总线

Data Bus 提供命名通道、引用计数流数据共享，完全独立于事件系统：

- **命名通道**：全局唯一名称，运行时动态创建/查找
- **零拷贝共享**：数据只拷贝一次到 slab 块，多消费者共享同一块内存
- **ISR / 线程统一发布**：`data_bus_publish()` 自动检测上下文并适配
- **自动释放**：消费者回调返回后框架自动 `release`（默认）
- **显式 Retain**：需要异步持有时，回调内调用 `data_bus_block_retain()`
- **事件桥接**：可选桥接到事件系统，发送轻量级通知
- **内存确定性**：全部来自预分配 slab，ISR 路径不依赖 `k_malloc`

```c
/* 创建通道并注册消费者 */
data_bus_channel_t *ch;
data_bus_channel_create("imu", &ch);

data_bus_consumer_cfg_t cfg = {
    .name     = "imu_fusion",
    .callback = imu_consumer_cb,
};
data_bus_consumer_register(ch, &cfg, NULL);

/* ISR 或线程中发布 */
data_bus_publish(ch, &imu_data, sizeof(imu_data));
```

> **Data Bus 与事件系统的区别**：Data Bus 适用于流式数据块（任意大小、引用计数），事件系统适用于离散事件（固定大小、发布即忘）。传感器流和大数据块共享用 Data Bus，控制命令和状态通知用事件系统。

## 配置

### Kconfig 选项

```kconfig
# 事件系统
CONFIG_EVENT_SYSTEM=y
CONFIG_EVENT_QUEUE_SIZE=64
CONFIG_EVENT_MAX_SUBSCRIBERS=16
CONFIG_EVENT_DISPATCHER_STACK_SIZE=2048
CONFIG_EVENT_DISPATCHER_PRIORITY=5

# 模块管理器
CONFIG_MODULE_MANAGER=y
CONFIG_MAX_MODULES=16
# 可选：运行时按 depends_on 拓扑启动/逆序停止（详见 docs/zh-CN/30-核心模块/32-模块系统详细使用说明.md）
# CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES=y
# CONFIG_MODULE_MANAGER_DEPENDS_LIST_MAX=16
# CONFIG_MODULE_MANAGER_START_ALL_ABORT_ON_FAILURE=y
# CONFIG_EXAMPLE_MODULE_MULTI_DEP=y

# 系统服务
CONFIG_SYS_LOG_LEVEL=3          # 0=关闭，1=错误，2=警告，3=信息，4=调试
CONFIG_SYS_MEMORY_POOL_SIZE=8192
CONFIG_SYS_WATCHDOG_ENABLE=y
CONFIG_SYS_WATCHDOG_TIMEOUT_MS=5000
```

### 应用配置

编辑 `include/zeplod/app_config.h` 自定义：
- 功能标志（启用/禁用模块和服务）
- 任务优先级和栈大小
- 定时配置
- 事件类型定义

## Shell 命令

应用程序提供以下 Shell 命令用于调试：

```bash
# 应用状态
app status

# 模块信息
app modules

# 事件统计
app events

# 内存统计
app memory

# 日志转储
app log [level]

# 应用键值（字符串 key/value；掉电保存见 conf/features/app_kv_persist.conf + boards/nucleo_l4r5zi.overlay）
app kv list
app kv set mykey hello world
app kv get mykey
app kv del mykey
app kv clear
app kv save
app kv load

# 帮助
app help
```

## 创建自定义模块

1. 创建模块头文件（`include/zeplod/my_module.h`）：

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

2. 实现模块（`src/modules/my_module.c`）：

```c
#include <zeplod/my_module.h>

static my_module_cb_t g_my_module;

int my_module_init(void *config) {
    // 初始化模块
    return 0;
}

int my_module_start(void) {
    // 启动模块操作
    return 0;
}

// ... 实现其他接口函数

DECLARE_MODULE_INTERFACE(my_module);
```

3. 在 **`my_module.c`** 中用 **`SYS_INIT`** 注册（并在 **`include/zeplod/app_config.h`** 增加 **`APP_INIT_PRIO_MODULE_MINE`**，取值在 **`APP_INIT_PRIO_MODULE_MGR`** 与 **`APP_INIT_PRIO_APP_FINAL`** 之间；若依赖其它已注册模块，优先级数值应**更大**以更晚执行）：

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

多模块依赖、`depends_on` 写法与 Kconfig 开关说明见 **docs/zh-CN/30-核心模块/32-模块系统详细使用说明.md** 中的「应用启动与初始化顺序（Zephyr SYS_INIT）」「运行时依赖」与「配置选项」；多依赖示例源码为 `src/modules_examples/example_module_multi_dep.c`。

## 事件流程

```
┌─────────────┐     ┌──────────────┐     ┌─────────────┐
│   模块 A     │────▶│  事件系统     │────▶│   模块 B     │
│  (发布者)    │     │  (分发器)     │     │  (订阅者)    │
└─────────────┘     └──────────────┘     └─────────────┘
                           │
                           ▼
                    ┌─────────────┐
                    │  事件队列    │
                    └─────────────┘
```

## 最佳实践

1. **事件设计**：保持事件小而专注。对大数据负载使用事件数据指针。
2. **模块隔离**：模块不应直接调用其他模块。使用事件进行模块间通信。
3. **优先级分配**：为时间关键事件使用适当的优先级。
4. **内存管理**：始终释放动态分配的事件数据。
5. **错误处理**：在所有模块回调中实现适当的错误处理。
6. **看门狗**：在长时间运行的操作中定期喂看门狗。

## 调试

### 启用调试日志

```kconfig
CONFIG_LOG_DEFAULT_LEVEL=4
CONFIG_SYS_LOG_LEVEL=4
```

### 检查统计信息

```c
// 事件系统统计
uint32_t total, depth, dropped;
event_get_statistics(&total, &depth, &dropped);

// 模块管理器统计
module_mgr_stats_t stats;
module_manager_get_stats(&stats);
```

### 内存泄漏检测

```c
// 检查泄漏
uint32_t leaks = sys_mem_check_leaks(SYS_MEM_POOL_GENERAL);
if (leaks > 0) {
    sys_mem_dump_allocations(SYS_MEM_POOL_GENERAL);
}
```

## 单元测试

使用 `tests/` 下的 ztest，与主应用共享 `src/` 实现（不链接 `app_main`、示例业务模块）；默认 **开启** `CONFIG_THREAD_IPC_SERVICE` 并编入 `ipc_service` 做烟测，堆大小已加大（见 `tests/prj.conf`）。

```bash
# native_sim 平台
west build -b native_sim tests/ --build-dir build_tests
west build -t run --build-dir build_tests

# native_sim 平台
west build -b native_sim tests/ --build-dir build_tests_sim
west build -t run --build-dir build_tests_sim
```

需设置 `ZEPHYR_BASE` 或提供根目录 `zephyr_config.env`。详见 tests/README.md。

## 许可证

本项目采用 **GNU General Public License v3.0 (GPL-3.0)** - 详见 LICENSE 文件。

### 双许可证模式

本项目采用**双许可证**模式：

- **GPL v3**（免费）：个人学习、研究、开源项目可免费使用，但**必须开源你的衍生代码**
- **商业许可证**（付费）：企业用户、闭源商业产品需购买许可证，可闭源商用

📧 获取商业许可证：发送邮件至 [china_qzh@163.com](mailto:china_qzh@163.com)

📄 详细条款：详见 [LICENSE_COMMERCIAL.md](LICENSE_COMMERCIAL.md)

> ⚠️ 违反 GPL 条款使用本代码将面临法律风险！购买商业许可证可合法闭源使用。

## 贡献

### 提交代码（Pull Request）

1. Fork 仓库
2. 创建功能分支（例如 `feature/my-feature` 或 `fix/issue-123`）
3. 进行更改，确保构建和测试通过
4. **使用规范的 Commit 格式**（见下文）
5. 提交 Pull Request

**Commit 格式**：采用 Conventional Commits 规范

```
<type>(<scope>): <subject>
```

**常用 type**：
- `feat`: 新功能
- `fix`: Bug 修复
- `docs`: 文档更新
- `style`: 代码格式（不改逻辑）
- `refactor`: 重构
- `perf`: 性能优化
- `test`: 测试相关
- `build`: 构建系统
- `ci`: CI/CD
- `chore`: 其他琐事

**示例**：
```bash
git commit -m "feat(sys_memory): 增加堆泄漏检测功能"
git commit -m "fix(event): 修复事件队列统计错误"
git commit -m "docs(ci): 更新 CI 板型配置说明"
```

**❌ 避免模糊的提交信息**：
```bash
git commit -m "修改"           # 过于模糊
git commit -m "更新代码"       # 没有信息量
git commit -m "修复 bug"       # 未说明修复什么
```

更详细的规范见 **[docs/zh-CN/80-贡献与维护/81-参与贡献与代码规范.md](docs/zh-CN/80-贡献与维护/81-参与贡献与代码规范.md)**。

## 支持

如有问题和功能请求，请在项目仓库中提交 Issue。

---

**版本**：1.0.0（单一来源：根目录 `APP_VERSION`；发布前可运行 `python scripts/bump_version.py X.Y.Z` 同步 Doxygen/README）
**构建类型**：Release/Debug
**目标**：通用/Zephyr 支持的开发板

## 文档索引

**总目录（推荐阅读顺序、名词表、旧文件名对照）**：[docs/zh-CN/00-入门/02-文档索引.md](docs/zh-CN/00-入门/02-文档索引.md)

| 文档 | 说明 |
|------|------|
| [docs/zh-CN/00-入门/02-文档索引.md](docs/zh-CN/00-入门/02-文档索引.md) | **总入口**：学习路径、全部手册列表 |
| [docs/zh-CN/10-环境与构建/11-环境搭建与配置指南.md](docs/zh-CN/10-环境与构建/11-环境搭建与配置指南.md) | 工具链、路径、验证构建 |
| [docs/zh-CN/10-环境与构建/12-Freestanding应用与构建基础.md](docs/zh-CN/10-环境与构建/12-Freestanding应用与构建基础.md) | 独立应用、`ZEPHYR_BASE`、overlay |
| [docs/zh-CN/00-入门/04-开发者入门指南.md](docs/zh-CN/00-入门/04-开发者入门指南.md) | 日常开发、测试、调试 |
| [docs/zh-CN/40-应用开发/41-Zephyr应用开发与服务指南.md](docs/zh-CN/40-应用开发/41-Zephyr应用开发与服务指南.md) | Zephyr 通用技术与服务开发纲要 |
| [docs/zh-CN/40-应用开发/44-设备树与内存配置手册.md](docs/zh-CN/40-应用开发/44-设备树与内存配置手册.md) | Devicetree、SRAM、`app.overlay` |
| [docs/zh-CN/40-应用开发/42-项目配置项说明.md](docs/zh-CN/40-应用开发/42-项目配置项说明.md) | **Kconfig 与应用配置项集中说明** |
| [docs/zh-CN/30-核心模块/31-事件系统详细使用说明.md](docs/zh-CN/30-核心模块/31-事件系统详细使用说明.md) | 事件 API 与用法 |
| [docs/zh-CN/30-核心模块/32-模块系统详细使用说明.md](docs/zh-CN/30-核心模块/32-模块系统详细使用说明.md) | 模块生命周期、运行时依赖 |
| [docs/zh-CN/30-核心模块/33-线程IPC服务使用说明.md](docs/zh-CN/30-核心模块/33-线程IPC服务使用说明.md) | Thread IPC 服务 |
| [docs/zh-CN/30-核心模块/34-Thread_IPC模块集成指南.md](docs/zh-CN/30-核心模块/34-Thread_IPC模块集成指南.md) | 在模块中集成 IPC |
| [docs/zh-CN/30-核心模块/37-数据总线使用说明.md](docs/zh-CN/30-核心模块/37-数据总线使用说明.md) | Data Bus：命名通道流数据共享 |
| [docs/zh-CN/70-发布与产品化/74-OTA与存储扩展指南.md](docs/zh-CN/70-发布与产品化/74-OTA与存储扩展指南.md) | OTA、NVS、低功耗（可选） |
| [docs/zh-CN/70-发布与产品化/71-版本管理.md](docs/zh-CN/70-发布与产品化/71-版本管理.md) | 版本号与构建信息 |
| [docs/zh-CN/70-发布与产品化/72-Zephyr版本与CI说明.md](docs/zh-CN/70-发布与产品化/72-Zephyr版本与CI说明.md) | 与 CI 镜像版本对齐 |
| [docs/zh-CN/70-发布与产品化/73-发布检查清单.md](docs/zh-CN/70-发布与产品化/73-发布检查清单.md) | 发布前检查 |
| [docs/zh-CN/60-调试与排错/62-常见问题与故障排除.md](docs/zh-CN/60-调试与排错/62-常见问题与故障排除.md) | 构建与环境排错 |
| [docs/zh-CN/60-调试与排错/61-烧录与调试快速指南.md](docs/zh-CN/60-调试与排错/61-烧录与调试快速指南.md) | 烧录、串口、调试 |
| [docs/zh-CN/60-调试与排错/63-脚本与工具说明.md](docs/zh-CN/60-调试与排错/63-脚本与工具说明.md) | `scripts/` 脚本说明 |
| [docs/zh-CN/50-测试与CI/51-单元测试与持续集成说明.md](docs/zh-CN/50-测试与CI/51-单元测试与持续集成说明.md) | ztest 与 CI 概览 |
| [docs/zh-CN/50-测试与CI/52-CI平台配置保姆级手册.md](docs/zh-CN/50-测试与CI/52-CI平台配置保姆级手册.md) | **GitHub / GitLab** 上启用与维护 CI 的逐步说明 |
| [docs/zh-CN/30-核心模块/36-系统服务使用说明.md](docs/zh-CN/30-核心模块/36-系统服务使用说明.md) | sys_log / sys_memory / sys_timer / sys_watchdog |
| [docs/zh-CN/80-贡献与维护/81-参与贡献与代码规范.md](docs/zh-CN/80-贡献与维护/81-参与贡献与代码规范.md) | PR、代码风格与 CI |
| [docs/zh-CN/70-发布与产品化/75-安全与密钥管理说明.md](docs/zh-CN/70-发布与产品化/75-安全与密钥管理说明.md) | 密钥、Secret、OTA 签名注意 |
| [tests/README.md](tests/README.md) | 单元测试（详细） |
| [LICENSE](LICENSE) | GPL v3 全文 |
| [LICENSE_COMMERCIAL.md](LICENSE_COMMERCIAL.md) | 商业许可证条款 |
