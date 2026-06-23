# Zeplod 配置片段目录

本目录存放 **叠加于根目录 `prj.conf`** 的配置片段。`prj.conf` 仅保留各档位共用的框架基线；完整默认构建由 **CMake 自动合并** 下列文件（等价于「标准版」）：

```
prj.conf
conf/profiles/standard.conf
conf/features/data_bus.conf
conf/features/thread_ipc.conf
```

详细对比见：[配置方案对比指南](../docs/zh-CN/40-应用开发/43-配置方案对比指南.md)

## 目录结构

| 子目录 | 用途 | 默认是否合并 |
|--------|------|-------------|
| `profiles/` | 内存档位 | `standard.conf` 由 CMake 自动合并；其余档位用 `EXTRA_CONF_FILE` |
| `features/` | 可选功能 | `data_bus`、`thread_ipc` 默认合并；其余按需叠加 |
| `targets/` | 仿真/目标环境 | 否（QEMU 脚本自动带上） |
| `examples/` | 示例模块 | 否 |

## 常用构建命令

```bash
# 默认标准版（CMake 自动合并，无需额外参数）
west build -b <board> .

# 平衡版（在默认链上追加覆盖项）
west build -b nucleo_l4r5zi . -- -DEXTRA_CONF_FILE=conf/profiles/balanced.conf

# 极简版
west build -b native_sim . -- -DEXTRA_CONF_FILE=conf/profiles/minimal.conf

# 极限版（须替换整条链，勿与 standard/features 默认链同用）
west build -b mimxrt1050_fire/mimxrt1052/qspi . \
  -- -DCONF_FILE="prj.conf;conf/profiles/tiny.conf" --pristine

# QEMU 仿真（run_qemu.ps1 已含完整链 + qemu.conf）
west build -b qemu_riscv32 . \
  -- -DCONF_FILE="prj.conf;conf/profiles/standard.conf;conf/features/data_bus.conf;conf/features/thread_ipc.conf;conf/targets/qemu.conf"

# 可选功能叠加
west build -b nucleo_l4r5zi . -- -DEXTRA_CONF_FILE=conf/features/ota.conf
west build -b nucleo_l4r5zi . -- -DEXTRA_CONF_FILE=conf/features/diag.conf
west build -b nucleo_l4r5zi . -- -DEXTRA_CONF_FILE=conf/features/recovery.conf
west build -b nucleo_l4r5zi . -- -DEXTRA_CONF_FILE=conf/features/secure_kv.conf
west build -b nucleo_l4r5zi . -- -DEXTRA_CONF_FILE=conf/features/sys_time.conf
west build -b nucleo_l4r5zi . -- -DEXTRA_CONF_FILE=conf/features/net_stub.conf
west build -b nucleo_l4r5zi . -- -DEXTRA_CONF_FILE=conf/features/factory_mode.conf
west build -b nucleo_l4r5zi . -- -DEXTRA_CONF_FILE=conf/features/scale.conf
west build -b nucleo_l4r5zi . -- -DEXTRA_CONF_FILE="conf/features/scale.conf;conf/profiles/sku_gateway.conf"
west build -b nucleo_l4r5zi . -- -DEXTRA_CONF_FILE=conf/targets/production.conf
west build -b nucleo_l4r5zi . -- -DEXTRA_CONF_FILE=conf/features/app_kv_persist.conf
west build -b <board> . -- -DEXTRA_CONF_FILE=conf/examples/gpio_uart.conf
```

## 文件一览

| 文件 | 说明 |
|------|------|
| `profiles/standard.conf` | 标准版（≥256KB SRAM），CMake 默认 |
| `profiles/balanced.conf` | 平衡版 delta（64–128KB） |
| `profiles/minimal.conf` | 极简版 delta（32–64KB） |
| `profiles/tiny.conf` | 极限版完整覆盖（≤32KB） |
| `features/data_bus.conf` | Data Bus，CMake 默认 |
| `features/thread_ipc.conf` | Thread IPC，CMake 默认 |
| `features/app_kv_persist.conf` | KV 掉电持久化 |
| `features/ota.conf` | OTA 模块（null 传输，ztest/开发） |
| `features/diag.conf` | sys_diag 健康快照 |
| `features/recovery.conf` | recovery_policy 模块 |
| `features/secure_kv.conf` | sys_secure_kv 加密 KV |
| `features/sys_time.conf` | 墙钟时间服务 |
| `features/connectivity.conf` | 连接管理模块（null 后端） |
| `features/provisioning.conf` | 配网模块（stub） |
| `features/net_stub.conf` | Phase 3 联网骨架三合一 |
| `features/factory_mode.conf` | 工厂产测模块 |
| `features/remote_ops.conf` | 远程运维钩子 |
| `features/feature_gate.conf` | 功能开关 license 槽位 |
| `features/scale.conf` | Phase 5 规模化三合一 |
| `profiles/sku_gateway.conf` | 网关 SKU 示例 |
| `features/boot_fast.conf` | 快速/极限启动优化 |
| `targets/production.conf` | 量产固件叠加（关闭 factory） |
| `targets/qemu.conf` | QEMU 仿真裁剪 |
| `targets/mcuboot.conf` | 主动 ingest（write_chunk / flash_img） |
| `targets/mcumgr_smp.conf` | MCUmgr SMP 被动接入（产品推荐） |
| `examples/gpio_uart.conf` | GPIO/UART 示例 |
| `examples/module_ipc.conf` | Thread IPC 集成示例 |

测试专用片段仍在 `tests/` 目录。

## 设计说明

- **`prj.conf`**：框架身份开关（事件系统、模块管理器等），不含内存档位与可选子系统参数。
- **`profiles/standard.conf`**：原 `prj.conf` 中的标准内存、日志、事件容量、应用功能等。
- **`features/`**：可独立启停的子系统；默认启用项由 CMake 写入合并链。
- **档位片段**：`balanced` / `minimal` 仅写与 standard 的**差异项**；`tiny` 为独立完整链。

## 参考实现模块（不在默认 CI 单测矩阵）

`connectivity`、`provisioning`、`remote_ops`、`feature_gate` 四个模块为**参考实现**，默认不在 CI 专门单测矩阵中运行。

**源码与配置文件均完整保留**，可随时使用：
- 源码：`src/modules/<模块名>/`
- 配置片段：`conf/features/<模块名>.conf`
- 测试配置：`tests/prj_<模块名>.conf`

**手动测试示例**（按模块替换最后一段文件名）：

```bash
west build -b native_sim tests/ --build-dir build_x \
  -- -DCONF_FILE="prj.conf;prj_connectivity.conf"
west build -t run --build-dir build_x
```

> 注：`west build tests/` 时 `CONF_FILE` 相对 `tests/` 目录解析，故不带 `tests/` 前缀。

其余模块替换示例：`prj_provisioning.conf`、`prj_remote_ops.conf`、`prj_feature_gate.conf`、`prj_feature_gate_boot.conf`。

**编译覆盖**：这四个模块仍由以下编译检查步骤覆盖，不会完全脱离 CI：
- `conf/features/scale.conf` 覆盖 `remote_ops`、`feature_gate`
- `conf/profiles/sku_gateway.conf`（与 `scale.conf` 联用）覆盖 `provisioning`、`connectivity`
