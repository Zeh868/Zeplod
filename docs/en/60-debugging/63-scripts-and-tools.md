> Language: [中文](../../zh-CN/60-调试与排错/63-脚本与工具说明.md) | **English**

# Scripts and Tools Guide

This page explains the purpose and usage of scripts under `scripts/`.

Scripts use **`project_layout.ps1` / `project_layout.sh` / `project_layout.py`** to detect two layouts:

| Layout | When | Script path | Work root |
|---|---|---|---|
| **framework** | This repo is the framework itself (e.g. zeplod) | `scripts/` | repo root |
| **app** | Parent `CMakeLists.txt` has `add_subdirectory(framework)` | `framework/scripts/` | APP repo root |

`zephyr_config.env` always lives under **framework** (`framework/zephyr_config.env` in APP repos). Optional **`zephyr_app.env`** at the APP root overrides `APP_PRJ_CONF`, `QEMU_CONF`, etc. (see `zephyr_app.env.template`).

## 1. Script Groups

| Group | Scripts |
|---|---|
| Layout resolver (internal) | `project_layout.ps1` / `project_layout.sh` / `project_layout.py` |
| Environment setup | `setup_env.ps1` / `setup_env.sh` / `setup_env.bat` |
| QEMU simulation | `run_qemu.ps1` |
| Host tests | `run_tests.ps1` / `run_tests.sh` |
| Host sanitizers | `run_sanitizers.ps1` / `run_sanitizers.sh` |
| Twister | `run_twister.ps1` / `run_twister.sh` |
| Unified QA entrypoint | `qa.ps1` / `qa.sh` |
| Preflight | `preflight_host_tests.py` |
| Docs and checks | `generate_docs.ps1` / `generate_docs.sh` / `lint_docs.py` / `check_encoding.py` / `check_script_docs.py` |
| Version and release | `bump_version.py` / `package_release.ps1` / `package_release.sh` |
| Build analysis | `build_all.ps1` / `build_all.bat` / `build_all.sh` / `analyze_map.ps1` / `analyze_map.sh` / `analyze_map.bat` |
| Module/config management | `module_config.py` |

## 2. Prerequisites

1. Prepare `zephyr_config.env` from `zephyr_config.env.template` (under **`framework/`** in APP repos).
2. Install west/CMake/Python/Zephyr SDK.
3. For `native_sim/native_posix`, run on Linux/WSL (native Windows is blocked by script checks).

### 2.1 Command prefix by layout

**Framework repo (zeplod)**:

```powershell
.\scripts\setup_env.ps1
.\scripts\run_qemu.ps1
```

**APP on framework (e.g. zephyr_gateway)**:

```powershell
.\framework\scripts\setup_env.ps1
.\framework\scripts\run_qemu.ps1
```

Examples below use `scripts/`; replace with `framework/scripts/` in APP repos.

## 3. Usage

### 3.1 Environment setup

```powershell
.\scripts\setup_env.ps1
```

```bash
source scripts/setup_env.sh
```

### 3.2 QEMU simulation (Windows)

```powershell
.\scripts\run_qemu.ps1
.\scripts\run_qemu.ps1 -Board qemu_riscv32 -ListBoards
.\scripts\run_qemu.ps1 -Board "qemu_riscv32/qemu_virt_riscv32/smp"
.\scripts\run_qemu.ps1 -Target framework   # APP repo: build framework/ only
```

`-Board` accepts any Zephyr QEMU board name (including SMP qualifiers). In APP mode, `CONF_FILE` merges `framework/prj.conf`, `*_prj.conf`, and `framework/conf/targets/qemu.conf` unless `ZEPHYR_QEMU_CONF` is set. See **[14-qemu-simulation-guide.md](../10-environment-build/14-qemu-simulation-guide.md)** §5 for SMP boards.

Requires **`QEMU_BIN_PATH`** in **`zephyr_config.env`**. See **[14-qemu-simulation-guide.md](../10-environment-build/14-qemu-simulation-guide.md)**.

### 3.3 Host tests

```powershell
.\scripts\run_tests.ps1
```

```bash
./scripts/run_tests.sh
```

Env overrides:
- `ZEPHYR_TEST_BOARD`
- `ZEPHYR_TEST_CONF`
- `ZEPHYR_TEST_BUILD_DIR`

### 3.4 Sanitizers

```powershell
.\scripts\run_sanitizers.ps1
```

```bash
./scripts/run_sanitizers.sh
```

Env overrides:
- `ZEPHYR_SANITIZER=asan|ubsan|asan-ubsan`
- `ZEPHYR_TEST_BOARD`
- `ZEPHYR_TEST_CONF`
- `ZEPHYR_SAN_BUILD_DIR`

### 3.5 Twister

```powershell
.\scripts\run_twister.ps1 -Platform native_sim
```

```bash
./scripts/run_twister.sh
```

Env overrides:
- `ZEPHYR_TWISTER_PLATFORM`
- `ZEPHYR_TWISTER_OUT_DIR`

### 3.6 Preflight

```bash
python scripts/preflight_host_tests.py
```

### 3.7 Unified QA entrypoint

```powershell
.\scripts\qa.ps1 -Mode all
```

```bash
./scripts/qa.sh all
```

Modes: `test`, `san`, `twister`, `all`

### 3.8 Encoding and docs checks

```bash
python scripts/check_encoding.py
python scripts/check_script_docs.py
```

## 4. Platform Notes

- `native_sim/native_posix` are POSIX-host targets.
- On Windows, use **QEMU** for the main app (`run_qemu.ps1`) or unit tests (`ZEPHYR_TEST_BOARD=qemu_riscv32` + `run_tests.ps1` — see [14-qemu-simulation-guide.md](../10-environment-build/14-qemu-simulation-guide.md) §6), or WSL for host POSIX tests.
- CI includes preflight, encoding checks, coverage gates, sanitizers, and twister.

## 5. Optional APP manifest (`zephyr_app.env`)

See **[15-creating-new-app-guide.md](../10-environment-build/15-creating-new-app-guide.md)** for the full APP repo bootstrap guide.

```bash
APP_PRJ_CONF=gateway_prj.conf
QEMU_CONF=framework/prj.conf;gateway_prj.conf;framework/conf/targets/qemu.conf
```

## 6. Advanced environment variables

| Variable | Purpose |
|---|---|
| `ZEPHYR_PROJECT_TARGET` | `auto` / `framework` / `app` |
| `ZEPHYR_APP_ROOT` | Force APP root path |
| `ZEPHYR_QEMU_CONF` | Override QEMU `CONF_FILE` fragments |
