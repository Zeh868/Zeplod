> Language: [中文](../../zh-CN/40-应用开发/43-配置方案对比指南.md) | **English**

# Configuration Scheme Comparison Guide

This document compares memory usage and applicable scenarios of four configuration schemes.

---

## 📊 Configuration Scheme Comparison Table

| Config Item | **prj.conf** (Standard) | **conf/profiles/balanced.conf** (Balanced) | **conf/profiles/minimal.conf** (Minimal) | **conf/profiles/tiny.conf** (Tiny) |
|-------------|-------------------------|------------------------------|---------------------------|--------------------------|
| **Target SRAM** | ≥ 256KB | 64-128KB | 32-64KB | **≤ 32KB** |
| **Framework Usage** | ~190KB | ~40KB | ~18KB | **< 10KB** |
| **APP Available** | ~66KB+ | ~24-88KB | ~14-46KB | **> 22KB** |
| **Target Scenario** | Resource-rich MCU | Medium-resource MCU | Extremely constrained MCU | **Extremely resource-constrained MCU** |
| **Example Boards** | STM32H7/NRF52840 | STM32L4/NRF52832 | STM32F103C8T6 | **32KB SRAM systems** |

---

## 🔧 Core Configuration Comparison

### 1. Event System

| Config Item | Standard | Balanced | Minimal | Tiny |
|-------------|----------|----------|---------|------|
| `CONFIG_EVENT_MAX_TYPES` | 256 | 128 | 64 | **8** |
| `CONFIG_EVENT_MAX_SUBSCRIBERS` | 16 | 8 | 4 | **1** |
| `CONFIG_EVENT_QUEUE_SIZE` | 64 | 32 | 16 | **4** |
| `CONFIG_EVENT_DISPATCHER_STACK_SIZE` | 2048 | 1536 | 1024 | **256** |
| **Memory Usage** | ~73KB | ~36KB | ~18KB | **~1KB** |

### 2. System Memory Pool

| Config Item | Standard | Balanced | Minimal | Tiny |
|-------------|----------|----------|---------|------|
| `CONFIG_SYS_MEMORY_ENABLE` | y | y | y | **n** |
| `CONFIG_SYS_MEMORY_POOL_SIZE` | 8192 | 4096 | 4096 | **0** |
| Memory tracking | 256 entries | 64 entries | Disabled | **Disabled** |
| **Memory Usage** | ~38KB | ~12KB | ~4KB | **0** |

### 3. Stack Configuration

| Config Item | Standard | Balanced | Minimal | Tiny |
|-------------|----------|----------|---------|------|
| `CONFIG_MAIN_STACK_SIZE` | 4096 | 3072 | 2048 | **512** |
| `CONFIG_ISR_STACK_SIZE` | 2048 | 2048 | 1024 | **512** |
| `CONFIG_IDLE_STACK_SIZE` | 320 | 320 | 256 | **128** |
| `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE` | 2048 | 1536 | 1024 | **256** |
| `CONFIG_EVENT_DISPATCHER_STACK_SIZE` | 2048 | 1536 | 1024 | **256** |

### 4. Heap Configuration

| Config Item | Standard | Balanced | Minimal | Tiny |
|-------------|----------|----------|---------|------|
| `CONFIG_HEAP_MEM_POOL_SIZE` | 65536 | 16384 | 4096 | **1024** |
| `CONFIG_KERNEL_MEM_POOL` | y | y | y | **y** |

### 5. System Services

| Config Item | Standard | Balanced | Minimal | Tiny |
|-------------|----------|----------|---------|------|
| `CONFIG_SYS_LOG_ENABLE` | y | y | n | **n** |
| `CONFIG_SYS_TIMER_ENABLE` | y | y | y | **n** |
| `CONFIG_SYS_WATCHDOG_ENABLE` | y | y | n | **n** |
| `CONFIG_WATCHDOG` | y | y | n | **n** |

### 6. Other Configuration

