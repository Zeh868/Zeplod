/**
 * @file event_dispatcher.c
 * @brief 事件分发器实现
 *
 * 高性能事件分发器，支持优先级调度和统计功能。
 *
 * 主要功能：
 * - 事件分发线程管理
 * - 事件过滤
 * - 处理延迟统计
 * - 批处理支持
 * @author zeh (china_qzh@163.com)
 * @version 1.6
 * @date 2026-09-29
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-05-09       1.0            zeh            文档注释修订
 * 2026-05-28       1.1            zeh            热路径：state 原子化，config 快照无锁读（P3.3）
 * 2026-09-18       1.2            zeh            拆分 init/stop/process_one 超长函数；EMA 种子读取纳入锁内防撕裂
 * 2026-09-27       1.3            zeh            统计同步 subscriber_filtered：get/reset 联动过滤跳过计数（memset
 * 路径随结构体覆盖） 2026-09-27       1.4            zeh            接入优先级批出队（process_all 一次取批/process_one
 * 单条有序）；新增提档统计 2026-09-27       1.5            zeh            手动消费准入收紧：RUNNING
 * 态仅分发线程自身放行（M-2，护共享批缓冲唯一消费者前提） 2026-09-29       1.6            zeh            优先级
 * process_all 重复取批直至排空；回调重入改走逐条出队
 *
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/time_units.h>
#include <zeplod/event_dispatcher.h>
#include "event_queue.h"
#include "event_system_internal.h"

LOG_MODULE_REGISTER(event_dispatcher, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部定义
 * ============================================================================= */

/** 默认栈大小（使用 Kconfig 配置） */
#define DEFAULT_STACK_SIZE       CONFIG_EVENT_DISPATCHER_STACK_SIZE

/** 默认优先级（使用 Kconfig 配置） */
#define DEFAULT_PRIORITY         CONFIG_EVENT_DISPATCHER_PRIORITY

/** 每个周期默认最大处理事件数（使用 Kconfig 配置） */
#define DEFAULT_MAX_EVENTS_CYCLE CONFIG_EVENT_DISPATCHER_MAX_EVENTS_PER_CYCLE

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

/**
 * @brief 分发器控制块
 *
 * 包含分发器的所有状态和配置信息。
 */
typedef struct {
    atomic_t            state;                        /**< 分发器当前状态（dispatcher_state_t） */
    dispatcher_config_t config;                       /**< 分发器配置（init 后不变） */
    uint32_t            hot_max_events_per_cycle;     /**< init 快照，热路径无锁读 */
    bool                hot_enable_stats;             /**< init 快照，热路径无锁读 */
    dispatcher_stats_t  stats;                        /**< 分发器统计信息 */
    struct k_thread     thread;                       /**< 分发器线程控制块 */
    K_KERNEL_STACK_MEMBER(stack, DEFAULT_STACK_SIZE); /**< 分发器线程栈 */
    event_filter_t filter;                            /**< 事件过滤函数 */
    void*          filter_user_data;                  /**< 过滤函数用户数据 */
    struct k_mutex lock;                              /**< 保护共享数据的互斥锁 */
    uint32_t       events_in_batch;                   /**< 当前批次处理的事件数 */
    uint64_t       last_event_time;                   /**< 上一个事件处理时间 */
    atomic_t       thread_started;                    /**< 分发线程是否已创建并运行 */
    bool           ever_started;                      /**< 本轮 init 后是否曾成功 start（用于手动消费判定） */
    uint32_t       thread_gen;                        /**< 当前分发线程世代（stop/join 配对用） */
} dispatcher_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

/** 全局分发器控制块实例 */
static dispatcher_cb_t g_dispatcher;

/** 全局事件队列指针（从事件系统获取） */
static struct k_msgq* g_event_queue;

/** init 完成后方可更新 stats（避免 SYS_INIT 顺序导致未初始化 mutex） */
static atomic_t g_dispatcher_initialized = ATOMIC_INIT(0);

/** ISR/线程均可递增；避免在 ISR 路径对 g_dispatcher.lock 调用 mutex */
static atomic_t g_dispatcher_events_dropped = ATOMIC_INIT(0);

#if defined(CONFIG_EVENT_PRIORITY_DISPATCH)
/** 优先级批出队路径累计的「有效优先级提档」事件-次计数（event_queue 经 inc 接口递增） */
static atomic_t g_dispatcher_aged_promotions = ATOMIC_INIT(0);

/** 优先级批出队缓冲：一次 dequeue_priority_batch 取批，处理期间持有批内事件 */
static event_t g_priority_batch[CONFIG_EVENT_QUEUE_SIZE];

/** process_all 批缓冲占用深度：回调再次进入时外层仍持有 g_priority_batch */
static uint32_t g_priority_batch_depth;
#endif /* CONFIG_EVENT_PRIORITY_DISPATCH */

/** 单调递增世代号；每次 k_thread_create 成功前递增并写入 thread_gen */
static atomic_t g_dispatcher_next_gen = ATOMIC_INIT(0);

/** Serializes init-time control-block reset with filter updates. */
static K_MUTEX_DEFINE(g_dispatcher_api_lock);

/** 串行化 init/start/stop/deinit 以及外部手动消费，避免控制块被并发重置 */
static K_SEM_DEFINE(g_dispatcher_lifecycle_gate, 1, 1);

/**
 * @brief 单批次统计累加器
 *
 * 每事件无锁折叠到本结构，批处理结束时（process_all 一轮 / process_one 一次）
 * 持锁提交一次，将热路径的每事件锁往返降为每批一次。
 * 仅 EMA 种子读取（每批首个事件）纳入 g_dispatcher.lock 短临界区：
 * event_dispatcher_reset_stats 可并发 memset 全局统计，无锁读取在 32 位 MCU
 * 上可能撕裂 64 位 events_processed；其余折叠路径仍保持无锁。
 */
typedef struct {
    bool     enable_stats;   /**< init 快照：本批是否累计统计 */
    bool     stats_seeded;   /**< EMA 是否已从全局统计接过种子 */
    uint32_t count;          /**< 本批已处理事件数 */
    uint32_t errors;         /**< 本批处理错误数 */
    uint32_t max_latency_us; /**< 本批最大延迟 */
    uint32_t avg_latency_us; /**< 与全局同公式（α=1/8）连续折叠的 EMA */
} batch_stats_t;

