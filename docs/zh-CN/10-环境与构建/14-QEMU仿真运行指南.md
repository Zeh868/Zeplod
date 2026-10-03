> 语言: **中文** | [English](../../en/10-environment-build/14-qemu-simulation-guide.md)

> **本文档范围**：QEMU 编译运行、板型选择、`run_qemu.ps1`、SMP、ztest 在 QEMU 上执行。
> **另见**：[63-脚本与工具说明](../60-调试与排错/63-脚本与工具说明.md)（脚本参数表）· [11-环境搭建与配置指南](11-环境搭建与配置指南.md)（`QEMU_BIN_PATH`）· [51-单元测试与持续集成说明](../50-测试与CI/51-单元测试与持续集成说明.md)（CI 概览）

# QEMU 仿真运行指南

本文说明如何在 **Windows / Linux / macOS** 上使用 Zephyr 自带的 **QEMU 板型** 编译并运行本工程，无需真实开发板。适用于快速验证启动流程、日志、模块初始化等逻辑。

> **与 `native_sim` 的区别**：`native_sim` / `native_posix` 是 Linux/WSL2 主机路径，
> **Windows 原生和 macOS 不支持**（见 [51-单元测试与持续集成说明.md](../50-测试与CI/51-单元测试与持续集成说明.md)）。
> **QEMU 仿真**模拟真实 CPU 架构，可用于 Windows、macOS 与 Linux（需安装 QEMU）。

---

## 1. 前提条件

1. 已完成 [11-环境搭建与配置指南.md](11-环境搭建与配置指南.md) 中的 Zephyr SDK、West、`zephyr_config.env` 配置。
2. 已安装 **QEMU**（本机需包含 `qemu-system-*` 可执行文件）。
3. 在 **`zephyr_config.env`** 中配置 **`QEMU_BIN_PATH`**（CMake 在**配置阶段**解析 QEMU 路径，仅加入系统 PATH 不够）。

### 1.1 安装 QEMU

**Windows（推荐 winget）**：

```powershell
winget install SoftwareFreedomConservancy.QEMU
```

默认安装路径示例：`C:\Program Files\qemu`。

**Linux**（示例）：

```bash
sudo apt install qemu-system-arm qemu-system-misc
```

### 1.2 配置 `QEMU_BIN_PATH`

编辑仓库根目录 **`zephyr_config.env`**：

```bash
QEMU_BIN_PATH=C:/Program Files/qemu
```

加载环境后，`scripts/setup_env.ps1` / `setup_env.sh` 会将其写入 **`$env:QEMU_BIN_PATH`** 并追加到 PATH。

验证：

```powershell
# Windows
. .\scripts\setup_env.ps1
Get-Command qemu-system-riscv32
```

```bash
# Linux / macOS
source scripts/setup_env.sh
which qemu-system-riscv32
```

---

## 2. 最快上手（Windows）

在 **可交互的 PowerShell 终端** 中执行（QEMU 串口绑定到 stdio，自动化终端可能无法附着）：

**框架本体仓库**：

```powershell
cd D:\Code\3-Project\zeplod
. .\scripts\setup_env.ps1
.\scripts\run_qemu.ps1
```

**基于 framework 的 APP 仓库**（脚本在子模块内，自动合并 APP 的 `*_prj.conf`）：

```powershell
cd D:\Code\3-Project\zephyr_gateway
. .\framework\scripts\setup_env.ps1
.\framework\scripts\run_qemu.ps1
```

默认板型 `qemu_riscv32`；framework 模式使用 `prj.conf;conf/targets/qemu.conf`，APP 模式使用 `framework/prj.conf;<app>_prj.conf;framework/conf/targets/qemu.conf`（可由 `zephyr_app.env` 覆盖）。

常用参数：

```powershell
# 列出脚本内置的 QEMU 板型（含 SMP 变体提示）
.\scripts\run_qemu.ps1 -ListBoards

# 指定仿真板型（等同 west build -b <板型>）
.\scripts\run_qemu.ps1 -Board qemu_riscv32
.\scripts\run_qemu.ps1 -Board qemu_x86_64

# 指定带 qualifier 的板型（多核 SMP，见 §4）
.\scripts\run_qemu.ps1 -Board "qemu_riscv32/qemu_virt_riscv32/smp"

# 自定义构建目录
.\scripts\run_qemu.ps1 -Board qemu_riscv32 -BuildDir build_qemu

# 仅编译，稍后手动运行
.\scripts\run_qemu.ps1 -Board qemu_riscv32 -BuildOnly
west build -t run --build-dir build_qemu_qemu_riscv32
```

