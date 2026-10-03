> Language: [中文](../../zh-CN/30-核心模块/34-Thread_IPC模块集成指南.md) | **English**

# Guide for Integrating Thread IPC Service in Module Development

This document explains how to embed **Thread IPC Service** in **business modules** (together with this project's **`module_manager`**), implement request-response services within modules, and optionally integrate them with the **Event System** and **Event Bridge**.

---

## Table of Contents

1. [Applicable Scenarios](#1-applicable-scenarios)
2. [Dependencies and Switches](#2-dependencies-and-switches)
3. [Initialization Order (Must Read)](#3-initialization-order-must-read)
4. [Steps to Integrate IPC in a Module](#4-steps-to-integrate-ipc-in-a-module)
5. [Reference Implementation: example_module_ipc](#5-reference-implementationexample_module_ipc)
6. [With Event System / Event Bridge](#6-with-event-system--event-bridge)
7. [RAM and Threads](#7-ram-and-threads)
8. [Common Issues](#8-common-issues)
9. [One-Click Verification Build](#9-one-click-verification-build)

---

## 1. Applicable Scenarios

| Scenario | Suitable |
|----------|----------|
| Module wants to put "time-consuming/serial" logic in an independent Worker thread, with other tasks calling through a unified entry | Suitable |
| Need Sync / Async / Future three calling semantics | Suitable |
| Multi-core / RPMsg / Communication with another CPU | **Not Suitable** (use Zephyr official `CONFIG_IPC_SERVICE` subsystem) |
| Only need to publish notifications, one-to-many subscription | Prefer **Event System**; can coexist with IPC |

---

## 2. Dependencies and Switches

| Kconfig | Meaning |
|---------|---------|
| `CONFIG_THREAD_IPC_SERVICE` | Thread IPC core, must be `y` |
| `CONFIG_MODULE_MANAGER` | Module Manager, example module registration depends on it |
| `CONFIG_EXAMPLE_MODULE_THREAD_IPC` | Compile **`example_module_ipc`** example module provided by this repository |
| `CONFIG_THREAD_IPC_SERVICE_EVENT_BRIDGE` (optional) | Broadcast IPC results to event bus via `thread_ipc_event_publish_result()` in `service_func` |

In **menuconfig**: Under `Thread IPC service (in-app)`, you can check **Example: business module integrating Thread IPC**.

---

## 3. Initialization Order (Must Read)

### 3.1 Before `main()` (`SYS_INIT`, `POST_KERNEL`)

Subsystem and **`module_manager_register()`** execute before **`main()`** in order of **`APP_INIT_PRIO_*`** in **`app_config.h`**, including:

```
event_system_init()
event_dispatcher_init()
…
module_manager_init()
Each example_module_*.c / business module's SYS_INIT → module_manager_register()
  → Each module's init() (can contain ipc_service_init())
app_init_finalize()
```

### 3.2 In `main()` (`app_start()`)

Order in `app_main.c` **`app_start()`**:

```
event_system_start()
event_dispatcher_start()
module_manager_start()
module_manager_start_all()    ← Calls each module's start(), can contain ipc_service_start()
```

Therefore:

- **`ipc_service_init()`** goes in module's **`init()`** (registration stage, already after `event_system_init()`), can call **`thread_ipc_event_register_types()`** here (if event bridge enabled).
- **`ipc_service_start()`** goes in module's **`start()`** (at this point **`event_system_start()` is complete**). If using event bridge to `event_publish_copy` in Worker, dispatcher is running, events won't be discarded.
- **`ipc_service_stop()`** goes in module's **`stop()`**; if other threads in module call `ipc_call_*`, must **first end or join** those threads, then `ipc_service_stop()`, avoid requests still being posted to queue during stop.

---

## 4. Steps to Integrate IPC in a Module

### 4.1 Data Structure

Embed **`ipc_service_t`** in module control block (large size, contains dual thread stacks and queue buffers):

```c
typedef struct {
    module_status_t status;
    ipc_service_t   ipc;   /* Embedded, no pointer needed */
    /* … other module state … */
} my_module_cb_t;
```

### 4.2 `ipc_service_init` Parameters

Current `ipc_service_init` has 4-parameter signature:

```c
int ipc_service_init(ipc_service_t *service, const char *name,
                    ipc_service_func_t service_func, int priority);
```

Stack size, queue capacity fixed at compile time by Kconfig:
- `CONFIG_THREAD_IPC_SERVICE_STACK_SIZE`
- `CONFIG_THREAD_IPC_SERVICE_REQUEST_QUEUE_SIZE`
- `CONFIG_THREAD_IPC_SERVICE_RESPONSE_QUEUE_SIZE`

Module only declares service name and worker thread priority.

### 4.3 Implement `ipc_service_func_t`

Executed in **Worker thread**; return value is business result code (negative errno style); output via `*out_data` / `*out_data_size`, note **buffer lifecycle** (see **[Thread IPC Service Usage Guide.md](33-thread-ipc-service-guide.md)**).

### 4.4 `start` / `stop`

- **`start`**: `ipc_service_start(&cb->ipc)`.
- **`stop`**: First stop threads/work in module that call IPC, then **`ipc_service_stop(&cb->ipc)`**.
- If **`stop` then need to `start` again**: Must re-execute **`ipc_service_init()`** (this module example handles re-`init` in `start()` for `MODULE_STATUS_STOPPED` branch).

### 4.5 Register with `module_manager`

Same as other example modules, provide **`module_interface_t`**, call **`SYS_INIT(..., APP_INIT_PRIO_MODULE_IPC)`** within **module's `.c`**, invoke **`module_manager_register(example_xxx_get_interface(), config, &module_id)`** (see `example_module_ipc.c`).

---

## 5. Reference Implementation: example_module_ipc

| Item | Description |
|------|-------------|
| Files | `src/modules_examples/example_module_ipc.c`, `include/zeplod/example_module_ipc.h` |
| Switch | `CONFIG_EXAMPLE_MODULE_THREAD_IPC=y` |
| Behavior | `init` calls `ipc_service_init`; `start` calls `ipc_service_start` and creates **demo thread**, about 300ms later executes one **`ipc_call_sync`**; `stop` first **`k_thread_join` demo thread** then `ipc_service_stop` |
| Service function | `mod_ipc_service_func`: echoes input; if **`CONFIG_THREAD_IPC_SERVICE_EVENT_BRIDGE`** enabled, then **`thread_ipc_event_publish_result(EXAMPLE_MODULE_IPC_EVENT_SOURCE_ID, …)`** |
| External Debug API | `example_module_ipc_demo_call_sync()`: Can initiate another sync call when module is RUNNING |

When **`CONFIG_EXAMPLE_MODULE_THREAD_IPC`** is enabled, the **`SYS_INIT`** in **`example_module_ipc.c`** automatically registers that module (no need to modify `app_main.c`).

---

## 6. With Event System / Event Bridge

- Subscribe to **`EVENT_TYPE_THREAD_IPC_RESPONSE`** (defined in `event_system.h`), payload type is **`thread_ipc_event_result_t`** (see `ipc_service_event.h`).
- Correspond **`EXAMPLE_MODULE_IPC_EVENT_SOURCE_ID` (42)** with `source_id` in `thread_ipc_event_publish_result`, convenient for multi-service differentiation.
- Detailed description in **[Thread IPC Service Usage Guide.md](33-thread-ipc-service-guide.md)** "Integration with Event System" section.

---

## 7. RAM and Threads

- Each **`ipc_service_t`** contains **two** `THREAD_IPC_SERVICE_STACK_SIZE` sized stacks and queue buffers, please reserve RAM.
- This example additionally increases demo thread stack `example_module_ipc_demo_stack` (**1024** bytes, see `example_module_ipc.c`).
- If RAM is tight: reduce `THREAD_IPC_SERVICE_STACK_SIZE`, queue depth, `MAX_PENDING`, demo thread stack; **do not** enable both **`CONFIG_THREAD_IPC_SERVICE_EXAMPLE`** and **`CONFIG_EXAMPLE_MODULE_THREAD_IPC`** (two demo sets overlay easily fills 192KB SRAM).
- Root **`prj.conf`** already sets **`CONFIG_HEAP_MEM_POOL_SIZE`** (Event and `event_publish_copy` need **`k_malloc`**); if still `0`, linking reports `undefined reference to k_malloc`.

---

## 8. Common Issues

| Symptom | Resolution |
|---------|------------|
| `ipc_service_init` returns `-EINVAL` | Possibly `service_func` is NULL or `priority` out of range |
| Module `start` but IPC no response | Confirm `ipc_service_start` succeeded, and Worker/Dispatcher not stopped |
| Deadlock or exception during stop | Ensure **`ipc_service_stop`** has no threads blocked in `ipc_call_*`; example ensures by **first joining demo thread** |
| Event bridge no callback | Confirm **`event_system_start()`** is earlier than module `start`; and already **`event_subscribe(EVENT_TYPE_THREAD_IPC_RESPONSE, …)`** |
| Link `undefined reference to k_malloc` | Set **`CONFIG_HEAP_MEM_POOL_SIZE`** to non-zero (e.g., 8192) in **`prj.conf`** |
| `region RAM overflowed` / `noinit will not fit` | Reduce IPC stack/queue, disable one demo module, or switch to board with larger SRAM; can align with current conservative values in `prj.conf` |
| `ipc_call_sync_shm` returns 0 but handle invalid | Check if `CONFIG_THREAD_IPC_SERVICE_SHARED_MEM` is enabled |

---

## 9. One-Click Verification Build

Based on `prj.conf` with `CONFIG_THREAD_IPC_SERVICE=y` enabled, merge **`conf/examples/module_ipc.conf`**:

```
west build -b <board> -- '-DEXTRA_CONF_FILE=conf/examples/module_ipc.conf'
```

(Note quotes and absolute paths under PowerShell.)

After successful build, running firmware should show `example_module_ipc` initialization, `Thread IPC demo: sync ok`, etc. in logs.
**Note**: When merging `conf/examples/module_ipc.conf`, it already sets **`CONFIG_THREAD_IPC_SERVICE_EXAMPLE=n`**, avoiding RAM conflict with `example_module_ipc`.

---

## Related Documentation

- [Thread IPC Service Usage Guide.md](33-thread-ipc-service-guide.md) — API, three calling modes, event bridge API
- [Event System Detailed Usage Guide.md](31-event-system-guide.md) — Subscription and publishing
