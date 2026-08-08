# 单元测试指南

本目录包含 **Zeplod** 工程的单元测试（ztest），与主应用共享 `../src/` 下的实现；**不**编译 `app_main` 与示例业务模块。因此**没有**主固件里的 **`SYS_INIT` 启动链**；各用例需自行按依赖调用 `module_manager_init()`、`event_system_init()` 等（与 [模块系统详细使用说明.md](../docs/zh-CN/30-核心模块/32-模块系统详细使用说明.md) 中「注册和使用模块」手写示例一致）。

**仓库级概览**（与 CI 的关系、与主应用差异）：见 **[docs/zh-CN/50-测试与CI/51-单元测试与持续集成说明.md](../docs/zh-CN/50-测试与CI/51-单元测试与持续集成说明.md)**。

## 配置文件

| 文件 | 用途 |
|------|------|
| `prj.conf` | **默认**：精简配置，**`CONFIG_THREAD_IPC_SERVICE=n`**，适合快速冒烟与 CI（Zephyr 4.3.0 容器使用 `native_sim`） |
| `prj_native_sim.conf` | **叠加层**：较大堆、Slab、**`CONFIG_THREAD_IPC_SERVICE=y`**（须与 `prj.conf` 联用） |
| `prj_ci_examples.conf` | 启用示例模块 A/B/GPIO/Multi（与上两文件联用） |
| `prj_qemu_coverage.conf` | QEMU + gcov 叠加：加大堆/栈、开启 `EVENT_RUNTIME_STATUS` / `EVENT_DEBUG_MEM`；**须与 `prj.conf` 联用** |
| `prj_block_overflow.conf` | `CONFIG_EVENT_QUEUE_OVERFLOW_BLOCK` 与 BLOCK 溢出用例（覆盖率构建默认叠加） |
| `prj_sys_diag.conf` | `CONFIG_SYS_DIAG_ENABLE` 与 `test_sys_diag.c`（CI `build_tests_diag`） |

默认 **`tests/prj.conf` 不开启 IPC**，不链接 `test_ipc_service.c`。需要 IPC 烟测时：

```bash
west build -b native_sim tests/ --build-dir build_tests \
  -- -DCONF_FILE="prj.conf;prj_native_sim.conf"
# 或: ./scripts/run_tests_ipc.sh
```

或在 `prj.conf` 中将 `CONFIG_THREAD_IPC_SERVICE` 改为 `y`（并确保 `tests/CMakeLists.txt` 中 IPC 源文件条件编译已满足）。

## 主机仿真板型

| Zephyr 版本 | 推荐板型 | 说明 |
|-------------|----------|------|
| 4.3.0（与 CI 容器一致） | **`native_sim`** | GitHub/GitLab CI 与本机统一使用该板型 |

可使用仓库脚本自动选择板型（见下方「运行测试」）。

## 目录结构

```
tests/
├── CMakeLists.txt
├── Kconfig                 # rsource 复用仓库根目录 Kconfig
├── prj.conf
├── prj_native_sim.conf     # native_sim 全量配置（可选）
├── test_event_system.c
├── test_event_system_compat.c
├── test_event_queue.c
├── test_event_dispatcher.c
├── test_core_coverage.c
├── test_module_manager.c
├── test_sys_memory.c
├── test_sys_timer.c
├── test_sys_watchdog.c
├── test_sys_log.c
└── test_ipc_service.c      # 需 CONFIG_THREAD_IPC_SERVICE=y
```

## 运行测试

### 前提

- 仓库根目录已配置 **`zephyr_config.env`**（由 `zephyr_config.env.template` 复制并填写路径）。
- **先激活环境**（加载 venv、`ZEPHYR_BASE`、SDK 到 PATH）：

```powershell
# Windows（当前终端生效）
.\scripts\setup_env.ps1
```

```bash
# Linux / macOS / WSL
source scripts/setup_env.sh
```

### 推荐：使用脚本（自动激活环境 + 选择板型）

**Linux / macOS / WSL：**

```bash
./scripts/run_tests.sh
```

**Windows（PowerShell）：**

```powershell
.\scripts\run_tests.ps1
```

脚本会先执行 **`setup_env`**（读取 `zephyr_config.env`），再构建/运行测试；无需单独激活环境。  
板型优先 `native_sim`（若 `west boards` 列出），否则 `native_posix`。可用环境变量 `ZEPHYR_TEST_BOARD`、`ZEPHYR_TEST_CONF` 覆盖。

### West（手动）

Zephyr 4.x（本机）：

