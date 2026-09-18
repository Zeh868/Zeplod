/**
 * @file sys_memory.c
 * @brief 系统内存管理实现（空闲链表版）
 *
 * 支持多内存池，每个池基于空闲链表（first-fit）管理。
 * 提供分配、释放、重新分配、统计、跟踪等功能。
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            修复分配块尺寸别名覆写UB、freed标记
 *                                                失效、init竞态防护、锁序登记、魔数清理
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <limits.h>
#include <stdalign.h>
#include <string.h>
#include <zeplod/app_config.h>
#include <zeplod/lock_order.h>
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

/** 重新初始化时静默化探测的试锁超时（毫秒）：仅为探测"有无并发操作在途"，非真实等待 */
#define SYS_MEM_REINIT_PROBE_TIMEOUT_MS 1

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
    uint32_t freed_magic;       ///< 已释放标记（放尾部：块首部入空闲链表后会被 free_block_t 覆盖）
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
 * @brief 池锁加锁（锁序登记 + 互斥锁）
 *
 * 锁序约定：pool->lock 与 tracker.lock 同为 RESOURCE 层，二者 key（成员地址）
 * 恒单调（pools[] 在结构体前部、tracker 在后部），嵌套只允许 pool → tracker
 * 方向（见 pool_free_locked），禁止 tracker → pool 反向嵌套。
 */
static void pool_lock(mem_pool_t* pool) {
    zepl_lock_enter(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &pool->lock);
    k_mutex_lock(&pool->lock, K_FOREVER);
}

/**
 * @brief 池锁解锁（互斥锁 + 锁序登记出栈）
 */
static void pool_unlock(mem_pool_t* pool) {
    k_mutex_unlock(&pool->lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &pool->lock);
}

/**
 * @brief 跟踪器锁加锁（锁序登记 + 互斥锁）
 * @note 若已持有任一池锁，本函数构成 pool → tracker 同层嵌套（key 恒单调，合法）
 */
static void tracker_lock(void) {
    zepl_lock_enter(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_sys_mem.tracker.lock);
    k_mutex_lock(&g_sys_mem.tracker.lock, K_FOREVER);
}

/**
 * @brief 跟踪器锁解锁（互斥锁 + 锁序登记出栈）
 */
static void tracker_unlock(void) {
    k_mutex_unlock(&g_sys_mem.tracker.lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_sys_mem.tracker.lock);
}

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

    pool_lock(pool);

    /* SIL-2: 再次验证指针是否在池内（锁内确保一致性） */
    if (!ptr_in_pool(pool, ptr)) {
        pool_unlock(pool);
        return NULL;
    }

    /* SIL-2: 确保指针距离池起始位置至少有头部大小
     * 防止 ptr 指向池起始位置,导致 header 计算到池外
     */
    uintptr_t ptr_offset = (uintptr_t) ptr - (uintptr_t) pool->buffer;
    if (ptr_offset < sizeof(alloc_header_t)) {
        LOG_ERR("Pointer too close to pool start: offset=%zu < header=%zu", (size_t) ptr_offset,
                sizeof(alloc_header_t));
        pool_unlock(pool);
        return NULL;
    }

    alloc_header_t* header = get_alloc_header(ptr);

    /* SIL-2: 验证 header 也在池内 */
    if (!ptr_in_pool(pool, header)) {
        pool_unlock(pool);
        return NULL;
    }

    /* 验证魔数：只接受 MEMORY_MAGIC。已释放块的 freed 标记存于 header 尾部
     * freed_magic 字段（首部 magic 在入空闲链表后会被链表指针覆盖，无法承载标记），
     * double-free 由 pool_free_locked 依据 freed_magic 识别并告警 */
    if (header->magic != MEMORY_MAGIC) {
        pool_unlock(pool);
        return NULL;
    }
    if (header->pool_type >= SYS_MEM_POOL_COUNT || header->pool_type != (uint32_t) pool->type) {
        pool_unlock(pool);
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

    tracker_lock();

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

    tracker_unlock();
}

/**
 * @brief 从跟踪器移除分配记录（全表扫描有效槽，不做尾交换紧凑）
 */