| Config Item | Standard | Balanced | Minimal | Tiny |
|-------------|----------|----------|---------|------|
| `CONFIG_SHELL` | ✓ | ✓ | ✗ | **✗** |
| `CONFIG_CONSOLE` | ✓ | ✓ | ✗ | **✗** |
| `CONFIG_SERIAL` | ✓ | ✓ | ✗ | **✗** |
| `CONFIG_PRINTK` | ✓ | ✓ | ✓ | **✗** |
| `CONFIG_LOG` | ✓ | ✓ | ✗ | **✗** |
| `CONFIG_ASSERT` | ✓ | ✓ | ✗ | **✗** |
| `CONFIG_MAX_MODULES` | 16 | 12 | 8 | **2** |
| `CONFIG_APP_KV_ENABLE` | y | y | n | **n** |
| `CONFIG_EXAMPLE_MODULE_A_ENABLE` | y | y | n | **n** |
| `CONFIG_EXAMPLE_MODULE_B_ENABLE` | y | n | n | **n** |

---

## 🎯 Detailed Memory Usage Comparison

### Standard (~190KB)

> ⚠️ **Note**: Standard event system uses significant memory (~73KB), suitable for MCUs with SRAM ≥ 256KB

```
Event System:        73.0KB  ████████████████████████████████████████
Memory Pool:         38.0KB  █████████████████████████████
Heap:                64.0KB  █████████████████████████████████████████████
Timer:               35.0KB  ████████████████████████
Logging System:       8.0KB  ██████
Module Manager:      10.0KB  ████████
Application:          2.0KB  ██
Kernel Stack/Other:  10.0KB  ████████
────────────────────────────────
Total:            ~190KB
```

### Balanced (~40KB)

```
Event System:        36.0KB  ███████████████████████████████
Memory Pool:         12.0KB  ████████████
Heap:                16.0KB  ████████████████
Timer:               16.0KB  ████████████████
Logging System:       8.0KB  ████████
Module Manager:       7.5KB  ███████
Application:          2.0KB  ██
Kernel Stack/Other:   6.5KB  ██████
────────────────────────────────
Total:           ~40KB
```

### Minimal (~18KB)

```
Event System:        18.0KB  ████████████████████
Memory Pool:          4.0KB  ████████
Heap:                 4.0KB  ████████
Timer:                5.0KB  ██████████
Logging System:       2.0KB  ████
Module Manager:       5.0KB  ██████████
Application:          1.0KB  ██
Kernel Stack/Other:   3.0KB  ██████
────────────────────────────────
Total:           ~18KB  ✅
```

### Tiny (<10KB) ⭐ Recommended for 32KB SRAM systems

```
Event System:         1.0KB  ██
Memory Pool:          0.0KB
Heap:                 1.0KB  ██
Timer:                0.0KB
Logging System:       0.0KB
Module Manager:       0.5KB  █
Application:          0.5KB  █
ISR Stack:            0.5KB  █
Main Thread Stack:     0.5KB  █
Dispatcher Stack:     0.25KB
Idle Thread Stack:    0.125KB
Kernel Other:         1.0KB  ██
────────────────────────────────
Total:           ~5-6KB  ✅✅✅
```

**Tiny version memory allocation (32KB SRAM system)**:
- Framework usage: **< 10KB**
- APP available: **> 22KB**

---

## 📝 Usage

### Build Standard (default)

```bash
west build -b mimxrt1050_fire/mimxrt1052/qspi .
# Or explicit
west build -b mimxrt1050_fire/mimxrt1052/qspi . -DCONF_FILE="prj.conf"
```

### Build Balanced

```bash
west build -b mimxrt1050_fire/mimxrt1052/qspi . -DCONF_FILE="prj.conf;conf/profiles/balanced.conf"
```

### Build Minimal

```bash
west build -b mimxrt1050_fire/mimxrt1052/qspi . -DCONF_FILE="prj.conf;conf/profiles/minimal.conf"
```

### Build Tiny ⭐ 32KB SRAM systems

```bash
west build -b mimxrt1050_fire/mimxrt1052/qspi . -DCONF_FILE="prj.conf;conf/profiles/tiny.conf" --pristine
```

> **Note**: Tiny version suitable for systems with only 32KB SRAM, framework usage < 10KB, reserving 22KB+ for APP modules.

### Verify Memory Usage

```bash
# View memory usage report after build
west build -t mem_report

# Or view map file
cat build/zephyr/zephyr.map | grep -A 5 "Memory Configuration"

# View BSS segment large objects
cat build/zephyr/zephyr.map | grep "\.bss\." | grep -v "0x0"
```

---

## ⚠️ Feature Limitations

### Tiny Version Limitations (conf/profiles/tiny.conf)

