/**
 * @file event_memory.c
 * @brief 事件系统内存管理模块实现 (Event Memory Management Implementation)
 *
 * 实现 Slab 池定义和内存管理函数。
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-15
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-15       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            include 顺序修正；耗尽回调指针加 volatile 并注明同步契约
 *
 */

#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include "event_memory.h"

LOG_MODULE_REGISTER(event_memory, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * Slab 池定义 (Slab Pool Definitions)
 * 使用 K_MEM_SLAB_DEFINE 定义 Slab 池
 * ============================================================================= */

#if EVENT_SLAB_ENABLED

#if EVENT_SLAB_CRITICAL_AVAILABLE
/** CRITICAL 优先级事件 Slab 池定义 */
K_MEM_SLAB_DEFINE(event_slab_critical, CONFIG_EVENT_STRUCT_SIZE, CONFIG_EVENT_SLAB_CRITICAL_COUNT, sizeof(void*));
#endif

#if EVENT_SLAB_HIGH_AVAILABLE
/** HIGH 优先级事件 Slab 池定义 */
K_MEM_SLAB_DEFINE(event_slab_high, CONFIG_EVENT_STRUCT_SIZE, CONFIG_EVENT_SLAB_HIGH_COUNT, sizeof(void*));
#endif

/** NORMAL/LOW 优先级事件 Slab 池定义 */
K_MEM_SLAB_DEFINE(event_slab_normal, CONFIG_EVENT_STRUCT_SIZE, CONFIG_EVENT_SLAB_NORMAL_COUNT, sizeof(void*));

#if EVENT_SLAB_LARGE_AVAILABLE

#if EVENT_SLAB_256_AVAILABLE
/** 256 字节数据块 Slab 池定义 */
K_MEM_SLAB_DEFINE(event_slab_data_256, 256, CONFIG_EVENT_SLAB_LARGE_256_COUNT, sizeof(void*));
#endif

#if EVENT_SLAB_1K_AVAILABLE
/** 1KB 数据块 Slab 池定义 */
K_MEM_SLAB_DEFINE(event_slab_data_1k, 1024, CONFIG_EVENT_SLAB_LARGE_1K_COUNT, sizeof(void*));
#endif

#if EVENT_SLAB_4K_AVAILABLE
/** 4KB 数据块 Slab 池定义 */
K_MEM_SLAB_DEFINE(event_slab_data_4k, 4096, CONFIG_EVENT_SLAB_LARGE_4K_COUNT, sizeof(void*));
#endif

#endif /* EVENT_SLAB_LARGE_AVAILABLE */

#endif /* EVENT_SLAB_ENABLED */

/* =============================================================================
 * 统计计数器 (Statistics Counters)
 * 条件编译：CONFIG_EVENT_RUNTIME_STATUS
 * ============================================================================= */

#if defined(CONFIG_EVENT_RUNTIME_STATUS) && (CONFIG_EVENT_RUNTIME_STATUS == 1)
/** 回退到 k_malloc 的计数器 */
static atomic_t g_fallback_count = ATOMIC_INIT(0);
#endif

/* =============================================================================
 * Slab 耗尽回调实现 (Slab Exhausted Callback Implementation)
 * 条件编译：CONFIG_EVENT_SLAB_EXHAUSTED_CB
 * ============================================================================= */

#if defined(CONFIG_EVENT_SLAB_EXHAUSTED_CB) && (CONFIG_EVENT_SLAB_EXHAUSTED_CB == 1)
/**
 * Slab 耗尽回调函数指针。
 * 同步契约：写侧（event_register_slab_exhausted_cb）在 irq_lock 临界区内更新；
 * 读侧（notify_slab_exhausted）为单次指针读——单核对齐字长读本身原子，volatile
 * 阻止编译器把读缓存到寄存器或与后续调用重排；跨核一致性由写侧先写后用的
 * 发布顺序约定保证（回调注册须先于可能触发耗尽的分配路径完成）。
 */
static event_slab_exhausted_cb_t volatile g_slab_exhausted_cb = NULL;

void event_register_slab_exhausted_cb(event_slab_exhausted_cb_t cb) {
    unsigned int key = irq_lock();

    g_slab_exhausted_cb = cb;
    irq_unlock(key);
}

/**
 * @brief 通知 Slab 耗尽（内部函数）
 *
 * @param priority 触发耗尽的优先级
 * @param slab_name Slab 名称
 */
static inline void notify_slab_exhausted(event_priority_t priority, const char* slab_name) {
    if (g_slab_exhausted_cb != NULL) {
        g_slab_exhausted_cb(priority, slab_name);
    }
}
#else  /* !CONFIG_EVENT_SLAB_EXHAUSTED_CB */

/** 空实现的耗尽通知（编译优化后会被移除） */
static inline void notify_slab_exhausted(event_priority_t priority, const char* slab_name) {
    (void) priority;
    (void) slab_name;
}
#endif /* CONFIG_EVENT_SLAB_EXHAUSTED_CB */

void event_memory_notify_slab_exhausted(event_priority_t priority, const char* slab_name) {
    notify_slab_exhausted(priority, slab_name);
}

/* =============================================================================
 * 内部分配函数实现 (Internal Allocation Functions Implementation)
 * ============================================================================= */

#if EVENT_SLAB_ENABLED
typedef struct {
    struct k_mem_slab* slab;
    size_t             block_size;
    uint32_t           num_blocks;
} event_object_slab_meta_t;

static const event_object_slab_meta_t g_event_object_slab_meta[] = {
#if EVENT_SLAB_CRITICAL_AVAILABLE
    {&event_slab_critical, CONFIG_EVENT_STRUCT_SIZE, CONFIG_EVENT_SLAB_CRITICAL_COUNT},
#endif
#if EVENT_SLAB_HIGH_AVAILABLE
    {&event_slab_high, CONFIG_EVENT_STRUCT_SIZE, CONFIG_EVENT_SLAB_HIGH_COUNT},
#endif
    {&event_slab_normal, CONFIG_EVENT_STRUCT_SIZE, CONFIG_EVENT_SLAB_NORMAL_COUNT},
};

static bool event_memory_event_ptr_in_pool(const void* ptr, const event_object_slab_meta_t* meta) {
    if (ptr == NULL || meta == NULL || meta->slab == NULL || meta->num_blocks == 0U || meta->block_size == 0U) {
        return false;
    }

    uintptr_t block = (uintptr_t) ptr;
    uintptr_t base = (uintptr_t) meta->slab->buffer;
    size_t    total = meta->block_size * meta->num_blocks;

    if (block < base || block >= (base + total)) {
        return false;
    }

    return ((block - base) % meta->block_size) == 0U;
}

struct k_mem_slab* event_memory_resolve_event_slab_for_ptr(void* ptr) {
    for (size_t i = 0; i < ARRAY_SIZE(g_event_object_slab_meta); i++) {
        if (event_memory_event_ptr_in_pool(ptr, &g_event_object_slab_meta[i])) {
            return g_event_object_slab_meta[i].slab;
        }
    }

    return NULL;
}

#if EVENT_SLAB_LARGE_AVAILABLE
typedef struct {
    struct k_mem_slab* slab;
    size_t             block_size;
    uint32_t           num_blocks;
    uint8_t            flag;
} event_data_slab_meta_t;

static const event_data_slab_meta_t g_event_data_slab_meta[] = {
#if EVENT_SLAB_256_AVAILABLE
    {&event_slab_data_256, 256U, CONFIG_EVENT_SLAB_LARGE_256_COUNT, EVENT_FLAG_SLAB_256},
#endif
#if EVENT_SLAB_1K_AVAILABLE
    {&event_slab_data_1k, 1024U, CONFIG_EVENT_SLAB_LARGE_1K_COUNT, EVENT_FLAG_SLAB_1K},
#endif
#if EVENT_SLAB_4K_AVAILABLE
    {&event_slab_data_4k, 4096U, CONFIG_EVENT_SLAB_LARGE_4K_COUNT, EVENT_FLAG_SLAB_4K},
#endif
};

static bool event_memory_slab_ptr_in_pool(const void* ptr, const event_data_slab_meta_t* meta) {
    if (ptr == NULL || meta == NULL || meta->slab == NULL || meta->num_blocks == 0U || meta->block_size == 0U) {
        return false;
    }

    uintptr_t block = (uintptr_t) ptr;
    uintptr_t base = (uintptr_t) meta->slab->buffer;
    size_t    total = meta->block_size * meta->num_blocks;

    if (block < base || block >= (base + total)) {
        return false;
    }

    return ((block - base) % meta->block_size) == 0U;
}

bool event_memory_data_slab_set_flag(event_t* event, struct k_mem_slab* slab) {
    if (event == NULL || slab == NULL) {
        return false;
    }

    for (size_t i = 0; i < ARRAY_SIZE(g_event_data_slab_meta); i++) {
        if (slab == g_event_data_slab_meta[i].slab) {
            event->flags |= g_event_data_slab_meta[i].flag;
            return true;
        }
    }

    LOG_ERR("Unknown data slab pointer %p, cannot set marker", slab);
    return false;
}

struct k_mem_slab* event_memory_data_slab_from_flag(uint8_t flag) {
    const uint8_t masked = flag & EVENT_FLAG_SLAB_MASK;

    if (masked == 0U) {
        return NULL;
    }

    for (size_t i = 0; i < ARRAY_SIZE(g_event_data_slab_meta); i++) {
        if (g_event_data_slab_meta[i].flag == masked) {
            return g_event_data_slab_meta[i].slab;
        }
    }

    return NULL;
}

struct k_mem_slab* event_memory_resolve_data_slab_for_ptr(void* ptr) {
    for (size_t i = 0; i < ARRAY_SIZE(g_event_data_slab_meta); i++) {
        if (event_memory_slab_ptr_in_pool(ptr, &g_event_data_slab_meta[i])) {
            return g_event_data_slab_meta[i].slab;
        }
    }

    return NULL;
}
#endif /* EVENT_SLAB_LARGE_AVAILABLE */

struct k_mem_slab* event_memory_select_event_slab(event_priority_t priority) {
    switch (priority) {
#if EVENT_SLAB_CRITICAL_AVAILABLE
    case EVENT_PRIORITY_CRITICAL:
        return &event_slab_critical;
#endif

#if EVENT_SLAB_HIGH_AVAILABLE
    case EVENT_PRIORITY_HIGH:
        return &event_slab_high;
#endif

    case EVENT_PRIORITY_NORMAL:
    case EVENT_PRIORITY_LOW:
    default:
        /* SIL-2: LOW 与 NORMAL 共用同一 slab 池是有意设计，
         * 两者在典型场景下数量最多，合并可减少内存碎片 */
        return &event_slab_normal;
    }
}

struct k_mem_slab* event_memory_select_data_slab(size_t data_len) {
    if (data_len == 0) {
        return NULL;
    }

#if EVENT_SLAB_LARGE_AVAILABLE
    for (size_t i = 0; i < ARRAY_SIZE(g_event_data_slab_meta); i++) {
        if (data_len <= g_event_data_slab_meta[i].block_size) {
            return g_event_data_slab_meta[i].slab;
        }
    }
#endif

    return NULL;
}

struct k_mem_slab* event_memory_select_data_slab_with_fallback(size_t data_len) {
    if (data_len == 0) {
        return NULL;
    }

#if EVENT_SLAB_LARGE_AVAILABLE
    for (size_t i = 0; i < ARRAY_SIZE(g_event_data_slab_meta); i++) {
        const event_data_slab_meta_t* meta = &g_event_data_slab_meta[i];

        if (data_len <= meta->block_size && k_mem_slab_num_free_get(meta->slab) > 0) {
            return meta->slab;
        }
    }
#endif

    return NULL;
}
#else  /* !EVENT_SLAB_ENABLED */

struct k_mem_slab* event_memory_select_event_slab(event_priority_t priority) {
    ARG_UNUSED(priority);
    return NULL;
}

struct k_mem_slab* event_memory_resolve_event_slab_for_ptr(void* ptr) {
    ARG_UNUSED(ptr);
    return NULL;
}

struct k_mem_slab* event_memory_select_data_slab(size_t data_len) {
    if (data_len == 0) {
        return NULL;
    }
    return NULL;
}

struct k_mem_slab* event_memory_select_data_slab_with_fallback(size_t data_len) {
    if (data_len == 0) {
        return NULL;
    }
    return NULL;
}
#endif /* EVENT_SLAB_ENABLED */

/* =============================================================================
 * 回退计数 API (Fallback Count API)
 * ============================================================================= */

void event_memory_inc_fallback_count(void) {
#if defined(CONFIG_EVENT_RUNTIME_STATUS) && (CONFIG_EVENT_RUNTIME_STATUS == 1)
    atomic_inc(&g_fallback_count);
#else
    /* 无统计功能，空操作 */
#endif
}

/* =============================================================================
 * 运行时状态 API 实现 (Runtime Status API Implementation)
 * 条件编译：CONFIG_EVENT_RUNTIME_STATUS
 * ============================================================================= */

#if defined(CONFIG_EVENT_RUNTIME_STATUS) && (CONFIG_EVENT_RUNTIME_STATUS == 1)
bool event_slab_available(event_priority_t priority) {
    struct k_mem_slab* slab = event_memory_select_event_slab(priority);

    if (slab == NULL) {
        return false;
    }

    return k_mem_slab_num_free_get(slab) > 0;
}

uint32_t event_slab_remaining(event_priority_t priority) {
    struct k_mem_slab* slab = event_memory_select_event_slab(priority);

    if (slab == NULL) {
        return 0;
    }

    return k_mem_slab_num_free_get(slab);
}

void event_get_slab_stats(event_slab_stats_t* stats) {
    if (stats == NULL) {
        return;
    }

    /* 清零结构体 */
    memset(stats, 0, sizeof(event_slab_stats_t));

#if EVENT_SLAB_ENABLED

#if EVENT_SLAB_CRITICAL_AVAILABLE
    stats->critical_used = k_mem_slab_num_used_get(&event_slab_critical);
    stats->critical_total = CONFIG_EVENT_SLAB_CRITICAL_COUNT;
#endif

#if EVENT_SLAB_HIGH_AVAILABLE
    stats->high_used = k_mem_slab_num_used_get(&event_slab_high);
    stats->high_total = CONFIG_EVENT_SLAB_HIGH_COUNT;
#endif

    stats->normal_used = k_mem_slab_num_used_get(&event_slab_normal);
    stats->normal_total = CONFIG_EVENT_SLAB_NORMAL_COUNT;

#if EVENT_SLAB_256_AVAILABLE
    stats->data_256_used = k_mem_slab_num_used_get(&event_slab_data_256);
    stats->data_256_total = CONFIG_EVENT_SLAB_LARGE_256_COUNT;
#endif

#if EVENT_SLAB_1K_AVAILABLE
    stats->data_1k_used = k_mem_slab_num_used_get(&event_slab_data_1k);
    stats->data_1k_total = CONFIG_EVENT_SLAB_LARGE_1K_COUNT;
#endif

#if EVENT_SLAB_4K_AVAILABLE
    stats->data_4k_used = k_mem_slab_num_used_get(&event_slab_data_4k);
    stats->data_4k_total = CONFIG_EVENT_SLAB_LARGE_4K_COUNT;
#endif

#endif /* EVENT_SLAB_ENABLED */

    stats->fallback_count = atomic_get(&g_fallback_count);
}
#endif /* CONFIG_EVENT_RUNTIME_STATUS */

/* =============================================================================
 * 内存调试 API 实现 (Memory Debug API Implementation)
 * 条件编译：CONFIG_EVENT_DEBUG_MEM
 * ============================================================================= */

#if defined(CONFIG_EVENT_DEBUG_MEM) && (CONFIG_EVENT_DEBUG_MEM == 1)
/** 调试跟踪条目 */
typedef struct debug_track_entry {
    void*                     ptr;       /**< 分配的内存指针 */
    size_t                    size;      /**< 分配大小 */
    event_priority_t          priority;  /**< 事件优先级 */
    uint32_t                  timestamp; /**< 分配时间戳 */
    struct debug_track_entry* next;      /**< 链表下一项 */
} debug_track_entry_t;

/** 调试跟踪链表头 */
static debug_track_entry_t* g_debug_track_head = NULL;

/** 调试跟踪链表锁 */
static struct k_spinlock g_debug_track_lock;

/** 调试跟踪池大小（可配置） */
#ifndef CONFIG_EVENT_DEBUG_TRACK_COUNT
#define CONFIG_EVENT_DEBUG_TRACK_COUNT 64
#endif

/** 调试跟踪池 */
K_MEM_SLAB_DEFINE(debug_track_slab, sizeof(debug_track_entry_t), CONFIG_EVENT_DEBUG_TRACK_COUNT, sizeof(void*));

/** 泄漏报告快照，避免持有自旋锁时输出日志 */
typedef struct {
    void*            ptr;
    size_t           size;
    event_priority_t priority;
    uint32_t         timestamp;
} debug_track_snapshot_t;

static debug_track_snapshot_t g_debug_track_snapshot[CONFIG_EVENT_DEBUG_TRACK_COUNT];
K_MUTEX_DEFINE(g_debug_dump_lock);

/** LOW-1: 调试跟踪 slab 耗尽时被丢弃的分配次数。
 * 当 debug_track_slab 满时，event_check_leaks 报告的数量会偏低；
 * 此计数器记录追踪缺口，使诊断者可识别"统计不完整"的情况。 */
static atomic_t g_debug_track_misses = ATOMIC_INIT(0);

/**
 * @brief 跟踪内存分配（内部函数）
 *
 * @param ptr 分配的内存指针
 * @param size 分配大小
 * @param priority 事件优先级
 */
void event_debug_track_alloc(void* ptr, size_t size, event_priority_t priority) {
    if (ptr == NULL) {
        return;
    }

    debug_track_entry_t* entry = NULL;

    if (k_mem_slab_alloc(&debug_track_slab, (void**) &entry, K_NO_WAIT) != 0) {
        /* LOW-1: 累计追踪缺口，使 dump_leaks 可暴露统计不完整的事实 */
        atomic_inc(&g_debug_track_misses);
        if (!k_is_in_isr()) {
            LOG_WRN("Debug track slab exhausted");
        }
        return;
    }

    entry->ptr = ptr;
    entry->size = size;
    entry->priority = priority;
    entry->timestamp = k_uptime_get_32();

    k_spinlock_key_t key = k_spin_lock(&g_debug_track_lock);
    entry->next = g_debug_track_head;
    g_debug_track_head = entry;
    k_spin_unlock(&g_debug_track_lock, key);
}

/**
 * @brief 取消跟踪内存分配（内部函数）
 *
 * @param ptr 要取消跟踪的内存指针
 */
void event_debug_untrack_alloc(void* ptr) {
    if (ptr == NULL) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&g_debug_track_lock);

    debug_track_entry_t** pp = &g_debug_track_head;

    while (*pp != NULL) {
        if ((*pp)->ptr == ptr) {
            debug_track_entry_t* entry = *pp;
            *pp = entry->next;
            k_spin_unlock(&g_debug_track_lock, key);
            k_mem_slab_free(&debug_track_slab, entry);
            return;
        }
        pp = &(*pp)->next;
    }

    k_spin_unlock(&g_debug_track_lock, key);
    if (!k_is_in_isr()) {
        LOG_WRN("Ptr %p not found in debug track", ptr);
    }
}