static void batch_stats_init(batch_stats_t* b) {
    b->enable_stats = g_dispatcher.hot_enable_stats;
    b->stats_seeded = false;
    b->count = 0;
    b->errors = 0;
    b->max_latency_us = 0;
    b->avg_latency_us = 0;
}

/**
 * @brief 从全局统计播种 EMA（短临界区持 g_dispatcher.lock）
 *
 * 与 event_dispatcher_reset_stats 的 memset 同锁互斥，避免 32 位 MCU 上
 * 64 位 events_processed 的撕裂读；每批仅首个事件付出一次锁往返。
 */
static void batch_stats_seed(batch_stats_t* b, uint32_t latency_us) {
    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);

    if (g_dispatcher.stats.events_processed == 0U) {
        b->avg_latency_us = latency_us; /* 全局首个事件：直接种子（v1 语义） */
    } else {
        b->avg_latency_us = (uint32_t) (((uint64_t) g_dispatcher.stats.avg_latency_us * 7 + latency_us) / 8);
    }

    k_mutex_unlock(&g_dispatcher.lock);
}

static void batch_stats_fold(batch_stats_t* b, uint32_t latency_us, bool error) {
    b->count++;
    if (error) {
        b->errors++;
    }
    if (latency_us > b->max_latency_us) {
        b->max_latency_us = latency_us;
    }

    if (!b->stats_seeded) {
        batch_stats_seed(b, latency_us);
        b->stats_seeded = true;
    } else {
        b->avg_latency_us = (uint32_t) (((uint64_t) b->avg_latency_us * 7 + latency_us) / 8);
    }
}

/** 持锁提交本批统计；count==0 时无状态变化，直接返回（不加锁） */
static void dispatcher_commit_batch(batch_stats_t* b, uint32_t batch_size) {
    if (b->count == 0U) {
        return;
    }

    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);
    g_dispatcher.last_event_time = k_uptime_get();
    g_dispatcher.events_in_batch = batch_size;

    if (b->enable_stats) {
        g_dispatcher.stats.events_processed += b->count;
        g_dispatcher.stats.processing_errors += b->errors;
        if (b->max_latency_us > g_dispatcher.stats.max_latency_us) {
            g_dispatcher.stats.max_latency_us = b->max_latency_us;
        }
        g_dispatcher.stats.avg_latency_us = b->avg_latency_us;
    }

    k_mutex_unlock(&g_dispatcher.lock);
}

/* =============================================================================
 * 前置声明
 * ============================================================================= */

/**
 * @brief 分发器线程入口函数
 */
static void dispatcher_thread_func(void* p1, void* p2, void* p3);

/**
 * @brief 处理单个事件（统计折叠进批累加器，无锁）
 * @param event 要处理的事件
 * @param batch 批统计累加器
 */
static void process_event(const event_t* event, batch_stats_t* batch);

/**
 * @brief 计算距上一个事件处理的空闲时间（微秒）
 *
 * MED-4: 参数实际传入的是 last_event_time（上一次 process_event 结束时刻），
 * 因此计算的是"系统空闲了多久"，而非事件从创建到处理的延迟。
 * 为保持向后兼容，外部接口 event_dispatcher_get_current_latency 名称未做更改。
 *
 * @param last_event_time 上一次事件处理完成的时间戳
 * @return 距上次事件处理的空闲时间（微秒）
 */
static uint32_t calculate_idle_time_us(uint64_t last_event_time);

/**
 * @brief 校验分发器配置参数
 *
 * @param config 配置指针，不可为 NULL
 * @return EVENT_OK 合法，EVENT_ERR_INVALID_ARG 非法
 */
static event_status_t event_dispatcher_validate_config(const dispatcher_config_t* config) {
    if (config->stack_size < EVENT_DISPATCHER_MIN_STACK_SIZE || config->stack_size > EVENT_DISPATCHER_MAX_STACK_SIZE) {
        LOG_ERR("Invalid stack size: %u (min: %u, max: %u)", config->stack_size, EVENT_DISPATCHER_MIN_STACK_SIZE,
                EVENT_DISPATCHER_MAX_STACK_SIZE);
        return EVENT_ERR_INVALID_ARG;
    }

    if (config->stack_size > DEFAULT_STACK_SIZE) {
        LOG_ERR("Stack size %u exceeds pre-allocated stack %u", config->stack_size, DEFAULT_STACK_SIZE);
        return EVENT_ERR_INVALID_ARG;
    }

    if (config->priority < EVENT_DISPATCHER_MIN_PRIORITY || config->priority > EVENT_DISPATCHER_MAX_PRIORITY) {
        LOG_ERR("Invalid priority: %d (min: %d, max: %d)", config->priority, EVENT_DISPATCHER_MIN_PRIORITY,
                EVENT_DISPATCHER_MAX_PRIORITY);
        return EVENT_ERR_INVALID_ARG;
    }

    if (config->max_events_per_cycle > EVENT_DISPATCHER_MAX_EVENTS_PER_CYCLE) {
        LOG_ERR("Invalid max_events_per_cycle: %u (max: %u)", config->max_events_per_cycle,
                EVENT_DISPATCHER_MAX_EVENTS_PER_CYCLE);
        return EVENT_ERR_INVALID_ARG;
    }

    return EVENT_OK;
}

static inline dispatcher_state_t dispatcher_state_load(void) {
    return (dispatcher_state_t) atomic_get(&g_dispatcher.state);
}

static inline void dispatcher_state_store(dispatcher_state_t state) {
    atomic_set(&g_dispatcher.state, (atomic_val_t) state);
}

static inline bool dispatcher_is_initialized(void) {
    return atomic_get(&g_dispatcher_initialized) != 0;
}

static inline bool dispatcher_thread_is_started(void) {
    return atomic_get(&g_dispatcher.thread_started) != 0;
}

static inline bool dispatcher_lifecycle_try_lock(void) {
    return k_sem_take(&g_dispatcher_lifecycle_gate, K_NO_WAIT) == 0;
}

static inline void dispatcher_lifecycle_unlock(void) {
    k_sem_give(&g_dispatcher_lifecycle_gate);
}

