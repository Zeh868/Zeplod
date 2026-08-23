/**
 * @file sys_memory.c
 * @brief 系统内存管理实现（空闲链表版）
 *
 * 支持多内存池，每个池基于空闲链表（first-fit）管理。
 * 提供分配、释放、重新分配、统计、跟踪等功能。
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <limits.h>
#include <stdalign.h>
#include <string.h>
#include <zeplod/sys_memory.h>

LOG_MODULE_REGISTER(sys_memory, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部定义
 * ============================================================================= */

#ifndef CONFIG_SYS_MEMORY_POOL_SIZE
#define CONFIG_SYS_MEMORY_POOL_SIZE 8192
#endif

/* 当内存池禁用时，减少跟踪记录数以节省内存 */
#if defined(CONFIG_SYS_MEMORY_ENABLE) && (CONFIG_SYS_MEMORY_POOL_SIZE > 0)
#define DEFAULT_POOL_SIZE CONFIG_SYS_MEMORY_POOL_SIZE ///< 默认池大小（字节）
#define MAX_ALLOCATIONS   256                         ///< 最大跟踪分配数
#else
#define DEFAULT_POOL_SIZE 0 ///< 内存池禁用
#define MAX_ALLOCATIONS   8 ///< 最小跟踪记录数
#endif

#define MEMORY_MAGIC       0x4D454D30U                                 ///< 魔数 - 有效分配标识（"MEM0"）
#define MEMORY_FREED_MAGIC 0x46524545U                                 ///< 魔数 - 已释放块标识（"FREE"）
#define MEMORY_ALIGN_BYTES 8U                                          ///< 内存对齐字节数（保证对齐到8字节）
#define MIN_BLOCK_SIZE     (sizeof(free_block_t) + MEMORY_ALIGN_BYTES) ///< 最小可分割块大小

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

/**
 * @brief 空闲块结构体（位于空闲内存中）
 */
typedef struct free_block {
    struct free_block* next; ///< 下一个空闲块
    size_t             size; ///< 空闲块总大小（包括本头部）
} free_block_t;

/**
 * @brief 分配块头部结构体（位于用户数据之前）
 */
typedef struct alloc_header {
    uint32_t magic;             ///< 魔数，用于验证
    uint32_t pool_type;         ///< 所属内存池类型
    size_t   requested_size;    ///< 用户请求大小（原始未对齐）
    size_t   aligned_size;      ///< 实际分配的用户数据大小（已对齐）
    size_t   actual_block_size; ///< 实际占用的总块大小（含头部和尾部碎片）
    /* 之后紧跟用户数据 */
} alloc_header_t;

/**
 * @brief 内存池控制结构
 */
typedef struct mem_pool {
    uint8_t*            buffer;       ///< 池内存缓冲区起始地址
    size_t              total_size;   ///< 池总大小（字节）
    free_block_t*       free_list;    ///< 空闲块链表头
    size_t              used_size;    ///< 当前已使用大小（字节）
    size_t              max_used;     ///< 历史最大使用量（字节）
    uint32_t            alloc_count;  ///< 累计分配次数
    uint32_t            free_count;   ///< 累计释放次数
    uint32_t            fail_count;   ///< 分配失败次数
    uint32_t            active_count; ///< 当前活跃分配数（不受统计重置影响）
    sys_mem_pool_type_t type;         ///< 池类型
    struct k_mutex      lock;         ///< 池互斥锁
    bool                initialized;  ///< 池是否已初始化
} mem_pool_t;

/**
 * @brief 内存跟踪器结构（环形写入 + 按槽有效位）
 *
 * 每条记录独立 slot_active[i]，移除时不清尾紧凑，避免与 head 游标冲突。
 */
typedef struct mem_tracker {
    sys_mem_alloc_info_t records[MAX_ALLOCATIONS];     ///< 记录数组
    bool                 slot_active[MAX_ALLOCATIONS]; ///< 槽位是否表示当前有效分配
    uint32_t             head;                         ///< 下次写入位置（环形）
    uint32_t             count;                        ///< slot_active 为 true 的槽位数
    uint32_t             max_records;                  ///< 最大记录数
    bool                 tracking_enabled;             ///< 跟踪是否启用
    struct k_mutex       lock;                         ///< 跟踪器互斥锁
} mem_tracker_t;

/**
 * @brief 内存系统全局控制块
 */
typedef struct sys_mem_cb {
    mem_pool_t       pools[SYS_MEM_POOL_COUNT];
    mem_tracker_t    tracker;
    sys_mem_config_t config;
} sys_mem_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static sys_mem_cb_t g_sys_mem;

/** 静态分配的内存池缓冲区（仅当 SYS_MEMORY_ENABLE 时分配）
 *  @note 使用对齐属性确保缓冲区适合存放任何结构体
 */
