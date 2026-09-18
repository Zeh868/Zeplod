/**
 * @file sys_watchdog.c
 * @brief 系统看门狗服务实现
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            init竞态防护与initialized原子标志、
 *                                                锁序登记、监控线程退出复位标志、魔数清理
 * 2026-09-18       1.2            zeh            复审整改：start 前有界 join 旧监控线程，消除线程自退出后控制块复用窗口
 *
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <zeplod/app_config.h>
#include <zeplod/lock_order.h>
#include <zeplod/sys_watchdog.h>

LOG_MODULE_REGISTER(sys_watchdog, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * SIL-2: 配置验证宏
 * ============================================================================= */

/* Zephyr 4.x watchdog.h 不再定义 WDT_OPT_NONE（仅 PAUSE_IN_SLEEP/PAUSE_HALTED），
 * 语义"无附加选项"即 0；此兼容宏使新旧版本均可编译硬件看门狗路径。 */
#ifndef WDT_OPT_NONE
#define WDT_OPT_NONE 0
#endif

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

/** 监控线程栈大小（字节） */
#ifndef SYS_WDT_MONITOR_STACK_SIZE
#define SYS_WDT_MONITOR_STACK_SIZE 2048
#endif

/** 监控线程优先级 */
#ifndef SYS_WDT_MONITOR_PRIORITY
#define SYS_WDT_MONITOR_PRIORITY 5
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

/** 重新初始化时静默化探测的试锁超时（毫秒）：仅为探测"有无操作在途"，非真实等待 */
#define SYS_WDT_REINIT_PROBE_TIMEOUT_MS 1

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
    K_KERNEL_STACK_MEMBER(monitor_stack, SYS_WDT_MONITOR_STACK_SIZE);
    struct k_mutex   lock;
    struct k_sem     feed_sem;
    thread_monitor_t threads[MAX_MONITORED_THREADS];
    uint32_t         thread_count;
    uint32_t         start_time;
    uint32_t         last_feed_time;
    bool             monitor_thread_started;
    bool             monitor_thread_created; /* 创建过即保持，供 start 侧 join 兜底判断 */
    bool             hw_wdt_setup_done;
    atomic_t         initialized; /* 显式初始化标志（memset 后自然清零） */
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
 * 锁辅助（锁序：登记 RESOURCE 层后加锁，解锁后出栈）
 * ============================================================================= */

static void wdt_lock(void) {
    zepl_lock_enter(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_wdt.lock);
    k_mutex_lock(&g_wdt.lock, K_FOREVER);
}

static void wdt_unlock(void) {
    k_mutex_unlock(&g_wdt.lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_wdt.lock);
}

/* =============================================================================
 * 前置声明
 * ============================================================================= */

static void wdt_monitor_thread(void* p1, void* p2, void* p3);
static void wdt_feed_internal(void);
static void wdt_expire_handler(void);

/* =============================================================================
 * 核心 API 实现
 * ============================================================================= */

#if defined(CONFIG_WATCHDOG)
/**
 * @brief 硬件看门狗设备探测与超时通道安装
 *
 * 硬件不可用或安装失败时降级为软件模式（调用前 config.mode 已填充）。
 */
static void wdt_hw_setup(void) {
    if (g_wdt.config.mode != WDT_MODE_HARDWARE && g_wdt.config.mode != WDT_MODE_DUAL) {
        return;
    }

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
    if (g_wdt.wdt_dev == NULL || !device_is_ready(g_wdt.wdt_dev)) {
        LOG_WRN("Hardware watchdog device not found or not ready, using software mode");
        g_wdt.config.mode = WDT_MODE_SOFTWARE;
        return;
    }

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
}
#else
static void wdt_hw_setup(void) {}
#endif /* CONFIG_WATCHDOG */

int sys_wdt_init(const wdt_config_t* config) {
    LOG_INF("Initializing watchdog...");

    /* 运行中/暂停/监控线程仍存活或并发操作进行中（feed/monitor 正持锁）时拒绝重新
     * 初始化：memset 会摧毁 g_wdt.lock 与线程状态，且 monitor_thread_started 被清零
     * 后再次 start 会对同一线程控制块二次 k_thread_create。短超时试锁兼作 in-use
     * 探测；BSS 零初始化的 k_mutex 在 Zephyr 中可直接加锁，首次调用同样适用。 */
    if (k_mutex_lock(&g_wdt.lock, K_MSEC(SYS_WDT_REINIT_PROBE_TIMEOUT_MS)) != 0) {
        LOG_ERR("sys_wdt_init rejected: watchdog operation in progress");
        return -EBUSY;
    }
    /* 探测锁已到手，补锁序登记 */
    zepl_lock_enter(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_wdt.lock);

    if (g_wdt.status == WDT_STATUS_RUNNING || g_wdt.status == WDT_STATUS_PAUSED || g_wdt.monitor_thread_started) {
        LOG_ERR("sys_wdt_init rejected: watchdog still active (status=%d)", g_wdt.status);
        wdt_unlock();
        return -EBUSY;
    }

    /* 此后旧锁将被 memset 摧毁：先按 enter 配对清登记，真实释放由重建的 k_mutex_init
     * 等效完成（旧锁生命周期随 memset 终止） */
    zepl_lock_exit(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_wdt.lock);

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
    wdt_hw_setup();

    /* 显式初始化标志：替代以 config.name != NULL 作为"已初始化"判据的隐式约定 */
    atomic_set(&g_wdt.initialized, 1);

    LOG_INF("Watchdog initialized: timeout=%dms, mode=%d", g_wdt.config.timeout_ms, g_wdt.config.mode);
    return 0;
}

