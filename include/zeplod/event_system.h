/**
 * @file event_system.h
 * @brief 核心事件系统头文件
 *
 * 为 Zephyr RTOS 设计的高性能实时事件系统。
 * 提供发布 - 订阅模式，支持线程安全操作。
 *
 * 主要特性：
 * - 支持最多 256 种事件类型
 * - 支持多个订阅者 per 事件类型
 * - 线程安全的发布/订阅操作
 * - 支持 ISR 上下文发布事件
 * - 动态事件数据分配
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-05-09       1.0            zeh            修正文档与注释
 *
 */

#ifndef EVENT_SYSTEM_H
#define EVENT_SYSTEM_H

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 配置宏
 * 可通过 Kconfig 或 prj.conf 覆盖默认值
 * ============================================================================= */

/**
 * @brief 最大支持的事件类型数量
 * @note 可通过 CONFIG_EVENT_MAX_TYPES 配置，默认 256
 */
#ifndef CONFIG_EVENT_MAX_TYPES
#define CONFIG_EVENT_MAX_TYPES 256
#endif

/**
 * @brief 事件队列大小（最多容纳的事件数量）
 * @note 队列满时，新事件将被丢弃
 */
#ifndef CONFIG_EVENT_QUEUE_SIZE
#define CONFIG_EVENT_QUEUE_SIZE 32
#endif

/**
 * @brief 每个事件类型最多支持的订阅者数量
 */
#ifndef CONFIG_EVENT_MAX_SUBSCRIBERS
#define CONFIG_EVENT_MAX_SUBSCRIBERS 8
#endif

/**
 * @brief 事件类型名称最大长度（含 NUL），用于 register_type 内部拷贝
 */
#ifndef CONFIG_EVENT_TYPE_NAME_MAX
#define CONFIG_EVENT_TYPE_NAME_MAX 32
#endif

/**
 * @brief 事件分发器线程栈大小（字节）
 */
#ifndef CONFIG_EVENT_DISPATCHER_STACK_SIZE
#define CONFIG_EVENT_DISPATCHER_STACK_SIZE 2048
#endif

/**
 * @brief 事件分发器线程优先级
 * @note Zephyr 中数值越小优先级越高，0 为最高优先级
 */
#ifndef CONFIG_EVENT_DISPATCHER_PRIORITY
#define CONFIG_EVENT_DISPATCHER_PRIORITY 5
#endif

/* =============================================================================
 * 内存配置宏
 * ============================================================================= */

/** 内联数据大小（从 Kconfig 获取，默认 48 字节） */
#ifndef CONFIG_EVENT_INLINE_DATA_SIZE
#define CONFIG_EVENT_INLINE_DATA_SIZE 48
#endif

/** 事件结构体大小（从 Kconfig 获取，默认 64 字节） */
#ifndef CONFIG_EVENT_STRUCT_SIZE
#define CONFIG_EVENT_STRUCT_SIZE 64
#endif

/* =============================================================================
 * 事件标志位定义
 * ============================================================================= */

/** 无效的订阅者 ID */
#define EVENT_SUBSCRIBER_ID_INVALID 0U

/** 数据内联存储 */
#define EVENT_FLAG_DATA_INLINE      0x01U

/** 数据动态分配 */
#define EVENT_FLAG_DATA_DYNAMIC     0x02U

/** event_t 来自 slab 池 */
#define EVENT_FLAG_FROM_SLAB        0x04U

/** 动态数据来自 slab 池（与 EVENT_FLAG_DATA_DYNAMIC 配合使用） */
#define EVENT_FLAG_DATA_FROM_SLAB   0x08U

/** 数据 slab 大小标记（CRIT-NEW-1：记录实际分配来源，避免级联 fallback 释放时配对错误） */
#define EVENT_FLAG_SLAB_256         0x10U /**< 数据来自 256B slab */
#define EVENT_FLAG_SLAB_1K          0x20U /**< 数据来自 1KB slab */
#define EVENT_FLAG_SLAB_4K          0x40U /**< 数据来自 4KB slab */
#define EVENT_FLAG_SLAB_MASK        0x70U /**< slab 标记位掩码 (bits 4-6) */