static void dispatcher_refresh_hot_config(void) {
    g_dispatcher.hot_max_events_per_cycle = g_dispatcher.config.max_events_per_cycle;
    g_dispatcher.hot_enable_stats = g_dispatcher.config.enable_stats;
}

static void dispatcher_set_state_locked(dispatcher_state_t state) {
    dispatcher_state_store(state);
}

static void dispatcher_mark_thread_started_locked(void) {
    atomic_set(&g_dispatcher.thread_started, 1);
    g_dispatcher.ever_started = true;
}

static void dispatcher_apply_thread_stopped_locked(uint32_t join_gen) {
    if (g_dispatcher.thread_gen == join_gen) {
        atomic_set(&g_dispatcher.thread_started, 0);
    } else {
        LOG_WRN("Dispatcher generation changed during stop (join_gen=%u current=%u); preserving thread_started",
                join_gen, g_dispatcher.thread_gen);
    }
}

/**
 * @brief 当前上下文是否允许调用 process_one / process_all
 *
 * RUNNING 态仅分发线程自身放行：共享批缓冲 g_priority_batch 与控制块统计
 * 的准入前提是「消费者唯一」——RUNNING 期间只有分发线程可消费；外部手动
 * 消费仅限 STOPPED 且从未 start（该形态下经 lifecycle gate 串行，不会与
 * 分发线程并存）。旧实现的「RUNNING && !thread_started 放行」窗口只在
 * start() 持锁临界区内可观测，收紧后无损。
 *
 * @pre 已持有 g_dispatcher.lock
 */
static bool dispatcher_can_process_locked(dispatcher_state_t state, bool thread_started, bool ever_started) {
    if (state == DISPATCHER_PAUSED) {
        return false;
    }

    if (state == DISPATCHER_RUNNING) {
        /* 仅分发线程自身（thread_started 恒真；窄窗内分发线程尚未起跑即被拒） */
        return (thread_started && k_current_get() == &g_dispatcher.thread);
    }

    if (state == DISPATCHER_STOPPED) {
        /* 仅 init、从未 start：允许外部手动消费；曾 start 后 stop 则禁止 */
        return !ever_started;
    }

    return false;
}

/* =============================================================================
 * 分发器控制 API
 * ============================================================================= */

/** init 失败路径统一释放两层锁（api_lock 先于 lifecycle gate，与获取顺序相反） */
static void dispatcher_init_unlock(void) {
    k_mutex_unlock(&g_dispatcher_api_lock);
    dispatcher_lifecycle_unlock();
}

/**
 * @brief 重置控制块但保留 filter/user_data（LOW-5）
 *
 * memset 会清除已设置的 filter 与 user_data，导致 stop->init 序列后
 * 用户先前注册的过滤器静默失效。在重置前保存，重置后恢复以保持幂等性；
 * 如需主动清除过滤器，调用方应显式 event_dispatcher_clear_filter()。
 */
static void dispatcher_init_reset_preserve_filter(void) {
    event_filter_t saved_filter = g_dispatcher.filter;
    void*          saved_filter_ud = g_dispatcher.filter_user_data;

    atomic_set(&g_dispatcher_initialized, 0);
    memset(&g_dispatcher, 0, sizeof(g_dispatcher));

    g_dispatcher.filter = saved_filter;
    g_dispatcher.filter_user_data = saved_filter_ud;
}

/**
 * @brief 应用分发器配置；config 为 NULL 时整套回退 Kconfig 默认值
 */
static void dispatcher_init_apply_config(const dispatcher_config_t* config) {
    if (config != NULL) {
        g_dispatcher.config = *config;
        if (config->thread_name == NULL) {
            g_dispatcher.config.thread_name = "event_disp";
            LOG_WRN("Thread name is NULL, using default 'event_disp'");
        }
        return;
    }

    g_dispatcher.config.stack_size = DEFAULT_STACK_SIZE;
    g_dispatcher.config.priority = DEFAULT_PRIORITY;
    g_dispatcher.config.thread_name = "event_disp";
    g_dispatcher.config.enable_stats = true;
    g_dispatcher.config.max_events_per_cycle = DEFAULT_MAX_EVENTS_CYCLE;
}

/**
 * @brief 初始化事件分发器
 *
 * @param config 分发器配置，NULL 使用默认配置
 * @return EVENT_OK 成功，EVENT_ERR_INVALID_ARG 配置无效或事件系统未初始化
 */
event_status_t event_dispatcher_init(const dispatcher_config_t* config) {
    LOG_DBG("Initializing event dispatcher...");

    if (!dispatcher_lifecycle_try_lock()) {
        return EVENT_ERR_TIMEOUT;
    }

    k_mutex_lock(&g_dispatcher_api_lock, K_FOREVER);

    /* HIGH-NEW-1: 防止在分发器线程运行时重新初始化。
     * memset 会清零 stack 等内嵌成员，若线程仍在运行则导致栈损坏。
     * thread_started 在 stop 后会被清零，因此正常重启序列不受限制。 */
    if (dispatcher_thread_is_started()) {
        LOG_WRN("Dispatcher thread already running, refusing re-init");
        dispatcher_init_unlock();
        return EVENT_ERR_INVALID_ARG;
    }

    if (config != NULL) {
        event_status_t vret = event_dispatcher_validate_config(config);
        if (vret != EVENT_OK) {
            dispatcher_init_unlock();
            return vret;
        }
    }

    dispatcher_init_reset_preserve_filter();
    dispatcher_init_apply_config(config);

    /* 初始化同步原语 */
    k_mutex_init(&g_dispatcher.lock);

    dispatcher_state_store(DISPATCHER_STOPPED);
    dispatcher_refresh_hot_config();
    g_dispatcher.last_event_time = k_uptime_get();
    atomic_set(&g_dispatcher.thread_started, 0);
    g_dispatcher.ever_started = false;

    g_event_queue = event_system_get_queue();
    if (g_event_queue == NULL) {
        LOG_ERR("Call event_system_init() before event_dispatcher_init()");
        dispatcher_init_unlock();
        return EVENT_ERR_INVALID_ARG;
    }

    atomic_set(&g_dispatcher_events_dropped, 0);
    atomic_set(&g_dispatcher_initialized, 1);

    dispatcher_init_unlock();

    LOG_DBG("Event dispatcher initialized");
    return EVENT_OK;
}