int sys_wdt_start(void) {
    wdt_lock();

    if (g_wdt.status == WDT_STATUS_RUNNING) {
        wdt_unlock();
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
        /* 旧线程可能已自行退出（STOPPED/EXPIRED 收口复位标志）但尚未走完收尾：
         * 对仍存活的控制块二次 k_thread_create 属未定义行为，先有界汇合兜底。
         * 零值从未创建的线程不可 join，用 monitor_thread_created 区分。 */
        if (g_wdt.monitor_thread_created) {
            int join_ret = k_thread_join(&g_wdt.monitor_thread, K_MSEC(SYS_WDT_THREAD_JOIN_TIMEOUT_MS));
            if (join_ret != 0) {
                LOG_ERR("Previous monitor thread not exited (%d); retry sys_wdt_start", join_ret);
                g_wdt.status = WDT_STATUS_STOPPED;
                wdt_unlock();
                return -EIO;
            }
        }
        k_tid_t tid =
            k_thread_create(&g_wdt.monitor_thread, g_wdt.monitor_stack, K_THREAD_STACK_SIZEOF(g_wdt.monitor_stack),
                            wdt_monitor_thread, NULL, NULL, NULL, SYS_WDT_MONITOR_PRIORITY, 0, K_FOREVER);
        if (tid == NULL) {
            LOG_ERR("Failed to create watchdog monitor thread");
            g_wdt.status = WDT_STATUS_STOPPED;
            wdt_unlock();
            return -ENOMEM;
        }
        k_thread_name_set(tid, "wdt_mon");
        k_thread_start(tid);
        g_wdt.monitor_thread_created = true;
        g_wdt.monitor_thread_started = true;
    }

    wdt_unlock();

    LOG_INF("Watchdog started");
    return 0;
}

int sys_wdt_stop(void) {
    wdt_lock();

    if (g_wdt.status == WDT_STATUS_STOPPED && !g_wdt.monitor_thread_started) {
        wdt_unlock();
        return 0;
    }

    /* 硬件看门狗一旦启动通常无法停止；继续喂狗或返回错误 */
    if (g_wdt.hw_wdt_setup_done) {
        wdt_unlock();
        LOG_ERR("Cannot stop hardware watchdog once started");
        return -ENOTSUP;
    }

    /* SIL-2: 设置停止标志，让线程在下一个轮询周期（SYS_WDT_MONITOR_INTERVAL_MS）自行退出 */
    const bool need_join = g_wdt.monitor_thread_started;
    g_wdt.status = WDT_STATUS_STOPPED;
    wdt_unlock();

    if (need_join) {
        /* join 超时不得 k_thread_abort：监控线程频繁持有 g_wdt.lock，若 abort 时恰持锁，
         * 该互斥锁将永久保持锁定，后续所有 sys_wdt_* 调用死锁。保留 monitor_thread_started
         * 并返回 -EIO，调用方可稍后重试 stop 完成收尾（线程见 STOPPED 后会自行退出）。 */
        int ret = k_thread_join(&g_wdt.monitor_thread, K_MSEC(SYS_WDT_THREAD_JOIN_TIMEOUT_MS));
        if (ret != 0) {
            LOG_ERR("Watchdog monitor thread join timeout (%d); retry sys_wdt_stop to complete", ret);
            return -EIO;
        }
    }

    wdt_lock();
    g_wdt.monitor_thread_started = false;
    wdt_unlock();

    LOG_INF("Watchdog stopped");
    return 0;
}

int sys_wdt_feed(void) {
    wdt_lock();

    if (g_wdt.status != WDT_STATUS_RUNNING && g_wdt.status != WDT_STATUS_PAUSED) {
        wdt_unlock();
        return -EPERM;
    }

    wdt_feed_internal();

    wdt_unlock();
    return 0;
}

