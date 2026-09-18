/**
 * @file ipc_service.h
 * @brief 基于专用线程的 IPC 服务框架（支持同步/异步回调/Future 模式）
 *
 * 设计特点：
 * - 使用专用工作线程处理服务请求
 * - 使用专用分发线程投递响应到调用者
 * - 双消息队列设计（请求队列 + 响应队列）
 * - 无堆内存分配（队列和栈嵌入结构体）
 * - 支持三种调用模式：SYNC（阻塞）、ASYNC（回调）、FUTURE（未来值）
 *
 * 配置要求：CONFIG_THREAD_IPC_SERVICE=y
 *
 * 注意：与 Zephyr 子系统 IPC_SERVICE（核间 IPC）无关
 *
 * 典型使用流程：
 * 1. 定义服务函数 ipc_service_func_t
 * 2. 调用 ipc_service_init() 初始化服务
 * 3. 调用 ipc_service_start() 启动服务
 * 4. 使用 ipc_call_sync/async/future 调用服务
 * 5. 调用 ipc_service_stop() 停止服务
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-05-09       1.1            zeh            stop/cancel 语义与 ipc_call_sync 超时文档
 * 2026-09-18       1.2            zeh            补 re-init 前置条件与 future shm 访问器缺失登记
 *
 */

#ifndef ZEPHYR_IPC_SERVICE_H_
#define ZEPHYR_IPC_SERVICE_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <stdbool.h>
#include <stdint.h>

#include <zeplod/state_machine.h>

#if !IS_ENABLED(CONFIG_THREAD_IPC_SERVICE)
#error "ipc_service.h requires CONFIG_THREAD_IPC_SERVICE=y (see THREAD_IPC_SERVICE in Kconfig)"
#endif

/* 包含共享内存管理器（如果启用） */
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
#include <zeplod/ipc_shared_mem.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 请求 ID 类型
 * @note 用于唯一标识每次 IPC 请求
 */
typedef uint32_t ipc_request_id_t;

/**
 * @brief IPC 服务函数类型
 *
 * 服务提供者实现此函数来处理请求。
 *
 * @param request_id 请求 ID（由框架自动生成）
 * @param data 输入数据指针
 * @param data_size 输入数据大小
 * @param out_data 输出数据指针（由服务函数设置）
 * @param out_data_size 输出数据大小（由服务函数设置）
 * @return 0 成功，负值错误码失败
 *
 * @note 服务函数在工作线程上下文中执行
 * @note data 指向的输入缓冲区必须在请求处理完成前保持有效；若通过 ipc_call_async / ipc_call_future
 *       传入，调用者须保证缓冲区一直存活到回调触发或 future 完成。
 * @note out_data 可以是：
 *       - 输入数据的指针（零拷贝回显）
 *       - 通过 ipc_shm_alloc() 分配的共享内存（推荐；框架按指针解析句柄）
 *       - 服务线程的静态缓冲区（需谨慎管理生命周期）
 * @note 共享内存：输入句柄由 ipc_call_* 的 in_shm_handle 传入；输出由框架解析 out_data 或透传输入句柄
 */
typedef int (*ipc_service_func_t)(ipc_request_id_t request_id, const void* data, size_t data_size, void** out_data,
                                  size_t* out_data_size);

/**
 * @brief 异步回调函数类型
 *
 * 异步调用完成时由框架调用此回调。
 *
 * @param request_id 请求 ID
 * @param result 服务函数返回值
 * @param data 输出数据指针
 * @param data_size 输出数据大小
 * @param user_data 用户自定义数据
 *
 * @note 回调在分发器线程上下文中执行
 * @note 避免在回调中执行长时间阻塞操作
 */
typedef void (*ipc_async_callback_t)(ipc_request_id_t request_id, int result, const void* data, size_t data_size,
                                     void* user_data);

/**
 * @brief Future 结构体
 *
 * 用于 FUTURE 模式，存储异步操作的结果。
 *
 * @note 由框架内部管理，用户不应直接修改
 */