**`-Board`** 接受任意 Zephyr 支持的 QEMU 板名（含 `/` 分隔的 SoC / variant qualifier）。每个板型使用独立构建目录（默认 `build_qemu_<板型>`，特殊字符会替换为 `_`）。

**退出 QEMU**：按 `Ctrl+A`，松开后按 `X`。

脚本说明见 [63-脚本与工具说明.md](../60-调试与排错/63-脚本与工具说明.md)。

---

## 3. 手动编译与运行

### 3.1 推荐板型与配置

| 板型 | Windows 仿真 | 说明 |
|------|:------------:|------|
| **`qemu_riscv32`** | ✅ 推荐 | 内存充裕，本应用已验证可正常启动 |
| **`qemu_riscv64`** | ✅ | RISC-V 64-bit virt |
| **`qemu_x86`** / **`qemu_x86_64`** | ✅ | x86 仿真 |
| **`qemu_cortex_m3`** / **`qemu_cortex_m0`** | ⚠️ | 可编译；当前应用在 ARM M 系 QEMU 上可能 HardFault，见 §8.4 |
| **`qemu_kvm_arm64`** | ❌ | 仅 Linux + KVM，Windows 不可用 |

仿真建议使用叠加配置 **`conf/targets/qemu.conf`**（在 `prj.conf` 基础上削减 RAM/栈，并保持软件看门狗关闭）。

### 3.2 编译

```powershell
# Windows PowerShell
. .\scripts\setup_env.ps1

west build -b qemu_riscv32 -d build_qemu . -p always `
  -- "-DCONF_FILE=prj.conf;conf/targets/qemu.conf"
```

```bash
# Linux / macOS
source scripts/setup_env.sh

west build -b qemu_riscv32 -d build_qemu . -p always \
  -- "-DCONF_FILE=prj.conf;conf/targets/qemu.conf"
```

构建日志中应出现：

```text
Found devicetree overlay: .../boards/qemu_riscv32.overlay
```

### 3.3 运行

```bash
west build -t run --build-dir build_qemu
```

等价于由 Zephyr 调用 `qemu-system-<arch>` 加载 `build_qemu/zephyr/zephyr.elf`，串口输出到当前终端。

### 3.4 切换其他 QEMU 板型

将 `-b` 参数改为对应板名即可，例如：

```bash
west build -b qemu_cortex_m3 -d build_qemu_m3 . -p always \
  -- "-DCONF_FILE=prj.conf;conf/targets/qemu.conf"
west build -t run --build-dir build_qemu_m3
```

仓库 **`boards/qemu_<板型>.overlay`** 会在匹配板名时自动合并（无需手动指定 `DTC_OVERLAY_FILE`）。部分 ARM M 板 overlay 已扩展 SRAM，详见各 overlay 文件注释。

---

## 4. 配置文件说明

| 文件 | 作用 |
|------|------|
| **`prj.conf`** | 主配置（默认功能集） |
| **`conf/targets/qemu.conf`** | QEMU 叠加：减小堆/栈/事件池，关闭 KV，单示例模块，看门狗保持关闭 |
| **`boards/qemu_*.overlay`** | 板级设备树覆盖（UART、部分板 SRAM 扩展等） |

仅做冒烟、不需完整功能时，可叠加 `conf/targets/qemu.conf`；需要与量产配置一致的行为时，可仅用 `prj.conf`（注意小内存 QEMU 板可能链接或运行失败）。

---

## 5. 多核（SMP）仿真验证

在 QEMU 上做 **双核 SMP** 冒烟时，需选用 Zephyr 提供的 **SMP 板型变体**（板级 defconfig 含 `CONFIG_SMP=y`、`CONFIG_MP_MAX_NUM_CPUS=2`）。本工程 `prj.conf` / `conf/targets/qemu.conf` **未禁用 SMP**，使用下列板型时会自动启用多核。

### 5.1 可用 SMP 板型（Windows）

| `west build -b` 板型 | 架构 | CPU 数 | 说明 |
|----------------------|------|:------:|------|
| **`qemu_riscv32/qemu_virt_riscv32/smp`** | RISC-V 32 | 2 | **推荐**：与单核 `qemu_riscv32` 同族，Win11 已验证单核可启动 |
| **`qemu_riscv64/qemu_virt_riscv64/smp`** | RISC-V 64 | 2 | 64 位 virt SMP |
| **`qemu_cortex_a53/qemu_cortex_a53/smp`** | ARM Cortex-A53 | 2 | ARM64 应用级 SMP |
| **`qemu_x86_64`** | x86 64 | 2 | 默认 defconfig 即 SMP（无需 `/smp` 后缀） |

下列板型为 **单核**，不能用于 SMP 验证：`qemu_cortex_m3`、`qemu_cortex_m0`、`qemu_cortex_a9`（非 smp 变体）等。

`qemu_kvm_arm64` 可多核，但依赖 Linux KVM，**Windows 不可用**。

### 5.2 脚本一键运行（推荐）

```powershell
. .\scripts\setup_env.ps1

