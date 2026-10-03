> Language: [中文](../../zh-CN/10-环境与构建/14-QEMU仿真运行指南.md) | **English**

# QEMU Simulation Guide

This guide explains how to **build and run** this project on Zephyr **QEMU boards** on **Windows / Linux / macOS** without physical hardware. Use it to verify boot flow, logging, and module initialization quickly.

> **vs. `native_sim`**: `native_sim` / `native_posix` are Linux/WSL2 host paths and
> **do not work on native Windows or macOS** (see [51-unit-testing-ci.md](../50-testing-ci/51-unit-testing-ci.md)).
> **QEMU** emulates a real CPU architecture and works on Windows, macOS, and Linux when installed.

---

## 1. Prerequisites

1. Complete [11-environment-setup.md](11-environment-setup.md) (Zephyr SDK, West, `zephyr_config.env`).
2. Install **QEMU** with `qemu-system-*` binaries available.
3. Set **`QEMU_BIN_PATH`** in **`zephyr_config.env`** — CMake resolves QEMU at **configure** time; PATH alone is not enough.

### 1.1 Install QEMU

**Windows (winget)**:

```powershell
winget install SoftwareFreedomConservancy.QEMU
```

Example install path: `C:\Program Files\qemu`.

**Linux** (example):

```bash
sudo apt install qemu-system-arm qemu-system-misc
```

### 1.2 Configure `QEMU_BIN_PATH`

Edit **`zephyr_config.env`** at the repo root:

```bash
QEMU_BIN_PATH=C:/Program Files/qemu
```

`scripts/setup_env.ps1` / `setup_env.sh` exports **`QEMU_BIN_PATH`** and prepends it to PATH.

Verify:

```powershell
. .\scripts\setup_env.ps1
Get-Command qemu-system-riscv32
```

```bash
source scripts/setup_env.sh
which qemu-system-riscv32
```

---

## 2. Quick start (Windows)

Use an **interactive** PowerShell session (QEMU attaches serial to stdio):

**Framework repo**:

```powershell
cd D:\Code\3-Project\zeplod
. .\scripts\setup_env.ps1
.\scripts\run_qemu.ps1
```

**APP repo with `framework/` submodule** (merges APP `*_prj.conf` automatically):

```powershell
cd D:\Code\3-Project\zephyr_gateway
. .\framework\scripts\setup_env.ps1
.\framework\scripts\run_qemu.ps1
```

Options:

```powershell
.\scripts\run_qemu.ps1 -ListBoards
.\scripts\run_qemu.ps1 -Board qemu_riscv32
.\scripts\run_qemu.ps1 -Board qemu_x86_64

# SMP variant (see §5)
.\scripts\run_qemu.ps1 -Board "qemu_riscv32/qemu_virt_riscv32/smp"

.\scripts\run_qemu.ps1 -Board qemu_riscv32 -BuildDir build_qemu
.\scripts\run_qemu.ps1 -Board qemu_riscv32 -BuildOnly
west build -t run --build-dir build_qemu_qemu_riscv32
```

**`-Board`** accepts any Zephyr QEMU board name, including qualified names with `/` (SoC / variant). Each board uses a separate build directory (default `build_qemu_<board>`).

**Exit QEMU**: `Ctrl+A`, then `X`.

See [63-scripts-and-tools.md](../60-debugging/63-scripts-and-tools.md) for script details.

---

## 3. Manual build and run

### 3.1 Recommended boards

| Board | Windows | Notes |
|-------|:-------:|-------|
| **`qemu_riscv32`** | ✅ Recommended | Plenty of RAM; app boot verified |
| **`qemu_riscv64`** | ✅ | RISC-V 64-bit virt |
| **`qemu_x86`** / **`qemu_x86_64`** | ✅ | x86 emulation; **`qemu_x86_64`** is 2-core SMP by default |
| **`qemu_cortex_m3`** / **`qemu_cortex_m0`** | ⚠️ | Builds; app may HardFault on ARM M QEMU — see §8.4 |
| **`qemu_kvm_arm64`** | ❌ | Linux KVM only |

Use overlay config **`conf/targets/qemu.conf`** on top of **`prj.conf`** for simulation (reduced RAM/stacks, watchdog off).

### 3.2 Build

```powershell
. .\scripts\setup_env.ps1
west build -b qemu_riscv32 -d build_qemu . -p always `
  -- "-DCONF_FILE=prj.conf;conf/targets/qemu.conf"
```

```bash
source scripts/setup_env.sh
west build -b qemu_riscv32 -d build_qemu . -p always \
  -- "-DCONF_FILE=prj.conf;conf/targets/qemu.conf"