#if defined(CONFIG_SYS_MEMORY_ENABLE) && (DEFAULT_POOL_SIZE > 0)
/* 确保缓冲区对齐到 8 字节，适合存放 free_block_t 等结构 */
static __aligned(8) uint8_t g_mem_buffer[SYS_MEM_POOL_COUNT][DEFAULT_POOL_SIZE];
#else
/* 当内存池禁用或大小为0时，不分配缓冲区 */
static uint8_t (*g_mem_buffer)[DEFAULT_POOL_SIZE] = NULL;
#endif

/* =============================================================================
 * 内部辅助函数
 * ============================================================================= */

/**
 * @brief 根据类型获取内存池指针
 */
static mem_pool_t* get_pool(sys_mem_pool_type_t type) {
    if (type >= SYS_MEM_POOL_COUNT) {
        return NULL;
    }
    return &g_sys_mem.pools[type];
}

/**
 * @brief 向上对齐大小到指定边界
 */
static bool align_up_size(size_t size, size_t align, size_t* aligned) {
    if (aligned == NULL || align == 0U || (align & (align - 1U)) != 0U) {
        return false;
    }
    if (size > (SIZE_MAX - (align - 1U))) {
        return false;
    }
    *aligned = (size + align - 1U) & ~(align - 1U);
    return true;
}

/**
 * @brief 从用户指针获取分配头部
 */
static alloc_header_t* get_alloc_header(void* ptr) {
    if (ptr == NULL) {
        return NULL;
    }
    return (alloc_header_t*) ((uint8_t*) ptr - sizeof(alloc_header_t));
}

/**
 * @brief 检查指针是否在池内（未加锁，调用者需持有锁）
 */
static bool ptr_in_pool(const mem_pool_t* pool, const void* ptr) {
    if (pool == NULL || ptr == NULL || !pool->initialized) {
        return false;
    }
    uintptr_t start = (uintptr_t) pool->buffer;
    uintptr_t end = start + pool->total_size;
    uintptr_t addr = (uintptr_t) ptr;
    return (addr >= start && addr < end);
}

/**
 * @brief 从指针获取池（遍历所有池，不加锁，仅用于查找池）
 */
static mem_pool_t* find_pool_containing_ptr(const void* ptr) {
    for (int i = 0; i < SYS_MEM_POOL_COUNT; i++) {
        mem_pool_t* pool = &g_sys_mem.pools[i];
        if (!pool->initialized) {
            continue;
        }
        /* 粗略范围检查，可能误判，但分配块头部在池内，后续会验证魔数 */
        if (ptr_in_pool(pool, ptr)) {
            return pool;
        }
    }
    return NULL;
}

/**
 * @brief 从指针获取池和分配头部（带锁验证）
 *
 * 锁定池后，验证指针有效性和魔数。
 * 返回时已持有池锁，调用者必须释放。
 */
static mem_pool_t* lock_pool_from_ptr(void* ptr, alloc_header_t** header_out) {
    mem_pool_t* pool = find_pool_containing_ptr(ptr);
    if (pool == NULL) {
        return NULL;
    }

    k_mutex_lock(&pool->lock, K_FOREVER);

    /* SIL-2: 再次验证指针是否在池内（锁内确保一致性） */
    if (!ptr_in_pool(pool, ptr)) {
        k_mutex_unlock(&pool->lock);
        return NULL;
    }

    /* SIL-2: 确保指针距离池起始位置至少有头部大小
     * 防止 ptr 指向池起始位置,导致 header 计算到池外
     */
    uintptr_t ptr_offset = (uintptr_t) ptr - (uintptr_t) pool->buffer;
    if (ptr_offset < sizeof(alloc_header_t)) {
        LOG_ERR("Pointer too close to pool start: offset=%zu < header=%zu", (size_t) ptr_offset,
                sizeof(alloc_header_t));
        k_mutex_unlock(&pool->lock);
        return NULL;
    }

    alloc_header_t* header = get_alloc_header(ptr);

    /* SIL-2: 验证 header 也在池内 */
    if (!ptr_in_pool(pool, header)) {
        k_mutex_unlock(&pool->lock);
        return NULL;
    }

    /* 验证魔数和池类型 */
    if (header->magic != MEMORY_MAGIC && header->magic != MEMORY_FREED_MAGIC) {
        k_mutex_unlock(&pool->lock);
        return NULL;
    }
    if (header->pool_type >= SYS_MEM_POOL_COUNT || header->pool_type != (uint32_t) pool->type) {
        k_mutex_unlock(&pool->lock);
        return NULL;
    }

    if (header_out != NULL) {
        *header_out = header;
    }
    return pool; /* 锁已持有 */
}

/**
 * @brief 添加分配记录到跟踪器（环形缓冲区）
 */
