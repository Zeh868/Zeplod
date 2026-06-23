/**
 * @file sys_watchdog.c
 * @brief 系统看门狗服务实现
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

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <zeplod/sys_watchdog.h>

LOG_MODULE_REGISTER(sys_watchdog, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * SIL-2: 配置验证宏
 * ============================================================================= */

/** 看门狗线程join超时 (毫秒) */
#ifndef SYS_WDT_THREAD_JOIN_TIMEOUT_MS
#define SYS_WDT_THREAD_JOIN_TIMEOUT_MS 500U
#endif

/** 监控循环检查间隔 (毫秒) */
#ifndef SYS_WDT_MONITOR_INTERVAL_MS
#define SYS_WDT_MONITOR_INTERVAL_MS 100U
#endif

/** 喂狗提前量最小值 (毫秒) */
#ifndef SYS_WDT_MIN_FEED_MARGIN_MS
#define SYS_WDT_MIN_FEED_MARGIN_MS 100U
#endif

/** 线程名称最大长度 */
#ifndef SYS_WDT_THREAD_NAME_MAX_LEN
#define SYS_WDT_THREAD_NAME_MAX_LEN 31U
#endif

/* =============================================================================
 * 内部定义
 * ============================================================================= */

#ifndef CONFIG_SYS_WATCHDOG_TIMEOUT_MS
#define CONFIG_SYS_WATCHDOG_TIMEOUT_MS 5000
#endif

#ifndef CONFIG_MAX_MODULES
#define CONFIG_MAX_MODULES 16
#endif

#define MAX_MONITORED_THREADS CONFIG_MAX_MODULES
#define WDT_FEED_MARGIN_MS    1000 /* 在到期前至少 1 秒喂狗 */

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    k_tid_t  thread_id;
    char     name[32];
    uint32_t max_idle_ms;
    uint32_t last_alive_time;
    bool     is_monitored;
} thread_monitor_t;

/* 验证宏与结构体定义一致 */
BUILD_ASSERT(SYS_WDT_THREAD_NAME_MAX_LEN == (sizeof(((thread_monitor_t*) 0)->name) - 1),
             "SYS_WDT_THREAD_NAME_MAX_LEN mismatch");

typedef struct {
    wdt_status_t    status;
    wdt_config_t    config;
    wdt_stats_t     stats;
    struct k_thread monitor_thread;
    K_KERNEL_STACK_MEMBER(monitor_stack, 2048);
    struct k_mutex   lock;
    struct k_sem     feed_sem;
    thread_monitor_t threads[MAX_MONITORED_THREADS];
    uint32_t         thread_count;
    uint32_t         start_time;
    uint32_t         last_feed_time;
    bool             monitor_thread_started;
    bool             hw_wdt_setup_done;
#ifdef CONFIG_WATCHDOG
    const struct device* wdt_dev;
    int                  wdt_channel;
#endif
} wdt_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static wdt_cb_t g_wdt;

/* =============================================================================
 * 前置声明
 * ============================================================================= */

static void wdt_monitor_thread(void* p1, void* p2, void* p3);
static void wdt_feed_internal(void);
static void wdt_expire_handler(void);

/* =============================================================================
 * 核心 API 实现
 * ============================================================================= */

