> Language: [中文](../../zh-CN/70-发布与产品化/74-OTA与存储扩展指南.md) | **English**

# OTA and Storage Expansion Guide (Optional)

This document explains how to introduce capabilities like **OTA (MCUboot)**, **non-volatile configuration (NVS/Settings)**, and **power management (PM)** during the **productization phase**. Content is **optional** integration guidance, not tied to specific boards; before implementation, refer to official documentation for your target SoC, Flash partitioning, and Zephyr version.

**Who should read this**: Developers with successful **`west build` / flashing** experience who need upgrade channels or persistent parameters. **Beginners** should first complete Path A in the **[Documentation Index.md](../00-getting-started/02-doc-index.md)** before deciding whether to read this.

**Signatures and Keys**: Do not commit private keys to the repository; see **[Security and Key Management Guide.md](75-security-key-management.md)**.

## 1. OTA / MCUboot

### 1.1 Role Division

In a typical Zephyr OTA solution, **MCUboot** is a **secondary bootloader** residing at the front of Flash. After power-on, MCUboot runs first and uses metadata and verification results to decide **which slot to boot**. It handles image transfer, verification, swap, or overwrite during the upgrade. Your business firmware (the application in this template) compiles as an **upgradable signed image**, built and flashed **separately from the bootloader** (or using **sysbuild** to produce multiple images at once).

