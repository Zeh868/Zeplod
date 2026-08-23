/**
 * @file sys_timer.c
 * @brief 系统定时器服务实现
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

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <zeplod/sys_timer.h>

LOG_MODULE_REGISTER(sys_timer, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * SIL-2: 配置验证宏
 * ============================================================================= */

/** 最小定时器延迟/周期 (毫秒) */
#ifndef SYS_TIMER_MIN_DELAY_MS
#define SYS_TIMER_MIN_DELAY_MS 10U
#endif

/** 最大定时器延迟/周期 (毫秒 - 约24天) */
#ifndef SYS_TIMER_MAX_DELAY_MS
#define SYS_TIMER_MAX_DELAY_MS 2147483647U
#endif

/** 线程join超时时间 (毫秒) */
#ifndef SYS_TIMER_THREAD_JOIN_TIMEOUT_MS
#define SYS_TIMER_THREAD_JOIN_TIMEOUT_MS 500U
#endif

/** 线程重启等待时间 (毫秒) */
#ifndef SYS_TIMER_RESTART_WAIT_MS
#define SYS_TIMER_RESTART_WAIT_MS 50U
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

#define MAX_TIMERS  32
#define TIMER_MAGIC 0x544D5253 /* "TMRS" */

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

struct sys_timer {
    uint32_t           magic;
    uint32_t           index;
    bool               thread_started;
    bool               terminate; /* true: timer_thread_func 必须退出（delete） */
    sys_timer_config_t config;
    sys_timer_status_t status;
    struct k_thread    thread;
    K_KERNEL_STACK_MEMBER(stack, CONFIG_SYS_TIMER_STACK_SIZE);
    struct k_sem sem;
    uint32_t     fire_count;
    uint32_t     last_fire_time;
    uint32_t     next_fire_time;
    uint32_t     avg_latency_us;
    uint32_t     max_latency_us;
    bool         is_allocated;
};

typedef struct {
    struct sys_timer timers[MAX_TIMERS];
    uint32_t         timer_count;
    struct k_mutex   lock;
    bool             initialized;
} sys_timer_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static sys_timer_cb_t g_sys_timer;

/* =============================================================================
 * 前置声明
 * ============================================================================= */

static void timer_thread_func(void* p1, void* p2, void* p3);

/* =============================================================================
 * 核心 API 实现
 * ============================================================================= */

int sys_timer_init(void) {
    LOG_DBG("Initializing timer system...");

    /* 有存活定时器（其工作线程可能仍在运行）时拒绝重新初始化：
     * memset 会摧毁 g_sys_timer.lock 与各定时器的线程控制块/信号量。 */
    if (g_sys_timer.initialized) {
        k_mutex_lock(&g_sys_timer.lock, K_FOREVER);
        uint32_t active = g_sys_timer.timer_count;
        k_mutex_unlock(&g_sys_timer.lock);

        if (active > 0U) {
            LOG_ERR("sys_timer_init rejected: %u timer(s) still allocated", (unsigned int) active);
            return -EBUSY;
        }
    }

    memset(&g_sys_timer, 0, sizeof(g_sys_timer));
    k_mutex_init(&g_sys_timer.lock);

    for (int i = 0; i < MAX_TIMERS; i++) {
        g_sys_timer.timers[i].magic = TIMER_MAGIC;
        g_sys_timer.timers[i].is_allocated = false;
        k_sem_init(&g_sys_timer.timers[i].sem, 0, 1);
    }

    g_sys_timer.initialized = true;
    LOG_DBG("Timer system initialized");
    return 0;
}

sys_timer_handle_t sys_timer_create(const sys_timer_config_t* config) {
    if (!g_sys_timer.initialized || config == NULL) {
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
            timer->index = i;
            break;
        }
    }

    if (timer == NULL) {
        k_mutex_unlock(&g_sys_timer.lock);
        LOG_ERR("No free timer slots (max: %d)", MAX_TIMERS);
        return NULL;
    }

    /* 初始化定时器 */
    timer->config = *config;
    timer->status = SYS_TIMER_STOPPED;
    timer->fire_count = 0;
    timer->last_fire_time = 0;
    timer->next_fire_time = 0;
    timer->avg_latency_us = 0;
    timer->max_latency_us = 0;
    timer->is_allocated = true;
    timer->thread_started = false;
    timer->terminate = false;

    k_sem_init(&timer->sem, 0, 1);

    /* 创建定时器线程 */
    int priority = config->priority != 0 ? config->priority : CONFIG_SYS_TIMER_PRIORITY;

    k_tid_t tid = k_thread_create(&timer->thread, timer->stack, K_THREAD_STACK_SIZEOF(timer->stack), timer_thread_func,
                                  timer, NULL, NULL, priority, 0, K_FOREVER);
    if (tid == NULL) {
        LOG_ERR("Failed to create timer thread");
        timer->is_allocated = false;
        k_mutex_unlock(&g_sys_timer.lock);
        return NULL;
    }

    char name[16];
    snprintf(name, sizeof(name), "timer_%s", config->name != NULL ? config->name : "unk");
    k_thread_name_set(&timer->thread, name);

    g_sys_timer.timer_count++;

    k_mutex_unlock(&g_sys_timer.lock);

    LOG_DBG("Timer created: %s", config->name != NULL ? config->name : "unnamed");
    return timer;
}