/* =============================================================================
 * 类型定义
 * ============================================================================= */

/**
 * @brief 事件类型标识符（支持最多 256 种事件类型）
 * @note 取值范围：0-255
 */
typedef uint8_t event_type_t;

/* event_type_t is uint8_t, so runtime checks like type >= 256 are ineffective.
 * Keep CONFIG_EVENT_MAX_TYPES within representable range.
 */
BUILD_ASSERT(CONFIG_EVENT_MAX_TYPES > 0U, "CONFIG_EVENT_MAX_TYPES must be > 0");
BUILD_ASSERT(CONFIG_EVENT_MAX_TYPES <= UINT8_MAX + 1U,
             "CONFIG_EVENT_MAX_TYPES must be <= 256 for uint8_t event_type_t");

/**
 * @brief 预定义的事件类型 ID
 * @note 可在各模块中注册具体名称
 */
#define EVENT_TYPE_GENERIC             1U  /**< 通用事件类型 */
#define EVENT_TYPE_SENSOR_DATA         10U /**< 传感器数据事件 */
#define EVENT_TYPE_SENSOR_CONFIG       11U /**< 传感器配置事件 */
/**
 * @brief Thread IPC Service 结果事件
 * @note 与 CONFIG_THREAD_IPC_SERVICE_EVENT_BRIDGE 配合使用
 */
#define EVENT_TYPE_THREAD_IPC_RESPONSE 20U

/**
 * @brief Data Bus 可用通知（与 CONFIG_DATA_BUS_EVENT_TYPE_ID 默认 30 对齐）
 * @note 启用 CONFIG_DATA_BUS_EVENT_BRIDGE 时由桥接模块注册该类型
 */
#define EVENT_TYPE_DATA_BUS_AVAILABLE  30U

/**
 * @brief 事件优先级枚举
 * @note 数值越小优先级越高，与 Zephyr 线程优先级约定一致
 */
typedef enum {
    EVENT_PRIORITY_LOW = 10,    /**< 低优先级事件 */
    EVENT_PRIORITY_NORMAL = 5,  /**< 普通优先级事件 */
    EVENT_PRIORITY_HIGH = 2,    /**< 高优先级事件 */
    EVENT_PRIORITY_CRITICAL = 0 /**< 临界优先级事件（最高） */
} event_priority_t;

/**
 * @brief 事件状态码枚举
 * @note 所有事件 API 均返回此枚举值
 */
typedef enum {
    EVENT_OK = 0,                 /**< 操作成功 */
    EVENT_ERR_NO_MEM = -1,        /**< 内存不足 */
    EVENT_ERR_QUEUE_FULL = -2,    /**< 队列已满 */
    EVENT_ERR_QUEUE_EMPTY = -3,   /**< 队列为空 */
    EVENT_ERR_INVALID_ARG = -4,   /**< 无效参数 */
    EVENT_ERR_NOT_FOUND = -5,     /**< 未找到 */
    EVENT_ERR_NO_SUBSCRIBER = -6, /**< 无订阅者 */
    EVENT_ERR_TIMEOUT = -7,       /**< 操作超时 */
    EVENT_ERR_NOT_RUNNING = -8    /**< 事件系统未运行 */
} event_status_t;

/**
 * @brief 事件数据结构
 *
 * 内存布局（以 64B 为例）：
 * ┌────────────────────────────────┐
 * │ type(1) priority(1) flags(1) ? │  4B
 * │ timestamp                      │  4B
 * │ source_id                      │  4B
 * │ data_len                       │  4B
 * ├────────────────────────────────┤  16B 头部
 * │ inline_data[48] 或 ptr(8)      │ 48B
 * └────────────────────────────────┘  64B 总计
 *
 * 数据存储策略：
 * - data_len ≤ INLINE_DATA_SIZE: 内联存储，无额外分配
 * - data_len > INLINE_DATA_SIZE: 从 slab/k_malloc 分配
 *
 * @note 结构体大小由 CONFIG_EVENT_STRUCT_SIZE 控制
 */