/**
 * @brief 启动分发器
 *
 * @return EVENT_OK 成功，EVENT_ERR_NO_MEM 线程创建失败
 */
event_status_t event_dispatcher_start(void) {
    if (!dispatcher_lifecycle_try_lock()) {
        return EVENT_ERR_TIMEOUT;
    }

    if (!dispatcher_is_initialized()) {
        dispatcher_lifecycle_unlock();
        return EVENT_ERR_INVALID_ARG;
    }

    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);

    if (dispatcher_state_load() == DISPATCHER_RUNNING) {
        k_mutex_unlock(&g_dispatcher.lock);
        dispatcher_lifecycle_unlock();
        return EVENT_OK;
    }

    if (dispatcher_state_load() == DISPATCHER_PAUSED) {
        dispatcher_set_state_locked(DISPATCHER_RUNNING);
        k_mutex_unlock(&g_dispatcher.lock);
        dispatcher_lifecycle_unlock();
        LOG_DBG("Event dispatcher resumed by start()");
        return EVENT_OK;
    }

    if (dispatcher_thread_is_started()) {
        if (dispatcher_state_load() == DISPATCHER_STOPPED) {
            k_mutex_unlock(&g_dispatcher.lock);
            dispatcher_lifecycle_unlock();
            LOG_WRN("Cannot start dispatcher while stop/join is in progress");
            return EVENT_ERR_TIMEOUT;
        }
        dispatcher_set_state_locked(DISPATCHER_RUNNING);
        k_mutex_unlock(&g_dispatcher.lock);
        dispatcher_lifecycle_unlock();
        return EVENT_OK;
    }

    dispatcher_set_state_locked(DISPATCHER_RUNNING);
    g_dispatcher.thread_gen = (uint32_t) atomic_inc(&g_dispatcher_next_gen);

    /* SIL-2: 创建分发器线程 */
    k_tid_t tid = k_thread_create(&g_dispatcher.thread, g_dispatcher.stack, g_dispatcher.config.stack_size,
                                  dispatcher_thread_func, NULL, NULL, NULL, g_dispatcher.config.priority, 0, K_FOREVER);

    /* SIL-2: 验证线程创建结果（IMP-7 修复） */
    if (tid == NULL) {
        dispatcher_set_state_locked(DISPATCHER_STOPPED);
        atomic_set(&g_dispatcher.thread_started, 0);
        k_mutex_unlock(&g_dispatcher.lock);
        dispatcher_lifecycle_unlock();
        LOG_ERR("Failed to create dispatcher thread");
        return EVENT_ERR_NO_MEM;
    }

    if (k_thread_name_set(&g_dispatcher.thread, g_dispatcher.config.thread_name) != 0) {
        LOG_WRN("Failed to set thread name, continuing anyway");
    }

    dispatcher_mark_thread_started_locked();
    k_thread_start(&g_dispatcher.thread);

    k_mutex_unlock(&g_dispatcher.lock);
    dispatcher_lifecycle_unlock();

    LOG_DBG("Event dispatcher started");
    return EVENT_OK;
}

/**
 * @brief join 分发线程并在成功后清理 thread_started
 *
 * join 失败时保持 thread_started=true：后续 stop/deinit 可重试 join，
 * start() 也无法在原线程可能仍持有事件负载时创建第二个消费者。
 *
 * @return EVENT_OK join 成功（或无需 join），EVENT_ERR_TIMEOUT join 超时/失败
 */
static event_status_t dispatcher_stop_join_thread(uint32_t join_gen, bool should_join) {
    if (should_join) {
        int jret = k_thread_join(&g_dispatcher.thread, K_MSEC(EVENT_DISPATCHER_THREAD_JOIN_TIMEOUT_MS));

        if (jret != 0) {
            LOG_ERR("Dispatcher thread join timeout/failed: %d (timeout=%u ms)", jret,
                    EVENT_DISPATCHER_THREAD_JOIN_TIMEOUT_MS);
            return EVENT_ERR_TIMEOUT;
        }
        LOG_DBG("Dispatcher thread joined successfully");
    }

    /* SIL-2: join 成功后清理状态；若 stop 期间另有 start 创建新线程则保留 thread_started */
    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);
    dispatcher_apply_thread_stopped_locked(join_gen);
    k_mutex_unlock(&g_dispatcher.lock);

    LOG_DBG("Event dispatcher stopped");
    return EVENT_OK;
}

/**
 * @brief 停止分发器
 *
 * @return EVENT_OK 成功
 */
static event_status_t event_dispatcher_stop_locked(void) {
    bool     should_join = false;
    uint32_t join_gen = 0U;

    if (!dispatcher_is_initialized()) {
        return EVENT_OK;
    }

    if (event_dispatcher_is_current_thread()) {
        LOG_ERR("Cannot stop dispatcher from dispatcher thread");
        return EVENT_ERR_INVALID_ARG;
    }

    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);

    if (dispatcher_state_load() == DISPATCHER_STOPPED) {
        if (dispatcher_thread_is_started()) {
            /* stop/join 已在别处进行：本次仅补 join，不重复置状态 */
            should_join = true;
            join_gen = g_dispatcher.thread_gen;
            k_mutex_unlock(&g_dispatcher.lock);
            LOG_WRN("Dispatcher stop/join already in progress, retrying join");
            return dispatcher_stop_join_thread(join_gen, should_join);
        }
        k_mutex_unlock(&g_dispatcher.lock);
        return EVENT_OK;
    }

    /* SIL-2: 设置停止状态，线程会在下次循环检查时退出；join 在锁外执行 */
    dispatcher_set_state_locked(DISPATCHER_STOPPED);
    should_join = dispatcher_thread_is_started();
    join_gen = g_dispatcher.thread_gen;
    k_mutex_unlock(&g_dispatcher.lock);

    return dispatcher_stop_join_thread(join_gen, should_join);
}

