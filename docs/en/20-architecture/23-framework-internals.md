> Language: [中文](../../zh-CN/20-架构设计/23-框架核心技术实现细节.md) | **English**

# Framework Core Technology Implementation Details

This document provides detailed explanation of all core technology implementation details in the Zephyr event-driven framework, suitable for:
- **Interview Preparation**: Deep understanding of design decisions and technical details
- **Architecture Learning**: Learning embedded framework design methods
- **Code Review**: Understanding design intent behind code

---

## Table of Contents

1. [Overall Architecture Overview](#1-overall-architecture-overview)
2. [Event System Implementation Details](#2-event-system-implementation-details)
3. [Module Manager Implementation Details](#3-module-manager-implementation-details)
4. [Memory Management System Implementation Details](#4-memory-management-system-implementation-details)
5. [Thread Safety and Concurrency Control](#5-thread-safety-and-concurrency-control)
6. [Performance Considerations and Tradeoffs](#6-performance-considerations-and-tradeoffs)
7. [Interview Q&A](#7-interview-qa)

---

## 1. Overall Architecture Overview

### 1.1 Layered Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Application Layer                        │
│              Business modules, sensor processing,            │
│              communication protocols                         │
├─────────────────────────────────────────────────────────────┤
│                     Modules Layer                           │
│         GPIO module, UART module, IPC module,               │
│         custom business modules                             │
├─────────────────────────────────────────────────────────────┤
│                     Services Layer                          │
│      Event system, module manager, memory service,          │
│      logging service, timer service                         │
├─────────────────────────────────────────────────────────────┤
│                   Abstraction Layer                         │
│              module_base.h, event_system.h                  │
├─────────────────────────────────────────────────────────────┤
│                   Zephyr RTOS Kernel                        │
│         k_thread, k_mutex, k_msgq, k_malloc, k_timer       │
└─────────────────────────────────────────────────────────────┘
```

### 1.2 Core Component Relationships

```
                    ┌──────────────────┐
                    │   Module Manager │
                    │ module_manager   │
                    └────────┬─────────┘
                             │ registration/lifecycle management
              ┌──────────────┼──────────────┐
              ▼              ▼              ▼
        ┌──────────┐   ┌──────────┐   ┌──────────┐
        │ Module A │   │ Module B │   │ Module C │
        └────┬─────┘   └────┬─────┘   └────┬─────┘
             │              │              │
             │    Event publish/subscribe  │
             └──────────────┼──────────────┘
                            ▼
                   ┌──────────────────┐
                   │    Event System  │
                   │  event_system    │
                   └────────┬─────────┘
                            │ memory allocation
                            ▼
                   ┌──────────────────┐
                   │   Memory Service │
                   │   sys_memory     │
                   └──────────────────┘
```

### 1.3 Design Principles

| Principle | Implementation |
|-----------|----------------|
| **Decoupling** | Event-driven communication, no direct calls between modules |
| **Thread Safety** | Mutex + atomic variables + snapshot technology |
| **Controllable Resources** | Static memory pool + configurable limits |
| **ISR Safe** | Dedicated ISR API + spinlock protection (not mutex) |
| **Extensibility** | String dependency declaration + runtime resolution |

---

## 2. Event System Implementation Details

### 2.1 Core Data Structures

#### 2.1.1 Event Structure

```c
// event_system.h:177-189
typedef struct {
    uint8_t          type;           // Event type (0-255)
    uint8_t          priority;       // Event priority (0=CRITICAL, 2=HIGH, 5=NORMAL, 10=LOW)
    uint8_t          flags;          // Flags (EVENT_FLAG_DATA_INLINE/DYNAMIC/FROM_SLAB)
    uint8_t          reserved;       // Reserved for extension
    uint32_t         timestamp;      // Creation timestamp (ms uptime)
    uint32_t         source_id;      // Source module ID
    uint32_t         data_len;       // Data length
    union {
        uint8_t  inline_data[CONFIG_EVENT_INLINE_DATA_SIZE]; // Inline data (default 48B)
        void*    ptr;                                             // External data pointer
    } data;
} event_t;
```

**Memory Layout** (with 64-byte configuration):
```
┌────────────────────────────────┐
│ type(1) priority(1) flags(1) ? │  4B
│ timestamp                      │  4B
│ source_id                      │  4B
│ data_len                       │  4B
├────────────────────────────────┤  16B header
│ inline_data[48] or ptr(8)      │ 48B
└────────────────────────────────┘  64B total
```

**Design Considerations**:
- `event_t` has fixed size (controlled by `CONFIG_EVENT_STRUCT_SIZE`, configurable 32/64/128 bytes)
- `data_len ≤ INLINE_DATA_SIZE` (default 48B): Data stored inline in `inline_data`, zero extra allocation
- `data_len > INLINE_DATA_SIZE`: Allocated from Slab pool or k_malloc, pointer stored in `data.ptr`
- Supports Slab mode (`EVENT_FLAG_FROM_SLAB`) for real-time safe allocation

#### 2.1.2 Subscriber Entry

```c
// event_system.h:213-218
typedef struct {
    event_callback_t callback;      // Callback function pointer
    void*            user_data;     // User data
    uint32_t         subscriber_id; // Unique subscriber ID (system allocated)
    bool             is_active;     // Whether active
} subscriber_entry_t;
```

#### 2.1.3 Event Type Registry

```c
// event_system.h:225-231
typedef struct {
    event_type_t       type;
    const char*        name;                                      // Debug name
    subscriber_entry_t subscribers[CONFIG_EVENT_MAX_SUBSCRIBERS]; // Subscriber array
    uint32_t           subscriber_count;
    struct k_mutex     lock;                                      // Protect subscriber list
} event_type_entry_t;
```

#### 2.1.4 Event System Control Block

```c
// event_system.c:56-65
typedef struct {
    uint32_t           magic;                        // Magic number validation (EVENT_SYSTEM_MAGIC)
    bool               initialized;                  // Initialization flag
    atomic_t           running;                      // Running state (atomic, ISR readable)
    struct k_msgq*     event_queue;                  // Event queue
    event_type_entry_t event_types[MAX_EVENT_TYPES]; // Event type table
    uint32_t           total_events;                 // Stats: events processed count
    struct k_mutex     stats_lock;                   // Stats lock
    uint32_t           next_subscriber_id;           // ID allocator
} event_system_cb_t;
```

### 2.2 Event Publish Flow

```
┌─────────────┐    event_publish()    ┌─────────────┐
│ Publisher   │ ───────────────────▶  │  Event Queue│
│   Thread   │                       │   k_msgq    │
└─────────────┘                       └──────┬──────┘
                                              │
                                              │ k_msgq_get()
                                              ▼
                                       ┌─────────────┐
                                       │ Dispatcher  │
                                       │   Thread    │
                                       └──────┬──────┘
                                              │
                                              │ event_notify_subscribers()
                                              ▼
                                       ┌─────────────┐
                                       │ Subscriber  │
                                       │  Callbacks  │
                                       └─────────────┘
```

### 2.3 Queue Full Handling Strategy

```c
// event_system.c:480-486
event_status_t event_publish(event_t* event) {
    // Use K_NO_WAIT, non-blocking
    int ret = k_msgq_put(g_event_system.event_queue, event, K_NO_WAIT);
    if (ret != 0) {
        // Atomic counter records dropped event
        atomic_fetch_add_explicit(&g_event_dropped_count, 1U, memory_order_relaxed);
        LOG_WRN("Event queue full, event dropped (type=%d)", event->type);
        return EVENT_ERR_QUEUE_FULL;
    }
    return EVENT_OK;
}
```

**Why choose drop over blocking?**

| Consideration | Blocking Wait | Drop Event |
|---------------|---------------|------------|
| Real-time | ❌ May cause high-priority thread blocking | ✅ Returns immediately |
| Predictability | ❌ Delay unpredictable | ✅ Behavior deterministic |
| Memory | Needs larger queue | Controllable memory |
| Caller Awareness | ❌ Hidden pressure | ✅ Explicit failure, can degrade |

### 2.4 ISR-Safe Publishing

```c
// event_system.c:497-513
event_status_t event_publish_from_isr(const event_t* event) {
    // 1. Don't use mutex (forbidden in ISR)
    // 2. Use atomic_get to read state (ISR safe)
    if (atomic_get(&g_event_system.running) == 0) {
        return EVENT_ERR_INVALID_ARG;
    }

    // 3. k_msgq_put with K_NO_WAIT is ISR safe
    //    Uses spinlock internally (not mutex), allowed in ISR
    int ret = k_msgq_put(g_event_system.event_queue, event, K_NO_WAIT);
    if (ret != 0) {
        atomic_fetch_add_explicit(&g_event_dropped_count, 1U, memory_order_relaxed);
        return EVENT_ERR_QUEUE_FULL;
    }
    return EVENT_OK;
}
```

#### Key Question: `k_msgq_put` has locks internally, why can it be called in ISR?

**Answer: `k_msgq` uses spinlock, not mutex**

| Lock Type | Allowed in ISR | Reason |
|-----------|----------------|--------|
| **Mutex** | ❌ Forbidden | May block, trigger scheduling, ISR cannot be scheduled |
| **Spinlock** | ✅ Allowed | Only disables local interrupt, won't trigger scheduling |

**Why spinlock is safe in ISR:**
```
1. Spinlock only temporarily disables local CPU interrupt
2. ISR itself is in interrupt context, disabling interrupt causes no problem
3. Lock hold time extremely short (only protects data structure operation)
4. Won't cause context switch or thread scheduling
```

**Why mutex is dangerous in ISR:**
```
1. Mutex may cause thread blocking
2. Blocking means need to schedule other thread to run
3. ISR is not a thread, cannot be "scheduled away"
4. Trying to block will cause system crash
```

> **Zephyr Official Documentation**:
> "This routine is safe to call from an ISR if the timeout is K_NO_WAIT."
> —— [Message Queue API](https://docs.zephyrproject.org/latest/kernel/services/data_passing/message_queues.html)

**ISR Restrictions Summary:**

| Operation | Thread Context | ISR Context | Reason |
|-----------|---------------|-------------|--------|
| Mutex | ✅ Allowed | ❌ Forbidden | May block/schedule |
| Spinlock | ✅ Allowed | ✅ Allowed | Only disables interrupt |
| Dynamic memory | ✅ Allowed | ⚠️ Avoid | Non-reentrant risk |
| Blocking wait | ✅ Allowed | ❌ Forbidden | ISR cannot schedule |
| Atomic operations | ✅ Allowed | ✅ Allowed | Hardware supported |
| `k_msgq_put(K_NO_WAIT)` | ✅ Allowed | ✅ Allowed | Internally uses spinlock |

### 2.5 Subscriber Notification (Snapshot Technology)

```c
// event_system.c:727-781
event_status_t event_notify_subscribers(const event_t* event) {
    // Snapshot structure: save subscriber callback and user data
    typedef struct {
        event_callback_t cb;
        void*            ud;
    } sub_snap_t;

    sub_snap_t snap[CONFIG_EVENT_MAX_SUBSCRIBERS];
    uint32_t   n = 0U;

    k_mutex_lock(&entry->lock, K_FOREVER);

    // Copy active subscriber info to snapshot
    for (uint32_t i = 0; i < CONFIG_EVENT_MAX_SUBSCRIBERS; i++) {
        subscriber_entry_t* sub = &entry->subscribers[i];
        if (sub->is_active && sub->callback != NULL) {
            snap[n].cb = sub->callback;
            snap[n].ud = sub->user_data;
            n++;
        }
    }

    k_mutex_unlock(&entry->lock);

    // Call all callbacks outside lock (avoid deadlock)
    for (uint32_t i = 0; i < n; i++) {
        snap[i].cb(event, snap[i].ud);
    }

    return EVENT_OK;
}
```

**Why use snapshot technology?**

```
Problem scenario:
  Callbacks may call event_subscribe/unsubscribe
  If holding lock while calling callback → deadlock

Solution:
  1. Only copy callback pointers while holding lock
  2. Release lock before calling callbacks
  3. Callbacks can safely call any event API
```

### 2.6 Event Data Memory Management

Event system supports three data storage strategies:

#### 2.6.1 Inline Data (Small data, zero allocation)

```c
// data_len <= CONFIG_EVENT_INLINE_DATA_SIZE (default 48B)
uint8_t small_data[16];

// Use event_publish_copy, inline storage with no extra allocation
event_publish_copy(EVENT_TYPE_SENSOR_DATA, EVENT_PRIORITY_NORMAL, 
                   small_data, sizeof(small_data));
// System automatically sets EVENT_FLAG_DATA_INLINE flag
```

#### 2.6.2 Slab Pool Allocation (Real-time Safe)

```c
// Entirely from slab pool, O(1) deterministic time, suitable for ISR and real-time tasks
event_t* event = event_create_with_data_rt(
    EVENT_TYPE_SENSOR_DATA,
    EVENT_PRIORITY_HIGH,
    data,
    data_len
);

// Or publish directly
event_publish_copy_rt(EVENT_TYPE_SENSOR_DATA, EVENT_PRIORITY_HIGH, 
                      data, sizeof(data));

// Slab pool architecture:
// - Event Slab: CRITICAL/HIGH/NORMAL priority stratified
// - Data Slab: 256B/1KB/4KB size selection
// - Returns NULL when exhausted, no fallback to k_malloc
```

#### 2.6.3 Heap Allocation (Normal Mode)

```c
// Uses k_malloc, may block
event_t* event = event_create_with_data(type, priority, data, data_len);
event_publish(event);
// Note: event_t shell needs manual free
if (ret != EVENT_OK) {
    event_free(event);  // Manual free on publish failure
}
// Or use convenience function (auto management)
event_publish_copy(type, priority, data, data_len);  // Recommended
```

#### Memory Management Convention

| Publish Method | event_t Source | Data Storage | Who frees data | Who frees event_t |
|----------------|---------------|--------------|----------------|-------------------|
| `event_publish_copy` | Internal temp | Inline/Slab/Heap | Dispatcher thread | Function internal |
| `event_publish_copy_rt` | Slab | Inline/Slab | Dispatcher thread | Function internal |
| `event_publish` + stack event_t | Stack | Inline | None | Stack auto |
| `event_publish` + `event_create*` | Slab/Heap | Inline/Slab/Heap | Dispatcher thread | **Module must free** |

### 2.7 Event Queue Implementation (event_queue)

Event queue is based on Zephyr `k_msgq`, storing complete `event_t` structures.

```c
// event_queue.h
typedef struct {
    uint32_t enqueue_count;  // Enqueue operation count
    uint32_t dequeue_count; // Dequeue operation count
    uint32_t overflow_count; // Overflow occurrence count
    uint32_t drop_count;    // Event drop count
    uint32_t high_watermark; // Queue depth historical maximum
} queue_stats_t;
```

**Key Features**:
- Queue full supports three overflow strategies: `DROP_LOWEST`, `DROP_NEWEST`, `BLOCK`
- High watermark alarm (default 75% queue capacity)
- Auto free dynamic data payload (when clearing queue)

### 2.8 Event Dispatcher Implementation (event_dispatcher)

Dispatcher is an independent thread that dequeues events and notifies subscribers.

```c
// event_dispatcher.h
typedef enum {
    DISPATCHER_STOPPED = 0, // Dispatcher stopped
    DISPATCHER_RUNNING,     // Dispatcher running
    DISPATCHER_PAUSED,      // Dispatcher paused
    DISPATCHER_ERROR        // Dispatcher error state
} dispatcher_state_t;

typedef struct {
    uint64_t events_processed;  // Total events processed
    uint64_t events_dropped;    // Events dropped
    uint32_t max_latency_us;    // Max processing latency (microseconds)
    uint32_t avg_latency_us;    // Avg processing latency (microseconds)
    uint32_t processing_errors; // Processing error count
} dispatcher_stats_t;
```

**Advanced Features**:
- **Pause/Resume**: Supports dynamic pause of event processing (`event_dispatcher_pause/resume`)
- **Event Filtering**: Can register filter functions to decide which events are processed
- **Manual Batch Processing**: Supports manual calling of `process_one` or `process_all`
- **Latency Statistics**: Real-time monitoring of event processing latency

### 2.9 Event System Compatibility Layer (event_system_compat)

Application lifecycle adapter that wraps the standard event system:

```c
event_compat_init(NULL);
event_compat_start();
event_compat_get_statistics(&stats);
event_compat_stop();
```

Registration, subscription, and publishing use the standard APIs from `event_system.h`.
The commercial event backend and dual-backend switching have been removed.

---

## 3. Module Manager Implementation Details

### 3.1 Core Data Structures

#### 3.1.1 Module Interface Definition

```c
// module_base.h
typedef struct {
    const char* name;              // Module name
    const char* const* depends_on; // Dependency module name array (string form)
    uint32_t version;              // Module version
    module_priority_t priority;    // Startup priority

    // Lifecycle callbacks
    int (*init)(void* config);
    int (*start)(void);
    int (*stop)(void);
    int (*shutdown)(void);

    // Event handling
    void (*on_event)(const event_t* event, void* user_data);
} module_interface_t;
```

#### 3.1.2 Module Info Structure

```c
// module_base.h
typedef struct {
    const module_interface_t* interface;   // Interface pointer
    void* config;                          // Configuration data
    void* internal_data;                   // Module private data
    module_status_t status;                // Current status
    uint32_t id;                           // Unique ID

    // Event subscription management
    struct {
        event_type_t type;
        uint32_t subscriber_id;
    } event_subscriptions[CONFIG_MODULE_MAX_EVENT_SUBSCRIPTIONS];
    uint8_t event_subscription_count;
} module_info_t;
```

#### 3.1.3 Module Manager Control Block

```c
// module_manager.c:73-83
typedef struct {
    module_info_t         modules[CONFIG_MAX_MODULES];
    uint32_t              module_count;
    uint32_t              next_module_id;
    module_mgr_stats_t    stats;
    module_mgr_callback_t callback;        // Status change callback
    void*                 callback_user_data;
    struct k_mutex        lock;
    bool                  initialized;
    bool                  running;
} module_manager_cb_t;
```

### 3.2 Module Lifecycle State Machine

```
                    register()
UNINITIALIZED ────────────────▶ INITIALIZING
     ▲                              │
     │                              │ init() success
     │                              ▼
     │                         INITIALIZED
     │                              │
     │                       start()│
     │                              ▼
     │                          RUNNING ◀────── resume()
     │                           │  │
     │                     stop()│  │suspend()
     │                           │  ▼
     │                           │  SUSPENDED
     │                           │
     │                           ▼
     └─────────────────────── STOPPED
                    unregister()
```

### 3.3 Dependency Management (Runtime Topological Sort)

#### 3.3.1 Dependency Declaration

```c
// Module declares dependencies (string form, no header files needed at compile time)
static const char* const module_c_deps[] = {"module_a", "module_b", NULL};

static const module_interface_t module_c_interface = {
    .name = "module_c",
    .depends_on = module_c_deps,  // Depends on A and B
    .priority = MODULE_PRIORITY_NORMAL,
    .init = module_c_init,
    // ...
};
```

#### 3.3.2 Startup Topological Sort Algorithm

```c
// module_manager.c:325-513
static int dependency_order_start_batch(start_order_entry_t* entries, int n) {
    // Phase 1: Fixed-point verification (remove illegal dependencies)
    for (;;) {
        // Verify each dependency:
        // - Is RUNNING, or
        // - Is in this batch's start set
        // Modules with illegal dependencies are marked invalid

        // Modules whose dependencies were removed will also be removed next round
        // Until set size no longer changes (fixed point)

        if (n2 == n_work) break;  // Converged
        n_work = n2;
    }

    // Phase 2: Build graph
    // Directed edge j→i means i depends on j (j must start before i)

    // Phase 3: Kahn's algorithm topological sort
    // At each step, select node with in-degree 0 and lowest priority

    // Phase 4: Detect cycles
    // If nodes still have non-zero in-degree, cycle exists, fallback to priority sort
}
```

#### 3.3.3 Startup Order Example

```
Module definitions:
  Module A: priority=1, depends_on=[]
  Module B: priority=2, depends_on=["module_a"]
  Module C: priority=1, depends_on=["module_a", "module_b"]

Dependency graph:
  A ──▶ B ──▶ C
  └────────▶ C

Topological sort result:
  Startup order: A → B → C
  (Even though C's priority is same as A, C depends on A, so A starts first)
```

#### 3.3.4 Stop Order (Reverse Topological Order)

```c
// module_manager.c:519-622
static int dependency_order_stop_batch(start_order_entry_t* entries, int n) {
    // 1. Get same topological order as startup
    // 2. Reverse entire order
    // Result: Modules that are depended on stop later

    // Example: Startup A→B→C, Stop C→B→A
}
```

### 3.4 Module Registration Process

```c
// module_manager.c:792-912
int module_manager_register(const module_interface_t* interface, void* config, uint32_t* module_id) {
    // 1. Validate parameters
    if (interface == NULL || interface->init == NULL) {
        return -EINVAL;
    }

    // 2. Check module count limit
    if (g_module_mgr.module_count >= CONFIG_MAX_MODULES) {
        return -ENOMEM;
    }

    // 3. Check for duplicate names
    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        if (strcmp(modules[i].interface->name, interface->name) == 0) {
            return -EALREADY;
        }
    }

    // 4. Allocate slot
    module_info_t* info = find_free_slot();

    // 5. Call init() (while holding lock)
    const uint32_t t0 = k_uptime_get_32();
    int ret = interface->init(config);
    const uint32_t elapsed = k_uptime_get_32() - t0;

    // 6. Check init timeout
    if (elapsed > CONFIG_MODULE_INIT_TIMEOUT_MS) {
        // Call shutdown to cleanup
        if (interface->shutdown) interface->shutdown();
        return -1;
    }

    // 7. Update status
    info->status = MODULE_STATUS_INITIALIZED;
    g_module_mgr.module_count++;

    return 0;
}
```

### 3.5 Lock-Free Call Pattern

```c
// This pattern is heavily used in module manager to avoid deadlock

int module_manager_start_module(uint32_t module_id) {
    int (*start_fn)(void);
    const char* name;

    // Phase 1: Get function pointer while holding lock
    k_mutex_lock(&g_module_mgr.lock, K_FOREVER);
    module_info_t* info = find_module_by_id_locked(module_id);
    start_fn = info->interface->start;
    name = info->interface->name;
    k_mutex_unlock(&g_module_mgr.lock);

    // Phase 2: Call outside lock (avoid reentrant deadlock)
    int ret = 0;
    if (start_fn != NULL) {
        ret = start_fn();
    }

    // Phase 3: Update status while holding lock
    k_mutex_lock(&g_module_mgr.lock, K_FOREVER);
    info = find_module_by_id_locked(module_id);  // Re-lookup
    if (ret == 0) {
        info->status = MODULE_STATUS_RUNNING;
    }
    k_mutex_unlock(&g_module_mgr.lock);

    return ret;
}
```

---

## 4. Memory Management System Implementation Details

### 4.1 Core Data Structures

#### 4.1.1 Memory Pool Types

```c
// sys_memory.h:47-53
typedef enum {
    SYS_MEM_POOL_GENERAL = 0, // General memory pool
    SYS_MEM_POOL_EVENT,       // Event memory pool
    SYS_MEM_POOL_MODULE,      // Module memory pool
    SYS_MEM_POOL_DMA,         // DMA memory pool
    SYS_MEM_POOL_COUNT
} sys_mem_pool_type_t;
```

#### 4.1.2 Allocation Block Header

```c
// sys_memory.c:65-72
typedef struct alloc_header {
    uint32_t magic;          // Magic: MEMORY_MAGIC or MEMORY_FREED_MAGIC
    uint32_t pool_type;      // Belongs to which memory pool type
    size_t   requested_size; // User-requested size (original)
    size_t   aligned_size;   // Actual allocated size (aligned)
    /* User data follows immediately after */
} alloc_header_t;
```

#### 4.1.3 Free Block Structure

```c
// sys_memory.c:57-61
typedef struct free_block {
    struct free_block* next; // Next free block
    size_t             size; // Free block total size (including header)
} free_block_t;
```

#### 4.1.4 Memory Pool Control Structure

```c
// sys_memory.c:77-89
typedef struct mem_pool {
    uint8_t*            buffer;      // Memory buffer start
    size_t              total_size;  // Total size
    free_block_t*       free_list;   // Free list head
    size_t              used_size;   // Used size
    size_t              max_used;    // Peak usage
    uint32_t            alloc_count; // Allocation count
    uint32_t            free_count;  // Free count
    uint32_t            fail_count;  // Failure count
    sys_mem_pool_type_t type;
    struct k_mutex      lock;
    bool                initialized;
} mem_pool_t;
```

### 4.2 Memory Allocation Algorithm (First-Fit)

```c
// sys_memory.c:355-446
static void* pool_alloc_locked(mem_pool_t* pool, size_t req_size, bool zero, ...) {
    // 1. Align requested size
    size_t aligned_req;
    align_up_size(req_size, MEMORY_ALIGN_BYTES, &aligned_req);

    // 2. Calculate total need (header + data)
    size_t total_needed = sizeof(alloc_header_t) + aligned_req;

    // 3. First-Fit search
    free_block_t* prev = NULL;
    free_block_t* curr = pool->free_list;
    while (curr != NULL) {
        if (curr->size >= total_needed) break;
        prev = curr;
        curr = curr->next;
    }

    if (curr == NULL) {
        pool->fail_count++;
        return NULL;
    }

    // 4. Split block (if remaining space is large enough)
    size_t remaining = curr->size - total_needed;
    if (remaining >= MIN_BLOCK_SIZE) {
        // Cut from end of current block
        curr->size = remaining;
        block_start = (uint8_t*) curr + remaining;
    } else {
        // Allocate entire block
        if (prev == NULL) {
            pool->free_list = curr->next;
        } else {
            prev->next = curr->next;
        }
    }

    // 5. Initialize allocation header
    alloc_header_t* header = (alloc_header_t*) block_start;
    header->magic = MEMORY_MAGIC;
    header->pool_type = pool->type;
    header->requested_size = req_size;
    header->aligned_size = aligned_req;

    return block_start + sizeof(alloc_header_t);
}
```

### 4.3 Memory Freeing and Coalescing

```c
// sys_memory.c:451-491
static void pool_free_locked(mem_pool_t* pool, void* ptr, alloc_header_t* header) {
    // 1. Detect double free
    if (header->magic != MEMORY_MAGIC) {
        LOG_WRN("Double free or corrupted header: ptr=%p", ptr);
        return;
    }

    // 2. Mark as freed
    header->magic = MEMORY_FREED_MAGIC;

    // 3. Add to free list (sorted by address)
    free_block_t* new_free = (free_block_t*) header;
    // ... insert at correct position ...

    // 4. Coalesce adjacent free blocks
    coalesce_free_blocks(pool);
}
```

### 4.4 Fragmentation Coalescing Algorithm

```c
// sys_memory.c:306-347
static void coalesce_free_blocks(mem_pool_t* pool) {
    // 1. Sort free list by address (insertion sort)
    free_block_t* sorted = NULL;
    free_block_t* curr = pool->free_list;
    while (curr != NULL) {
        // Insert into sorted list (ascending by address)
        // ...
    }
    pool->free_list = sorted;

    // 2. Coalesce adjacent blocks
    curr = pool->free_list;
    while (curr != NULL && curr->next != NULL) {
        uintptr_t curr_end = (uintptr_t) curr + curr->size;
        uintptr_t next_start = (uintptr_t) curr->next;

        if (curr_end == next_start) {
            // Coalesce
            curr->size += curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}
```

### 4.5 Memory Leak Detection

#### 4.5.1 Tracker Structure

```c
// sys_memory.c:94-101
typedef struct mem_tracker {
    sys_mem_alloc_info_t records[MAX_ALLOCATIONS]; // Circular buffer
    uint32_t             head;                     // Write position
    uint32_t             count;                    // Current record count
    uint32_t             max_records;
    bool                 tracking_enabled;
    struct k_mutex       lock;
} mem_tracker_t;

// Each record
typedef struct {
    void*       ptr;       // Allocation pointer
    size_t      size;      // Size
    uint32_t    timestamp; // Timestamp
    const char* module;    // Module name (debug mode)
    uint32_t    line;      // Line number (debug mode)
} sys_mem_alloc_info_t;
```

#### 4.5.2 Record on Allocation

```c
// sys_memory.c:252-275
static void tracker_add(void* ptr, size_t size, const char* module, uint32_t line) {
    k_mutex_lock(&g_sys_mem.tracker.lock, K_FOREVER);

    // Write at current position
    uint32_t idx = g_sys_mem.tracker.head;
    g_sys_mem.tracker.records[idx].ptr = ptr;
    g_sys_mem.tracker.records[idx].size = size;
    g_sys_mem.tracker.records[idx].timestamp = k_uptime_get_32();
    g_sys_mem.tracker.records[idx].module = module;
    g_sys_mem.tracker.records[idx].line = line;

    // Circular advance
    g_sys_mem.tracker.head = (idx + 1) % g_sys_mem.tracker.max_records;

    k_mutex_unlock(&g_sys_mem.tracker.lock);
}
```

#### 4.5.3 Remove on Free

```c
// sys_memory.c:280-298
static void tracker_remove(void* ptr) {
    k_mutex_lock(&g_sys_mem.tracker.lock, K_FOREVER);

    // Linear search
    for (uint32_t i = 0; i < g_sys_mem.tracker.count; i++) {
        if (g_sys_mem.tracker.records[i].ptr == ptr) {
            // Fill gap with last element
            g_sys_mem.tracker.records[i] = g_sys_mem.tracker.records[count - 1];
            g_sys_mem.tracker.count--;
            break;
        }
    }

    k_mutex_unlock(&g_sys_mem.tracker.lock);
}
```

#### 4.5.4 Leak Detection

```c
// sys_memory.c:812-835
uint32_t sys_mem_check_leaks(sys_mem_pool_type_t type) {
    uint32_t leaks = 0;

    k_mutex_lock(&g_sys_mem.tracker.lock, K_FOREVER);

    // Iterate all records, check magic number
    for (uint32_t i = 0; i < g_sys_mem.tracker.count; i++) {
        alloc_header_t* header = get_alloc_header(g_sys_mem.tracker.records[i].ptr);

        // Magic number MEMORY_MAGIC means not freed
        if (header->magic == MEMORY_MAGIC) {
            leaks++;
        }
    }

    k_mutex_unlock(&g_sys_mem.tracker.lock);
    return leaks;
}
```

### 4.6 Security Mechanism Summary

| Problem | Detection Mechanism | Implementation Location |
|---------|---------------------|------------------------|
| Double free | Magic number check | `pool_free_locked()` |
| Wild pointer free | Pool range check + magic check | `lock_pool_from_ptr()` |
| Pool type mismatch | Record pool_type, verify on free | `sys_mem_free()` |
| Overflow detection | Header prefix, extensible to red zone | `alloc_header_t` |

---

## 5. Thread Safety and Concurrency Control

### 5.1 Lock Usage Strategy

#### 5.1.1 Mutex Protection Scope

| Data Structure | Protection Lock | Access Mode |
|----------------|----------------|-------------|
| Event type subscriber list | `event_type_entry_t.lock` | Per-type independent lock |
| Event system stats | `event_system_cb_t.stats_lock` | Global stats lock |
| Module manager state | `module_manager_cb_t.lock` | Single global lock |
| Memory pool free list | `mem_pool_t.lock` | Per-pool independent lock |
| Memory tracker | `mem_tracker_t.lock` | Global tracker lock |

#### 5.1.2 Atomic Variable Usage

```c
// Event system running state (ISR readable)
atomic_t running;  // event_system.c:55

// Event drop count (ISR updatable)
static _Atomic uint32_t g_event_dropped_count;  // event_system.c:80
```

**Atomic Variables vs Mutexes:**

| Feature | Atomic Variables | Mutexes |
|---------|-----------------|---------|
| ISR Safe | ✅ Yes | ❌ No |
| Overhead | Extremely low | Higher (system call) |
| Use Case | Single variable read/write | Complex data structures |
| Reentrant | ✅ Naturally supported | ❌ Need recursive lock |

### 5.2 Deadlock Prevention

#### 5.2.1 Lock Ordering Rules

```
Lock acquisition order (outside to inside):
1. module_manager.lock
2. event_type_entry_t.lock
3. mem_pool_t.lock
4. mem_tracker.lock

Reverse acquisition forbidden!
```

#### 5.2.2 Lock-Free Call Pattern

```c
// Correct example: Call callbacks outside lock
k_mutex_lock(&lock, K_FOREVER);
callback_fn = info->callback;  // Copy function pointer
k_mutex_unlock(&lock);

callback_fn(...);  // Call outside lock

// Wrong example: Call callback while holding lock (may cause deadlock)
k_mutex_lock(&lock, K_FOREVER);
info->callback(...);  // Dangerous! Callback may acquire same lock again
k_mutex_unlock(&lock);
```

### 5.3 Snapshot Technology Details

```c
// Module traversal example
void module_manager_foreach(void (*callback)(module_info_t*, void*), void* user_data) {
    // 1. Create snapshot
    module_info_t snapshot[CONFIG_MAX_MODULES];

    k_mutex_lock(&g_module_mgr.lock, K_FOREVER);
    int n = 0;
    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        if (modules[i].status != MODULE_STATUS_UNINITIALIZED) {
            snapshot[n++] = modules[i];  // Copy
        }
    }
    k_mutex_unlock(&g_module_mgr.lock);

    // 2. Traverse outside lock
    for (int i = 0; i < n; i++) {
        callback(&snapshot[i], user_data);
    }
}
```

**Advantages:**
- Callbacks can safely call any API
- Lock hold time controllable
- Won't block other threads due to long callback execution

---

## 6. Performance Considerations and Tradeoffs

### 6.1 Memory Overhead Analysis

| Component | Static Overhead | Configuration Item |
|-----------|----------------|-------------------|
| Event queue | `QUEUE_SIZE * sizeof(event_t)` ≈ 32 * 28 = 896 bytes | `CONFIG_EVENT_QUEUE_SIZE` |
| Event type table | `MAX_TYPES * sizeof(event_type_entry_t)` ≈ 256 * 136 | `CONFIG_EVENT_MAX_TYPES` |
| Module table | `MAX_MODULES * sizeof(module_info_t)` ≈ 16 * 200 | `CONFIG_MAX_MODULES` |
| Memory tracker | `MAX_ALLOCATIONS * sizeof(sys_mem_alloc_info_t)` ≈ 256 * 24 | `CONFIG_SYS_MEMORY_DEBUG` |

### 6.2 Time Complexity Analysis

| Operation | Complexity | Description |
|-----------|------------|-------------|
| Event publish | O(1) | Queue operation |
| Event subscribe | O(1) | Array find free slot |
| Event notify | O(n) | n = subscriber count, snapshot used to avoid holding lock |
| Module register | O(m) | m = registered module count, check for duplicates |
| Dependency topological sort | O(m²) | m = modules to start, Kahn's algorithm |
| Memory allocate | O(n) | n = free block count, First-Fit |
| Memory free | O(n) | Insertion sort + coalesce |

### 6.3 Design Tradeoffs

#### 6.3.1 Static vs Dynamic Allocation

| Aspect | Static Allocation | Dynamic Allocation |
|--------|------------------|-------------------|
| Memory predictability | ✅ Compile-time determined | ❌ Runtime varies |
| Fragmentation | ✅ No fragmentation | ❌ May fragment |
| Flexibility | ❌ Fixed upper limit | ✅ Expand as needed |
| Use case | Embedded real-time systems | General systems |

**This project's choice: Static allocation + configurable limits**

#### 6.3.2 Event Queue Strategy

| Strategy | Advantages | Disadvantages |
|----------|-----------|---------------|
| Blocking wait | No event loss | Unpredictable delay |
| Drop events | Good real-time | May lose data |
| Priority queue | Important events first | Complex implementation |

**This project's choice: Drop events + atomic counter**

#### 6.3.3 Dependency Resolution Timing

| Timing | Advantages | Disadvantages |
|--------|-----------|---------------|
| Compile-time | Zero runtime overhead | Modification requires recompilation |
| Runtime | Flexible, supports dynamic loading | Resolution overhead |

**This project's choice: Runtime resolution (string dependencies)**

---

## 7. Interview Q&A

### Q1: How to handle when event queue is full?

**Core answer: Drop events + atomic counter statistics**

```c
int ret = k_msgq_put(g_event_system.event_queue, event, K_NO_WAIT);
if (ret != 0) {
    atomic_fetch_add_explicit(&g_event_dropped_count, 1U, memory_order_relaxed);
    return EVENT_ERR_QUEUE_FULL;
}
```

**Design rationale:**
- Using `K_NO_WAIT` doesn't block, ensuring real-time performance
- Atomic counter needs no lock, ISR safe
- Error code return lets caller perceive system pressure, can take degradation measures

---

### Q2: Any special handling when publishing events from ISR context?

**Core answer: No mutex + atomic variable read state + `k_msgq_put` uses spinlock**

```c
event_status_t event_publish_from_isr(const event_t* event) {
    // 1. Don't use mutex (forbidden in ISR)
    // 2. Use atomic_get to read running state
    if (atomic_get(&g_event_system.running) == 0) {
        return EVENT_ERR_INVALID_ARG;
    }
    // 3. k_msgq_put with K_NO_WAIT is ISR safe
    //    Uses spinlock internally (not mutex), allowed in ISR
    int ret = k_msgq_put(g_event_system.event_queue, event, K_NO_WAIT);
    // ...
}
```

**Follow-up question: `k_msgq_put` has locks internally, why can it be called in ISR?**

| Lock Type | Allowed in ISR | Reason |
|-----------|----------------|--------|
| **Mutex** | ❌ Forbidden | May block/trigger thread scheduling, ISR cannot be scheduled |
| **Spinlock** | ✅ Allowed | Only disables local interrupt, won't trigger scheduling |

**Key difference:**
- When mutex acquisition fails, thread **blocks** waiting, scheduler switches to other thread
- When spinlock acquisition fails, CPU **busy-waits** (or disables interrupt), no scheduling
- ISR is not a thread, cannot be switched by scheduler, so mutex crashes, spinlock is safe

**ISR restrictions:**
- ❌ Cannot acquire mutex (will block/schedule)
- ⚠️ Avoid `k_malloc` (may use mutex or be non-reentrant)
- ❌ Cannot use `K_FOREVER` wait (will block)
- ✅ Can use spinlock (`k_msgq_put(K_NO_WAIT)` uses spinlock internally)
- ✅ Can use atomic operations

---

### Q3: How to decouple module dependencies?

**Core answer: String dependency declaration + runtime topological sort**

```c
// Declare dependencies at compile time (no need to include other module headers)
static const char* const module_c_deps[] = {"module_a", "module_b", NULL};

static const module_interface_t module_c_interface = {
    .name = "module_c",
    .depends_on = module_c_deps,
    // ...
};
```

**Startup topological sort:**
1. Fixed-point verification: Remove illegal dependencies
2. Build graph: Directed edge j→i means i depends on j
3. Kahn's algorithm topological sort
4. Detect cycles, fallback to priority sort if cycle exists

**Advantages:**
- Compile-time decoupling, modules don't need to include each other's headers
- Supports dynamic module loading and unloading
- Runtime can detect circular dependencies

---

### Q4: How is memory leak detection implemented?

**Core answer: Tracker records each allocation, removes on free**

```c
// Record on allocation
static void tracker_add(void* ptr, size_t size, const char* module, uint32_t line) {
    g_sys_mem.tracker.records[idx].ptr = ptr;
    g_sys_mem.tracker.records[idx].size = size;
    g_sys_mem.tracker.records[idx].timestamp = k_uptime_get_32();
    g_sys_mem.tracker.records[idx].module = module;
    g_sys_mem.tracker.records[idx].line = line;
}

// Remove on free
static void tracker_remove(void* ptr) {
    // Linear search and remove
}

// Leak detection
uint32_t sys_mem_check_leaks(sys_mem_pool_type_t type) {
    // Iterate tracker, check magic number
    if (header->magic == MEMORY_MAGIC) {
        leaks++;  // Not freed
    }
}
```

**Security mechanisms:**
| Problem | Detection Method |
|---------|-----------------|
| Memory leak | Record in tracker not removed |
| Double free | Magic `MEMORY_MAGIC` → `MEMORY_FREED_MAGIC` |
| Wild pointer | Pool range check + magic verification |

---

### Q5: Why use snapshot technology to notify subscribers?

**Core answer: Avoid deadlock**

```c
// Only copy callback pointers while holding lock
k_mutex_lock(&entry->lock, K_FOREVER);
for (uint32_t i = 0; i < CONFIG_EVENT_MAX_SUBSCRIBERS; i++) {
    snap[n].cb = sub->callback;
    snap[n].ud = sub->user_data;
    n++;
}
k_mutex_unlock(&entry->lock);

// Call callbacks outside lock
for (uint32_t i = 0; i < n; i++) {
    snap[i].cb(event, snap[i].ud);
}
```

**Problem scenario:**
- Callbacks may call `event_subscribe/unsubscribe`
- If calling callback while holding lock → deadlock

**Solution:**
- Only copy callback pointers while holding lock
- Release lock before calling callbacks
- Callbacks can safely call any event API

---

### Q6: Why choose First-Fit for memory allocation?

**Core answer: Embedded system characteristics determine this**

| Algorithm | Time Complexity | Memory Utilization | Fragmentation |
|-----------|----------------|-------------------|---------------|
| First-Fit | O(n) | Medium | Medium |
| Best-Fit | O(n) | High | High (more small fragments) |
| Worst-Fit | O(n) | Low | Low |

**Embedded system characteristics:**
- Allocation requests relatively regular (events, module data)
- Care more about allocation speed than maximum utilization
- Periodic fragmentation coalescing alleviates fragmentation issues

---

### Q7: How to ensure thread safety?

**Core answer: Multi-level lock strategy**

1. **Mutex**: Protect complex data structures
   - Independent lock per event type
   - Independent lock per memory pool
   - Module manager global lock

2. **Atomic variables**: Single variable read/write
   - Event system running state
   - Event drop count

3. **Snapshot technology**: Avoid calling callbacks while holding lock
   - Event notification
   - Module traversal

4. **Lock ordering**: Prevent deadlock
   - Fixed lock acquisition order
   - Call user code outside lock

---

## Appendix: Key Code Location Index

| Function | File | Key Function/Structure |
|---------|------|----------------------|
| Event publish | `src/core/event_system.c` | `event_publish()` L465 |
| ISR publish | `src/core/event_system.c` | `event_publish_from_isr()` L497 |
| Real-time safe publish | `src/core/event_system.c` | `event_publish_copy_rt()` |
| Subscriber notification | `src/core/event_system.c` | `event_notify_subscribers()` L727 |
| Event queue | `src/core/event_queue.c` | `event_queue_enqueue/dequeue()` |
| Dispatcher control | `src/core/event_dispatcher.c` | `event_dispatcher_pause/resume()` |
| Slab memory | `src/core/event_memory.c` | `event_create_with_data_rt()` |
| Module register | `src/modules/module_manager.c` | `module_manager_register()` L792 |
| Dependency sort | `src/modules/module_manager.c` | `dependency_order_start_batch()` L325 |
| Memory allocate | `src/services/sys_memory.c` | `pool_alloc_locked()` L355 |
| Memory free | `src/services/sys_memory.c` | `pool_free_locked()` L451 |
| Leak detection | `src/services/sys_memory.c` | `sys_mem_check_leaks()` L812 |

---

*Document last updated: 2026-04-17*