static void tracker_remove(void* ptr) {
    if (!g_sys_mem.tracker.tracking_enabled || ptr == NULL) {
        return;
    }

    tracker_lock();

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

    tracker_unlock();
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
 * @brief 从空闲链表取出可容纳 total_needed 的块（first-fit），必要时分割
 *
 * 调用时需持有池锁。
 *
 * @param pool 目标内存池
 * @param total_needed 含头部的总需求大小（字节）
 * @param actual_block_size_out 输出：本次分配实际占用的总块大小（分割分支为 total_needed，
 *                              整块分支为原空闲块尺寸）
 * @return 分配块（头部）起始地址；无合适块时返回 NULL
 *
 * @note header 写入与 free_block_t 内存重叠（整块分支 header 直接覆盖原 free_block_t），
 *       必须在写 header 前保存原块尺寸并通过 actual_block_size_out 带出，否则读取到的是
 *       被 header 字段覆写后的垃圾值（别名覆写 UB）
 */
static uint8_t* pool_take_block_locked(mem_pool_t* pool, size_t total_needed, size_t* actual_block_size_out) {
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
        return NULL;
    }

    uint8_t* block_start = (uint8_t*) curr;
    size_t   remaining = curr->size - total_needed;

    if (remaining >= MIN_BLOCK_SIZE) {
        /* 分割：从当前块尾部切出分配块，剩余部分保留在前（空闲链表按地址有序便于合并） */
        curr->size = remaining;
        block_start = (uint8_t*) curr + remaining;
        *actual_block_size_out = total_needed;
    } else {
        /* 整个块用于分配，从链表中移除；此后 header 写入将覆盖原 free_block_t 内存 */
        if (prev == NULL) {
            pool->free_list = curr->next;
        } else {
            prev->next = curr->next;
        }
        *actual_block_size_out = curr->size; /* 必须在 header 写入前保存（别名覆写） */
    }

    return block_start;
}

/**
 * @brief 初始化分配头部
 *
 * 调用时需持有池锁。
 */
