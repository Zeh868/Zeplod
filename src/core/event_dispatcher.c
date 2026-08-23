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
 * @version 1.0
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-05-09       1.0            zeh            文档注释修订
 * 2026-05-28       1.1            zeh            热路径：state 原子化，config 快照无锁读（P3.3）
 *
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/time_units.h>
#include <zeplod/event_dispatcher.h>
#include "event_queue.h"

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
 * 全局 stats 的唯一写者是分发线程或持 lifecycle gate 的手动消费者，
 * 种子读取无需加锁。
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

static void batch_stats_fold(batch_stats_t* b, uint32_t latency_us, bool error) {
    b->count++;
    if (error) {
        b->errors++;
    }
    if (latency_us > b->max_latency_us) {
        b->max_latency_us = latency_us;
    }

    if (!b->stats_seeded) {
        if (g_dispatcher.stats.events_processed == 0U) {
            b->avg_latency_us = latency_us; /* 全局首个事件：直接种子（v1 语义） */
        } else {
            b->avg_latency_us = (uint32_t) (((uint64_t) g_dispatcher.stats.avg_latency_us * 7 + latency_us) / 8);
        }
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
 * @pre 已持有 g_dispatcher.lock
 */
static bool dispatcher_can_process_locked(dispatcher_state_t state, bool thread_started, bool ever_started) {
    if (state == DISPATCHER_PAUSED) {
        return false;
    }

    if (state == DISPATCHER_RUNNING) {
        if (thread_started && k_current_get() != &g_dispatcher.thread) {
            return false;
        }
        return true;
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
        k_mutex_unlock(&g_dispatcher_api_lock);
        dispatcher_lifecycle_unlock();
        return EVENT_ERR_INVALID_ARG;
    }

    if (config != NULL) {
        event_status_t vret = event_dispatcher_validate_config(config);
        if (vret != EVENT_OK) {
            k_mutex_unlock(&g_dispatcher_api_lock);
            dispatcher_lifecycle_unlock();
            return vret;
        }
    }

    /* LOW-5: memset 会清除已设置的 filter 与 user_data，导致 stop->init 序列后
     * 用户先前注册的过滤器静默失效。在重置前保存，重置后恢复以保持幂等性；
     * 如需主动清除过滤器，调用方应显式 event_dispatcher_clear_filter()。 */
    event_filter_t saved_filter = g_dispatcher.filter;
    void*          saved_filter_ud = g_dispatcher.filter_user_data;

    atomic_set(&g_dispatcher_initialized, 0);
    memset(&g_dispatcher, 0, sizeof(g_dispatcher));

    g_dispatcher.filter = saved_filter;
    g_dispatcher.filter_user_data = saved_filter_ud;

    /* 设置默认配置或用户提供的配置 */
    if (config != NULL) {
        g_dispatcher.config = *config;
        if (config->thread_name == NULL) {
            g_dispatcher.config.thread_name = "event_disp";
            LOG_WRN("Thread name is NULL, using default 'event_disp'");
        }
    } else {
        g_dispatcher.config.stack_size = DEFAULT_STACK_SIZE;
        g_dispatcher.config.priority = DEFAULT_PRIORITY;
        g_dispatcher.config.thread_name = "event_disp";
        g_dispatcher.config.enable_stats = true;
        g_dispatcher.config.max_events_per_cycle = DEFAULT_MAX_EVENTS_CYCLE;
    }

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
        k_mutex_unlock(&g_dispatcher_api_lock);
        dispatcher_lifecycle_unlock();
        return EVENT_ERR_INVALID_ARG;
    }

    atomic_set(&g_dispatcher_events_dropped, 0);
    atomic_set(&g_dispatcher_initialized, 1);

    k_mutex_unlock(&g_dispatcher_api_lock);
    dispatcher_lifecycle_unlock();

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
            should_join = true;
            join_gen = g_dispatcher.thread_gen;
            k_mutex_unlock(&g_dispatcher.lock);
            LOG_WRN("Dispatcher stop/join already in progress, retrying join");
            goto join_thread;
        }
        k_mutex_unlock(&g_dispatcher.lock);
        return EVENT_OK;
    }

    /* SIL-2: 设置停止状态，线程会在下次循环检查时退出 */
    dispatcher_set_state_locked(DISPATCHER_STOPPED);
    should_join = dispatcher_thread_is_started();
    join_gen = g_dispatcher.thread_gen;
    k_mutex_unlock(&g_dispatcher.lock);

    /* SIL-2: 等待线程退出，使用有限超时 */