```

Expect in the log:

```text
Found devicetree overlay: .../boards/qemu_riscv32.overlay
```

### 3.3 Run

```bash
west build -t run --build-dir build_qemu
```

### 3.4 Other QEMU boards

Change `-b` and use a separate build directory, e.g.:

```bash
west build -b qemu_cortex_m3 -d build_qemu_m3 . -p always \
  -- "-DCONF_FILE=prj.conf;conf/targets/qemu.conf"
west build -t run --build-dir build_qemu_m3
```

**`boards/qemu_<board>.overlay`** is merged automatically when the board name matches.

---

## 4. Config files

| File | Role |
|------|------|
| **`prj.conf`** | Main Kconfig |
| **`conf/targets/qemu.conf`** | QEMU overlay: smaller heaps/stacks, KV off, single example module, watchdog off |
| **`boards/qemu_*.overlay`** | Board Devicetree overlays (UART, SRAM on some ARM M boards) |

---

## 5. Multi-core (SMP) simulation

For **dual-core SMP** smoke tests, use Zephyr **SMP board variants** (board defconfig sets `CONFIG_SMP=y`, `CONFIG_MP_MAX_NUM_CPUS=2`). This project's `prj.conf` / `conf/targets/qemu.conf` do **not** disable SMP, so these boards enable multi-core automatically.

### 5.1 SMP boards (Windows)

| `west build -b` | Arch | CPUs | Notes |
|-----------------|------|:----:|-------|
| **`qemu_riscv32/qemu_virt_riscv32/smp`** | RISC-V 32 | 2 | **Recommended**; same family as single-core `qemu_riscv32` |
| **`qemu_riscv64/qemu_virt_riscv64/smp`** | RISC-V 64 | 2 | 64-bit virt SMP |
| **`qemu_cortex_a53/qemu_cortex_a53/smp`** | ARM Cortex-A53 | 2 | ARM64 application-class SMP |
| **`qemu_x86_64`** | x86 64 | 2 | SMP enabled in default defconfig (no `/smp` suffix) |

**Single-core only** (not for SMP): `qemu_cortex_m3`, `qemu_cortex_m0`, non-smp `qemu_cortex_a9`, etc.

`qemu_kvm_arm64` can be multi-core but requires Linux KVM — **not on Windows**.

### 5.2 Script

```powershell
.\scripts\run_qemu.ps1 -Board "qemu_riscv32/qemu_virt_riscv32/smp"
.\scripts\run_qemu.ps1 -Board qemu_x86_64
```

### 5.3 Manual build and run

```powershell
west build -b qemu_riscv32/qemu_virt_riscv32/smp -d build_qemu_smp . -p always `
  -- "-DCONF_FILE=prj.conf;conf/targets/qemu.conf"
west build -t run --build-dir build_qemu_smp
```

Expect:

```text
Board: qemu_riscv32, qualifiers: qemu_virt_riscv32/smp
```

SMP variants need **fully qualified overlay filenames** (Zephyr does not fall back to `qemu_riscv32.overlay`):

| Board | Overlay file |
|-------|----------------|
| `qemu_riscv32/qemu_virt_riscv32/smp` | `boards/qemu_riscv32_qemu_virt_riscv32_smp.overlay` |
| `qemu_riscv64/qemu_virt_riscv64/smp` | `boards/qemu_riscv64_qemu_virt_riscv64_smp.overlay` |
| `qemu_cortex_a53/qemu_cortex_a53/smp` | `boards/qemu_cortex_a53_qemu_cortex_a53_smp.overlay` |

### 5.4 Verification tips

1. **Link smoke** — SMP increases kernel/stack usage; confirm a full link.
2. **Boot log** — version banner and `Application ready` on the console.
3. **Multi-core behavior** (optional) — shell thread list, per-CPU logging, later ztest for locks/IPC/events under SMP.

SMP board defconfigs usually disable `CONFIG_QEMU_ICOUNT` (incompatible with SMP timing). Boot speed may differ from single-core `qemu_riscv32`.

---

## 6. Unit tests (ztest) on QEMU

Native Windows and macOS cannot run `native_sim` / `native_posix`, but they can build and run the **`tests/`** ztest suite on a QEMU board via **`ZEPHYR_TEST_BOARD`**. Linux can use the same QEMU approach.

> Test layout, coverage, and authoring rules: **[51-unit-testing-ci.md](../50-testing-ci/51-unit-testing-ci.md)** and **`tests/README.md`**.

### 6.1 Prerequisites