static void tracker_add(void* ptr, size_t size, const char* module, uint32_t line) {
    if (!g_sys_mem.tracker.tracking_enabled || ptr == NULL) {
        return;
    }

    k_mutex_lock(&g_sys_mem.tracker.lock, K_FOREVER);

    const uint32_t max_r = g_sys_mem.tracker.max_records;
    uint32_t       idx;
    bool           had_active = false;

    for (idx = 0; idx < max_r; idx++) {
        if (!g_sys_mem.tracker.slot_active[idx]) {
            break;
        }
    }

    if (idx >= max_r) {
        idx = g_sys_mem.tracker.head % max_r;
        had_active = g_sys_mem.tracker.slot_active[idx];
        if (had_active) {
            LOG_WRN("Tracker ring full: dropping trace for %p (replaced slot %u)", ptr, idx);
        }
        g_sys_mem.tracker.head = (idx + 1U) % max_r;
    }

    sys_mem_alloc_info_t* info = &g_sys_mem.tracker.records[idx];

    info->ptr = ptr;
    info->size = size;
    info->timestamp = k_uptime_get_32();
    info->module[0] = '\0';
    if (module != NULL) {
        strncpy(info->module, module, SYS_MEM_MODULE_NAME_LEN - 1);
        info->module[SYS_MEM_MODULE_NAME_LEN - 1] = '\0';
    }
    info->line = line;

    g_sys_mem.tracker.slot_active[idx] = true;
    if (!had_active) {
        g_sys_mem.tracker.count++;
    }

    k_mutex_unlock(&g_sys_mem.tracker.lock);
}

/**
 * @brief 从跟踪器移除分配记录（全表扫描有效槽，不做尾交换紧凑）
 */
static void tracker_remove(void* ptr) {
    if (!g_sys_mem.tracker.tracking_enabled || ptr == NULL) {
        return;
    }

    k_mutex_lock(&g_sys_mem.tracker.lock, K_FOREVER);

    const uint32_t max_r = g_sys_mem.tracker.max_records;

    for (uint32_t i = 0; i < max_r; i++) {
        if (g_sys_mem.tracker.slot_active[i] && g_sys_mem.tracker.records[i].ptr == ptr) {
            g_sys_mem.tracker.slot_active[i] = false;
            memset(&g_sys_mem.tracker.records[i], 0, sizeof(g_sys_mem.tracker.records[i]));
            if (g_sys_mem.tracker.count > 0U) {
                g_sys_mem.tracker.count--;
            }
            break;
        }
    }

    k_mutex_unlock(&g_sys_mem.tracker.lock);
}

/**
 * @brief 合并相邻空闲块
 *
 * 给定空闲链表头，合并地址相邻的块。
 * 假设链表按地址排序。
 */
static void coalesce_free_blocks(mem_pool_t* pool) {
    /* 由于空闲链表是按地址排序的，合并只需遍历一次 */
    if (pool->free_list == NULL) {
        return;
    }

    /* 将链表排序（插入排序，因链表通常很短） */
    free_block_t* sorted = NULL;
    free_block_t* curr = pool->free_list;
    while (curr != NULL) {
        free_block_t* next = curr->next;
        /* 插入到 sorted 链表中（按地址升序） */
        if (sorted == NULL || (uintptr_t) curr < (uintptr_t) sorted) {
            curr->next = sorted;
            sorted = curr;
        } else {
            free_block_t* prev = sorted;
            while (prev->next != NULL && (uintptr_t) prev->next < (uintptr_t) curr) {
                prev = prev->next;
            }
            curr->next = prev->next;
            prev->next = curr;
        }
        curr = next;
    }
    pool->free_list = sorted;

    /* 合并相邻块 */
    curr = pool->free_list;
    while (curr != NULL && curr->next != NULL) {
        uintptr_t curr_end = (uintptr_t) curr + curr->size;
        uintptr_t next_start = (uintptr_t) curr->next;
        if (curr_end == next_start) {
            /* 合并 */
            curr->size += curr->next->size;
            curr->next = curr->next->next;
            /* 继续检查新的下一个 */
        } else {
            curr = curr->next;
        }
    }
}

/**
 * @brief 从空闲链表中分配内存
 *
 * 返回分配的用户指针（位于分配头部之后）。
 * 调用时需持有池锁。
 */
