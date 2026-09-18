/**
 * @file sys_memory.h
 * @brief 系统内存管理头文件
 *
 * 提供内存池管理、分配跟踪和统计功能。
 * 支持多个独立内存池，每个池可配置不同大小和用途。
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            补充 init 并发语义、realloc 独占要求、
 *                                                get_min_free_size 语义澄清
 *
 */

#ifndef SYS_MEMORY_H
#define SYS_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_SYS_MEMORY_DEBUG) && CONFIG_SYS_MEMORY_DEBUG
#define SYS_MEM_ALLOC_WITH_INFO(type, size, mod, line) sys_mem_alloc_with_info(type, size, mod, line)
#else
#define SYS_MEM_ALLOC_WITH_INFO(type, size, mod, line) sys_mem_alloc(type, size)
#endif

/* =============================================================================
 * 类型定义
 * ============================================================================= */

/**
 * @brief 内存池类型枚举
 *
 * 定义不同的内存池类别，用于专门的内存分配场景。
 * 每个池可以独立配置大小，满足不同的内存需求。
 */
typedef enum {
    SYS_MEM_POOL_GENERAL = 0, ///< 通用内存池 - 一般用途分配
    SYS_MEM_POOL_EVENT,       ///< 事件内存池 - 用于事件数据分配
    SYS_MEM_POOL_MODULE,      ///< 模块内存池 - 模块专用内存
    SYS_MEM_POOL_DMA,         ///< DMA 内存池 - 支持 DMA 的内存（如可用）
    SYS_MEM_POOL_COUNT        ///< 内存池数量（用于数组定义）
} sys_mem_pool_type_t;

/**
 * @brief 内存统计信息结构体
 *
 * 包含内存池的完整统计信息，用于监控和调试。
 * 所有计数器在初始化时清零，可通过 sys_mem_reset_stats() 重置。
 */
typedef struct {
    size_t   total_size;    ///< 池总大小（字节）
    size_t   used_size;     ///< 当前已使用大小（字节）
    size_t   free_size;     ///< 当前可用大小（字节）
    size_t   max_used;      ///< 历史最大使用量（峰值，字节）
    uint32_t alloc_count;   ///< 累计分配次数
    uint32_t free_count;    ///< 累计释放次数
    uint32_t fail_count;    ///< 分配失败次数
    uint32_t fragmentation; ///< 碎片化程度百分比（0-100）
} sys_mem_stats_t;

/** 分配模块名称最大长度 */
#ifndef SYS_MEM_MODULE_NAME_LEN
#define SYS_MEM_MODULE_NAME_LEN 16
#endif

/**
 * @brief 分配信息结构体（调试用）
 *
 * 记录每次内存分配的详细信息，用于泄漏检测和调试。
 * 仅在启用跟踪功能时有效。
 */
typedef struct {
    void*    ptr;                             ///< 分配的内存指针
    size_t   size;                            ///< 请求的大小（字节）
    uint32_t timestamp;                       ///< 分配时间戳（系统运行毫秒数）
    char     module[SYS_MEM_MODULE_NAME_LEN]; ///< 分配模块名称（可选）
    uint32_t line;                            ///< 源代码行号（可选）
} sys_mem_alloc_info_t;

/* =============================================================================
 * 配置结构
 * ============================================================================= */

/**
 * @brief 内存系统配置结构体
 *
 * 用于初始化内存管理系统，可自定义各池大小和功能开关。
 * 在 sys_mem_init() 时传入，之后不可修改。
 */
typedef struct {
    size_t   pool_sizes[SYS_MEM_POOL_COUNT]; ///< 各内存池大小配置（字节）
    bool     enable_tracking;                ///< 是否启用分配跟踪
    bool     enable_defrag;                  ///< 是否启用碎片整理
    uint32_t max_allocations;                ///< 最大跟踪分配数
} sys_mem_config_t;

/* =============================================================================
 * 核心 API
 * ============================================================================= */

