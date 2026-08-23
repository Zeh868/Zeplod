/**
 * @file test_sys_timer.c
 * @brief sys_timer 单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-04-01
 *
 * Zehao Qian
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 *
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>
#include <zeplod/sys_timer.h>
#include "ztest_sync.h"

LOG_MODULE_REGISTER(test_sys_timer);

static void timer_cb(sys_timer_handle_t timer, void* user_data) {
    (void) timer;
    (void) user_data;
}

/* 触发计数回调：user_data 指向 atomic_t 计数器 */
static void count_cb(sys_timer_handle_t timer, void* user_data) {
    (void) timer;
    atomic_inc((atomic_t*) user_data);
}

static atomic_t g_fire_count;

/* 回调内删除自身：应返回 -EDEADLK 并置标志 */
static atomic_t g_deadlk_seen;
static void     self_delete_cb(sys_timer_handle_t timer, void* user_data) {
    (void) user_data;
    if (sys_timer_delete(timer) == -EDEADLK) {
        atomic_set(&g_deadlk_seen, 1);
    }
}

static bool fired_at_least(void* want) {
    return atomic_get(&g_fire_count) >= (atomic_val_t) (intptr_t) want;
}

static bool deadlk_flag_set(void* ctx) {
    (void) ctx;
    return atomic_get(&g_deadlk_seen) != 0;
}

ZTEST(sys_timer, test_init) {
    zassert_equal(sys_timer_init(), 0, "sys_timer_init 失败");
}

ZTEST(sys_timer, test_create_null_config) {
    zassert_equal(sys_timer_init(), 0, NULL);
    zassert_is_null(sys_timer_create(NULL), "NULL config 应返回 NULL");
}

ZTEST(sys_timer, test_create_and_delete) {
    sys_timer_config_t cfg = {
        .mode = SYS_TIMER_ONESHOT,
        .delay_ms = 200U,
        .period_ms = 0U,
        .callback = timer_cb,
        .user_data = NULL,
        .name = "ut",
        .priority = 5,
    };
    sys_timer_handle_t t;

    zassert_equal(sys_timer_init(), 0, NULL);
    t = sys_timer_create(&cfg);
    zassert_not_null(t, "create 失败");
    zassert_equal(sys_timer_delete(t), 0, "delete 失败");
}

ZTEST(sys_timer, test_delete_null) {
    zassert_equal(sys_timer_init(), 0, NULL);
    zassert_equal(sys_timer_delete(NULL), -EINVAL, "NULL handle 应失败");
}

ZTEST(sys_timer, test_stop_then_start) {
    sys_timer_config_t cfg = {
        .mode = SYS_TIMER_PERIODIC,
        .delay_ms = 50U,
        .period_ms = 50U,
        .callback = timer_cb,
        .user_data = NULL,
        .name = "st",
        .priority = 5,
    };
    sys_timer_handle_t t;

    zassert_equal(sys_timer_init(), 0, NULL);
    t = sys_timer_create(&cfg);
    zassert_not_null(t, NULL);
    zassert_equal(sys_timer_start(t), 0, NULL);
    zassert_equal(sys_timer_stop(t), 0, NULL);
    zassert_equal(sys_timer_start(t), 0, "stop 后应能再次 start");
    zassert_equal(sys_timer_stop(t), 0, NULL);
    zassert_equal(sys_timer_delete(t), 0, NULL);
}

ZTEST(sys_timer, test_pause_resume) {
    sys_timer_config_t cfg = {
        .mode = SYS_TIMER_PERIODIC,
        .delay_ms = 50U,
        .period_ms = 50U,
        .callback = timer_cb,
        .user_data = NULL,
        .name = "pr",
        .priority = 5,
    };
    sys_timer_handle_t t;

    zassert_equal(sys_timer_init(), 0, NULL);
    t = sys_timer_create(&cfg);
    zassert_not_null(t, NULL);
    zassert_equal(sys_timer_start(t), 0, NULL);
    zassert_equal(sys_timer_pause(t), 0, NULL);
    zassert_equal(sys_timer_resume(t), 0, NULL);
    zassert_equal(sys_timer_stop(t), 0, NULL);
    zassert_equal(sys_timer_delete(t), 0, NULL);
}