static void* pool_alloc_locked(mem_pool_t* pool, size_t req_size, bool zero, const char* module, uint32_t line) {
    if (!pool->initialized) {
        return NULL;
    }

    /* 对齐用户请求大小 */
    size_t aligned_req;
    if (!align_up_size(req_size, MEMORY_ALIGN_BYTES, &aligned_req)) {
        pool->fail_count++;
        return NULL;
    }

    size_t total_needed = sizeof(alloc_header_t) + aligned_req;
    /* 保证总大小对齐到 MEMORY_ALIGN_BYTES（头部本身已对齐） */
    if (!align_up_size(total_needed, MEMORY_ALIGN_BYTES, &total_needed)) {
        pool->fail_count++;
        return NULL;
    }

    /* SIL-2: 验证分配不会超出池边界 */
    if (total_needed > pool->total_size) {
        LOG_ERR("Requested size %zu exceeds pool size %zu", total_needed, pool->total_size);
        pool->fail_count++;
        return NULL;
    }

    /* 查找第一个足够大的空闲块 */
    free_block_t* prev = NULL;
    free_block_t* curr = pool->free_list;
    while (curr != NULL) {
        if (curr->size >= total_needed) {
            break;
        }
        prev = curr;
        curr = curr->next;
    }

    if (curr == NULL) {
        pool->fail_count++;
        return NULL;
    }

    /* 分配内存块 */
    uint8_t* block_start = (uint8_t*) curr;
    size_t   remaining = curr->size - total_needed;

    if (remaining >= MIN_BLOCK_SIZE) {
        /* 分割：分配尾部（或头部），我们选择从当前块尾部切出，保持剩余块在前 */
        /* 将当前空闲块缩小为剩余部分，新分配块放在后面 */
        curr->size = remaining;
        block_start = (uint8_t*) curr + remaining; /* 新分配块的起始地址 */
    } else {
        /* 整个块用于分配，从链表中移除 */
        if (prev == NULL) {
            pool->free_list = curr->next;
        } else {
            prev->next = curr->next;
        }
        /* 此时 block_start 仍是原块起始地址 */
    }

    /* SIL-2: 最终边界检查 - 确保分配块不超出池 */
    if ((block_start + total_needed) > (pool->buffer + pool->total_size)) {
        LOG_ERR("Allocation would exceed pool boundary: %p + %zu > %p", block_start, total_needed,
                pool->buffer + pool->total_size);
        pool->fail_count++;
        return NULL;
    }

    /* 初始化分配头部 */
    alloc_header_t* header = (alloc_header_t*) block_start;
    header->magic = MEMORY_MAGIC;
    header->pool_type = (uint32_t) pool->type;
    header->requested_size = req_size;
    header->aligned_size = aligned_req;
    header->actual_block_size = (remaining >= MIN_BLOCK_SIZE) ? total_needed : curr->size;

    void* user_ptr = block_start + sizeof(alloc_header_t);

    /* 清零用户数据（如果需要） */
    if (zero) {
        memset(user_ptr, 0, aligned_req);
    }

    /* 更新统计 */
    pool->used_size += header->actual_block_size;
    if (pool->used_size > pool->max_used) {
        pool->max_used = pool->used_size;
    }
    pool->alloc_count++;
    pool->active_count++;

    return user_ptr;
}

/**
 * @brief 释放内存（内部，调用时需持有池锁）
 */
static void pool_free_locked(mem_pool_t* pool, void* ptr, alloc_header_t* header) {
    if (header->magic != MEMORY_MAGIC) {
        LOG_WRN("Double free or corrupted header: ptr=%p", ptr);
        return;
    }

    /* 标记为已释放 */
    header->magic = MEMORY_FREED_MAGIC;

    /* 将这块内存加入空闲链表 */
    free_block_t* new_free = (free_block_t*) header;
    size_t        freed_size = header->actual_block_size;
    new_free->size = freed_size;
    /* 确保大小对齐 */
    if (!align_up_size(new_free->size, MEMORY_ALIGN_BYTES, &new_free->size)) {
        LOG_ERR("align_up_size failed for freed block");
        new_free->size = freed_size;
    }

    /* 插入到空闲链表（保持地址顺序，便于合并） */
    free_block_t* prev = NULL;
    free_block_t* curr = pool->free_list;
    while (curr != NULL && (uintptr_t) curr < (uintptr_t) new_free) {
        prev = curr;
        curr = curr->next;
    }
    if (prev == NULL) {
        new_free->next = pool->free_list;
        pool->free_list = new_free;
    } else {
        new_free->next = prev->next;
        prev->next = new_free;
    }

    /* 合并相邻空闲块 */
    coalesce_free_blocks(pool);

    /* 更新统计：使用合并前保存的本次释放大小 */
    if (pool->used_size >= freed_size) {
        pool->used_size -= freed_size;
    } else {
        pool->used_size = 0;
    }
    pool->free_count++;
    pool->active_count--;

    /* 在池锁保护下移除跟踪记录，防止地址复用后误删新记录 */
    tracker_remove(ptr);
}

/**
 * @brief 获取分配的实际大小（调用者需持有池锁）
 */
static size_t get_allocation_size_locked(const alloc_header_t* header) {
    if (header == NULL || header->magic != MEMORY_MAGIC) {
        return 0;
    }
    return header->requested_size;
}

/* =============================================================================
 * 核心 API 实现
 * ============================================================================= */