```bash
west build -b native_sim tests/ --build-dir build_tests
west build -t run --build-dir build_tests
```

Zephyr 4.3.0 / CI 对齐：

```bash
west build -b native_sim tests/ --build-dir build_tests
west build -t run --build-dir build_tests
```

全量 IPC + Slab（`native_sim`）：

```bash
west build -b native_sim tests/ --build-dir build_tests \
  -- -DCONF_FILE="prj.conf;prj_native_sim.conf"
west build -t run --build-dir build_tests
```

快捷脚本（Linux/macOS/WSL）：

```bash
./scripts/run_tests.sh              # 默认 prj.conf（核心 + data_bus）
./scripts/run_tests_ipc.sh          # + IPC
./scripts/run_tests_full.sh         # + IPC + 示例模块
```

### 配置矩阵（叠加 `prj.conf`）

| 叠加文件 | 用途 |
| --- | --- |
| `prj.conf;prj_test_extensions.conf` | P0/P1 扩展 + 并发压测（CI 默认；`run_tests.sh`） |
| `prj.conf;prj_concurrency_stress.conf` | 仅并发压测（硬件 640KB SRAM 后本地验证） |
| `prj.conf;prj_native_sim.conf` | IPC + 大堆 Slab（CI `build_tests_ipc`；脚本 `./scripts/run_tests_ipc.sh`） |
| `prj.conf;prj_native_sim.conf;prj_ci_examples.conf` | 再上示例模块 A/B/GPIO/Multi（CI `build_tests_full`；`./scripts/run_tests_full.sh`） |
| `prj.conf;prj_test_extensions.conf;prj_qemu_coverage.conf;prj_block_overflow.conf` | **QEMU 覆盖率**（`run_qemu.ps1 -Tests -Coverage` 默认；目标 `src/core/` ≥95%） |
| `prj_block_overflow.conf` | `CONFIG_EVENT_QUEUE_OVERFLOW_BLOCK` 与 `test_block_publish_unblocks_on_stop`（CI `build_tests_block`） |
| `prj.conf;prj_data_bus_optimized.conf` | Data Bus 64/128/512B slab + `NO_MALLOC` 固定池路径（CI `build_tests_data_bus_optimized`） |
| `prj.conf;prj_sys_diag.conf` | `CONFIG_SYS_DIAG_ENABLE` 与 `test_sys_diag`（CI `build_tests_diag`） |
| `prj_test_watchdog.conf` | 看门狗相关套件 |

示例（BLOCK 溢出策略）：

```bash
west build -b native_sim tests/ --build-dir build_tests \
  -- -DCONF_FILE="prj.conf;prj_block_overflow.conf"
west build -t run --build-dir build_tests
```

在 Linux/macOS 上也可直接执行 `build_tests/zephyr/zephyr`。

### 套件间隔离

部分用例依赖全局事件/分发器状态。`test_event_dispatcher.c` 等在 `after_each` 中调用 `event_system_shutdown()` 以清空类型表与分发线程；新增套件时请遵循相同模式，避免跨套件污染。

## 生命周期与异步测试约定

- **边界契约表**：见 [LIFECYCLE_CONTRACTS.md](LIFECYCLE_CONTRACTS.md)（cancel、timeout、重复 stop 等返回值）。
- **线程服务对照**：见 [85-线程服务生命周期约定.md](../docs/zh-CN/80-贡献与维护/85-线程服务生命周期约定.md)（dispatcher / IPC / Data Bus）。
- **异步等待**：优先使用 [ztest_sync.h](ztest_sync.h)（`ztest_wait_atomic_*`、`ztest_wait_until`）或 `k_sem` / `k_event`；避免仅用裸 `k_msleep()` 判断异步完成。
- **订阅回调**：使用 [test_event_stubs.h](test_event_stubs.h) 中的 `test_event_noop_callback`，勿使用 `0x1000` 等不可调用地址。

## 编写新测试

### 测试模板

```c
#include <zephyr/ztest.h>
#include <被测试模块.h>

ZTEST(test_module_name, test_feature)
{
    zassert_equal(expected, actual, "错误消息");
}

ZTEST_SUITE(test_module_name, NULL, NULL, NULL, NULL, NULL);
```

### 常用断言宏

| 宏 | 描述 |
|----|------|
| `zassert_equal(expected, actual, msg)` | 断言相等 |
| `zassert_not_equal(expected, actual, msg)` | 断言不相等 |
| `zassert_true(condition, msg)` | 断言条件为真 |
| `zassert_false(condition, msg)` | 断言条件为假 |
| `zassert_null(pointer, msg)` | 断言指针为空 |
| `zassert_not_null(pointer, msg)` | 断言指针非空 |

