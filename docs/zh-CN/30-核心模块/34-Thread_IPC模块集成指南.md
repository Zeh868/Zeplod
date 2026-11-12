> 语言: **中文** | [English](../../en/30-core-modules/34-thread-ipc-integration-guide.md)

# 模块开发中集成 Thread IPC Service 指南

本文说明如何在 **业务模块**（配合本工程的 **`module_manager`**）中嵌入 **Thread IPC Service**，实现模块内「请求—响应」式服务，并与 **事件系统**、**事件桥** 可选联动。

---

## 目录

1. [适用场景](#1-适用场景)
2. [依赖与开关](#2-依赖与开关)
3. [初始化顺序（必读）](#3-初始化顺序必读)
4. [在模块中集成 IPC 的步骤](#4-在模块中集成-ipc-的步骤)
5. [参考实现：example_module_ipc](#5-参考实现example_module_ipc)
6. [与事件系统 / 事件桥](#6-与事件系统--事件桥)
7. [RAM 与线程](#7-ram-与线程)
8. [常见问题](#8-常见问题)
9. [一键验证构建](#9-一键验证构建)

---

## 1. 适用场景

| 场景 | 是否适合 |
|------|----------|
| 模块内希望把「耗时/需串行」的逻辑放到独立 Worker 线程，其它任务通过统一入口调用 | 适合 |
| 需要 Sync / Async / Future 三种调用语义 | 适合 |
| 跨核 / RPMsg / 与另一 CPU 通信 | **不适合**（请用 Zephyr 官方 `CONFIG_IPC_SERVICE` 子系统） |
| 仅需发布通知、一对多订阅 | 优先用 **事件系统**；可与 IPC 并存 |

---

## 2. 依赖与开关

| Kconfig | 含义 |
|---------|------|
| `CONFIG_THREAD_IPC_SERVICE` | Thread IPC 核心，必须为 `y` |
| `CONFIG_MODULE_MANAGER` | 模块管理器，示例模块注册依赖 |
| `CONFIG_EXAMPLE_MODULE_THREAD_IPC` | 编译本仓库提供的 **`example_module_ipc`** 示例模块 |
| `CONFIG_THREAD_IPC_SERVICE_EVENT_BRIDGE`（可选） | 在 `service_func` 内通过 `thread_ipc_event_publish_result()` 向事件总线广播结果 |

在 **menuconfig** 中：`Thread IPC service (in-app)` 下可勾选 **Example: business module integrating Thread IPC**。

---

## 3. 初始化顺序（必读）

### 3.1 `main()` 之前（`SYS_INIT`，`POST_KERNEL`）

子系统与 **`module_manager_register()`** 在 **`main()` 前**按 **`app_config.h`** 中 **`APP_INIT_PRIO_*`** 顺序执行，包括：

```text
event_system_init()
event_dispatcher_init()
…
module_manager_init()
各 example_module_*.c / 业务模块内的 SYS_INIT → module_manager_register()
  → 各模块 init()（可含 ipc_service_init()）
app_init_finalize()
```

### 3.2 `main()` 中（`app_start()`）

`app_main.c` 中 **`app_start()`** 顺序为：

```text
event_system_start()
event_dispatcher_start()
module_manager_start()
module_manager_start_all()    ← 此处调用各模块的 start()，可含 ipc_service_start()
```

因此：

- **`ipc_service_init()`** 放在模块的 **`init()`** 中（注册阶段，已在 `event_system_init()` 之后），可在此调用 **`thread_ipc_event_register_types()`**（若启用事件桥）。
- **`ipc_service_start()`** 放在模块的 **`start()`** 中（此时 **`event_system_start()` 已完成**）。若使用事件桥在 Worker 里 `event_publish_copy`，调度器已运行，事件不会被丢弃。
- **`ipc_service_stop()`** 放在模块的 **`stop()`** 中；若模块内有其它线程会调用 `ipc_call_*`，必须先 **结束或 join** 这些线程，再 `ipc_service_stop()`，避免停止过程中仍向队列投递请求。

---

## 4. 在模块中集成 IPC 的步骤

### 4.1 数据结构

在模块控制块中嵌入 **`ipc_service_t`**（体积较大，含双线程栈与队列缓冲）：

```c
typedef struct {
    module_status_t status;
    ipc_service_t   ipc;   /* 嵌入，不用指针 */
    /* … 其它模块状态 … */
} my_module_cb_t;
```

### 4.2 `ipc_service_init` 参数

当前 `ipc_service_init` 是 4 参数签名：

```c
int ipc_service_init(ipc_service_t *service, const char *name,
                    ipc_service_func_t service_func, int priority);
```

栈大小、队列容量由 Kconfig 在编译期固定：
- `CONFIG_THREAD_IPC_SERVICE_STACK_SIZE`
- `CONFIG_THREAD_IPC_SERVICE_REQUEST_QUEUE_SIZE`
- `CONFIG_THREAD_IPC_SERVICE_RESPONSE_QUEUE_SIZE`

模块只需声明服务名称和工作线程优先级。

### 4.3 实现 `ipc_service_func_t`

在 **Worker 线程**中执行；返回值为业务结果码（负 errno 风格）；通过 `*out_data` / `*out_data_size` 返回输出，注意 **缓冲区生命周期**（见 **[线程IPC服务使用说明.md](33-线程IPC服务使用说明.md)**）。

### 4.4 `start` / `stop`

- **`start`**：`ipc_service_start(&cb->ipc)`。
- **`stop`**：先停止本模块内会调用 IPC 的线程/work，再 **`ipc_service_stop(&cb->ipc)`**。
- 若 **`stop` 之后需要再次 `start`**：须再次执行 **`ipc_service_init()`**（本模块示例在 `start()` 里对 `MODULE_STATUS_STOPPED` 分支做了重新 `init`）。

### 4.5 注册到 `module_manager`

与其它示例模块相同，提供 **`module_interface_t`**，在**本模块 `.c`** 内通过 **`SYS_INIT(..., APP_INIT_PRIO_MODULE_IPC)`** 调用 **`module_manager_register(example_xxx_get_interface(), config, &module_id)`**（见 `example_module_ipc.c`）。

---

## 5. 参考实现：example_module_ipc

| 项目 | 说明 |
|------|------|
| 文件 | `src/modules_examples/example_module_ipc.c`、`include/zeplod/example_module_ipc.h` |
| 开关 | `CONFIG_EXAMPLE_MODULE_THREAD_IPC=y` |
| 行为 | `init` 中 `ipc_service_init`；`start` 中 `ipc_service_start` 并创建 **演示线程**，约 300ms 后执行一次 **`ipc_call_sync`**；`stop` 中 **`k_thread_join` 演示线程** 再 `ipc_service_stop` |
| 业务函数 | `mod_ipc_service_func`：echo 输入；若启用 **`CONFIG_THREAD_IPC_SERVICE_EVENT_BRIDGE`**，则 **`thread_ipc_event_publish_result(EXAMPLE_MODULE_IPC_EVENT_SOURCE_ID, …)`** |
| 对外调试 API | `example_module_ipc_demo_call_sync()`：模块 RUNNING 时可再次发起同步调用 |

打开 **`CONFIG_EXAMPLE_MODULE_THREAD_IPC`** 时，**`example_module_ipc.c`** 内的 **`SYS_INIT`** 会自动注册该模块（无需改 `app_main.c`）。

---

## 6. 与事件系统 / 事件桥

- 订阅 **`EVENT_TYPE_THREAD_IPC_RESPONSE`**（定义在 `event_system.h`），载荷类型为 **`thread_ipc_event_result_t`**（见 `ipc_service_event.h`）。
- 将 **`EXAMPLE_MODULE_IPC_EVENT_SOURCE_ID`（42）** 与 `thread_ipc_event_publish_result` 的 `source_id` 对应，便于多服务区分。
- 详细说明见 **[线程IPC服务使用说明.md](33-线程IPC服务使用说明.md)** 中「与事件系统联动」章节。

---

## 7. RAM 与线程

- 每个 **`ipc_service_t`** 含 **两个** `THREAD_IPC_SERVICE_STACK_SIZE` 大小的栈及队列缓冲，请预留 RAM。
- 本示例额外增加 **演示线程** 栈 `example_module_ipc_demo_stack`（当前 **1024** 字节，见 `example_module_ipc.c`）。
- 若 RAM 紧张：减小 `THREAD_IPC_SERVICE_STACK_SIZE`、队列深度、`MAX_PENDING`、演示线程栈；**不要**同时打开 **`CONFIG_THREAD_IPC_SERVICE_EXAMPLE`** 与 **`CONFIG_EXAMPLE_MODULE_THREAD_IPC`**（两套演示叠加易撑满 192KB SRAM）。
- 根目录 **`prj.conf`** 已设置 **`CONFIG_HEAP_MEM_POOL_SIZE`**（事件与 `event_publish_copy` 需要 **`k_malloc`**）；若仍为 `0` 会链接报错 `undefined reference to k_malloc`。

---

## 8. 常见问题

| 现象 | 处理 |
|------|------|
| `ipc_service_init` 返回 `-EINVAL` | 可能是 `service_func` 为 NULL 或 `priority` 越界 |
| 模块 `start` 后 IPC 无响应 | 确认 `ipc_service_start` 成功，且 Worker/Dispatcher 未被 `stop` |
| 停止阶段死锁或异常 | 确保 **`ipc_service_stop` 前** 已无线程阻塞在 `ipc_call_*`；示例通过 **先 join 演示线程** 保证 |
| 事件桥无回调 | 确认 **`event_system_start()` 早于** 模块 `start`；且已 **`event_subscribe(EVENT_TYPE_THREAD_IPC_RESPONSE, …)`** |
| 链接 `undefined reference to k_malloc` | 在 **`prj.conf`** 中设置 **`CONFIG_HEAP_MEM_POOL_SIZE`** 为非零（例如 8192） |
| `region RAM overflowed` / `noinit will not fit` | 减小 IPC 栈/队列、关其一演示模块、或换更大 SRAM 的板型；可与 `prj.conf` 中当前保守数值对齐 |
| `ipc_call_sync_shm` 返回 0 但 handle 无效 | 检查 `CONFIG_THREAD_IPC_SERVICE_SHARED_MEM` 是否启用 |

---

## 9. 一键验证构建

在已启用 `CONFIG_THREAD_IPC_SERVICE=y` 的 `prj.conf` 基础上，合并 **`conf/examples/module_ipc.conf`**：

```text
west build -b <board> -- '-DEXTRA_CONF_FILE=conf/examples/module_ipc.conf'
```

（PowerShell 下注意引号与绝对路径。）

构建成功后运行固件，日志中应出现 `example_module_ipc` 初始化、`Thread IPC demo: sync ok` 等输出。
**注意**：合并 `conf/examples/module_ipc.conf` 时，其中已将 **`CONFIG_THREAD_IPC_SERVICE_EXAMPLE=n`**，避免与 `example_module_ipc` 重复占用 RAM。

---

## 相关文档

- [线程IPC服务使用说明.md](33-线程IPC服务使用说明.md) — API、三种调用模式、事件桥 API
- [事件系统详细使用说明.md](31-事件系统详细使用说明.md) — 订阅与发布