When using `conf/profiles/tiny.conf`, the following features are **unavailable**:

| Feature | Status | Description |
|---------|--------|-------------|
| Shell | ❌ Disabled | No interactive debugging |
| Console | ❌ Disabled | No console output |
| Serial | ❌ Disabled | No serial driver |
| Printk | ❌ Disabled | No print output |
| LOG | ❌ Disabled | No logging system |
| Assert | ❌ Disabled | No assertion checking |
| Watchdog | ❌ Disabled | No watchdog protection |
| Timer Service | ❌ Disabled | No software timers |
| Memory Pool | ❌ Disabled | No sys_memory |
| KV Storage | ❌ Disabled | No key-value storage |
| Example Modules | ❌ Disabled | No example code |

**Event System Limitations**:

| Config Item | Standard | Tiny |
|-------------|----------|------|
| Event type count | 256 | **8** |
| Subscribers per type | 16 | **1** |
| Event queue depth | 64 | **4** |
| Max modules | 16 | **2** |

**Stack Size Limitations**:

| Stack Type | Standard | Tiny |
|------------|----------|------|
| Main thread stack | 4096 | **512** |
| ISR stack | 2048 | **512** |
| Idle thread stack | 320 | **128** |
| Dispatcher stack | 2048 | **256** |
| Workqueue stack | 2048 | **256** |

### Minimal Version Limitations

When using `conf/profiles/minimal.conf`, the following features are **unavailable**:

- ❌ Shell interactive commands
- ❌ Memory leak tracking
- ❌ Event/dispatcher statistics
- ❌ Log dump commands
- ❌ Watchdog protection
- ❌ KV storage
- ❌ Example modules A/B
- ❌ Log color output
- ❌ Assertion checking

### Balanced Version Limitations

When using `conf/profiles/balanced.conf`, the following features are **limited**:

- ⚠️ Max 128 event types (Standard 256)
- ⚠️ Max 8 subscribers per type (Standard 16)
- ⚠️ Memory pool only 2 (Standard 4)
- ⚠️ Max 16 timers (Standard 32)
- ⚠️ No runtime statistics (`APP_ENABLE_STATS=n`)
- ⚠️ Watchdog software mode only

---

## 🔍 How to Choose

### Choose Standard When

- ✅ MCU SRAM ≥ 256KB
- ✅ Need complete debugging features
- ✅ Need hardware watchdog
- ✅ Need memory leak tracking
- ✅ Need Shell interactive debugging
- ✅ Event types > 128

### Choose Balanced When

- ✅ MCU SRAM 64-128KB
- ✅ Need basic debugging features
- ✅ Software watchdog sufficient
- ✅ Event types ≤ 128
- ✅ Subscribers ≤ 8 per type
- ✅ Need Shell but can give up statistics

### Choose Minimal When

- ✅ MCU SRAM 32-64KB
- ✅ Production environment (no debugging)
- ✅ Event types ≤ 64
- ✅ Subscribers ≤ 4 per type
- ✅ No interactive debugging
- ✅ Resource constrained

### Choose Tiny When ⭐ Recommended

- ✅ **MCU SRAM ≤ 32KB**
- ✅ **Need to reserve ≥ 22KB for APP modules**
- ✅ Production environment (completely no debugging)
- ✅ Event types ≤ 8
- ✅ Subscribers ≤ 1 per type
- ✅ No logging, Shell, watchdog, timer
- ✅ Minimal event-driven architecture

---

## 🛠️ Custom Configuration

For further adjustment, create your own overlay config file:

```bash
# Copy template
cp conf/profiles/tiny.conf conf/profiles/custom.conf

# Edit config
vim conf/profiles/custom.conf

# Build with it
west build -b mimxrt1050_fire/mimxrt1052/qspi . -DCONF_FILE="prj.conf;conf/profiles/custom.conf"
```

### Key Config Items Quick Reference

