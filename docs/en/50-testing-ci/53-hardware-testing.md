> Language: [中文](../../zh-CN/50-测试与CI/53-硬件测试运行指南.md) | **English**

# Hardware Testing Guide

This document describes how to compile and run test cases on **real hardware**, rather than only in the `native_sim` emulator.

---

## 1. Why Run Tests on Hardware?

| Test Type | Target Platform | Purpose |
|-----------|----------------|---------|
| **Unit Tests (native_sim)** | PC (Linux/WSL) | Quickly verify core logic, event system, module manager, and other pure software functions |
| **Hardware Tests (real boards)** | ARM Cortex-M / RISC-V, etc. | Verify peripheral drivers, interrupts, timers, watchdog, and other hardware-related functions |
| **Integration Tests** | Real boards | Verify module collaboration, IPC communication, multi-threading concurrency, and other system-level functions |

**Recommended Flow**: First run unit tests on `native_sim` → Then flash to hardware to verify peripheral-related functions.

---

## 2. Building Test Firmware

### 2.1 Selecting Target Board

Boards supported by this repository can be listed with:

```bash
# List all supported boards
west boards
```

Common board examples:
- `nucleo_f429zi` (STM32F429)
- `nucleo_f767zi` (STM32F767)
- `disco_l475_iot1` (STM32L475)
- `native_sim` (Linux/WSL host simulation)

### 2.2 Building Test Firmware

```bash
# Build tests for target board (example: nucleo_f429zi)
west build -b nucleo_f429zi tests/ --build-dir build_tests_hw

# Or use other boards
west build -b nucleo_f767zi tests/ --build-dir build_tests_hw
west build -b disco_l475_iot1 tests/ --build-dir build_tests_hw
```

**Build Artifacts**:
- `build_tests_hw/zephyr/zephyr.elf` - ELF file (for debugging)
- `build_tests_hw/zephyr/zephyr.bin` - Binary file (for flashing)
- `build_tests_hw/zephyr/zephyr.hex` - Intel HEX format

### 2.3 Cleaning Build

```bash
west build -t pristine --build-dir build_tests_hw
```

---

## 3. Flashing to Hardware

### 3.1 Using west flash (Recommended)

```bash
# Flash to target board
west flash --build-dir build_tests_hw

# Specify programmer (if using ST-Link)
west flash --build-dir build_tests_hw --runner stlink

# Specify programmer (if using J-Link)
west flash --build-dir build_tests_hw --runner jlink
```

### 3.2 Using OpenOCD Manually

```bash
# Flash ELF using OpenOCD
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c "program build_tests_hw/zephyr/zephyr.elf verify reset exit"
```

### 3.3 Using IDE to Flash

If using Segger Embedded Studio, Keil, IAR, etc.:
1. Import `build_tests_hw/zephyr/zephyr.elf`
2. Use IDE's flash function to download firmware to target board

---

## 4. Monitoring Test Output

### 4.1 Using west console

```bash
# Monitor serial output (requires serial baud rate configuration first)
west console --build-dir build_tests_hw
```

### 4.2 Using Serial Tools

```bash
# Linux/macOS
screen /dev/ttyACM0 115200
picocom -b 115200 /dev/ttyACM0

# Windows (use PuTTY or Tera Term)
# Configure COM port, baud rate 115200, 8N1
```

### 4.3 Using pyOCD for Monitoring

```bash
# Install pyOCD
pip install pyocd

# Start RTT monitoring (no extra serial cable needed)
pyocd rtt --target stm32f429zitx
```

---

## 5. Interpreting Test Results

### 5.1 Success Output Example

```
*** Booting Zephyr OS build zephyr-v3.4.0 ***
Running TESTSUITE test_event_system
===================================================================
START - test_event_system_init
PASS - test_event_system_init in 0.001 seconds
===================================================================
START - test_event_register_type
PASS - test_event_register_type in 0.001 seconds
...
===================================================================
PROJECT EXEC SUCCESSFUL
```

### 5.2 Failure Output Example

