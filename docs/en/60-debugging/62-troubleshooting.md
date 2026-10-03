> Language: [中文](../../zh-CN/60-调试与排错/62-常见问题与故障排除.md) | **English**

# Troubleshooting

This document summarizes **frequently encountered issues** and their solutions during development with this repository, helping beginners troubleshoot independently. For board-level hardware differences, always refer to the **Zephyr Official Documentation** and chip reference manuals.

**Related Documents**: [Documentation Index.md](../00-getting-started/02-doc-index.md) · [Environment Setup Guide.md](../10-environment-build/11-environment-setup.md) · [Standalone Build Guide.md](../10-environment-build/12-freestanding-app-build.md) · [Device Tree and Memory Config.md](../40-app-development/44-devicetree-memory-config.md)

---

## 1. Environment and Paths

### 1.1 `ZEPHYR_BASE not set` / Cannot find Zephyr

**Symptom**: CMake reports `ZEPHYR_BASE not set and zephyr_config.env not found`.

**Resolution**:

1. Copy **`zephyr_config.env.template`** to **`zephyr_config.env`** and fill in your local **`ZEPHYR_BASE`** (pointing to the Zephyr repository root) and **`ZEPHYR_SDK_INSTALL_DIR`**.  
2. Or manually set environment variables in your terminal before running **`west build`**.  
3. Verify that **`CMakeLists.txt`** reads `zephyr_config.env` (this project includes this logic).

### 1.2 `west` not found in terminal

**Symptom**: `'west' is not recognized` (Windows) or `command not found: west`.

**Resolution**:

- Install in the **Python virtual environment** for Zephyr: `pip install west`, then **activate that venv** before running `west`.  
- Or add the venv's `Scripts` (Windows) or `bin` (Linux/macOS) to your **PATH**.  
- Verify: `west --version`.

### 1.3 Python / CMake version requirements not met

**Symptom**: Zephyr or CMake reports version too low.

**Resolution**: Install **Python 3.10+** (per current Zephyr release notes), **CMake 3.20+**, and ensure your command line calls the new versions (`python --version`, `cmake --version`).

---

## 2. Build Failures

### 2.1 `region RAM overflowed` / `noinit` doesn't fit

**Meaning**: At the linking stage, the main **RAM** region is smaller than the total size required by `.bss` / `noinit` / heap, etc.

**Resolution**:

1. Read **[Device Tree and Memory Config.md](../40-app-development/44-devicetree-memory-config.md)**: Verify the overlay is applied and main RAM has been extended to the chip's allowed range.
   **Note**: When building **`nucleo_l4r5zi`**, if a **`boards/nucleo_l4r5zi.overlay`** exists, Zephyr **typically no longer** auto-merges the root **`app.overlay`**; if that board-level overlay only has settings without SRAM extension, linking uses the default ~**192KB** RAM, which can easily **overflow** when combined with **64KB heap**, NVS, etc. This repository has **640KB SRAM** and **settings-partition** in the same **`boards/nucleo_l4r5zi.overlay`**.  
2. In **`prj.conf`**, reduce **`CONFIG_HEAP_MEM_POOL_SIZE`**, thread stack sizes, queue depths, etc. as appropriate (see **[Project Config Options.md](../40-app-development/42-config-options.md)**).
3. After modifying overlay or `prj.conf`, use **`west build ... -p always`** or **`west build -t pristine`** to rebuild.

### 2.2 Devicetree / overlay parse error

**Symptom**: `devicetree error: ... parse error`, **`expected node name`**.

**Resolution**:

- **`/delete-node/`** must be at the **overlay file top level**, not inside `/ { }`.  
- Auto-merged overlays must use the **`.overlay`** suffix; **`.dts`** files are not auto-merged as application overlays unless specified via **`DTC_OVERLAY_FILE`**.  
See **[Device Tree and Memory Config.md](../40-app-development/44-devicetree-memory-config.md)** for details.

### 2.3 `Board 'xxx' not found`

**Resolution**:

- Use Zephyr's built-in board names (e.g., **`nucleo_l4r5zi`**), or ensure custom boards are placed in the **`BOARD_ROOT`** corresponding directory and **`CMakeLists.txt`** has **`list(APPEND BOARD_ROOT ...)`**.  
See **[Standalone Build Guide.md](../10-environment-build/12-freestanding-app-build.md)** for details.

### 2.4 Changes don't take effect after config modification

**Resolution**: Run **`west build -t pristine`** or **`west build ... -p always`** to bypass CMake/Devicetree cache and avoid old config being reused.

---

## 3. Runtime and Hardware

### 3.1 Flash succeeds but no serial output

**Resolution**:

1. Verify **`CONFIG_SERIAL`** and the used UART are enabled in **`prj.conf`**; the **`zephyr,console`** assigned UART matches the board's **USB-to-serial** pins (see board DTS / user manual).  
2. Baud rate matches the terminal software (commonly **115200**).  
3. On Windows, confirm the **COM port number** in Device Manager; on Linux, check **`/dev/ttyACM0`** permissions.

See **[Flash and Debug Quickstart.md](61-flash-debug-quickstart.md)** for more details.

### 3.2 Unit tests can't run on PC

**Resolution**: This repository's tests target **`native_sim`** by default on Linux/WSL; native Windows uses `qemu_riscv32`. Commands and directories are in **`tests/README.md`** and the **[Unit Testing and CI Guide](../50-testing-ci/51-unit-testing-ci.md)**.

---

## 4. Documentation and Repository

### 4.1 Old documentation filenames won't open

This repository previously renamed some files under **`docs/`**. The old-to-new name mapping is at the end of the **[Documentation Index.md](../00-getting-started/02-doc-index.md)** under "Old Filename Reference".

### 4.2 What is `docs/zephyr.pdf`?

If **`docs/zephyr.pdf`** exists in the repository, it is typically a locally saved backup of the **Zephyr Official PDF Documentation**, **not involved in builds**; use the [Zephyr Online Documentation](https://docs.zephyrproject.org/) as the authoritative source.

---

## 5. Code Changes from 2026-05-09

### 5.1 Event SLAB pool exhaustion

**Symptom**: Event publish returns `EVENT_ERR_NO_MEM`.

**Resolution**: Register a callback with `event_register_slab_exhausted_cb` to monitor slab status; check `event_get_slab_stats` for pool usage rates.

### 5.2 Event dispatcher not started

**Symptom**: `event_publish` returns `EVENT_ERR_NOT_RUNNING (-8)`.

**Resolution**: Verify `event_system_start()` has been called and the dispatcher thread was created.

### 5.3 IPC shared memory not enabled

**Symptom**: `ipc_call_sync_shm` returns `-ENOTSUP` or compile-time error.

**Resolution**: Enable `CONFIG_THREAD_IPC_SERVICE_SHARED_MEM=y` in `prj.conf`.

### 5.4 Module stop idempotency

**Symptom**: Module state abnormal after repeated `module_manager_stop` calls.

**Resolution**: `module_manager_stop_module` returns 0 and does not call business stop when not in RUNNING state; repeated stop is safe.

### 5.5 Calling manager API in module init

**Symptom**: Deadlock or state machine anomaly during module initialization.

**Resolution**: init() is now called outside the lock, but calling stop in init triggers illegal state transitions. Avoid operating on module state machine in init.

---

*If you encounter a new issue with broad applicability, feel free to add it here and submit a PR.*
