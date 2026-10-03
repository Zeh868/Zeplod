> Language: [中文](../../zh-CN/10-环境与构建/15-新建APP开发指南.md) | **English**

# Creating a New APP Repository

This guide explains how to create a **standalone business APP repository** on top of **Zeplod (framework)**: layout, required files, Kconfig / devicetree / script configuration, and differences between hardware boards and QEMU simulation.

> **Relation to [12-freestanding-app-build.md](12-freestanding-app-build.md)**  
> Doc 12 covers Zephyr freestanding concepts (`ZEPHYR_BASE`, `BOARD_ROOT`, overlay rules).  
> **This guide focuses on the recommended `framework/` submodule + APP layout**, step by step.  
> Reference implementation: [zephyr_gateway](https://github.com/QZehao/zephyr_gateway).

---

## 1. Two project shapes

| Shape | Use case | Build entry | Scripts |
|-------|----------|-------------|---------|
| **Framework repo** (zeplod) | Framework maintenance, examples, framework tests | `west build -b <board> .` at repo root | `scripts/` |
| **APP repo** (recommended for products) | Product firmware, own versioning/CI, framework as submodule | `west build -b <board> .` at APP root | `framework/scripts/` |

Typical APP layout:

```
my_product/
├── CMakeLists.txt
├── my_product_prj.conf
├── my_product_conf/targets/qemu.conf    # optional but recommended
├── src/
├── modules/my_product/
└── framework/                  # zeplod submodule
```

**Prerequisite**: complete [11-environment-setup.md](11-environment-setup.md) and verify zeplod builds on your machine.

---

## 2. Step-by-step checklist

### 2.1 Create repo and add framework submodule

```bash
mkdir my_product && cd my_product
git init
git submodule add <zeplod-repo-url> framework
git submodule update --init --recursive
```

### 2.2 Configure Zephyr (`framework/zephyr_config.env`)

```powershell
copy framework\zephyr_config.env.template framework\zephyr_config.env
```

Set `ZEPHYR_BASE`, `ZEPHYR_SDK_INSTALL_DIR`, `QEMU_BIN_PATH`. Do **not** commit this file.

### 2.3 Top-level `CMakeLists.txt`

Responsibilities: load env, merge `CONF_FILE`, handle overlays, register out-of-tree modules, `find_package(Zephyr)`, `add_subdirectory(framework)`.

Minimal template (replace `my_product`):

```cmake
cmake_minimum_required(VERSION 3.20.0)

if(NOT CONF_FILE)
  set(CONF_FILE
      "${CMAKE_CURRENT_LIST_DIR}/framework/prj.conf;${CMAKE_CURRENT_LIST_DIR}/my_product_prj.conf"
      CACHE STRING "Zephyr application config files" FORCE)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/framework/cmake/zeplod_app_overlays.cmake)
zeplod_append_app_devicetree_overlays("${CMAKE_CURRENT_LIST_DIR}")

if(NOT DEFINED ENV{ZEPHYR_EXTRA_MODULES} OR "$ENV{ZEPHYR_EXTRA_MODULES}" STREQUAL "")
  set(ENV{ZEPHYR_EXTRA_MODULES} "${CMAKE_CURRENT_LIST_DIR}/modules/my_product")
endif()

# Load framework/zephyr_config.env when ZEPHYR_BASE unset (same pattern as zephyr_gateway)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_product VERSION 1.0.0)

set(MY_PRODUCT_TOPLEVEL_BOOTSTRAP ON)
add_subdirectory(framework)
```

**Important**

- Call **`find_package(Zephyr)`** at the APP root (required on Windows).
- Use **`zeplod_append_app_devicetree_overlays()`** — do not unconditionally append `framework/app.overlay` (breaks QEMU with `sram0` errors).

### 2.4 `my_product_prj.conf`

Business Kconfig on top of `framework/prj.conf`:

```kconfig
CONFIG_MY_PRODUCT_BUSINESS=y
CONFIG_EXAMPLE_MODULE_A_ENABLE=n
```

### 2.5 `my_product_conf/targets/qemu.conf` (strongly recommended)

Disable hardware features QEMU lacks (network, CAN, flash, NVS, etc.).

QEMU merge order (auto by scripts):

```text
framework/prj.conf → <app>_prj.conf → <app>_conf/targets/qemu.conf → framework/conf/targets/qemu.conf
```

See [14-qemu-simulation-guide.md](14-qemu-simulation-guide.md).

### 2.6 Out-of-tree module `modules/my_product/`

`zephyr/module.yml`:

```yaml
name: my_product
build:
  cmake: .
  kconfig: Kconfig
```

Source files live under APP `src/`; the module CMakeLists references them (same pattern as zephyr_gateway).

### 2.7 Optional `zephyr_app.env`

Copy `zephyr_app.env.template` from the framework repo to the APP root to override script discovery:

```bash
APP_PRJ_CONF=my_product_prj.conf
APP_PRJ_QEMU_CONF=my_product_conf/targets/qemu.conf
QEMU_CONF=framework/prj.conf;my_product_prj.conf;my_product_conf/targets/qemu.conf;framework/conf/targets/qemu.conf
```

### 2.8 First build

```powershell
.\framework\scripts\setup_env.ps1
.\framework\scripts\run_qemu.ps1 -BuildOnly -Board qemu_riscv32
.\framework\scripts\run_qemu.ps1
```

Hardware:

```powershell
west build -b nucleo_l4r5zi -d build . -p always
```

---

## 3. Configuration reference

| File | Location | Role |
|------|----------|------|
| `zephyr_config.env` | `framework/` | Zephyr SDK paths, QEMU, venv |
| `zephyr_app.env` | APP root | Optional script/CONF overrides |
| `framework/prj.conf` | framework | Framework defaults |
| `<app>_prj.conf` | APP root | Business Kconfig |
| `<app>_conf/targets/qemu.conf` | APP root | QEMU trim overlay |
| `framework/conf/targets/qemu.conf` | framework | Framework QEMU trim |
| `APP_VERSION` | APP root | App semver (overrides framework) |

---

## 4. Scripts and layout detection

`framework/scripts/project_layout.*` detects **app mode** when the APP root `CMakeLists.txt` contains `add_subdirectory(framework)`.

| Feature | framework mode | app mode |
|---------|----------------|----------|
| Config file | `./zephyr_config.env` | `./framework/zephyr_config.env` |
| QEMU CONF | `prj.conf;conf/targets/qemu.conf` | Auto-merge `framework/prj.conf` + `*_prj.conf` + `*_conf/targets/qemu.conf` |
| QEMU script | `.\scripts\run_qemu.ps1` | `.\framework\scripts\run_qemu.ps1` |

Details: [63-scripts-and-tools.md](../60-debugging/63-scripts-and-tools.md).

---

## 5. Devicetree notes

1. `framework/app.overlay` targets default hardware boards — **not** `qemu_*` / `native_*`.
2. `zeplod_append_app_devicetree_overlays()` picks `framework/boards/<board>.overlay` for QEMU.
3. Board migration: [13-board-porting-guide.md](13-board-porting-guide.md).

---

## 6. Acceptance checklist

- [ ] `framework/` submodule initialized and pinned  
- [ ] `framework/zephyr_config.env` configured, not committed  
- [ ] Top `CMakeLists.txt` with overlays helper + `add_subdirectory(framework)`  
- [ ] `<app>_prj.conf` and `<app>_conf/targets/qemu.conf` present  
- [ ] `modules/<name>/` + `ZEPHYR_EXTRA_MODULES` aligned  
- [ ] `west build -b qemu_riscv32 .` passes  
- [ ] Target board build passes  
- [ ] CI board/CONF matches local  

---

## 7. Troubleshooting

| Symptom | Fix |
|---------|-----|
| `undefined node label 'sram0'` | Use `zeplod_app_overlays.cmake`; update framework submodule |
| QEMU still builds networking | Add `*_conf/targets/qemu.conf`; update `project_layout` scripts |
| Garbled Chinese on Windows | Updated `run_qemu.ps1` UTF-8 setup — [14-qemu §8.11](14-qemu-simulation-guide.md) |
| Scripts use framework mode | Fix `add_subdirectory(framework)` in top CMakeLists |

More: [62-troubleshooting.md](../60-debugging/62-troubleshooting.md).

---

## 8. Related docs

- [12-freestanding-app-build.md](12-freestanding-app-build.md)  
- [14-qemu-simulation-guide.md](14-qemu-simulation-guide.md)  
- Example: **zephyr_gateway**