event_status_t event_dispatcher_stop(void) {
    if (event_dispatcher_is_current_thread()) {
        LOG_ERR("Cannot stop dispatcher from dispatcher thread");
        return EVENT_ERR_INVALID_ARG;
    }

    if (!dispatcher_lifecycle_try_lock()) {
        return EVENT_ERR_TIMEOUT;
    }

    event_status_t ret = event_dispatcher_stop_locked();
    dispatcher_lifecycle_unlock();
    return ret;
}

/**
 * @brief 反初始化分发器
 */
event_status_t event_dispatcher_deinit(void) {
    if (event_dispatcher_is_current_thread()) {
        LOG_ERR("Cannot deinit dispatcher from dispatcher thread");
        return EVENT_ERR_INVALID_ARG;
    }

    if (!dispatcher_lifecycle_try_lock()) {
        return EVENT_ERR_TIMEOUT;
    }

    if (!dispatcher_is_initialized()) {
        dispatcher_lifecycle_unlock();
        return EVENT_OK;
    }

    event_status_t ret = event_dispatcher_stop_locked();
    if (ret != EVENT_OK) {
        dispatcher_lifecycle_unlock();
        return ret;
    }

    k_mutex_lock(&g_dispatcher_api_lock, K_FOREVER);
    /* deinit/shutdown should reset external callback context to avoid stale pointers on next init */
    g_dispatcher.filter = NULL;
    g_dispatcher.filter_user_data = NULL;
    g_event_queue = NULL;
    atomic_set(&g_dispatcher_initialized, 0);
    k_mutex_unlock(&g_dispatcher_api_lock);
    dispatcher_lifecycle_unlock();

    LOG_DBG("Event dispatcher deinitialized");
    return EVENT_OK;
}

/**
 * @brief 暂停分发器
 *
 * @return EVENT_OK 成功，EVENT_ERR_INVALID_ARG 状态不正确
 */
event_status_t event_dispatcher_pause(void) {
    if (!dispatcher_lifecycle_try_lock()) {
        return EVENT_ERR_TIMEOUT;
    }

    if (!dispatcher_is_initialized()) {
        dispatcher_lifecycle_unlock();
        return EVENT_ERR_INVALID_ARG;
    }

    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);

    if (dispatcher_state_load() != DISPATCHER_RUNNING) {
        k_mutex_unlock(&g_dispatcher.lock);
        dispatcher_lifecycle_unlock();
        return EVENT_ERR_INVALID_ARG;
    }

    dispatcher_set_state_locked(DISPATCHER_PAUSED);
    k_mutex_unlock(&g_dispatcher.lock);
    dispatcher_lifecycle_unlock();

    LOG_DBG("Event dispatcher paused");
    return EVENT_OK;
}

/**
 * @brief 恢复分发器
 *
 * @return EVENT_OK 成功，EVENT_ERR_INVALID_ARG 状态不正确
 */
event_status_t event_dispatcher_resume(void) {
    if (!dispatcher_lifecycle_try_lock()) {
        return EVENT_ERR_TIMEOUT;
    }

    if (!dispatcher_is_initialized()) {
        dispatcher_lifecycle_unlock();
        return EVENT_ERR_INVALID_ARG;
    }

    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);

    if (dispatcher_state_load() != DISPATCHER_PAUSED) {
        k_mutex_unlock(&g_dispatcher.lock);
        dispatcher_lifecycle_unlock();
        return EVENT_ERR_INVALID_ARG;
    }

    dispatcher_set_state_locked(DISPATCHER_RUNNING);
    k_mutex_unlock(&g_dispatcher.lock);
    dispatcher_lifecycle_unlock();

    LOG_DBG("Event dispatcher resumed");
    return EVENT_OK;
}

/**
 * @brief 获取分发器状态
 *
 * @return 当前分发器状态
 */
bool event_dispatcher_is_initialized(void) {
    return dispatcher_is_initialized();
}

dispatcher_state_t event_dispatcher_get_state(void) {
    if (!dispatcher_is_initialized()) {
        return DISPATCHER_STOPPED;
    }

    return dispatcher_state_load();
}

/**
 * @brief 检查当前线程是否为分发器线程
 *
 * @return true 当前线程是分发器线程，false 不是或未初始化
 */
bool event_dispatcher_is_current_thread(void) {
    return dispatcher_is_initialized() && dispatcher_thread_is_started() && (k_current_get() == &g_dispatcher.thread);
}

/* =============================================================================
 * 事件处理 API
 * ============================================================================= */

/**
 * @brief 设置事件过滤器
 *
 * @param filter 过滤函数指针
 * @param user_data 用户数据
 */
void event_dispatcher_set_filter(event_filter_t filter, void* user_data) {
    /* SIL-2: 验证过滤器一致性 */
    if (filter == NULL && user_data != NULL) {
        LOG_WRN("Setting user_data without filter function");
    }

    k_mutex_lock(&g_dispatcher_api_lock, K_FOREVER);

    if (!dispatcher_is_initialized()) {
        g_dispatcher.filter = filter;
        g_dispatcher.filter_user_data = user_data;
        k_mutex_unlock(&g_dispatcher_api_lock);
        return;
    }

    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);
    g_dispatcher.filter = filter;
    g_dispatcher.filter_user_data = user_data;
    k_mutex_unlock(&g_dispatcher.lock);
    k_mutex_unlock(&g_dispatcher_api_lock);
}

/**
 * @brief 清除事件过滤器
 */
void event_dispatcher_clear_filter(void) {
    k_mutex_lock(&g_dispatcher_api_lock, K_FOREVER);

    if (!dispatcher_is_initialized()) {
        g_dispatcher.filter = NULL;
        g_dispatcher.filter_user_data = NULL;
        k_mutex_unlock(&g_dispatcher_api_lock);
        return;
    }

    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);
    g_dispatcher.filter = NULL;
    g_dispatcher.filter_user_data = NULL;
    k_mutex_unlock(&g_dispatcher.lock);
    k_mutex_unlock(&g_dispatcher_api_lock);
}

/**
 * @brief 持锁快照分发器状态与过滤器，并判定当前上下文是否允许手动消费
 *
 * 出队前后各调用一次：dequeue 可能阻塞，期间 stop() 可能已改变状态。
 * 临界区边界与拆分前一致（仅快照与判定，不含出队与回调）。
 *
 * @param state_out 状态快照
 * @param thread_started_out 分发线程存活快照
 * @param ever_started_out 本轮 init 后是否曾启动的快照
 * @param filter_out 过滤器快照
 * @param filter_user_data_out 过滤器用户数据快照
 * @return true 允许消费，false 拒绝（状态/上下文不满足）
 */