typedef struct ipc_future {
    ipc_request_id_t   request_id; /**< 关联的请求 ID */
    struct k_sem       semaphore;  /**< 完成信号量 */
    int                result;     /**< 服务函数返回值 */
    const void*        data;       /**< 输出数据指针 */
    size_t             data_size;  /**< 输出数据大小 */
    atomic_t           completed;  /**< 是否已完成（原子变量） */
    struct ipc_future* next;       /**< 空闲链表下一节点 */
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
    ipc_shm_handle_t shm_handle; /**< 共享内存句柄（0=未使用） */
#endif
} ipc_future_t;

/**
 * @brief 待处理请求结构体
 *
 * 跟踪每个待处理请求的状态。
 *
 * @note 由框架内部管理
 */
typedef struct ipc_pending_request {
    ipc_request_id_t     request_id;         /**< 请求 ID */
    struct k_thread*     caller_thread;      /**< 调用者线程 */
    ipc_async_callback_t callback;           /**< 异步回调函数（ASYNC 模式） */
    void*                callback_user_data; /**< 回调用户数据 */
    ipc_future_t*        future;             /**< 关联的 future（FUTURE 模式） */
    struct k_sem         response_sem;       /**< 响应信号量（SYNC 模式） */
    int                  result;             /**< 服务函数返回值 */
    const void*          response_data;      /**< 响应数据指针 */
    size_t               response_data_size; /**< 响应数据大小 */
    bool                 in_use;             /**< 槽位是否在使用中 */
    bool                 completed;          /**< 同步请求是否已收到响应。 */
    bool                 canceled;           /**< 请求已取消，但调用方仍可能持有槽位。 */
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
    ipc_shm_handle_t shm_handle; /**< 共享内存句柄（0=未使用） */
#endif
} ipc_pending_request_t;

/**
 * @brief 请求消息结构（内部使用）
 *
 * 通过请求队列传递的消息格式。
 */
typedef struct ipc_request_msg {
    ipc_request_id_t     request_id;         /**< 请求 ID */
    const void*          data;               /**< 输入数据 */
    size_t               data_size;          /**< 输入数据大小 */
    ipc_async_callback_t callback;           /**< 回调函数 */
    void*                callback_user_data; /**< 回调用户数据 */
    struct k_thread*     caller_thread;      /**< 调用者线程 */
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
    ipc_shm_handle_t shm_handle; /**< 共享内存句柄（0=未使用） */
#endif
} ipc_request_msg_t;

/**
 * @brief 响应消息结构（内部使用）
 *
 * 通过响应队列传递的消息格式。
 */
typedef struct ipc_response_msg {
    ipc_request_id_t request_id;    /**< 请求 ID */
    int              result;        /**< 服务函数返回值 */
    const void*      data;          /**< 输出数据 */
    size_t           data_size;     /**< 输出数据大小 */
    struct k_thread* caller_thread; /**< 调用者线程 */
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
    ipc_shm_handle_t shm_handle; /**< 共享内存句柄（0=未使用） */
#endif
} ipc_response_msg_t;

/**
 * @brief IPC 服务实例结构
 *
 * 包含 IPC 服务的所有内部状态和资源。
 * 队列和线程栈嵌入结构体，不使用动态内存分配。
 *
 * @note 用户应通过 API 操作此结构，不应直接访问内部成员
 * @warning 此结构体包含线程栈和队列缓冲区，体积较大（默认配置约 2.5KB，含共享内存可达 6KB+）。
 *          必须在全局/静态存储中定义，严禁在栈上分配。
 */