static void pool_alloc_header_init(mem_pool_t* pool, uint8_t* block_start, size_t req_size, size_t aligned_req,
                                   size_t actual_block_size) {
    alloc_header_t* header = (alloc_header_t*) block_start;

    header->magic = MEMORY_MAGIC;
    header->pool_type = (uint32_t) pool->type;
    header->requested_size = req_size;
    header->aligned_size = aligned_req;
    header->actual_block_size = actual_block_size;
    header->freed_magic = 0U; /* 清除上一轮生命周期的已释放标记 */
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

    /* 查找并摘取空闲块（actual_block_size 在 header 写入前保存带出，规避别名覆写） */
    size_t   actual_block_size = 0U;
    uint8_t* block_start = pool_take_block_locked(pool, total_needed, &actual_block_size);
    if (block_start == NULL) {
        pool->fail_count++;
        return NULL;
    }

    /* SIL-2: 最终边界检查 - 确保分配块不超出池 */
    if ((block_start + total_needed) > (pool->buffer + pool->total_size)) {
        LOG_ERR("Allocation would exceed pool boundary: %p + %zu > %p", block_start, total_needed,
                pool->buffer + pool->total_size);
        pool->fail_count++;
        return NULL;
    }

    pool_alloc_header_init(pool, block_start, req_size, aligned_req, actual_block_size);

    void* user_ptr = block_start + sizeof(alloc_header_t);

    /* 清零用户数据（如果需要） */
    if (zero) {
        memset(user_ptr, 0, aligned_req);
    }

    /* 更新统计 */
    pool->used_size += actual_block_size;
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
    /* freed 标记存于 header 尾部 freed_magic 字段：块首部 magic 在入空闲链表后会被
     * free_block_t.next 覆盖（原实现把标记写在首部，写完即被链表指针冲掉，
     * double-free 检测形同虚设），故双 free 检测依据尾部标记 + 首部魔数双重判断 */
    if (header->magic != MEMORY_MAGIC || header->freed_magic == MEMORY_FREED_MAGIC) {
        LOG_WRN("Double free or corrupted header: ptr=%p", ptr);
        return;
    }

    /* 标记为已释放（尾部字段，不被链表指针覆盖；首部 magic 保持有效魔数） */
    header->freed_magic = MEMORY_FREED_MAGIC;

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

/**
 * @brief 校验初始化配置（在任何全局状态写入之前调用）
 *
 * @param config 入参配置；NULL 表示使用默认值（默认值必然合法）
 * @return 0 合法；-EINVAL max_allocations 非法
 */
static int mem_config_validate(const sys_mem_config_t* config) {
    uint32_t max_alloc = (config != NULL) ? config->max_allocations : (uint32_t) MAX_ALLOCATIONS;

    if (max_alloc == 0U || max_alloc > (uint32_t) MAX_ALLOCATIONS) {
        LOG_ERR("Invalid max_allocations: %u", max_alloc);
        return -EINVAL;
    }
    return 0;
}

/**
 * @brief 重新初始化前的静默化探测（in-use 检查）
 *
 * memset 会摧毁池锁/跟踪器锁与分配状态；若其他线程正持锁分配/释放，将导致锁
 * 内存损坏与不可恢复的堆破坏。故逐池检查活跃分配数并短暂试锁确认无人持锁
 * （BSS 零初始化的 k_mutex 在 Zephyr 中可直接加锁）。
 *
 * 成功返回时所有已初始化池锁与跟踪器锁均被本线程持有（不放行其他线程），
 * 这些锁随后被 memset 摧毁并由 k_mutex_init 重建（替代真实 unlock）。
 *
 * @return 0 探测通过（持有多把锁）；-EBUSY 有活跃分配或并发操作进行中
 */
static int mem_reinit_try_quiesce(void) {
    mem_pool_t* held[SYS_MEM_POOL_COUNT];
    int         held_count = 0;
    bool        tracker_held = false;
    int         ret = 0;

    for (int i = 0; i < SYS_MEM_POOL_COUNT; i++) {
        mem_pool_t* pool = &g_sys_mem.pools[i];

        if (!pool->initialized) {
            continue;
        }
        if (pool->active_count != 0U) {
            LOG_ERR("sys_mem_init rejected: pool %d has %u active allocation(s)", i, (unsigned int) pool->active_count);
            ret = -EBUSY;
            goto rollback;
        }
        if (k_mutex_lock(&pool->lock, K_MSEC(SYS_MEM_REINIT_PROBE_TIMEOUT_MS)) != 0) {
            LOG_ERR("sys_mem_init rejected: concurrent memory operation in progress");
            ret = -EBUSY;
            goto rollback;
        }
        zepl_lock_enter(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &pool->lock);
        held[held_count++] = pool;
    }

    if (g_sys_mem.tracker.tracking_enabled) {
        if (k_mutex_lock(&g_sys_mem.tracker.lock, K_MSEC(SYS_MEM_REINIT_PROBE_TIMEOUT_MS)) != 0) {
            LOG_ERR("sys_mem_init rejected: tracker busy");
            ret = -EBUSY;
            goto rollback;
        }
        zepl_lock_enter(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_sys_mem.tracker.lock);
        tracker_held = true;
    }
    return 0;

rollback:
    /* 失败路径对称回滚：真实解锁 + 登记出栈（逆序保持 LIFO） */
    if (tracker_held) {
        tracker_unlock();
    }
    while (held_count > 0) {
        mem_pool_t* pool = held[--held_count];
        pool_unlock(pool);
    }
    return ret;
}

/**
 * @brief 清除静默化探测建立的锁登记（不做真实 unlock）
 *
 * 探测持有的锁即将被 memset 摧毁并由 k_mutex_init 重建（重建等效于释放），
 * 此处仅按 enter 的逆序清除 zepl_lock 登记栈，保持 enter/exit 平衡。
 */
static void mem_reinit_clear_registry(void) {
    if (g_sys_mem.tracker.tracking_enabled) {
        zepl_lock_exit(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_sys_mem.tracker.lock);
    }
    for (int i = SYS_MEM_POOL_COUNT - 1; i >= 0; i--) {
        mem_pool_t* pool = &g_sys_mem.pools[i];
        if (pool->initialized) {
            zepl_lock_exit(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &pool->lock);
        }
    }
}

/**
 * @brief 应用配置（传入或默认值）；调用前已通过 mem_config_validate
 */
static void mem_config_apply(const sys_mem_config_t* config) {
    if (config != NULL) {
        g_sys_mem.config = *config;
        return;
    }

    /* 默认配置 */
    g_sys_mem.config.pool_sizes[SYS_MEM_POOL_GENERAL] = DEFAULT_POOL_SIZE;
    g_sys_mem.config.pool_sizes[SYS_MEM_POOL_EVENT] = DEFAULT_POOL_SIZE / 2;
    g_sys_mem.config.pool_sizes[SYS_MEM_POOL_MODULE] = DEFAULT_POOL_SIZE / 2;
    g_sys_mem.config.pool_sizes[SYS_MEM_POOL_DMA] = 0;
    g_sys_mem.config.enable_tracking = true;
    g_sys_mem.config.enable_defrag = false;
    g_sys_mem.config.max_allocations = MAX_ALLOCATIONS;
}

/**
 * @brief 初始化所有内存池（缓冲区、空闲链表、互斥锁）
 */
static void mem_pools_setup(void) {
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
}

/**
 * @brief 初始化分配跟踪器
 */
static void mem_tracker_setup(void) {
    g_sys_mem.tracker.head = 0;
    g_sys_mem.tracker.count = 0;
    g_sys_mem.tracker.max_records = g_sys_mem.config.max_allocations;
    g_sys_mem.tracker.tracking_enabled = g_sys_mem.config.enable_tracking;
    k_mutex_init(&g_sys_mem.tracker.lock);
}

int sys_mem_init(const sys_mem_config_t* config) {
    LOG_DBG("Initializing memory system...");

    /* 配置校验前置：失败必须发生在任何全局状态写入之前（首次 init 为 BSS 零值，
     * 重复 init 时保留原状态不动），避免半初始化 */
    int ret = mem_config_validate(config);
    if (ret != 0) {
        return ret;
    }

    ret = mem_reinit_try_quiesce();
    if (ret != 0) {
        return ret;
    }

    /* 探测持有的锁将被 memset 摧毁并重建（见 mem_reinit_try_quiesce 注释），先清登记 */
    mem_reinit_clear_registry();

    /* 清零除互斥锁外的全局控制块 */
    memset(&g_sys_mem, 0, sizeof(g_sys_mem));

    mem_config_apply(config);
    mem_pools_setup();
    mem_tracker_setup();

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

    pool_lock(pool);
    void* ptr = pool_alloc_locked(pool, size, false, NULL, 0);
    if (ptr != NULL) {
        /* 锁内登记跟踪记录（锁序 pool->lock → tracker.lock，与 pool_free_locked 一致）：
         * 若解锁后再登记，另一线程可能已释放同一指针，tracker_remove 先于 add 执行，
         * 留下指向已释放内存的陈旧泄漏记录。 */
        tracker_add(ptr, size, NULL, 0);
    }
    pool_unlock(pool);
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

    pool_lock(pool);
    void* ptr = pool_alloc_locked(pool, size, true, NULL, 0);
    if (ptr != NULL) {
        tracker_add(ptr, size, NULL, 0);
    }
    pool_unlock(pool);
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
    pool_unlock(pool);
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
    pool_unlock(pool);

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

    pool_lock(pool);

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

    pool_unlock(pool);
}

void sys_mem_reset_stats(sys_mem_pool_type_t type) {
    mem_pool_t* pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        return;
    }

    pool_lock(pool);
    pool->max_used = pool->used_size;
    pool->alloc_count = 0;
    pool->free_count = 0;
    pool->fail_count = 0;
    /* active_count 不清零：它反映真实的活跃分配，与累计统计分离 */
    pool_unlock(pool);
}