static bool process_one_state_check(dispatcher_state_t* state_out, bool* thread_started_out, bool* ever_started_out,
                                    event_filter_t* filter_out, void** filter_user_data_out) {
    bool allowed;

    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);
    *state_out = dispatcher_state_load();
    *thread_started_out = dispatcher_thread_is_started();
    *ever_started_out = g_dispatcher.ever_started;
    allowed = dispatcher_can_process_locked(*state_out, *thread_started_out, *ever_started_out);
    *filter_out = g_dispatcher.filter;
    *filter_user_data_out = g_dispatcher.filter_user_data;
    k_mutex_unlock(&g_dispatcher.lock);

    return allowed;
}

/** 出队前被拒绝时的差异化日志：区分「曾启动已停止」「分发线程独占消费」与「start 进行中」三类误用 */
static void process_one_log_reject(dispatcher_state_t state, bool thread_started, bool ever_started) {
    if (ever_started && state == DISPATCHER_STOPPED) {
        LOG_WRN("process_one rejected: dispatcher was started and is now stopped");
    } else if (state == DISPATCHER_RUNNING) {
        if (thread_started) {
            LOG_WRN("process_one rejected: dispatcher thread is consuming the queue");
        } else {
            LOG_WRN("process_one rejected: dispatcher start is in progress");
        }
    }
}

/**
 * @brief 应用过滤器并分发事件（锁外执行，统计折叠进 batch）
 *
 * 被过滤的事件计入 events_filtered（短临界区）；过滤与分发都视为消费成功。
 *
 * @return EVENT_OK 事件已消费（含被过滤）
 */
static event_status_t process_one_dispatch(event_t* event, event_filter_t filter, void* filter_user_data,
                                           batch_stats_t* batch) {
    /* 应用过滤器（如果已设置） */
    if (filter != NULL && !filter(event, filter_user_data)) {
        /* SIL-2: 使用统一接口释放动态数据，正确处理 slab 来源 */
        event_free_data(event);
        if (batch->enable_stats) {
            k_mutex_lock(&g_dispatcher.lock, K_FOREVER);
            g_dispatcher.stats.events_filtered++;
            k_mutex_unlock(&g_dispatcher.lock);
        }
        return EVENT_OK;
    }

    process_event(event, batch);

    /* SIL-2: 使用统一接口释放动态数据，正确处理 slab 来源 */
    event_free_data(event);

    return EVENT_OK;
}

/**
 * @brief 处理单个事件
 *
 * @param timeout 等待超时时间
 * @return EVENT_OK 成功，EVENT_ERR_INVALID_ARG 状态不正确，
 *         EVENT_ERR_QUEUE_EMPTY 队列为空
 */
static event_status_t event_dispatcher_process_one_impl(k_timeout_t timeout, bool manage_lifecycle,
                                                        batch_stats_t* batch) {
    batch_stats_t      local_batch;
    dispatcher_state_t state;
    bool               thread_started;
    bool               ever_started;
    event_filter_t     filter;
    void*              filter_user_data;
    bool               lifecycle_locked = false;
    event_status_t     st;
    event_t            event;

    if (batch == NULL) {
        batch = &local_batch;
        batch_stats_init(batch);
    }

    /* 外部线程手动消费须持有生命周期门闩，避免与 init/stop 并发重置控制块 */
    if (manage_lifecycle && !event_dispatcher_is_current_thread()) {
        if (!dispatcher_lifecycle_try_lock()) {
            return EVENT_ERR_TIMEOUT;
        }
        lifecycle_locked = true;
        if (K_TIMEOUT_EQ(timeout, K_FOREVER)) {
            /* 外部线程在整个阻塞期间持有 lifecycle gate：期间 dispatcher 的
             * start/stop 与 event_system_stop 都会被拒绝（TIMEOUT）。请改用有限超时。 */
            LOG_WRN("process_one(K_FOREVER) from external thread blocks dispatcher lifecycle ops until it returns");
        }
    }

    if (!dispatcher_is_initialized()) {
        st = EVENT_ERR_INVALID_ARG;
        goto out;
    }

    /* SIL-2: 出队前在持有锁的情况下快照状态并检查 */
    if (!process_one_state_check(&state, &thread_started, &ever_started, &filter, &filter_user_data)) {
        process_one_log_reject(state, thread_started, ever_started);
        st = EVENT_ERR_INVALID_ARG;
        goto out;
    }

#if defined(CONFIG_EVENT_PRIORITY_DISPATCH)
    /* 优先级有序消费：队列非空时经批出队取有效优先级最优的一条（max_out=1）；
     * 队列为空时回退阻塞式单条出队以保留 timeout 语义（等待期间无排序对象，
     * 新到事件按到达序取出，不破坏既有阻塞行为的可观测契约） */
    if (event_queue_dequeue_priority_batch(g_event_queue, &event, 1U, NULL) == 1U) {
        st = EVENT_OK;
    } else {
        st = event_queue_dequeue(g_event_queue, &event, timeout);
    }
#else
    st = event_queue_dequeue(g_event_queue, &event, timeout);
#endif
    if (st != EVENT_OK) {
        goto out;
    }

    /* SIL-2: 阻塞期间状态可能已改变（如 stop() 被调用），出队后复核 */
    if (!process_one_state_check(&state, &thread_started, &ever_started, &filter, &filter_user_data)) {
        event_free_data(&event);
        st = EVENT_ERR_INVALID_ARG;
        goto out;
    }

    (void) process_one_dispatch(&event, filter, filter_user_data, batch);
    st = EVENT_OK;

    if (batch == &local_batch) {
        dispatcher_commit_batch(batch, 1U);
    }

out:
    if (lifecycle_locked) {
        dispatcher_lifecycle_unlock();
    }
    return st;
}

event_status_t event_dispatcher_process_one(k_timeout_t timeout) {
    return event_dispatcher_process_one_impl(timeout, true, NULL);
}

/**
 * @brief 处理所有待处理事件
 *
 * @param max_events 最大处理事件数，0 使用配置默认值
 * @return 已处理的事件数量
 */