int sys_wdt_set_pre_expire_callback(sys_wdt_user_cb_t callback, void* user_data) {
    /* 显式 initialized 原子标志判据：config.name 可能由调用方传入 NULL，
     * 不能再作为"已初始化"的隐式判据 */
    if (atomic_get(&g_wdt.initialized) == 0) {
        return -EINVAL;
    }

    wdt_lock();
    g_wdt.config.pre_expire_callback = callback;
    g_wdt.config.callback_user_data = user_data;
    wdt_unlock();
    return 0;
}

int sys_wdt_pause(void) {
    wdt_lock();

    if (g_wdt.status != WDT_STATUS_RUNNING) {
        wdt_unlock();
        return -EPERM;
    }

    /* 硬件看门狗一旦启动通常无法暂停 */
    if (g_wdt.hw_wdt_setup_done) {
        wdt_unlock();
        LOG_ERR("Cannot pause hardware watchdog");
        return -ENOTSUP;
    }

    g_wdt.status = WDT_STATUS_PAUSED;
    LOG_WRN("Watchdog paused");

    wdt_unlock();
    return 0;
}

int sys_wdt_resume(void) {
    wdt_lock();

    if (g_wdt.status != WDT_STATUS_PAUSED) {
        wdt_unlock();
        return -EPERM;
    }

    g_wdt.status = WDT_STATUS_RUNNING;
    g_wdt.last_feed_time = k_uptime_get_32();
    LOG_INF("Watchdog resumed");

    wdt_unlock();
    return 0;
}

wdt_status_t sys_wdt_get_status(void) {
    /* 单读语义：wdt_status_t 为单字枚举，读取本身原子且无复合不变式需要保护；
     * 状态可能被并发迁移，返回值允许瞬时滞后（诊断用途）。刻意不加锁，
     * 以便本函数可在更广泛的上下文（含临界区内）安全调用。 */
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

    wdt_lock();

    if (g_wdt.thread_count >= MAX_MONITORED_THREADS) {
        wdt_unlock();
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

            wdt_unlock();
            LOG_INF("Monitoring thread: %s (max_idle=%dms)", g_wdt.threads[i].name, max_idle_ms);
            return 0;
        }
    }

    wdt_unlock();
    return -ENOMEM;
}

int sys_wdt_unmonitor_thread(k_tid_t thread_id) {
    if (thread_id == NULL) {
        return -EINVAL;
    }

    wdt_lock();

    for (uint32_t i = 0; i < MAX_MONITORED_THREADS; i++) {
        if (g_wdt.threads[i].is_monitored && g_wdt.threads[i].thread_id == thread_id) {
            g_wdt.threads[i].is_monitored = false;
            g_wdt.thread_count--;

            wdt_unlock();
            LOG_INF("Stopped monitoring thread: %s", g_wdt.threads[i].name);
            return 0;
        }
    }

    wdt_unlock();
    return -ENOENT;
}

int sys_wdt_thread_alive(k_tid_t thread_id) {
    if (thread_id == NULL) {
        return -EINVAL;
    }

    wdt_lock();

    for (uint32_t i = 0; i < MAX_MONITORED_THREADS; i++) {
        if (g_wdt.threads[i].is_monitored && g_wdt.threads[i].thread_id == thread_id) {
            g_wdt.threads[i].last_alive_time = k_uptime_get_32();
            wdt_unlock();
            return 0;
        }
    }

    wdt_unlock();
    return -ENOENT; /* 线程未被监控 */
}

/* =============================================================================
 * 统计与调试 API
 * ============================================================================= */

void sys_wdt_get_stats(wdt_stats_t* stats) {
    if (stats == NULL) {
        return;
    }

    wdt_lock();
    *stats = g_wdt.stats;
    wdt_unlock();
}

void sys_wdt_reset_stats(void) {
    wdt_lock();
    memset(&g_wdt.stats, 0, sizeof(g_wdt.stats));
    wdt_unlock();
}

uint32_t sys_wdt_get_time_since_feed(void) {
    wdt_lock();
    uint32_t now = k_uptime_get_32();
    uint32_t elapsed = now - g_wdt.last_feed_time;
    wdt_unlock();
    return elapsed;
}

