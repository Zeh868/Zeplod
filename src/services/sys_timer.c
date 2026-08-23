/**
 * @file sys_timer.c
 * @brief 系统定时器服务实现
 * @author zeh (china_qzh@163.com)
 * @version 2.0
 * @date 2026-08-23
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布（每定时器一线程）
 * 2026-08-23       2.0            zeh            单服务线程 + k_timer 模型，
 *                                                RAM 从 32×(线程+2KB栈) 降至单栈
 *
 * 架构（v2.0）：
 * - 每定时器一个 struct k_timer（无栈，约 40B），到期回调在时钟 ISR 中只做
 *   一件事：把 {槽位号, 世代号, 计划/实际到期时刻} 投入到期队列。
 * - 唯一的定时器服务线程从到期队列取项，重新校验槽位（magic/世代/已分配/
 *   RUNNING/计划时刻未变）后在**线程上下文**执行用户回调。
 * - 槽位世代号（gen）在 delete 释放槽位时递增，队列中的陈旧项因世代/计划
 *   时刻不匹配而被跳过，槽位可立即安全复用。
 * - 到期队列打满时 ISR 侧丢弃并累计 miss_count（真实丢火计数）。
 * - 服务线程持久存活（无事件时阻塞在 k_msgq_get），重复 sys_timer_init()
 *   仅重置定时器表并清空队列。
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/time_units.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <zeplod/sys_timer.h>

LOG_MODULE_REGISTER(sys_timer, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 配置验证宏
 * ============================================================================= */

/** 最小定时器延迟/周期 (毫秒) */
#ifndef SYS_TIMER_MIN_DELAY_MS
#define SYS_TIMER_MIN_DELAY_MS 10U
#endif

/** 最大定时器延迟/周期 (毫秒 - 约24天) */
#ifndef SYS_TIMER_MAX_DELAY_MS
#define SYS_TIMER_MAX_DELAY_MS 2147483647U
#endif

/** delete 等待回调结束的超时 (毫秒) */
#ifndef SYS_TIMER_DELETE_WAIT_MS
#define SYS_TIMER_DELETE_WAIT_MS 500U
#endif

/* =============================================================================
 * 内部定义
 * ============================================================================= */

#ifndef CONFIG_SYS_TIMER_STACK_SIZE
#define CONFIG_SYS_TIMER_STACK_SIZE 2048
#endif

#ifndef CONFIG_SYS_TIMER_PRIORITY
#define CONFIG_SYS_TIMER_PRIORITY 5
#endif

#ifndef CONFIG_SYS_TIMER_MAX_TIMERS
#define CONFIG_SYS_TIMER_MAX_TIMERS 32
#endif

#define MAX_TIMERS     CONFIG_SYS_TIMER_MAX_TIMERS
#define TIMER_MAGIC    0x544D5253 /* "TMRS" */

/** 到期队列深度：最坏情况下每个定时器至多积压 2 个未处理到期项 */
#define EXPIRE_Q_DEPTH (2U * MAX_TIMERS)

BUILD_ASSERT(MAX_TIMERS <= 255, "timer slot index must fit in uint8_t");

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

struct sys_timer {
    uint32_t           magic;
    uint32_t           gen; /* 槽位世代号；delete 释放时 +1，使队列陈旧项失效 */
    sys_timer_config_t config;
    sys_timer_status_t status;
    struct k_timer     ktimer;  /* 内核定时器（无栈） */
    struct k_sem       cb_done; /* 每次回调结束后 give，供 delete 等待 */
    uint32_t           fire_count;
    uint32_t           last_fire_time;
    uint32_t           next_fire_time; /* 计划到期时刻；也用于丢弃陈旧队列项 */
    uint32_t           avg_latency_us;
    uint32_t           max_latency_us;
    atomic_t           miss_count; /* 到期队列打满时 ISR 侧丢弃计数 */
    bool               is_allocated;
    bool               cb_active;      /* 服务线程正在执行本槽位回调 */
    bool               delete_pending; /* 回调结束后由服务线程代为释放槽位 */
};

/** 到期队列项（ISR 产生，服务线程消费） */
typedef struct {
    uint32_t gen;        /* 到期时刻槽位世代号 */
    uint32_t scheduled;  /* 到期时刻的 next_fire_time 快照（等值校验用） */
    uint32_t expired_ms; /* 实际到期时刻（延迟统计基准） */
    uint8_t  idx;        /* 槽位号 */
} expire_entry_t;