ZTEST(sys_timer, test_periodic_fires) {
    sys_timer_config_t cfg = {
        .mode = SYS_TIMER_PERIODIC,
        .delay_ms = 50U,
        .period_ms = 50U,
        .callback = count_cb,
        .user_data = &g_fire_count,
        .name = "pf",
        .priority = 0,
    };
    sys_timer_handle_t t;

    atomic_set(&g_fire_count, 0);
    zassert_equal(sys_timer_init(), 0, NULL);

    t = sys_timer_create(&cfg);
    zassert_not_null(t, NULL);
    zassert_equal(sys_timer_start(t), 0, NULL);

    zassert_true(ztest_wait_until(fired_at_least, (void*) 3, 2000U), "周期定时器应在 2s 内至少触发 3 次");

    zassert_equal(sys_timer_stop(t), 0, NULL);
    zassert_equal(sys_timer_delete(t), 0, NULL);
}

ZTEST(sys_timer, test_oneshot_fires_once) {
    atomic_set(&g_fire_count, 0);
    zassert_equal(sys_timer_init(), 0, NULL);

    sys_timer_handle_t t = sys_timer_oneshot(50U, count_cb, &g_fire_count);
    zassert_not_null(t, NULL);

    zassert_true(ztest_wait_until(fired_at_least, (void*) 1, 2000U), "单次定时器应触发");

    /* 触发后再等 3 个周期时长，确保不会重复触发 */
    k_msleep(150U);
    zassert_equal(atomic_get(&g_fire_count), 1, "单次定时器只应触发一次");
    zassert_equal(sys_timer_get_status(t), SYS_TIMER_EXPIRED, "触发后状态应为 EXPIRED");

    zassert_equal(sys_timer_delete(t), 0, NULL);
}

ZTEST(sys_timer, test_stats_fire_count) {
    sys_timer_stats_t stats;

    atomic_set(&g_fire_count, 0);
    zassert_equal(sys_timer_init(), 0, NULL);

    sys_timer_handle_t t = sys_timer_periodic(50U, count_cb, &g_fire_count);
    zassert_not_null(t, NULL);

    zassert_true(ztest_wait_until(fired_at_least, (void*) 2, 2000U), "应至少触发 2 次");

    zassert_equal(sys_timer_get_stats(t, &stats), 0, NULL);
    zassert_true(stats.fire_count >= 2U, "fire_count 应 >= 2");

    zassert_equal(sys_timer_reset_stats(t), 0, NULL);
    zassert_equal(sys_timer_get_stats(t, &stats), 0, NULL);
    zassert_equal(stats.fire_count, 0U, "重置后 fire_count 应为 0");

    zassert_equal(sys_timer_stop(t), 0, NULL);
    zassert_equal(sys_timer_delete(t), 0, NULL);
}

ZTEST(sys_timer, test_time_until_expiry) {
    zassert_equal(sys_timer_init(), 0, NULL);

    sys_timer_handle_t t = sys_timer_oneshot(500U, timer_cb, NULL);
    zassert_not_null(t, NULL);

    uint32_t remaining = sys_timer_get_time_until_expiry(t);
    zassert_true(remaining > 0U && remaining <= 500U, "剩余时间应在 (0, 500] 内");

    zassert_equal(sys_timer_stop(t), 0, NULL);
    zassert_equal(sys_timer_delete(t), 0, NULL);
}

ZTEST(sys_timer, test_delete_from_callback_rejected) {
    sys_timer_config_t cfg = {
        .mode = SYS_TIMER_ONESHOT,
        .delay_ms = 50U,
        .period_ms = 0U,
        .callback = self_delete_cb,
        .user_data = NULL,
        .name = "sd",
        .priority = 0,
    };
    sys_timer_handle_t t;

    atomic_set(&g_deadlk_seen, 0);
    zassert_equal(sys_timer_init(), 0, NULL);

    t = sys_timer_create(&cfg);
    zassert_not_null(t, NULL);
    zassert_equal(sys_timer_start(t), 0, NULL);

    zassert_true(ztest_wait_until(deadlk_flag_set, NULL, 2000U), "回调内 delete 应返回 -EDEADLK");

    zassert_equal(sys_timer_delete(t), 0, "回调外 delete 应成功");
}

ZTEST_SUITE(sys_timer, NULL, NULL, NULL, NULL, NULL);
