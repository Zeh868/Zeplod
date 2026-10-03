> Language: [中文](../../zh-CN/50-测试与CI/54-看门狗测试指南.md) | **English**

# Hardware Watchdog Test Guide

## Problem Diagnosis

### Why Didn't I See Watchdog Restart the System During Testing?

**Cause:** Test code uses `WDT_MODE_SOFTWARE` (software mode) by default, which does not initialize hardware watchdog.

Even if device tree has `watchdog0 = &wdog0;` node, you need to:
1. Explicitly set mode to `WDT_MODE_HARDWARE` or `WDT_MODE_DUAL`
2. Run on real hardware (native_posix has no hardware watchdog)

## Testing Methods

### Method 1: Using Test Overlay Configuration (Recommended)

```bash
# Clean previous build
west build -t pristine

# Build tests with hardware watchdog configuration
west build -b mimxrt1050_fire tests/ \
  -- -DCONF_FILE="prj.conf;prj_test_watchdog.conf"

# Flash and monitor
west flash
west console
```

This enables:
- `CONFIG_SYS_WATCHDOG_DEFAULT_MODE=1` (hardware mode)
- `CONFIG_SYS_WATCHDOG_TIMEOUT_MS=3000` (3 second timeout)
- Debug log level

### Method 2: Using Main Application Configuration

Create `prj_watchdog_test.conf`:

```conf
CONFIG_SYS_WATCHDOG_DEFAULT_MODE=1
CONFIG_SYS_WATCHDOG_TIMEOUT_MS=3000
CONFIG_WATCHDOG=y
```

Then build main application:

```bash
west build -b mimxrt1050_fire . \
  -- -DCONF_FILE="prj.conf;prj_watchdog_test.conf"
west flash
west console
```

### Method 3: Explicit Configuration in Code

```c
wdt_config_t hw_config = {
    .mode = WDT_MODE_HARDWARE,
    .timeout_ms = 3000,
    .feed_margin_ms = 1000,
    .reset_on_expire = false,  /* Disable reset during testing */
    .name = "my_wdt"
};

sys_wdt_init(&hw_config);
sys_wdt_start();
```

## Verifying Hardware Watchdog is Working

### 1. Check Boot Logs

Successful hardware watchdog initialization outputs:

```
[INF] sys_wdt: Initializing watchdog...
[INF] sys_wdt: Hardware watchdog device found
[INF] sys_wdt: Hardware watchdog channel installed: 0
[INF] sys_wdt: Watchdog initialized: timeout=3000ms, mode=1
[INF] sys_wdt: Watchdog started
[INF] sys_wdt: Watchdog monitor thread started
```

If degraded to software mode, outputs:

```
[WRN] sys_wdt: Hardware watchdog device not found or not ready, using software mode
[INF] sys_wdt: Watchdog initialized: timeout=3000ms, mode=0
```

### 2. Test System Reset by Not Feeding

**Warning: This will cause continuous system restart!**

```conf
# prj_watchdog_reset.conf
CONFIG_SYS_WATCHDOG_DEFAULT_MODE=1
CONFIG_SYS_WATCHDOG_TIMEOUT_MS=2000
CONFIG_SYS_WATCHDOG_FORCE_RESET=y  # Enable reset
```

After building, if `sys_wdt_feed()` is not called, system will restart after timeout.

### 3. Test Feeding Prevents Reset

```c
/* Feed periodically in main loop */
while (1) {
    sys_wdt_feed();  /* Feed every 1-2 seconds */
    k_sleep(K_SECONDS(1));
}
```

## New Test Cases Added

`test_hardware_watchdog_init` test case has been added:

```c
ZTEST(sys_watchdog, test_hardware_watchdog_init) {
    wdt_config_t hw_config = {
        .mode = WDT_MODE_HARDWARE,
        .timeout_ms = CONFIG_SYS_WATCHDOG_TIMEOUT_MS,
        .feed_margin_ms = 1000,
        .pre_expire_callback = NULL,
        .callback_user_data = NULL,
        .reset_on_expire = false,
        .name = "hw_wdt_test"
    };

    int ret = sys_wdt_init(&hw_config);
    zassert_equal(ret, 0, "Hardware watchdog init should succeed");
    
    zassert_equal(sys_wdt_start(), 0, NULL);
    zassert_equal(sys_wdt_feed(), 0, NULL);
    zassert_equal(sys_wdt_stop(), 0, NULL);
}
```

## Kconfig Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `CONFIG_SYS_WATCHDOG_ENABLE` | bool | y | Enable watchdog service |
| `CONFIG_SYS_WATCHDOG_TIMEOUT_MS` | int | 5000 | Timeout in milliseconds |
| `CONFIG_SYS_WATCHDOG_DEFAULT_MODE` | int | 0 | Default mode: 0=software, 1=hardware, 2=dual |
| `CONFIG_SYS_WATCHDOG_FORCE_RESET` | bool | n | Force reset on expiry (disabled during development) |
| `CONFIG_WATCHDOG` | bool | y | Enable Zephyr watchdog driver |

## Frequently Asked Questions

### Q: Why does it show "Hardware watchdog device not found"?

**A:** Possible causes:
1. Using `native_posix` board (no hardware watchdog)
2. No `watchdog0` alias in device tree or `wdog0` node not enabled
3. `CONFIG_WATCHDOG=n`

### Q: How to confirm hardware watchdog is actually working?

**A:**
1. Check logs for "Hardware watchdog channel installed"
2. Stop feeding, observe if system resets after timeout (need `FORCE_RESET` enabled)
3. Use multimeter to measure watchdog pin (if applicable)

### Q: System keeps restarting during test, what to do?

**A:**
1. Hold reset button while flashing
2. Re-flash using `west flash`
3. Immediately modify configuration, set `CONFIG_SYS_WATCHDOG_FORCE_RESET=n`

## Production Environment Recommendations

```conf
# Production configuration example
CONFIG_SYS_WATCHDOG_DEFAULT_MODE=1  # Hardware mode
CONFIG_SYS_WATCHDOG_TIMEOUT_MS=5000  # 5 second timeout
CONFIG_SYS_WATCHDOG_FORCE_RESET=y    # Enable reset
```

Ensure main loop or critical tasks periodically call `sys_wdt_feed()`.