uint32_t event_check_leaks(void) {
    uint32_t count = 0;

    k_spinlock_key_t     key = k_spin_lock(&g_debug_track_lock);
    debug_track_entry_t* entry = g_debug_track_head;
    while (entry != NULL) {
        count++;
        entry = entry->next;
    }
    k_spin_unlock(&g_debug_track_lock, key);

    /* LOW-1: 若曾发生追踪缺口，警告调用方：返回值低估真实泄漏数 */
    uint32_t misses = (uint32_t) atomic_get(&g_debug_track_misses);
    if (misses > 0) {
        LOG_WRN("Leak count may be incomplete: %u allocations untracked due to slab exhaustion", misses);
    }

    return count;
}

void event_dump_leaks(void) {
    if (k_is_in_isr()) {
        return;
    }

    k_mutex_lock(&g_debug_dump_lock, K_FOREVER);

    uint32_t             count = 0;
    k_spinlock_key_t     key = k_spin_lock(&g_debug_track_lock);
    debug_track_entry_t* entry = g_debug_track_head;
    while (entry != NULL && count < ARRAY_SIZE(g_debug_track_snapshot)) {
        g_debug_track_snapshot[count].ptr = entry->ptr;
        g_debug_track_snapshot[count].size = entry->size;
        g_debug_track_snapshot[count].priority = entry->priority;
        g_debug_track_snapshot[count].timestamp = entry->timestamp;
        count++;
        entry = entry->next;
    }
    k_spin_unlock(&g_debug_track_lock, key);

    uint32_t misses = (uint32_t) atomic_get(&g_debug_track_misses);

    if (count == 0U) {
        if (misses > 0) {
            LOG_WRN("No tracked leaks, but %u allocations were untracked (slab exhausted)", misses);
        } else {
            LOG_INF("No memory leaks detected");
        }
        k_mutex_unlock(&g_debug_dump_lock);
        return;
    }

    LOG_INF("=== Memory Leak Report ===");

    for (uint32_t i = 0; i < count; i++) {
        LOG_INF("Leak #%u: ptr=%p, size=%zu, priority=%d, time=%u", i + 1, g_debug_track_snapshot[i].ptr,
                g_debug_track_snapshot[i].size, g_debug_track_snapshot[i].priority,
                g_debug_track_snapshot[i].timestamp);
    }

    LOG_INF("Total leaks: %u", count);
    if (misses > 0) {
        LOG_WRN("Untracked allocations (slab exhausted): %u; total leaks may exceed %u", misses, count);
    }
    k_mutex_unlock(&g_debug_dump_lock);
}
#endif /* CONFIG_EVENT_DEBUG_MEM */