uint32_t event_dispatcher_process_all(uint32_t max_events) {
    bool lifecycle_locked = false;

    if (!event_dispatcher_is_current_thread()) {
        if (!dispatcher_lifecycle_try_lock()) {
            return 0U;
        }
        lifecycle_locked = true;
    }

    if (!dispatcher_is_initialized()) {
        if (lifecycle_locked) {
            dispatcher_lifecycle_unlock();
        }
        return 0U;
    }

    /* SIL-2: 验证输入参数 */
    if (max_events > EVENT_DISPATCHER_MAX_EVENTS_PER_CYCLE) {
        LOG_WRN("max_events %u exceeds limit, capping to %u", max_events, EVENT_DISPATCHER_MAX_EVENTS_PER_CYCLE);
        max_events = EVENT_DISPATCHER_MAX_EVENTS_PER_CYCLE;
    }

    /* SIL-2: config 在 event_dispatcher_init 后不再改变，
     * 无需加锁；若未来支持运行时重配置，需改为原子读取或加锁。 */
    if (max_events == 0) {
        max_events = g_dispatcher.hot_max_events_per_cycle;
    }

    uint32_t      processed = 0;
    batch_stats_t batch;
    batch_stats_init(&batch);

#if defined(CONFIG_EVENT_PRIORITY_DISPATCH)
    /* 批模式重复取批，直到队列空或达到 max_events。单批容量受队列大小限制；
     * 回调里新发布的事件由下一轮取批纳入同一次 process_all。
     * 回调若再次进入 process_all，外层仍持有 g_priority_batch，内层改逐条出队。 */
    if (g_priority_batch_depth > 0U) {
        while (processed < max_events) {
            if (event_dispatcher_process_one_impl(K_NO_WAIT, false, &batch) != EVENT_OK) {
                break;
            }
            processed++;
        }
    } else {
        dispatcher_state_t state_snapshot;
        bool               thread_started_snapshot;
        bool               ever_started_snapshot;
        event_filter_t     filter = NULL;
        void*              filter_user_data = NULL;

        g_priority_batch_depth = 1U;
        while (processed < max_events) {
            uint32_t room = max_events - processed;

            if (room > (uint32_t) CONFIG_EVENT_QUEUE_SIZE) {
                room = (uint32_t) CONFIG_EVENT_QUEUE_SIZE;
            }
            if (!process_one_state_check(&state_snapshot, &thread_started_snapshot, &ever_started_snapshot, &filter,
                                         &filter_user_data)) {
                break;
            }

            uint32_t n = event_queue_dequeue_priority_batch(g_event_queue, g_priority_batch, room, NULL);

            if (n == 0U) {
                break;
            }
            for (uint32_t i = 0U; i < n; i++) {
                (void) process_one_dispatch(&g_priority_batch[i], filter, filter_user_data, &batch);
            }
            processed += n;
        }
        g_priority_batch_depth = 0U;
    }
#else
    while (processed < max_events) {
        if (event_dispatcher_process_one_impl(K_NO_WAIT, false, &batch) != EVENT_OK) {
            break; /* 无更多事件 */
        }
        processed++;
    }
#endif

    /* 批末一次性提交：每批一次锁往返，替代 v1 的每事件一次 */
    dispatcher_commit_batch(&batch, processed);

    if (lifecycle_locked) {
        dispatcher_lifecycle_unlock();
    }
    return processed;
}

/* =============================================================================
 * 统计 API
 * ============================================================================= */

/**
 * @brief 获取分发器统计信息
 *
 * @param stats 输出：统计信息结构指针
 */
void event_dispatcher_get_stats(dispatcher_stats_t* stats) {
    if (stats == NULL) {
        return;
    }

    k_mutex_lock(&g_dispatcher_api_lock, K_FOREVER);

    if (!dispatcher_is_initialized()) {
        memset(stats, 0, sizeof(*stats));
        k_mutex_unlock(&g_dispatcher_api_lock);
        return;
    }

    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);
    *stats = g_dispatcher.stats;
    stats->events_dropped += (uint64_t) atomic_get(&g_dispatcher_events_dropped);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    /* 订阅级过滤跳过计数在 pubsub 侧原子累计，此处镜像进分发器统计 */
    stats->subscriber_filtered += (uint64_t) atomic_get(&g_event_filter_skip_count);
#endif
#if defined(CONFIG_EVENT_PRIORITY_DISPATCH)
    /* 优先级提档计数在 queue 侧批出队路径原子累计，此处镜像进分发器统计 */
    stats->aged_promotions += (uint64_t) atomic_get(&g_dispatcher_aged_promotions);
#endif
    k_mutex_unlock(&g_dispatcher.lock);
    k_mutex_unlock(&g_dispatcher_api_lock);
}

/**
 * @brief 重置分发器统计信息
 */
void event_dispatcher_reset_stats(void) {
    k_mutex_lock(&g_dispatcher_api_lock, K_FOREVER);

    if (!dispatcher_is_initialized()) {
        atomic_set(&g_dispatcher_events_dropped, 0);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
        atomic_set(&g_event_filter_skip_count, 0);
#endif
#if defined(CONFIG_EVENT_PRIORITY_DISPATCH)
        atomic_set(&g_dispatcher_aged_promotions, 0);
#endif
        k_mutex_unlock(&g_dispatcher_api_lock);
        return;
    }

    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);
    memset(&g_dispatcher.stats, 0, sizeof(g_dispatcher.stats));
    atomic_set(&g_dispatcher_events_dropped, 0);
#if defined(CONFIG_EVENT_SUBSCRIBER_FILTER_STATS)
    atomic_set(&g_event_filter_skip_count, 0);
#endif
#if defined(CONFIG_EVENT_PRIORITY_DISPATCH)
    atomic_set(&g_dispatcher_aged_promotions, 0);
#endif
    k_mutex_unlock(&g_dispatcher.lock);
    k_mutex_unlock(&g_dispatcher_api_lock);

    LOG_DBG("Dispatcher statistics reset");
}

void event_dispatcher_stats_inc_dropped(void) {
    if (!dispatcher_is_initialized()) {
        return;
    }

    atomic_inc(&g_dispatcher_events_dropped);
}

