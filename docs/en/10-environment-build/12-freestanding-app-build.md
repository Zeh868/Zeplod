> Language: [中文](../../zh-CN/10-环境与构建/12-Freestanding应用与构建基础.md) | **English**

# Freestanding Zephyr Application Configuration Guide

This document explains how this project is configured and built as a **freestanding Zephyr application**.

## What is a Freestanding Application?

A **freestanding application** is an application whose source code is placed **outside the Zephyr workspace**: the application directory and Zephyr source directory are independent, for example:

```
<home>/
├── zephyrproject/              # Zephyr workspace
│   ├── .west/
│   ├── zephyr/                 # Zephyr source (ZEPHYR_BASE)
│   ├── modules/
│   └── ...
└── zeplod/            # This application directory
    ├── CMakeLists.txt
    ├── prj.conf
    ├── src/
    └── ...
```

This differs from:

- **In-tree application**: An application directory located inside the Zephyr repository.
- **Workspace application**: An application that is within the west workspace but not in the `zephyr/` repository directory.

## Environment Configuration

### Setting ZEPHYR_BASE

The build system needs to know where Zephyr is installed, typically specified via the environment variable **`ZEPHYR_BASE`**.

**Method 1: Using `zephyr_config.env` (Recommended)**

This repository provides **`zephyr_config.env`**, which can write the required variables before building (modify example paths for your machine):

```bash
ZEPHYR_BASE=D:/Code/1-github-code/zephyrproject/zephyr
ZEPHYR_SDK_INSTALL_DIR=D:/Code/1-github-code/zephyr-sdk-0.17.2
```

Copy **`zephyr_config.env.template`** to **`zephyr_config.env`** and edit it. The project's **`CMakeLists.txt`** attempts to read this file when **`ZEPHYR_BASE`** is not set (consistent with the official freestanding workflow).

**Method 2: System Environment Variables**

Set directly in the terminal or system:

```bash
# Windows (PowerShell)
$env:ZEPHYR_BASE="D:/zephyrproject/zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR="C:/zephyr-sdk"

# Linux / macOS
export ZEPHYR_BASE=/path/to/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk
```

**Method 3: CMake Command Line**

Pass variables to CMake / west:

```bash
west build -b <board> . -- -DZEPHYR_BASE=/path/to/zephyr
```

## Building

### Regular Build

```bash
# Configure and compile (run in application root directory)
west build -b <board> .

# Or use CMake directly
cmake -B build -GNinja -DBOARD=<board>
cmake --build build
```

### Specifying Configuration Files

```bash
# Use specified Kconfig fragment
west build -b <board> . -- -DCONF_FILE=prj.conf

# Combine multiple fragments
west build -b <board> . -- -DCONF_FILE="prj.conf;prj_extra.conf"
```

### Cleaning Build

```bash
# Clean artifacts (usually keeps some CMake cache)
west build -t clean

# Completely clean build directory (pristine)
west build -t pristine
```

After modifying **`prj.conf`**, **`app.overlay`**, etc., if behavior is abnormal, use **`west build ... -p always`** or **`pristine`** before rebuilding.

## QEMU simulation (no hardware)

On **Windows 11** and other hosts, Zephyr QEMU boards can replace `native_sim` for boot smoke tests (`native_sim` requires Linux/WSL). This repo provides:

- **`boards/qemu_*.overlay`** — auto-merged for `west build -b qemu_<board>`
- **`conf/targets/qemu.conf`** — slim Kconfig overlay for simulation
- **`scripts/run_qemu.ps1`** — one-shot build and run on Windows (default `qemu_riscv32`)

Quick start:

```powershell
. .\scripts\setup_env.ps1
.\scripts\run_qemu.ps1
```

Manual build:

```bash
west build -b qemu_riscv32 -d build_qemu . -p always \
  -- "-DCONF_FILE=prj.conf;conf/targets/qemu.conf"
west build -t run --build-dir build_qemu
```

Set **`QEMU_BIN_PATH`** in **`zephyr_config.env`**. See **[14-qemu-simulation-guide.md](14-qemu-simulation-guide.md)** for board matrix and troubleshooting.

## APP repos with a `framework/` submodule

> **Step-by-step guide** to create a new APP repo (layout, CMakeLists template, Kconfig/QEMU/scripts, checklist): **[15-creating-new-app-guide.md](15-creating-new-app-guide.md)**.

Business projects (e.g. **zephyr_gateway**) vendor this framework under **`framework/`** and add a top-level `CMakeLists.txt` plus `*_prj.conf`:

```
my_app/
├── CMakeLists.txt          # add_subdirectory(framework)
├── gateway_prj.conf
├── src/
├── modules/
└── framework/              # this repo as submodule
    ├── scripts/
    ├── zephyr_config.env
    └── tests/
```

Build from the APP root: `west build -b <board> .`. For QEMU, tests, and QA, use **`framework/scripts/`** (auto-detects APP layout — see **[63-scripts-and-tools.md](../60-debugging/63-scripts-and-tools.md)**):