typedef struct {
    struct sys_timer timers[MAX_TIMERS];
    uint32_t         timer_count;
    struct k_mutex   lock;
    bool             initialized;
    bool             svc_started; /* 服务线程已创建并启动 */
    struct k_thread  svc_thread;
    K_KERNEL_STACK_MEMBER(svc_stack, CONFIG_SYS_TIMER_STACK_SIZE);
} sys_timer_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static sys_timer_cb_t g_sys_timer;

static char          g_expire_q_buf[EXPIRE_Q_DEPTH * sizeof(expire_entry_t)] __aligned(__alignof__(expire_entry_t));
static struct k_msgq g_expire_q;

/* =============================================================================
 * 前置声明
 * ============================================================================= */

static void timer_expiry_fn(struct k_timer* ktimer);
static void timer_svc_thread_func(void* p1, void* p2, void* p3);

/* =============================================================================
 * 内部工具
 * ============================================================================= */

/** 持有 g_sys_timer.lock 时释放槽位（世代号 +1 使队列陈旧项失效） */
static void timer_free_slot_locked(struct sys_timer* timer) {
    timer->is_allocated = false;
    timer->gen++;
    timer->delete_pending = false;
    timer->config.callback = NULL;
    timer->config.user_data = NULL;
    timer->status = SYS_TIMER_STOPPED;
    g_sys_timer.timer_count--;
}

static bool timer_slot_valid(const struct sys_timer* timer) {
    return timer->magic == TIMER_MAGIC && timer->is_allocated;
}

/* =============================================================================
 * 核心 API 实现
 * ============================================================================= */

int sys_timer_init(void) {
    LOG_DBG("Initializing timer system...");

    /* 检查与重置保持在同一连续临界区内：中途释放锁会让 create() 钻空隙，
     * 导致 armed k_timer 被重置（timer_count 检查的 TOCTOU）。 */
    if (g_sys_timer.initialized) {
        k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

        if (g_sys_timer.timer_count > 0U) {
            k_mutex_unlock(&g_sys_timer.lock);
            LOG_ERR("sys_timer_init rejected: %u timer(s) still allocated", (unsigned int) g_sys_timer.timer_count);
            return -EBUSY;
        }

        /* timer_count==0：无 k_timer 在跑，服务线程只可能阻塞在空队列上 */
        k_msgq_purge(&g_expire_q);
    } else {
        memset(&g_sys_timer, 0, sizeof(g_sys_timer));
        k_mutex_init(&g_sys_timer.lock);
        k_msgq_init(&g_expire_q, g_expire_q_buf, sizeof(expire_entry_t), EXPIRE_Q_DEPTH);
        k_mutex_lock(&g_sys_timer.lock, K_FOREVER);
    }

    /* 持锁重置槽位：服务线程可能正读取陈旧到期项，持锁避免撕裂视图 */
    for (int i = 0; i < MAX_TIMERS; i++) {
        g_sys_timer.timers[i].magic = TIMER_MAGIC;
        g_sys_timer.timers[i].is_allocated = false;
        g_sys_timer.timers[i].cb_active = false;
        g_sys_timer.timers[i].delete_pending = false;
        k_sem_init(&g_sys_timer.timers[i].cb_done, 0, 1);
        k_timer_init(&g_sys_timer.timers[i].ktimer, timer_expiry_fn, NULL);
    }

    g_sys_timer.timer_count = 0;
    g_sys_timer.initialized = true;

    k_mutex_unlock(&g_sys_timer.lock);

    /* 服务线程持久存活：仅在首次 init 时创建，重复 init 复用 */
    if (!g_sys_timer.svc_started) {
        k_tid_t tid = k_thread_create(&g_sys_timer.svc_thread, g_sys_timer.svc_stack,
                                      K_THREAD_STACK_SIZEOF(g_sys_timer.svc_stack), timer_svc_thread_func, NULL, NULL,
                                      NULL, CONFIG_SYS_TIMER_PRIORITY, 0, K_FOREVER);
        if (tid == NULL) {
            LOG_ERR("Failed to create timer service thread");
            g_sys_timer.initialized = false;
            return -ENOMEM;
        }

        k_thread_name_set(tid, "sys_timer_svc");
        g_sys_timer.svc_started = true;
        k_thread_start(tid);
    }

    LOG_DBG("Timer system initialized");
    return 0;
}