typedef struct ipc_service {
    const char*     name;            /**< 服务名称 */
    struct k_thread thread;          /**< 工作线程控制块 */
    struct k_thread response_thread; /**< 响应分发线程控制块 */
    struct k_msgq   request_queue;   /**< 请求消息队列 */
    struct k_msgq   response_queue;  /**< 响应消息队列 */

    /* 请求队列缓冲区（k_msgq 要求与 msg_size 指针对齐） */
    __aligned(sizeof(void*)) uint8_t
        request_queue_buf[CONFIG_THREAD_IPC_SERVICE_REQUEST_QUEUE_SIZE * sizeof(ipc_request_msg_t)];
    /* 响应队列缓冲区 */
    __aligned(sizeof(void*)) uint8_t
        response_queue_buf[CONFIG_THREAD_IPC_SERVICE_RESPONSE_QUEUE_SIZE * sizeof(ipc_response_msg_t)];

    /* 工作线程栈 */
    K_KERNEL_STACK_MEMBER(worker_stack, CONFIG_THREAD_IPC_SERVICE_STACK_SIZE);
    /* 分发线程栈 */
    K_KERNEL_STACK_MEMBER(dispatcher_stack, CONFIG_THREAD_IPC_SERVICE_STACK_SIZE);

    int priority; /**< 线程优先级 */

    ipc_service_func_t service_func; /**< 服务处理函数 */

    /* 待处理请求表 */
    ipc_pending_request_t pending_requests[CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS];
    struct k_mutex        pending_lock; /**< 保护待处理请求表的互斥锁 */
    /* Future 对象池 */
    ipc_future_t  futures[CONFIG_THREAD_IPC_SERVICE_MAX_PENDING_REQUESTS];
    ipc_future_t* free_futures; /**< 空闲 Future 链表头 */

    struct k_mutex       state_lock;       /**< 保护 running 的互斥锁 */
    zepl_state_machine_t lifecycle;        /**< 服务生命周期状态机 */
    bool                 initialized;      /**< 服务资源是否已初始化。 */
    bool                 running;          /**< 服务是否正在运行 */
    atomic_t             shutdown;         /**< 关闭标志：0=运行，非 0=正在/已关闭（原子读写，多核安全） */
    atomic_t             stop_in_progress; /**< stop 串行化标志：1=已有线程正在执行 stop */

#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
    ipc_shm_pool_t shm_pool; /**< 共享内存池（引用计数管理） */
#endif
} ipc_service_t;

/**
 * @brief 初始化 IPC 服务
 *
 * @param service 服务实例指针
 * @param name 服务名称
 * @param service_func 服务处理函数
 * @param priority 线程优先级
 * @return 0 成功，负值错误码失败
 *
 * @note 此函数只初始化资源，不启动线程
 * @note 必须在使用服务前调用
 * @note 线程栈大小、队列大小通过 Kconfig 配置：
 *       - CONFIG_THREAD_IPC_SERVICE_STACK_SIZE
 *       - CONFIG_THREAD_IPC_SERVICE_REQUEST_QUEUE_SIZE
 *       - CONFIG_THREAD_IPC_SERVICE_RESPONSE_QUEUE_SIZE
 * @warning 硬性前置条件：对已初始化实例重新 init（stop 后 re-init）前，必须确认所有
 *          in-flight 同步调用（ipc_call_sync / ipc_future_wait 等）均已返回。init 会整块
 *          清零实例（含 pending 表与信号量），若调用方仍阻塞在旧信号量上将永久睡眠或
 *          访问已回收内存。实现层会尽力拒绝仍有 pending 请求的 re-init（返回 -EBUSY），
 *          但该检查存在竞态窗口，不能替代本约定。
 */
int ipc_service_init(ipc_service_t* service, const char* name, ipc_service_func_t service_func, int priority);

/**
 * @brief 启动 IPC 服务
 *
 * 创建工作线程和响应分发线程。
 *
 * @param service 服务实例指针
 * @return 0 成功，负值错误码失败
 *
 * @note 启动后才能处理请求
 */
int ipc_service_start(ipc_service_t* service);

/**
 * @brief 停止 IPC 服务
 *
 * 停止工作线程和响应分发线程。
 *
 * @param service 服务实例指针
 * @return 0 成功；-EINVAL 参数无效；-EALREADY 已有其他线程正在执行 stop；
 *         -EIO 任一线程未能在超时内 join（实例保持 STOPPING，可稍后重试 stop）
 *
 * @note 停止后不再处理新请求；已在请求队列中的条目可能被 worker 丢弃（若对应 pending 已释放）
 * @note 若线程在 join 超时后仍未退出，实例保持 STOPPING 且不会强制 abort 线程；
 *       调用方应稍后重试 stop，直至线程退出并完成资源清理。
 */