# RISC-V 32 双核
.\scripts\run_qemu.ps1 -Board "qemu_riscv32/qemu_virt_riscv32/smp"

# x86_64 双核（板名本身即 SMP）
.\scripts\run_qemu.ps1 -Board qemu_x86_64
```

### 5.3 手动编译与运行

```powershell
. .\scripts\setup_env.ps1

west build -b qemu_riscv32/qemu_virt_riscv32/smp -d build_qemu_smp . -p always `
  -- "-DCONF_FILE=prj.conf;conf/targets/qemu.conf"

west build -t run --build-dir build_qemu_smp
```

构建日志中板型应类似：

```text
Board: qemu_riscv32, qualifiers: qemu_virt_riscv32/smp
```

SMP 变体须使用 **带完整 qualifier 的 overlay 文件名**（Zephyr 不会回退到 `qemu_riscv32.overlay`）：

| 板型 | overlay 文件 |
|------|----------------|
| `qemu_riscv32/qemu_virt_riscv32/smp` | `boards/qemu_riscv32_qemu_virt_riscv32_smp.overlay` |
| `qemu_riscv64/qemu_virt_riscv64/smp` | `boards/qemu_riscv64_qemu_virt_riscv64_smp.overlay` |
| `qemu_cortex_a53/qemu_cortex_a53/smp` | `boards/qemu_cortex_a53_qemu_cortex_a53_smp.overlay` |

单核 `qemu_riscv32` 仍使用 `boards/qemu_riscv32.overlay`。

### 5.4 验证建议

1. **编译冒烟**：确认 SMP 板型能完整链接（多核会增大内核与栈占用）。
2. **启动日志**：串口应正常输出版本横幅与 `Application ready`。
3. **多核行为**（按需深入）：
   - Shell：`kernel cycles`、线程列表（若启用 `CONFIG_SHELL`）；
   - 在业务代码中对不同 CPU 打日志（`arch_curr_cpu()->id` 等）；
   - 后续可叠加专用测试配置或 ztest 用例验证锁、IPC、事件系统在 SMP 下的行为。

SMP 板级 defconfig 通常关闭 `CONFIG_QEMU_ICOUNT`（与多核计时仿真不兼容），启动速度可能与单核 `qemu_riscv32` 略有不同，属正常现象。

---

## 6. 单元测试（ztest）QEMU 仿真

Windows 原生和 macOS 无法运行 `native_sim` / `native_posix`，但可通过 **`ZEPHYR_TEST_BOARD`** 将 `tests/` 下的 ztest 用例编译到 QEMU 板型上运行。Linux 也可用同样方式在 QEMU 上跑测试。

> 测试配置、覆盖范围与编写约定见 **[51-单元测试与持续集成说明.md](../50-测试与CI/51-单元测试与持续集成说明.md)** 与 **`tests/README.md`**。

### 6.1 前提

与 §1 相同：已配置 **`QEMU_BIN_PATH`**，并在**交互式终端**中运行（QEMU 串口绑定 stdio）。

### 6.2 脚本一键运行（推荐）

```powershell
. .\scripts\setup_env.ps1

# 默认：prj.conf;prj_test_extensions.conf（P0/P1 扩展 + 并发压测）
$env:ZEPHYR_TEST_BOARD = 'qemu_riscv32'
$env:ZEPHYR_TEST_BUILD_DIR = 'build_tests_qemu_riscv32'
.\scripts\run_tests.ps1
```