Same as §1: **`QEMU_BIN_PATH`** configured, and an **interactive** terminal (QEMU serial on stdio).

### 6.2 Script (recommended)

```powershell
. .\scripts\setup_env.ps1

# Default: prj.conf;prj_test_extensions.conf (P0/P1 extensions + concurrency stress)
$env:ZEPHYR_TEST_BOARD = 'qemu_riscv32'
$env:ZEPHYR_TEST_BUILD_DIR = 'build_tests_qemu_riscv32'
.\scripts\run_tests.ps1
```

Variants:

```powershell
# IPC smoke (prj.conf;prj_native_sim.conf)
$env:ZEPHYR_TEST_BOARD = 'qemu_riscv32'
$env:ZEPHYR_TEST_BUILD_DIR = 'build_tests_ipc_qemu'
.\scripts\run_tests_ipc.ps1

# Full matrix (+ example modules A/B/GPIO/Multi)
$env:ZEPHYR_TEST_BOARD = 'qemu_riscv32'
$env:ZEPHYR_TEST_BUILD_DIR = 'build_tests_full_qemu'
.\scripts\run_tests_full.ps1

# Other QEMU board
$env:ZEPHYR_TEST_BOARD = 'qemu_x86_64'
$env:ZEPHYR_TEST_BUILD_DIR = 'build_tests_qemu_x86_64'
.\scripts\run_tests.ps1
```

> **Note**: Without **`ZEPHYR_TEST_BOARD`**, `run_tests.ps1` defaults to `native_sim` and fails on native Windows. Sanitizers (`run_sanitizers.ps1`) and Twister still require host POSIX boards only.

**Linux / macOS / WSL**:

```bash
source scripts/setup_env.sh

ZEPHYR_TEST_BOARD=qemu_riscv32 ZEPHYR_TEST_BUILD_DIR=build_tests_qemu \
  ./scripts/run_tests.sh

ZEPHYR_TEST_BOARD=qemu_riscv32 ZEPHYR_TEST_BUILD_DIR=build_tests_ipc_qemu \
  ./scripts/run_tests_ipc.sh
```

### 6.3 Manual West commands

```powershell
. .\scripts\setup_env.ps1

west build -b qemu_riscv32 tests/ --build-dir build_tests_qemu -p always `
  -- "-DCONF_FILE=prj.conf;prj_test_extensions.conf"
west build -t run --build-dir build_tests_qemu
```

```bash
source scripts/setup_env.sh

west build -b qemu_riscv32 tests/ --build-dir build_tests_qemu -p always \
  -- "-DCONF_FILE=prj.conf;prj_test_extensions.conf"
west build -t run --build-dir build_tests_qemu
```

### 6.4 Test config matrix

| CONF_FILE (under `tests/`) | Script | Purpose |
|----------------------------|--------|---------|
| `prj.conf;prj_test_extensions.conf` | `run_tests.ps1` / `run_tests.sh` | Default; P0/P1 extensions + concurrency stress |
| `prj.conf;prj_native_sim.conf` | `run_tests_ipc.ps1` / `run_tests_ipc.sh` | IPC + larger heap |
| `prj.conf;prj_native_sim.conf;prj_ci_examples.conf` | `run_tests_full.ps1` / `run_tests_full.sh` | + example modules |
| `prj.conf` | manual `-DCONF_FILE` | Minimal smoke (no extension overlay) |

Tests use **`tests/prj.conf`**, not the main app's **`conf/targets/qemu.conf`**. `tests/prj.conf` is already trimmed for simulation (smaller heaps, watchdog off).

### 6.5 vs. main-app QEMU simulation

| Item | Main app (`run_qemu.ps1`) | Unit tests (`run_tests.ps1` + QEMU board) |
|------|---------------------------|-------------------------------------------|
| Source dir | repo root `.` | `tests/` |
| Default CONF | `prj.conf;conf/targets/qemu.conf` | `tests/prj.conf` + overlays |
| Entry | `app_main` | ztest |
| Devicetree overlay | root `boards/qemu_*.overlay` auto-merged | app root is `tests/` — root overlays **not** auto-merged; default `qemu_riscv32` RAM is usually enough |

### 6.6 Expected ztest output (excerpt)

On **`qemu_riscv32`**:

```text
*** Booting Zephyr OS build ...
Running TESTSUITE event_system
...
PROJECT EXECUTION SUCCESSFUL
```

### 6.7 SMP and board choice

- **Recommended**: **`qemu_riscv32`** (see §3.1).
- **SMP experiment**: `ZEPHYR_TEST_BOARD=qemu_riscv32/qemu_virt_riscv32/smp` (see §5).
- **Avoid**: `qemu_cortex_m3` and other ARM M QEMU boards (see §8.4).

---

## 7. Main-app expected output (excerpt)

On **`qemu_riscv32`**:

```text
[00:00:00.000,000] <inf> example_module_a: Initializing Example Module A...
  Version:       1.0.0
  Target:        qemu_riscv32