int sys_mem_init(const sys_mem_config_t* config) {
    LOG_DBG("Initializing memory system...");

    /* 静默化检查：有活跃分配或并发操作进行中时拒绝重新初始化。
     * memset 会摧毁池锁/跟踪器锁与分配状态；若其他线程正持锁分配/释放，
     * 将导致锁内存损坏与不可恢复的堆破坏。逐池短暂取锁确认无人持锁后再重置
     * （BSS 零初始化的 k_mutex 在 Zephyr 中可直接加锁）。
     * 持有的锁在下方 memset 后由统一的 k_mutex_init 重建，无需释放。 */
    {
        mem_pool_t* held[SYS_MEM_POOL_COUNT];
        int         held_count = 0;

        for (int i = 0; i < SYS_MEM_POOL_COUNT; i++) {
            mem_pool_t* pool = &g_sys_mem.pools[i];

            if (!pool->initialized) {
                continue;
            }
            if (pool->active_count != 0U) {
                for (int j = 0; j < held_count; j++) {
                    k_mutex_unlock(&held[j]->lock);
                }
                LOG_ERR("sys_mem_init rejected: pool %d has %u active allocation(s)", i,
                        (unsigned int) pool->active_count);
                return -EBUSY;
            }
            if (k_mutex_lock(&pool->lock, K_MSEC(1)) != 0) {
                for (int j = 0; j < held_count; j++) {
                    k_mutex_unlock(&held[j]->lock);
                }
                LOG_ERR("sys_mem_init rejected: concurrent memory operation in progress");
                return -EBUSY;
            }
            held[held_count++] = pool;
        }

        if (g_sys_mem.tracker.tracking_enabled && k_mutex_lock(&g_sys_mem.tracker.lock, K_MSEC(1)) != 0) {
            for (int j = 0; j < held_count; j++) {
                k_mutex_unlock(&held[j]->lock);
            }
            LOG_ERR("sys_mem_init rejected: tracker busy");
            return -EBUSY;
        }
    }

    /* 清零除互斥锁外的全局控制块 */
    memset(&g_sys_mem, 0, sizeof(g_sys_mem));

    /* 设置配置（使用传入或默认值） */
    if (config != NULL) {
        g_sys_mem.config = *config;
    } else {
        /* 默认配置 */
        g_sys_mem.config.pool_sizes[SYS_MEM_POOL_GENERAL] = DEFAULT_POOL_SIZE;
        g_sys_mem.config.pool_sizes[SYS_MEM_POOL_EVENT] = DEFAULT_POOL_SIZE / 2;
        g_sys_mem.config.pool_sizes[SYS_MEM_POOL_MODULE] = DEFAULT_POOL_SIZE / 2;
        g_sys_mem.config.pool_sizes[SYS_MEM_POOL_DMA] = 0;
        g_sys_mem.config.enable_tracking = true;
        g_sys_mem.config.enable_defrag = false;
        g_sys_mem.config.max_allocations = MAX_ALLOCATIONS;
    }

    /* 验证配置参数 */
    if (g_sys_mem.config.max_allocations == 0 || g_sys_mem.config.max_allocations > MAX_ALLOCATIONS) {
        LOG_ERR("Invalid max_allocations: %u", (uint32_t) g_sys_mem.config.max_allocations);
        return -EINVAL;
    }

    /* 初始化所有内存池 */
    for (int i = 0; i < SYS_MEM_POOL_COUNT; i++) {
        mem_pool_t* pool = &g_sys_mem.pools[i];
        size_t      configured_size = g_sys_mem.config.pool_sizes[i];

        if (configured_size > DEFAULT_POOL_SIZE) {
            LOG_WRN("Pool %d size %u exceeds buffer %u, clamped", i, (uint32_t) configured_size,
                    (uint32_t) DEFAULT_POOL_SIZE);
            configured_size = DEFAULT_POOL_SIZE;
            g_sys_mem.config.pool_sizes[i] = DEFAULT_POOL_SIZE;
        }

        /* 初始化互斥锁 */
        k_mutex_init(&pool->lock);

        if (configured_size == 0) {
            pool->initialized = false;
            continue;
        }

        if (configured_size < sizeof(free_block_t)) {
            LOG_WRN("Pool %d size %u too small (min %zu), disabled", i, (uint32_t) configured_size,
                    sizeof(free_block_t));
            pool->initialized = false;
            continue;
        }

        pool->buffer = g_mem_buffer[i];
        pool->total_size = configured_size;
        pool->used_size = 0;
        pool->max_used = 0;
        pool->alloc_count = 0;
        pool->free_count = 0;
        pool->fail_count = 0;
        pool->type = (sys_mem_pool_type_t) i;
        pool->initialized = true;

        /* 初始化空闲链表：整个池为一个空闲块 */
        pool->free_list = (free_block_t*) pool->buffer;
        pool->free_list->next = NULL;
        pool->free_list->size = pool->total_size;

        LOG_DBG("Pool %d initialized: %u bytes", i, (uint32_t) pool->total_size);
    }

    /* 初始化跟踪器 */
    g_sys_mem.tracker.head = 0;
    g_sys_mem.tracker.count = 0;
    g_sys_mem.tracker.max_records = g_sys_mem.config.max_allocations;
    g_sys_mem.tracker.tracking_enabled = g_sys_mem.config.enable_tracking;
    k_mutex_init(&g_sys_mem.tracker.lock);

    LOG_DBG("Memory system initialized");
    return 0;
}