常用变体：

```powershell
# IPC 烟测（prj.conf;prj_native_sim.conf）
$env:ZEPHYR_TEST_BOARD = 'qemu_riscv32'
$env:ZEPHYR_TEST_BUILD_DIR = 'build_tests_ipc_qemu'
.\scripts\run_tests_ipc.ps1

# 完整矩阵（+ 示例模块 A/B/GPIO/Multi）
$env:ZEPHYR_TEST_BOARD = 'qemu_riscv32'
$env:ZEPHYR_TEST_BUILD_DIR = 'build_tests_full_qemu'
.\scripts\run_tests_full.ps1

# 其他 QEMU 板型
$env:ZEPHYR_TEST_BOARD = 'qemu_x86_64'
$env:ZEPHYR_TEST_BUILD_DIR = 'build_tests_qemu_x86_64'
.\scripts\run_tests.ps1
```

> **注意**：未设置 `ZEPHYR_TEST_BOARD` 时，`run_tests.ps1` 默认选择 `native_sim`，在 Windows 原生环境会失败。Sanitizer（`run_sanitizers.ps1`）与 Twister 仍仅支持主机 POSIX 板型。

**Linux / macOS / WSL**（同样支持 QEMU 板型）：

```bash
source scripts/setup_env.sh

ZEPHYR_TEST_BOARD=qemu_riscv32 ZEPHYR_TEST_BUILD_DIR=build_tests_qemu \
  ./scripts/run_tests.sh

ZEPHYR_TEST_BOARD=qemu_riscv32 ZEPHYR_TEST_BUILD_DIR=build_tests_ipc_qemu \
  ./scripts/run_tests_ipc.sh
```

### 6.3 手动 West 命令

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

### 6.4 测试配置矩阵

| CONF_FILE（相对 `tests/`） | 脚本 | 说明 |
|----------------------------|------|------|
| `prj.conf;prj_test_extensions.conf` | `run_tests.ps1` / `run_tests.sh` | 默认；P0/P1 扩展 + 并发压测 |
| `prj.conf;prj_native_sim.conf` | `run_tests_ipc.ps1` / `run_tests_ipc.sh` | IPC + 较大堆 |
| `prj.conf;prj_native_sim.conf;prj_ci_examples.conf` | `run_tests_full.ps1` / `run_tests_full.sh` | 再上示例模块 |
| `prj.conf` | 手动 `-DCONF_FILE` | 最小冒烟（无扩展层） |

测试使用 **`tests/prj.conf`**（非主应用根目录的 **`conf/targets/qemu.conf`**）。`tests/prj.conf` 已针对仿真环境精简堆栈并关闭看门狗。

### 6.5 与主应用仿真的区别

| 项目 | 主应用（`run_qemu.ps1`） | 单元测试（`run_tests.ps1` + QEMU 板型） |
|------|--------------------------|----------------------------------------|
| 源码目录 | 仓库根 `.` | `tests/` |
| 默认 CONF | `prj.conf;conf/targets/qemu.conf` | `tests/prj.conf` + 叠加层 |
| 入口 | `app_main` | ztest |
| Devicetree overlay | 根目录 `boards/qemu_*.overlay` 自动合并 | 应用根为 `tests/`，根目录 overlay **不会**自动合并；`qemu_riscv32` 默认内存通常足够 |

### 6.6 成功输出示例

`qemu_riscv32` 上 ztest 典型串口输出（节选）：

```text
*** Booting Zephyr OS build ...
Running TESTSUITE event_system
...
PROJECT EXECUTION SUCCESSFUL
```

### 6.7 SMP 与板型建议

- **推荐板型**：**`qemu_riscv32`**（与 §3.1 一致）。
- **SMP 实验**：`ZEPHYR_TEST_BOARD=qemu_riscv32/qemu_virt_riscv32/smp`（见 §5）。
- **避免**：`qemu_cortex_m3` 等 ARM M 系板型（见 §8.4）。

### 6.8 单元测试覆盖率（`src/core/` 目标 ≥95%）

QEMU 目标无主机 `.gcda` 文件；启用 `CONFIG_COVERAGE=y` 与 `CONFIG_COVERAGE_DUMP=y` 后，gcov 数据经串口以 `GCOV_COVERAGE_DUMP_START/END` 块输出，由 `run_qemu.ps1` 调用 Zephyr 的 gen_gcov_files.py 与 **gcovr** 生成报告。

