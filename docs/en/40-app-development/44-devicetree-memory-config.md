> Language: [中文](../../zh-CN/40-应用开发/44-设备树与内存配置手册.md) | **English**

# Device Tree and Memory Configuration Manual

This manual explains how **Devicetree Overlays take effect**, how **main RAM (`zephyr,sram`) affects linking and heap**, and typical handling of **multiple non-contiguous memory blocks** in Zephyr, within this project (`zeplod`) and Zephyr's general mechanisms. Applicable to Zephyr 4.x; build system uses CMake-generated linker scripts.

**Reading tips**: If you haven't understood **`west build`**, `prj.conf` and **`ZEPHYR_BASE`**, first read the **[Documentation Index.md](../00-getting-started/02-doc-index.md)** and **[Standalone Application Build Guide.md](../10-environment-build/12-freestanding-app-build.md)**; this manual focuses on board-level and linking details, so read chapters as needed.

---

## Table of Contents

1. [Core Concepts](#1-core-concepts)
2. [How Overlay Files Are Discovered by Build System](#2-how-overlay-files-are-discovered-by-build-system)
3. [File Naming, Extensions and Directory Conventions](#3-file-naming-extensions-and-directory-conventions)
4. [Manually Specifying Overlay](#4-manually-specifying-overlay)
5. [Main RAM: `zephyr,sram` and Linker Script](#5-main-ram-zephyrsram-and-linker-script)
6. [This Project: STM32L4R5ZI and `app.overlay`](#6-this-projectstm32l4r5zi-and-appoverlay)
7. [Non-Contiguous or Multiple Physical RAM Blocks](#7-non-contiguous-or-multiple-physical-ram-blocks)
8. [Working with Kconfig](#8-working-with-kconfig)
9. [Verification and Debugging](#9-verification-and-debugging)
10. [FAQ](#10-faq)
11. [Configuration Examples](#11-configuration-examples) (including [11.8 Scattered Memory Example](#example-scattered-memory))
12. [References](#12-references)

---

## 1. Core Concepts

### 1.1 Board Device Tree (Board DTS)

- Each board has a **`BOARD.dts`** in Zephyr repository (e.g., `boards/st/nucleo_l4r5zi/nucleo_l4r5zi.dts`), composed from SoC's **`*.dtsi`** for on-chip peripherals, memory, `chosen` etc.
- **`chosen`**'s **`zephyr,sram`** points to main RAM node, used to generate **`CONFIG_SRAM_*`** and linker **`RAM`** region; **`zephyr,flash`** points to Flash.

### 1.2 Overlay

- **Overlay** is a fragment appended **on top of** board DTS, with identical DTS syntax, used to override or add/delete nodes without modifying Zephyr upstream board files.
- Merge order determined by build system; later-applied overlay can override earlier-appearing properties (specifics per Zephyr documentation and implementation).

### 1.3 Application Configuration Directory

- Default **`APPLICATION_CONFIG_DIR`** equals **application source root** (directory containing `CMakeLists.txt`, `prj.conf`).
- If using Sysbuild etc. modified config directory, all paths below follow actual config directory.

---

## 2. How Overlay Files Are Discovered by Build System

Zephyr fills **`DTC_OVERLAY_FILE`** via CMake module **`configuration_files.cmake`**. Key logic (when not pre-set via command line):

1. **`${APPLICATION_CONFIG_DIR}/socs/`**
   Search for matching overlay by SoC qualified name (with `QUALIFIERS` etc.).

2. **`${APPLICATION_CONFIG_DIR}/boards/`**
   Search for **`<board>.overlay`** or **`shortened`** naming by **current board name** (and optional SoC qualifier; multi-SoC boards require naming with SoC suffix, see Zephyr error hints).

3. If still **no** `DTC_OVERLAY_FILE`:
   Search root **`APPLICATION_CONFIG_DIR`** again by board name rule for `*.overlay`.

4. If still none:
   Specifically search for **`app.overlay`** in root directory (can use with `FILE_SUFFIX` etc. variants).

**Conclusion**: Not "any path in project works"; must conform to **directory + naming + extension** rules, or **explicitly pass variables** (see below).

Build log shows like:

```text
-- Found devicetree overlay: D:/.../zeplod/app.overlay
```

---

## 3. File Naming, Extensions and Directory Conventions

### 3.1 Auto-discovery Only Recognizes `.overlay`

- Zephyr's **`zephyr_file(CONF_FILES ... DTS ...)`** for DTS-type configs only collects **`.overlay`** suffix (see explanation in `extensions.cmake`).
- If file named **`boards/overlay.dts`**, it will **not** automatically participate in merge as overlay, unless explicitly specified via **`DTC_OVERLAY_FILE`** / **`EXTRA_DTC_OVERLAY_FILE`**.

### 3.2 Recommended Approach

| Purpose | Recommended File |
|---------|------------------|
| Project-wide default, board-agnostic generic overlay | Application root **`app.overlay`** |
| Board-specific only | **`boards/<board>.overlay`** or SoC subdirectory per Zephyr rules |
| Temporary experiment | `west build ... -- -DDTC_OVERLAY_FILE=path` |

### 3.3 `/delete-node/` Syntax Position

- **`/delete-node/ &label;`** must be written at **overlay file top level**, **cannot** be inside `/ { ... }` node, otherwise Devicetree parsing errors (`expected node name` etc.).

---

## 4. Manually Specifying Overlay

### 4.1 `DTC_OVERLAY_FILE`

- Set **`DTC_OVERLAY_FILE`** in CMake or `west build` to **one or more** overlay paths (semicolon or list, depending on environment).
- Once set, **stops** walking auto-search logic above (per Zephyr current implementation; if need to stack, see below).

### 4.2 `EXTRA_DTC_OVERLAY_FILE`

- Merge **after** auto or manually specified overlay, suitable for "adding one more overlay layer" without modifying original `DTC_OVERLAY_FILE` list.

### 4.3 With Sysbuild

- In multi-image projects, variables may come from Sysbuild's **global/local** passing; if overlay not taking effect, check if `APPLICATION_CONFIG_DIR` and `zephyr_get(... SYSBUILD ...)` are overriding.

---

## 5. Main RAM: `zephyr,sram` and Linker Script

- **`chosen { zephyr,sram = &sram0; }`** points to **`memory`** node whose **`reg`** `base address + length` determines:
  - Kconfig **`CONFIG_SRAM_BASE_ADDRESS`**, **`CONFIG_SRAM_SIZE`** (usually in KB);
  - Linker script **`RAM`** region (`.data`, `.bss`, default kernel heap, most stacks, etc.).
- Zephyr 4.x linker scripts generated by **CMake/template**, **do not** follow old tutorials custom **`INCLUDE linker-base.ld`** with path relative to build directory, unless fully following current Zephyr's `HAVE_CUSTOM_LINKER_SCRIPT` and template requirements.

---

## 6. This Project: STM32L4R5ZI and `app.overlay`

### 6.1 SoC Default DTS Split

STM32L4R5 series in Zephyr DTS often splits on-chip SRAM into multiple **`memory`** nodes (e.g., only `sram0` as `zephyr,sram`'s 192KB, rest as `zephyr,memory-region` etc.). **If only using 192KB main region**, with large **`CONFIG_HEAP_MEM_POOL_SIZE`** and multiple thread stacks, **`RAM` region** may fail linking (`region RAM overflowed`).

### 6.2 This Repository's Approach

In **`app.overlay`**:

- **Top level** `/delete-node/` to delete SoC default **`sram0`**, **`sram1`**, **`sram2`** nodes;
- Redefine **`sram0`**: **640 KiB (`0xA0000`)** continuous region starting at `0x20000000`, consistent with RM0432's SRAM1+SRAM2+SRAM3 address space continuous mapping;
- Board-level **`chosen`** still points to **`&sram0`**, no need to modify `nucleo_l4r5zi.dts`.

This makes linker **`RAM`** **640KB**, consistent with **`west build`** end **Memory region** output.

### 6.3 Relationship with `conf/profiles/balanced.conf`

- **`conf/profiles/balanced.conf`** is **illustrative** fragment (comments), usable with **`prj.conf`**; **heap size** etc. still configured by **`CONFIG_HEAP_MEM_POOL_SIZE`** etc. in **`prj.conf`**.

---

## 7. Non-Contiguous or Multiple Physical RAM Blocks

### 7.1 What Devicetree Can Describe

- Multiple **`memory`** nodes can be defined in DTS, some with **`compatible = "zephyr,memory-region"`** and **`zephyr,memory-region = "name"`**, for DMA, peripheral drivers, or explicit linking segments.

### 7.2 Zephyr Won't "Auto-merge into One Heap"

- **`k_malloc`** etc. **default kernel heap** usually comes from one **linear address range** corresponding to **`zephyr,sram`** (allocated within that region by **`CONFIG_HEAP_MEM_POOL_SIZE`** etc.).
- **Devicetree only describes multiple RAM blocks, does not automatically combine scattered physical blocks into one unified dynamic allocation pool**.

### 7.3 Common Strategies

| Scenario | Approach |
|----------|---------|
| CPU-view **address continuous** (like this L4R5 main SRAM region) | Combine into one **`memory`** node (like this project's `app.overlay`) |
| Multiple **non-contiguous** segments | Main image only selects one as **`zephyr,sram`**; others use **multi-heap**, **mem_attr**, **linker script/relocation** etc. separately (**example see [11.8](#example-scattered-memory)**) |
| Only need segment for DMA | Separate **`memory-region`** + driver or application fixed buffer |

---

## 8. Working with Kconfig

- **`CONFIG_HEAP_MEM_POOL_SIZE`**: Kernel heap size, **should not exceed** actual available space in main `RAM` region after deducting `.bss`/stacks etc. (assess with map file).
- **`CONFIG_SRAM_SIZE`**, **`CONFIG_SRAM_BASE_ADDRESS`**: Generated from Devicetree, **do not** manually override in `prj.conf` unless clearly understanding consequences.
- Other stack sizes (like **`CONFIG_MAIN_STACK_SIZE`**, application module Kconfig) together with **total RAM** affect overflow.

---

## 9. Verification and Debugging

1. **Build log**
   - Confirm **`Found devicetree overlay: ...`** includes expected file.
   - **Memory region** table at link end: **`RAM`**'s **Region Size** matches expectation (e.g., 640 KB).

2. **Generated tree**
   - Check **`build/zephyr/zephyr.dts`** and **`include/generated/zephyr/devicetree_generated.h`**, confirm `sram` node and `reg`.

3. **Map file**
   - **`build/zephyr/zephyr.map`** analyze segment usage and heap location.

4. **After config changes**
   - If modifying overlay or `prj.conf`, recommend **`west build ... -p always`** or clean rebuild, avoid CMake cache causing old tree residue.

---

## 10. FAQ

**Q1: `boards/overlay.dts` written but not taking effect?**
Auto-discovery only recognizes **`.overlay`** (unless **`DTC_OVERLAY_FILE`** explicitly specified). Recommend renaming to **`boards/<board>.overlay`** or merging into **`app.overlay`**.

**Q2: Overlay `parse error` / `expected node name`?**
Check **`/delete-node/`** is at **file top level**; check misusing **`#include`** at unsupported positions (depends on Zephyr version and preprocessing flow).

**Q3: Still `region RAM overflowed`?**
Confirm overlay merged and **`RAM` region size increased**; then reduce `CONFIG_HEAP_MEM_POOL_SIZE`, thread stacks or disable unneeded modules.

**Q4: Can use custom `.ld` instead of overlay?**
Zephyr 4.x requires **`CONFIG_HAVE_CUSTOM_LINKER_SCRIPT=y`**, and script must conform to current kernel template; **prefer** describing `zephyr,sram` via Devicetree, consistent with official generated linking.

---

## 11. Configuration Examples

All example paths below use **application root** (directory with `CMakeLists.txt`, `prj.conf`) as current directory; commands work in **PowerShell** or **bash**, adjust path separators per system.

### 11.0 Example 0: 32KB SRAM Tiny Optimization (conf/profiles/tiny.conf)

**Scenario**: MCU with only 32KB SRAM, needs to reserve 22KB for APP modules, framework must be controlled within 10KB.

**`conf/profiles/tiny.conf`** (tiny config):

```properties
# =============================================================================
# Tiny Config - Target framework <10KB, reserve 22KB for APP (total 32KB SRAM)
# =============================================================================

# Disable all debugging features
CONFIG_SHELL=n
CONFIG_CONSOLE=n
CONFIG_SERIAL=n
CONFIG_UART_CONSOLE=n
CONFIG_PRINTK=n
CONFIG_LOG=n
CONFIG_ASSERT=n

# Enable kernel memory pool (Heap)
CONFIG_KERNEL_MEM_POOL=y

# Heap reduced to 1KB (only for event system internal dynamic allocation)
CONFIG_HEAP_MEM_POOL_SIZE=1024

# Thread stacks reduced to tiny
CONFIG_MAIN_STACK_SIZE=512
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=256
CONFIG_IDLE_STACK_SIZE=128
CONFIG_ISR_STACK_SIZE=512
CONFIG_EVENT_DISPATCHER_STACK_SIZE=256

# Disable static memory pool
CONFIG_SYS_MEMORY_POOL_SIZE=0
CONFIG_SYS_MEMORY_ENABLE=n

# Event System - tiny config
CONFIG_EVENT_SYSTEM=y
CONFIG_EVENT_QUEUE_SIZE=4
CONFIG_EVENT_MAX_SUBSCRIBERS=1
CONFIG_EVENT_MAX_TYPES=8      # Key: reduced from 256 to 8

# Module Manager - minimal
CONFIG_MODULE_MANAGER=y
CONFIG_MAX_MODULES=2

# System Services - all disabled
CONFIG_SYS_LOG_ENABLE=n
CONFIG_SYS_TIMER_ENABLE=n
CONFIG_SYS_WATCHDOG_ENABLE=n
CONFIG_WATCHDOG=n

# Application Features - all disabled
CONFIG_APP_KV_ENABLE=n
CONFIG_EXAMPLE_MODULE_A_ENABLE=n
CONFIG_EXAMPLE_MODULE_B_ENABLE=n

# Disable extra kernel features
CONFIG_TIMESLICING=n
CONFIG_SCHED_DUMB=y
CONFIG_WAITQ_DUMB=y

# Compilation optimization
CONFIG_SIZE_OPTIMIZATIONS=y
```

**Memory usage estimation**:

| Component | Config Value | Estimated Usage |
|------------|--------------|-----------------|
| Event System | 8 types × 1 subscriber | ~1 KB |
| Event Queue | 4 depth | ~128 B |
| Heap Pool | 1KB | 1 KB |
| Main Thread Stack | 512B | 512 B |
| ISR Stack | 512B | 512 B |
| Idle Thread Stack | 128B | 128 B |
| Dispatcher Stack | 256B | 256 B |
| Module Manager | 2 modules | ~200 B |
| Zephyr Kernel | base | ~1-2 KB |
| **Framework Total** | | **< 6 KB** |
| **APP Available** | | **> 26 KB** |

**Key optimization points**:

1. **Conditional compilation**: System services and example modules automatically skip compilation
2. **Stack extreme reduction**: All stack sizes reduced to minimum viable
3. **Event system streamlined**: Types, subscribers, queue all minimized
4. **Debug output disabled**: Completely off printk/console/serial

**Build**:

```bash
west build -b mimxrt1050_fire/mimxrt1052/qspi . -DCONF_FILE="prj.conf;conf/profiles/tiny.conf" --pristine
```

**Verify memory usage**:

```bash
# View memory usage report
west build -t mem_report

# View BSS segment details
cat build/zephyr/zephyr.map | grep "\.bss\." | grep -v "0x0"

# View noinit segment (stack and heap)
cat build/zephyr/zephyr.map | grep "\.noinit\."
```

---

### 11.1 Example A: Extend Main RAM (this repository's `app.overlay` + `prj.conf`)

**Scenario**: Nucleo **STM32L4R5ZI** default DTS only uses ~**192KB** as `zephyr,sram`, needs to match RM0432 with **640KB** continuous SRAM starting at `0x20000000`.

**`app.overlay`** (consistent with repository, entire block copyable):

```dts
/* STM32L4R5ZI: zephyr,sram expanded to 640KB continuous SRAM starting at 0x20000000 (see RM0432) */

 /delete-node/ &sram0;
 /delete-node/ &sram1;
 /delete-node/ &sram2;

 / {
 	sram0: memory@20000000 {
 		/* 640 KiB = 0xA0000 */
 		reg = <0x20000000 0xA0000>;
 	};
 };
```

**`prj.conf` fragment** (coordinate with main RAM, heap, threads; values adjustable):

```properties
# Kernel heap (k_malloc; used by event system etc.)
CONFIG_HEAP_MEM_POOL_SIZE=65536

# Main stack / idle stack (as needed)
CONFIG_MAIN_STACK_SIZE=1024
CONFIG_IDLE_STACK_SIZE=320
CONFIG_ISR_STACK_SIZE=2048

# Event dispatch thread stack
CONFIG_EVENT_DISPATCHER_STACK_SIZE=2048

# Thread IPC service (if enabled)
CONFIG_THREAD_IPC_SERVICE_STACK_SIZE=1280
```

**Build**:

```bash
west build -b nucleo_l4r5zi . -p always
```

**Expected**: Log shows `Found devicetree overlay: .../app.overlay`, link end **Memory region** **`RAM`** **Region Size** is **640 KB**.

---

### 11.2 Example B: Board-Specific Only——`boards/<board>.overlay`

**Scenario**: Same source built for multiple boards, only want **Nucleo L4R5** to expand SRAM, others unaffected.

Create **`boards/nucleo_l4r5zi.overlay`** in application directory with same **`/delete-node/`** + `sram0` node as Example A. Build specifying that board:

```bash
west build -b nucleo_l4r5zi .
```

Zephyr will match **`nucleo_l4r5zi.overlay`** under **`boards/`**. Once found and written to **`DTC_OVERLAY_FILE`**, **generally won't** fall back to root **`app.overlay`** auto-search, so **SRAM expansion should choose one**: either only **`boards/nucleo_l4r5zi.overlay`** or only **`app.overlay`**, avoid duplicate definition in two places. If must use both files simultaneously, use **`DTC_OVERLAY_FILE`** to explicitly list both paths (see Example C).

---

### 11.3 Example C: Command Line Specified Overlay (temporary experiment)

**Scenario**: Don't move files, temporarily specify an overlay; or merge multiple files.

**Single file** (Windows path note quotes):

```powershell
west build -b nucleo_l4r5zi . -- -DDTC_OVERLAY_FILE="D:/Code/3-Project/zeplod/my_ram.overlay"
```

**Multiple files** (CMake list, semicolon-separated, quote entire block):

```powershell
west build -b nucleo_l4r5zi . -- "-DDTC_OVERLAY_FILE=D:/proj/o1.overlay;D:/proj/o2.overlay"
```

> **Note**: Once **`DTC_OVERLAY_FILE`** set, generally **stops** auto-searching **`app.overlay`**; if still need `app.overlay`, include it in **`DTC_OVERLAY_FILE`** list.

---

### 11.4 Example D: Append After Auto Overlay——`EXTRA_DTC_OVERLAY_FILE`

**Scenario**: Keep **`app.overlay`** auto-discovery, add one more layer with only small changes (e.g., disable a UART, change `status`).

Create **`extra.overlay`**:

```dts
/* Example: disable a node (label per board-level DTS, needs consistent with zephyr.dts) */
&lpuart1 {
	status = "disabled";
};
```

**Build with append**:

```powershell
west build -b nucleo_l4r5zi . -- -DEXTRA_DTC_OVERLAY_FILE="D:/Code/3-Project/zeplod/extra.overlay"
```

Merge order: **first** `DTC_OVERLAY_FILE` (auto or manual), **then** `EXTRA_DTC_OVERLAY_FILE`.

---

### 11.5 Example E: Property Override Only (no `memory` node deletion)

**Scenario**: Don't change SRAM layout, only adjust **`chosen`** or peripheral status.

```dts
/ {
	chosen {
		zephyr,console = &usart1;
	};
};

&usart1 {
	current-speed = <921600>;
};
```

Save as **`app.overlay`** or board **`boards/<board>.overlay`** (no **`/delete-node/`** needed).

---

### 11.6 Example F: When Cannot Expand Main RAM——Save Memory via Kconfig

**Scenario**: Still using default **192KB** `zephyr,sram`, getting **`region RAM overflowed`**, and temporarily cannot modify DTS.

In **`prj.conf`** reduce as appropriate (values are examples only):

```properties
CONFIG_HEAP_MEM_POOL_SIZE=32768
CONFIG_MAIN_STACK_SIZE=1024
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=1024
CONFIG_EVENT_DISPATCHER_STACK_SIZE=1536
CONFIG_THREAD_IPC_SERVICE_STACK_SIZE=1024
```

Also tighten related items in the **[Project Configuration Guide.md](42-config-options.md)**; after changes, always run **`west build ... -p always`** and check the **map file** to confirm the remaining margin.

---

### 11.7 Example G: Using Illustrative Fragment `conf/profiles/balanced.conf`

This repository's **`conf/profiles/balanced.conf`** is **comment-type** illustrative file, **not** auto-loaded by Zephyr. To apply its heap/stack suggestions to build:

- **Copy** needed lines to **`prj.conf`**, or
- **Merge build**:

```bash
west build -b nucleo_l4r5zi . -- -DCONF_FILE="prj.conf;conf/profiles/balanced.conf"
```

(Permitted **`CONF_FILE`** multi-file merge syntax per Zephyr; if same key appears in multiple files, latter or merge rules prevail.)

---

<a id="example-scattered-memory"></a>

### 11.8 Example H: Multiple Scattered Memory Blocks (Non-Contiguous Addresses)

**Scenario**: Chip has two (or more) on-chip SRAM blocks, **physically non-contiguous** (hole or completely separated), for example:

- **Segment A**: **128 KB** starting at `0x20000000` (main RAM, kernel, default heap, stacks)
- **Segment B**: **64 KB** starting at `0x24000000` (separate bank, for DMA buffers, large buffers etc.)

**Cannot** combine like L4R5 main SRAM into one continuous `reg` from CPU view, **cannot** expect `k_malloc` to automatically allocate across both segments.

#### Devicetree (Example Overlay)

Below addresses and sizes are **fictional illustration**, actual must follow **datasheet**; node names, labels need to be compatible with board DTS (to avoid conflict with existing `&sram0` can use `delete-node` then rebuild).

```dts
/* Illustration: two non-contiguous SRAM segments; only Segment A as zephyr,sram */

 / {
 	chosen {
 		zephyr,sram = &sram_primary;
 	};

 	/* Primary RAM: linker RAM region, CONFIG_SRAM_*, default kernel heap all based on this segment */
 	sram_primary: memory@20000000 {
 		reg = <0x20000000 0x20000>; /* 128 KiB, example */
 	};

 	/* Second segment: named region, for driver/application/explicit linking use, not default k_malloc pool */
 	sram_aux: memory@24000000 {
 		compatible = "zephyr,memory-region", "mmio-sram";
 		zephyr,memory-region = "SRAM_AUX";
 		reg = <0x24000000 0x10000>; /* 64 KiB, example */
 	};
 };
```

**Key points**:

- **`chosen`**'s **`zephyr,sram`** can only point to **one** segment (here `&sram_primary`).
- **`sram_aux`** uses **`compatible = "zephyr,memory-region", "mmio-sram"`** and **`zephyr,memory-region`** string for easy access in Zephyr via **memory attributes/reserved regions** or application code by region.
- If SoC has default `memory` nodes, may need **`/delete-node/ &sram0;`** etc. then rebuild per above example, same rules as previous **`/delete-node/` must be top-level**.

#### Software Side (relationship with default heap)

| Requirement | Approach |
|-------------|----------|
| Default **`k_malloc`** / **`CONFIG_HEAP_MEM_POOL_SIZE`** | Only occupies one segment corresponding to **`zephyr,sram`**; **won't** automatically use second segment. |
| Independent small heap on second segment | See below **Method 1: `k_heap` + `DT_REG_ADDR`/`DT_REG_SIZE`**. |
| Multi-segment unified via **attribute selection** (e.g., external / non-cacheable) | See below **Method 2: Shared Multi-Heap**; or if SoC/board already `shared_multi_heap_add` at startup. |
| Compile-time put into specified region (**static buffer**) | See below **Method 3: Linker segment**; or **`k_heap`** treating entire second segment as pool (Method 1). |
| **DMA** | DTS **`zephyr,memory-region-flags`**, Cache/MPU and driver conventions; buffers preferably allocated from second segment. |

All C examples below assume overlay's second segment node **`sram_aux`** matches above DTS; if label differs, change **`DT_NODELABEL(sram_aux)`** to your **`nodelabel`**.

---

#### Method 1: Independent `k_heap` on Second Segment (Recommended for Application Start)

Approach: use **`DT_REG_ADDR` / `DT_REG_SIZE`** to read the second segment's physical address and length, pass the **entire segment** to **`k_heap_init`**, then use **`k_heap_alloc` / `k_heap_free`**, independently of the default **`k_malloc`** heap.

**`prj.conf`**: Generally **no need** to additionally enable `SHARED_MULTI_HEAP` (kernel **`k_heap`** already available with **`CONFIG_HEAP`** etc., per your current Zephyr `Kconfig`).

**Example code** (put in any `.c`, call once during `main` or `SYS_INIT`):

```c
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h> /* UINT_TO_POINTER */

#define SRAM_AUX_NODE DT_NODELABEL(sram_aux)

static struct k_heap sram_aux_heap;

static int sram_aux_heap_init(void)
{
	if (!DT_NODE_EXISTS(SRAM_AUX_NODE)) {
		return -ENODEV;
	}

	void *mem = UINT_TO_POINTER(DT_REG_ADDR(SRAM_AUX_NODE));
	size_t bytes = DT_REG_SIZE(SRAM_AUX_NODE);

	k_heap_init(&sram_aux_heap, mem, bytes);
	return 0;
}

/* Initialize at startup; priority adjust per project needs */
SYS_INIT(sram_aux_heap_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

void *my_aux_alloc(size_t nbytes)
{
	return k_heap_alloc(&sram_aux_heap, nbytes, K_NO_WAIT);
}

void my_aux_free(void *ptr)
{
	k_heap_free(&sram_aux_heap, ptr);
}
```

**Notes**:

- **`UINT_TO_POINTER`**: See `<zephyr/sys/util.h>`, converts `uintptr_t` to accessible pointer (if macro name changes with version, can use `(void *)DT_REG_ADDR(...)`, note integer and pointer width consistency).
- **Whole segment as heap pool**: `k_heap` maintains metadata in this memory; **do not** write to that range for other purposes unless you reduce `bytes` to leave a reserved area.
- **MPU / Bus**: Second segment must already be **accessible** by CPU; if default inaccessible after power-on, need SoC/board or **`MPU`** configuration, otherwise access causes Fault.
- **Alignment**: For **DMA**, besides allocation alignment, also need to meet peripheral and Cache requirements (e.g., **`cache_data_flush_range`** etc., per chip).

---

#### Method 2: Shared Multi-Heap (Multi-region, "attribute"-based heap selection)

Use this when aligning with Zephyr's **`shared_multi_heap_*`** APIs, or when the SoC already registers regions such as **EXTERNAL / NON_CACHEABLE**. It requires **`CONFIG_SHARED_MULTI_HEAP=y`** (automatically selects **`CONFIG_MULTI_HEAP`**).

**`prj.conf`**:

```properties
CONFIG_MULTI_HEAP=y
CONFIG_SHARED_MULTI_HEAP=y
```

**Example** (when board-level hasn't registered second segment, supplement via application; if **`shared_multi_heap_pool_init` already called by SoC**, second call may return **`-EALREADY`**, can ignore or only **`shared_multi_heap_add`**):

```c
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/multi_heap/shared_multi_heap.h>

#define SRAM_AUX_NODE DT_NODELABEL(sram_aux)

static int smh_aux_register(void)
{
	struct shared_multi_heap_region reg = {
		/* Business convention: can use enum or custom uint32_t, must match alloc */
		.attr = SMH_REG_ATTR_EXTERNAL,
		.addr = DT_REG_ADDR(SRAM_AUX_NODE),
		.size = DT_REG_SIZE(SRAM_AUX_NODE),
	};
	int err;

	err = shared_multi_heap_pool_init();
	if (err != 0 && err != -EALREADY) {
		return err;
	}
	return shared_multi_heap_add(&reg, NULL);
}

SYS_INIT(smh_aux_register, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

void *dma_buf_alloc(size_t len)
{
	return shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL, 32, len);
}
```

Reference Zephyr tree example: **`samples/boards/espressif/spiram_test`** (`shared_multi_heap_aligned_alloc` / `shared_multi_heap_free`). Different SoCs may have already registered external RAM in **`soc_*` / `board_*`**, application side avoid duplicate `add` for same segment.

---

#### Method 3: Linker Segment——Put global/static objects into named region

If DTS contains **`zephyr,memory-region = "SRAM_AUX"`** and **`LINKER_DT_REGIONS()`** participates in linking (depending on the SoC and Zephyr version), the linker generates a **memory region** and **section** for **`SRAM_AUX`**. You can place read-only/read-write data in this segment; the section name must match the generated name, commonly the uppercase/underscore form of the property value. Check **`build/zephyr/zephyr.map`**:

```c
/* Example: large static buffer placed in SRAM_AUX (name per actual map / devicetree_regions) */
__attribute__((section(".SRAM_AUX")))
static uint8_t s_dma_static_pool[4096];
```

A more robust approach is to use macros such as **`LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(sram_aux))`** from **`#include <zephyr/linker/devicetree_regions.h>`** to generate region/section names consistent with DTS, and consult the current Zephyr documentation for linker script generation and Devicetree memory regions. If the linker cannot find the segment name, the SoC did not connect **`zephyr,memory-region`** to the final **`linker.ld`**; fall back to **Method 1** and build a **`k_heap`** from the address.

---

#### `prj.conf` (with Method 1/2)

```properties
# Method 2 needs:
# CONFIG_MULTI_HEAP=y
# CONFIG_SHARED_MULTI_HEAP=y

# Method 1 usually only needs kernel heap related defaults; if turning off default k_malloc evaluate separately
# CONFIG_HEAP_MEM_POOL_SIZE=...
```

---

#### Summary

Typical pattern for **scattered memory**: **one segment as `zephyr,sram` + other segments as `zephyr,memory-region`**. Most common on code side is **`DT_REG_*` + `k_heap_*`** (Method 1); use **`shared_multi_heap_*`** (Method 2) only when needing alignment with Zephyr subsystems for "attribute-based region selection". **Do not** "fake merge" non-contiguous physical regions into one continuous `reg` in DTS (unless hardware addresses truly contiguous).

---

## 12. References

- [Zephyr Application Development — Devicetree](https://docs.zephyrproject.org/latest/build/dts/index.html)
- [Zephyr Devicetree — Overlays](https://docs.zephyrproject.org/latest/build/dts/index.html) ("Overlays" chapter in docs)
- This repository: **[Developer Getting Started Guide.md](../00-getting-started/04-developer-guide.md)**, **[Project Configuration Guide.md](42-config-options.md)**
- STM32L4R5: ST **RM0432** memory map

---

*Document version consistent with project directory structure; when upgrading Zephyr major version, verify CMake and DTS behavior against official Release Notes.*