```powershell
.\framework\scripts\setup_env.ps1
.\framework\scripts\run_qemu.ps1
.\framework\scripts\qa.ps1 -Mode all
```

Optional **`zephyr_app.env`** at the APP root (`zephyr_app.env.template` in the framework repo).

## Custom Boards (Optional)

You can add custom boards to this repository without modifying the Zephyr upstream tree.

### BOARD_ROOT

Set **`BOARD_ROOT`** in **`CMakeLists.txt`** **before `find_package(Zephyr)`**, pointing to the directory containing board definition files. This repository **comments out** the **`BOARD_ROOT`** lines by default (not needed when using Zephyr's built-in boards like `nucleo_*`):

```cmake
# Only uncomment when placing custom boards under boards/:
# list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/boards)
```

After uncommenting, you can place custom boards under **`boards/<vendor>/<board_name>/`**, with structure consistent with Zephyr documentation **[Custom Board](https://docs.zephyrproject.org/latest/hardware/porting/board_porting.html)**, usually containing:

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

Build example:

```bash
west build -b vendor/board_name .
```

### Devicetree Overlay

Overlays automatically merged by Zephyr must have **`.overlay`** suffix, and are resolved by **`configuration_files.cmake`** based on directory and board name (see **[Device Tree and Memory Configuration Guide.md](../40-app-development/44-devicetree-memory-config.md)** for details). Common usage:

| File | Description |
|------|-------------|
| Application root **`app.overlay`** | Fallback when no board-specific overlay is found; this repository uses it to extend SRAM, etc. |
| **`boards/<board>.overlay`** | Only participates when building that `board` |
| **`boards/<board>_<suffix>.overlay`** | Used together with **`-DFILE_SUFFIX=<suffix>`** |

Note: **`boards/overlay.dts`** and other **non `.overlay`** files are **not** automatically treated as overlays by the above logic, unless explicitly specified via **`DTC_OVERLAY_FILE`**.

Example:

```bash
west build -b nucleo_l4r5zi . -DFILE_SUFFIX=mouse
```

## Custom SoC (Optional)

If custom SoC descriptions are needed, set **`SOC_ROOT`** **before `find_package(Zephyr)`**:

```cmake
list(APPEND SOC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/soc)
```

Directory structure can reference Zephyr documentation's SoC porting guide, for example:

```
soc/
└── vendor/
    └── soc_family/
        ├── Kconfig
        ├── Kconfig.soc
        └── Kconfig.defconfig
```

## Custom DTS / Bindings (Optional)

If additional device tree or binding search paths are needed, set **`DTS_ROOT`**:

```cmake
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/dts)
```

Typical directories can contain `dts/include`, `dts/bindings`, etc., consistent with Zephyr documentation.

## West and Manifest Files

This repository can work with or without **`west.yml`**; the **freestanding application** common practice is to just set **`ZEPHYR_BASE`** before building.

### Method 1: Without west.yml (Common)

After using scripts or manually setting environment variables:

```bash
# Linux / macOS
source scripts/setup_env.sh

# Windows: use scripts/setup_env.bat or set manually

west build -b <board> .
```

### Method 2: Using west.yml

If initializing an independent west workspace in this application directory:

```bash
west init -l .
west update
west build -b <board> .
```

Follow your team's workflow.

## Troubleshooting

### Prompt: `ZEPHYR_BASE` Is Not Set

```text
FATAL_ERROR: ZEPHYR_BASE not set and zephyr_config.env not found.
```

**Resolution**:

1. Copy **`zephyr_config.env.template`** to **`zephyr_config.env`**, fill in your machine's **`ZEPHYR_BASE`**, **`ZEPHYR_SDK_INSTALL_DIR`**.
2. Or manually **`export` / `$env:`** set **`ZEPHYR_BASE`** in your environment.

### Board not found

```text
error: Board 'vendor/board_name' not found
```

**Resolution**:

1. Confirm **`boards/vendor/board_name/`** exists and structure is complete.
2. If using a custom board, confirm **`CMakeLists.txt`** has **`list(APPEND BOARD_ROOT ...)`** pointing to the correct directory.
3. Check that **`board_name_defconfig`**, **`board_name.dts`**, **`board_name.yaml`**, etc. are all present.

### Build directory and board type mismatch

```text
error: Build directory is not configured for this board
```

**Resolution**:

```bash
west build -t pristine
west build -b <board> .
```

## Reference Links

- [Zephyr Application Development](https://docs.zephyrproject.org/latest/application/index.html)
- [Freestanding Applications](https://docs.zephyrproject.org/latest/application/index.html#zephyr-freestanding-app)
- [Custom Board Definitions](https://docs.zephyrproject.org/latest/application/index.html#custom-board-definition)
- [Important Build Variables](https://docs.zephyrproject.org/latest/application/index.html#important-build-vars)
- This repository: **[Device Tree and Memory Configuration Guide.md](../40-app-development/44-devicetree-memory-config.md)**
