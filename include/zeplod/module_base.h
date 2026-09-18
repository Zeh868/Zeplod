/**
 * @file module_base.h
 * @brief 模块基类接口头文件
 *
 * 所有业务模块的抽象接口定义。
 *
 * 主要功能：
 * - 定义模块的标准接口（虚函数表）
 * - 提供模块状态和优先级枚举
 * - 提供模块注册宏
 * - 支持模块订阅事件系统
 *
 * 模块生命周期：
 * UNINITIALIZED -> INITIALIZING -> INITIALIZED -> RUNNING -> STOPPED
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            internal_data 标注拟移除；depends_version_min 登记防御计划
 *
 */

#ifndef MODULE_BASE_H
#define MODULE_BASE_H

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <stdbool.h>
#include <stdint.h>
#include <zeplod/event_system.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 类型定义
 * ============================================================================= */

/**
 * @brief 模块状态枚举
 *
 * 描述模块在其生命周期中的当前状态。
 */
typedef enum {
    MODULE_STATUS_UNINITIALIZED = 0, /**< 未初始化状态 */
    MODULE_STATUS_INITIALIZING,      /**< 正在初始化 */
    MODULE_STATUS_INITIALIZED,       /**< 已初始化，等待启动 */
    MODULE_STATUS_RUNNING,           /**< 正在运行 */
    MODULE_STATUS_STOPPED,           /**< 已停止 */
    MODULE_STATUS_ERROR,             /**< 错误状态 */
    MODULE_STATUS_SUSPENDED          /**< 已挂起：管理器暂停事件路由，不调用 stop()，硬件仍可能活跃 */
} module_status_t;

/**
 * @brief 模块优先级枚举
 *
 * 决定模块启动顺序，数值越小优先级越高。
 */
typedef enum {
    MODULE_PRIORITY_LOW = 10,    /**< 低优先级 */
    MODULE_PRIORITY_NORMAL = 5,  /**< 普通优先级 */
    MODULE_PRIORITY_HIGH = 2,    /**< 高优先级 */
    MODULE_PRIORITY_CRITICAL = 0 /**< 临界优先级（最高） */
} module_priority_t;

/**
 * @brief 模块事件处理器函数类型
 *
 * 模块通过此函数处理接收到的事件。
 *
 * @param event 接收到的事件指针
 * @param user_data 模块特定的用户数据
 */
typedef void (*module_event_handler_t)(const event_t* event, void* user_data);

/**
 * @brief 模块接口结构（虚函数表）
 *
 * 定义模块必须实现的标准接口函数。
 * 所有业务模块都应实现此接口结构。
 */
typedef struct {
    const char*       name;     /**< 模块名称 */
    uint32_t          version;  /**< 模块版本号（MAJOR.MINOR.PATCH 编码） */
    module_priority_t priority; /**< 模块优先级 */
    /**
     * 运行时依赖：指向以 NULL 结尾的字符串数组，每项为其它模块的 interface->name。
     * 仅在 Kconfig CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES 启用时参与排序；
     * 未启用或未使用时可置 NULL。
     *
     * 关于 MODULE_MANAGER_DEPENDS_LIST_MAX / CONFIG_MODULE_MANAGER_DEPENDS_LIST_MAX：
     * 表示「单个模块」在 depends_on 里最多列出多少个直接依赖名（不含末尾 NULL），
     * 管理器遍历时有上限以防未正确终止；不是全系统模块个数，也不是依赖链深度。
     */
    const char* const* depends_on;
    /**
     * 可选依赖版本下限：与 depends_on 下标一一对应。
     * NULL 表示不检查依赖版本；非 NULL 时必须与 depends_on 的非 NULL 项等长。
     * @warning 该数组无独立长度，仅靠与 depends_on 等长的契约约束。若长度短于
     *          depends_on，规划器按 depends_on 下标访问本数组将越界读取（未定义行为）。
     *          已登记：未来改为哨兵值终止并加运行时防御（属 API 变更，随版本演进处理）。
     * 无版本约束的依赖项填 MODULE_DEP_VERSION_ANY。
     * 版本值使用 MODULE_VERSION(major, minor, patch) 打包，不解析 semver 字符串。
     * 仅在 CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES 启用且 depends_on 非 NULL 时生效。
     */
    const uint32_t* depends_version_min;
    int (*init)(void* config);           /**< 初始化函数指针 */
    int (*start)(void);                  /**< 启动函数指针 */
    int (*stop)(void);                   /**< 停止函数指针 */
    int (*shutdown)(void);               /**< 关闭/销毁函数指针 */
    module_event_handler_t on_event;     /**< 事件处理函数指针 */
    module_status_t (*get_status)(void); /**< 获取状态函数指针 */
    int (*control)(int cmd, void* arg);  /**< 控制命令函数指针 */
} module_interface_t;

