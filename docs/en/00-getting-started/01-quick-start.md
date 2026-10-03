> Language: [中文](../../zh-CN/00-入门/01-5分钟快速体验.md) | **English**

# 5-Minute Quick Start

**Goal**: build and run Zeplod without a development board in five minutes, assuming a working Zephyr 4.3.x environment.

> If Zephyr, the SDK, West, or their dependencies are not installed, complete the
> [Environment Setup Guide](../10-environment-build/11-environment-setup.md) first. Initial downloads are not part of the five minutes.

## Prerequisite check

```bash
python --version
west --version
cmake --version
```

Use one of these supported paths:

- **Linux / WSL2 (recommended)**: use Zephyr's `native_sim` board.
- **Native Windows**: install QEMU and use the repository's `qemu_riscv32` PowerShell path.
- **macOS**: install QEMU and use `qemu_riscv32`. macOS and native Windows do not support `native_sim` or `native_posix`.

## Step 1: get the source

```bash
git clone https://github.com/QZehao/Zeplod.git
cd Zeplod
```

## Step 2: configure local paths

Create the local file from the template and set your Zephyr and SDK paths:

```powershell
# Windows PowerShell
Copy-Item zephyr_config.env.template zephyr_config.env
.\scripts\setup_env.ps1
```

```bash
# Linux / macOS / WSL
cp zephyr_config.env.template zephyr_config.env
# Edit zephyr_config.env, then load it
source scripts/setup_env.sh
```

`zephyr_config.env` is machine-local and must not be committed. If repository history still tracks it, use the template as the
reference until maintainers clean up that repository state.

## Step 3: build and run

### Linux / WSL2: native_sim

```bash
west build -b native_sim . -p always
west build -t run
```

### Native Windows: QEMU

```powershell
.\scripts\run_qemu.ps1 -Board qemu_riscv32
```

The script builds with `prj.conf;conf/targets/qemu.conf` and starts QEMU. See the
[QEMU Simulation Guide](../10-environment-build/14-qemu-simulation-guide.md) for more boards and options.

### macOS: QEMU

```bash
west build -b qemu_riscv32 -d build_qemu . -p always -- \
  -DCONF_FILE="prj.conf;conf/targets/qemu.conf"
west build -t run --build-dir build_qemu
```

A successful run shows Zephyr startup information, the Zeplod banner, and a shell prompt. Commit hashes, timestamps, and log levels
vary per build, so this guide does not present a fabricated fixed transcript.

## Step 4: inspect the application

Run these commands in the shell:

```text
app status
app modules
app events
app help
version
```

They display application state, registered modules, event statistics, help, and version/build information.

## What this proves

- Zephyr, the SDK, West, and local paths work together.
- The application configures, compiles, and links for host simulation or QEMU.
- Zeplod's automatic initialization chain and shell start successfully.

This is not hardware validation. Peripherals, flashing, RAM layout, and board overlays still require target-board testing.

## Next steps

| Goal | Read next |
|---|---|
| Understand the repository and workflow | [Developer Guide](04-developer-guide.md) |
| Navigate by task | [Task Navigation](05-task-navigation.md) |
| Use the event system | [Event System Guide](../30-core-modules/31-event-system-guide.md) |
| Add a module or use the GPIO example | [Module System Guide](../30-core-modules/32-module-system-guide.md) |
| Flash real hardware | [Flash and Debug Quick Start](../60-debugging/61-flash-debug-quickstart.md) |
| Diagnose a build or runtime problem | [Troubleshooting](../60-debugging/62-troubleshooting.md) |

## Common problems

**West or the SDK is not found**: check the virtual environment, `ZEPHYR_BASE`, and `ZEPHYR_SDK_INSTALL_DIR` in the environment guide.

**Windows or macOS reports that a POSIX board cannot run**: use the QEMU command above; Windows can also run `native_sim` from WSL2.

**A configuration change has no effect**: use `-p always`, or run `west build -t pristine` before rebuilding.