```powershell
. .\scripts\setup_env.ps1

# 默认板型 qemu_riscv32；CI 对齐门槛 80%
.\scripts\run_qemu.ps1 -Tests -Coverage -CoverageMin 80

# 95% 冲刺目标（未达标时非零退出；当前约 77%）
.\scripts\run_qemu.ps1 -Tests -Coverage -CoverageMin 95

# 可选：指定过滤路径与最低门槛
.\scripts\run_qemu.ps1 -Tests -Coverage -CoverageFilter "src/core/" -CoverageMin 95

# 已有日志时仅重算 HTML/JSON（跳过构建与 QEMU）
.\scripts\run_qemu.ps1 -Tests -Coverage -ReportOnly -CoverageLog coverage_qemu_qemu_riscv32.log
```

| 项 | 说明 |
|----|------|
| 默认 CONF | `prj.conf;prj_test_extensions.conf;prj_qemu_coverage.conf;prj_block_overflow.conf` |
| 构建目录 | `build_tests_qemu_cov_qemu_riscv32` |
| HTML 报告 | `coverage-qemu-core.html` |
| 日志 | `coverage_qemu_qemu_riscv32.log` |
| gcov 工具 | Zephyr SDK 内 `riscv64-zephyr-elf-gcov` |
| 依赖 | `pip install gcovr` |

`prj_qemu_coverage.conf` 加大堆与各服务线程栈（gcov 插桩耗栈），并开启 `EVENT_RUNTIME_STATUS` / `EVENT_DEBUG_MEM`；覆盖率构建默认关闭 `CONFIG_ZTEST_CONCURRENCY_STRESS` 以避免栈溢出。

与 CI 区别：GitHub Actions 在 **`native_sim`** 上对 `src/core/` 执行 **≥80%** 门禁。本地 Windows 以 QEMU 全量 ztest 验证同一范围；**95%** 为下一阶段目标（ISR 入队、`SYS_INIT` 自动初始化等路径仍难触达）。

---

## 7. 主应用成功输出示例

`qemu_riscv32` 上典型串口输出（节选）：

```text
[00:00:00.000,000] <inf> example_module_a: Initializing Example Module A...
...
  Version:       1.0.0
  Target:        qemu_riscv32
...
[00:00:00.020,000] <inf> app_main: Application ready (modules=1)
```

---

## 8. 常见问题

### 8.1 `QEMU-NOTFOUND` 或找不到 `qemu-system-*`

**原因**：首次 `west build` 时 CMake 未找到 QEMU，缓存为 `QEMU-NOTFOUND`。

**处理**：

1. 在 **`zephyr_config.env`** 中设置正确的 **`QEMU_BIN_PATH`**。
2. 执行 **`. .\scripts\setup_env.ps1`**（或 `source scripts/setup_env.sh`）。
3. **重新配置**构建目录：`west build ... -p always`。

### 8.2 运行后长时间无串口输出

**现象**：构建成功并显示 `Starting QEMU...`，但终端没有任何 Zephyr 启动日志。

**原因**：早期版 `run_qemu.ps1` 用 `2>&1` 缓冲了 `west build -t run` 的全部输出；QEMU 是长期前台进程，日志会被憋到退出后才一次性打印。

**处理**：使用已修复的 `run_qemu.ps1`（直接流式调用 `west`）。若仍无输出，请在本地 **交互式** PowerShell 窗口中运行，不要通过无 TTY 的后台任务或输出重定向启动 QEMU。

### 8.3 `west build -t run` 在非交互环境失败

QEMU 默认将串口绑定到 **stdio**。请在本地 PowerShell / 终端窗口中运行，不要依赖无 TTY 的后台任务。

### 8.4 `qemu_cortex_m3` 上 HardFault / Lockup

Zephyr 官方 `hello_world` 在同板型上可正常运行，本应用在 ARM Cortex-M QEMU 上存在已知兼容问题。**Windows 上请优先使用 `qemu_riscv32`**。若必须在 M 系 QEMU 上调试，请单独排查 ARM 相关初始化路径。

### 8.5 叠加 `conf/profiles/balanced.conf` 后链接失败 `undefined reference to sys_reboot`