int ipc_service_stop(ipc_service_t* service);

/**
 * @brief 同步调用 IPC 服务
 *
 * 发送请求并阻塞等待响应。
 *
 * @param service 服务实例指针
 * @param data 输入数据（可为 NULL，表示无输入数据）
 * @param data_size 输入数据大小（data 为 NULL 时应为 0）
 * @param out_data 输出数据指针
 * @param out_data_size 输出数据大小
 * @param timeout 超时时间（K_NO_WAIT 和零超时无效，会返回 -EINVAL）
 * @return 成功时为服务函数返回值；参数/资源错误时为 -EINVAL、-ENOMEM 等；阻塞等待失败时为
 *         k_sem_take 返回值（Zephyr 下超时常见 -EAGAIN，以所用内核版本为准）
 *
 * @note 调用线程将阻塞直到收到响应或超时
 * @note 若响应携带共享内存句柄，须使用 ipc_call_sync_shm()；ipc_call_sync() 将返回 -ENOTSUP
 * @warning 调用者必须保证输入数据（data 及其指向的内存）在请求处理完成前保持有效。
 *          超时返回仅表示调用者不再阻塞，worker 仍可能在访问该数据，调用者不得立即释放栈缓冲区或堆内存。
 * @warning 超时返回后，out_data 指向的数据可能已被释放，调用者不应使用超时返回后的数据。
 *          若请求携带共享内存输入（通过 ipc_call_sync_shm），超时后该内存引用会被框架自动释放；
 *          调用者不得继续使用原数据指针。
 */
int ipc_call_sync(ipc_service_t* service, const void* data, size_t data_size, void** out_data, size_t* out_data_size,
                  k_timeout_t timeout);

#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
/**
 * @brief 同步调用 IPC 服务（带共享内存句柄输入/输出）
 *
 * @param service 服务实例指针
 * @param data 输入数据
 * @param data_size 输入数据大小
 * @param in_shm_handle 输入共享内存句柄（0 表示无）
 * @param out_data 输出数据指针
 * @param out_data_size 输出数据大小
 * @param out_shm_handle 输出：共享内存句柄（可为 NULL，为 NULL 且存在输出句柄时自动 release）
 * @param timeout 超时时间
 * @return 语义同 ipc_call_sync；额外通过 out_shm_handle 返回共享内存句柄（若有）
 *
 * @note 若 out_shm_handle 非 NULL 且返回值非零句柄，调用者须在使用完 out_data 后 ipc_shm_release()
 */
int ipc_call_sync_shm(ipc_service_t* service, const void* data, size_t data_size, ipc_shm_handle_t in_shm_handle,
                      void** out_data, size_t* out_data_size, ipc_shm_handle_t* out_shm_handle, k_timeout_t timeout);
#endif

/**
 * @brief 异步调用 IPC 服务
 *
 * 发送请求并立即返回，结果通过回调通知。
 *
 * @param service 服务实例指针
 * @param data 输入数据（可为 NULL，表示无输入数据）
 * @param data_size 输入数据大小（data 为 NULL 时应为 0）
 * @param callback 回调函数
 * @param user_data 回调用户数据
 * @param in_shm_handle 输入共享内存句柄（仅 SHARED_MEM=y；0 表示无）
 * @param out_request_id 输出：请求 ID（可为 NULL）
 * @return 0 成功，负值错误码失败
 *
 * @note 调用线程立即返回，不阻塞
 * @note 回调在分发器线程中执行
 * @warning 调用者必须保证输入数据（data 及其指向的内存）在回调触发前保持有效，
 *          不得因 ipc_call_async 立即返回就释放栈缓冲区或堆内存。
 */
int ipc_call_async(ipc_service_t* service, const void* data, size_t data_size, ipc_async_callback_t callback,
                   void* user_data,
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                   ipc_shm_handle_t in_shm_handle,
#endif
                   ipc_request_id_t* out_request_id);

