> Language: [中文](../../zh-CN/40-应用开发/41-Zephyr应用开发与服务指南.md) | **English**

# Zephyr Application Development and Service Guide

This document is for developers **writing application code on Zephyr**, covering commonly used kernel capabilities, device and configuration workflows, and considerations when writing **system-level services** (background threads, timers, synchronization, resource management). For details, refer to the current Zephyr version's official documentation; this repository's specific wrappers are documented in the **[System Services Usage Guide.md](../30-core-modules/36-system-services-guide.md)** and **[Event System Detailed Usage Guide.md](../30-core-modules/31-event-system-guide.md)**.

**Prerequisites**: [Environment Setup Guide.md](../10-environment-build/11-environment-setup.md) · [Standalone Application Build Guide.md](../10-environment-build/12-freestanding-app-build.md) · [Device Tree and Memory Configuration Manual.md](44-devicetree-memory-config.md) (as needed)

**Kernel Data Structure Details and Code Examples**: See **[§6 Zephyr Kernel Data Structures (`struct`) and Examples](#6-zephyr-kernel-data-structures-struct-and-examples)** below.

---

## 1. Layering and Responsibilities

| Layer | Typical Content | Application Developer Focus |
|-------|-----------------|---------------------------|
| **Application / Business** | `main()`, `app_main`, modules, state machines | Minimize blocking, avoid heavy work in interrupts |
| **This Template's Services** | `sys_*`, event bus, module manager | Align with `CONFIG_*`, see **[Project Configuration Guide.md](42-config-options.md)** |
| **Zephyr Kernel** | Threads, scheduling, synchronization, timers, heap | Stack size, priority, timeout |
| **Drivers / Subsystems** | GPIO, UART, I2C, Flash... | `device` handles, **`DT`** nodes, error codes |
| **Hardware** | SoC, peripherals | Clocks, pins, DMA, MPU (related to overlay) |

---

## 2. Threads and Scheduling

- **Threads**: `k_thread_create()` / `K_THREAD_DEFINE`; each thread requires an **independent stack**, configured in `prj.conf` via **`CONFIG_MAIN_STACK_SIZE`**, **`CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE`**, or custom thread attributes.
- **Priority**: Smaller values mean **higher priority** (consistent with some RTOS conventions; follow Zephyr documentation); avoid starvation from many same-priority threads, use busy-waiting cautiously.
- **Cooperative vs Preemptive**: Depends on `CONFIG_NUM_COOP_PRIORITIES` / `CONFIG_NUM_PREEMPT_PRIORITIES`; don't mix assumptions if unfamiliar.
- **System Workqueue**: `k_work` submitted to **`system workqueue`**, suitable for **non-real-time**, deferrable short tasks; don't hold locks or block for long in work handlers.

**Practice**: Long-running tasks get their own thread; hardware-coupled tasks use high-priority threads or interrupt + queue.

---

## 3. Synchronization and Communication (Common)

| Mechanism | Typical Use |
|-----------|--------------|
| **`k_mutex`** | Protect shared mutable state (between same-priority threads); lock duration should be short. |
| **`k_sem`** | Counting, signaling, thread wakeup; note initial count and upper limit. |
| **`k_msgq` / `k_fifo` / `k_pipe`** | Structured data flow, producer-consumer. |
| **`k_poll`** | Multi-event waiting (socket, fd, kernel object scenarios). |

**Note**: Only **ISR-safe** APIs can be used in **ISR** (e.g., `k_sem_give` in some scenarios); complex logic goes in threads.

---

## 4. Time and Timers

- **`k_sleep()` / `k_msleep()`**: Thread yields CPU; precision affected by tick configuration (**`CONFIG_SYS_CLOCK_TICKS_PER_SEC`**, tickless, etc.).
- **`k_timer`**: Periodic/one-shot callbacks based on kernel timer; callback context rules per documentation.
- **Timeouts**: Many blocking APIs accept **`k_timeout_t`** (e.g., `K_MSEC(n)`, `K_FOREVER`); design timeout paths to avoid deadlock.

This template's **`sys_timer`** wraps Zephyr timer capabilities; see the **[System Services Usage Guide.md](../30-core-modules/36-system-services-guide.md)**.

---

## 5. Memory

- **`k_malloc()` / `k_free()`**: Use kernel heap, size determined by **`CONFIG_HEAP_MEM_POOL_SIZE`** etc.; related to total **SRAM** budget, see **[Device Tree and Memory Configuration Manual.md](44-devicetree-memory-config.md)**.
- **`k_heap`**: Can create independent heap on static buffer or specified memory region (multi-segment RAM scenarios).
- **Stack overflow**: Enable **`CONFIG_THREAD_STACK_INFO`**, `CONFIG_THREAD_NAME` etc. for debugging; configure thread stacks appropriately.

This template's **`sys_memory`** provides multiple pools and statistics; see the **[System Services Usage Guide.md](../30-core-modules/36-system-services-guide.md)**.

---

## 6. Zephyr Kernel Data Structures (`struct`) and Examples

Below are the most commonly used **kernel object types** at the application layer (all **`struct`**, obtained via static allocation or macro definitions). API names follow the Zephyr 3.x/4.x mainline; if your version has different signatures, refer to **`include/zephyr/kernel.h`** and official documentation.

### 6.1 Quick Reference Table

| Kernel Type | Typical Use |
|-------------|-------------|
| **`struct k_thread`** | Thread control block; used with stack by `k_thread_create` / `K_THREAD_DEFINE` |
| **`struct k_mutex`** | Mutex, protect shared data |
| **`struct k_sem`** | Counting semaphore, synchronization, ISR→thread notification |
| **`struct k_msgq`** | Fixed-length message queue (copy integers or small structs) |
| **`struct k_fifo` / `struct k_lifo`** | Pointer queue (nodes need list link field, commonly used for large buffer transfer) |
| **`struct k_pipe`** | Byte stream pipe |
| **`struct k_timer`** | System clock-based one-shot/periodic timer, expiry callback |
| **`struct k_work` / `struct k_work_delayable`** | Submitted to workqueue for execution, avoid heavy work in ISR |
| **`struct k_poll_event`** | `k_poll` multi-way wait |
| **`struct k_heap`** | Heap on specified memory region (distinguishable from kernel heap used by `k_malloc`) |
| **`struct k_mem_slab`** | Fixed-size object pool, predictable alloc/free, less fragmentation |
| **`struct ring_buf`** | Ring byte buffer (commonly used for UART/streaming data, see `zephyr/sys/ring_buffer.h`) |
| **`struct k_spinlock`** | Multi-core/low-latency critical section (SMP or ISR-thread mutex with minimal latency, see official Spinlocks documentation) |

---

### 6.2 Thread `struct k_thread` and Stack

Threads must be bound to an **allocated stack**. Two common approaches:

**Method A: Macro one-shot definition of thread + stack + entry**

```c
#include <zephyr/kernel.h>

#define MY_STACK_SIZE 1024
#define MY_PRIORITY   5

static void my_thread_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		/* Thread body */
		k_msleep(100);
	}
}

K_THREAD_DEFINE(my_tid, MY_STACK_SIZE, my_thread_entry,
		NULL, NULL, NULL, MY_PRIORITY, 0, 0);
```

**Method B: Explicit `struct k_thread` + `K_THREAD_STACK_DEFINE`**

```c
static struct k_thread my_thread_data;
K_THREAD_STACK_DEFINE(my_stack, MY_STACK_SIZE);

static void start_my_thread(void)
{
	k_thread_create(&my_thread_data, my_stack,
			K_THREAD_STACK_SIZEOF(my_stack),
			my_thread_entry, NULL, NULL, NULL,
			MY_PRIORITY, 0, K_NO_WAIT);
}
```

---

### 6.3 Mutex `struct k_mutex`

```c
static struct k_mutex g_mtx;

static void mutex_demo(void)
{
	k_mutex_init(&g_mtx);

	k_mutex_lock(&g_mtx, K_FOREVER);
	/* Critical section: access shared variable */
	k_mutex_unlock(&g_mtx);

	/* With timeout, avoid deadlock */
	if (k_mutex_lock(&g_mtx, K_MSEC(10)) == 0) {
		/* ... */
		k_mutex_unlock(&g_mtx);
	}
}
```

---

### 6.4 Semaphore `struct k_sem`

```c
static K_SEM_DEFINE(my_sem, 0, 1); /* Initial 0, max 1 -- binary semaphore */

static void sem_thread(void)
{
	k_sem_take(&my_sem, K_FOREVER); /* Wait for ISR or other thread to give */
}

/* Notify thread from ISR (example) */
static void my_isr(const void *arg)
{
	ARG_UNUSED(arg);
	k_sem_give(&my_sem);
}
```

---

### 6.5 Message Queue `struct k_msgq`

Suitable for passing **fixed-length** small messages (e.g., `uint32_t`, small `struct`):

```c
#define MSG_WORDS 4
#define MSG_COUNT 8

K_MSGQ_DEFINE(my_msgq, sizeof(uint32_t), MSG_COUNT, 4);

static void msgq_producer(void)
{
	uint32_t v = 0xAA;

	if (k_msgq_put(&my_msgq, &v, K_MSEC(10)) != 0) {
		/* Queue full */
	}
}

static void msgq_consumer(void)
{
	uint32_t out;

	if (k_msgq_get(&my_msgq, &out, K_FOREVER) == 0) {
		/* Use out */
	}
}
```

---

### 6.6 FIFO `struct k_fifo` (Pointer Passing)

Queue elements must be **structs containing list node field**, and nodes are typically allocated from **slab / heap**:

```c
struct msg_node {
	void *fifo_reserved;   /* Kernel use, must be first member */
	int payload;
};

static struct k_fifo g_fifo;

static void fifo_init(void)
{
	k_fifo_init(&g_fifo);
}

static void fifo_send(int data)
{
	struct msg_node *n = k_malloc(sizeof(*n));

	if (n == NULL) {
		return;
	}
	n->payload = data;
	k_fifo_put(&g_fifo, n);
}

static void fifo_recv_thread(void)
{
	struct msg_node *n;

	for (;;) {
		n = k_fifo_get(&g_fifo, K_FOREVER);
		/* Process n->payload */
		k_free(n);
	}
}
```

---

### 6.7 Timer `struct k_timer`

```c
static void on_timer_expiry(struct k_timer *t);

K_TIMER_DEFINE(my_timer, on_timer_expiry, NULL);

static void on_timer_expiry(struct k_timer *t)
{
	ARG_UNUSED(t);
	/* Note: callback context rules per official documentation; complex logic can be deferred to k_work */
}

static void timer_start_demo(void)
{
	/* First trigger after 100ms, then periodic 500ms */
	k_timer_start(&my_timer, K_MSEC(100), K_MSEC(500));
}
```

---

### 6.8 Work Queue `struct k_work` / `struct k_work_delayable`

Move time-consuming or thread-context-requiring logic from **ISR / timer callback** to **system workqueue**:

```c
static void my_work_handler(struct k_work *w);

K_WORK_DEFINE(my_work, my_work_handler);

static void my_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	/* Execute in thread context */
}

static void trigger_from_isr_or_thread(void)
{
	k_work_submit(&my_work);
}
```

**Delayed execution** (debounce, coalescing triggers) can use **`k_work_delayable`**: first **`k_work_init_delayable`**, then **`k_work_schedule`** / **`k_work_reschedule`** (specific APIs per current version `kernel.h`).

---

### 6.9 Private Heap `struct k_heap`

Create heap on static array or SRAM segment, isolated from global **`k_malloc`**:

```c
static struct k_heap app_heap;
static uint8_t heap_mem[4096] __aligned(8);

static void app_heap_init(void)
{
	k_heap_init(&app_heap, heap_mem, sizeof(heap_mem));
}

static void *app_alloc(size_t n)
{
	return k_heap_alloc(&app_heap, n, K_NO_WAIT);
}

static void app_free(void *p)
{
	k_heap_free(&app_heap, p);
}
```

---

### 6.10 Fixed-Size Memory Pool `struct k_mem_slab`

Better than general-purpose heap for **fixed block size, low fragmentation** scenarios (e.g., network packet descriptors, sensor frames):

```c
#define BLK_SZ   64
#define BLK_NUM  8

K_MEM_SLAB_DEFINE(my_slab, BLK_SZ, BLK_NUM, 4);

static void slab_demo(void)
{
	void *blk;

	if (k_mem_slab_alloc(&my_slab, &blk, K_FOREVER) == 0) {
		/* Use BLK_SZ bytes at (uint8_t *)blk */
		k_mem_slab_free(&my_slab, blk);
	}
}
```

---

### 6.11 `k_poll` (Multi-Event Waiting, Simple Example)

Used to wait for multiple kernel objects ready simultaneously (e.g., semaphore + FIFO), suitable for network/multi-channel IO; **complete usage** (multi-event, `K_POLL_STATE_*` checking) see **[Kernel — Polling](https://docs.zephyrproject.org/latest/kernel/services/polling.html)**. Below demonstrates **single semaphore** waiting only, assuming **`static K_SEM_DEFINE(my_sem, ...)`** already exists.

```c
struct k_poll_event events[1];

k_poll_event_init(&events[0], K_POLL_TYPE_SEM_AVAILABLE,
		  K_POLL_MODE_NOTIFY_ONLY, &my_sem);

/* Wait for ready, timeout 100ms; return 0 means not timed out, then check events[i].state */
k_poll(events, ARRAY_SIZE(events), K_MSEC(100));
```

(If compiler lacks **`ARRAY_SIZE`**, can write **`1`** or include **`<zephyr/sys/util.h>`**.)

---

## 7. Logging (Zephyr `LOG`)

- **`LOG_MODULE_REGISTER(name)`** in module, use **`LOG_INF` / `LOG_ERR`** etc.; level controlled by **`CONFIG_LOG`** and module **`LOG_LEVEL`**.
- Default output depends on **UART/console** and **`CONFIG_LOG`**, **`CONFIG_CONSOLE`** etc. in **`prj.conf`**.
- High-throughput scenarios pay attention to **stack** and **buffer**; production can disable debug level to reduce Flash/RAM.

Relationship with template **`sys_log`**: can uniformly wrap at business layer for memory ring buffer and module name fields; both can coexist, but **avoid duplicate spamming**.

---

## 8. Shell (Optional)

After enabling **`CONFIG_SHELL`** and related subcommands, interactive commands available via serial for debugging. Occupies **Flash/RAM** and **one session context**; release images often disable or trim command set.

---

## 9. Devices and Drivers (Application-Side Usage)

1. When Devicetree node has **`status = "okay"`** and driver is enabled, use **`DEVICE_DT_GET()`** / **`DEVICE_DT_GET_ONE()`** etc. in C code to get **`const struct device *`**.
2. Before use, call **`device_is_ready(dev)`**.
3. Call **`gpio_pin_configure`**, **`i2c_transfer`** etc. passing **`dev`** and pin/channel specified by DT macros (**`GPIO_DT_SPEC_GET`** etc.).
4. Error codes are mostly **negative errno** (`-EIO`, `-ETIMEDOUT` etc.), need to propagate or log.

### 9.1 Example: GPIO (`gpio_dt_spec`)

Common pattern below: assumes board-level DTS provides **`led0`** alias (many Nucleo boards do; if not, need to use your board's node label or **`DT_NODELABEL`**).

```c
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#define LED_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

static int board_led_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	return ret;
}
```

**`gpio_pin_configure_dt`** simultaneously binds **device and pin**; if using manual **`DEVICE_DT_GET(DT_PARENT(...))`** etc., need to pass **`dev`** and **`pin`** yourself. Application-level nodes and **`app.overlay`** see **[Device Tree and Memory Configuration Manual.md](44-devicetree-memory-config.md)**.

**Device Tree**: Application-level modifications prefer **`app.overlay`**, see **[Device Tree and Memory Configuration Manual.md](44-devicetree-memory-config.md)**.

---

## 10. Kconfig and `prj.conf` Workflow

- **`prj.conf`**: Write **`CONFIG_*=y`** or assignments; merged into final **`.config`**.
- **`Kconfig`** (application root etc.): Defines menu items visible to this application; uses **`rsource`** to split into subdirectories.
- If behavior is abnormal after modification: **`west build -p always`** or **`pristine`**; complex items use **`menuconfig` / `guiconfig`**.

This application's extended options explained in **[Project Configuration Guide.md](42-config-options.md)**.

---

## 11. Interrupts and ISR

- ISRs must be **extremely short**: clear flags, copy data to buffer, **release semaphore/submit work** etc.
- If SoC supports **priority nesting**, pay attention to critical sections and **`irq_lock`** coordination.
- **Never** call potentially blocking or heap-allocating APIs in ISR (unless documentation explicitly states ISR-safe).

**ISR-Safe API Examples**:
| API | ISR-Safe Condition | Reason |
|-----|-------------------|--------|
| `k_msgq_put(K_NO_WAIT)` | ✅ Safe | Internal spinlock |
| `k_sem_give()` | ✅ Safe | Non-blocking |
| `k_mutex_lock()` | ❌ Forbidden | May block/schedule |
| `k_malloc()` | ⚠️ Avoid | May not be reentrant |

> Technical detail: Spinlock only disables interrupts without triggering scheduling, while Mutex may block and trigger scheduling. See **[Framework Core Technology Implementation Details.md](../20-architecture/23-framework-internals.md#24-isr-safe-publishing)**.

---

## 12. Common Patterns When Writing "Services"

"Service" here refers to long-running, externally-provided API backend capabilities (similar to this template's **`sys_*`**, IPC worker threads):

1. **Clear initialization phase**: Prefer **`SYS_INIT`** (this template's **`app_main.c`** and various **`example_module_*.c`** already use this, priorities see **`app_config.h`**'s **`APP_INIT_PRIO_*`**); return errors should be recoverable or logged.
2. **Resource ownership**: Heap, static buffers, handle lifecycle clarity; stop paths must **release/unregister** (if applicable).
3. **Thread model**: Single-threaded serial processing vs multi-threaded; if multi-threaded sharing state, **unified mutex or message queue**, avoid naked global lock-free.
4. **Event system collaboration**: Slow business goes through **publish-subscribe**, don't block IPC/network in callbacks.
5. **Configurability**: Expose stack size, queue depth, and timeout via **`Kconfig`** so different boards can trim resource use.

---

## 13. Connection with This Repository's Documentation

| Topic | Document |
|-------|----------|
| This template's `sys_*` | [System Services Usage Guide.md](../30-core-modules/36-system-services-guide.md) |
| Events and modules | [Event System Detailed Usage Guide.md](../30-core-modules/31-event-system-guide.md) · [Module System Detailed Usage Guide.md](../30-core-modules/32-module-system-guide.md) |
| Thread IPC | [Thread IPC Service Usage Guide.md](../30-core-modules/33-thread-ipc-service-guide.md) |
| Memory layout / overlay | [Device Tree and Memory Configuration Manual.md](44-devicetree-memory-config.md) |
| OTA / Storage / PM | [OTA and Storage Extension Guide.md](../70-release-productization/74-ota-storage-guide.md) |

---

## 14. Recommended Reading (Official)

- [Kernel Services](https://docs.zephyrproject.org/latest/kernel/services/index.html)
- [Device Driver Model](https://docs.zephyrproject.org/latest/kernel/drivers/index.html)
- [Build and Configuration Systems](https://docs.zephyrproject.org/latest/build/index.html)
- [Application Development](https://docs.zephyrproject.org/latest/application/index.html)

---

*This document evolves with Zephyr major versions; if API differs from official, the official takes precedence.*