uint64_t event_dispatcher_get_aged_promotions(void) {
#if defined(CONFIG_EVENT_PRIORITY_DISPATCH)
    return (uint64_t) atomic_get(&g_dispatcher_aged_promotions);
#else
    return 0U;
#endif
}

#if defined(CONFIG_EVENT_PRIORITY_DISPATCH)
void event_dispatcher_stats_inc_aged_promotions(void) {
    if (!dispatcher_is_initialized()) {
        return;
    }

    atomic_inc(&g_dispatcher_aged_promotions);
}
#endif /* CONFIG_EVENT_PRIORITY_DISPATCH */

/**
 * @brief 获取当前事件延迟
 *
 * @return 延迟时间（微秒）
 */
uint32_t event_dispatcher_get_idle_time_us(void) {
    uint64_t last_event_time;

    k_mutex_lock(&g_dispatcher_api_lock, K_FOREVER);

    if (!dispatcher_is_initialized()) {
        k_mutex_unlock(&g_dispatcher_api_lock);
        return 0U;
    }

    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);
    last_event_time = g_dispatcher.last_event_time;
    k_mutex_unlock(&g_dispatcher.lock);
    k_mutex_unlock(&g_dispatcher_api_lock);

    return calculate_idle_time_us(last_event_time);
}

uint32_t event_dispatcher_get_current_latency(void) {
    return event_dispatcher_get_idle_time_us();
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

/**
 * @brief 分发器线程入口函数
 *
 * 主循环：RUNNING 时批量处理事件；PAUSED 时休眠不消费；STOPPED 时退出。
 * 批量处理可提高吞吐量，同时 max_events_per_cycle 限制防止饥饿其他线程。
 */
static void dispatcher_thread_func(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_DBG("Dispatcher thread running");

    while (1) {
        /* SIL-2: 防御性检查 g_event_queue（NEW-2），防止队列指针在异常情况下
         * 被清理（如未来扩展支持 dispatcher_deinit）后线程仍尝试访问。
         * 正常生命周期下不会触发，触发即代表错误的关闭顺序。 */
        if (g_event_queue == NULL) {
            LOG_ERR("Dispatcher queue is NULL, exiting thread");
            break;
        }

        dispatcher_state_t st = dispatcher_state_load();
        uint32_t           max_events = g_dispatcher.hot_max_events_per_cycle;

        /* SIL-2: 优先检查停止状态，确保快速退出 */
        if (st == DISPATCHER_STOPPED) {
            LOG_DBG("Dispatcher thread received stop signal");
            break;
        }

        if (st == DISPATCHER_PAUSED) {
            /* SIL-2: 使用命名常量代替魔法数字 */
            k_msleep(EVENT_DISPATCHER_PAUSE_SLEEP_MS);
            continue;
        }

        /* SIL-2: 批量处理事件，max_events_per_cycle 限制防止单轮处理时间过长 */
        uint32_t processed = event_dispatcher_process_all(max_events);

        /* SIL-2: 若本周期未处理任何事件，使用有限超时阻塞等待新事件，
         * 减少轮询开销同时保留停止响应能力（K_FOREVER 无法响应 STOPPED） */
        if (processed == 0) {
            event_dispatcher_process_one(K_MSEC(EVENT_DISPATCHER_IDLE_TIMEOUT_MS));
        }
    }

    /* thread_started 由 event_dispatcher_stop() 在 join 成功后清理，避免 join 完成前允许 start 复用线程控制块 */
    LOG_DBG("Dispatcher thread exiting");
}

/**
 * @brief 处理单个事件
 *
 * 调用 event_notify_subscribers 分发事件，统计折叠进 batch（无锁），
 * 由调用方在批末统一提交。stats 关闭时跳过 cycle 计时与 64 位除法。
 *
 * @param event 要处理的事件
 * @param batch 批统计累加器（调用方保证非 NULL）
 */
static void process_event(const event_t* event, batch_stats_t* batch) {
    if (event == NULL) {
        return;
    }

    const bool time_it = batch->enable_stats;
    uint64_t   start_time = time_it ? k_cycle_get_64() : 0U;

    event_status_t status = event_notify_subscribers(event);

    uint32_t latency_us = 0U;
    if (time_it) {
        uint64_t end_time = k_cycle_get_64();
        /* SIL-2: 使用安全的除法计算延迟，避免溢出；防御极端情况下的 cycle counter 回绕。
         * sys_clock_hw_cycles_per_sec() 通常为编译时常量，非零分支几乎无开销；
         * 防御性分支保留以保护新硬件移植。 */
        if (sys_clock_hw_cycles_per_sec() != 0) {
            uint64_t delta = (end_time >= start_time) ? (end_time - start_time) : 0;
            latency_us = (uint32_t) (delta * 1000000ULL / sys_clock_hw_cycles_per_sec());
        } else {
            LOG_ERR("sys_clock_hw_cycles_per_sec() returned 0");
        }
    }

    /* 「无订阅者」不是处理错误：向已注册但当前无订阅者的类型发布事件属正常情形 */
    const bool is_error = (status != EVENT_OK && status != EVENT_ERR_NO_SUBSCRIBER);
    batch_stats_fold(batch, latency_us, is_error);

    LOG_DBG("Processed event type=%u, latency=%uus", (unsigned int) event->type, latency_us);
}

/**
 * @brief 计算距上一次事件处理的空闲时间（微秒）
 *
 * MED-4: 此函数计算的是"距上一个事件处理完成过去了多久"，
 * 而非事件从创建到分发的处理延迟。若需真实的"事件创建到处理"延迟，
 * 应使用 event->timestamp（来自 event_create）而非 last_event_time。
 *
 * @param last_event_time 上一次事件处理完成的时间戳
 * @return 距上次事件处理的空闲时间（微秒）
 */
static uint32_t calculate_idle_time_us(uint64_t last_event_time) {
    uint64_t now = k_uptime_get();
    if (last_event_time > now) {
        return 0U; /* 防止时间回绕或异常时间戳导致下溢 */
    }
    uint64_t delta_ms = now - last_event_time;
    uint64_t usec = delta_ms * 1000U;

    if (usec > UINT32_MAX) {
        return UINT32_MAX;
    }

    return (uint32_t) usec;
}