int sys_wdt_init(const wdt_config_t* config) {
    LOG_INF("Initializing watchdog...");

    memset(&g_wdt, 0, sizeof(g_wdt));

    /* 设置默认或提供的配置 */
    if (config != NULL) {
        g_wdt.config = *config;
    } else {
        /* SIL-2: 使用 Kconfig 配置的默认模式 */
#if defined(CONFIG_SYS_WATCHDOG_DEFAULT_MODE)
        g_wdt.config.mode = (wdt_mode_t) CONFIG_SYS_WATCHDOG_DEFAULT_MODE;
#else
        g_wdt.config.mode = WDT_MODE_SOFTWARE;
#endif
        g_wdt.config.timeout_ms = CONFIG_SYS_WATCHDOG_TIMEOUT_MS;
        g_wdt.config.feed_margin_ms = WDT_FEED_MARGIN_MS;
        g_wdt.config.reset_on_expire = IS_ENABLED(CONFIG_SYS_WATCHDOG_FORCE_RESET);
        g_wdt.config.name = "sys_wdt";
    }

    /* 初始化同步原语 */
    k_mutex_init(&g_wdt.lock);
    k_sem_init(&g_wdt.feed_sem, 0, 1);

    g_wdt.status = WDT_STATUS_STOPPED;
    g_wdt.start_time = k_uptime_get_32();
    g_wdt.last_feed_time = g_wdt.start_time;

    /* 如配置则初始化硬件看门狗 */
#ifdef CONFIG_WATCHDOG
    if (g_wdt.config.mode == WDT_MODE_HARDWARE || g_wdt.config.mode == WDT_MODE_DUAL) {
        /* SIL-2: 尝试获取看门狗设备 */
#if DT_NODE_EXISTS(DT_ALIAS(watchdog0))
        g_wdt.wdt_dev = DEVICE_DT_GET(DT_ALIAS(watchdog0));
#elif DT_HAS_COMPAT_STATUS_OKAY(nxp_imx_rtwdog)
        g_wdt.wdt_dev = DEVICE_DT_GET_ONE(nxp_imx_rtwdog);
#elif DT_HAS_COMPAT_STATUS_OKAY(st_stm32_watchdog)
        g_wdt.wdt_dev = DEVICE_DT_GET_ONE(st_stm32_watchdog);
#else
        g_wdt.wdt_dev = NULL;
#endif
        if (g_wdt.wdt_dev != NULL && device_is_ready(g_wdt.wdt_dev)) {
            LOG_INF("Hardware watchdog device found");
            /* SIL-2: 初始化硬件看门狗通道 */
            struct wdt_timeout_cfg wdt_config = {
                .flags = WDT_FLAG_RESET_SOC,
                .window.min = 0,
                .window.max = g_wdt.config.timeout_ms,
                .callback = NULL, /* 使用软件回调 */
            };
            g_wdt.wdt_channel = wdt_install_timeout(g_wdt.wdt_dev, &wdt_config);
            if (g_wdt.wdt_channel < 0) {
                LOG_ERR("Failed to install watchdog timeout: %d", g_wdt.wdt_channel);
                g_wdt.config.mode = WDT_MODE_SOFTWARE;
                g_wdt.wdt_channel = -1;
            } else {
                LOG_INF("Hardware watchdog channel installed: %d", g_wdt.wdt_channel);
            }
        } else {
            LOG_WRN("Hardware watchdog device not found or not ready, using software mode");
            g_wdt.config.mode = WDT_MODE_SOFTWARE;
        }
    }
#endif

    LOG_INF("Watchdog initialized: timeout=%dms, mode=%d", g_wdt.config.timeout_ms, g_wdt.config.mode);
    return 0;
}

int sys_wdt_start(void) {
    k_mutex_lock(&g_wdt.lock, K_FOREVER);

    if (g_wdt.status == WDT_STATUS_RUNNING) {
        k_mutex_unlock(&g_wdt.lock);
        return 0;
    }

    g_wdt.status = WDT_STATUS_RUNNING;
    g_wdt.last_feed_time = k_uptime_get_32();

#ifdef CONFIG_WATCHDOG
    if ((g_wdt.config.mode == WDT_MODE_HARDWARE || g_wdt.config.mode == WDT_MODE_DUAL) && g_wdt.wdt_dev != NULL &&
        g_wdt.wdt_channel >= 0 && !g_wdt.hw_wdt_setup_done) {
        const int setup_ret = wdt_setup(g_wdt.wdt_dev, WDT_OPT_NONE);
        if (setup_ret != 0) {
            LOG_ERR("Hardware watchdog setup failed: %d", setup_ret);
            g_wdt.config.mode = WDT_MODE_SOFTWARE;
            g_wdt.wdt_channel = -1;
        } else {
            g_wdt.hw_wdt_setup_done = true;
            LOG_INF("Hardware watchdog started");
        }
    }
#endif

    if (!g_wdt.monitor_thread_started) {
        k_thread_create(&g_wdt.monitor_thread, g_wdt.monitor_stack, K_THREAD_STACK_SIZEOF(g_wdt.monitor_stack),
                        wdt_monitor_thread, NULL, NULL, NULL, 5, 0, K_FOREVER);
        k_thread_name_set(&g_wdt.monitor_thread, "wdt_mon");
        k_thread_start(&g_wdt.monitor_thread);
        g_wdt.monitor_thread_started = true;
    }

    k_mutex_unlock(&g_wdt.lock);

    LOG_INF("Watchdog started");
    return 0;
}