sys_timer_handle_t sys_timer_create(const sys_timer_config_t* config) {
    if (!g_sys_timer.initialized || !g_sys_timer.svc_started || config == NULL) {
        LOG_ERR("Timer system not initialized or NULL config");
        return NULL;
    }

    /* SIL-2: 验证配置参数 */
    if (config->callback == NULL) {
        LOG_ERR("Timer callback is NULL");
        return NULL;
    }

    if (config->delay_ms == 0) {
        LOG_ERR("Timer delay_ms is zero");
        return NULL;
    }

    if (config->delay_ms > SYS_TIMER_MAX_DELAY_MS) {
        LOG_ERR("Timer delay_ms %u exceeds maximum %u", config->delay_ms, SYS_TIMER_MAX_DELAY_MS);
        return NULL;
    }

    if (config->delay_ms < SYS_TIMER_MIN_DELAY_MS) {
        LOG_ERR("Timer delay_ms %u below minimum %u", config->delay_ms, SYS_TIMER_MIN_DELAY_MS);
        return NULL;
    }

    if (config->mode == SYS_TIMER_PERIODIC && config->period_ms == 0) {
        LOG_ERR("Periodic timer requires non-zero period_ms");
        return NULL;
    }

    if (config->mode == SYS_TIMER_PERIODIC && config->period_ms < SYS_TIMER_MIN_DELAY_MS) {
        LOG_ERR("Timer period_ms %u below minimum %u", config->period_ms, SYS_TIMER_MIN_DELAY_MS);
        return NULL;
    }

    if (config->priority < -15 || config->priority > 15) {
        LOG_ERR("Invalid timer priority: %d (valid range: -15 to 15)", config->priority);
        return NULL;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    /* 查找空闲定时器槽位 */
    sys_timer_handle_t timer = NULL;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!g_sys_timer.timers[i].is_allocated) {
            timer = &g_sys_timer.timers[i];
            break;
        }
    }

    if (timer == NULL) {
        k_mutex_unlock(&g_sys_timer.lock);
        LOG_ERR("No free timer slots (max: %d)", MAX_TIMERS);
        return NULL;
    }

    /* 初始化定时器（v2：无每定时器线程；priority 字段保留兼容但被忽略） */
    timer->config = *config;
    timer->status = SYS_TIMER_STOPPED;
    timer->fire_count = 0;
    timer->last_fire_time = 0;
    timer->next_fire_time = 0;
    timer->avg_latency_us = 0;
    timer->max_latency_us = 0;
    atomic_set(&timer->miss_count, 0);
    timer->is_allocated = true;
    timer->cb_active = false;
    timer->delete_pending = false;

    k_timer_init(&timer->ktimer, timer_expiry_fn, NULL);
    k_sem_init(&timer->cb_done, 0, 1);

    g_sys_timer.timer_count++;

    k_mutex_unlock(&g_sys_timer.lock);

    LOG_DBG("Timer created: %s", config->name != NULL ? config->name : "unnamed");
    return timer;
}