uint32_t sys_mem_get_active_allocations(sys_mem_pool_type_t type) {
    mem_pool_t* pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        return 0;
    }

    pool_lock(pool);
    uint32_t active = pool->active_count;
    pool_unlock(pool);
    return active;
}

/**
 * @brief 打印空闲块布局
 *
 * 调用时需持有池锁的操作在函数内部完成。
 */
static void dump_pool_free_blocks(mem_pool_t* pool) {
    pool_lock(pool);
    printk("Free blocks:\n");
    free_block_t* fb = pool->free_list;
    int           idx = 0;
    while (fb != NULL) {
        printk("  [%d] addr=%p size=%u\n", idx++, fb, (uint32_t) fb->size);
        fb = fb->next;
    }
    pool_unlock(pool);
}

/**
 * @brief 打印跟踪器中的活跃分配（含池归属验证）
 *
 * 锁序约定：全局统一为 pool->lock 先于 tracker.lock（见 pool_free_locked
 * 在持有池锁时调用 tracker_remove）。因此此处禁止在持有 tracker.lock 的
 * 同时再去获取 pool->lock，否则会与释放路径形成 AB-BA 死锁。
 * 解决方式：逐槽在 tracker.lock 下做快照后立即释放，再用池锁验证打印。
 */
static void dump_active_allocations(sys_mem_pool_type_t type) {
    if (!g_sys_mem.tracker.tracking_enabled) {
        return;
    }

    bool header_printed = false;

    tracker_lock();
    const uint32_t max_r = g_sys_mem.tracker.max_records;
    tracker_unlock();

    for (uint32_t i = 0; i < max_r; i++) {
        sys_mem_alloc_info_t info_copy;
        bool                 active = false;

        tracker_lock();
        if (g_sys_mem.tracker.slot_active[i]) {
            info_copy = g_sys_mem.tracker.records[i];
            active = true;
        }
        tracker_unlock();

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
        pool_unlock(owner);
    }
}