## 测试覆盖率

### 范围与目标

| 范围 | 本地目标 | CI 门禁（`.github/workflows/ci.yml`） |
|------|----------|--------------------------------------|
| `src/core/` | 目标 **≥95%**（QEMU 全量 ztest；当前约 **77%**，见下方说明） | **≥80%**（`native_sim` + `--coverage`） |

全仓库 100% 不现实（`src/app/`、未纳入测试的路径、`SYS_INIT` 自动初始化路径等不在统计范围内）。

### Windows / QEMU（推荐）

Windows 原生无法运行 `native_sim`，使用 **`scripts/run_qemu.ps1`** 在 QEMU 上跑 ztest 并解析串口 gcov 转储：

```powershell
. .\scripts\setup_env.ps1

# 构建、运行测试、生成报告（CI 对齐门槛 80%）
.\scripts\run_qemu.ps1 -Tests -Coverage -CoverageMin 80

# 冲刺 95% 目标（当前未达标时会非零退出）
.\scripts\run_qemu.ps1 -Tests -Coverage -CoverageMin 95

# 仅根据已有日志重算报告（跳过 QEMU）
.\scripts\run_qemu.ps1 -Tests -Coverage -ReportOnly -CoverageLog coverage_qemu_qemu_riscv32.log
```

默认 CONF 链：`prj.conf;prj_test_extensions.conf;prj_qemu_coverage.conf;prj_block_overflow.conf`。  
产物：`coverage-qemu-core.html`、`coverage-qemu-core-summary.json`、日志 `coverage_qemu_<board>.log`。

需已安装 **Zephyr SDK**（`riscv64-zephyr-elf-gcov`）与 **gcovr**（`pip install gcovr`）。

### Linux / macOS / WSL（native_sim）

将 `native_sim` 或 `native_posix` 替换为当前环境可用板型：

```bash
west build -b native_sim tests/ --build-dir build_tests_cov -p always \
  -- -DCMAKE_C_FLAGS="--coverage"
west build -t run --build-dir build_tests_cov
gcovr -r . --filter "src/core/" --html --html-details coverage-core.html --print-summary
```

## 持续集成

`native_sim` 测试在 GitHub Actions 的 `build-tests` 任务中执行（Zephyr **4.3.0** 容器）；见 `.github/workflows/ci.yml`。本机与 CI 统一使用 `native_sim`。

## 参考文档

- [Zephyr 测试框架](https://docs.zephyrproject.org/latest/develop/test.html)
- [ztest API 参考](https://docs.zephyrproject.org/latest/develop/test/test_api_reference.html)

## Advanced Local Checks

### Sanitizers (host-only)

Run AddressSanitizer / UndefinedBehaviorSanitizer on host simulation boards:

```bash
./scripts/run_sanitizers.sh
# PowerShell: .\scripts\run_sanitizers.ps1
```

Environment overrides:

- `ZEPHYR_SANITIZER=asan|ubsan|asan-ubsan` (default: `asan-ubsan`)
- `ZEPHYR_TEST_BOARD=native_sim|native_posix`
- `ZEPHYR_TEST_CONF=<conf-file>`
- `ZEPHYR_SAN_BUILD_DIR=<build-dir>`

### Twister

Run Zephyr Twister against this repository's `tests/` suite:

```bash
./scripts/run_twister.sh
# PowerShell: .\scripts\run_twister.ps1
```

Environment overrides:

- `ZEPHYR_TWISTER_PLATFORM=native_sim|native_posix` (default: `native_sim`)
- `ZEPHYR_TWISTER_OUT_DIR=<output-dir>`

### Host/Board Support Matrix

| Host OS | `native_sim` / `native_posix` | Recommended path |
|---|---|---|
| Linux | Supported | Run `./scripts/run_tests.sh`, `./scripts/run_sanitizers.sh`, `./scripts/run_twister.sh` |
| WSL (Linux kernel) | Supported | Same as Linux |
| Windows (native) | Not supported for POSIX arch | Use WSL/Linux for host tests, or use hardware board tests |

Notes:
- `run_tests.ps1`, `run_sanitizers.ps1`, and `run_twister.ps1` now fail fast on native Windows when POSIX boards are selected.
- Use `ZEPHYR_TEST_BOARD=<hardware_board>` for non-host board builds on Windows.
