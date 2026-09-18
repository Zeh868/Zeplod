/**
 * @file test_sys_watchdog.c
 * @brief sys_watchdog 单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            喂狗时间窗魔数具名（WDT_*_MS）
 *
 */

#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>

#include <zeplod/sys_watchdog.h>
#include "ztest_sync.h"

LOG_MODULE_REGISTER(test_sys_watchdog);

/* 喂狗后时间窗参数：立即读取的上界、最小等待时长与等待轮询上限（毫秒） */
#define WDT_FEED_FRESH_MS   100U
#define WDT_MIN_ELAPSED_MS  50U
#define WDT_ELAPSED_WAIT_MS 500U

static bool wdt_at_least_50ms_since_feed(void* ctx) {
    ARG_UNUSED(ctx);
    return sys_wdt_get_time_since_feed() >= WDT_MIN_ELAPSED_MS;
}

ZTEST(sys_watchdog, test_init_start_feed_stop) {
    zassert_equal(sys_wdt_init(NULL), 0, "init 失败");
    zassert_equal(sys_wdt_start(), 0, "start 失败");
    zassert_equal(sys_wdt_feed(), 0, "feed 失败");
    zassert_equal(sys_wdt_stop(), 0, "stop 失败");
}

ZTEST(sys_watchdog, test_pause_resume) {
    zassert_equal(sys_wdt_init(NULL), 0, NULL);
    zassert_equal(sys_wdt_start(), 0, NULL);
    zassert_equal(sys_wdt_pause(), 0, NULL);
    zassert_equal(sys_wdt_resume(), 0, NULL);
    zassert_equal(sys_wdt_stop(), 0, NULL);
}

ZTEST(sys_watchdog, test_get_status) {
    wdt_status_t status;

    zassert_equal(sys_wdt_init(NULL), 0, NULL);

    /* 初始化后状态 */
    status = sys_wdt_get_status();
    zassert_true(status == WDT_STATUS_STOPPED || status == WDT_STATUS_RUNNING, "状态应合理");

    zassert_equal(sys_wdt_start(), 0, NULL);
    status = sys_wdt_get_status();
    zassert_equal(status, WDT_STATUS_RUNNING, "启动后应为 RUNNING");

    zassert_equal(sys_wdt_pause(), 0, NULL);
    status = sys_wdt_get_status();
    zassert_equal(status, WDT_STATUS_PAUSED, "暂停后应为 PAUSED");

    zassert_equal(sys_wdt_stop(), 0, NULL);
}

ZTEST(sys_watchdog, test_get_stats) {
    wdt_stats_t stats;

    zassert_equal(sys_wdt_init(NULL), 0, NULL);
    zassert_equal(sys_wdt_start(), 0, NULL);

    /* 获取初始统计 */
    sys_wdt_get_stats(&stats);
    zassert_true(stats.feed_count == 0, "初始喂狗计数应为 0");

    /* 喂狗几次 */
    sys_wdt_feed();
    sys_wdt_feed();
    sys_wdt_feed();

    /* 再次获取统计 */
    sys_wdt_get_stats(&stats);
    zassert_true(stats.feed_count >= 3, "喂狗计数应至少为 3");

    sys_wdt_stop();
}

ZTEST(sys_watchdog, test_reset_stats) {
    wdt_stats_t stats;

    zassert_equal(sys_wdt_init(NULL), 0, NULL);
    zassert_equal(sys_wdt_start(), 0, NULL);

    /* 喂狗 */
    sys_wdt_feed();
    sys_wdt_feed();

    /* 重置统计 */
    sys_wdt_reset_stats();
    sys_wdt_get_stats(&stats);
    zassert_equal(stats.feed_count, 0U, "重置后喂狗计数应为 0");
    zassert_equal(stats.warning_count, 0U, "重置后警告计数应为 0");
    zassert_equal(stats.expire_count, 0U, "重置后过期计数应为 0");

    sys_wdt_stop();
}

ZTEST(sys_watchdog, test_get_time_since_feed) {
    uint32_t time_since_feed;

    zassert_equal(sys_wdt_init(NULL), 0, NULL);
    zassert_equal(sys_wdt_start(), 0, NULL);

    /* 喂狗 */
    sys_wdt_feed();

    /* 立即获取 */
    time_since_feed = sys_wdt_get_time_since_feed();
    zassert_true(time_since_feed < WDT_FEED_FRESH_MS, "应在喂狗后短时间内");

    zassert_true(ztest_wait_until(wdt_at_least_50ms_since_feed, NULL, WDT_ELAPSED_WAIT_MS), "应至少过了 50ms");
    time_since_feed = sys_wdt_get_time_since_feed();
    zassert_true(time_since_feed >= WDT_MIN_ELAPSED_MS, "应至少过了 50ms");

    sys_wdt_stop();
}