void* sys_mem_alloc(sys_mem_pool_type_t type, size_t size) {
    if (size == 0) {
        return NULL;
    }

    mem_pool_t* pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        pool = get_pool(SYS_MEM_POOL_GENERAL);
    }
    if (pool == NULL || !pool->initialized) {
        return NULL;
    }

    k_mutex_lock(&pool->lock, K_FOREVER);
    void* ptr = pool_alloc_locked(pool, size, false, NULL, 0);
    if (ptr != NULL) {
        /* 锁内登记跟踪记录（锁序 pool->lock → tracker.lock，与 pool_free_locked 一致）：
         * 若解锁后再登记，另一线程可能已释放同一指针，tracker_remove 先于 add 执行，
         * 留下指向已释放内存的陈旧泄漏记录。 */
        tracker_add(ptr, size, NULL, 0);
    }
    k_mutex_unlock(&pool->lock);
    return ptr;
}

void* sys_mem_calloc(sys_mem_pool_type_t type, size_t size) {
    if (size == 0) {
        return NULL;
    }

    mem_pool_t* pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        pool = get_pool(SYS_MEM_POOL_GENERAL);
    }
    if (pool == NULL || !pool->initialized) {
        return NULL;
    }

    k_mutex_lock(&pool->lock, K_FOREVER);
    void* ptr = pool_alloc_locked(pool, size, true, NULL, 0);
    if (ptr != NULL) {
        tracker_add(ptr, size, NULL, 0);
    }
    k_mutex_unlock(&pool->lock);
    return ptr;
}

void sys_mem_free(sys_mem_pool_type_t type, void* ptr) {
    if (ptr == NULL) {
        return;
    }

    alloc_header_t* header;
    mem_pool_t*     pool = lock_pool_from_ptr(ptr, &header);
    if (pool == NULL) {
        LOG_WRN("Invalid free: ptr=%p", ptr);
        return;
    }

    /* 验证期望类型（可选） */
    if (type != pool->type) {
        LOG_WRN("Free pool mismatch: expected=%d actual=%d ptr=%p", type, pool->type, ptr);
    }

    pool_free_locked(pool, ptr, header);
    k_mutex_unlock(&pool->lock);
}

void* sys_mem_realloc(sys_mem_pool_type_t type, void* ptr, size_t size) {
    /* SIL-2: ptr为NULL时等同于alloc */
    if (ptr == NULL) {
        return sys_mem_alloc(type, size);
    }

    /* SIL-2: size为0时等同于free */
    if (size == 0) {
        sys_mem_free(type, ptr);
        return NULL;
    }

    /* 获取原分配大小 */
    alloc_header_t* header;
    mem_pool_t*     pool = lock_pool_from_ptr(ptr, &header);
    if (pool == NULL) {
        LOG_WRN("realloc on invalid pointer: %p", ptr);
        return NULL;
    }
    size_t old_size = get_allocation_size_locked(header);
    k_mutex_unlock(&pool->lock);

    if (old_size == 0) {
        return NULL;
    }

    /* SIL-2: 先分配新内存，失败时保持原指针不变 */
    void* new_ptr = sys_mem_alloc(type, size);
    if (new_ptr == NULL) {
        LOG_WRN("realloc failed: cannot allocate %zu bytes", size);
        return NULL; /* 失败时原指针保持不变 */
    }

    /* SIL-2: 复制数据 (取新旧大小的较小值) */
    size_t copy_size = (old_size < size) ? old_size : size;
    memcpy(new_ptr, ptr, copy_size);

    /* SIL-2: 释放原内存 */
    sys_mem_free(type, ptr);

    return new_ptr;
}

/* =============================================================================
 * 统计与调试 API 实现
 * ============================================================================= */

void sys_mem_get_stats(sys_mem_pool_type_t type, sys_mem_stats_t* stats) {
    if (stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(sys_mem_stats_t));

    mem_pool_t* pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        return;
    }

    k_mutex_lock(&pool->lock, K_FOREVER);

    stats->total_size = pool->total_size;
    stats->used_size = pool->used_size;
    stats->free_size = pool->total_size - pool->used_size;
    stats->max_used = pool->max_used;
    stats->alloc_count = pool->alloc_count;
    stats->free_count = pool->free_count;
    stats->fail_count = pool->fail_count;

    /* 计算外部碎片率：1 - (最大连续空闲块 / 总空闲大小) */
    if (stats->free_size > 0) {
        size_t        max_free_block = 0;
        free_block_t* curr = pool->free_list;
        while (curr != NULL) {
            if (curr->size > max_free_block) {
                max_free_block = curr->size;
            }
            curr = curr->next;
        }
        stats->fragmentation = (uint32_t) ((1.0f - (float) max_free_block / stats->free_size) * 100);
    } else {
        stats->fragmentation = 0;
    }

    k_mutex_unlock(&pool->lock);
}