/**
 * @brief 模块最大事件订阅数量
 * @note 每个模块最多可订阅的事件类型数量
 */
#ifndef CONFIG_MODULE_MAX_EVENT_SUBSCRIPTIONS
#define CONFIG_MODULE_MAX_EVENT_SUBSCRIPTIONS 8
#endif

BUILD_ASSERT(CONFIG_MODULE_MAX_EVENT_SUBSCRIPTIONS <= 255);

/**
 * @brief 模块事件订阅结构
 *
 * 表示模块对某个事件类型的订阅。
 */
typedef struct {
    event_type_t type;          /**< 事件类型 ID */
    uint32_t     subscriber_id; /**< 订阅者 ID（由事件系统分配） */
    uint32_t     cookie;        /**< 管理器分配的稳定订阅身份，进程生命周期内不复用 */
    bool         removing;      /**< 正在锁外调用 event_unsubscribe，阻止重复操作和新分发 */
} module_event_subscription_t;

/**
 * @brief 模块操作态枚举（与 module_status_t 正交）
 *
 * 用于 start/stop 协议中防止同一槽位并发 start 或 stop。
 * 0 表示 idle，可以接受新操作；非 0 表示有进行中的操作。
 */
typedef enum {
    MM_OP_IDLE = 0,
    MM_OP_STARTING = 1,
    MM_OP_STOPPING = 2,
} mm_op_state_t;

/**
 * @brief 模块注册信息结构
 *
 * 包含模块注册后的所有管理信息。
 *
 * @note 指针字段（interface / config / internal_data）在锁外可能失效。
 *       跨锁使用时请结合 generation 字段校验，或在持锁期间复制所需数据。
 *       internal_data 由框架恒置 NULL（业务模块不应读写），计划后续版本移除。
 * @note generation 在每次清槽时自增，生命周期快照重入时用它检测
 *       「槽位已被回收并复用」。事件订阅使用独立且不复用的 cookie。
 */
typedef struct {
    const module_interface_t*   interface;     /**< 模块接口指针 */
    void*                       config;        /**< 模块配置数据 */
    void*                       internal_data; /**< 预留：框架内恒为 NULL，计划后续版本移除（勿使用） */
    module_status_t             status;        /**< 当前模块状态 */
    uint32_t                    id;            /**< 模块唯一 ID */
    module_event_subscription_t event_subscriptions[CONFIG_MODULE_MAX_EVENT_SUBSCRIPTIONS];
    uint8_t                     event_subscription_count; /**< 已订阅的事件类型数量 */
    mm_op_state_t               op_state;                 /**< 生命周期操作态：防止并发 start/stop */
    uint32_t                    generation;               /**< 槽位 generation：清槽时自增，用于跨锁稳定性校验 */
    atomic_t                    in_flight;                /**< 正在锁外执行的回调计数；注销前需等待归零 */
    atomic_t                    draining;                 /**< 注销入口置 1；新分发路径见到即跳过 */
    bool                        unregistering;            /**< unregister 入口防重入标志 */
} module_info_t;

static inline uint32_t mm_generation_next(uint32_t cur) {
    /* 0 保留给首次初始化；回绕时跳过 0。 */
    return (cur == UINT32_MAX) ? 1U : (cur + 1U);
}