[00:00:00.020,000] <inf> app_main: Application ready (modules=1)
```

---

## 8. Troubleshooting

### 8.1 `QEMU-NOTFOUND`

Set **`QEMU_BIN_PATH`** in **`zephyr_config.env`**, run **`setup_env`**, then rebuild with **`-p always`**.

### 8.2 No serial output after `Starting QEMU...`

**Symptom**: Build succeeds but the terminal shows no Zephyr boot log.

**Cause**: An older `run_qemu.ps1` buffered all `west build -t run` output via `2>&1`; QEMU is a long-running foreground process, so logs only appeared after QEMU exited.

**Fix**: Use the updated `run_qemu.ps1` (streams `west` directly). If output is still missing, run in an **interactive** terminal — not a background job or redirected stdio.

### 8.3 `west build -t run` fails in CI / background

Run in a real terminal with stdio attached.

### 8.4 HardFault on `qemu_cortex_m3`

Upstream `hello_world` works; this app has known issues on ARM Cortex-M QEMU. Prefer **`qemu_riscv32`** on Windows.

### 8.5 `undefined reference to sys_reboot` with `conf/profiles/balanced.conf`

Use **`conf/targets/qemu.conf`** for QEMU (`CONFIG_SYS_WATCHDOG_ENABLE=n`).

### 8.6 SMP configure fails: `app.overlay` merged

If the log shows `Found devicetree overlay: .../app.overlay` then configure errors, the SMP qualifier did not match a board overlay and Zephyr fell back to **`app.overlay`** (STM32L4 SRAM — invalid on QEMU). Use the SMP overlay files listed in §5.3.

### 8.7 Wrong SMP board name

Use the full qualifier, e.g. `qemu_riscv32/qemu_virt_riscv32/smp` — not `qemu_riscv32_smp`.

### 8.8 `qemu_kvm_arm64`

Requires Linux **KVM**; not available on Windows.

### 8.9 `run_tests.ps1` fails with POSIX board on Windows

**Symptom**: Error that `native_sim` / `native_posix` cannot run on Windows when **`ZEPHYR_TEST_BOARD`** is unset.

**Fix**: Set `$env:ZEPHYR_TEST_BOARD = 'qemu_riscv32'` (or another QEMU board) per §6.2, then run `run_tests.ps1`. Or use WSL for host tests.

### 8.10 Unit tests on QEMU: no `PROJECT EXECUTION SUCCESSFUL`

**Symptom**: QEMU boots but ztest does not finish, or link fails.

**Fix**:

1. Run `west build -t run` in an interactive terminal (§8.3).
2. If RAM is tight, use `prj.conf` only (drop `prj_test_extensions.conf` concurrency stress).
3. Ensure **`QEMU_BIN_PATH`** is set and reconfigure with `-p always`.

### 8.11 Garbled Chinese log text on Windows

**Symptom**: CJK characters in serial logs look like mojibake; ASCII is fine.

**Cause**: Firmware `LOG_*` / `printk` strings are **UTF-8**. On Chinese Windows, PowerShell often defaults to **GBK (CP936)** while QEMU binds the UART to stdio.

**Fix**:

1. Use an updated **`run_qemu.ps1`** (sets `chcp 65001` and UTF-8 console encoding before launch).
2. For manual `west build -t run`, run first:

```powershell
chcp 65001
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = [System.Text.UTF8Encoding]::new($false)
```

3. Prefer **Windows Terminal** or **PowerShell 7+**; optional OS setting: enable **“Beta: Use Unicode UTF-8 for worldwide language support”**.
4. Set **`ZEPHYR_CONSOLE_UTF8=0`** to skip the script’s UTF-8 switch.

---

## 9. Related docs

- [51-unit-testing-ci.md](../50-testing-ci/51-unit-testing-ci.md) — ztest coverage and CI matrix

- [12-freestanding-app-build.md](12-freestanding-app-build.md)
- [15-creating-new-app-guide.md](15-creating-new-app-guide.md)
- [44-devicetree-memory-config.md](../40-app-development/44-devicetree-memory-config.md)
- [53-hardware-testing.md](../50-testing-ci/53-hardware-testing.md)
- [63-scripts-and-tools.md](../60-debugging/63-scripts-and-tools.md)
