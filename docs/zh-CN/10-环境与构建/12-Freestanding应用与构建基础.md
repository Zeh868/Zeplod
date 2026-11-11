> 语言: **中文** | [English](../../en/10-environment-build/12-freestanding-app-build.md)

> **本文档范围**：Freestanding 应用概念、`ZEPHYR_BASE`、常规 `west build`、`BOARD_ROOT` / overlay 规则（概要）。
> **另见**：[15-新建APP开发指南](15-新建APP开发指南.md)（APP 子模块仓库）· [14-QEMU仿真运行指南](14-QEMU仿真运行指南.md)（无硬件仿真）· [44-设备树与内存配置手册](../40-应用开发/44-设备树与内存配置手册.md)（overlay 细节）

# Freestanding 应用与构建基础

本文说明本工程如何作为 **独立 Zephyr 应用（freestanding application）** 进行配置与构建。

## 什么是独立应用？

**独立应用**指源码放在 **Zephyr 工作区之外** 的应用：应用目录与 Zephyr 源码目录相互独立，例如：

```
<home>/
├── zephyrproject/              # Zephyr 工作区
│   ├── .west/
│   ├── zephyr/                 # Zephyr 源码（ZEPHYR_BASE）
│   ├── modules/
│   └── ...
└── zeplod/            # 本应用目录
    ├── CMakeLists.txt
    ├── prj.conf
    ├── src/
    └── ...
```

与下列形态不同：

- **仓库内应用**：位于 Zephyr 仓库内部的应用目录。
- **工作区内应用**：在 west 工作区内，但不在 `zephyr/` 仓库目录下的应用。

## 环境配置

### 设置 ZEPHYR_BASE

构建系统需要知道 Zephyr 的安装位置，通常通过环境变量 **`ZEPHYR_BASE`** 指定。

**方式一：使用 `zephyr_config.env`（推荐）**

本仓库提供 **`zephyr_config.env`**，可在构建前写入所需变量（示例路径请按本机修改）：

```bash
ZEPHYR_BASE=D:/Code/1-github-code/zephyrproject/zephyr
ZEPHYR_SDK_INSTALL_DIR=D:/Code/1-github-code/zephyr-sdk-0.17.2
```

将 **`zephyr_config.env.template`** 复制为 **`zephyr_config.env`** 后编辑即可。本工程 **`CMakeLists.txt`** 会在未设置 **`ZEPHYR_BASE`** 时尝试读取该文件（与官方 freestanding 流程一致）。

**方式二：系统环境变量**

直接在终端或系统中设置：

```bash
# Windows (PowerShell)
$env:ZEPHYR_BASE="D:/zephyrproject/zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR="C:/zephyr-sdk"

# Linux / macOS
export ZEPHYR_BASE=/path/to/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk
```

**方式三：CMake 命令行**

将变量传给 CMake / west：

```bash
west build -b <board> . -- -DZEPHYR_BASE=/path/to/zephyr
```

## 构建

### 常规构建

```bash
# 配置并编译（在应用根目录执行）
west build -b <board> .

# 或直接使用 CMake
cmake -B build -GNinja -DBOARD=<board>
cmake --build build
```

### 指定配置文件

```bash
# 使用指定 Kconfig 片段
west build -b <board> . -- -DCONF_FILE=prj.conf

# 合并多个片段
west build -b <board> . -- -DCONF_FILE="prj.conf;prj_extra.conf"
```

### 清理构建

```bash
# 清理产物（通常保留部分 CMake 缓存）
west build -t clean

# 完全清理构建目录（pristine）
west build -t pristine
```

修改 **`prj.conf`**、**`app.overlay`** 等后，若行为异常，建议使用 **`west build ... -p always`** 或 **`pristine`** 后重编。

## QEMU 仿真（无硬件）

Windows 等无 Linux/WSL 环境时，用 QEMU 板型做启动冒烟。完整流程、板型列表、`run_qemu.ps1` 参数与 ztest 见 **[14-QEMU仿真运行指南.md](14-QEMU仿真运行指南.md)**。

## 基于 framework 子模块的 APP 仓库

产品固件推荐独立 APP 仓库 + `framework/` 子模块。目录模板、`zephyr_app.env`、overlay 与验收清单见 **[15-新建APP开发指南.md](15-新建APP开发指南.md)**；脚本布局见 **[63-脚本与工具说明.md](../60-调试与排错/63-脚本与工具说明.md)**。

## 自定义开发板（可选）

在不修改 Zephyr 上游树的前提下，可在本仓库内增加自定义板。

### BOARD_ROOT

在 **`find_package(Zephyr)` 之前** 于 **`CMakeLists.txt`** 中设置 **`BOARD_ROOT`**，指向包含板定义文件的目录。本仓库默认将 **`BOARD_ROOT`** 相关行**注释掉**（使用 Zephyr 自带的 `nucleo_*` 等板时不需要）：

