> Language: [中文](../../zh-CN/10-环境与构建/13-板型迁移指南.md) | **English**

# Board Porting Guide

This document describes the configuration and modification work required when a project needs to switch to or add a new development board (Board).

---

## Table of Contents

- [Applicable Scenarios](#applicable-scenarios)
- [Preparation](#preparation)
- [Step 1: Confirm Zephyr Board Support](#step-1-confirm-zephyr-board-support)
- [Step 2: Add or Modify Board Configuration](#step-2-add-or-modify-board-configuration)
- [Step 3: Adjust Devicetree Overlays](#step-3-adjust-devicetree-overlays)
- [Step 4: Adjust Memory and Partition Configuration](#step-4-adjust-memory-and-partition-configuration)
- [Step 5: Update Project Configuration](#step-5-update-project-configuration)
- [Step 6: Update CI/CD Configuration](#step-6-update-cicd-configuration)
- [Step 7: Verify and Debug](#step-7-verify-and-debug)
- [Common Issues](#common-issues)

---

## Applicable Scenarios

- Migrating from one development board to another (e.g., from `nucleo_l4r5zi` to `nucleo_h743zi`)
- Adding multi-board support to a project
- Using custom development boards or product boards
- Switching MCU series or vendors

---

## Pre-Migration Preparation

Before starting, prepare the following information:

1. **Target board name**: Board identifier in Zephyr (e.g., `nucleo_l4r5zi`)
2. **MCU model**: Specific chip model (e.g., `STM32L4R5ZITx`)
3. **Memory specifications**: Flash and RAM sizes
4. **Peripheral requirements**: GPIO, UART, SPI, I2C, etc. needed by the project
5. **Debug interface**: SWD, JTAG, or other debugger types

---

## Step 1: Confirm Zephyr Board Support

### 1.1 Check if Zephyr Already Supports the Target Board

```bash
# Search for board in Zephyr directory
cd %ZEPHYR_BASE%
find boards -name "*<board_name>*"
```

Or query in Zephyr documentation: https://docs.zephyrproject.org/latest/boards/index.html

### 1.2 View Board Support Levels

Zephyr classifies board support into the following levels:

| Level | Description |
|-------|-------------|
| **Supported** | Full support, recommended for production |
| **Maintained** | Has maintainer support |
| **Provisional** | Initial support, may be incomplete |
| **Unsupported** | Not supported, requires custom porting |

**Recommendation**: Prioritize boards at **Supported** or **Maintained** level.

### 1.3 Review Board Documentation

Each board in Zephyr has a corresponding documentation page, containing:

- Board features and peripherals
- Pin definitions and functions
- Default configuration (defconfig)
- Device tree structure

Example path: `$ZEPHYR_BASE/boards/<vendor>/<board_name>/`

---

## Step 2: Add or Modify Board Configuration Files

### 2.1 Using Zephyr Built-in Boards

If the target board is already supported by Zephyr, **no** board files need to be added to the project; just specify it during build:

```bash
west build -b <board_name> .
```

### 2.2 Adding Custom Boards

If a custom board (product board or unsupported board) is needed, create it under `boards/`:

```
boards/
└── <vendor>/
    └── <board_name>/
        ├── <board_name>.yaml
        ├── <board_name>_defconfig
        ├── <board_name>.dts
        ├── <board_name>.overlay
        ├── Kconfig.defconfig
        └── <board_name>.cmake
```

#### 2.2.1 Board YAML File (Required)

Define board metadata:

```yaml
identifier: <board_name>
name: <Board Display Name>
type: mcu
arch: <arch>
toolchain:
  - gnuarmemb
  - zephyr
ram: <size_in_kb>
flash: <size_in_kb>
supported:
  - gpio
  - uart
  - spi
  - i2c
  - adc
testing:
  ignore_tags:
    - net
    - bluetooth
```

#### 2.2.2 Default Configuration (Recommended)

`<board_name>_defconfig` sets board-default enabled drivers and features:

```kconfig
CONFIG_SOC_<soc_series>_y=y
CONFIG_UART_CONSOLE=y
CONFIG_GPIO=y
```

#### 2.2.3 Device Tree File (Optional)

Create a `.dts` file if full custom device tree is needed; otherwise, use device tree overlays.

#### 2.2.4 CMake Configuration (Optional)

`<board_name>.cmake` sets board-specific build parameters:

```cmake
board_runner_args(openocd "--gdb-port=3333" "--telnet-port=4444")
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
```

### 2.3 Enable Custom Board Path

Uncomment `BOARD_ROOT` in the root `CMakeLists.txt`:

```cmake
# Enable custom board directory
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/boards)
```

---

## Step 3: Adjust Device Tree Overlays

Device tree overlays (Overlay) customize hardware configuration without modifying Zephyr source code.

### 3.1 Common Overlay Files

The project provides the following device tree-related files:

- `boards/overlay.dts` - **Example template**, not auto-loaded, needs rename or explicit specification
- `app.overlay` - Application-level overlay (**auto fallback**, takes effect when no board-specific overlay exists)

### 3.2 Board-Specific Overlays

Create exclusive overlays for specific boards:

```
boards/
├── overlay.dts                    # Example template (needs rename to <board>.overlay to take effect)
├── nucleo_l4r5zi.overlay          # Only for nucleo_l4r5zi
├── nucleo_h743zi.overlay          # Only for nucleo_h743zi
└── <board_name>.overlay           # Custom board overlay
```

The build system automatically loads corresponding `.overlay` files based on the `BOARD` variable (if they exist). If no board-specific overlay exists, it falls back to `app.overlay`.

### 3.3 Common Overlay Configurations

#### 3.3.1 Enable UART

```dts
&uart0 {
    status = "okay";
};
```

#### 3.3.2 Configure GPIO

```dts
&gpio0 {
    status = "okay";
};

&led0 {
    gpios = <&gpio0 1 GPIO_ACTIVE_HIGH>;
};
```

#### 3.3.3 Configure SPI/I2C

```dts
&spi0 {
    status = "okay";
    cs-gpios = <&gpio0 2 GPIO_ACTIVE_LOW>;
};

&i2c0 {
    status = "okay";
    clock-frequency = <400000>;
};
```

#### 3.3.4 Configure External Crystal

```dts
&clock {
    clock-frequency = <DT_FREQ_M(8)>;  // 8MHz external crystal
};
```

### 3.4 Complete Migration Steps (Old Board to New Board)

#### Step 3.4.1: Export Full Device Tree of Old Board

```bash
# 1. Build with old board
west build -b <old_board> .

# 2. View compiled full device tree
cat build/zephyr/zephyr.dts > old_board.dts

# 3. View memory region definitions
cat build/zephyr/zephyr.dts | grep -A 5 "memory@"

# 4. View enabled peripheral nodes
cat build/zephyr/zephyr.dts | grep "status = \"okay\""
```

#### Step 3.4.2: Analyze Differences Between Old and New Boards

| Item | Old Board | New Board | Modification Needed |
|------|-----------|-----------|-------------------|
| MCU Model | `<old_soc>` | `<new_soc>` | - |
| Flash Size | `XXX KB` | `XXX KB` | Partition changes if different |
| RAM Size | `XXX KB` | `XXX KB` | `zephyr,sram` changes if different |
| UART Node | `&uart0` | `&uart0` | Check if labels match |
| GPIO Node | `&gpio0` | `&gpio0` | Check if labels match |
| Peripheral Labels | See exported DTS | See exported DTS | Record differences |

#### Step 3.4.3: Create Overlay File for New Board

```bash
# 1. Copy example template
cp boards/overlay.dts boards/<new_board>.overlay

# 2. Or copy from old board overlay (if exists)
cp boards/<old_board>.overlay boards/<new_board>.overlay
```

#### Step 3.4.4: Modify Overlay File Content

```dts
/* boards/<new_board>.overlay */

/*
 * Step 1: Delete SoC default memory nodes (if merging SRAM)
 * Label names need to reference the new board SoC DTSI file
 */
/delete-node/ &sram0;
/delete-node/ &sram1;
/delete-node/ &sram2;

/*
 * Step 2: Define contiguous memory region
 * Address and size need to reference new board MCU reference manual
 */
/ {
    sram0: memory@<base_address> {
        device_type = "memory";
        /* Base address, size (hex) */
        reg = <0x<BASE_ADDR> 0x<SIZE_IN_HEX>>;
    };

    /* Specify SRAM region used by Zephyr */
    zephyr,sram = &sram0;
};

/*
 * Step 3: Enable necessary peripherals
 * Label names need to match new board DTS
 */
&uart0 {
    status = "okay";
    current-speed = <115200>;
};

&gpio0 {
    status = "okay";
};

/*
 * Step 4: Define board-level peripherals (LEDs, buttons, etc.)
 */
& {
    aliases {
        led0 = &led0;
        sw0 = &sw0;
    };

    led0: led_0 {
        gpios = <&gpio0 <PIN_NUM> GPIO_ACTIVE_HIGH>;
        label = "User LED";
    };

    sw0: sw0 {
        gpios = <&gpio0 <PIN_NUM> (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;
        label = "User Button";
    };
};

/*
 * Step 5: Flash partitions (if NVS/Settings/OTA needed)
 */
&flash0 {
    #address-cells = <1>;
    #size-cells = <1>;
    ranges = <0x0 <FLASH_BASE_ADDR> DT_SIZE_K(<FLASH_SIZE_KB>)>;
    partitions {
        ranges;
        slot0_partition: partition@0 {
            label = "image-0";
            reg = <0x0 <IMAGE_SIZE>>;
        };
        storage_partition: partition@<STORAGE_OFFSET> {
            label = "storage";
            reg = <<STORAGE_OFFSET> <STORAGE_SIZE>>;
        };
    };
};

/*
 * Step 6: Bind Settings partition (persistent storage)
 */
& {
    chosen {
        zephyr,settings-partition = &storage_partition;
        zephyr,code-partition = &slot0_partition;
    };
};
```

#### Step 3.4.5: Find Node Labels for New Board

```bash
# Method 1: View Zephyr board-level DTS file
cat $ZEPHYR_BASE/boards/<vendor>/<board>/<board>.dts

# Method 2: View SoC series DTSI file
cat $ZEPHYR_BASE/dts/arm/<soc_series>.dtsi

# Method 3: View post-processed device tree after build
west build -b <new_board> .
cat build/zephyr/zephyr.dts | grep -E "&(uart|gpio|spi|i2c)"

# Method 4: Decompile board DTS using dtc
dtc -I dtb -O dts $ZEPHYR_BASE/build/<board>/zephyr/zephyr.dts -o board.dts
```

#### Step 3.4.6: Common MCU Memory Label Reference

| MCU Series | SRAM Labels | Flash Labels | Example |
|------------|-------------|--------------|---------|
| STM32L4 | `sram0`, `sram1`, `sram2` | `flash0` | Nucleo L4R5ZI |
| STM32H7 | `sram0`, `sram1`, `sram2`, `sram3` | `flash0` | Nucleo H743ZI |
| STM32F4 | `sram0` | `flash0` | Nucleo F429ZI |
| nRF52 | `sram0` | `flash0` | nRF52840 DK |
| nRF53 | `sram0`, `sram1` | `flash0` | nRF5340 DK |
| RP2040 | `sram0` | `flash0` | Raspberry Pi Pico |

### 3.5 Verify Device Tree

```bash
# View final merged device tree
west build -b <board_name>
cat build/zephyr/zephyr.dts

# Check memory regions are correct
cat build/zephyr/zephyr.dts | grep -A 3 "memory@"

# Check zephyr,sram pointer
cat build/zephyr/zephyr.dts | grep "zephyr,sram"

# Check peripheral status
cat build/zephyr/zephyr.dts | grep "status = \"okay\""
```

### 3.6 Device Tree Migration Checklist

- [ ] Exported and analyzed full device tree of old board
- [ ] Reviewed new board MCU memory mapping (datasheet)
- [ ] Created `boards/<new_board>.overlay`
- [ ] Deleted SoC default memory nodes (`/delete-node/`)
- [ ] Defined correct memory base address and size
- [ ] Set `zephyr,sram` pointer
- [ ] Enabled UART (for log output)
- [ ] Enabled GPIO (for LEDs/buttons)
- [ ] Defined Flash partitions (if persistent storage needed)
- [ ] Set `chosen` nodes (settings/code partitions)
- [ ] After build, checked `zephyr.dts` confirms overlay merged
- [ ] RAM size correct in build output

---

## Step 4: Adjust Memory and Partition Configuration

### 4.1 Check RAM/Flash Sizes

Different boards may have vastly different memory specifications; adjust the following:

```kconfig
# prj.conf or board defconfig
CONFIG_RAM_SIZE=<size_in_kb>
CONFIG_FLASH_SIZE=<size_in_kb>
```

### 4.2 Complete Memory Configuration Migration Steps

#### Step 4.2.1: Review New Board Memory Specifications

| Item | How to Obtain | Example Value |
|------|---------------|---------------|
| Total SRAM | MCU datasheet | 640 KB |
| SRAM partitions | MCU reference manual | SRAM1: 192KB, SRAM2: 64KB, SRAM3: 384KB |
| Total Flash | MCU datasheet | 2048 KB |
| SRAM base address | MCU reference manual | 0x20000000 |
| Flash base address | MCU reference manual | 0x08000000 |

#### Step 4.2.2: Calculate Available Memory

```
Total SRAM = SRAM1 + SRAM2 + SRAM3 + ...

Available main RAM (zephyr,sram) = Total contiguously-mapped SRAM
Reserved SRAM (optional) = For special purposes (DMA, backup, etc.)

Available Heap = Total SRAM - Main RAM usage - Reserved SRAM
```

#### Step 4.2.3: Adjust Heap Size

Adjust kernel heap based on available RAM:

```kconfig
# Large RAM boards (>256KB)
CONFIG_HEAP_MEM_POOL_SIZE=65536    # 64KB

# Medium RAM boards (128-256KB)
CONFIG_HEAP_MEM_POOL_SIZE=32768    # 32KB

# Small RAM boards (<64KB)
CONFIG_HEAP_MEM_POOL_SIZE=8192     # 8KB
```

#### Step 4.2.4: Adjust Stack Sizes

```kconfig
# Large RAM boards
CONFIG_MAIN_STACK_SIZE=4096
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048
CONFIG_EVENT_DISPATCHER_STACK_SIZE=4096

# Small RAM boards
CONFIG_MAIN_STACK_SIZE=2048
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=1024
CONFIG_EVENT_DISPATCHER_STACK_SIZE=2048
```

#### Step 4.2.5: Memory Configuration Migration Examples

**Example A: Migrating from STM32L4R5ZI (640KB RAM) to STM32H743ZI (1MB RAM)**

```dts
/* boards/nucleo_h743zi.overlay */

/* H743ZI has SRAM1(128KB) + SRAM2(128KB) + SRAM3(384KB) + SRAM4(64KB) + AXI SRAM(320KB) */
/* Address contiguously mapped, can merge as 1MB for use */

/delete-node/ &sram0;
/delete-node/ &sram1;
/delete-node/ &sram2;
/delete-node/ &sram3;

/ {
    sram0: memory@24000000 {
        device_type = "memory";
        /* H743ZI: 1MB = 0x100000 */
        reg = <0x24000000 0x100000>;
    };

    zephyr,sram = &sram0;
};
```

```kconfig
# prj.conf or prj_h743zi.conf
CONFIG_HEAP_MEM_POOL_SIZE=131072   # 128KB (larger heap)
CONFIG_MAIN_STACK_SIZE=8192        # 8KB (more relaxed)
CONFIG_EVENT_DISPATCHER_STACK_SIZE=8192
```

**Example B: Migrating from STM32L4R5ZI (640KB RAM) to nRF52840 (256KB RAM)**

```dts
/* boards/nrf52840dk.overlay */

/* nRF52840: 256KB RAM contiguous */
/delete-node/ &sram0;

/ {
    sram0: memory@20000000 {
        device_type = "memory";
        /* 256KB = 0x40000 */
        reg = <0x20000000 0x40000>;
    };

    zephyr,sram = &sram0;
};
```

```kconfig
# prj.conf or prj_nrf52840.conf
CONFIG_HEAP_MEM_POOL_SIZE=32768    # 32KB (reduced)
CONFIG_MAIN_STACK_SIZE=2048        # 2KB
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=1024
CONFIG_EVENT_DISPATCHER_STACK_SIZE=2048

# Optional: Disable logging to save resources
# CONFIG_LOG=n
```

**Example C: Small RAM boards (e.g., STM32F103C8T6 - 20KB RAM)**

```dts
/* boards/bluepill.overlay */

/* STM32F103C8T6: 20KB RAM */
/delete-node/ &sram0;

/ {
    sram0: memory@20000000 {
        device_type = "memory";
        /* 20KB = 0x5000 */
        reg = <0x20000000 0x5000>;
    };

    zephyr,sram = &sram0;
};
```

```kconfig
# prj_bluepill.conf - Minimal configuration
CONFIG_HEAP_MEM_POOL_SIZE=4096     # 4KB (minimum)
CONFIG_MAIN_STACK_SIZE=1024        # 1KB
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=512
CONFIG_EVENT_DISPATCHER_STACK_SIZE=1024

# Disable unnecessary features
CONFIG_LOG=n
CONFIG_PRINTK=n
CONFIG_CONSOLE=n
CONFIG_SYS_LOG_LEVEL=0

# Disable complex services
CONFIG_SYS_WATCHDOG=n
CONFIG_SYS_TIMER=n
CONFIG_THREAD_IPC_SERVICE=n
```

### 4.3 Flash Partitions (if NVS/OTA needed)

Define Flash partitions in device tree overlay:

```dts
/* Adjust partitions according to new board Flash size */
&flash0 {
    #address-cells = <1>;
    #size-cells = <1>;
    ranges = <0x0 <FLASH_BASE> DT_SIZE_K(<FLASH_SIZE_KB>)>;
    partitions {
        ranges;
        slot0_partition: partition@0 {
            label = "image-0";
            /* Reserve 80% for application */
            reg = <0x0 (DT_SIZE_K(<FLASH_SIZE_KB>) * 80 / 100)>;
        };
        storage_partition: partition@<storage_offset> {
            label = "storage";
            /* Remaining 20% for storage */
            reg = <<storage_offset> (DT_SIZE_K(<FLASH_SIZE_KB>) * 20 / 100)>;
        };
    };
};

& {
    chosen {
        zephyr,code-partition = &slot0_partition;
        zephyr,settings-partition = &storage_partition;
    };
};
```

### 4.4 Using SRAM Partition Configuration

This project uses SRAM partitions for optimization; when switching boards, check:

1. View SRAM configuration in `prj.conf`
2. Check if board overlay has SRAM merge configuration
3. Small RAM boards may need to disable some features

### 4.5 Memory Configuration Checklist

- [ ] Reviewed new board MCU memory specifications (datasheet)
- [ ] Calculated total available RAM
- [ ] Adjusted `CONFIG_HEAP_MEM_POOL_SIZE` (no more than 50% of available RAM)
- [ ] Adjusted stack sizes (`MAIN_STACK_SIZE`, `EVENT_DISPATCHER_STACK_SIZE`, etc.)
- [ ] Defined correct `zephyr,sram` region in overlay
- [ ] Defined Flash partitions (if persistent storage needed)
- [ ] Small RAM boards disabled unnecessary features (logging, complex services)
- [ ] After build, checked memory utilization (RAM < 80% is safe)
- [ ] Verified no stack overflow or memory exhaustion at runtime

---

## Step 5: Update Project Configuration Files

### 5.1 Update prj.conf

Adjust `prj.conf` based on new board hardware capabilities:

```kconfig
# Disable unneeded features to save resources
CONFIG_LOG=n                    # Small RAM boards can disable logging
CONFIG_SYS_LOG_LEVEL=0

# Or enable specific hardware drivers
CONFIG_GPIO=y
CONFIG_SPI=y
CONFIG_I2C=y
CONFIG_ADC=y
```

### 5.2 Create Board-Specific Configuration Files

Create dedicated configuration files for different boards:

```
prj.conf                        # Default configuration
prj_nucleo_l4r5zi.conf          # L4 series configuration
prj_nucleo_h743zi.conf          # H7 series configuration (high performance)
prj_low_ram.conf                # Small RAM board minimal configuration
```

Build with:

```bash
west build -b <board_name> . -- -DCONF_FILE="prj.conf;prj_<board_name>.conf"
```

### 5.3 Adjust Module Dependencies

Some modules may depend on specific hardware; adjust in `app_config.h`:

```c
// Enable/disable modules based on board
#if defined(CONFIG_BOARD_NUCLEO_L4R5ZI)
    #define APP_INIT_PRIO_MODULE_GPIO  85
#elif defined(CONFIG_BOARD_NUCLEO_H743ZI)
    #define APP_INIT_PRIO_MODULE_GPIO  85
    #define ENABLE_HIGH_PERF_MODULE    1
#else
    #define ENABLE_HIGH_PERF_MODULE    0
#endif
```

---

## Step 6: Update CI/CD Configuration

### 6.1 Update GitHub Actions

Edit `.github/workflows/ci.yml`, add new boards to the build matrix:

```yaml
jobs:
  build:
    strategy:
      matrix:
        board:
          - nucleo_l4r5zi
          - nucleo_h743zi    # New board
          - custom_board
```

### 6.2 Update GitLab CI

Edit `.gitlab-ci.yml`:

```yaml
build_job:
  variables:
    BOARD: "nucleo_h743zi"  # Add or modify board
```

### 6.3 Add Board Testing

If hardware-in-the-loop (HIL) testing is available, update test configuration:

```yaml
test:
  stage: test
  variables:
    TEST_BOARD: "nucleo_h743zi"
  script:
    - west test -b ${TEST_BOARD}
```

---

## Step 7: Verification and Debugging

### 7.1 Build Verification

```bash
# Clean build
west build -t pristine

# Build for new board
west build -b <board_name> .

# Check for build errors
# Check if memory usage is within limits
```

### 7.2 View Memory Usage

```bash
# View memory map
arm-none-eabi-nm --size-sort build/zephyr/zephyr.elf
arm-none-eabi-size build/zephyr/zephyr.elf
```

### 7.3 Flash to Device

```bash
# Flash to new board
west flash

# If using different debugger, may need adjustment
west flash --runner jlink
west flash --runner pyocd
```

### 7.4 Serial Console Monitoring

```bash
# Open serial console
west console

# Specify serial port
west console --port COM3    # Windows
west console --port /dev/ttyUSB0  # Linux
```

### 7.5 Debugging Common Issues

| Issue | Possible Cause | Solution |
|-------|----------------|----------|
| Build failure | Board not recognized | Check if `BOARD` name is correct |
| Link error | RAM/Flash insufficient | Reduce heap/stack sizes, disable non-essential features |
| Abnormal runtime | Device tree misconfiguration | Check `.overlay` file |
| No serial output | UART not enabled | Check `CONFIG_UART_CONSOLE` and device tree |
| Flash failure | Debugger mismatch | Adjust `--runner` parameter |

---

## FAQ

### Q1: How to find the exact board name?

```bash
# In Zephyr directory
cd %ZEPHYR_BASE%
west boards | grep <keyword>
```

### Q2: Custom board not recognized?

Ensure:
1. `BOARD_ROOT` is enabled in `CMakeLists.txt`
2. Board directory structure is correct
3. `identifier` in `.yaml` file matches board name

### Q3: How to migrate existing configuration to a new board?

1. Copy current `prj.conf` and `.overlay` files
2. Adjust memory configuration based on new board specifications
3. Modify peripheral pin definitions in device tree
4. Gradually enable features and verify stability

### Q4: How to manage configurations for multiple boards?

Recommended approach using configuration combinations:

```
prj.conf                    # Common base configuration
prj_common.overlay          # Common device tree overlay
prj_<board_name>.conf       # Board-specific configuration
boards/<board_name>.overlay # Board-specific overlay
```

Build with:

```bash
west build -b <board_name> -DCONF_FILE="prj.conf;prj_<board_name>.conf" .
```

### Q5: How to verify board migration is successful?

Complete the following checklist:

- [ ] Build completed without errors
- [ ] Memory usage within board limits
- [ ] Flash succeeded
- [ ] Serial output present
- [ ] Basic functions (GPIO, UART) working
- [ ] Project-specific functions working
- [ ] CI/CD passed

---

## Appendix A: Complete Migration Example

### A.1 Scenario: Migrating from Nucleo L4R5ZI to Nucleo H743ZI

**Background**: Project originally used `nucleo_l4r5zi` (640KB RAM, 2MB Flash), now migrating to `nucleo_h743zi` (1MB RAM, 2MB Flash).

#### Step 1: Export Old Board Device Tree

```bash
# Build with old board
west build -b nucleo_l4r5zi .

# Export full device tree
cat build/zephyr/zephyr.dts > nucleo_l4r5zi_full.dts

# View key information
cat nucleo_l4r5zi_full.dts | grep -A 3 "memory@"
cat nucleo_l4r5zi_full.dts | grep "zephyr,sram"
```

#### Step 2: Review H743ZI Specifications

| Parameter | L4R5ZI | H743ZI |
|-----------|--------|--------|
| SRAM | 640KB | 1MB (1024KB) |
| Flash | 2MB | 2MB |
| SRAM Base | 0x20000000 | 0x24000000 |
| Flash Base | 0x08000000 | 0x08000000 |

#### Step 3: Create New Board Overlay

```bash
# Copy template
cp boards/overlay.dts boards/nucleo_h743zi.overlay
```

Edit `boards/nucleo_h743zi.overlay`:

```dts
/* boards/nucleo_h743zi.overlay */
/* STM32H743ZI: 1MB SRAM, 2MB Flash */

/* Delete SoC default memory nodes */
/delete-node/ &sram0;
/delete-node/ &sram1;
/delete-node/ &sram2;
/delete-node/ &sram3;

/ {
    /* Define 1MB contiguous SRAM */
    sram0: memory@24000000 {
        device_type = "memory";
        reg = <0x24000000 0x100000>;  /* 1MB = 0x100000 */
    };

    zephyr,sram = &sram0;
};

/* Flash partitions (2MB) */
&flash0 {
    #address-cells = <1>;
    #size-cells = <1>;
    ranges = <0x0 0x08000000 DT_SIZE_K(2048)>;
    partitions {
        ranges;
        slot0_partition: partition@0 {
            label = "image-0";
            reg = <0x0 0x001C0000>;  /* ~1.75MB for app */
        };
        storage_partition: partition@1c0000 {
            label = "storage";
            reg = <0x001C0000 0x00040000>;  /* 256KB for settings */
        };
    };
};

& {
    chosen {
        zephyr,settings-partition = &storage_partition;
        zephyr,code-partition = &slot0_partition;
    };
};

/* Enable peripherals */
&uart0 {
    status = "okay";
    current-speed = <115200>;
};

&gpio0 {
    status = "okay";
};
```

#### Step 4: Create Board-Specific Configuration File

Create `prj_nucleo_h743zi.conf`:

```kconfig
# prj_nucleo_h743zi.conf
# H743ZI has stronger performance and larger RAM, can increase resources

# Increase Heap to 128KB
CONFIG_HEAP_MEM_POOL_SIZE=131072

# Increase main stack and workqueue stack
CONFIG_MAIN_STACK_SIZE=8192
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=4096
CONFIG_EVENT_DISPATCHER_STACK_SIZE=8192

# H7 series can have more detailed logging
CONFIG_SYS_LOG_LEVEL=4
CONFIG_LOG_DEFAULT_LEVEL=4
```

#### Step 5: Build Test

```bash
# Clean old build
west build -t pristine

# Build with new board and configuration
west build -b nucleo_h743zi . -- -DCONF_FILE="prj.conf;prj_nucleo_h743zi.conf"

# View memory usage
arm-none-eabi-size build/zephyr/zephyr.elf

# View device tree
cat build/zephyr/zephyr.dts | grep -A 3 "memory@"
```

#### Step 6: Flash Verification

```bash
# Flash
west flash

# Serial console
west console
```

### A.2 Scenario: Migrating from Nucleo L4R5ZI to nRF52840 DK

**Background**: Migrating from STM32 to Nordic nRF52840 (256KB RAM, 1MB Flash), more resource-constrained.

#### Step 1: Create Minimal Overlay

```dts
/* boards/nrf52840dk.overlay */
/* nRF52840: 256KB SRAM, 1MB Flash */

&sram0;

/ {
    sram0: memory@20000000 {
        device_type = "memory";
        reg = <0x20000000 0x40000>;  /* 256KB = 0x40000 */
    };

    zephyr,sram = &sram0;
};

/* Flash partitions (1MB) */
&flash0 {
    #address-cells = <1>;
    #size-cells = <1>;
    ranges = <0x0 0x00000000 DT_SIZE_K(1024)>;
    partitions {
        ranges;
        slot0_partition: partition@0 {
            label = "image-0";
            reg = <0x0 0x000C0000>;  /* 768KB for app */
        };
        storage_partition: partition@c0000 {
            label = "storage";
            reg = <0x000C0000 0x00040000>;  /* 256KB for settings */
        };
    };
};

& {
    chosen {
        zephyr,settings-partition = &storage_partition;
        zephyr,code-partition = &slot0_partition;
    };
};

&uart0 {
    status = "okay";
    current-speed = <115200>;
};
```

#### Step 2: Create Minimal Configuration

Create `prj_nrf52840.conf`:

```kconfig
# prj_nrf52840.conf
# nRF52840 has smaller RAM, need minimal configuration

# Reduce Heap to 32KB
CONFIG_HEAP_MEM_POOL_SIZE=32768

# Reduce stack sizes
CONFIG_MAIN_STACK_SIZE=2048
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=1024
CONFIG_EVENT_DISPATCHER_STACK_SIZE=2048

# Disable debug logging
CONFIG_LOG=n
CONFIG_SYS_LOG_LEVEL=0

# Optional: Disable complex services
# CONFIG_THREAD_IPC_SERVICE=n
# CONFIG_SYS_WATCHDOG=n
```

#### Step 3: Build Test

```bash
west build -t pristine
west build -b nrf52840dk_nrf52840 . -- -DCONF_FILE="prj.conf;prj_nrf52840.conf"

# Check RAM usage
arm-none-eabi-size build/zephyr/zephyr.elf
```

---

## Appendix B: Common MCU Memory Parameters Quick Reference

| MCU Model | SRAM Size | SRAM Base | Flash Size | Flash Base |
|-----------|-----------|-----------|------------|------------|
| STM32L4R5ZI | 640KB | 0x20000000 | 2MB | 0x08000000 |
| STM32H743ZI | 1MB | 0x24000000 | 2MB | 0x08000000 |
| STM32F429ZI | 256KB | 0x20000000 | 2MB | 0x08000000 |
| STM32F103C8T6 | 20KB | 0x20000000 | 64KB | 0x08000000 |
| nRF52840 | 256KB | 0x20000000 | 1MB | 0x00000000 |
| nRF5340 | 512KB | 0x20000000 | 1MB | 0x00000000 |
| RP2040 | 264KB | 0x20000000 | 16MB (XIP) | 0x10000000 |
| ESP32-C3 | 400KB | 0x3FC80000 | 4MB | 0x42000000 |

---

## Reference Resources

- [Zephyr Board Documentation](https://docs.zephyrproject.org/latest/boards/index.html)
- [Zephyr Device Tree Guide](https://docs.zephyrproject.org/latest/build/dts/index.html)
- [Zephyr Build System](https://docs.zephyrproject.org/latest/build/west/build-flash-debug.html)
- [Device Tree and Memory Configuration Guide](../40-app-development/44-devicetree-memory-config.md)
- [Flash and Debug Quick Guide](../60-debugging/61-flash-debug-quickstart.md)

---

**Version**: 1.1.0
**Last Updated**: 2026-04-01
**Update Notes**: Added detailed DTS/overlay migration steps, complete practical examples, MCU memory parameter quick reference table