int sys_timer_delete(sys_timer_handle_t timer) {
    if (timer == NULL || !g_sys_timer.initialized) {
        return -EINVAL;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (!timer->is_allocated || timer->magic != TIMER_MAGIC) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    /* 从定时器自身回调内删除自己会导致 join 自己的线程（必然超时后 abort 自己），
     * 槽位永不回收。请在回调外的线程调用 delete。 */
    if (k_current_get() == &timer->thread) {
        k_mutex_unlock(&g_sys_timer.lock);
        LOG_ERR("sys_timer_delete called from the timer's own callback thread");
        return -EDEADLK;
    }

    /* 请求工作线程退出并在 join 后回收槽位 */
    timer->terminate = true;
    timer->status = SYS_TIMER_STOPPED;
    k_sem_give(&timer->sem);

    const bool need_join = timer->thread_started;

    k_mutex_unlock(&g_sys_timer.lock);

    if (need_join) {
        int ret = k_thread_join(&timer->thread, K_MSEC(SYS_TIMER_THREAD_JOIN_TIMEOUT_MS));
        if (ret != 0) {
            LOG_ERR("Timer thread join timeout (%d), aborting", ret);
            k_thread_abort(&timer->thread);
        }
    } else {
        /* 线程从未启动（处于 suspended 状态），必须 abort 才能安全重用 k_thread 对象 */
        k_thread_abort(&timer->thread);
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (!timer->is_allocated || timer->magic != TIMER_MAGIC) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    /* 清除定时器 */
    timer->is_allocated = false;
    timer->terminate = false;
    timer->thread_started = false;
    timer->config.callback = NULL;
    timer->config.user_data = NULL;
    timer->status = SYS_TIMER_STOPPED;
    g_sys_timer.timer_count--;

    k_mutex_unlock(&g_sys_timer.lock);

    LOG_DBG("Timer deleted");
    return 0;
}

int sys_timer_start(sys_timer_handle_t timer) {
    if (timer == NULL || !g_sys_timer.initialized) {
        return -EINVAL;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (!timer->is_allocated || timer->magic != TIMER_MAGIC) {
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

    k_sem_give(&timer->sem); /* Wake up thread */

    if (!timer->thread_started) {
        k_thread_start(&timer->thread);
        timer->thread_started = true;
    }

    k_mutex_unlock(&g_sys_timer.lock);

    LOG_DBG("Timer started: %s", timer->config.name != NULL ? timer->config.name : "unnamed");
    return 0;
}

int sys_timer_stop(sys_timer_handle_t timer) {
    if (timer == NULL || !g_sys_timer.initialized) {
        return -EINVAL;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (!timer->is_allocated || timer->magic != TIMER_MAGIC) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    timer->status = SYS_TIMER_STOPPED;
    k_sem_give(&timer->sem); /* 唤醒 PARK 于 STOPPED 的工作线程 */

    k_mutex_unlock(&g_sys_timer.lock);

    LOG_DBG("Timer stopped");
    return 0;
}

int sys_timer_restart(sys_timer_handle_t timer) {
    if (timer == NULL || !g_sys_timer.initialized) {
        return -EINVAL;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (!timer->is_allocated || timer->magic != TIMER_MAGIC) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    /* 工作线程常驻：先停再启，仅重置状态与唤醒，不 join */
    timer->status = SYS_TIMER_STOPPED;
    k_sem_give(&timer->sem);

    timer->status = SYS_TIMER_RUNNING;
    timer->next_fire_time = k_uptime_get_32() + timer->config.delay_ms;
    k_sem_give(&timer->sem);

    if (!timer->thread_started) {
        k_thread_start(&timer->thread);
        timer->thread_started = true;
    }

    k_mutex_unlock(&g_sys_timer.lock);

    return 0;
}

int sys_timer_pause(sys_timer_handle_t timer) {
    if (timer == NULL || !g_sys_timer.initialized) {
        return -EINVAL;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (!timer->is_allocated || timer->magic != TIMER_MAGIC) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    if (timer->status != SYS_TIMER_RUNNING) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    timer->status = SYS_TIMER_PAUSED;
    k_sem_give(&timer->sem);

    k_mutex_unlock(&g_sys_timer.lock);

    LOG_DBG("Timer paused");
    return 0;
}

int sys_timer_resume(sys_timer_handle_t timer) {
    if (timer == NULL || !g_sys_timer.initialized) {
        return -EINVAL;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (!timer->is_allocated || timer->magic != TIMER_MAGIC) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    if (timer->status != SYS_TIMER_PAUSED) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    timer->status = SYS_TIMER_RUNNING;
    timer->next_fire_time = k_uptime_get_32() + timer->config.delay_ms;
    k_sem_give(&timer->sem);

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
    if (timer->is_allocated && timer->magic == TIMER_MAGIC) {
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

    if (!timer->is_allocated) {
        k_mutex_unlock(&g_sys_timer.lock);
        return -EINVAL;
    }

    timer->config.period_ms = period_ms;

    k_mutex_unlock(&g_sys_timer.lock);

    LOG_DBG("Timer period set to %dms", period_ms);
    return 0;
}

uint32_t sys_timer_get_time_until_expiry(sys_timer_handle_t timer) {
    uint32_t remaining = 0;

    if (timer == NULL || !g_sys_timer.initialized) {
        return 0;
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);

    if (timer->is_allocated && timer->magic == TIMER_MAGIC && timer->status == SYS_TIMER_RUNNING) {
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
    stats->miss_count = 0; /* 可实现 */
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

static void timer_thread_func(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    sys_timer_handle_t timer = (sys_timer_handle_t) p1;

    LOG_DBG("Timer thread started: %s", timer->config.name != NULL ? timer->config.name : "unnamed");

    for (;;) {
        k_mutex_lock(&g_sys_timer.lock, K_FOREVER);
        bool               should_exit = !timer->is_allocated || timer->magic != TIMER_MAGIC || timer->terminate;
        sys_timer_status_t status = timer->status;
        k_mutex_unlock(&g_sys_timer.lock);
        if (should_exit) {
            break;
        }

        if (status == SYS_TIMER_STOPPED) {
            (void) k_sem_take(&timer->sem, K_FOREVER);
            continue;
        }
        if (status == SYS_TIMER_PAUSED) {
            (void) k_sem_take(&timer->sem, K_FOREVER);
            continue;
        }
        if (status == SYS_TIMER_EXPIRED) {
            (void) k_sem_take(&timer->sem, K_FOREVER);
            continue;
        }
        if (status != SYS_TIMER_RUNNING) {
            break;
        }

        k_mutex_lock(&g_sys_timer.lock, K_FOREVER);
        uint32_t next_fire = timer->next_fire_time;
        k_mutex_unlock(&g_sys_timer.lock);

        uint32_t now = k_uptime_get_32();
        int32_t  diff = (int32_t) (next_fire - now);
        uint32_t wait_time = (diff > 0) ? (uint32_t) diff : 0U;

        if (wait_time > 0U) {
            (void) k_sem_take(&timer->sem, K_MSEC(wait_time));
        }

        k_mutex_lock(&g_sys_timer.lock, K_FOREVER);
        should_exit = !timer->is_allocated || timer->magic != TIMER_MAGIC || timer->terminate;
        status = timer->status;
        k_mutex_unlock(&g_sys_timer.lock);
        if (should_exit) {
            break;
        }
        if (status == SYS_TIMER_STOPPED || status == SYS_TIMER_PAUSED || status == SYS_TIMER_EXPIRED) {
            continue;
        }
        if (status != SYS_TIMER_RUNNING) {
            break;
        }

        now = k_uptime_get_32();
        k_mutex_lock(&g_sys_timer.lock, K_FOREVER);
        next_fire = timer->next_fire_time;
        status = timer->status;
        sys_timer_callback_t cb = timer->config.callback;
        void*                ud = timer->config.user_data;
        k_mutex_unlock(&g_sys_timer.lock);

        diff = (int32_t) (next_fire - now);
        if (diff <= 0 && status == SYS_TIMER_RUNNING) {
            /* k_uptime_get_32 为毫秒分辨率；以下为基于毫秒的粗粒度延迟估计 */
            uint32_t scheduled_time = next_fire;
            uint32_t latency_us = (diff <= 0) ? ((uint32_t) (-diff) * 1000U) : 0U;

            if (cb != NULL) {
                cb(timer, ud);
            }

            k_mutex_lock(&g_sys_timer.lock, K_FOREVER);
            /* 回调后重新验证：定时器可能已被删除或重启 */
            if (timer->is_allocated && timer->magic == TIMER_MAGIC && !timer->terminate) {
                if (timer->status == SYS_TIMER_RUNNING) {
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
                        timer->next_fire_time = scheduled_time + timer->config.period_ms;
                        int32_t nf_diff = (int32_t) (now - timer->next_fire_time);
                        if (nf_diff >= 0) {
                            uint32_t periods_behind =
                                (uint32_t) (nf_diff + timer->config.period_ms) / timer->config.period_ms;
                            timer->next_fire_time = scheduled_time + ((periods_behind + 1U) * timer->config.period_ms);
                        }
                    } else {
                        timer->status = SYS_TIMER_EXPIRED;
                        k_sem_give(&timer->sem);
                    }
                }
            }
            k_mutex_unlock(&g_sys_timer.lock);
        }
    }

    k_mutex_lock(&g_sys_timer.lock, K_FOREVER);
    if (timer->magic == TIMER_MAGIC) {
        timer->thread_started = false;
    }
    k_mutex_unlock(&g_sys_timer.lock);

    LOG_DBG("Timer thread stopped");
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