ZTEST(sys_watchdog, test_get_time_until_expire) {
    uint32_t time_until_expire;

    zassert_equal(sys_wdt_init(NULL), 0, NULL);
    zassert_equal(sys_wdt_start(), 0, NULL);

    /* 获取超时时间（不应崩溃）*/
    time_until_expire = sys_wdt_get_time_until_expire();
    /* 应该是一个正数 */
    zassert_true(time_until_expire > 0, "超时时间应 > 0");

    sys_wdt_stop();
}

ZTEST(sys_watchdog, test_simulate_expire) {
    zassert_equal(sys_wdt_init(NULL), 0, NULL);
    zassert_equal(sys_wdt_start(), 0, NULL);

    /* 模拟过期（不应崩溃）*/
    sys_wdt_simulate_expire();

    /* 验证状态变为 EXPIRED */
    wdt_status_t status = sys_wdt_get_status();
    zassert_equal(status, WDT_STATUS_EXPIRED, "模拟过期后状态应为 EXPIRED");

    sys_wdt_stop();
}

ZTEST(sys_watchdog, test_custom_config) {
    wdt_config_t config = {.mode = WDT_MODE_SOFTWARE,
                           .timeout_ms = 1000,
                           .feed_margin_ms = 100,
                           .pre_expire_callback = NULL,
                           .callback_user_data = NULL,
                           .reset_on_expire = false,
                           .name = "test_wdt"};

    /* 使用自定义配置初始化 */
    zassert_equal(sys_wdt_init(&config), 0, "自定义配置初始化应成功");
    zassert_equal(sys_wdt_start(), 0, NULL);
    zassert_equal(sys_wdt_feed(), 0, NULL);
    zassert_equal(sys_wdt_stop(), 0, NULL);
}

ZTEST(sys_watchdog, test_set_pre_expire_callback) {
    zassert_equal(sys_wdt_init(NULL), 0, NULL);
    zassert_equal(sys_wdt_set_pre_expire_callback(NULL, NULL), 0, NULL);
    zassert_equal(sys_wdt_set_pre_expire_callback(NULL, NULL), 0, "clear callback");
    (void) sys_wdt_stop();
}

ZTEST(sys_watchdog, test_monitor_thread) {
    zassert_equal(sys_wdt_init(NULL), 0, NULL);
    zassert_equal(sys_wdt_start(), 0, NULL);

    /* 注册当前线程进行监控 */
    k_tid_t my_tid = k_current_get();
    int     ret = sys_wdt_monitor_thread(my_tid, "test_thread", 5000);
    /* 在 native_posix 上可能不支持，但不应崩溃 */
    (void) ret;

    /* 标记线程存活 */
    ret = sys_wdt_thread_alive(my_tid);
    (void) ret;

    /* 取消监控 */
    ret = sys_wdt_unmonitor_thread(my_tid);
    (void) ret;

    sys_wdt_stop();
}

ZTEST(sys_watchdog, test_hardware_watchdog_init) {
    wdt_config_t hw_config = {.mode = WDT_MODE_HARDWARE,
                              .timeout_ms = CONFIG_SYS_WATCHDOG_TIMEOUT_MS,
                              .feed_margin_ms = 1000,
                              .pre_expire_callback = NULL,
                              .callback_user_data = NULL,
                              .reset_on_expire = false,
                              .name = "hw_wdt_test"};

    /* 尝试初始化硬件看门狗 */
    int ret = sys_wdt_init(&hw_config);

#ifdef CONFIG_WATCHDOG
    /* 有硬件看门狗配置时应成功 */
    zassert_equal(ret, 0, "硬件看门狗初始化应成功");

    /* 验证模式（如果硬件不可用会降级为软件）*/
    wdt_status_t status = sys_wdt_get_status();
    zassert_true(status == WDT_STATUS_STOPPED, "初始化后应为 STOPPED");
#else
    /* 无硬件看门狗时应降级为软件模式 */
    zassert_equal(ret, 0, "降级为软件模式应成功");
#endif

    zassert_equal(sys_wdt_start(), 0, NULL);
    zassert_equal(sys_wdt_feed(), 0, NULL);
    zassert_equal(sys_wdt_stop(), 0, NULL);
}

ZTEST_SUITE(sys_watchdog, NULL, NULL, NULL, NULL, NULL);