typedef struct {
    uint8_t  type;      /**< 事件类型标识符 */
    uint8_t  priority;  /**< 事件优先级 */
    uint8_t  flags;     /**< 标志位 (EVENT_FLAG_*) */
    uint8_t  reserved;  /**< 预留扩展：必须初始化为 0，为未来版本保留 */
    uint32_t timestamp; /**< 事件创建时间戳（毫秒 uptime） */
    uint32_t source_id; /**< 源模块/组件 ID */
    uint32_t data_len;  /**< 事件数据长度（字节） */
    union {
        uint8_t inline_data[CONFIG_EVENT_INLINE_DATA_SIZE]; /**< 内联数据 */
        void*   ptr;                                        /**< 外部数据指针 */
    } data;
} event_t;

/* 编译时验证结构体大小 */
BUILD_ASSERT(sizeof(event_t) == CONFIG_EVENT_STRUCT_SIZE, "event_t size mismatch with CONFIG_EVENT_STRUCT_SIZE");

/**
 * @brief 事件回调函数类型
 *
 * 订阅者通过此回调函数接收事件通知。
 *
 * @param event 指向事件的指针（只读，不要修改）
 * @param user_data 订阅时传入的用户数据
 *
 * @note 回调在分发器线程上下文中执行，避免长时间阻塞操作
 * @note event 仅在回调执行期间有效；返回后不得持有 event 或 event->data.ptr 指针
 * @note 不要在此回调中调用 event_free / event_free_data
 */
typedef void (*event_callback_t)(const event_t* event, void* user_data);

/**
 * @brief 订阅者条目结构
 *
 * 每个订阅者在注册时都会创建一个此结构。
 * callback == NULL 即表示槽位空闲（event_subscribe 拒绝 NULL 回调），
 * 不设单独的 is_active 标志以压缩事件类型表内存占用。
 */
typedef struct {
    event_callback_t callback;      /**< 回调函数指针；NULL 表示槽位空闲 */
    void*            user_data;     /**< 回调用户数据 */
    uint32_t         subscriber_id; /**< 唯一订阅者 ID（由系统分配） */
} subscriber_entry_t;

/**
 * @brief 事件类型注册表条目
 *
 * 每个已注册的事件类型对应一个此结构，包含该类型的所有订阅者信息。
 */
typedef struct {
    event_type_t       type;                                      /**< 事件类型 ID */
    char               name_storage[CONFIG_EVENT_TYPE_NAME_MAX];  /**< 类型名存储（register 时拷贝） */
    const char*        name;                                      /**< 指向 name_storage，未注册时为 NULL */
    atomic_t           registered;                                /**< 1=已注册（ISR/线程可无锁读取） */
    subscriber_entry_t subscribers[CONFIG_EVENT_MAX_SUBSCRIBERS]; /**< 订阅者数组 */
    uint32_t           subscriber_count;                          /**< 当前活跃的订阅者数量 */
    struct k_mutex     lock;                                      /**< 保护订阅者列表的互斥锁 */
} event_type_entry_t;

/* =============================================================================
 * 核心 API
 * 初始化和控制系统的主要接口
 * ============================================================================= */

/**
 * @brief 初始化事件系统
 *
 * 调用此函数初始化事件系统的所有内部数据结构：
 * - 初始化事件类型表
 * - 初始化消息队列
 * - 初始化互斥锁
 *
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 *
 * @note 必须在调用其他事件系统 API 之前调用此函数
 * @note 此函数是幂等的，重复调用不会产生错误
 */