int sys_wdt_stop(void) {
    k_mutex_lock(&g_wdt.lock, K_FOREVER);

    if (g_wdt.status == WDT_STATUS_STOPPED) {
        k_mutex_unlock(&g_wdt.lock);
        return 0;
    }

    /* 硬件看门狗一旦启动通常无法停止；继续喂狗或返回错误 */
    if (g_wdt.hw_wdt_setup_done) {
        k_mutex_unlock(&g_wdt.lock);
        LOG_ERR("Cannot stop hardware watchdog once started");
        return -ENOTSUP;
    }

    /* SIL-2: 设置停止标志，让线程自行退出 */
    g_wdt.status = WDT_STATUS_STOPPED;
    k_mutex_unlock(&g_wdt.lock);

    /* SIL-2: 给线程时间退出 */
    int ret = k_thread_join(&g_wdt.monitor_thread, K_MSEC(SYS_WDT_THREAD_JOIN_TIMEOUT_MS));
    if (ret != 0) {
        LOG_ERR("Watchdog monitor thread join timeout (%d), aborting", ret);
        k_thread_abort(&g_wdt.monitor_thread);
    }

    k_mutex_lock(&g_wdt.lock, K_FOREVER);
    g_wdt.monitor_thread_started = false;
    k_mutex_unlock(&g_wdt.lock);

    LOG_INF("Watchdog stopped");
    return 0;
}

int sys_wdt_feed(void) {
    k_mutex_lock(&g_wdt.lock, K_FOREVER);

    if (g_wdt.status != WDT_STATUS_RUNNING && g_wdt.status != WDT_STATUS_PAUSED) {
        k_mutex_unlock(&g_wdt.lock);
        return -EPERM;
    }

    wdt_feed_internal();

    k_mutex_unlock(&g_wdt.lock);
    return 0;
}

int sys_wdt_set_pre_expire_callback(sys_wdt_user_cb_t callback, void* user_data) {
    if (g_wdt.config.name == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&g_wdt.lock, K_FOREVER);
    g_wdt.config.pre_expire_callback = callback;
    g_wdt.config.callback_user_data = user_data;
    k_mutex_unlock(&g_wdt.lock);
    return 0;
}

int sys_wdt_pause(void) {
    k_mutex_lock(&g_wdt.lock, K_FOREVER);

    if (g_wdt.status != WDT_STATUS_RUNNING) {
        k_mutex_unlock(&g_wdt.lock);
        return -EPERM;
    }

    /* 硬件看门狗一旦启动通常无法暂停 */
    if (g_wdt.hw_wdt_setup_done) {
        k_mutex_unlock(&g_wdt.lock);
        LOG_ERR("Cannot pause hardware watchdog");
        return -ENOTSUP;
    }

    g_wdt.status = WDT_STATUS_PAUSED;
    LOG_WRN("Watchdog paused");

    k_mutex_unlock(&g_wdt.lock);
    return 0;
}

int sys_wdt_resume(void) {
    k_mutex_lock(&g_wdt.lock, K_FOREVER);

    if (g_wdt.status != WDT_STATUS_PAUSED) {
        k_mutex_unlock(&g_wdt.lock);
        return -EPERM;
    }

    g_wdt.status = WDT_STATUS_RUNNING;
    g_wdt.last_feed_time = k_uptime_get_32();
    LOG_INF("Watchdog resumed");

    k_mutex_unlock(&g_wdt.lock);
    return 0;
}