```
*** Booting Zephyr OS build zephyr-v3.4.0 ***
Running TESTSUITE test_event_system
===================================================================
START - test_event_subscribe
FAIL - test_event_subscribe in 0.002 seconds
Assertion failed at test_event_system.c:85: event_subscribe(...) expected 0 but got -4
...
===================================================================
PROJECT EXEC FAILED
```

### 5.3 Common Failure Causes

| Error Type | Possible Cause | Solution |
|------------|----------------|---------|
| `CONFIG_* not defined` | Kconfig option not enabled | Check if `tests/prj.conf` includes required configuration |
| `Device not found` | Device tree doesn't configure that peripheral | Add board overlay or modify `tests/prj.conf` |
| `Stack overflow` | Thread stack size insufficient | Increase `CONFIG_MAIN_STACK_SIZE` |
| `Memory allocation failed` | Heap memory insufficient | Increase `CONFIG_HEAP_MEM_POOL_SIZE`; confirm 640KB overlay (§6.4) |
| `region RAM overflowed` | 192KB RAM or concurrency stress stacks | Use `tests/boards/nucleo_l4r5zi.overlay`; hardware: `prj.conf` only |
| Hardware-related test failures | Peripheral not connected or misconfigured | Check hardware connections, confirm device tree configuration |

---

## 6. Hardware-Specific Test Configuration

### 6.1 `tests/prj.conf` and overlays

| Overlay | Purpose |
| --- | --- |
| **`prj.conf` only** | **Hardware default** (slim; no concurrency stress) |
| `prj.conf;prj_concurrency_stress.conf` | Enable `CONFIG_ZTEST_CONCURRENCY_STRESS` |
| `prj.conf;prj_test_extensions.conf` | Full P0/P1 extensions (**native_sim CI**; not for hardware) |

Do not use `prj_test_extensions.conf` on boards with only 192KB `zephyr,sram` (link will fail).

### 6.2 Creating Board-Specific Configuration

Create dedicated configuration files for different boards:

```bash
# Create board-specific overlay config
cp tests/prj.conf tests/prj_nucleo_f429zi.conf

# Edit specific configuration
# Add board-specific options in tests/prj_nucleo_f429zi.conf
```

Build with specified configuration:

```bash
west build -b nucleo_f429zi tests/ \
  --build-dir build_tests_hw \
  -DCONF_FILE="tests/prj.conf;tests/prj_nucleo_f429zi.conf"
```

### 6.3 Device tree (main app `west build .`)

Root **`boards/nucleo_l4r5zi.overlay`** applies when building the main application from the repo root.

### 6.4 Unit tests `tests/` and 640KB SRAM (`nucleo_l4r5zi`)

For `west build … tests/`, the application root is **`tests/`** — root `boards/nucleo_l4r5zi.overlay` is **not** auto-merged. Use **`tests/boards/nucleo_l4r5zi.overlay`** (also appended in `tests/CMakeLists.txt` for `nucleo_l4r5zi`). After `west build -p always`, verify **RAM Region Size = 640 KB**.

### 6.5 Device Tree Overlay (other boards)

If you need to modify device tree configuration, create board-level overlay:

```bash
# Create board overlay
cat > boards/nucleo_f429zi.overlay << EOF
&usart2 {
    status = "okay";
    current-speed = <115200>;
};
EOF
```

---

## 7. Running Specific Test Cases

### 7.1 Enabling/Disabling Specific Tests

In test files, you can control which tests run via conditional compilation:

```c
/* Tests running only on hardware */
#ifdef CONFIG_HARDWARE_TEST
ZTEST(sys_watchdog, test_hardware_wdt) {
    /* Hardware watchdog test */
}
#endif

/* Tests running only on native_posix */
#ifndef CONFIG_HARDWARE_TEST
ZTEST(event_system, test_posix_specific) {
    /* PC-specific test */
}
#endif
```

### 7.2 Using Kconfig to Control Tests

Add options in `tests/Kconfig`:

```kconfig
config HARDWARE_TEST
    bool "Enable hardware-specific tests"
    default n
    depends on GPIO || UART || COUNTER

config TEST_WATCHDOG_HARDWARE
    bool "Test hardware watchdog"
    default y
    depends on HARDWARE_TEST
```

Enable in `tests/prj.conf`:

```kconfig
CONFIG_HARDWARE_TEST=y
CONFIG_TEST_WATCHDOG_HARDWARE=y
```

---

## 8. Debugging Tips

### 8.1 Using GDB Debugging

```bash
# Start GDB Server (example: ST-Link)
st-util

# Start GDB in another terminal
arm-none-eabi-gdb build_tests_hw/zephyr/zephyr.elf

# GDB commands
(gdb) target extended-remote :4242
(gdb) break test_event_system.c:85
(gdb) run
(gdb) print subscriber_id
(gdb) continue
```

### 8.2 Using Segger RTT

```bash
# Start JLinkRTTLogger
JLinkRTTLogger -Device STM32F429ZI -RTTChannel 0 -Speed 4000

# Or use pyOCD
pyocd rtt --target stm32f429zitx
```

### 8.3 Adding Debug Logs

Add detailed logs in test code:

```c
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(test_my_module);

ZTEST(my_module, test_something) {
    LOG_INF("Starting test_something");
    
    int ret = my_function();
    LOG_DBG("my_function returned %d", ret);
    
    zassert_equal(ret, 0, "my_function failed");
}
```

---

## 9. Automated Testing (CI/CD)

### 9.1 Running Hardware Tests in CI

If you have a CI hardware farm (such as Renode, QEMU, or real boards), you can add in CI:

```yaml
# .github/workflows/ci.yml example
hardware-tests:
  runs-on: self-hosted
  steps:
    - uses: actions/checkout@v3
    
    - name: Build tests for hardware
      run: |
        west build -b nucleo_f429zi tests/ --build-dir build_tests_hw
    
    - name: Flash and run tests
      run: |
        west flash --build-dir build_tests_hw
        # Use script to capture serial output and parse results
        python scripts/capture_test_output.py
```

### 9.2 Using Twister to Run Tests

Zephyr provides `twister` tool for automated testing:

```bash
# Install twister
pip install pykwalify

# Run tests (automatically builds, flashes, collects results)
twister -T tests/ -p nucleo_f429zi -p nucleo_f767zi

# Generate report
twister -T tests/ -p nucleo_f429zi --report
```

---

## 10. Frequently Asked Questions

### Q1: Compilation errors with `undefined reference to`?

**A**: Check if `tests/CMakeLists.txt` includes all required source files. If test depends on certain modules, ensure they are added to `target_sources`.

### Q2: Board unresponsive after flashing?

**A**:
1. Check if serial connection is normal
2. Confirm baud rate configuration is correct (usually 115200)
3. Check if UART is enabled in device tree configuration
4. Try using `west flash --runner stlink` to explicitly specify programmer

### Q3: Tests run but all cases fail?

**A**:
1. Check if `tests/prj.conf` enables required Kconfig options
2. View error messages in serial output
3. Confirm hardware connections are correct (such as GPIO pins, peripheral power, etc.)

### Q4: How to run same test on multiple board types?

**A**: Create separate build directories for each board type:

```bash
west build -b nucleo_f429zi tests/ --build-dir build_f429
west build -b nucleo_f767zi tests/ --build-dir build_f767
west flash --build-dir build_f429
west flash --build-dir build_f767
```

---

## 11. Reference Resources

- [Zephyr Testing Official Documentation](https://docs.zephyrproject.org/latest/develop/test/ztest.html)
- [Zephyr Twister Test Framework](https://docs.zephyrproject.org/latest/develop/test/twister.html)
- [Zephyr Device Tree Guide](https://docs.zephyrproject.org/latest/build/dts/intro.html)
- Repository [Unit Testing and CI Guide.md](51-unit-testing-ci.md)
- Repository [Board Porting Guide.md](../10-environment-build/13-board-porting-guide.md)
- Repository [Flashing and Debugging Quick Guide.md](../60-debugging/61-flash-debug-quickstart.md)