`conf/profiles/balanced.conf` 启用了软件看门狗相关选项，部分 QEMU 板未提供 `sys_reboot` 实现。仿真请使用 **`conf/targets/qemu.conf`**，其中保持 **`CONFIG_SYS_WATCHDOG_ENABLE=n`**。

### 8.6 SMP 配置失败：合并了 `app.overlay` 或 DTS 报错

**现象**：构建日志出现 `Found devicetree overlay: .../app.overlay`，随后 `Configuring incomplete`。

**原因**：SMP 板型带 qualifier（如 `qemu_virt_riscv32/smp`），Zephyr 只匹配 **`boards/<板名>_<soc>_<variant>.overlay`**。若找不到，会回退到根目录 **`app.overlay`**（本仓库为 **STM32L4R5 SRAM 扩展**，不适用于 QEMU）。

**处理**：确认仓库已包含对应 SMP overlay（见 §5.3 表格），构建日志应显示：

```text
Found devicetree overlay: .../boards/qemu_riscv32_qemu_virt_riscv32_smp.overlay
```

### 8.7 SMP 板型名写错

使用 **`west boards`** 查看完整板名。须写全 qualifier，例如 `qemu_riscv32/qemu_virt_riscv32/smp`，不要写成 `qemu_riscv32_smp`。

### 8.8 `qemu_kvm_arm64`

该板依赖 Linux **KVM**，无法在 Windows 原生环境使用。

### 8.9 Windows 上 `run_tests.ps1` 提示 POSIX 板型不可用

**现象**：未设置 `ZEPHYR_TEST_BOARD` 时脚本报错 `native_sim` / `native_posix` 不能在 Windows 运行。

**处理**：按 §6.2 设置 `$env:ZEPHYR_TEST_BOARD = 'qemu_riscv32'`（或其他 QEMU 板型）后再运行 `run_tests.ps1`。或使用 WSL 跑主机测试。

### 8.10 单元测试 QEMU 构建后无 `PROJECT EXECUTION SUCCESSFUL`

**现象**：QEMU 启动但 ztest 未跑完或链接失败。

**处理**：

1. 确认在交互式终端运行 `west build -t run`（见 §8.3）。
2. 内存不足时改用 `prj.conf`（去掉 `prj_test_extensions.conf` 中的并发压测）。
3. 确认 **`QEMU_BIN_PATH`** 已配置并 `-p always` 重新配置构建目录。

### 8.11 Windows 上中文日志乱码

**现象**：串口日志里的中文显示为 `鍒濆鍖` 一类乱码，英文正常。

**原因**：固件 `LOG_*` / `printk` 字符串为 **UTF-8**；中文 Windows 上 PowerShell 默认代码页常为 **GBK（CP936）**，QEMU 将串口绑定到 stdio 时字节被按错误编码解释。

**处理**：

1. 使用已更新的 **`run_qemu.ps1`**（启动前自动 `chcp 65001` 并设置控制台 UTF-8）。
2. 手动运行 `west build -t run` 时，先执行：

```powershell
chcp 65001
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = [System.Text.UTF8Encoding]::new($false)
```

3. 推荐使用 **Windows Terminal** 或 **PowerShell 7+**；可在「设置 → 时间和语言 → 语言和区域 → 管理语言设置」中启用 **“Beta: 使用 Unicode UTF-8 提供全球语言支持”**（系统级，可选）。
4. 若需恢复默认编码，设置环境变量 **`ZEPHYR_CONSOLE_UTF8=0`** 跳过脚本中的 UTF-8 切换。

---

## 9. 相关文档

- [51-单元测试与持续集成说明.md](../50-测试与CI/51-单元测试与持续集成说明.md) — ztest 覆盖范围与 CI 矩阵

- [12-Freestanding应用与构建基础.md](12-Freestanding应用与构建基础.md) — `west build`、overlay 通用规则
- [15-新建APP开发指南.md](15-新建APP开发指南.md) — 创建 APP 仓库、`*_conf/targets/qemu.conf` 与脚本配置
- [44-设备树与内存配置手册.md](../40-应用开发/44-设备树与内存配置手册.md) — `boards/*.overlay` 合并机制
- [53-硬件测试运行指南.md](../50-测试与CI/53-硬件测试运行指南.md) — 真实硬件烧录与测试
- [63-脚本与工具说明.md](../60-调试与排错/63-脚本与工具说明.md) — `run_qemu.ps1` 等脚本