int sys_timer_delete(sys_timer_handle_t timer) {
    if (timer == NULL || !g_sys_timer.initialized) {
        return -EINVAL;
    }

    /* 回调在服务线程执行：从回调内删除自己需等自己返回（死等），拒绝 */
    if (g_sys_timer.svc_started && k_current_get() == &g_sys_timer.svc_thread) {
        LOG_ERR("sys_timer_delete called from a timer callback (service thread)");
        return -EDEADLK;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (!timer_slot_valid(timer)) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    k_timer_stop(&timer->ktimer);
    timer->status = SYS_TIMER_STOPPED;

    if (!timer->cb_active) {
        timer_free_slot_locked(timer);
        k_mutex_unlock(&g_sys_timer.lock);
        LOG_DBG("Timer deleted");
        return 0;
    }

    /* 回调执行中：标记 delete_pending，由服务线程在回调结束后释放槽位 */
    timer->delete_pending = true;
    k_mutex_unlock(&g_sys_timer.lock);

    /* cb_done 信号量可能残留上一轮回调的令牌，须持锁复查 cb_active */
    k_timepoint_t end = sys_timepoint_calc(K_MSEC(SYS_TIMER_DELETE_WAIT_MS));
    for (;;) {
        if (k_sem_take(&timer->cb_done, sys_timepoint_timeout(end)) != 0) {
            break; /* 超时 */
        }

        k_mutex_lock(&g_sys_timer.lock, K_FOREVER);
        bool busy = timer->cb_active;
        k_mutex_unlock(&g_sys_timer.lock);
        if (!busy) {
            break;
        }
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);
    if (!timer->cb_active && timer->delete_pending && timer->is_allocated) {
        /* 服务线程已结束回调但尚未走到释放路径（或仍需在此补释放） */
        timer_free_slot_locked(timer);
        k_mutex_unlock(&g_sys_timer.lock);
        LOG_DBG("Timer deleted (after callback completion)");
        return 0;
    }
    if (!timer->is_allocated) {
        /* 服务线程已按 delete_pending 代为释放 */
        k_mutex_unlock(&g_sys_timer.lock);
        LOG_DBG("Timer deleted (freed by service thread)");
        return 0;
    }
    /* 回调超时未结束：槽位保留 delete_pending，可重试 delete */
    k_mutex_unlock(&g_sys_timer.lock);
    LOG_ERR("Timer callback still running after %u ms; delete not completed", SYS_TIMER_DELETE_WAIT_MS);
    return -EIO;
}

int sys_timer_start(sys_timer_handle_t timer) {
    if (timer == NULL || !g_sys_timer.initialized) {
        return -EINVAL;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (!timer_slot_valid(timer)) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    if (timer->status == SYS_TIMER_RUNNING) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EALREADY;
    }

    if (timer->status == SYS_TIMER_PAUSED) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    timer->status = SYS_TIMER_RUNNING;
    timer->next_fire_time = k_uptime_get_32() + timer->config.delay_ms;
    k_timer_start(&timer->ktimer, K_MSEC(timer->config.delay_ms),
                  timer->config.mode == SYS_TIMER_PERIODIC ? K_MSEC(timer->config.period_ms) : K_NO_WAIT);

    k_mutex_unlock(&g_sys_timer.lock);

    LOG_DBG("Timer started: %s", timer->config.name != NULL ? timer->config.name : "unnamed");
    return 0;
}

int sys_timer_stop(sys_timer_handle_t timer) {
    if (timer == NULL || !g_sys_timer.initialized) {
        return -EINVAL;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (!timer_slot_valid(timer)) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    /* STOPPED 状态即可丢弃队列中已到期的陈旧项（服务线程校验 status） */
    timer->status = SYS_TIMER_STOPPED;
    k_timer_stop(&timer->ktimer);

    k_mutex_unlock(&g_sys_timer.lock);

    LOG_DBG("Timer stopped");
    return 0;
}

int sys_timer_restart(sys_timer_handle_t timer) {
    if (timer == NULL || !g_sys_timer.initialized) {
        return -EINVAL;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (!timer_slot_valid(timer)) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    /* k_timer_start 对运行中的定时器等效于重新计时，无需先 stop */
    timer->status = SYS_TIMER_RUNNING;
    timer->next_fire_time = k_uptime_get_32() + timer->config.delay_ms;
    k_timer_start(&timer->ktimer, K_MSEC(timer->config.delay_ms),
                  timer->config.mode == SYS_TIMER_PERIODIC ? K_MSEC(timer->config.period_ms) : K_NO_WAIT);

    k_mutex_unlock(&g_sys_timer.lock);

    return 0;
}

int sys_timer_pause(sys_timer_handle_t timer) {
    if (timer == NULL || !g_sys_timer.initialized) {
        return -EINVAL;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (!timer_slot_valid(timer)) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    if (timer->status != SYS_TIMER_RUNNING) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    timer->status = SYS_TIMER_PAUSED;
    k_timer_stop(&timer->ktimer);

    k_mutex_unlock(&g_sys_timer.lock);

    LOG_DBG("Timer paused");
    return 0;
}

int sys_timer_resume(sys_timer_handle_t timer) {
    if (timer == NULL || !g_sys_timer.initialized) {
        return -EINVAL;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (!timer_slot_valid(timer)) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    if (timer->status != SYS_TIMER_PAUSED) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    /* 与 v1 语义一致：resume 以完整 delay 重新计时（非续走剩余时间） */
    timer->status = SYS_TIMER_RUNNING;
    timer->next_fire_time = k_uptime_get_32() + timer->config.delay_ms;
    k_timer_start(&timer->ktimer, K_MSEC(timer->config.delay_ms),
                  timer->config.mode == SYS_TIMER_PERIODIC ? K_MSEC(timer->config.period_ms) : K_NO_WAIT);

    k_mutex_unlock(&g_sys_timer.lock);

    LOG_DBG("Timer resumed");
    return 0;
}

sys_timer_status_t sys_timer_get_status(sys_timer_handle_t timer) {
    sys_timer_status_t status = SYS_TIMER_STOPPED;

    if (timer == NULL || !g_sys_timer.initialized) {
        return SYS_TIMER_STOPPED;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);
    if (timer_slot_valid(timer)) {
        status = timer->status;
    }
    k_mutex_unlock(&g_sys_timer.lock);

    return status;
}

int sys_timer_set_period(sys_timer_handle_t timer, uint32_t period_ms) {
    if (timer == NULL || period_ms == 0) {
        return -EINVAL;
    }

    if (period_ms < SYS_TIMER_MIN_DELAY_MS) {
        LOG_WRN("Period %u ms below minimum %u ms", period_ms, SYS_TIMER_MIN_DELAY_MS);
        return -EINVAL;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (!timer_slot_valid(timer)) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    timer->config.period_ms = period_ms;

    k_mutex_unlock(&g_sys_timer.lock);

    LOG_DBG("Timer period set to %ums", period_ms);
    return 0;
}

uint32_t sys_timer_get_time_until_expiry(sys_timer_handle_t timer) {
    uint32_t remaining = 0;

    if (timer == NULL || !g_sys_timer.initialized) {
        return 0;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (timer_slot_valid(timer) && timer->status == SYS_TIMER_RUNNING) {
        const uint32_t now = k_uptime_get_32();
        int32_t        diff = (int32_t) (timer->next_fire_time - now);

        if (diff > 0) {
            remaining = (uint32_t) diff;
        }
    }

    k_mutex_unlock(&g_sys_timer.lock);
    return remaining;
}

/* =============================================================================
 * 统计 API
 * ============================================================================= */

int sys_timer_get_stats(sys_timer_handle_t timer, sys_timer_stats_t* stats) {
    if (timer == NULL || stats == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (!timer->is_allocated) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    stats->fire_count = timer->fire_count;
    stats->miss_count = (uint32_t) atomic_get(&timer->miss_count);
    stats->last_fire_time_ms = timer->last_fire_time;
    stats->avg_latency_us = timer->avg_latency_us;
    stats->max_latency_us = timer->max_latency_us;

    k_mutex_unlock(&g_sys_timer.lock);
    return 0;
}

int sys_timer_reset_stats(sys_timer_handle_t timer) {
    if (timer == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (timer->is_allocated) {
        timer->fire_count = 0;
        timer->last_fire_time = 0;
        timer->avg_latency_us = 0;
        timer->max_latency_us = 0;
        atomic_set(&timer->miss_count, 0);
    }

    k_mutex_unlock(&g_sys_timer.lock);
    return 0;
}

/* =============================================================================
 * 便捷函数
 * ============================================================================= */

sys_timer_handle_t sys_timer_oneshot(uint32_t delay_ms, sys_timer_callback_t callback, void* user_data) {
    sys_timer_config_t config = {.mode = SYS_TIMER_ONESHOT,
                                 .delay_ms = delay_ms,
                                 .period_ms = 0,
                                 .callback = callback,
                                 .user_data = user_data,
                                 .name = "oneshot",
                                 .priority = 0};

    sys_timer_handle_t timer = sys_timer_create(&config);
    if (timer != NULL) {
        sys_timer_start(timer);
    }
    return timer;
}

sys_timer_handle_t sys_timer_periodic(uint32_t period_ms, sys_timer_callback_t callback, void* user_data) {
    sys_timer_config_t config = {.mode = SYS_TIMER_PERIODIC,
                                 .delay_ms = period_ms,
                                 .period_ms = period_ms,
                                 .callback = callback,
                                 .user_data = user_data,
                                 .name = "periodic",
                                 .priority = 0};

    sys_timer_handle_t timer = sys_timer_create(&config);
    if (timer != NULL) {
        sys_timer_start(timer);
    }
    return timer;
}

void sys_timer_sleep(uint32_t ms) {
    k_msleep(ms);
}

uint32_t sys_timer_get_uptime(void) {
    return k_uptime_get_32();
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

/** k_timer 到期（时钟 ISR 上下文）：只入队，不做任何重活 */
static void timer_expiry_fn(struct k_timer* ktimer) {
    struct sys_timer* timer = CONTAINER_OF(ktimer, struct sys_timer, ktimer);

    expire_entry_t entry = {
        .gen = timer->gen, /* u32 对齐加载，ISR 安全 */
        .scheduled = timer->next_fire_time,
        .expired_ms = k_uptime_get_32(),
        .idx = (uint8_t) (timer - g_sys_timer.timers),
    };

    if (k_msgq_put(&g_expire_q, &entry, K_NO_WAIT) != 0) {
        atomic_inc(&timer->miss_count);
    }
}

/** 定时器服务线程：校验到期项后在线程上下文执行回调 */
static void timer_svc_thread_func(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    expire_entry_t entry;

    LOG_DBG("Timer service thread started");

    for (;;) {
        if (k_msgq_get(&g_expire_q, &entry, K_FOREVER) != 0) {
            continue;
        }

        struct sys_timer* timer = &g_sys_timer.timers[entry.idx];

        k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

        /* 全量重校验：世代号挡陈旧项（delete 后槽位复用），scheduled 等值挡
         * stop/start 之间遗留的到期项，status 挡已停/暂停的定时器 */
        bool                 valid = timer_slot_valid(timer) && !timer->delete_pending && timer->gen == entry.gen &&
                                     timer->status == SYS_TIMER_RUNNING && timer->next_fire_time == entry.scheduled;
        sys_timer_callback_t cb = NULL;
        void*                ud = NULL;
        uint32_t             scheduled = 0U;

        if (valid) {
            cb = timer->config.callback;
            ud = timer->config.user_data;
            scheduled = timer->next_fire_time;
            timer->cb_active = true;
        }

        k_mutex_unlock(&g_sys_timer.lock);

        if (!valid) {
            continue;
        }

        uint32_t now = k_uptime_get_32();
        /* k_uptime_get_32 为毫秒分辨率；以下为基于毫秒的粗粒度延迟估计 */
        uint32_t latency_us = (now >= entry.expired_ms) ? ((now - entry.expired_ms) * 1000U) : 0U;

        if (cb != NULL) {
            cb(timer, ud);
        }

        k_mutex_lock(&g_sys_timer.lock, K_FOREVER);
        timer->cb_active = false;

        if (timer->delete_pending && timer_slot_valid(timer)) {
            /* delete 正在等回调结束：代为释放槽位 */
            timer_free_slot_locked(timer);
        } else if (timer_slot_valid(timer) && timer->gen == entry.gen && timer->status == SYS_TIMER_RUNNING) {
            timer->fire_count++;
            timer->last_fire_time = now;

            if (timer->fire_count == 1U) {
                timer->avg_latency_us = latency_us;
                timer->max_latency_us = latency_us;
            } else {
                if (latency_us > timer->max_latency_us) {
                    timer->max_latency_us = latency_us;
                }

                uint64_t total = ((uint64_t) timer->avg_latency_us * (timer->fire_count - 1U)) + latency_us;
                timer->avg_latency_us = (uint32_t) (total / timer->fire_count);
            }

            if (timer->config.mode == SYS_TIMER_PERIODIC) {
                uint32_t period = timer->config.period_ms;
                timer->next_fire_time = scheduled + period;
                int32_t nf_diff = (int32_t) (now - timer->next_fire_time);
                if (nf_diff >= 0) {
                    uint32_t periods_behind = ((uint32_t) nf_diff + period) / period;
                    timer->next_fire_time = scheduled + ((periods_behind + 1U) * period);
                }
            } else {
                timer->status = SYS_TIMER_EXPIRED;
            }
        }

        k_mutex_unlock(&g_sys_timer.lock);

        k_sem_give(&timer->cb_done);
    }
}

/* =============================================================================
 * SYS_INIT 自动初始化
 * ============================================================================= */

#include <zeplod/app_config.h>

static int sys_timer_auto_init(void) {
#if APP_CONFIG_ENABLE_TIMER_SVC
    sys_timer_init();
#endif
    return 0;
}

SYS_INIT(sys_timer_auto_init, POST_KERNEL, APP_INIT_PRIO_SYS_TIMER);