event_status_t event_system_init(void);

/**
 * @brief 启动事件系统投递
 *
 * 将事件系统置为 running 状态，允许 event_publish/event_publish_from_isr 投递事件。
 *
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 *
 * @note 必须先调用 event_system_init()
 * @note 事件消费由 event_dispatcher 模块负责，本函数不会创建分发线程
 */
event_status_t event_system_start(void);

/**
 * @brief 停止事件系统投递
 *
 * 将事件系统置为非 running 状态；若分发器处于 RUNNING/PAUSED，会先停止分发器线程，
 * 再清空队列中未处理事件。随后调用 event_system_start() 时会自动重新启动分发器。
 *
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 *
 * @note 停止后发布的事件将被拒绝
 * @note 已排队但未处理的动态事件负载会被释放
 * @note 与 event_system_shutdown() 不同：stop 不释放队列与订阅表，可再次 start 恢复投递
 * @note 不得从事件回调（分发器线程）内部调用；否则返回 EVENT_ERR_INVALID_ARG
 * @note 若返回 EVENT_ERR_TIMEOUT，dispatcher 线程可能仍存活且队列未 purge；后续重复 stop
 *       可能因 running 已清零而返回 EVENT_OK，但不保证线程已终止。该状态应视为故障，建议系统复位。
 */
event_status_t event_system_stop(void);

/**
 * @brief 关闭事件系统
 *
 * 完全关闭事件系统，清理所有资源，重置所有状态。
 * 调用后需要重新调用 event_system_init 才能再次使用。
 *
 * @return EVENT_OK 成功；若 event_dispatcher_deinit() 失败则返回对应错误码且保留已分配资源，可重试 shutdown
 *
 * @note 会清理所有已注册的事件类型和订阅
 * @note 会释放所有动态分配的事件负载
 * @note 须在 magic 置为 IDLE 之前完成 event_queue_deinit/purge，否则 event_free_data 可能跳过释放
 * @note dispatcher_deinit 失败时 running 已为 0（禁止新入队），队列/订阅未释放；可重试 shutdown
 * @note 若返回 EVENT_ERR_TIMEOUT（join 超时），dispatcher 线程可能仍存活；建议记录故障并系统复位，
 *       不宜在无人工介入下反复调用 shutdown
 */
event_status_t event_system_shutdown(void);

/**
 * @brief 检查事件系统是否正在运行
 *
 * @return true 正在运行，false 已停止
 */
bool event_system_is_running(void);

/* =============================================================================
 * 事件类型管理
 * 在发布/订阅事件前，必须先注册事件类型
 * ============================================================================= */

/**
 * @brief 注册新的事件类型
 *
 * 注册一个事件类型，使其可以被订阅和发布。
 *
 * @param type 事件类型 ID（0-255）
 * @param name 事件类型名称（拷贝到内部缓冲，长度 < CONFIG_EVENT_TYPE_NAME_MAX）
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 *
 * @note 使用相同名称重复注册同一类型是幂等的；同一 ID 使用不同名称返回 EVENT_ERR_INVALID_ARG
 * @note 类型 ID 应预先规划，避免冲突
 * @note name 过长返回 EVENT_ERR_INVALID_ARG
 */
event_status_t event_register_type(event_type_t type, const char* name);

/**
 * @brief 注销事件类型
 *
 * 注销一个已注册的事件类型。
 *
 * @param type 事件类型 ID
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 *
 * @note 如果该类型仍有活跃订阅者，将返回错误
 * @note 注销后，该类型不能再被订阅或发布
 */
event_status_t event_unregister_type(event_type_t type);

/* =============================================================================
 * 订阅管理
 * 订阅者通过订阅事件类型来接收通知
 * ============================================================================= */