join_thread:
    if (should_join) {
        int jret = k_thread_join(&g_dispatcher.thread, K_MSEC(EVENT_DISPATCHER_THREAD_JOIN_TIMEOUT_MS));

        if (jret != 0) {
            LOG_ERR("Dispatcher thread join timeout/failed: %d (timeout=%u ms)", jret,
                    EVENT_DISPATCHER_THREAD_JOIN_TIMEOUT_MS);
            /* Keep thread_started=true so later stop/deinit can retry join and start() cannot create
             * a second consumer while the original thread may still own an event payload.
             */
            return EVENT_ERR_TIMEOUT;
        } else {
            LOG_DBG("Dispatcher thread joined successfully");
        }
    }

    /* SIL-2: join 成功后清理状态；若 stop 期间另有 start 创建新线程则保留 thread_started */
    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);
    dispatcher_apply_thread_stopped_locked(join_gen);
    k_mutex_unlock(&g_dispatcher.lock);

    LOG_DBG("Event dispatcher stopped");
    return EVENT_OK;
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
 * @brief 处理单个事件
 *
 * @param timeout 等待超时时间
 * @return EVENT_OK 成功，EVENT_ERR_INVALID_ARG 状态不正确，
 *         EVENT_ERR_QUEUE_EMPTY 队列为空
 */
static event_status_t event_dispatcher_process_one_impl(k_timeout_t timeout, bool manage_lifecycle,
                                                        batch_stats_t* batch) {
    dispatcher_state_t state;
    bool               thread_started;
    bool               ever_started;
    event_filter_t     filter;
    void*              filter_user_data;
    bool               lifecycle_locked = false;
    batch_stats_t      local_batch;

    if (batch == NULL) {
        batch = &local_batch;
        batch_stats_init(batch);
    }

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
        if (lifecycle_locked) {
            dispatcher_lifecycle_unlock();
        }
        return EVENT_ERR_INVALID_ARG;
    }

    /* SIL-2: 在持有锁的情况下读取所有需要的状态并检查 */
    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);
    state = dispatcher_state_load();
    thread_started = dispatcher_thread_is_started();
    ever_started = g_dispatcher.ever_started;

    if (!dispatcher_can_process_locked(state, thread_started, ever_started)) {
        k_mutex_unlock(&g_dispatcher.lock);
        if (ever_started && state == DISPATCHER_STOPPED) {
            LOG_WRN("process_one rejected: dispatcher was started and is now stopped");
        } else if (thread_started && state == DISPATCHER_RUNNING) {
            LOG_WRN("process_one rejected: dispatcher thread is consuming the queue");
        }
        if (lifecycle_locked) {
            dispatcher_lifecycle_unlock();
        }
        return EVENT_ERR_INVALID_ARG;
    }

    k_mutex_unlock(&g_dispatcher.lock);

    event_t        event;
    event_status_t dq_st = event_queue_dequeue(g_event_queue, &event, timeout);
    if (dq_st != EVENT_OK) {
        if (lifecycle_locked) {
            dispatcher_lifecycle_unlock();
        }
        return dq_st;
    }

    /* SIL-2: 阻塞期间状态可能已改变（如 stop() 被调用），重新检查 */
    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);
    state = dispatcher_state_load();
    thread_started = dispatcher_thread_is_started();
    ever_started = g_dispatcher.ever_started;
    if (!dispatcher_can_process_locked(state, thread_started, ever_started)) {
        k_mutex_unlock(&g_dispatcher.lock);
        event_free_data(&event);
        if (lifecycle_locked) {
            dispatcher_lifecycle_unlock();
        }
        return EVENT_ERR_INVALID_ARG;
    }
    filter = g_dispatcher.filter;
    filter_user_data = g_dispatcher.filter_user_data;
    k_mutex_unlock(&g_dispatcher.lock);

    /* 应用过滤器（如果已设置） */
    if (filter != NULL) {
        if (!filter(&event, filter_user_data)) {
            /* SIL-2: 使用统一接口释放动态数据，正确处理 slab 来源 */
            event_free_data(&event);
            if (batch->enable_stats) {
                k_mutex_lock(&g_dispatcher.lock, K_FOREVER);
                g_dispatcher.stats.events_filtered++;
                k_mutex_unlock(&g_dispatcher.lock);
            }
            if (lifecycle_locked) {
                dispatcher_lifecycle_unlock();
            }
            return EVENT_OK;
        }
    }

    process_event(&event, batch);

    /* SIL-2: 使用统一接口释放动态数据，正确处理 slab 来源 */
    event_free_data(&event);

    if (batch == &local_batch) {
        dispatcher_commit_batch(batch, 1U);
    }

    if (lifecycle_locked) {
        dispatcher_lifecycle_unlock();
    }
    return EVENT_OK;
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

    while (processed < max_events) {
        if (event_dispatcher_process_one_impl(K_NO_WAIT, false, &batch) != EVENT_OK) {
            break; /* 无更多事件 */
        }
        processed++;
    }

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
        k_mutex_unlock(&g_dispatcher_api_lock);
        return;
    }

    k_mutex_lock(&g_dispatcher.lock, K_FOREVER);
    memset(&g_dispatcher.stats, 0, sizeof(g_dispatcher.stats));
    atomic_set(&g_dispatcher_events_dropped, 0);
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