void sys_mem_reset_stats(sys_mem_pool_type_t type) {
    mem_pool_t* pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        return;
    }

    k_mutex_lock(&pool->lock, K_FOREVER);
    pool->max_used = pool->used_size;
    pool->alloc_count = 0;
    pool->free_count = 0;
    pool->fail_count = 0;
    /* active_count 不清零：它反映真实的活跃分配，与累计统计分离 */
    k_mutex_unlock(&pool->lock);
}

uint32_t sys_mem_get_active_allocations(sys_mem_pool_type_t type) {
    mem_pool_t* pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        return 0;
    }

    k_mutex_lock(&pool->lock, K_FOREVER);
    uint32_t active = pool->active_count;
    k_mutex_unlock(&pool->lock);
    return active;
}

void sys_mem_dump_allocations(sys_mem_pool_type_t type) {
    mem_pool_t* pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        return;
    }

    /* 获取统计快照 */
    sys_mem_stats_t stats;
    sys_mem_get_stats(type, &stats);

    printk("\n=== Memory Pool %d Dump ===\n", type);
    printk("Total: %u, Used: %u, Free: %u, MaxUsed: %u\n", (uint32_t) stats.total_size, (uint32_t) stats.used_size,
           (uint32_t) stats.free_size, (uint32_t) stats.max_used);
    printk("Allocations: %u, Frees: %u, Fails: %u\n", stats.alloc_count, stats.free_count, stats.fail_count);

    /* 打印空闲块信息 */
    k_mutex_lock(&pool->lock, K_FOREVER);
    printk("Free blocks:\n");
    free_block_t* fb = pool->free_list;
    int           idx = 0;
    while (fb != NULL) {
        printk("  [%d] addr=%p size=%u\n", idx++, fb, (uint32_t) fb->size);
        fb = fb->next;
    }
    k_mutex_unlock(&pool->lock);

    /* 打印活跃分配（来自跟踪器）
     *
     * 锁序约定：全局统一为 pool->lock 先于 tracker.lock（见 pool_free_locked
     * 在持有池锁时调用 tracker_remove）。因此此处禁止在持有 tracker.lock 的
     * 同时再去获取 pool->lock，否则会与释放路径形成 AB-BA 死锁。
     * 解决方式：逐槽在 tracker.lock 下做快照后立即释放，再用池锁验证打印。 */
    if (g_sys_mem.tracker.tracking_enabled) {
        uint32_t max_r;
        bool     header_printed = false;

        k_mutex_lock(&g_sys_mem.tracker.lock, K_FOREVER);
        max_r = g_sys_mem.tracker.max_records;
        k_mutex_unlock(&g_sys_mem.tracker.lock);

        for (uint32_t i = 0; i < max_r; i++) {
            sys_mem_alloc_info_t info_copy;
            bool                 active = false;

            k_mutex_lock(&g_sys_mem.tracker.lock, K_FOREVER);
            if (g_sys_mem.tracker.slot_active[i]) {
                info_copy = g_sys_mem.tracker.records[i];
                active = true;
            }
            k_mutex_unlock(&g_sys_mem.tracker.lock);

            if (!active) {
                continue;
            }

            mem_pool_t* owner = lock_pool_from_ptr(info_copy.ptr, NULL);
            if (owner == NULL) {
                continue;
            }
            alloc_header_t* header = get_alloc_header(info_copy.ptr);
            if (header != NULL && header->magic == MEMORY_MAGIC && header->pool_type == (uint32_t) type) {
                if (!header_printed) {
                    printk("Active Allocations:\n");
                    header_printed = true;
                }
                printk("  [%u] ptr=%p, size=%u, time=%u", i, info_copy.ptr, (uint32_t) info_copy.size,
                       info_copy.timestamp);
                if (info_copy.module[0] != '\0') {
                    printk(" (%s:%u)", info_copy.module, info_copy.line);
                }
                printk("\n");
            }
            k_mutex_unlock(&owner->lock);
        }
    }

    printk("=== End Dump ===\n\n");
}

uint32_t sys_mem_check_leaks(sys_mem_pool_type_t type) {
    if (!g_sys_mem.tracker.tracking_enabled) {
        return 0;
    }

    uint32_t leaks = 0;

    k_mutex_lock(&g_sys_mem.tracker.lock, K_FOREVER);

    const uint32_t max_r = g_sys_mem.tracker.max_records;

    if (type >= SYS_MEM_POOL_COUNT) {
        for (uint32_t i = 0; i < max_r; i++) {
            if (!g_sys_mem.tracker.slot_active[i]) {
                continue;
            }
            alloc_header_t* header = get_alloc_header(g_sys_mem.tracker.records[i].ptr);
            if (header != NULL && header->magic == MEMORY_MAGIC) {
                leaks++;
            }
        }
    } else {
        for (uint32_t i = 0; i < max_r; i++) {
            if (!g_sys_mem.tracker.slot_active[i]) {
                continue;
            }
            alloc_header_t* header = get_alloc_header(g_sys_mem.tracker.records[i].ptr);
            if (header != NULL && header->magic == MEMORY_MAGIC && header->pool_type == (uint32_t) type) {
                leaks++;
            }
        }
    }

    k_mutex_unlock(&g_sys_mem.tracker.lock);
    return leaks;
}