/**
 * @brief Future 模式调用 IPC 服务
 *
 * 发送请求并返回 future 对象，用于后续获取结果。
 *
 * @param service 服务实例指针
 * @param data 输入数据（可为 NULL，表示无输入数据）
 * @param data_size 输入数据大小（data 为 NULL 时应为 0）
 * @param in_shm_handle 输入共享内存句柄（仅 SHARED_MEM=y；0 表示无）
 * @param out_future 输出：future 对象指针
 * @return 0 成功，负值错误码失败
 *
 * @note 调用线程立即返回，不阻塞
 * @note 使用 ipc_future_wait 或 ipc_future_is_ready 获取结果
 * @note 已登记（API 不对称）：future 模式的共享内存输出句柄暂无访问器——future 携带的
 *       shm_handle 只能随 ipc_future_release 整体释放，调用方无法单独取用句柄延长
 *       生命周期。需要句柄级输出时请改用 ipc_call_sync_shm；后续版本考虑补充访问 API。
 * @warning 调用者必须保证输入数据（data 及其指向的内存）在 future 完成前保持有效，
 *          不得因 ipc_call_future 立即返回就释放栈缓冲区或堆内存。
 */
int ipc_call_future(ipc_service_t* service, const void* data, size_t data_size,
#if IS_ENABLED(CONFIG_THREAD_IPC_SERVICE_SHARED_MEM)
                    ipc_shm_handle_t in_shm_handle,
#endif
                    ipc_future_t** out_future);

/**
 * @brief 等待 Future 结果
 *
 * @param service 所属 IPC 服务实例（用于校验 future 归属）
 * @param future Future 对象指针
 * @param out_result 输出：服务函数返回值
 * @param out_data 输出：输出数据指针
 * @param out_data_size 输出：输出数据大小
 * @param timeout 超时时间
 * @return 0 成功，负值错误码失败
 *
 * @note 阻塞直到 future 完成或超时
 * @note 完成后须 ipc_future_release()；若 future 带 shm_handle，release 时一并释放
 */
int ipc_future_wait(ipc_service_t* service, ipc_future_t* future, int* out_result, const void** out_data,
                    size_t* out_data_size, k_timeout_t timeout);

/**
 * @brief 检查 Future 是否就绪
 *
 * @param future Future 对象指针
 * @return true 已就绪，false 未完成
 */
bool ipc_future_is_ready(ipc_future_t* future);

/**
 * @brief 释放 Future 对象
 *
 * @param service 服务实例指针
 * @param future Future 对象指针
 * @return 0 成功；-EBUSY 仍被 pending 请求引用；-EALREADY 已释放；其余为负值错误码
 *
 * @note 使用后必须释放 future 以回收资源
 * @note 仅可释放未被 pending 请求占用的 future
 */
int ipc_future_release(ipc_service_t* service, ipc_future_t* future);

/**
 * @brief 获取待处理请求数量
 *
 * @param service 服务实例指针
 * @return 待处理请求数量
 */
size_t ipc_service_get_pending_count(ipc_service_t* service);

/**
 * @brief 取消待处理请求
 *
 * @param service 服务实例指针
 * @param request_id 请求 ID
 * @return 0 成功，-ENOENT 未找到请求
 *
 * @note 仅当请求仍在 pending 表中时可取消；已完成并出表后不可取消
 * @note 已完成但尚未被同步调用方收尾的请求返回 -EALREADY，不会覆盖已产生的响应结果
 * @note 取消成功后 worker 若稍后从队列取出该 request_id，将跳过 service_func（不再投递响应）
 * @note 若 worker 已开始执行 service_func，取消无法中断其副作用；仅保证回调不被触发、
 *       响应被丢弃（dispatcher 按 request_id 找不到 pending 条目而安全释放）
 */
int ipc_service_cancel(ipc_service_t* service, ipc_request_id_t request_id);

/**
 * @brief 生成请求 ID（内部使用）
 *
 * @return 唯一的请求 ID
 *
 * @note 请求 ID 单调递增，跳过 0 值
 */
ipc_request_id_t ipc_generate_request_id(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_IPC_SERVICE_H_ */