/**
 * @brief 订阅事件类型
 *
 * 注册一个回调函数来接收指定类型的事件通知。
 *
 * @param type 事件类型 ID
 * @param callback 回调函数指针（不能为 NULL）
 * @param user_data 用户数据，将在回调时原样传入
 * @param subscriber_id 输出参数，接收分配的订阅者 ID
 * @return EVENT_OK 成功，其他错误码见 event_status_t（未注册类型返回 EVENT_ERR_NOT_FOUND）
 *
 * @note 订阅者 ID 用于后续取消订阅
 * @note 每个订阅者最多订阅 CONFIG_EVENT_MAX_SUBSCRIBERS 个事件类型
 * @note 达到订阅上限时返回 EVENT_ERR_QUEUE_FULL（与队列满共用错误码，非队列入队失败）
 */
event_status_t event_subscribe(event_type_t type, event_callback_t callback, void* user_data, uint32_t* subscriber_id);

/**
 * @brief 取消订阅事件类型
 *
 * 从指定事件类型中移除订阅者。
 *
 * @param type 事件类型 ID
 * @param subscriber_id 要移除的订阅者 ID
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 *
 * @note 取消订阅后，该订阅者不再接收此类型的事件
 * @note 如果订阅者 ID 不存在，返回 EVENT_ERR_NOT_FOUND
 * @note 本函数立即从订阅表移除条目；若分发器线程已拍下回调快照，
 *       仍可能在返回后短暂执行旧回调。请勿在返回后立即释放 user_data，
 *       除非能证明无并发分发（例如已停止 dispatcher 或事件系统）。
 */
event_status_t event_unsubscribe(event_type_t type, uint32_t subscriber_id);

/**
 * @brief 从所有事件类型中取消订阅
 *
 * 遍历所有事件类型，移除指定订阅者 ID 的所有订阅。
 *
 * @param subscriber_id 要移除的订阅者 ID
 *
 * @note 用于模块卸载或清理时批量取消订阅
 * @note 此函数会锁定所有事件类型，可能耗时较长
 * @note 与 event_unsubscribe 相同：不等待已拍下快照的回调执行完毕
 */
void event_unsubscribe_all(uint32_t subscriber_id);

/* =============================================================================
 * 事件发布
 * 将事件发布到队列中，供订阅者消费
 * ============================================================================= */

/**
 * @brief 发布事件（同步方式）
 *
 * 将事件发布到全局事件队列中。事件会被复制到队列中，
 * 然后由分发器线程异步处理。
 *
 * @param event 要发布的事件（成功入队后会清除其动态数据所有权标记，便于 event_free）
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 *
 * @note 如果队列已满，事件将被丢弃并返回 EVENT_ERR_QUEUE_FULL；失败时调用方仍拥有动态负载
 * @note k_msgq 按值浅拷贝 event_t；成功入队后动态数据由队列内副本持有，勿再 event_free_data
 * @note 成功后可对同一 event 调用 event_free 释放壳体；失败时需自行释放动态数据
 * @note event->type 必须已 register_type，否则返回 EVENT_ERR_NOT_FOUND
 * @note 会校验 flags/data_len/data.ptr 一致性；手搓 event_t 不一致时返回 EVENT_ERR_INVALID_ARG
 */
event_status_t event_publish(event_t* event);

/**
 * @brief 从中断服务程序 (ISR) 发布事件
 *
 * 专用于 ISR 上下文的事件发布函数。
 *
 * @param event 要发布的事件（语义同 event_publish）
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 *
 * @note 此函数不使用互斥锁，适用于 ISR 上下文
 * @note 如果队列已满，事件将被丢弃
 * @note 避免在 ISR 中调用 event_publish_copy，因为其中包含 k_malloc
 * @note 与 event_publish 相同：event_system_stop 后 running 为假时返回 EVENT_ERR_NOT_RUNNING
 * @note 与 event_publish 相同：校验 flags/data_len 一致性
 */
event_status_t event_publish_from_isr(event_t* event);

