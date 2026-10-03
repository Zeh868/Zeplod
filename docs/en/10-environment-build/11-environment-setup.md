> Language: [中文](../../zh-CN/10-环境与构建/11-环境搭建与配置指南.md) | **English**

# Environment Setup and Configuration Guide

This guide covers how to install **Zephyr SDK**, **West**, **CMake**, **Python**, how to fill in **`zephyr_config.env`**, and how to verify that the project can be built with **`west build`**. **The entire text starts from scratch**; if you only need to look up a specific step, use the table of contents to jump directly.

**Recommendation**: If this is your first time, please read "Path A: First Build of This Project" in **[Documentation Index.md](../00-getting-started/02-doc-index.md)** before following this guide step by step.

## Prerequisites

Before configuring this project, ensure the following are installed:

1. **Zephyr SDK** - Download: https://github.com/zephyrproject-rtos/zephyr-sdk
2. **Zephyr Source Code** - Clone: https://github.com/zephyrproject-rtos/zephyr
3. **West** - Install via: `pip install west`
4. **CMake** (3.20.0 or higher)
5. **Python 3.8+**

## Configuration Steps

### Step 1: Locate Zephyr Installation Paths

Find the paths to your local Zephyr installation:

- **Zephyr SDK Path**: Where Zephyr SDK is installed
  - Windows default: `C:/zephyr-sdk`
  - Linux default: `/opt/zephyr-sdk`
  - macOS default: `/opt/zephyr-sdk`

- **Zephyr Base Path**: Where Zephyr source code is cloned
  - Example: `D:/zephyrproject/zephyr` or `/home/user/zephyrproject/zephyr`

### Step 2: Configure Paths

Choose one of the following methods:

#### Method A: Configuration File (Recommended)

1. Copy the template file:
   ```bash
   copy zephyr_config.env.template zephyr_config.env
   ```

2. Edit `zephyr_config.env` and enter your paths:
   ```bash
   ZEPHYR_SDK_INSTALL_DIR=C:/zephyr-sdk
   ZEPHYR_BASE=D:/zephyrproject/zephyr
   ```

#### Method B: Setup Script

Run the setup script for your platform:

```bash
# Windows PowerShell
.\scripts\setup_env.ps1

# Windows CMD
scripts\setup_env.bat

# Linux/macOS
source scripts/setup_env.sh
```

#### Method C: Environment Variables

Set environment variables directly:

```bash
# Windows (PowerShell)
$env:ZEPHYR_BASE="D:/zephyrproject/zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR="C:/zephyr-sdk"

# Windows (CMD)
set ZEPHYR_BASE=D:\zephyrproject\zephyr
set ZEPHYR_SDK_INSTALL_DIR=C:\zephyr-sdk

# Linux/macOS
export ZEPHYR_BASE=/path/to/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk
```

### Step 3: Verify Configuration

Check that paths are set correctly:

```bash
# Windows PowerShell
echo $env:ZEPHYR_BASE
echo $env:ZEPHYR_SDK_INSTALL_DIR

# Linux/macOS
echo $ZEPHYR_BASE
echo $ZEPHYR_SDK_INSTALL_DIR
```

### Step 4: Build the Project

```bash
# Linux/WSL2: clean native_sim build (optional)
west build -b native_sim . --clean

# Linux/WSL: build for native_sim (testing)
west build -b native_sim .

# Build for target development board
west build -b <your_board> .
```

macOS can use `setup_env.sh` to activate the environment, but it cannot run `native_sim`.
For a hardware-free path, use the [QEMU Simulation Guide](14-qemu-simulation-guide.md).

## Troubleshooting

### Error: ZEPHYR_BASE not set

**Solution**: Ensure the environment variable is set or the setup script has been run.

```bash
# Check if it's set
echo $env:ZEPHYR_BASE  # Windows PowerShell
echo $ZEPHYR_BASE      # Linux/macOS

# If not set, run the setup script
.\scripts\setup_env.ps1  # Windows
source scripts/setup_env.sh  # Linux/macOS
```

### Error: Zephyr SDK not found

**Solution**: Verify `ZEPHYR_SDK_INSTALL_DIR` points to the correct SDK location.

```bash
# Check if directory exists
Test-Path "C:/zephyr-sdk"  # Windows
ls /opt/zephyr-sdk         # Linux/macOS
```

### Error: CMake configuration failed

**Solution**: Clear CMake cache and rebuild.

```bash
# Delete build directory
rm -rf build/

# Rebuild
west build -b <your_board> . --clean
```

### Error: West not found

**Solution**: Install west using pip.

```bash
pip install west
```

## Custom Board Configuration

To build for a specific board:

1. Create board configuration in `boards/` directory
2. Modify `zephyr_config.env` to set `DEFAULT_BOARD`
3. Build using the board flag:

```bash
west build -b your_board_name .
```

## Updating Paths

If you move your Zephyr installation:

1. Edit `zephyr_config.env` with the new path
2. Run the setup script again
3. Clean and rebuild:

```bash
west build -b <your_board> . --clean
```

## Environment Variable Reference

| Variable | Description | Example |
|----------|-------------|---------|
| `ZEPHYR_BASE` | Path to Zephyr source root directory | `D:/zephyrproject/zephyr` |
| `ZEPHYR_SDK_INSTALL_DIR` | Path to Zephyr SDK | `C:/zephyr-sdk` |
| `QEMU_BIN_PATH` | QEMU binaries directory (`west build -t run`) | `C:/Program Files/qemu` |
| `DEFAULT_BOARD` | Default build board | `native_sim` (Linux/WSL2 only; override with a QEMU board on macOS) |
| `BUILD_DIR` | Build output directory | `build` |

See **[14-qemu-simulation-guide.md](14-qemu-simulation-guide.md)** for QEMU build and run commands.

## Special Notes for Windows Users

### PowerShell Execution Policy

If you encounter execution policy errors when running `setup_env.ps1`, run:

```powershell
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
```

### Path Format

On Windows, use forward slashes `/` or double backslashes `\\`:
- ✅ `C:/zephyr-sdk`
- ✅ `C:\\zephyr-sdk`
- ❌ `C:\zephyr-sdk` (may cause issues in some cases)

## Special Notes for Linux/macOS Users

### Permission Issues

If you encounter permission issues, you may need to install the SDK with sudo:

```bash
sudo ./zephyr-sdk-xxx.run
```

### User Groups

On some Linux distributions, you may need to add the user to specific groups:

```bash
sudo usermod -a -G dialout $USER
```

## Additional Resources

- [Zephyr Documentation](https://docs.zephyrproject.org/)
- [Zephyr SDK Installation](https://docs.zephyrproject.org/latest/getting_started/index.html)
- [West Documentation](https://docs.zephyrproject.org/latest/guides/west/index.html)
- [Zephyr GitHub](https://github.com/zephyrproject-rtos/zephyr)

## FAQ

### Q: Can I support multiple boards simultaneously?

A: Yes. Simply run `west build -b <board>` separately for each board.

### Q: How do I change the log level?

A: Edit the `CONFIG_SYS_LOG_LEVEL` value in `prj.conf`, or edit `include/zeplod/app_config.h`.

### Q: What boards does this project support?

A: All boards supported by Zephyr. Configure required peripherals and features in `prj.conf`.

### Q: How do I add a new module?

A: Refer to the "Creating Custom Modules" section in `README.md`.

---

For more help, see the main [README.md](../../../README.md).