uint32_t sys_wdt_get_time_until_expire(void) {
    wdt_lock();
    uint32_t now = k_uptime_get_32();
    uint32_t elapsed = now - g_wdt.last_feed_time;
    uint32_t timeout = g_wdt.config.timeout_ms;
    wdt_unlock();

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

/**
 * @brief 监控线程单轮检查：喂狗超时预警与到期处理
 *
 * @retval 1  正常，继续执行受监控线程检查
 * @retval 0  本轮跳过（暂停状态）
 * @retval -1 看门狗已到期（已触发过期处理），监控线程应退出
 */
static int wdt_poll_timeout_once(void) {
    wdt_lock();
    if (g_wdt.status == WDT_STATUS_STOPPED) {
        wdt_unlock();
        return -1;
    }
    if (g_wdt.status == WDT_STATUS_PAUSED) {
        wdt_unlock();
        return 0;
    }

    uint32_t          now = k_uptime_get_32();
    uint32_t          time_since_feed = now - g_wdt.last_feed_time;
    uint32_t          timeout_ms = g_wdt.config.timeout_ms;
    uint32_t          feed_margin_ms = g_wdt.config.feed_margin_ms;
    uint32_t          expire_threshold = (timeout_ms > feed_margin_ms) ? (timeout_ms - feed_margin_ms) : timeout_ms;
    sys_wdt_user_cb_t pre_cb = g_wdt.config.pre_expire_callback;
    void*             cb_ud = g_wdt.config.callback_user_data;

    if (time_since_feed < expire_threshold) {
        wdt_unlock();
        return 1;
    }

    g_wdt.stats.warning_count++;
    wdt_unlock();

    LOG_ERR("Watchdog critical: %ums since last feed (expire at %ums)", time_since_feed, timeout_ms);

    if (pre_cb != NULL) {
        pre_cb(cb_ud);
    }

    wdt_lock();
    bool expired = false;
    if (g_wdt.status == WDT_STATUS_RUNNING) {
        now = k_uptime_get_32();
        if ((now - g_wdt.last_feed_time) >= timeout_ms) {
            expired = true;
        }
        /* 不再自动喂狗：应用必须主动喂狗，否则下次循环将真正超时 */
    }
    wdt_unlock();

    if (expired) {
        wdt_expire_handler();
        return -1;
    }
    return 1;
}

/**
 * @brief 监控线程单轮检查：受监控线程卡死检测（需看门狗处于 RUNNING）
 */
static void wdt_poll_threads_once(void) {
    wdt_lock();
    if (g_wdt.status == WDT_STATUS_RUNNING) {
        uint32_t now = k_uptime_get_32();
        for (uint32_t i = 0; i < MAX_MONITORED_THREADS; i++) {
            if (!g_wdt.threads[i].is_monitored) {
                continue;
            }
            uint32_t idle_time = now - g_wdt.threads[i].last_alive_time;
            if (idle_time > g_wdt.threads[i].max_idle_ms) {
                LOG_ERR("Thread '%s' appears stuck: idle for %ums (max: %ums)", g_wdt.threads[i].name, idle_time,
                        g_wdt.threads[i].max_idle_ms);
                /* expire_count 语义区分：真超时计入 wdt_expire_handler（软件看门狗
                 * 到期，通常伴随复位）；此处复用同一计数记录"线程卡死达到阈值"，
                 * 两者都表示看门狗触发条件达成，不再拆分新字段（公开统计结构） */
                g_wdt.stats.expire_count++;
            }
        }
    }
    wdt_unlock();
}

static void wdt_monitor_thread(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("Watchdog monitor thread started");

    for (;;) {
        int poll = wdt_poll_timeout_once();
        if (poll < 0) {
            break;
        }
        if (poll > 0) {
            wdt_poll_threads_once();
        }
        k_msleep(SYS_WDT_MONITOR_INTERVAL_MS);
    }

    LOG_INF("Watchdog monitor thread stopped");

    /* 复位作为线程最后动作：STOPPED 自行退出与 EXPIRED 超时退出统一在此收口，
     * 避免 EXPIRED 路径残留 monitor_thread_started=true 导致后续 sys_wdt_init 被
     * -EBUSY 拒绝；残余的收尾窗口由 sys_wdt_start 侧的有界 join 兜底。 */
    wdt_lock();
    g_wdt.monitor_thread_started = false;
    wdt_unlock();
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

    wdt_lock();
    g_wdt.stats.expire_count++;
    g_wdt.status = WDT_STATUS_EXPIRED;
    wdt_unlock();

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

static int sys_wdt_auto_init(void) {
#if APP_CONFIG_ENABLE_WATCHDOG
    wdt_config_t wdt_config = {.mode = WDT_MODE_SOFTWARE,
                               .timeout_ms = APP_WATCHDOG_TIMEOUT_MS,
                               .feed_margin_ms = WDT_FEED_MARGIN_MS,
                               .reset_on_expire = IS_ENABLED(CONFIG_APP_WATCHDOG_RESET_ON_EXPIRE)};
    int ret = sys_wdt_init(&wdt_config);
    if (ret == 0) {
        LOG_INF("Watchdog initialized");
    }
    /* 显式传播初始化失败：SYS_INIT 返回非 0 时 Zephyr 会在启动阶段报致命错误，
     * 而不是带着未初始化的看门狗继续运行 */
    return ret;
#else
    return 0;
#endif
}

SYS_INIT(sys_wdt_auto_init, POST_KERNEL, APP_INIT_PRIO_SYS_WDT);
