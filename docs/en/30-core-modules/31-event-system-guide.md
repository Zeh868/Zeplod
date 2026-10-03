> Language: [中文](../../zh-CN/30-核心模块/31-事件系统详细使用说明.md) | **English**

# Event System Detailed Usage Guide

## Table of Contents

- [Overview](#overview)
- [Core Concepts](#core-concepts)
- [System Architecture](#system-architecture)
- [Memory and Ownership](#memory-and-ownership)
- [Configuration Options](#configuration-options)
- [API Reference](#api-reference)
- [Usage Guide](#usage-guide)
- [Best Practices](#best-practices)
- [Troubleshooting](#troubleshooting)

---

## Overview

The Event System is a core component of the Zephyr RTOS framework, providing a high-performance, thread-safe Publish-Subscribe communication mechanism. The system supports:

- **Asynchronous Event Processing**: Decouples event producers from consumers
- **Priority Scheduling**: Supports event priority classification
- **Thread Safety**: All operations are protected by mutexes
- **Dynamic Subscription**: Runtime registration/unregistration of event types and subscribers
- **ISR Support**: Supports publishing events from interrupt context

### Key Features

| Feature | Description |
|---------|-------------|
| Max Event Types | `CONFIG_EVENT_MAX_TYPES` (default 128) |
| Max Subscribers Per Type | `CONFIG_EVENT_MAX_SUBSCRIBERS` (default 16) |
| Event Queue Size | 64 (configurable) |
| Dispatcher Priority | 5 (configurable) |
| ISR-Safe Publishing | Supported |
| **Slab Memory Pool** | Priority-tiered + large data block pool |
| **Real-time Safe API** | `_rt` suffix, O(1) deterministic allocation time |
| **Dispatcher** | Supports pause/resume, event filtering, manual batch processing |

---

## Core Concepts

### Event

An Event is the most basic communication unit in the system, containing the following fields:

```c
typedef struct {
    uint8_t          type;           // Event type identifier
    uint8_t          priority;       // Event priority
    uint8_t          flags;          // Flags (EVENT_FLAG_*)
    uint8_t          reserved;       // Reserved for extension
    uint32_t         timestamp;      // Event creation timestamp (ms uptime)
    uint32_t         source_id;      // Source module/component ID
    uint32_t         data_len;       // Event data length
    union {
        uint8_t  inline_data[CONFIG_EVENT_INLINE_DATA_SIZE]; // Inline data
        void*    ptr;                                             // External data pointer
    } data;
} event_t;
```

**Memory Layout** (example with 64-byte configuration):
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

**Data Storage Strategy**:
- `data_len ≤ CONFIG_EVENT_INLINE_DATA_SIZE`: Inline storage, no additional allocation
- `data_len > CONFIG_EVENT_INLINE_DATA_SIZE`: Allocated from Slab pool or k_malloc

**Flag Definitions**:
| Flag | Value | Meaning |
|------|-------|---------|
| `EVENT_FLAG_DATA_INLINE` | 0x01 | Data stored inline in inline_data |
| `EVENT_FLAG_DATA_DYNAMIC` | 0x02 | Data dynamically allocated, managed by system |
| `EVENT_FLAG_FROM_SLAB` | 0x04 | event_t from Slab pool |
| `EVENT_FLAG_DATA_FROM_SLAB` | 0x08 | Data from Slab pool (freed with event) |
| `EVENT_FLAG_SLAB_256` | 0x10 | Current data in 256B slab pool |
| `EVENT_FLAG_SLAB_1K` | 0x20 | Current data in 1KB slab pool |
| `EVENT_FLAG_SLAB_4K` | 0x40 | Current data in 4KB slab pool |
| `EVENT_FLAG_SLAB_MASK` | 0x70 | Mask covering three slab size bits |

### Event Type

Event Types are used to classify events. The system supports up to 256 event types. Predefined common event types:

```c
#define EVENT_TYPE_GENERIC        1U    // Generic event
#define EVENT_TYPE_SENSOR_DATA    10U   // Sensor data event
#define EVENT_TYPE_SENSOR_CONFIG  11U   // Sensor configuration event
```

### Subscriber

A Subscriber is a callback function registered to receive specific types of events:

```c
typedef struct {
    event_callback_t callback;      // Callback function
    void            *user_data;     // User data
    uint32_t         subscriber_id; // Unique subscriber ID
    bool             is_active;     // Whether active
} subscriber_entry_t;
```

### Event Priority

```c
typedef enum {
    EVENT_PRIORITY_LOW      = 10,   // Low priority
    EVENT_PRIORITY_NORMAL   = 5,    // Normal priority
    EVENT_PRIORITY_HIGH     = 2,    // High priority
    EVENT_PRIORITY_CRITICAL = 0     // Critical priority
} event_priority_t;
```

---

## System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Application Layer                        │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │  Module A   │  │  Module B   │  │  Module C   │         │
│  │ (Publisher) │  │(Subscriber) │  │(Subscriber) │         │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘         │
│         │                │                │                 │
│         │ event_publish  │                │                 │
│         ▼                │                │                 │
│  ┌─────────────────────────────────────────────────────┐   │
│  │              Event System Core                       │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  │   │
│  │  │ Event Queue │  │Dispatcher   │  │ Type Mgr    │  │   │
│  │  │ (k_msgq)    │  │ (Thread)    │  │ (Registry)  │  │   │
│  │  └─────────────┘  └─────────────┘  └─────────────┘  │   │
│  └─────────────────────────────────────────────────────┘   │
│         │                │                │                 │
│         │ event_notify   │                │                 │
│         ▼                ▼                ▼                 │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │  Callback A │  │  Callback B │  │  Callback C │         │
│  └─────────────┘  └─────────────┘  └─────────────┘         │
└─────────────────────────────────────────────────────────────┘
```

### Component Description

1. **Event Queue**
   - Implemented based on Zephyr `k_msgq`
   - Supports priority queuing
   - Discards new events on overflow

2. **Event Dispatcher**
   - Runs as an independent thread
   - Dequeues events and notifies subscribers
   - Configurable priority and stack size

3. **Event Type Manager**
   - Maintains event type registry
   - Manages subscriber list for each type
   - Thread-safe access control

---

## Memory and Ownership

This section explains which component allocates and frees events during **enqueue, dispatch, and callback** stages, to avoid misuse leading to **use-after-free, double-free, or memory leaks**.

### Slab Memory Management Overview

The Event System supports two memory allocation modes:

| Mode | API | Memory Source | Real-time Safe | Use Case |
|------|-----|---------------|----------------|----------|
| **Slab Mode** | `event_create_rt`, `event_create_with_data_rt` | Pre-allocated Slab pool | ✅ O(1) deterministic | ISR, real-time tasks |
| **Heap Mode** | `event_create`, `event_create_with_data` | k_malloc / Slab fallback | ⚠️ May block | Normal tasks |

**Slab Pool Architecture**:

```
┌─────────────────────────────────────────────────────────────┐
│                    Event Slab Pool (event_t)                     │
├───────────────┬───────────────┬───────────────────────────┤
│  CRITICAL Pool│   HIGH Pool   │      NORMAL/LOW Pool      │
│   (optional)  │   (optional)  │        (required)        │
├───────────────┴───────────────┴───────────────────────────┤
│                    Data Slab Pool (optional)                       │
├─────────────┬─────────────┬─────────────────────────────┤
│   256B Pool │    1KB Pool │         4KB Pool           │
└─────────────┴─────────────┴─────────────────────────────┘
```

**Data Storage Strategy**:
- `data_len ≤ CONFIG_EVENT_INLINE_DATA_SIZE` (default 48B): Inline storage, zero additional allocation
- `data_len ≤ 256`: 256B data pool
- `data_len ≤ 1024`: 1KB data pool
- `data_len ≤ 4096`: 4KB data pool
- `data_len > 4096`: Fallback to k_malloc (not real-time safe)

### Cascading Fallback Mechanism

The `event_memory_select_data_slab_with_fallback` function implements a **cascading fallback** strategy:

1. Primary: Select the smallest suitable Slab pool based on `data_len` (256B → 1KB → 4KB)
2. Check: If the primary Slab pool has available blocks, return directly
3. Fallback: If the primary pool is full, try a larger Slab pool
4. Final: If all Slab pools are full, fallback to `k_malloc`

**Memory Amplification Cost**:

Cascading fallback may cause "memory amplification" — for example, 300 bytes of data ultimately uses a 4KB Slab block. This is because:
- Slab block sizes are fixed and cannot be split
- `EVENT_FLAG_SLAB_*` flags record the actual allocation source, ensuring correct pairing during free

**Why Use `EVENT_FLAG_SLAB_*` Flags**:

When data is allocated to a larger Slab than actually needed through cascading fallback, free must return to the correct Slab pool. For example:
- 300B data primary choice 256B pool (full) → allocated to 1KB pool → `EVENT_FLAG_SLAB_1K` set
- During free, check `EVENT_FLAG_SLAB_*`, return block to 1KB pool, not 256B pool

**Real-time Safe Notes**:

Cascading fallback occurs in **non-realtime** paths like `event_create_with_data`. Realtime safe APIs (`_rt` suffix) do not use fallback and return errors directly when memory is insufficient.

### What is Actually Stored in the Queue

The Event Queue is based on Zephyr `k_msgq` initialization, with single message size of `sizeof(event_t)` (configurable 32/64/128 bytes):

- `k_msgq_put` **copies the entire `event_t` structure** pointed to by the caller's pointer **byte-by-byte** into the queue's ring buffer, **not** just storing "a pointer to `event_t`".
- The `data` member inside the structure is a **union**:
  - Small data (≤ INLINE_DATA_SIZE): Stored in `inline_data[48]`, fully copied
  - Large data: The `ptr` pointer is copied; the heap/Slab memory it points to is NOT copied

When the dispatcher thread `k_msgq_get`, it **copies the `event_t` in the queue slot to a local variable on the thread stack** again, then calls `event_notify_subscribers`, and finally frees dynamically allocated data at the appropriate time (see below).

### `event_publish_copy` (Recommended): Module Does Not Need Manual Free

Flow summary:

1. `event_create_with_data` allocates the **`event_t` shell** (preferred Slab, fallback k_malloc) and **data buffer** (small data inline, large data Slab/k_malloc), and `memcpy`s the caller's data into the buffer.
2. `k_msgq_put` writes the **value of `event_t`** to the queue; the copy in the queue carries complete event data.
3. After successful enqueue, clear the `EVENT_FLAG_DATA_DYNAMIC` flag, then **free the `event_t` shell** (Slab or k_free).
4. After the dispatcher thread dequeues the event and executes callbacks, if `flags & EVENT_FLAG_DATA_DYNAMIC && data.ptr != NULL`, free the data memory.

**Conclusion**: When using `event_publish_copy`, the module **does not need to and should not** free the event's `data` or `event_t`; publish failures are cleaned up internally by `event_publish_copy`.

### `event_publish_copy_rt` (Real-time Safe): Recommended for Slab Mode

```c
// ✅ Recommended: Use _rt suffix API for ISR and real-time critical tasks
void isr_handler(void)
{
    uint8_t data[16] = {0x01, 0x02};

    event_status_t ret = event_publish_copy_rt(
        EVENT_TYPE_SENSOR_DATA,
        EVENT_PRIORITY_HIGH,
        data,
        sizeof(data)
    );

    if (ret == EVENT_ERR_NO_MEM) {
        // Slab exhausted, take degradation measures
    }
}
```

**Characteristics**:
- Entirely allocated from Slab pool, O(1) deterministic time
- Returns NULL/error when Slab is exhausted, **does NOT fallback** to k_malloc
- Small data (≤ INLINE_DATA_SIZE) stored inline, zero additional allocation

### `event_publish`: Ownership After Successful Enqueue

`event_publish` / `event_publish_from_isr` **copy `event_t` by value** into `k_msgq` (see "What the Queue Actually Stores" above).

- **On successful enqueue**: Implementation clears `EVENT_FLAG_DATA_DYNAMIC` (and related flags) on the caller's `event`, meaning **dynamic payload ownership moves to the queue copy**; caller **must not** call `event_free_data()`, but **may** call `event_free()` on the same pointer to release the **`event_t` shell** (stack locals need no free).
- **On failure** (e.g. `EVENT_ERR_QUEUE_FULL`): Caller **retains** dynamic data; free with `event_free_data()` / `event_free()` as appropriate.

The dispatcher frees dynamic `data.ptr` on the **queue copy**; it does **not** free your original shell after a successful publish (use `event_free` on the caller side).

| Publishing Method | `event_t` Source | Data Storage | Who Frees `data` | Who Frees `event_t` Shell |
|-------------------|-------------------|--------------|-------------------|---------------------------|
| `event_publish_copy` | Internal temp | Inline/Slab/Heap | Dispatcher thread | Internal |
| `event_publish_copy_rt` | Internal temp Slab | Inline/Slab | Dispatcher thread | Internal |
| `event_publish` + stack-local `event_t` | Stack | Inline | Dispatcher (copy) | Stack auto-cleanup |
| `event_publish` + `event_create*` heap object | Slab/Heap | Inline/Slab/Heap | Dispatcher (copy) | **Caller `event_free` after success** |

**Practical Recommendations**:

- **Normal tasks**: Prefer `event_publish_copy`
- **ISR/Real-time tasks**: Use `event_publish_copy_rt`
- **Small data on stack**: Can use `event_publish` + stack `event_t` (inline data)

### `event_publish_from_isr`

- **Recommended** in ISR: `event_publish_copy_rt` or `event_create_from_isr` + `event_publish`
- Using real-time safe API guarantees O(1) deterministic time, avoids unpredictable heap allocation
- **Technical detail**: `k_msgq_put(K_NO_WAIT)` is safe in ISR because Zephyr `k_msgq` internally uses **spinlock**, not mutex
  - **Mutex**: May block and trigger thread scheduling, prohibited in ISR
  - **Spinlock**: Only disables local interrupts, does not schedule, safe in ISR

### Rules in Subscription Callbacks

- The `const event_t *event` received by the callback points to **a copy on the dispatcher thread stack**, **only valid during callback execution**
- **Do not** save this pointer for asynchronous use
- **Do not** `k_free(event->data.ptr)` in the callback: Dynamic data is freed by dispatcher thread after all callbacks return

### Correspondence with Examples in "Best Practices"

After a successful enqueue: do **not** call `event_free_data()`; do **not** mutate cleared flags on `event`; for heap shells call **`event_free(event)`**. `event_publish_copy` manages the full lifecycle internally.

---

## Configuration Options

Detailed explanations of Event System related Kconfig items are in **[Project Configuration Options.md](../40-app-development/42-config-options.md) §1**. Common configuration examples:

```kconfig
CONFIG_EVENT_SYSTEM=y
CONFIG_EVENT_QUEUE_SIZE=64
CONFIG_EVENT_MAX_SUBSCRIBERS=16
CONFIG_EVENT_DISPATCHER_STACK_SIZE=2048
CONFIG_EVENT_DISPATCHER_PRIORITY=5
CONFIG_EVENT_STRUCT_SIZE_64=y
CONFIG_EVENT_INLINE_DATA_SIZE=48
```

### Slab Pool Configuration

```kconfig
CONFIG_EVENT_SLAB_ENABLE=y
CONFIG_EVENT_SLAB_CRITICAL_COUNT=8
CONFIG_EVENT_SLAB_HIGH_COUNT=16
CONFIG_EVENT_SLAB_NORMAL_COUNT=32
CONFIG_EVENT_SLAB_LARGE_ENABLE=y
CONFIG_EVENT_SLAB_LARGE_256_COUNT=8
CONFIG_EVENT_SLAB_LARGE_1K_COUNT=4
CONFIG_EVENT_SLAB_LARGE_4K_COUNT=2
```

### Runtime Debug Configuration

```kconfig
CONFIG_EVENT_RUNTIME_STATUS=y
CONFIG_EVENT_DEBUG_MEM=y
CONFIG_EVENT_SLAB_EXHAUSTED_CB=y
CONFIG_EVENT_QUEUE_HIGH_WATERMARK=48
CONFIG_EVENT_DISPATCHER_MAX_EVENTS_PER_CYCLE=100
```

---

## API Reference

### Core API

#### Initialization and Startup

```c
/**
 * @brief Initialize the Event System
 * @return EVENT_OK on success, other values are error codes
 */
event_status_t event_system_init(void);

/**
 * @brief Start the Event Dispatcher
 * @return EVENT_OK on success, other values are error codes
 */
event_status_t event_system_start(void);

/**
 * @brief Stop the Event Dispatcher
 * @return EVENT_OK on success, other values are error codes
 */
event_status_t event_system_stop(void);

/**
 * @brief Check if Event System is running
 * @return true if running, false if stopped
 */
bool event_system_is_running(void);

/**
 * @brief Shutdown the Event System
 * @return EVENT_OK on success, other values are error codes
 * @note Stops dispatcher thread, releases all resources
 */
event_status_t event_system_shutdown(void);

/**
 * @brief Reset Event System statistics
 */
void event_system_reset_statistics(void);
```

#### Event Type Management

```c
/**
 * @brief Register an event type
 * @param type Event type ID (0-255)
 * @param name Event type name
 * @return EVENT_OK on success, other values are error codes
 */
event_status_t event_register_type(event_type_t type, const char *name);

/**
 * @brief Unregister an event type
 * @param type Event type ID
 * @return EVENT_OK on success, other values are error codes
 */
event_status_t event_unregister_type(event_type_t type);
```

#### Subscription Management

```c
/**
 * @brief Subscribe to an event type
 * @param type Event type ID
 * @param callback Callback function
 * @param user_data User data (passed to callback)
 * @param subscriber_id Output: assigned subscriber ID
 * @return EVENT_OK on success, other values are error codes
 */
event_status_t event_subscribe(event_type_t type,
                                event_callback_t callback,
                                void *user_data,
                                uint32_t *subscriber_id);

/**
 * @brief Unsubscribe from a specific event type
 * @param type Event type ID
 * @param subscriber_id Subscriber ID
 * @return EVENT_OK on success, other values are error codes
 */
event_status_t event_unsubscribe(event_type_t type, uint32_t subscriber_id);

/**
 * @brief Unsubscribe from all event types
 * @param subscriber_id Subscriber ID
 */
void event_unsubscribe_all(uint32_t subscriber_id);
```

#### Event Publishing

```c
/**
 * @brief Publish an event (synchronous)
 * @param event Event pointer
 * @return EVENT_OK on success, other values are error codes
 */
event_status_t event_publish(event_t *event);

/**
 * @brief Publish an event from interrupt context
 * @param event Event pointer (same ownership rules as event_publish)
 * @return EVENT_OK on success, other values are error codes
 */
event_status_t event_publish_from_isr(event_t *event);

/**
 * @brief Publish an event and copy data (internal copy created)
 * @param type Event type ID
 * @param priority Event priority
 * @param data Data pointer
 * @param data_len Data length
 * @return EVENT_OK on success, other values are error codes
 */
event_status_t event_publish_copy(event_type_t type,
                                   event_priority_t priority,
                                   const void *data,
                                   size_t data_len);
```

#### Event Creation and Memory Management

```c
/**
 * @brief Create an empty event
 * @param type Event type ID
 * @param priority Event priority
 * @return Event pointer, NULL on failure
 */
event_t *event_create(event_type_t type, event_priority_t priority);

/**
 * @brief Create an event with data
 * @param type Event type ID
 * @param priority Event priority
 * @param data Data pointer
 * @param data_len Data length
 * @return Event pointer, NULL on failure
 */
event_t *event_create_with_data(event_type_t type,
                                 event_priority_t priority,
                                 const void *data,
                                 size_t data_len);

/**
 * @brief Free an event
 * @param event Event pointer
 */
void event_free(event_t *event);
```

#### Real-time Safe API (Slab Pool)

The following APIs allocate memory entirely from Slab pools with O(1) deterministic allocation time, suitable for ISR and real-time critical tasks:

```c
/**
 * @brief Create an event (real-time safe)
 * @param type Event type ID
 * @param priority Event priority
 * @return Pointer to new event, NULL on failure
 * @note Entirely from Slab pool, no k_malloc fallback
 */
event_t *event_create_rt(event_type_t type, event_priority_t priority);

/**
 * @brief Create an event with data (real-time safe)
 * @param type Event type ID
 * @param priority Event priority
 * @param data Data pointer
 * @param data_len Data length
 * @return Pointer to new event, NULL on failure
 * @note Small data stored inline; large data from Slab, no k_malloc fallback
 */
event_t *event_create_with_data_rt(event_type_t type,
                                    event_priority_t priority,
                                    const void *data,
                                    size_t data_len);

/**
 * @brief Publish an event and copy data (real-time safe)
 * @param type Event type ID
 * @param priority Event priority
 * @param data Data pointer
 * @param data_len Data length
 * @return EVENT_OK on success, other error codes
 * @note Returns error when memory is insufficient, no k_malloc fallback
 */
event_status_t event_publish_copy_rt(event_type_t type,
                                      event_priority_t priority,
                                      const void *data,
                                      size_t data_len);

/**
 * @brief Create an event from ISR (real-time safe)
 * @note Equivalent to event_create_with_data_rt, for explicit ISR context use
 */
static inline event_t *event_create_from_isr(event_type_t type,
                                              event_priority_t priority,
                                              const void *data,
                                              size_t data_len);
```

#### Runtime Status Query (Slab Pool)

```c
/**
 * @brief Check if Slab of specified priority is available
 * @param priority Event priority
 * @return true if available blocks exist, false if Slab exhausted or unavailable
 */
bool event_slab_available(event_priority_t priority);

/**
 * @brief Get remaining block count for specified priority Slab
 * @param priority Event priority
 * @return Remaining available blocks, 0 if Slab unavailable
 */
uint32_t event_slab_remaining(event_priority_t priority);

/**
 * @brief Slab statistics structure
 */
typedef struct {
    uint32_t critical_used;     // CRITICAL pool used blocks
    uint32_t critical_total;    // CRITICAL pool total blocks
    uint32_t high_used;         // HIGH pool used blocks
    uint32_t high_total;        // HIGH pool total blocks
    uint32_t normal_used;       // NORMAL pool used blocks
    uint32_t normal_total;      // NORMAL pool total blocks
    uint32_t data_256_used;     // 256B data pool used blocks
    uint32_t data_256_total;    // 256B data pool total blocks
    uint32_t data_1k_used;      // 1KB data pool used blocks
    uint32_t data_1k_total;     // 1KB data pool total blocks
    uint32_t data_4k_used;      // 4KB data pool used blocks
    uint32_t data_4k_total;     // 4KB data pool total blocks
    uint32_t fallback_count;    // Fallback to k_malloc count
} event_slab_stats_t;

/**
 * @brief Get statistics for all Slab pools
 * @param stats Output parameter, receives statistics
 */
void event_get_slab_stats(event_slab_stats_t *stats);

/**
 * @brief Register Slab exhausted callback
 * @param cb Callback function, NULL to clear callback
 * @note Requires CONFIG_EVENT_SLAB_EXHAUSTED_CB=y
 */
void event_register_slab_exhausted_cb(event_slab_exhausted_cb_t cb);

/**
 * @brief Check for memory leaks
 * @return Number of unfreed allocations
 * @note Requires CONFIG_EVENT_DEBUG_MEM=y
 */
uint32_t event_check_leaks(void);

/**
 * @brief Dump memory leak details
 * @note Requires CONFIG_EVENT_DEBUG_MEM=y
 */
void event_dump_leaks(void);
```

#### Utility Functions

```c
/**
 * @brief Get event type name
 * @param type Event type ID
 * @return Type name string
 */
const char *event_get_type_name(event_type_t type);

/**
 * @brief Get subscriber count for an event type
 * @param type Event type ID
 * @return Subscriber count
 */
uint32_t event_get_subscriber_count(event_type_t type);

/**
 * @brief Get Event System statistics
 * @param total_events Output: total event count
 * @param queue_depth Output: current queue depth
 * @param dropped_events Output: dropped event count
 */
void event_get_statistics(uint32_t *total_events,
                          uint32_t *queue_depth,
                          uint32_t *dropped_events);

/**
 * @brief Get the global event queue
 * @return Event queue pointer
 */
struct k_msgq *event_system_get_queue(void);

/**
 * @brief Manually notify subscribers (for custom scheduling)
 * @param event Event pointer
 * @return EVENT_OK on success, other values are error codes
 */
event_status_t event_notify_subscribers(const event_t *event);
```

### Event Queue API

```c
/**
 * @brief Initialize an event queue
 */
event_status_t event_queue_init(struct k_msgq *queue, void *buffer,
                                 size_t capacity);

/**
 * @brief Enqueue an event
 */
event_status_t event_queue_enqueue(struct k_msgq *queue,
                                    const event_t *event,
                                    queue_overflow_policy_t policy,
                                    k_timeout_t timeout);

/**
 * @brief Dequeue an event
 */
event_status_t event_queue_dequeue(struct k_msgq *queue,
                                    event_t *event,
                                    k_timeout_t timeout);

/**
 * @brief Check if queue is empty
 * @return true if empty, false if not empty
 */
bool event_queue_is_empty(const struct k_msgq *queue);

/**
 * @brief Check if queue is full
 * @return true if full, false if not full
 */
bool event_queue_is_full(const struct k_msgq *queue);

/**
 * @brief Get queue depth (used slot count)
 * @return Number of events in queue
 */
uint32_t event_queue_depth(const struct k_msgq *queue);

/**
 * @brief Get queue capacity
 * @return Maximum queue capacity
 */
uint32_t event_queue_capacity(const struct k_msgq *queue);

/**
 * @brief Purge the queue
 */
void event_queue_purge(struct k_msgq *queue);

/**
 * @brief Get queue statistics
 * @param stats Output statistics
 */
void event_queue_get_stats(const struct k_msgq *queue, queue_stats_t *stats);

/**
 * @brief Reset queue statistics
 */
void event_queue_reset_stats(struct k_msgq *queue);

/**
 * @brief Deinitialize event queue (release resources)
 */
void event_queue_deinit(struct k_msgq *queue);
```

### Event Dispatcher API

```c
/**
 * @brief Initialize the dispatcher
 * @param config Dispatcher configuration, NULL for default
 * @return EVENT_OK on success
 */
event_status_t event_dispatcher_init(const dispatcher_config_t *config);

/**
 * @brief Start the dispatcher
 * @return EVENT_OK on success
 */
event_status_t event_dispatcher_start(void);

/**
 * @brief Stop the dispatcher
 * @return EVENT_OK on success
 */
event_status_t event_dispatcher_stop(void);

/**
 * @brief Pause event processing
 */
event_status_t event_dispatcher_pause(void);

/**
 * @brief Resume event processing
 */
event_status_t event_dispatcher_resume(void);

/**
 * @brief Get current dispatcher state
 * @return Dispatcher state (DISPATCHER_STOPPED/RUNNING/PAUSED)
 */
dispatcher_state_t event_dispatcher_get_state(void);

/**
 * @brief Check if current thread is the dispatcher thread
 * @return true if dispatcher thread, false otherwise
 */
bool event_dispatcher_is_current_thread(void);

/**
 * @brief Set event filter
 * @param filter Filter function, NULL to clear filter
 * @param user_data User data
 */
void event_dispatcher_set_filter(event_filter_t filter, void *user_data);

/**
 * @brief Clear event filter
 */
void event_dispatcher_clear_filter(void);

/**
 * @brief Process a single event (manual dispatch mode)
 * @param timeout Wait timeout
 * @return EVENT_OK on success, EVENT_ERR_QUEUE_EMPTY if queue empty
 */
event_status_t event_dispatcher_process_one(k_timeout_t timeout);

/**
 * @brief Process all pending events
 * @param max_events Maximum number to process, 0 for all
 * @return Number of events processed
 */
uint32_t event_dispatcher_process_all(uint32_t max_events);

/**
 * @brief Get dispatcher statistics
 */
void event_dispatcher_get_stats(dispatcher_stats_t *stats);

/**
 * @brief Reset dispatcher statistics
 */
void event_dispatcher_reset_stats(void);

/**
 * @brief Get idle time since last event processing completed (microseconds)
 * @return Idle time (microseconds)
 */
uint32_t event_dispatcher_get_current_latency(void);
```

---

## Usage Guide

### Quick Start

#### 1. Initialize Event System

```c
#include <zeplod/event_system.h>

void app_main(void)
{
    // Initialize event system
    event_status_t ret = event_system_init();
    if (ret != EVENT_OK) {
        LOG_ERR("Failed to initialize event system: %d", ret);
        return;
    }

    // Start event dispatcher
    ret = event_system_start();
    if (ret != EVENT_OK) {
        LOG_ERR("Failed to start event system: %d", ret);
        return;
    }

    LOG_INF("Event system started successfully");
}
```

#### 2. Register Event Types

```c
// Define custom event types
#define MY_EVENT_TYPE_SENSOR_READ    100U
#define MY_EVENT_TYPE_DATA_READY     101U
#define MY_EVENT_TYPE_ALARM          102U

void register_event_types(void)
{
    event_register_type(MY_EVENT_TYPE_SENSOR_READ, "sensor_read");
    event_register_type(MY_EVENT_TYPE_DATA_READY, "data_ready");
    event_register_type(MY_EVENT_TYPE_ALARM, "alarm");
}
```

#### 3. Subscribe to Events

```c
static uint32_t g_subscriber_id;

// Event callback function
static void my_event_callback(const event_t *event, void *user_data)
{
    LOG_INF("Received event: type=%d, priority=%d, timestamp=%u",
            event->type, event->priority, event->timestamp);

    if (event->data_len > 0) {
        if (event->flags & EVENT_FLAG_DATA_INLINE) {
            uint8_t byte = event->data.inline_data[0];
            (void)byte;
        } else {
            uint8_t byte = *(uint8_t *)event->data.ptr;
            (void)byte;
        }
    }

    // Access user data
    int *my_data = (int *)user_data;
    LOG_INF("User data: %d", *my_data);
}

void subscribe_to_events(void)
{
    int user_data = 42;

    event_status_t ret = event_subscribe(
        MY_EVENT_TYPE_SENSOR_READ,
        my_event_callback,
        &user_data,  // User data pointer
        &g_subscriber_id
    );

    if (ret != EVENT_OK) {
        LOG_ERR("Failed to subscribe: %d", ret);
    }
}
```

#### 4. Publish Events

```c
// Method 1: Publish static event (no dynamic data, stack event_t)
void publish_simple_event(void)
{
    event_t event = {
        .type = MY_EVENT_TYPE_SENSOR_READ,
        .priority = EVENT_PRIORITY_NORMAL,
        .timestamp = k_uptime_get_32(),
        .source_id = 1,
        .flags = EVENT_FLAG_DATA_INLINE,  // Inline data, no extra allocation
        .reserved = 0,  // Must explicitly initialize
        .data_len = 0
    };

    event_publish(&event);
}

// Method 2: Use event_publish_copy with payload (recommended)
// System manages memory automatically; module does not need event_free.
void publish_data_event(const void *data, size_t len)
{
    event_publish_copy(
        MY_EVENT_TYPE_DATA_READY,
        EVENT_PRIORITY_HIGH,
        data,
        len
    );
}

// Method 3: ISR or real-time tasks use _rt suffix API (real-time safe)
void isr_publish_event(void)
{
    uint8_t sensor_data[16] = {0x01, 0x02};

    // Entirely from Slab pool, O(1) deterministic time
    event_status_t ret = event_publish_copy_rt(
        MY_EVENT_TYPE_SENSOR_READ,
        EVENT_PRIORITY_HIGH,
        sensor_data,
        sizeof(sensor_data)
    );

    if (ret == EVENT_ERR_NO_MEM) {
        // Slab pool exhausted, take degradation measures
    }
}

// Method 4: Manual create and publish (not recommended, easy to leak)
void manual_publish(const void *data, size_t len)
{
    event_t *event = event_create_with_data(
        MY_EVENT_TYPE_SENSOR_READ,
        EVENT_PRIORITY_NORMAL,
        data,
        len
    );
    if (event == NULL) {
        return;
    }
    if (event_publish(event) != EVENT_OK) {
        event_free(event);  // Publish failed, free the event
    }
    // On successful publish, data is freed by dispatcher thread, but event_t shell is not auto-freed
    // Recommended to use event_publish_copy to avoid omissions
}
```

#### 5. Unsubscribe

```c
void unsubscribe_from_events(void)
{
    // Unsubscribe from specific event
    event_unsubscribe(MY_EVENT_TYPE_SENSOR_READ, g_subscriber_id);

    // Or unsubscribe from all events
    // event_unsubscribe_all(g_subscriber_id);
}
```

### Complete Example

```c
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zeplod/event_system.h>

LOG_MODULE_REGISTER(event_example, LOG_LEVEL_INF);

// Define event types
#define EVENT_TYPE_BUTTON    50U
#define EVENT_TYPE_LED       51U

// Global subscriber IDs
static uint32_t button_sub_id;
static uint32_t led_sub_id;

// Button event callback
static void button_callback(const event_t *event, void *user_data)
{
    LOG_INF("Button event received: data_len=%u", event->data_len);

    if (event->data_len == sizeof(uint8_t)) {
        // Small data stored inline, read directly from inline_data
        uint8_t button_state;
        if (event->flags & EVENT_FLAG_DATA_INLINE) {
            button_state = event->data.inline_data[0];
        } else {
            button_state = *(uint8_t *)event->data.ptr;
        }
        LOG_INF("Button state: %s", button_state ? "PRESSED" : "RELEASED");
    }
}

// LED event callback
static void led_callback(const event_t *event, void *user_data)
{
    LOG_INF("LED event received");

    if (event->data_len == sizeof(uint8_t)) {
        uint8_t led_state;
        if (event->flags & EVENT_FLAG_DATA_INLINE) {
            led_state = event->data.inline_data[0];
        } else {
            led_state = *(uint8_t *)event->data.ptr;
        }
        // Control LED
        LOG_INF("LED turned %s", led_state ? "ON" : "OFF");
    }
}

// Button thread (publishes button events)
static void button_thread(void *p1, void *p2, void *p3)
{
    while (1) {
        uint8_t button_state = 1;  // Simulate button press

        event_publish_copy(
            EVENT_TYPE_BUTTON,
            EVENT_PRIORITY_NORMAL,
            &button_state,
            sizeof(button_state)
        );

        k_msleep(1000);

        button_state = 0;  // Simulate button release
        event_publish_copy(
            EVENT_TYPE_BUTTON,
            EVENT_PRIORITY_NORMAL,
            &button_state,
            sizeof(button_state)
        );

        k_msleep(1000);
    }
}

// LED control thread (publishes LED events)
static void led_thread(void *p1, void *p2, void *p3)
{
    k_sleep(K_SECONDS(2));  // Wait for button thread to start first

    while (1) {
        uint8_t led_state = 1;  // LED ON

        event_publish_copy(
            EVENT_TYPE_LED,
            EVENT_PRIORITY_LOW,
            &led_state,
            sizeof(led_state)
        );

        k_msleep(500);

        led_state = 0;  // LED OFF
        event_publish_copy(
            EVENT_TYPE_LED,
            EVENT_PRIORITY_LOW,
            &led_state,
            sizeof(led_state)
        );

        k_msleep(500);
    }
}

// Main initialization function
void event_system_example_init(void)
{
    // 1. Initialize event system
    event_system_init();

    // 2. Register event types
    event_register_type(EVENT_TYPE_BUTTON, "button");
    event_register_type(EVENT_TYPE_LED, "led");

    // 3. Subscribe to events
    event_subscribe(EVENT_TYPE_BUTTON, button_callback, NULL, &button_sub_id);
    event_subscribe(EVENT_TYPE_LED, led_callback, NULL, &led_sub_id);

    // 4. Start event dispatcher
    event_system_start();

    // 5. Create worker threads
    K_THREAD_DEFINE(button_tid, 1024, button_thread, NULL, NULL, NULL,
                    5, 0, K_NO_WAIT);
    K_THREAD_DEFINE(led_tid, 1024, led_thread, NULL, NULL, NULL,
                    6, 0, K_NO_WAIT);

    LOG_INF("Event system example initialized");
}
```

### Using Event System in ISR

```c
#include <zeplod/event_system.h>

// Method 1: Use event_publish_copy_rt (recommended, real-time safe)
static void gpio_callback_rt(const struct device *dev,
                              struct gpio_callback *cb,
                              uint32_t pins)
{
    uint8_t button_state = 1;

    // Entirely from Slab pool, O(1) deterministic time
    event_status_t ret = event_publish_copy_rt(
        EVENT_TYPE_BUTTON,
        EVENT_PRIORITY_HIGH,
        &button_state,
        sizeof(button_state)
    );

    if (ret == EVENT_ERR_NO_MEM) {
        // Slab pool exhausted, log (note: LOG may be limited in ISR)
    }
}

// Method 2: Use stack event_t + inline data (suitable for no payload)
static void gpio_callback_stack(const struct device *dev,
                                 struct gpio_callback *cb,
                                 uint32_t pins)
{
    // Stack event structure, small data stored inline
    event_t event = {
        .type = EVENT_TYPE_BUTTON,
        .priority = EVENT_PRIORITY_HIGH,
        .timestamp = k_uptime_get_32(),
        .source_id = 0,
        .flags = EVENT_FLAG_DATA_INLINE,
        .reserved = 0,
        .data_len = 0
    };

    event_publish_from_isr(&event);
}

// Method 3: Use event_create_from_isr (manual lifecycle control)
static void gpio_callback_manual(const struct device *dev,
                                  struct gpio_callback *cb,
                                  uint32_t pins)
{
    uint8_t button_state = 1;

    // Create event (from Slab, real-time safe)
    event_t *event = event_create_from_isr(
        EVENT_TYPE_BUTTON,
        EVENT_PRIORITY_HIGH,
        &button_state,
        sizeof(button_state)
    );

    if (event != NULL) {
        if (event_publish_from_isr(event) != EVENT_OK) {
            event_free(event);  // Publish failed, free event
        }
        // After successful publish, event needs to be freed somewhere (not recommended)
    }
}
```

---

## Best Practices

### Publish, teardown, and manual consumption contract

| Scenario | Requirement |
|----------|-------------|
| **Publish** | Call `event_register_type` first; prefer `event_publish_copy` / `event_create_with_data`. `event_publish` / `event_publish_from_isr` validate `flags` and `data_len`; inconsistent hand-built `event_t` values return `EVENT_ERR_INVALID_ARG` |
| **Module teardown** | `event_system_stop()` → `event_unsubscribe_all(subscriber_id)` → then free `user_data`. After `event_unsubscribe` returns, the dispatcher may still invoke callbacks from an earlier snapshot |
| **Manual `process_one` / `process_all`** | Use only after **init** and **before the first** `event_dispatcher_start()`. After start→stop, or while the dispatcher thread is `RUNNING`, calls from other threads return `EVENT_ERR_INVALID_ARG` |
| **`DROP_LOWEST` overflow policy** | Priority eviction applies only to thread-side `event_publish`; `event_publish_from_isr` still uses `DROP_NEWEST` when the queue is full |

### 1. Event Design Principles

```c
// ✅ Recommended: Use compact data structures
typedef struct {
    int16_t temperature;
    int16_t humidity;
    uint32_t timestamp;
} sensor_data_t;

void publish_sensor_data(sensor_data_t *data)
{
    event_publish_copy(
        EVENT_TYPE_SENSOR_DATA,
        EVENT_PRIORITY_NORMAL,
        data,
        sizeof(*data)
    );
}

// ❌ Avoid: Passing oversized data
void publish_large_data(void *large_buffer, size_t size)  // size > 256 bytes
{
    // Consider using pointer + ownership transfer pattern
}
```

### 2. Callback Function Design

```c
// ✅ Recommended: Fast processing, async execution of time-consuming operations
static void fast_callback(const event_t *event, void *user_data)
{
    // Fast processing: copy data, trigger semaphores, etc.
    k_sem_give(&processing_sem);
}

// ❌ Avoid: Executing time-consuming operations in callbacks
static void slow_callback(const event_t *event, void *user_data)
{
    k_msleep(1000);  // Blocks dispatcher!
    // Complex calculations...
}
```

### 3. Memory Management

For complete conventions, see **[Memory and Ownership](#memory-and-ownership)** above. Only common patterns listed here.

```c
// ✅ Recommended: event_publish_copy — for normal tasks
void safe_publish(void)
{
    uint8_t data[16];
    event_publish_copy(EVENT_TYPE_GENERIC, EVENT_PRIORITY_NORMAL,
                       data, sizeof(data));
}

// ✅ Recommended: event_publish_copy_rt — for ISR and real-time tasks
void realtime_safe_publish(void)
{
    uint8_t data[16];
    event_status_t ret = event_publish_copy_rt(
        EVENT_TYPE_GENERIC, EVENT_PRIORITY_HIGH,
        data, sizeof(data)
    );
    if (ret == EVENT_ERR_NO_MEM) {
        // Slab pool exhausted, take degradation measures
    }
}

// ✅ Acceptable: Stack event_t, small data inline storage
void stack_publish(void)
{
    uint32_t counter = 42;
    event_t event = {
        .type = EVENT_TYPE_GENERIC,
        .priority = EVENT_PRIORITY_NORMAL,
        .timestamp = k_uptime_get_32(),
        .source_id = 0,
        .flags = EVENT_FLAG_DATA_INLINE,
        .data_len = sizeof(counter),
    };
    memcpy(event.data.inline_data, &counter, sizeof(counter));
    event_publish(&event);
}

// ⚠️ Avoid manual management: event_create_with_data + event_publish is leak-prone
// Recommended to use event_publish_copy or event_publish_copy_rt
```

### 4. Error Handling

```c
void robust_publish(const void *data, size_t len)
{
    event_status_t ret = event_publish_copy(
        EVENT_TYPE_SENSOR_DATA,
        EVENT_PRIORITY_NORMAL,
        data,
        len
    );

    switch (ret) {
    case EVENT_OK:
        // Success
        break;
    case EVENT_ERR_QUEUE_FULL:
        LOG_WRN("Event queue full, data dropped");
        break;
    case EVENT_ERR_INVALID_ARG:
        LOG_ERR("Invalid event arguments");
        break;
    case EVENT_ERR_NO_MEM:
        LOG_ERR("Memory allocation failed");
        break;
    default:
        LOG_ERR("Unknown error: %d", ret);
        break;
    }
}
```

### 5. Statistics and Debugging

```c
void print_event_stats(void)
{
    uint32_t total, depth, dropped;
    event_get_statistics(&total, &depth, &dropped);

    LOG_INF("=== Event System Statistics ===");
    LOG_INF("Total events processed: %u", total);
    LOG_INF("Current queue depth: %u", depth);
    LOG_INF("Dropped events: %u", dropped);

    if (dropped > 0) {
        LOG_WRN("Consider increasing CONFIG_EVENT_QUEUE_SIZE");
    }
}

// Slab pool status query (requires CONFIG_EVENT_RUNTIME_STATUS=y)
void print_slab_stats(void)
{
    event_slab_stats_t stats;
    event_get_slab_stats(&stats);

    LOG_INF("=== Slab Pool Statistics ===");
    LOG_INF("CRITICAL: %u/%u used", stats.critical_used, stats.critical_total);
    LOG_INF("HIGH:      %u/%u used", stats.high_used, stats.high_total);
    LOG_INF("NORMAL:    %u/%u used", stats.normal_used, stats.normal_total);
    LOG_INF("Data 256B: %u/%u used", stats.data_256_used, stats.data_256_total);
    LOG_INF("Data 1KB:  %u/%u used", stats.data_1k_used, stats.data_1k_total);
    LOG_INF("Data 4KB:  %u/%u used", stats.data_4k_used, stats.data_4k_total);
    LOG_INF("Fallback to k_malloc: %u", stats.fallback_count);

    // Check Slab availability for specific priority
    if (!event_slab_available(EVENT_PRIORITY_HIGH)) {
        LOG_WRN("HIGH priority Slab exhausted!");
    }
}
```

---

## Troubleshooting

### Common Issues

#### 1. Event Not Delivered

**Symptom**: Callback not executed after publishing event

**Checklist**:
- [ ] Is the event type registered?
- [ ] Did the subscriber successfully subscribe?
- [ ] Is the event dispatcher started?
- [ ] Is the callback function correctly implemented?

```c
// Debug steps
void debug_event_delivery(void)
{
    // Check event type
    LOG_INF("Event type name: %s", event_get_type_name(MY_EVENT_TYPE));

    // Check subscriber count
    uint32_t count = event_get_subscriber_count(MY_EVENT_TYPE);
    LOG_INF("Subscriber count: %u", count);

    // Check system status
    LOG_INF("System running: %s",
            event_system_is_running() ? "yes" : "no");
}
```

#### 2. Queue Overflow

**Symptom**: Log shows "Event queue full, event dropped"

**Solutions**:
```kconfig
# Increase queue size
CONFIG_EVENT_QUEUE_SIZE=128

# Or optimize event processing speed
# Reduce processing time in callbacks
```

#### 3. Memory Leaks / Wild Pointers

**Symptom**: Insufficient memory after system runs for a period, or random crashes

**Checkpoints**:
- Prefer `event_publish_copy`; see [Memory and Ownership](#memory-and-ownership)
- `event_publish` + `event_create*`: Dispatcher thread **does NOT** free `event_t` shell; heap path is leak-prone; failure path must `event_free`
- **Prohibited**: `data` of `event_publish` pointing to local stack buffer in publishing function (callback may execute after function returns)
- Avoid heap allocation in ISR; memory pointed to by `data` must remain valid until dispatch
- Do not `k_free(event->data)` in callbacks (unless project has other ownership agreements)

#### 4. Deadlock Issues

**Symptom**: System hangs, watchdog resets

**Cause**: Calling potentially blocking APIs in event callbacks

**Solutions**:
```c
// ❌ Wrong example
static void bad_callback(const event_t *event, void *user_data)
{
    k_mutex_lock(&some_mutex, K_FOREVER);  // Potential deadlock
}

// ✅ Correct example
static void good_callback(const event_t *event, void *user_data)
{
    k_sem_give(&processing_sem);  // Non-blocking
}
```

### Error Code Reference

| Error Code | Value | Description |
|------------|-------|-------------|
| EVENT_OK | 0 | Success |
| EVENT_ERR_NO_MEM | -1 | Out of memory |
| EVENT_ERR_QUEUE_FULL | -2 | Queue full |
| EVENT_ERR_QUEUE_EMPTY | -3 | Queue empty |
| EVENT_ERR_INVALID_ARG | -4 | Invalid argument |
| EVENT_ERR_NOT_FOUND | -5 | Not found |
| EVENT_ERR_NO_SUBSCRIBER | -6 | No subscriber |
| EVENT_ERR_TIMEOUT | -7 | Timeout |
| EVENT_ERR_NOT_RUNNING | -8 | Event system/dispatcher not started |

---

## Appendix

### Header File Includes

Public API lives under `include/zeplod/`; use `#include <zeplod/...>` or the umbrella `zeplod_framework.h`.

```c
#include <zeplod/event_system.h>      // Core API
#include <zeplod/event_dispatcher.h>  // Dispatcher API
/* event_queue.h is internal; use event_system / event_dispatcher APIs in application code */
```

### Related Documentation

- [Module System Detailed Usage Guide.md](32-module-system-guide.md)
- [Developer Getting Started Guide.md](../00-getting-started/04-developer-guide.md)
- [Zephyr RTOS Official Documentation](https://docs.zephyrproject.org/)