/* =============================================================================
 * 模块辅助宏
 * ============================================================================= */

/**
 * @brief 版本号编码宏
 *
 * 将主版本号、次版本号、补丁号编码为 32 位整数。
 *
 * @param major 主版本号
 * @param minor 次版本号
 * @param patch 补丁号
 * @return 编码后的版本号
 * @note 各字段按 8 位存放；编码端对 major/minor/patch 均做 0xFF 掩码，
 *       避免某字段 >= 256 时溢出串入相邻字段。
 */
#define MODULE_VERSION(major, minor, patch)                                                                            \
    ((((uint32_t) (major) & 0xFFU) << 16) | (((uint32_t) (minor) & 0xFFU) << 8) | ((uint32_t) (patch) & 0xFFU))

/**
 * @brief 提取主版本号
 * @param v 编码后的版本号
 * @return 主版本号
 */
#define MODULE_VERSION_MAJOR(v) (((v) >> 16) & 0xFF)

/**
 * @brief 提取次版本号
 * @param v 编码后的版本号
 * @return 次版本号
 */
#define MODULE_VERSION_MINOR(v) (((v) >> 8) & 0xFF)

/**
 * @brief 提取补丁号
 * @param v 编码后的版本号
 * @return 补丁号
 */
#define MODULE_VERSION_PATCH(v) ((v) & 0xFF)

#define MODULE_DEP_VERSION_ANY  0U

/**
 * @brief 声明完整的模块接口（包含所有可选函数）
 *
 * 此宏用于声明并定义一个完整的模块接口结构。
 * 要求模块实现所有接口函数：init, start, stop, shutdown, on_event, get_status, control
 *
 * @param mod_name 模块名称（也是函数前缀）；勿用 name，以免与结构体字段 .name 冲突被宏展开替换
 *
 * @note 使用示例：在模块 .c 文件中使用 DECLARE_MODULE_INTERFACE(my_module);
 */
#define DECLARE_MODULE_INTERFACE(mod_name)                                                                             \
    extern const module_interface_t mod_name##_interface;                                                              \
    const module_interface_t        mod_name##_interface = {.name = #mod_name,                                         \
                                                            .version = MODULE_VERSION(1, 0, 0),                        \
                                                            .priority = MODULE_PRIORITY_NORMAL,                        \
                                                            .depends_on = NULL,                                        \
                                                            .depends_version_min = NULL,                               \
                                                            .init = mod_name##_init,                                   \
                                                            .start = mod_name##_start,                                 \
                                                            .stop = mod_name##_stop,                                   \
                                                            .shutdown = mod_name##_shutdown,                           \
                                                            .on_event = mod_name##_on_event,                           \
                                                            .get_status = mod_name##_get_status,                       \
                                                            .control = mod_name##_control}

/**
 * @brief 与 DECLARE_MODULE_INTERFACE 相同，但显式指定 depends_on 数组（NULL 结尾）
 */
#define DECLARE_MODULE_INTERFACE_WITH_DEPS(mod_name, deps_array)                                                       \
    extern const module_interface_t mod_name##_interface;                                                              \
    const module_interface_t        mod_name##_interface = {.name = #mod_name,                                         \
                                                            .version = MODULE_VERSION(1, 0, 0),                        \
                                                            .priority = MODULE_PRIORITY_NORMAL,                        \
                                                            .depends_on = (deps_array),                                \
                                                            .depends_version_min = NULL,                               \
                                                            .init = mod_name##_init,                                   \
                                                            .start = mod_name##_start,                                 \
                                                            .stop = mod_name##_stop,                                   \
                                                            .shutdown = mod_name##_shutdown,                           \
                                                            .on_event = mod_name##_on_event,                           \
                                                            .get_status = mod_name##_get_status,                       \
                                                            .control = mod_name##_control}

/**
 * @brief 与 DECLARE_MODULE_INTERFACE_WITH_DEPS 相同，并声明依赖版本下限数组
 */