```cmake
# 仅在 boards/ 下放置自定义板时取消注释：
# list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/boards)
```

取消注释后，可将自定义板放在 **`boards/<厂商>/<板名>/`** 下，结构与 Zephyr 文档 **[Custom Board](https://docs.zephyrproject.org/latest/hardware/porting/board_porting.html)** 一致，通常包含：

```
boards/
└── vendor/
    └── board_name/
        ├── board_name_defconfig
        ├── board_name.dts
        ├── board_name.yaml
        ├── board.cmake
        ├── Kconfig.defconfig
        ├── Kconfig.board
        └── board.h
```

构建示例：

```bash
west build -b vendor/board_name .
```

### Devicetree Overlay

Zephyr 自动合并的 overlay 须为 **`.overlay`** 后缀，并由 **`configuration_files.cmake`** 按目录与板名解析（详见 **[设备树与内存配置手册.md](../40-应用开发/44-设备树与内存配置手册.md)**）。常见用法：

| 文件 | 说明 |
|------|------|
| 应用根目录 **`app.overlay`** | 未命中板级专用 overlay 时的回退；本仓库用于扩展 SRAM 等 |
| **`boards/<board>.overlay`** | 仅构建该 `board` 时参与合并 |
| **`boards/<board>_<suffix>.overlay`** | 与 **`-DFILE_SUFFIX=<suffix>`** 联用 |

注意：**`boards/overlay.dts`** 等 **非 `.overlay`** 文件**不会**被上述逻辑自动当作 overlay，除非通过 **`DTC_OVERLAY_FILE`** 显式指定。

示例：

```bash
west build -b nucleo_l4r5zi . -DFILE_SUFFIX=mouse
```

## 自定义 SoC（可选）

若需自定义 SoC 描述，可在 **`find_package(Zephyr)` 之前** 设置 **`SOC_ROOT`**：

```cmake
list(APPEND SOC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/soc)
```

目录结构可参考 Zephyr 文档中的 SoC 移植说明，例如：

```
soc/
└── vendor/
    └── soc_family/
        ├── Kconfig
        ├── Kconfig.soc
        └── Kconfig.defconfig
```

## 自定义 DTS / 绑定（可选）

若需额外设备树或绑定搜索路径，可设置 **`DTS_ROOT`**：

```cmake
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/dts)
```

典型目录可包含 `dts/include`、`dts/bindings` 等，与 Zephyr 文档一致。

## West 与清单文件

本仓库是否包含 **`west.yml`** 均可；**独立应用**常见做法是只设置 **`ZEPHYR_BASE`** 后构建。

### 方式一：不使用 west.yml（常见）

配合脚本或手动设置环境变量后：

```bash
# Linux / macOS
source scripts/setup_env.sh

# Windows：使用 scripts/setup_env.bat 或手动设置

west build -b <board> .
```

### 方式二：使用 west.yml

若在本应用目录初始化独立 west 工作区：

```bash
west init -l .
west update
west build -b <board> .
```

具体以团队工作流为准。

## 故障排除

### 提示 `ZEPHYR_BASE` 未设置

```text
FATAL_ERROR: ZEPHYR_BASE not set and zephyr_config.env not found.
```

**处理**：

1. 复制 **`zephyr_config.env.template`** 为 **`zephyr_config.env`**，填入本机 **`ZEPHYR_BASE`**、**`ZEPHYR_SDK_INSTALL_DIR`**。
2. 或在环境中手动 **`export` / `$env:`** 设置 **`ZEPHYR_BASE`**。

### 找不到开发板

```text
error: Board 'vendor/board_name' not found
```

**处理**：

1. 确认 **`boards/vendor/board_name/`** 存在且结构完整。
2. 若使用自定义板，确认 **`CMakeLists.txt`** 中已 **`list(APPEND BOARD_ROOT ...)`** 并指向正确目录。
3. 检查 **`board_name_defconfig`**、**`board_name.dts`**、**`board_name.yaml`** 等是否齐全。

### 构建目录与板型不匹配

```text
error: Build directory is not configured for this board
```

**处理**：

```bash
west build -t pristine
west build -b <board> .
```

## 参考链接

- [Zephyr Application Development](https://docs.zephyrproject.org/latest/application/index.html)
- [Freestanding Applications](https://docs.zephyrproject.org/latest/application/index.html#zephyr-freestanding-app)
- [Custom Board Definitions](https://docs.zephyrproject.org/latest/application/index.html#custom-board-definition)
- [Important Build Variables](https://docs.zephyrproject.org/latest/application/index.html#important-build-vars)
- 本仓库：**[设备树与内存配置手册.md](../40-应用开发/44-设备树与内存配置手册.md)**