/**
 * @brief 初始化内存管理系统
 *
 * 初始化所有内存池和分配跟踪器。
 * 必须在调用任何其他内存函数之前调用此函数。
 *
 * @param config 配置结构体指针。如为 NULL，则使用默认配置：
 *               - 通用池：DEFAULT_POOL_SIZE
 *               - 事件池：DEFAULT_POOL_SIZE/2
 *               - 模块池：DEFAULT_POOL_SIZE/2
 *               - DMA 池：0（默认不启用）
 *               - 跟踪：启用
 *               - 碎片整理：禁用
 *
 * @return 返回值
 * @retval 0 成功
 * @retval -EINVAL 配置无效
 * @retval -EBUSY 内存系统仍在使用中（存在活跃分配或并发内存操作进行中）
 *
 * @note 此函数会初始化互斥锁，确保线程安全
 * @note 并发语义：重复初始化时逐池检查活跃分配数（active_count > 0 拒绝），并
 *       以短超时试锁探测并发操作；检测到任一池/跟踪器仍有使用者时返回 -EBUSY，
 *       不会破坏正在使用的内存池与互斥锁。无活跃分配时允许安全重初始化
 */
int sys_mem_init(const sys_mem_config_t* config);

/**
 * @brief 从内存池分配内存
 *
 * 从指定的内存池分配指定大小的内存。
 * 返回的内存内容未初始化（不保证为零）。
 *
 * @param type 内存池类型
 * @param size 请求的大小（字节）
 * @return 成功时返回分配的内存指针，失败时返回 NULL
 *
 * @note 内存按 8 字节对齐；如指定池不可用，自动回退到通用池
 */
void* sys_mem_alloc(sys_mem_pool_type_t type, size_t size);

/**
 * @brief 从内存池分配并清零内存
 *
 * 分配内存并将内容初始化为零。
 *
 * @param type 内存池类型
 * @param size 请求的大小（字节）
 * @return 成功时返回分配的内存指针，失败时返回 NULL
 *
 * @note 等同于 sys_mem_alloc() 后跟 memset() 清零
 */
void* sys_mem_calloc(sys_mem_pool_type_t type, size_t size);

/**
 * @brief 释放内存回池
 *
 * 释放先前分配的内存。包含验证功能以检测：
 * - 重复释放（double-free）
 * - 无效指针
 * - 池类型不匹配
 *
 * @param type 内存池类型（用于验证；实际归属以指针的分配头为准）
 * @param ptr 要释放的指针
 *
 * @note 传入 NULL 指针是安全的（无操作）
 * @note 池类型不匹配会产生警告，但会正确处理
 * @note 释放后指针不应再使用
 */
void sys_mem_free(sys_mem_pool_type_t type, void* ptr);

/**
 * @brief 重新分配内存块
 *
 * 更改已分配内存块的大小。
 * 内容保留到新旧大小的较小值。
 *
 * @param type 新分配的内存池类型
 * @param ptr 现有指针（可为 NULL，此时等同于分配）
 * @param size 新大小（字节）（如为 0，则释放指针）
 * @return 成功时返回重新分配的内存指针，失败时返回 NULL
 *
 * @retval NULL 分配失败，原指针不变
 * @retval 非 NULL 新指针，原指针已释放（如不同）
 *
 * @note 如 ptr 为 NULL，行为同 sys_mem_alloc()
 * @note 如 size 为 0，行为同 sys_mem_free()
 * @note 新指针可能与原指针不同（数据会复制）
 * @warning ptr 在调用期间必须由调用方独占（无并发 free/realloc 同一指针）：
 *          实现为「读旧尺寸 → 解锁 → 分配新块 → 复制 → 释放旧块」的多步序列，
 *          期间并发操作同一 ptr 会破坏其内容或产生未定义行为
 */
void* sys_mem_realloc(sys_mem_pool_type_t type, void* ptr, size_t size);

/* =============================================================================
 * 统计与调试 API
 * ============================================================================= */

/**
 * @brief 获取内存池统计信息
 *
 * 检索指定内存池的综合统计信息。
 * 统计信息受互斥锁保护，代表一致的时间点快照。
 *
 * @param type 内存池类型
 * @param stats 输出：统计信息结构体指针（不能为 NULL）
 *
 * @note 统计信息在调用期间被锁定
 */
void sys_mem_get_stats(sys_mem_pool_type_t type, sys_mem_stats_t* stats);

/**
 * @brief 重置内存统计信息
 *
 * 重置运行时统计信息（max_used、计数器），
 * 但保留当前使用量信息。
 *
 * @param type 内存池类型
 *
 * @note 不影响已分配的内存，仅重置统计计数器
 * @note max_used 重置为当前 used_size
 */
void sys_mem_reset_stats(sys_mem_pool_type_t type);