| Kconfig Option | Purpose | Minimum | Tiny | Minimal | Recommended |
|---------------|---------|---------|------|---------|-------------|
| `CONFIG_EVENT_MAX_TYPES` | Event type count | 4 | **8** | 64 | 128-256 |
| `CONFIG_EVENT_QUEUE_SIZE` | Event queue depth | 4 | **4** | 16 | 32-64 |
| `CONFIG_EVENT_MAX_SUBSCRIBERS` | Subscribers per type | 1 | **1** | 4 | 8-16 |
| `CONFIG_EVENT_DISPATCHER_STACK_SIZE` | Dispatcher stack | 256 | **256** | 1024 | 1024-2048 |
| `CONFIG_MAX_MODULES` | Max module count | 1 | **2** | 8 | 8-16 |
| `CONFIG_SYS_MEMORY_POOL_SIZE` | Memory pool size | 0 | **0** | 4096 | 4096-8192 |
| `CONFIG_HEAP_MEM_POOL_SIZE` | Heap size | 512 | **1024** | 4096 | 4096-16384 |
| `CONFIG_MAIN_STACK_SIZE` | Main thread stack | 512 | **512** | 2048 | 2048-4096 |
| `CONFIG_ISR_STACK_SIZE` | ISR stack | 512 | **512** | 1024 | 2048 |
| `CONFIG_IDLE_STACK_SIZE` | Idle stack | 128 | **128** | 256 | 320 |

---

## 📌 Tiny Configuration Optimization Tips

### 1. Conditional Compilation

In tiny config, the following modules automatically skip compilation:

- `sys_log.c` - When `CONFIG_SYS_LOG_ENABLE=n`
- `sys_memory.c` - When `CONFIG_SYS_MEMORY_ENABLE=n`
- `sys_watchdog.c` - When `CONFIG_SYS_WATCHDOG_ENABLE=n`
- `sys_timer.c` - When `CONFIG_SYS_TIMER_ENABLE=n`
- `example_module_*.c` - When `CONFIG_EXAMPLE_MODULE_*_ENABLE=n`

### 2. Stack Size Optimization

Stack size is the main contributor to memory usage; tiny config key optimization:

```properties
# Minimal viable stack config
CONFIG_MAIN_STACK_SIZE=512      # Main thread
CONFIG_ISR_STACK_SIZE=512       # Interrupt stack
CONFIG_IDLE_STACK_SIZE=128      # Idle thread
CONFIG_EVENT_DISPATCHER_STACK_SIZE=256  # Event dispatcher
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=256  # Work queue
```

### 3. Event System Optimization

Event system is the framework core; proper configuration significantly saves memory:

```properties
# Tiny config
CONFIG_EVENT_MAX_TYPES=8        # Only support 8 event types
CONFIG_EVENT_MAX_SUBSCRIBERS=1  # Only 1 subscriber per event type
CONFIG_EVENT_QUEUE_SIZE=4       # Queue depth only 4
```

### 4. Disable Debug Output

Completely disabling output saves significant code and data space:

```properties
CONFIG_PRINTK=n          # Disable print
CONFIG_CONSOLE=n         # Disable console
CONFIG_SERIAL=n          # Disable serial
CONFIG_LOG=n             # Disable log
CONFIG_ASSERT=n          # Disable assert
```

---

## 📌 Notes

1. **Config file overlay order**: `prj.conf` → `prj_xxx.conf` (latter overrides former)
2. **Kconfig range limits**: Some options have minimum limits, see `Kconfig` files
3. **Memory calculation**: Only includes static allocation, not Zephyr kernel and dynamic allocation
4. **Build verification**: Always `--pristine` rebuild after config changes
5. **Actual measurement**: Use `CONFIG_INIT_STACKS=y` to view stack usage
6. **Stack overflow risk**: Tiny stack config may cause complex callbacks to overflow, test carefully

---

## 🔧 Tiny Configuration Tuning Guide

### Stack Size Adjustment Principles

| Callback Complexity | Recommended Dispatcher Stack | Recommended Main Stack |
|-------------------|------------------------------|----------------------|
| Minimal (no nested calls) | 256 | 512 |
| Simple (few functions) | 512 | 1024 |
| Medium (with logging) | 1024 | 2048 |
| Complex (multi-layer nested) | 2048 | 4096 |

### Event Queue Depth Selection

| Scenario | Recommended Depth | Description |
|----------|-------------------|-------------|
| Sparse events (<10/sec) | 4 | Tiny config |
| Medium events (10-100/sec) | 8-16 | Minimal config |
| Dense events (>100/sec) | 32-64 | Standard config |

---

*Document version: 1.2*
*Update date: 2026-04-13*
*Added: Tiny version detailed config description, conditional compilation mechanism, stack optimization tips*