Official concepts and option descriptions: [MCUboot with Zephyr](https://docs.zephyrproject.org/latest/services/device_mgmt/mcuboot.html).

### 1.2 Introducing MCUboot Source in Workspace

MCUboot is usually checked out as **one of the West projects** into `bootloader/mcuboot` (path subject to your `west.yml`). Approaches include:

- Add the `mcuboot` repository in the manifest of the **Zephyr workspace**, and run `west update`;
- Or **import** the bootloader fragment already included in Zephyr's official manifest in your existing `west.yml` (depending on your workspace organization).

**Key Point**: The application project (this repo) and MCUboot are **not compiled in the same CMake project**; if using **sysbuild**, the top-level CMake orchestrates **bootloader + app** images and dependencies together.

### 1.3 Build Methods: Sysbuild and "Dual Image"

- **Sysbuild (recommended for new projects)**
  Zephyr's **system build** can build **MCUboot** and **application** simultaneously in one configuration, automatically handling image layout, partition names, and some packaging options. Entry point and `sysbuild.cmake` writing see Zephyr documentation's **Sysbuild** and **MCUboot** chapters.

- **Traditional dual image**
  Separately run `west build` for **MCUboot** and **application**, then flash `zephyr.elf` / `zephyr.bin` / signed artifacts to corresponding partitions according to the partition table. Suitable for scenarios with existing scripts or CI already fixed for two-step builds.

Regardless of method, **Flash partition table** must be consistent with **MCUboot's slot strategy** and **application linker script**.

### 1.4 Partitions and Slots (slot0 / slot1 / scratch)

Common layout (names and sizes depend on SoC and product):

| Area | Typical Meaning |
|------|-----------------|
| **slot0** | Current running partition (primary) |
| **slot1** | Upgrade write partition (secondary) or image B |
| **scratch** | Special partition for **temporary movement** under partial swap strategy; if using **overwrite / single slot** strategies, scratch may not exist or layout differs |

Partitions are described in **device tree** via `fixed-partitions` / `partition` nodes, corresponding to `chosen` (e.g., `zephyr,flash`, partition labels). **Different chips** have different Flash alignment, sector sizes, and whether in-place swap is supported—must consult **that SoC's MCUboot porting guide** and Zephyr board examples.

**This template**: when `boards/` only provides generic overlays, **do not** directly use as mass production partition scheme; write **separate overlays for your board**, consistent with `pm_static.yml` (if used) or partition generation scripts.

### 1.5 Application-side Kconfig (example, subject to current Zephyr version)

Symbol names evolve with version; before merging, use `menuconfig` / search **IMG_MANAGER**, **MCUboot**, **BOOTLOADER_MCUBOOT** to verify. Common directions:

| Direction | Description |
|-----------|-------------|
| **Bootloader and image management** | If application needs to **confirm/switch/query** image status, enable options related to **IMG_MANAGER**, **MCUboot** (specific names subject to documentation). |
| **Signing and encryption** | After enabling **signed** build, configure **key path**, **signature algorithm** (e.g., `CONFIG_IMG_SIGNING_KEY_FILE`, etc., subject to version); **keys and CI security** must not be committed to repository. |
| **Post-upgrade behavior** | Like **test** mode, **revert**, **confirm**, etc., consistent with MCUboot's `bootutil` behavior—align with application side and documentation. |

**Note**: `CONFIG_BOOTLOADER_MCUBOOT` type options often appear in **bootloader projects** or **sysbuild sub-images**; application `prj.conf` may not have the same name; avoid copying bootloader options directly to application.

### 1.6 Transport and mcumgr (SMP)

Devices often use **MCUmgr (SMP)** via **UART / BLE / UDP** to transport **image fragments**. During integration, pay attention to:

- **Stack and threads**: SMP and transport layer occupy **task stack**; when concurrent with this template's event bus, Thread IPC, and large buffers, **increase stack** or **rate limit** in `prj.conf`.
- **Isolation from business**: Recommend placing **download/verification/state machine** in an independent module, notifying other modules via **events** (see Section 4), avoiding long-term blocking in callbacks.

Reference: [Device Management (mcumgr)](https://docs.zephyrproject.org/latest/services/device_mgmt/index.html).

### 1.7 Device Tree Overlay Key Points

- Add `*.overlay` for **your board** under **`boards/<vendor>/<board>/`** or this repository's **`boards/`**: declare **partitions**, **bootloader slots**, **storage peripherals**.
- Must be consistent with **MCUboot's** **partition labels** and `CONFIG_BOOTLOADER_MCUBOOT_*` / partition generation tools.

### 1.8 Reference Links

- [MCUboot with Zephyr](https://docs.zephyrproject.org/latest/services/device_mgmt/mcuboot.html)
- [MCUboot sample (Zephyr)](https://docs.zephyrproject.org/latest/samples/subsys/mcuboot/README.html)
- [Device Management (mcumgr)](https://docs.zephyrproject.org/latest/services/device_mgmt/index.html)

---

**Summary**: OTA is strongly related to **Flash layout, signatures, and boot chain**; this template only provides suggestions on **architecture and module boundaries**; **before mass production**, complete **full-chain flashing, rollback, and power-failure recovery** testing on target hardware.

---

## 2. NVS / Factory Configuration (Settings)

**Concept**: Use [Settings subsystem](https://docs.zephyrproject.org/latest/services/storage/index.html) (often backed by NVS or file) to store calibration parameters, device names, counters, etc.

**Example configuration fragment** (specific symbols subject to Zephyr version in use):

```kconfig
CONFIG_FLASH=y
CONFIG_FLASH_PAGE_LAYOUT=y
CONFIG_NVS=y
CONFIG_SETTINGS=y
CONFIG_SETTINGS_NVS=y
```

**Application side**: `settings_subsys_init()` → `settings_load()` → `settings_save_one()`, etc.; pay attention to **wear leveling** and **first-power-on default values**.

---

## 3. Power Management (PM / Tickless)

**Concept**: Enable `CONFIG_PM`, tickless timer on sleep-capable hardware, and in business modules shorten lock-holding time, avoid long-term blocking on critical paths.

**Configuration direction (summary)**:

- `CONFIG_PM=y`, `CONFIG_PM_DEVICE=y` (if using device runtime PM)
- `CONFIG_TICKLESS_KERNEL=y` (if SoC/driver supports)

**With event system**: Before long `k_sleep`, confirm whether **watchdog**, **IPC timeout**, **event queue depth** still meet real-time requirements.

---

## 4. Relationship with This Template's Event/Module System

- **OTA**: Recommend processing download state machine in an independent module, notifying other modules via **events** (e.g., restart, version number display).
- **NVS**: Initialization should be in `app_main` or a dedicated `storage` module's `init` phase; failure paths need **logging + safe default configuration**.
- **PM**: Before entering deep sleep, stop peripherals, unregister unnecessary polling; simultaneously evaluate with `sys_timer`, Thread IPC thread stacks.

For architecture details, see the **[Module System Detailed Usage Guide.md](../30-core-modules/32-module-system-guide.md)** and **[Event System Detailed Usage Guide.md](../30-core-modules/31-event-system-guide.md)**. For **Kconfig and configuration macro meanings**, see the **[Project Configuration Options Guide.md](../40-app-development/42-config-options.md)**.
