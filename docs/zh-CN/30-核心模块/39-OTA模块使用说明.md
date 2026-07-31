> 语言: **中文** | English（待译）

# OTA 模块使用说明

本文说明 Zeplod **可选 OTA 模块**（`CONFIG_OTA_MODULE`）的 API、事件契约、接入路径与测试用法。

**相关**： [产品化扩展路线图](../../superpowers/plan/2026-06-13-product-framework-expansion-roadmap.md) · [OTA 实施计划](../../superpowers/plan/2026-06-13-ota-module-implementation-plan.md) · [74-OTA与存储扩展指南](../70-发布与产品化/74-OTA与存储扩展指南.md)

---

## 1. 启用

```bash
# 开发 / ztest（null 传输）
west build -b <board> . -- -DEXTRA_CONF_FILE=conf/features/ota.conf

# 产品默认：MCUmgr SMP 被动接入（UART / BLE / UDP 由 Zephyr 多传输承载）
west build -b <board> . -- -DEXTRA_CONF_FILE=conf/targets/mcumgr_smp.conf

# 仅主动 write_chunk 路径（自建协议 / CI）
west build -b <board> . -- -DEXTRA_CONF_FILE=conf/targets/mcuboot.conf
```

`conf/features/ota.conf`：**null**（RAM 模拟，ztest）。
`conf/targets/mcumgr_smp.conf`：**MCUmgr SMP**（产品推荐）。
`conf/targets/mcuboot.conf`：**主动 ingest**（`OTA_TRANSPORT_ACTIVE`，`flash_img` + `write_chunk`）。

二者可在同一固件并存（`mcumgr_smp.conf` 中取消注释 `CONFIG_OTA_TRANSPORT_ACTIVE=y`），**运行时仅允许一路升级会话**。

---

## 2. 双接入模型

| 路径 | Kconfig | API | 典型场景 |
|------|---------|-----|----------|
| **被动（默认）** | `OTA_TRANSPORT_MCUMGR_SMP` | 外部 `mcumgr image upload` | 串口/485、BLE、UDP |
| **主动（扩展）** | `OTA_TRANSPORT_ACTIVE` | `begin_update` → `write_chunk` → `finish` | Modbus、HTTP、自建协议 |

状态机与事件（50/51）两条路径共用；`ingest_owner` 在模块内互斥。

---

## 3. 状态机

| 状态 | 含义 |
|------|------|
| `OTA_STATE_IDLE` | 空闲，可开始升级 |
| `OTA_STATE_DOWNLOADING` | 正在写入镜像块 |
| `OTA_STATE_VERIFYING` | 校验中 |
| `OTA_STATE_READY_REBOOT` | 校验通过，待重启切换 |
| `OTA_STATE_ERROR` | 失败 |

主动路径典型流程：`ota_module_begin_update()` → `ota_module_write_chunk()` × N → `ota_module_finish_update()` → `ota_module_request_reboot()`。

---

## 4. 事件

| 事件 ID | 名称 | 负载 |
|---------|------|------|
| 50 | `EVENT_OTA_STATE_CHANGED` | `ota_progress_t` |
| 51 | `EVENT_OTA_PROGRESS` | `ota_progress_t`（与 STATE_CHANGED 同步发布） |

```c
typedef struct {
    ota_state_t state;
    uint8_t     percent;
    int         error_code;
} ota_progress_t;
```

---

## 5. 传输与后端

| Kconfig | 实现 | 说明 |
|---------|------|------|
| `OTA_TRANSPORT_NULL` | `ota_transport_null_get()` | native_sim / ztest |
| `OTA_TRANSPORT_MCUMGR_SMP` | `ota_transport_mcumgr_smp_get()` | img_mgmt 回调桥接；多 `MCUMGR_TRANSPORT_*` |
| `OTA_TRANSPORT_ACTIVE` | `ota_transport_mcuboot_get()` | `flash_img` 顺序写入 secondary slot |

主动路径要求 **顺序写入**（`offset` 须等于已写字节数）。`finish` 成功后 `boot_request_upgrade(BOOT_UPGRADE_TEST)`。

MCUmgr 被动路径：`ota_module_start()` 注册 img_mgmt 钩子；上传完成自动 `boot_request_upgrade(TEST)` 并进入 `OTA_STATE_READY_REBOOT`。

升级后配置结构变化时，在固件启动早期调用 `app_kv_run_migrations()`；见 `app_kv.h`。

---

## 6. Shell

在 `CONFIG_APP_ENABLE_SHELL` 与 `CONFIG_OTA_MODULE` 同时启用时：

```
ota status   # 打印当前状态
ota begin    # 开始下载（须 OTA_TRANSPORT_NULL 或 OTA_TRANSPORT_ACTIVE 之一）
ota abort    # 中止会话
```

---

## 7. 错误码

| 宏 | 值 | 含义 |
|----|-----|------|
| `APP_ERR_OTA_INVALID_STATE` | -20 | 当前状态不允许该操作 |
| `APP_ERR_OTA_TRANSPORT` | -21 | 传输层 open/verify 失败或未启用主动路径 |

---

## 8. 测试

```bash
west build -b native_sim tests/ --build-dir build_tests_ota -p always \
  -- -DCONF_FILE="prj.conf;prj_ota.conf"
west build -t run --build-dir build_tests_ota
```

`prj_ota.conf` 关闭 `CONFIG_OTA_MODULE_AUTOINIT`。用例：`tests/test_ota_module.c`。