wdt_status_t sys_wdt_get_status(void) {
    return g_wdt.status;
}

/* =============================================================================
 * 线程监控 API
 * ============================================================================= */

int sys_wdt_monitor_thread(k_tid_t thread_id, const char* thread_name, uint32_t max_idle_ms) {
    if (thread_id == NULL || max_idle_ms == 0) {
        return -EINVAL;
    }

    /* SIL-2: 验证max_idle_ms合理性 */
    if (max_idle_ms < SYS_WDT_MONITOR_INTERVAL_MS) {
        LOG_ERR("max_idle_ms %u too small (min: %u)", max_idle_ms, SYS_WDT_MONITOR_INTERVAL_MS);
        return -EINVAL;
    }

    k_mutex_lock(&g_wdt.lock, K_FOREVER);

    if (g_wdt.thread_count >= MAX_MONITORED_THREADS) {
        k_mutex_unlock(&g_wdt.lock);
        LOG_ERR("Maximum monitored threads reached (%u)", MAX_MONITORED_THREADS);
        return -ENOMEM;
    }

    /* 查找空槽位或现有线程 */
    for (uint32_t i = 0; i < MAX_MONITORED_THREADS; i++) {
        if (!g_wdt.threads[i].is_monitored) {
            g_wdt.threads[i].thread_id = thread_id;
            /* SIL-2: 安全复制字符串，确保终止符 */
            if (thread_name != NULL) {
                strncpy(g_wdt.threads[i].name, thread_name, SYS_WDT_THREAD_NAME_MAX_LEN);
                g_wdt.threads[i].name[SYS_WDT_THREAD_NAME_MAX_LEN] = '\0';
            } else {
                strncpy(g_wdt.threads[i].name, "unknown", SYS_WDT_THREAD_NAME_MAX_LEN);
                g_wdt.threads[i].name[SYS_WDT_THREAD_NAME_MAX_LEN] = '\0';
            }
            g_wdt.threads[i].max_idle_ms = max_idle_ms;
            g_wdt.threads[i].last_alive_time = k_uptime_get_32();
            g_wdt.threads[i].is_monitored = true;
            g_wdt.thread_count++;

            k_mutex_unlock(&g_wdt.lock);
            LOG_INF("Monitoring thread: %s (max_idle=%dms)", g_wdt.threads[i].name, max_idle_ms);
            return 0;
        }
    }

    k_mutex_unlock(&g_wdt.lock);
    return -ENOMEM;
}

int sys_wdt_unmonitor_thread(k_tid_t thread_id) {
    if (thread_id == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&g_wdt.lock, K_FOREVER);

    for (uint32_t i = 0; i < MAX_MONITORED_THREADS; i++) {
        if (g_wdt.threads[i].is_monitored && g_wdt.threads[i].thread_id == thread_id) {
            g_wdt.threads[i].is_monitored = false;
            g_wdt.thread_count--;

            k_mutex_unlock(&g_wdt.lock);
            LOG_INF("Stopped monitoring thread: %s", g_wdt.threads[i].name);
            return 0;
        }
    }

    k_mutex_unlock(&g_wdt.lock);
    return -ENOENT;
}

int sys_wdt_thread_alive(k_tid_t thread_id) {
    if (thread_id == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&g_wdt.lock, K_FOREVER);

    for (uint32_t i = 0; i < MAX_MONITORED_THREADS; i++) {
        if (g_wdt.threads[i].is_monitored && g_wdt.threads[i].thread_id == thread_id) {
            g_wdt.threads[i].last_alive_time = k_uptime_get_32();
            k_mutex_unlock(&g_wdt.lock);
            return 0;
        }
    }

    k_mutex_unlock(&g_wdt.lock);
    return -ENOENT; /* 线程未被监控 */
}

/* =============================================================================
 * 统计与调试 API
 * ============================================================================= */

void sys_wdt_get_stats(wdt_stats_t* stats) {
    if (stats == NULL) {
        return;
    }

    k_mutex_lock(&g_wdt.lock, K_FOREVER);
    *stats = g_wdt.stats;
    k_mutex_unlock(&g_wdt.lock);
}