void sys_mem_dump_allocations(sys_mem_pool_type_t type) {
    mem_pool_t* pool = get_pool(type);
    if (pool == NULL || !pool->initialized) {
        return;
    }

    /* printk 直用依据：sys_memory.h 已声明 dump 输出到 printk/控制台而非日志系统——
     * 内存故障诊断需在日志子系统不可用（早期启动/堆破坏）时仍能工作 */

    /* 获取统计快照 */
    sys_mem_stats_t stats;
    sys_mem_get_stats(type, &stats);

    printk("\n=== Memory Pool %d Dump ===\n", type);
    printk("Total: %u, Used: %u, Free: %u, MaxUsed: %u\n", (uint32_t) stats.total_size, (uint32_t) stats.used_size,
           (uint32_t) stats.free_size, (uint32_t) stats.max_used);
    printk("Allocations: %u, Frees: %u, Fails: %u\n", stats.alloc_count, stats.free_count, stats.fail_count);

    dump_pool_free_blocks(pool);
    dump_active_allocations(type);

    printk("=== End Dump ===\n\n");
}

uint32_t sys_mem_check_leaks(sys_mem_pool_type_t type) {
    if (!g_sys_mem.tracker.tracking_enabled) {
        return 0;
    }

    uint32_t leaks = 0;

    /* 仅持 tracker 锁读 header 字段：与并发分配/释放存在瞬时竞态（分配中 magic 尚未
     * 写好、释放中标记刚更新），诊断值允许瞬时失真。不为此引入 tracker → pool 的
     * 反向嵌套锁——那会违反既定的 pool → tracker 锁序（见 pool_lock 注释）。 */
    tracker_lock();

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

    tracker_unlock();
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

    pool_lock(pool);

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

    pool_unlock(pool);
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
        pool_lock(pool);
        total += pool->total_size;
        pool_unlock(pool);
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
        pool_lock(pool);
        free_size += pool->total_size - pool->used_size;
        pool_unlock(pool);
    }
    return free_size;
}

size_t sys_mem_get_min_free_size(void) {
    /* 语义说明：返回各池"各自峰值使用时刻"剩余量（total_size - max_used）的最小值，
     * 而非同一时刻的全局合计——max_used 是各池独立的峰值，不同池的峰值可出现在
     * 不同时刻，多池同时紧张时本值偏保守。 */
    size_t min_free = SIZE_MAX;
    for (int i = 0; i < SYS_MEM_POOL_COUNT; i++) {
        mem_pool_t* pool = &g_sys_mem.pools[i];
        if (!pool->initialized) {
            continue;
        }
        pool_lock(pool);
        size_t free = pool->total_size - pool->max_used;
        pool_unlock(pool);
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

    pool_lock(pool);
    void* ptr = pool_alloc_locked(pool, size, false, module, line);
    if (ptr != NULL) {
        tracker_add(ptr, size, module, line);
    }
    pool_unlock(pool);
    return ptr;
}

/* =============================================================================
 * SYS_INIT 自动初始化
 * ============================================================================= */

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
                                   .max_allocations = MAX_ALLOCATIONS};
    /* 显式传播初始化失败：SYS_INIT 返回非 0 时 Zephyr 会在启动阶段报致命错误，
     * 而不是带着未初始化的内存系统继续运行 */
    return sys_mem_init(&mem_config);
#else
    return 0;
#endif
}

SYS_INIT(sys_mem_auto_init, POST_KERNEL, APP_INIT_PRIO_SYS_MEM);
