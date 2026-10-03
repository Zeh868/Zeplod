> Language: [中文](../../zh-CN/60-调试与排错/61-烧录与调试快速指南.md) | **English**

# Flash and Debug Quickstart

This guide covers how to flash firmware to a development board, view serial logs, and get started with debugging—assuming you have **already successfully run `west build`**. Specific commands may vary slightly depending on your OS and debugger (ST-Link, J-Link, etc.). Refer to the **[Zephyr Official Documentation](https://docs.zephyrproject.org/latest/develop/west/build-flash-debug.html)** for details.

**Prerequisite Reading**: [Environment Setup Guide.md](../10-environment-build/11-environment-setup.md) · [Standalone Build Guide.md](../10-environment-build/12-freestanding-app-build.md)

---

## 1. Flashing

From the **application root directory** (containing `CMakeLists.txt`) after at least one successful **`west build -b <board> .`**:

```bash
west flash
```

- **Build Directory**: If not using the default **`./build`**, specify explicitly:  
  `west flash -d build_nucleo`  
  (Must match the **`-d` / `--build-dir`** used with **`west build`**.)
- **Switching Runners**: If the default runner doesn't match your board (e.g., using **J-Link** or **pyOCD**), specify:  
  `west flash -r jlink`  
  or `west flash -r pyocd`  
  Run **`west flash --help`** or check the Zephyr documentation for your board's supported **Runner**.

Common Notes:

- **ST Nucleo** boards with onboard ST-Link typically work after installing the **ST driver**; if it fails, check USB, driver, and the Zephyr documentation for the board's **Runner** info.  
- For **J-Link**, **DAPLink**, etc., install the corresponding tools and ensure they are in your `PATH`.  
- Multi-image / partitioned schemes (e.g., MCUboot) require following the **[OTA and Storage Guide.md](../70-release-productization/74-ota-storage-guide.md)** and official MCUboot documentation—**do not** rely on a single `west flash` to cover all partitions.

---

## 2. Serial Terminal (View `printk` / Shell)

When the application uses **`CONFIG_SERIAL`** and board-level **`zephyr,console`**, connect to the development board's **USB virtual serial port** on the host side.

### Windows

1. Open **Device Manager** → **Ports (COM & LPT)** and note the **COM number** (e.g., `COM3`).  
2. Use **PuTTY**, **Tera Term**, or install **Python pyserial**:

```powershell
python -m pip install pyserial
python -m serial.tools.miniterm COM3 115200
```

(Replace `COM3` and `115200` with your port and baud rate; this template's console typically uses **115200**.)

### Linux / macOS

```bash
# Common device names: /dev/ttyACM0, /dev/ttyUSB0
python3 -m serial.tools.miniterm /dev/ttyACM0 115200
# Or use minicom, screen, etc.
```

If you encounter permission issues, add your user to the **`dialout`** group or use `sudo` (not recommended for long-term use).

---

## 3. Debugging

### 3.1 `west debug`

When GDB/OpenOCD support is configured:

```bash
west debug
```

This launches a debug session using the board's default **runner**; it depends on **OpenOCD**, **J-Link GDB Server**, etc. being installed. For non-default build directories, add **`-d <dir>`**; to switch runners, use **`west debug -r <runner>`** (same rules as **`west flash`**).

### 3.2 IDE

You can use **VS Code**, **CLion**, etc. with **GDB** and Zephyr's generated **ELF** file (typically at **`build/zephyr/zephyr.elf`**), configuring the "executable path" and "GDB server". See the Zephyr documentation's **Debugging** chapter for details.

---

## 4. Checklist (No Output)

| Check | Description |
|-------|-------------|
| Build succeeded | `build/zephyr/zephyr.bin` or `zephyr.elf` exists |
| Baud rate | Matches `prj.conf` / board default (commonly 115200) |
| Console device | The UART assigned `zephyr,console` matches the USB-connected serial on your board |
| Pin wiring | If using external USB-TTL, verify TX/RX/GND match the board silkscreen |

---

## 5. References

- [Flashing and Debugging](https://docs.zephyrproject.org/latest/develop/west/build-flash-debug.html)  
- **Developer Quickstart.md](../00-getting-started/04-developer-quickstart.md)** in this repository, "Debugging" section