void sys_wdt_reset_stats(void) {
    k_mutex_lock(&g_wdt.lock, K_FOREVER);
    memset(&g_wdt.stats, 0, sizeof(g_wdt.stats));
    k_mutex_unlock(&g_wdt.lock);
}

uint32_t sys_wdt_get_time_since_feed(void) {
    k_mutex_lock(&g_wdt.lock, K_FOREVER);
    uint32_t now = k_uptime_get_32();
    uint32_t elapsed = now - g_wdt.last_feed_time;
    k_mutex_unlock(&g_wdt.lock);
    return elapsed;
}

uint32_t sys_wdt_get_time_until_expire(void) {
    k_mutex_lock(&g_wdt.lock, K_FOREVER);
    uint32_t now = k_uptime_get_32();
    uint32_t elapsed = now - g_wdt.last_feed_time;
    uint32_t timeout = g_wdt.config.timeout_ms;
    k_mutex_unlock(&g_wdt.lock);

    if (elapsed >= timeout) {
        return 0;
    }

    return timeout - elapsed;
}

void sys_wdt_simulate_expire(void) {
    LOG_WRN("Simulating watchdog expiration");
    wdt_expire_handler();
}

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static void wdt_monitor_thread(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("Watchdog monitor thread started");

    for (;;) {
        k_mutex_lock(&g_wdt.lock, K_FOREVER);
        if (g_wdt.status == WDT_STATUS_STOPPED) {
            k_mutex_unlock(&g_wdt.lock);
            break;
        }
        if (g_wdt.status == WDT_STATUS_PAUSED) {
            k_mutex_unlock(&g_wdt.lock);
            k_msleep(SYS_WDT_MONITOR_INTERVAL_MS);
            continue;
        }

        uint32_t          now = k_uptime_get_32();
        uint32_t          time_since_feed = now - g_wdt.last_feed_time;
        uint32_t          timeout_ms = g_wdt.config.timeout_ms;
        uint32_t          feed_margin_ms = g_wdt.config.feed_margin_ms;
        uint32_t          expire_threshold = (timeout_ms > feed_margin_ms) ? (timeout_ms - feed_margin_ms) : timeout_ms;
        sys_wdt_user_cb_t pre_cb = g_wdt.config.pre_expire_callback;
        void*             cb_ud = g_wdt.config.callback_user_data;

        if (time_since_feed >= expire_threshold) {
            g_wdt.stats.warning_count++;
            k_mutex_unlock(&g_wdt.lock);

            LOG_ERR("Watchdog critical: %ums since last feed (expire at %ums)", time_since_feed, timeout_ms);

            if (pre_cb != NULL) {
                pre_cb(cb_ud);
            }

            k_mutex_lock(&g_wdt.lock, K_FOREVER);
            if (g_wdt.status == WDT_STATUS_RUNNING) {
                now = k_uptime_get_32();
                time_since_feed = now - g_wdt.last_feed_time;
                if (time_since_feed >= timeout_ms) {
                    k_mutex_unlock(&g_wdt.lock);
                    wdt_expire_handler();
                    break;
                }
                /* 不再自动喂狗：应用必须主动喂狗，否则下次循环将真正超时 */
            }
            k_mutex_unlock(&g_wdt.lock);
        } else {
            k_mutex_unlock(&g_wdt.lock);
        }

        k_mutex_lock(&g_wdt.lock, K_FOREVER);
        if (g_wdt.status == WDT_STATUS_RUNNING) {
            now = k_uptime_get_32();
            for (uint32_t i = 0; i < MAX_MONITORED_THREADS; i++) {
                if (!g_wdt.threads[i].is_monitored) {
                    continue;
                }
                uint32_t idle_time = now - g_wdt.threads[i].last_alive_time;
                if (idle_time > g_wdt.threads[i].max_idle_ms) {
                    LOG_ERR("Thread '%s' appears stuck: idle for %ums (max: %ums)", g_wdt.threads[i].name, idle_time,
                            g_wdt.threads[i].max_idle_ms);
                    g_wdt.stats.expire_count++;
                }
            }
        }
        k_mutex_unlock(&g_wdt.lock);

        k_msleep(SYS_WDT_MONITOR_INTERVAL_MS);
    }

    LOG_INF("Watchdog monitor thread stopped");
}