#define DECLARE_MODULE_INTERFACE_WITH_DEPS_VERSION_MIN(mod_name, deps_array, versions_array)                           \
    extern const module_interface_t mod_name##_interface;                                                              \
    const module_interface_t        mod_name##_interface = {.name = #mod_name,                                         \
                                                            .version = MODULE_VERSION(1, 0, 0),                        \
                                                            .priority = MODULE_PRIORITY_NORMAL,                        \
                                                            .depends_on = (deps_array),                                \
                                                            .depends_version_min = (versions_array),                   \
                                                            .init = mod_name##_init,                                   \
                                                            .start = mod_name##_start,                                 \
                                                            .stop = mod_name##_stop,                                   \
                                                            .shutdown = mod_name##_shutdown,                           \
                                                            .on_event = mod_name##_on_event,                           \
                                                            .get_status = mod_name##_get_status,                       \
                                                            .control = mod_name##_control}

/**
 * @brief 声明最小化的模块接口（可选函数为 NULL）
 *
 * 此宏用于声明并定义一个最小化的模块接口结构。
 * 只要求模块实现必需函数：init, start, stop, on_event
 * shutdown, get_status, control 将被设置为 NULL
 *
 * @param mod_name 模块名称（也是函数前缀）
 *
 * @note 管理器在调用这些函数前会进行 NULL 检查
 */
#define DECLARE_MODULE_INTERFACE_MINIMAL(mod_name)                                                                     \
    extern const module_interface_t mod_name##_interface;                                                              \
    const module_interface_t        mod_name##_interface = {.name = #mod_name,                                         \
                                                            .version = MODULE_VERSION(1, 0, 0),                        \
                                                            .priority = MODULE_PRIORITY_NORMAL,                        \
                                                            .depends_on = NULL,                                        \
                                                            .depends_version_min = NULL,                               \
                                                            .init = mod_name##_init,                                   \
                                                            .start = mod_name##_start,                                 \
                                                            .stop = mod_name##_stop,                                   \
                                                            .shutdown = NULL,                                          \
                                                            .on_event = mod_name##_on_event,                           \
                                                            .get_status = NULL,                                        \
                                                            .control = NULL}

#define DECLARE_MODULE_INTERFACE_MINIMAL_WITH_DEPS(mod_name, deps_array)                                               \
    extern const module_interface_t mod_name##_interface;                                                              \
    const module_interface_t        mod_name##_interface = {.name = #mod_name,                                         \
                                                            .version = MODULE_VERSION(1, 0, 0),                        \
                                                            .priority = MODULE_PRIORITY_NORMAL,                        \
                                                            .depends_on = (deps_array),                                \
                                                            .depends_version_min = NULL,                               \
                                                            .init = mod_name##_init,                                   \
                                                            .start = mod_name##_start,                                 \
                                                            .stop = mod_name##_stop,                                   \
                                                            .shutdown = NULL,                                          \
                                                            .on_event = mod_name##_on_event,                           \
                                                            .get_status = NULL,                                        \
                                                            .control = NULL}

#define DECLARE_MODULE_INTERFACE_MINIMAL_WITH_DEPS_VERSION_MIN(mod_name, deps_array, versions_array)                   \
    extern const module_interface_t mod_name##_interface;                                                              \
    const module_interface_t        mod_name##_interface = {.name = #mod_name,                                         \
                                                            .version = MODULE_VERSION(1, 0, 0),                        \
                                                            .priority = MODULE_PRIORITY_NORMAL,                        \
                                                            .depends_on = (deps_array),                                \
                                                            .depends_version_min = (versions_array),                   \
                                                            .init = mod_name##_init,                                   \
                                                            .start = mod_name##_start,                                 \
                                                            .stop = mod_name##_stop,                                   \
                                                            .shutdown = NULL,                                          \
                                                            .on_event = mod_name##_on_event,                           \
                                                            .get_status = NULL,                                        \
                                                            .control = NULL}

#ifdef __cplusplus
}
#endif

#endif /* MODULE_BASE_H */