/**
 * @brief 发布事件并复制数据
 *
 * 创建事件的内部副本，包括数据部分。
 * 适用于数据生命周期短于事件处理时间的场景。
 *
 * @param type 事件类型 ID
 * @param priority 事件优先级
 * @param data 要复制的数据指针
 * @param data_len 数据长度（字节）
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 *
 * @note 内存分配优先尝试 slab 池（若启用），失败时回退到 k_malloc
 * @note 数据副本在事件处理完成后由分发器自动释放
 * @note 适用于栈上数据或临时数据的发布
 */
event_status_t event_publish_copy(event_type_t type, event_priority_t priority, const void* data, size_t data_len);

/* =============================================================================
 * 实时安全 API
 * 用于 ISR 上下文和实时关键任务的确定性内存分配
 * ============================================================================= */

/**
 * @brief 创建事件（实时安全）
 *
 * @param type 事件类型 ID
 * @param priority 事件优先级
 * @return 指向新事件的指针，失败返回 NULL
 *
 * @note 完全从 slab 池分配，分配时间 O(1) 确定
 * @note Slab 耗尽时返回 NULL，不回退 k_malloc
 * @note 无 Slab 配置时返回 NULL
 */
event_t* event_create_rt(event_type_t type, event_priority_t priority);

/**
 * @brief 创建带数据的事件（实时安全）
 *
 * @param type 事件类型 ID
 * @param priority 事件优先级
 * @param data 要附加的数据指针
 * @param data_len 数据长度（字节）
 * @return 指向新事件的指针，失败返回 NULL
 *
 * @note 数据存储策略：
 *   - data_len <= INLINE_DATA_SIZE: 内联存储，无额外分配
 *   - data_len > INLINE_DATA_SIZE: 从 slab 池分配
 *   - 无可用 slab 或 slab 满: 返回 NULL
 * @note 完全实时安全，永不回退 k_malloc
 */
event_t* event_create_with_data_rt(event_type_t type, event_priority_t priority, const void* data, size_t data_len);

/**
 * @brief 发布事件并复制数据（实时安全）
 *
 * @param type 事件类型 ID
 * @param priority 事件优先级
 * @param data 要复制的数据指针
 * @param data_len 数据长度（字节）
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 *
 * @note 完全实时安全，内存不足时返回错误
 */
event_status_t event_publish_copy_rt(event_type_t type, event_priority_t priority, const void* data, size_t data_len);

/**
 * @brief 从 ISR 创建事件（实时安全）
 *
 * @param type 事件类型 ID
 * @param priority 事件优先级
 * @param data 数据指针
 * @param data_len 数据长度
 * @return 事件指针，失败返回 NULL
 *
 * @note 等同于 event_create_with_data_rt，明确 ISR 上下文使用
 */
event_t* event_create_from_isr(event_type_t type, event_priority_t priority, const void* data, size_t data_len);

/* =============================================================================
 * 事件创建与内存管理
 * 用于动态创建和管理事件对象
 * ============================================================================= */

/**
 * @brief 创建新事件
 *
 * 分配并初始化一个事件对象。
 *
 * @param type 事件类型 ID
 * @param priority 事件优先级
 * @return 指向新事件的指针，失败返回 NULL
 *
 * @note 返回的事件需用 event_free() 释放；flags 反映 event_t 自身的内存来源
 * @note 此函数仅分配事件外壳，不分配数据空间；data_len 初始为 0
 */
event_t* event_create(event_type_t type, event_priority_t priority);

/**
 * @brief 创建带数据的事件
 *
 * 分配一个事件对象并附加数据副本。
 *
 * @param type 事件类型 ID
 * @param priority 事件优先级
 * @param data 要附加的数据指针
 * @param data_len 数据长度（字节）
 * @return 指向新事件的指针，失败返回 NULL
 *
 * @note 如果 data 为 NULL 或 data_len 为 0，退化为 event_create()
 * @note 数据会被复制到新分配的内存中
 * @note 返回的事件必须用 event_free() 释放
 */