/**
 * @brief 获取活跃分配数量
 *
 * 返回当前已分配但未释放的内存块数量。
 *
 * @param type 内存池类型
 * @return 活跃分配数量
 * @retval 0 池无效或无活跃分配
 *
 * @note 使用独立的 active_count 计数，不受 sys_mem_reset_stats 影响
 */
uint32_t sys_mem_get_active_allocations(sys_mem_pool_type_t type);

/**
 * @brief 转储分配信息（调试）
 *
 * 向控制台打印详细的分配信息。
 * 用于调试内存问题。
 *
 * @param type 内存池类型
 *
 * @note 输出到 printk/控制台，非日志系统
 * @note 如启用跟踪，包含活跃分配及时间戳
 * @note 线程安全
 */
void sys_mem_dump_allocations(sys_mem_pool_type_t type);

/**
 * @brief 检查内存泄漏
 *
 * 通过检查未释放的跟踪分配来统计潜在的内存泄漏。
 *
 * @param type 内存池类型。传入无效值（>= SYS_MEM_POOL_COUNT）时检查所有池。
 * @return 潜在泄漏数量
 * @retval 0 未检测到泄漏或跟踪已禁用
 *
 * @note 需要在初始化时启用跟踪功能
 * @note 仅检测跟踪器中记录的分配
 */
uint32_t sys_mem_check_leaks(sys_mem_pool_type_t type);

/**
 * @brief 碎片整理内存池
 *
 * 尝试回收碎片化内存。
 * 当前实现采用简单的"全释放则重置"策略。
 *
 * @param type 内存池类型
 * @return 回收的字节数
 * @retval 0 无法碎片整理或功能禁用
 *
 * @note 生产代码应实现更完善的碎片整理算法
 * @note 需要在配置中启用 enable_defrag
 * @note 仅当所有分配都已释放时才能回收内存
 */
size_t sys_mem_defrag(sys_mem_pool_type_t type);

/* =============================================================================
 * 便捷宏
 * ============================================================================= */

/**
 * @brief 从通用池分配内存
 * @param size 大小（字节）
 * @return 指针或 NULL
 */
#define SYS_MEM_ALLOC_GENERAL(size) sys_mem_alloc(SYS_MEM_POOL_GENERAL, size)

/**
 * @brief 释放到通用池
 * @param ptr 要释放的指针
 */
#define SYS_MEM_FREE_GENERAL(ptr)   sys_mem_free(SYS_MEM_POOL_GENERAL, ptr)

/**
 * @brief 从事件池分配内存
 * @param size 大小（字节）
 * @return 指针或 NULL
 */
#define SYS_MEM_ALLOC_EVENT(size)   sys_mem_alloc(SYS_MEM_POOL_EVENT, size)

/**
 * @brief 释放到事件池
 * @param ptr 要释放的指针
 */
#define SYS_MEM_FREE_EVENT(ptr)     sys_mem_free(SYS_MEM_POOL_EVENT, ptr)

/* =============================================================================
 * 堆信息 API
 * ============================================================================= */

/**
 * @brief 获取总堆大小
 *
 * 返回所有配置的内存池大小之和。
 *
 * @return 总堆大小（字节）
 */
size_t sys_mem_get_heap_size(void);

/**
 * @brief 获取当前可用堆大小
 *
 * 返回所有内存池当前可用的总内存。
 *
 * @return 可用堆大小（字节）
 */
size_t sys_mem_get_free_size(void);

/**
 * @brief 获取各池最小剩余空间的较小值（峰值使用量指标）
 *
 * 对每个池独立计算 total_size - max_used（该池各自峰值使用时刻的剩余量），
 * 返回其中的最小值。注意这不是同一时刻的全局合计——各池的峰值可出现在
 * 不同时刻，因此该值作为内存压力指标偏保守。
 *
 * @return 各池"峰值时刻剩余量"的最小值（字节）；无已初始化池时返回 0
 */
size_t sys_mem_get_min_free_size(void);

/**
 * @brief 从指定内存池分配内存，记录分配信息（调试用）
 * @param  type      内存池类型
 * @param  size      大小（字节）
 * @param  module    模块名称
 * @param  line      行号
 * @return 分配的内存指针，失败返回 NULL
 */
void* sys_mem_alloc_with_info(sys_mem_pool_type_t type, size_t size, const char* module, uint32_t line);
#ifdef __cplusplus
}
#endif

#endif /* SYS_MEMORY_H */