/**
 * @brief 喂狗内部实现
 *
 * 调用方必须已持有 g_wdt.lock（与 sys_wdt_feed、监控线程路径一致）。
 */
static void wdt_feed_internal(void) {
    uint32_t now = k_uptime_get_32();
    uint32_t interval = now - g_wdt.last_feed_time;

    g_wdt.last_feed_time = now;
    g_wdt.stats.feed_count++;

    if (interval > g_wdt.stats.max_feed_interval_ms) {
        g_wdt.stats.max_feed_interval_ms = interval;
    }

    g_wdt.stats.last_feed_time_ms = now;

    int ret = -ENODEV;
#ifdef CONFIG_WATCHDOG
    /* SIL-2: 检查硬件喂狗是否成功 */
    if (g_wdt.wdt_dev != NULL && g_wdt.wdt_channel >= 0) {
        ret = wdt_feed(g_wdt.wdt_dev, g_wdt.wdt_channel);
        if (ret != 0) {
            LOG_ERR("Hardware watchdog feed failed: %d", ret);
            /* 调用方已持有 g_wdt.lock，禁止再次加锁 */
            g_wdt.status = WDT_STATUS_ERROR;
            g_wdt.stats.warning_count++;
        }
    }
#else
    ARG_UNUSED(ret);
#endif

    LOG_DBG("Watchdog fed (interval: %ums)", interval);
}

static void wdt_expire_handler(void) {
    LOG_ERR("Watchdog expired!");

    k_mutex_lock(&g_wdt.lock, K_FOREVER);
    g_wdt.stats.expire_count++;
    g_wdt.status = WDT_STATUS_EXPIRED;
    k_mutex_unlock(&g_wdt.lock);

    /* SIL-2: 看门狗超时后必须执行系统复位
     * CONFIG_SYS_WATCHDOG_FORCE_RESET 为 y 时无条件复位。
     * 否则由 reset_on_expire 与 CONFIG_DEBUG 共同决定（见下方分支）。
     */
#if !defined(CONFIG_SYS_WATCHDOG_FORCE_RESET)
    /* 开发模式: 允许通过配置选择是否复位,但发出严重警告 */
    if (g_wdt.config.reset_on_expire) {
        LOG_ERR("Watchdog expiration - executing system reset");
#if !IS_ENABLED(CONFIG_DEBUG)
        /* 非调试构建: 复位 */
        sys_reboot(SYS_REBOOT_COLD);
#else
        LOG_ERR("SECURITY WARNING: CONFIG_DEBUG=y — skipping reset (not for production)");
#endif
    } else {
        LOG_ERR("Watchdog expiration with reset disabled - CRITICAL SAFETY ISSUE");
    }
#else
    /* 配置启用强制复位: 生产模式执行复位 */
    LOG_ERR("Executing system reset via watchdog");
    sys_reboot(SYS_REBOOT_COLD);
#endif
}

/* =============================================================================
 * SYS_INIT 自动初始化
 * ============================================================================= */

#include <zeplod/app_config.h>

static int sys_wdt_auto_init(void) {
#if APP_CONFIG_ENABLE_WATCHDOG
    wdt_config_t wdt_config = {.mode = WDT_MODE_SOFTWARE,
                               .timeout_ms = APP_WATCHDOG_TIMEOUT_MS,
                               .feed_margin_ms = 1000,
                               .reset_on_expire = IS_ENABLED(CONFIG_APP_WATCHDOG_RESET_ON_EXPIRE)};
    sys_wdt_init(&wdt_config);
    LOG_INF("Watchdog initialized");
#endif
    return 0;
}

SYS_INIT(sys_wdt_auto_init, POST_KERNEL, APP_INIT_PRIO_SYS_WDT);