event_t* event_create_with_data(event_type_t type, event_priority_t priority, const void* data, size_t data_len);

/**
 * @brief 释放事件的动态数据
 *
 * 释放事件中的动态分配数据，不释放事件对象本身。
 * 正确处理来自 slab 池和 k_malloc 的数据。
 *
 * @param event 要释放数据的事件指针
 *
 * @note 如果 event->flags 包含 EVENT_FLAG_DATA_FROM_SLAB，使用 k_mem_slab_free 释放
 * @note 如果 event->flags 仅包含 EVENT_FLAG_DATA_DYNAMIC，使用 k_free 释放
 * @note 传入 NULL 是安全的
 */
void event_free_data(event_t* event);

/**
 * @brief 释放事件对象
 *
 * 释放事件对象及其动态分配的数据。
 *
 * @param event 要释放的事件指针
 *
 * @note 如果 event->flags 包含 EVENT_FLAG_DATA_DYNAMIC，event->data.ptr 也会被释放
 * @note 传入 NULL 是安全的，函数会直接返回
 * @note 不要释放栈上的事件对象
 */
void event_free(event_t* event);

/* =============================================================================
 * 工具函数
 * 调试、监控和辅助功能
 * ============================================================================= */

/**
 * @brief 获取事件类型名称
 *
 * @param type 事件类型 ID
 * @return 事件类型名称字符串
 *
 * @note 如果类型未注册，返回 "UNREGISTERED"
 * @note 如果类型 ID 无效，返回 "UNKNOWN"
 * @note 返回的是指向内部名称缓冲的指针，函数返回后不再持锁。仅当调用方能保证此后
 *       不会对该类型并发执行 event_unregister_type()/event_register_type() 时，指针内容才稳定；
 *       否则可能读到被覆盖或撕裂的字符串。返回指针不得跨 event_system_shutdown() 使用。
 *       如需长期保存，请在调用后立即拷贝到调用方缓冲区。
 */
const char* event_get_type_name(event_type_t type);

/**
 * @brief 获取事件类型的订阅者数量
 *
 * @param type 事件类型 ID
 * @return 活跃订阅者数量
 *
 * @note 此函数会获取互斥锁，避免在 ISR 中调用
 */
uint32_t event_get_subscriber_count(event_type_t type);

/**
 * @brief 获取事件系统统计信息
 *
 * @param total_events 输出：已处理的事件总数
 * @param queue_depth 输出：当前队列深度（已用槽位数）
 * @param dropped_events 输出：被丢弃的事件数量
 *
 * @note 用于系统监控和调试
 * @note 所有输出参数都可以为 NULL，表示不关心该值
 */
void event_get_statistics(uint32_t* total_events, uint32_t* queue_depth, uint32_t* dropped_events);

/**
 * @brief 重置事件系统统计信息
 *
 * 清零全局丢弃计数、已处理事件计数，并联动重置事件队列与分发器统计。
 * 由兼容层的 event_compat_reset_statistics() 调用。
 */
void event_system_reset_statistics(void);

/**
 * @brief 获取全局事件队列指针
 *
 * @return 指向全局事件队列的指针，未初始化时返回 NULL
 *
 * @note 主要用于高级用法，如直接操作队列
 * @note 仅在 event_system_init() 调用后有效
 */
struct k_msgq* event_system_get_queue(void);

/**
 * @brief 将事件分发给所有订阅者
 *
 * 遍历指定事件类型的所有活跃订阅者，调用其回调函数。
 *
 * @param event 要分发的事件
 * @return EVENT_OK 成功，其他错误码见 event_status_t
 *
 * @note 此函数在分发器线程中调用
 * @note 回调函数执行时不持有事件类型锁，避免竞态条件
 * @note 如果无订阅者，返回 EVENT_ERR_NO_SUBSCRIBER
 */
event_status_t event_notify_subscribers(const event_t* event);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_SYSTEM_H */