size_t sys_mem_defrag(sys_mem_pool_type_t type) {
    mem_pool_t* pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        return 0;
    }

    if (!g_sys_mem.config.enable_defrag) {
        return 0;
    }

    k_mutex_lock(&pool->lock, K_FOREVER);

    size_t reclaimed = 0;

    /* SIL-2: 使用活跃分配计数而非总计数,防止误判
     * 只有当前没有活跃分配时才能安全重置整个池
     */
    if (pool->active_count == 0) {
        reclaimed = pool->used_size;
        pool->free_list = (free_block_t*) pool->buffer;
        pool->free_list->next = NULL;
        pool->free_list->size = pool->total_size;
        pool->used_size = 0;
        pool->max_used = pool->used_size; /* 重置峰值 */
    } else {
        /* SIL-2: 仅合并相邻空闲块,不影响活跃分配 */
        coalesce_free_blocks(pool);
        /* 计算合并后回收的空间 */
        reclaimed = 0; /* 碎片整理不释放内存,只优化布局 */
    }

    k_mutex_unlock(&pool->lock);
    return reclaimed;
}

/* =============================================================================
 * 堆信息 API 实现
 * ============================================================================= */

size_t sys_mem_get_heap_size(void) {
    size_t total = 0;
    for (int i = 0; i < SYS_MEM_POOL_COUNT; i++) {
        mem_pool_t* pool = &g_sys_mem.pools[i];
        if (!pool->initialized) {
            continue;
        }
        k_mutex_lock(&pool->lock, K_FOREVER);
        total += pool->total_size;
        k_mutex_unlock(&pool->lock);
    }
    return total;
}

size_t sys_mem_get_free_size(void) {
    size_t free_size = 0;
    for (int i = 0; i < SYS_MEM_POOL_COUNT; i++) {
        mem_pool_t* pool = &g_sys_mem.pools[i];
        if (!pool->initialized) {
            continue;
        }
        k_mutex_lock(&pool->lock, K_FOREVER);
        free_size += pool->total_size - pool->used_size;
        k_mutex_unlock(&pool->lock);
    }
    return free_size;
}

size_t sys_mem_get_min_free_size(void) {
    size_t min_free = SIZE_MAX;
    for (int i = 0; i < SYS_MEM_POOL_COUNT; i++) {
        mem_pool_t* pool = &g_sys_mem.pools[i];
        if (!pool->initialized) {
            continue;
        }
        k_mutex_lock(&pool->lock, K_FOREVER);
        size_t free = pool->total_size - pool->max_used;
        k_mutex_unlock(&pool->lock);
        if (free < min_free) {
            min_free = free;
        }
    }
    return (min_free == SIZE_MAX) ? 0 : min_free;
}

void* sys_mem_alloc_with_info(sys_mem_pool_type_t type, size_t size, const char* module, uint32_t line) {
    if (size == 0)
        return NULL;

    mem_pool_t* pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        pool = get_pool(SYS_MEM_POOL_GENERAL);
        if (pool == NULL || !pool->initialized)
            return NULL;
    }

    k_mutex_lock(&pool->lock, K_FOREVER);
    void* ptr = pool_alloc_locked(pool, size, false, module, line);
    if (ptr != NULL) {
        tracker_add(ptr, size, module, line);
    }
    k_mutex_unlock(&pool->lock);
    return ptr;
}

/* =============================================================================
 * SYS_INIT 自动初始化
 * ============================================================================= */

#include <zeplod/app_config.h>

static int sys_mem_auto_init(void) {
#if APP_CONFIG_ENABLE_MEMORY_MGR
    sys_mem_config_t mem_config = {.pool_sizes =
                                       {
                                           [SYS_MEM_POOL_GENERAL] = CONFIG_SYS_MEMORY_POOL_SIZE,
                                           [SYS_MEM_POOL_EVENT] = CONFIG_SYS_MEMORY_POOL_SIZE / 2,
                                           [SYS_MEM_POOL_MODULE] = CONFIG_SYS_MEMORY_POOL_SIZE / 2,
                                       },
                                   .enable_tracking = true,
                                   .enable_defrag = false,
                                   .max_allocations = 256};
    sys_mem_init(&mem_config);
#endif
    return 0;
}

SYS_INIT(sys_mem_auto_init, POST_KERNEL, APP_INIT_PRIO_SYS_MEM);
