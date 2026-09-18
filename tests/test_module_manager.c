/**
 * @file test_module_manager.c
 * @brief 模块管理器单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-09-18       1.1            zeh            嵌套函数提取为文件级 static；接口表加 static；手写等待改用 ztest_sync；补 STOPPING 窗口真实用例
 *
 */

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>
#include <errno.h>
#include <zeplod/event_system.h>
#include <zeplod/module_manager.h>
#include "ztest_sync.h"

LOG_MODULE_REGISTER(test_module_manager);

/** 并发交错用例中等待对端进入预期状态的单步超时（ms），超时即判失败 */
#define MM_UT_INTERLOCK_WAIT_MS 500U

static int stub_init(void* config) {
    (void) config;
    return 0;
}

static int stub_start(void) {
    return 0;
}

static int stub_stop(void) {
    return 0;
}

static void stub_on_event(const event_t* event, void* user_data) {
    (void) event;
    (void) user_data;
}

DECLARE_MODULE_INTERFACE_MINIMAL(stub);

/* 为需要多模块的测试创建额外的 stub 模块 */
static int stub2_init(void* config) {
    (void) config;
    return 0;
}

static int stub2_start(void) {
    return 0;
}

static int stub2_stop(void) {
    return 0;
}

static void stub2_on_event(const event_t* event, void* user_data) {
    (void) event;
    (void) user_data;
}

DECLARE_MODULE_INTERFACE_MINIMAL(stub2);

static int stub3_init(void* config) {
    (void) config;
    return 0;
}

static int stub3_start(void) {
    return 0;
}

static int stub3_stop(void) {
    return 0;
}

static void stub3_on_event(const event_t* event, void* user_data) {
    (void) event;
    (void) user_data;
}

DECLARE_MODULE_INTERFACE_MINIMAL(stub3);

#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
static const char* const version_dep_names[] = {"version_base", NULL};
static const uint32_t    version_dep_min_high[] = {MODULE_VERSION(2, 0, 0)};

static const module_interface_t version_base_interface = {
    .name = "version_base",
    .version = MODULE_VERSION(1, 0, 0),
    .priority = MODULE_PRIORITY_NORMAL,
    .depends_on = NULL,
    .depends_version_min = NULL,
    .init = stub_init,
    .start = stub_start,
    .stop = stub_stop,
    .shutdown = NULL,
    .on_event = stub_on_event,
    .get_status = NULL,
    .control = NULL,
};

static const module_interface_t version_needs_high_interface = {
    .name = "version_needs_high",
    .version = MODULE_VERSION(1, 0, 0),
    .priority = MODULE_PRIORITY_NORMAL,
    .depends_on = version_dep_names,
    .depends_version_min = version_dep_min_high,
    .init = stub_init,
    .start = stub_start,
    .stop = stub_stop,
    .shutdown = NULL,
    .on_event = stub_on_event,
    .get_status = NULL,
    .control = NULL,
};
#endif

/* 用于 shutdown / unregister 交叠场景：统计 shutdown 调用次数 */
static atomic_t g_counting_shutdown_calls;

static int counting_init(void* config) {
    (void) config;
    return 0;
}

static int counting_start(void) {
    return 0;
}

static int counting_stop(void) {
    return 0;
}

static int counting_shutdown(void) {
    atomic_inc(&g_counting_shutdown_calls);
    return MODULE_OK;
}

static void counting_on_event(const event_t* event, void* user_data) {
    (void) event;
    (void) user_data;
}

static module_status_t counting_get_status(void) {
    return MODULE_STATUS_RUNNING;
}

static int counting_control(int cmd, void* arg) {
    (void) cmd;
    (void) arg;
    return 0;
}

static const module_interface_t counting_stub_interface = {
    .name = "counting_stub",
    .version = MODULE_VERSION(1, 0, 0),
    .priority = MODULE_PRIORITY_NORMAL,
    .depends_on = NULL,
    .init = counting_init,
    .start = counting_start,
    .stop = counting_stop,
    .shutdown = counting_shutdown,
    .on_event = counting_on_event,
    .get_status = counting_get_status,
    .control = counting_control,
};

/* ---------- 文件级回调：替代嵌套函数（GCC 扩展，不可移植） ---------- */
static int g_foreach_count;
static int g_mgr_event_callback_count;

static void foreach_count_callback(module_info_t* info, void* user_data) {
    ARG_UNUSED(info);
    ARG_UNUSED(user_data);
    g_foreach_count++;
}

static void mgr_event_callback(uint32_t module_id, module_mgr_event_t event, void* user_data) {
    ARG_UNUSED(module_id);
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
    g_mgr_event_callback_count++;
}

/* 谓词：等待 slow_stub 注册后可通过名称查到 provisional id */
static bool slow_stub_id_assigned_pred(void* ctx) {
    uint32_t* found_id = ctx;

    *found_id = module_manager_get_id_by_name("slow_stub");
    return *found_id != 0U;
}

/* =============================================================================
 * 测试夹具 (Test Fixtures)
 * ============================================================================= */

static void* module_manager_suite_setup(void) {
    int ret;

    /* 前一套件（如 concurrency_stress）可能遗留 init/running 态 */
    module_manager_shutdown();

    ret = event_system_init();
    zassert_true(ret == EVENT_OK || ret == -EALREADY, "事件系统初始化失败: %d", ret);

    ret = module_manager_init();
    zassert_true(ret == 0 || ret == -EALREADY, "模块管理器初始化失败: %d", ret);

    ret = module_manager_start();
    zassert_true(ret == 0 || ret == -EALREADY || ret == MODULE_ERR_ALREADY_RUNNING, "模块管理器启动失败: %d", ret);

    return NULL;
}

static void module_manager_suite_teardown(void* fixture) {
    (void) fixture;
    module_manager_shutdown();
}

/**
 * @brief 每个测试用例后的清理函数
 *
 * 确保测试间的状态隔离，即使某个测试失败，也不会影响后续测试。
 */
static void module_manager_test_teardown(void* fixture) {
    (void) fixture;
    /* 直接调用 shutdown，它内部会处理 stop 和清理 */
    module_manager_shutdown();
    /* 重新初始化和启动 */
    module_manager_init();
    module_manager_start();
}

ZTEST(module_manager, test_register_unregister) {
    uint32_t id = 0U;

    zassert_equal(module_manager_register(&stub_interface, NULL, &id), 0, "register 失败");
    zassert_true(id > 0U, "module id 应非 0");

    zassert_equal(module_manager_unregister(id), 0, "unregister 失败");
}

ZTEST(module_manager, test_get_stats) {
    module_mgr_stats_t stats;

    module_manager_get_stats(&stats);
    zassert_equal(stats.total_modules, 0U, "初始应为 0 个模块");
}

ZTEST(module_manager, test_start_all_stop_all) {
    uint32_t id1 = 0U, id2 = 0U;

    /* 注册多个不同的模块 */
    zassert_equal(module_manager_register(&stub_interface, NULL, &id1), 0, "register 1 失败");
    zassert_equal(module_manager_register(&stub2_interface, NULL, &id2), 0, "register 2 失败");

    /* 启动所有模块 */
    int started = module_manager_start_all();
    zassert_true(started >= 2, "应至少启动 2 个模块");

    /* 停止所有模块 */
    int stopped = module_manager_stop_all();
    zassert_true(stopped >= 2, "应至少停止 2 个模块");

    zassert_equal(module_manager_unregister(id1), 0, NULL);
    zassert_equal(module_manager_unregister(id2), 0, NULL);
}

#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
ZTEST(module_manager, test_dependency_version_min_rejects_low_version) {
    uint32_t base_id = 0U;
    uint32_t dependent_id = 0U;

    zassert_equal(module_manager_register(&version_base_interface, NULL, &base_id), 0, NULL);
    zassert_equal(module_manager_register(&version_needs_high_interface, NULL, &dependent_id), 0, NULL);

    int started = module_manager_start_all();

    zassert_equal(started, 1, "only dependency with acceptable version should start");

    module_info_t base_info;
    module_info_t dependent_info;

    zassert_equal(module_manager_get_module_info(base_id, &base_info), 0, NULL);
    zassert_equal(module_manager_get_module_info(dependent_id, &dependent_info), 0, NULL);
    zassert_equal(base_info.status, MODULE_STATUS_RUNNING, NULL);
    zassert_equal(dependent_info.status, MODULE_STATUS_INITIALIZED, NULL);
}
#endif

ZTEST(module_manager, test_suspend_resume) {
    uint32_t id = 0U;

    zassert_equal(module_manager_register(&stub_interface, NULL, &id), 0, "register 失败");

    /* 启动模块 */
    zassert_equal(module_manager_start_module(id), 0, "start_module 失败");

    /* 暂停模块 */
    zassert_equal(module_manager_suspend_module(id), 0, "suspend 失败");

    /* 恢复模块 */
    zassert_equal(module_manager_resume_module(id), 0, "resume 失败");

    /* 停止并注销 */
    zassert_equal(module_manager_stop_module(id), 0, "stop_module 失败");
    zassert_equal(module_manager_unregister(id), 0, "unregister 失败");
}

ZTEST(module_manager, test_get_module_info) {
    uint32_t      id = 0U;
    module_info_t info;

    zassert_equal(module_manager_register(&stub_interface, NULL, &id), 0, "register 失败");

    /* 获取模块信息 */
    zassert_equal(module_manager_get_module_info(id, &info), 0, "get_module_info 失败");
    zassert_equal(info.id, id, "模块 ID 应匹配");
    zassert_not_null(info.interface, "接口指针不应为 NULL");
    zassert_not_null(info.interface->name, "模块名称不应为 NULL");

    /* 测试无效 ID（API 返回 MODULE_ERR_NOT_FOUND / -ENOENT，勿写死 -1） */
    zassert_equal(module_manager_get_module_info(9999, &info), MODULE_ERR_NOT_FOUND, "无效 ID 应返回 NOT_FOUND");

    /* 测试 NULL 输出（实现返回 MODULE_ERR_INVALID_ARG，非 -1） */
    zassert_equal(module_manager_get_module_info(id, NULL), MODULE_ERR_INVALID_ARG, "NULL 输出应返回 INVALID_ARG");

    zassert_equal(module_manager_unregister(id), 0, NULL);
}

ZTEST(module_manager, test_get_id_by_name) {
    uint32_t id = 0U;

    zassert_equal(module_manager_register(&stub_interface, NULL, &id), 0, "register 失败");

    /* 按名称获取 ID */
    uint32_t found_id = module_manager_get_id_by_name("stub");
    zassert_equal(found_id, id, "按名称查找的 ID 应匹配");

    /* 测试不存在的名称 */
    uint32_t invalid_id = module_manager_get_id_by_name("nonexistent");
    zassert_equal(invalid_id, 0U, "不存在的名称应返回 0");

    zassert_equal(module_manager_unregister(id), 0, NULL);
}

ZTEST(module_manager, test_foreach) {
    /* 注册几个不同的模块 */
    uint32_t id1 = 0U, id2 = 0U, id3 = 0U;
    zassert_equal(module_manager_register(&stub_interface, NULL, &id1), 0, NULL);
    zassert_equal(module_manager_register(&stub2_interface, NULL, &id2), 0, NULL);
    zassert_equal(module_manager_register(&stub3_interface, NULL, &id3), 0, NULL);

    g_foreach_count = 0;
    module_manager_foreach(foreach_count_callback, NULL);
    zassert_equal(g_foreach_count, 3, "应遍历到 3 个模块");

    zassert_equal(module_manager_unregister(id1), 0, NULL);
    zassert_equal(module_manager_unregister(id2), 0, NULL);
    zassert_equal(module_manager_unregister(id3), 0, NULL);
}

ZTEST(module_manager, test_reset_stats) {
    module_mgr_stats_t stats;

    /* 注册和注销模块会增加 events_processed */
    uint32_t id = 0U;
    module_manager_register(&stub_interface, NULL, &id);
    module_manager_unregister(id);

    /* 重置统计 */
    module_manager_reset_stats();
    module_manager_get_stats(&stats);
    zassert_equal(stats.total_modules, 0U, "重置后 total_modules 应为 0");
}

ZTEST(module_manager, test_dump_info) {
    /* 注册一个模块 */
    uint32_t id = 0U;
    zassert_equal(module_manager_register(&stub_interface, NULL, &id), 0, NULL);

    /* 调用 dump（不应崩溃）*/
    module_manager_dump_info();

    zassert_equal(module_manager_unregister(id), 0, NULL);
}

ZTEST(module_manager, test_shutdown_invokes_module_shutdown_once) {
    uint32_t id = 0U;

    atomic_set(&g_counting_shutdown_calls, 0);

    zassert_equal(module_manager_register(&counting_stub_interface, NULL, &id), 0, NULL);
    zassert_equal(module_manager_start_module(id), 0, NULL);

    zassert_equal(module_manager_shutdown(), MODULE_OK, NULL);
    zassert_equal(atomic_get(&g_counting_shutdown_calls), 1, "manager shutdown 应调用模块 shutdown 一次");

    module_mgr_stats_t stats;
    module_manager_get_stats(&stats);
    zassert_equal(stats.total_modules, 0U, "shutdown 后模块表应清空");

    zassert_equal(module_manager_init(), MODULE_OK, NULL);
    zassert_equal(module_manager_start(), MODULE_OK, NULL);
}

ZTEST(module_manager, test_unregister_skips_shutdown_during_manager_shutdown) {
    uint32_t id = 0U;

    atomic_set(&g_counting_shutdown_calls, 0);

    zassert_equal(module_manager_register(&counting_stub_interface, NULL, &id), 0, NULL);
    zassert_equal(module_manager_start_module(id), 0, NULL);

    zassert_equal(module_manager_shutdown(), MODULE_OK, NULL);
    const int shutdown_calls_after_mgr_shutdown = atomic_get(&g_counting_shutdown_calls);

    /* shutdown 后管理器已反初始化；unregister 应拒绝且不得再次调用模块 shutdown */
    zassert_equal(module_manager_unregister(id), MODULE_ERR_NOT_INITIALIZED, NULL);
    zassert_equal(atomic_get(&g_counting_shutdown_calls), shutdown_calls_after_mgr_shutdown,
                  "shutdown 完成后 unregister 不应再次调用模块 shutdown");

    zassert_equal(module_manager_init(), MODULE_OK, NULL);
    zassert_equal(module_manager_start(), MODULE_OK, NULL);
}

ZTEST(module_manager, test_set_callback) {
    /* 设置回调 */
    g_mgr_event_callback_count = 0;
    module_manager_set_callback(mgr_event_callback, NULL);

    /* 注册模块应触发回调 */
    uint32_t id = 0U;
    zassert_equal(module_manager_register(&stub_interface, NULL, &id), 0, NULL);

    zassert_true(g_mgr_event_callback_count > 0, "注册时回调应同步触发");

    zassert_equal(module_manager_unregister(id), 0, NULL);

    /* 尾态复位：避免回调残留影响后续用例的 manager 事件统计 */
    module_manager_set_callback(NULL, NULL);
}

/* =============================================================================
 * 10 项并发/契约修复的测试覆盖
 * =============================================================================
 *
 * 这些用例通过信号量精确控制线程交错，验证每项并发修复的语义行为。
 */

#include <zephyr/kernel.h>

/* ---------- 共享桩：用于 init 期间注销的测试（#4） ---------- */
static struct k_sem    g_slow_init_go;
static atomic_t        g_slow_init_entered;
static struct k_thread g_slow_register_thread;
static K_THREAD_STACK_DEFINE(g_slow_register_stack, 1024);
static atomic_t g_slow_register_result;
static uint32_t g_slow_register_id;

/* 谓词：等待 slow_init 已被管理器调用进入阻塞点 */
static bool slow_init_entered_pred(void* ctx) {
    ARG_UNUSED(ctx);
    return atomic_get(&g_slow_init_entered) != 0;
}

static int slow_init(void* config) {
    (void) config;
    atomic_set(&g_slow_init_entered, 1);
    /* 等待测试主线程进入 unregister，然后完成 init */
    (void) k_sem_take(&g_slow_init_go, K_MSEC(500));
    return 0;
}

static int slow_start(void) {
    return 0;
}
static int slow_stop(void) {
    return 0;
}

/* 定义置于 slow_register_thread_entry 之前，替代原先的 extern 前置声明（本文件私有，加 static） */
static const module_interface_t slow_stub_interface = {
    .name = "slow_stub",
    .version = MODULE_VERSION(1, 0, 0),
    .priority = MODULE_PRIORITY_NORMAL,
    .depends_on = NULL,
    .init = slow_init,
    .start = slow_start,
    .stop = slow_stop,
    .shutdown = NULL,
    .on_event = stub_on_event,
    .get_status = NULL,
    .control = NULL,
};

static void slow_register_thread_entry(void* p1, void* p2, void* p3) {
    (void) p1;
    (void) p2;
    (void) p3;
    const int ret = module_manager_register(&slow_stub_interface, NULL, &g_slow_register_id);
    atomic_set(&g_slow_register_result, ret);
}

/* ---------- 共享桩：用于依赖 start 失败的测试（#5，仅运行时依赖配置启用） ---------- */
#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
static atomic_t g_dep_failing_start_calls;
static atomic_t g_dep_dependent_start_calls;

static int dep_failing_start(void) {
    atomic_inc(&g_dep_failing_start_calls);
    return -1; /* 强制 start 失败 */
}

static int dep_dependent_start(void) {
    atomic_inc(&g_dep_dependent_start_calls);
    return 0;
}

static const char* const dep_fail_dep_names[] = {"dep_fail_base", NULL};

static const module_interface_t dep_fail_base_interface = {
    .name = "dep_fail_base",
    .version = MODULE_VERSION(1, 0, 0),
    .priority = MODULE_PRIORITY_LOW,
    .depends_on = NULL,
    .init = stub_init,
    .start = dep_failing_start, /* 关键：base 的 start 强制失败 */
    .stop = stub_stop,
    .shutdown = NULL,
    .on_event = stub_on_event,
    .get_status = NULL,
    .control = NULL,
};

static const module_interface_t dep_fail_dependent_interface = {
    .name = "dep_fail_dependent",
    .version = MODULE_VERSION(1, 0, 0),
    .priority = MODULE_PRIORITY_NORMAL,
    .depends_on = dep_fail_dep_names,
    .depends_version_min = NULL,
    .init = stub_init,
    .start = dep_dependent_start,
    .stop = stub_stop,
    .shutdown = NULL,
    .on_event = stub_on_event,
    .get_status = NULL,
    .control = NULL,
};
#endif /* CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES */

/* ---------- 共享桩：验证 shutdown 等待在途事件回调 ---------- */
static struct k_sem    g_block_event_entered;
static struct k_sem    g_block_event_release;
static struct k_thread g_block_event_thread;
static struct k_thread g_block_shutdown_thread;
static K_THREAD_STACK_DEFINE(g_block_event_stack, 1024);
static K_THREAD_STACK_DEFINE(g_block_shutdown_stack, 1536);
static atomic_t g_block_event_result;
static atomic_t g_block_shutdown_result;
static atomic_t g_block_shutdown_done;
static atomic_t g_block_shutdown_calls;
static uint32_t g_block_module_id;

static void blocking_on_event(const event_t* event, void* user_data) {
    (void) event;
    (void) user_data;
    k_sem_give(&g_block_event_entered);
    (void) k_sem_take(&g_block_event_release, K_SECONDS(2));
}

static int blocking_shutdown(void) {
    atomic_inc(&g_block_shutdown_calls);
    return MODULE_OK;
}

static const module_interface_t blocking_stub_interface = {
    .name = "blocking_stub",
    .version = MODULE_VERSION(1, 0, 0),
    .priority = MODULE_PRIORITY_NORMAL,
    .depends_on = NULL,
    .depends_version_min = NULL,
    .init = stub_init,
    .start = stub_start,
    .stop = stub_stop,
    .shutdown = blocking_shutdown,
    .on_event = blocking_on_event,
    .get_status = NULL,
    .control = NULL,
};

static void blocking_send_thread_entry(void* p1, void* p2, void* p3) {
    (void) p1;
    (void) p2;
    (void) p3;
    event_t event = {0};
    atomic_set(&g_block_event_result, module_manager_send_to_module(g_block_module_id, &event));
}

static void blocking_shutdown_thread_entry(void* p1, void* p2, void* p3) {
    (void) p1;
    (void) p2;
    (void) p3;
    atomic_set(&g_block_shutdown_result, module_manager_shutdown());
    atomic_set(&g_block_shutdown_done, 1);
}

/* ---------- 共享桩：验证 manager stop 等待并发 start 收敛 ---------- */
static struct k_sem    g_block_start_entered;
static struct k_sem    g_block_start_release;
static struct k_thread g_block_start_thread;
static struct k_thread g_manager_stop_thread;
static K_THREAD_STACK_DEFINE(g_block_start_stack, 1024);
static K_THREAD_STACK_DEFINE(g_manager_stop_stack, 1536);
static atomic_t g_block_start_result;
static atomic_t g_manager_stop_result;
static atomic_t g_manager_stop_done;
static uint32_t g_block_start_module_id;

static int blocking_start(void) {
    k_sem_give(&g_block_start_entered);
    (void) k_sem_take(&g_block_start_release, K_SECONDS(2));
    return MODULE_OK;
}

static const module_interface_t blocking_start_interface = {
    .name = "blocking_start",
    .version = MODULE_VERSION(1, 0, 0),
    .priority = MODULE_PRIORITY_NORMAL,
    .depends_on = NULL,
    .depends_version_min = NULL,
    .init = stub_init,
    .start = blocking_start,
    .stop = stub_stop,
    .shutdown = NULL,
    .on_event = stub_on_event,
    .get_status = NULL,
    .control = NULL,
};

static void blocking_start_thread_entry(void* p1, void* p2, void* p3) {
    (void) p1;
    (void) p2;
    (void) p3;
    atomic_set(&g_block_start_result, module_manager_start_module(g_block_start_module_id));
}

static void manager_stop_thread_entry(void* p1, void* p2, void* p3) {
    (void) p1;
    (void) p2;
    (void) p3;
    atomic_set(&g_manager_stop_result, module_manager_stop());
    atomic_set(&g_manager_stop_done, 1);
}

/* ---------- #4：init 期间注销返回 EBUSY ---------- */
ZTEST(module_manager, test_unregister_during_init_returns_busy) {
    k_sem_init(&g_slow_init_go, 0, 1);
    atomic_set(&g_slow_init_entered, 0);
    atomic_set(&g_slow_register_result, MODULE_ERR_BUSY);
    g_slow_register_id = 0U;

    k_tid_t tid =
        k_thread_create(&g_slow_register_thread, g_slow_register_stack, K_THREAD_STACK_SIZEOF(g_slow_register_stack),
                        slow_register_thread_entry, NULL, NULL, NULL, 5, 0, K_NO_WAIT);

    uint32_t slow_id = 0U;

    zassert_true(ztest_wait_until(slow_init_entered_pred, NULL, MM_UT_INTERLOCK_WAIT_MS), "init 应已进入");

    /* register() 仅在 init 成功后才写出 *module_id；init 期间用名称查询 */
    zassert_true(ztest_wait_until(slow_stub_id_assigned_pred, &slow_id, MM_UT_INTERLOCK_WAIT_MS),
                 "init 期间应已分配 provisional id");
    zassert_not_equal(slow_id, 0U, "init 期间应已分配 provisional id");

    zassert_equal(module_manager_unregister(slow_id), MODULE_ERR_BUSY, "init 期间注销应返回 BUSY");

    k_sem_give(&g_slow_init_go);
    zassert_equal(k_thread_join(tid, K_SECONDS(2)), 0, "register thread 应结束");
    zassert_equal(atomic_get(&g_slow_register_result), MODULE_OK, "register 应成功");
    zassert_not_equal(g_slow_register_id, 0U, "register 完成后应写出 module id");

    zassert_equal(module_manager_unregister(g_slow_register_id), 0, "init 完成后注销应成功");
}

ZTEST(module_manager, test_manager_stop_waits_for_concurrent_start) {
    k_sem_init(&g_block_start_entered, 0, 1);
    k_sem_init(&g_block_start_release, 0, 1);
    atomic_set(&g_block_start_result, MODULE_ERR_BUSY);
    atomic_set(&g_manager_stop_result, MODULE_ERR_BUSY);
    atomic_set(&g_manager_stop_done, 0);
    g_block_start_module_id = 0U;

    zassert_equal(module_manager_register(&blocking_start_interface, NULL, &g_block_start_module_id), MODULE_OK, NULL);

    k_tid_t start_tid =
        k_thread_create(&g_block_start_thread, g_block_start_stack, K_THREAD_STACK_SIZEOF(g_block_start_stack),
                        blocking_start_thread_entry, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    zassert_equal(k_sem_take(&g_block_start_entered, K_SECONDS(1)), 0, "start 回调应已进入");

    k_tid_t stop_tid =
        k_thread_create(&g_manager_stop_thread, g_manager_stop_stack, K_THREAD_STACK_SIZEOF(g_manager_stop_stack),
                        manager_stop_thread_entry, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    /* 时间窗意图：给 stop 线程 20ms 调度余量越过 stop 前置检查、
     * 阻塞在"等待在途 start"上；随后断言其尚未完成。仅作负向时序窗口，
     * 不用于等待任何完成事件（完成走 k_thread_join）。 */
    k_msleep(20);
    zassert_equal(atomic_get(&g_manager_stop_done), 0, "manager stop 不得越过在途 start");

    k_sem_give(&g_block_start_release);
    zassert_equal(k_thread_join(start_tid, K_SECONDS(1)), 0, "start thread 应结束");
    zassert_equal(k_thread_join(stop_tid, K_SECONDS(1)), 0, "stop thread 应结束");
    zassert_equal(atomic_get(&g_block_start_result), MODULE_OK, "并发 start 应正常完成");
    zassert_equal(atomic_get(&g_manager_stop_result), MODULE_OK, "manager stop 应在 start 后完成");

    module_info_t info;
    zassert_equal(module_manager_get_module_info(g_block_start_module_id, &info), MODULE_OK, NULL);
    zassert_equal(info.status, MODULE_STATUS_STOPPED, "并发 start 完成后模块必须被 stop");
    zassert_equal(module_manager_unregister(g_block_start_module_id), MODULE_OK, NULL);
}

/* ---------- #5：依赖未 RUNNING 时 dependent 不应被启动 ---------- */
#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
ZTEST(module_manager, test_dep_start_failure_skips_dependent) {
    uint32_t base_id = 0U;
    uint32_t dep_id = 0U;

    atomic_set(&g_dep_failing_start_calls, 0);
    atomic_set(&g_dep_dependent_start_calls, 0);

    /* 关键布局：base 的 start_fn 强制失败 → 启动后状态为 ERROR。
     * dep_fail_dependent 的 depends_on=[base]。
     * start_all 调用时，base 是 INITIALIZED/STOPPED 候选；启动后失败。
     * 第二次循环到 dependent 时，依赖重检发现 base 不是 RUNNING，跳过。 */
    zassert_equal(module_manager_register(&dep_fail_base_interface, NULL, &base_id), 0, NULL);
    zassert_equal(module_manager_register(&dep_fail_dependent_interface, NULL, &dep_id), 0, NULL);

    const int started = module_manager_start_all();

    /* base.start 被调用（强制失败），dependent.start 不应被调用 */
    zassert_true(atomic_get(&g_dep_failing_start_calls) >= 1, "base.start 至少应被调用一次");
    /* 关键断言：dependent.start 只在 base 成功 RUNNING 后才会被尝试；
     * 由于 base 失败，dependent.start 不应被调用。 */
    zassert_equal(atomic_get(&g_dep_failing_start_calls), 1, "base.start 应只调用一次");
    zassert_equal(atomic_get(&g_dep_dependent_start_calls), 0, "依赖 base 未 RUNNING 时 dependent.start 不应被调用");

    zassert_equal(started, 0, "base 与 dependent 均不应计入 started");

    /* 清理 */
    (void) module_manager_unregister(base_id);
    (void) module_manager_unregister(dep_id);
}
#endif

/* ---------- #6：manager 门控的语义性检查 ---------- */
ZTEST(module_manager, test_error_code_values) {
    /* 门控路径返回的错误码定义须与 errno 语义一致（register/unregister/start_all 均依赖） */
    zassert_equal(MODULE_ERR_INVALID_STATE, -EPERM, "错误码 INVALID_STATE 应等于 -EPERM");
    zassert_equal(MODULE_ERR_BUSY, -EBUSY, "错误码 BUSY 应等于 -EBUSY");
    zassert_equal(MODULE_ERR_IO, -EIO, "错误码 IO 应等于 -EIO");
}

/* ---------- #6 补充：STOPPING 窗口内 register 被门控拒绝（真实交错） ---------- */
static struct k_sem    g_stopping_probe_entered;
static struct k_sem    g_stopping_probe_release;
static struct k_thread g_mgr_stop_probe_thread;
static K_THREAD_STACK_DEFINE(g_mgr_stop_probe_stack, 1536);
static atomic_t g_mgr_stop_probe_result;

/* stop_fn 阻塞：让 manager 停在 STOPPING 排空阶段（lifecycle.c 的 stop 在
 * RUNNING→STOPPING 迁移后即释放管理器锁进入排空循环，此时 register 可进入门控检查） */
static int stopping_probe_stop(void) {
    k_sem_give(&g_stopping_probe_entered);
    (void) k_sem_take(&g_stopping_probe_release, K_SECONDS(2));
    return MODULE_OK;
}

static const module_interface_t stopping_probe_interface = {
    .name = "stopping_probe",
    .version = MODULE_VERSION(1, 0, 0),
    .priority = MODULE_PRIORITY_NORMAL,
    .depends_on = NULL,
    .depends_version_min = NULL,
    .init = stub_init,
    .start = stub_start,
    .stop = stopping_probe_stop,
    .shutdown = NULL,
    .on_event = stub_on_event,
    .get_status = NULL,
    .control = NULL,
};

static void mgr_stop_probe_thread_entry(void* p1, void* p2, void* p3) {
    (void) p1;
    (void) p2;
    (void) p3;
    atomic_set(&g_mgr_stop_probe_result, module_manager_stop());
}

ZTEST(module_manager, test_register_rejected_in_stopping_window) {
    uint32_t probe_id = 0U;
    uint32_t late_id = 0U;

    k_sem_init(&g_stopping_probe_entered, 0, 1);
    k_sem_init(&g_stopping_probe_release, 0, 1);
    atomic_set(&g_mgr_stop_probe_result, MODULE_ERR_BUSY);

    /* 前置：manager 处于 RUNNING（套件 setup/teardown 保证），探测模块已启动 */
    zassert_equal(module_manager_register(&stopping_probe_interface, NULL, &probe_id), MODULE_OK, NULL);
    zassert_equal(module_manager_start_module(probe_id), MODULE_OK, NULL);

    /* 后台线程发起 manager stop：先迁移到 STOPPING，随后在排空循环里阻塞于探测模块的 stop_fn */
    k_tid_t stop_tid = k_thread_create(&g_mgr_stop_probe_thread, g_mgr_stop_probe_stack,
                                       K_THREAD_STACK_SIZEOF(g_mgr_stop_probe_stack), mgr_stop_probe_thread_entry,
                                       NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    zassert_not_null(stop_tid, NULL);
    zassert_equal(k_sem_take(&g_stopping_probe_entered, K_SECONDS(1)), 0, "stop 应已进入探测模块的 stop_fn");

    /* 此刻 manager 生命周期状态为 STOPPING：register 应被门控拒绝 */
    zassert_equal(module_manager_register(&stub_interface, NULL, &late_id), MODULE_ERR_INVALID_STATE,
                  "STOPPING 窗口内 register 应返回 INVALID_STATE");

    /* 放行 stop_fn，manager stop 正常收敛 */
    k_sem_give(&g_stopping_probe_release);
    zassert_equal(k_thread_join(stop_tid, K_SECONDS(2)), 0, "stop 线程应结束");
    zassert_equal(atomic_get(&g_mgr_stop_probe_result), MODULE_OK, "manager stop 应正常完成");

    /* STOPPED 可直接再次 start（manager 仍处于已初始化态，勿重复 init）；
     * probe 模块残留由用例尾部 teardown 的 shutdown 统一回收 */
    zassert_equal(module_manager_start(), MODULE_OK, NULL);
}

/* ---------- #7：unsubscribe 返回值与本地记录保持一致 ---------- */
ZTEST(module_manager, test_unsubscribe_reports_missing_subscription) {
    const event_type_t   type = 203U;
    uint32_t             id = 0U;
    const event_status_t register_status = event_register_type(type, "module_unsubscribe");
    zassert_equal(register_status, EVENT_OK, "event type 应已注册或可注册");
    zassert_equal(module_manager_register(&stub_interface, NULL, &id), 0, NULL);
    zassert_equal(module_manager_subscribe(id, type), 0, "subscribe 应成功");

    zassert_equal(module_manager_unsubscribe(id, type), 0, "首次 unsubscribe 应成功");
    zassert_equal(module_manager_unsubscribe(id, type), MODULE_ERR_NOT_FOUND, "重复 unsubscribe 应报告本地订阅不存在");

    (void) module_manager_unregister(id);
}

ZTEST(module_manager, test_subscription_cookie_survives_array_compaction) {
    const event_type_t type_a = 201U;
    const event_type_t type_b = 202U;
    uint32_t           id = 0U;
    module_info_t      before;
    module_info_t      after;

    zassert_equal(event_register_type(type_a, "module_cookie_a"), EVENT_OK, NULL);
    zassert_equal(event_register_type(type_b, "module_cookie_b"), EVENT_OK, NULL);
    zassert_equal(module_manager_register(&stub_interface, NULL, &id), MODULE_OK, NULL);
    zassert_equal(module_manager_subscribe(id, type_a), MODULE_OK, NULL);
    zassert_equal(module_manager_subscribe(id, type_b), MODULE_OK, NULL);
    zassert_equal(module_manager_get_module_info(id, &before), MODULE_OK, NULL);
    zassert_equal(before.event_subscription_count, 2U, NULL);

    const uint32_t cookie_b = before.event_subscriptions[1].cookie;
    zassert_not_equal(cookie_b, 0U, NULL);

    zassert_equal(module_manager_unsubscribe(id, type_a), MODULE_OK, NULL);
    zassert_equal(module_manager_get_module_info(id, &after), MODULE_OK, NULL);
    zassert_equal(after.event_subscription_count, 1U, NULL);
    zassert_equal(after.event_subscriptions[0].type, type_b, NULL);
    zassert_equal(after.event_subscriptions[0].cookie, cookie_b, "数组压缩不得改变订阅身份");

    zassert_equal(module_manager_unsubscribe(id, type_b), MODULE_OK, NULL);
    zassert_equal(module_manager_unregister(id), MODULE_OK, NULL);
}

ZTEST(module_manager, test_shutdown_waits_for_in_flight_event_callback) {
    k_sem_init(&g_block_event_entered, 0, 1);
    k_sem_init(&g_block_event_release, 0, 1);
    atomic_set(&g_block_event_result, MODULE_ERR_BUSY);
    atomic_set(&g_block_shutdown_result, MODULE_ERR_BUSY);
    atomic_set(&g_block_shutdown_done, 0);
    atomic_set(&g_block_shutdown_calls, 0);
    g_block_module_id = 0U;

    zassert_equal(module_manager_register(&blocking_stub_interface, NULL, &g_block_module_id), MODULE_OK, NULL);
    zassert_equal(module_manager_start_module(g_block_module_id), MODULE_OK, NULL);

    k_tid_t event_tid =
        k_thread_create(&g_block_event_thread, g_block_event_stack, K_THREAD_STACK_SIZEOF(g_block_event_stack),
                        blocking_send_thread_entry, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    zassert_equal(k_sem_take(&g_block_event_entered, K_SECONDS(1)), 0, "事件回调应已进入");

    k_tid_t shutdown_tid =
        k_thread_create(&g_block_shutdown_thread, g_block_shutdown_stack, K_THREAD_STACK_SIZEOF(g_block_shutdown_stack),
                        blocking_shutdown_thread_entry, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    /* 时间窗意图：给 shutdown 线程 20ms 调度余量进入"等待在途事件回调"的阻塞点；
     * 随后断言其尚未完成。仅作负向时序窗口，完成判定走 k_thread_join。 */
    k_msleep(20);
    zassert_equal(atomic_get(&g_block_shutdown_done), 0, "shutdown 不得越过在途事件回调");
    zassert_equal(atomic_get(&g_block_shutdown_calls), 0, "回调结束前不得调用模块 shutdown");

    k_sem_give(&g_block_event_release);
    zassert_equal(k_thread_join(event_tid, K_SECONDS(1)), 0, NULL);
    zassert_equal(k_thread_join(shutdown_tid, K_SECONDS(2)), 0, NULL);
    zassert_equal(atomic_get(&g_block_event_result), MODULE_OK, NULL);
    zassert_equal(atomic_get(&g_block_shutdown_result), MODULE_OK, NULL);
    zassert_equal(atomic_get(&g_block_shutdown_calls), 1, NULL);
}

/* ---------- #9：reset_stats 保留模块计数字段 ---------- */
ZTEST(module_manager, test_reset_stats_keeps_module_counts) {
    uint32_t id1 = 0U;
    uint32_t id2 = 0U;

    zassert_equal(module_manager_register(&stub_interface, NULL, &id1), 0, NULL);
    zassert_equal(module_manager_register(&stub2_interface, NULL, &id2), 0, NULL);
    zassert_equal(module_manager_start_module(id1), 0, NULL);
    zassert_equal(module_manager_start_module(id2), 0, NULL);

    /* 触发一些事件让 processed/dropped 非 0 */
    (void) module_manager_send_to_module(id1, NULL); /* NULL event 走 dropped 路径 */
    (void) module_manager_broadcast(NULL);           /* 同上 */

    module_mgr_stats_t before;
    module_manager_get_stats(&before);
    zassert_true(before.events_dropped > 0U, "应至少有一次 dropped");

    module_manager_reset_stats();

    module_mgr_stats_t after;
    module_manager_get_stats(&after);
    /* 模块计数不应被 reset 清零 */
    zassert_equal(after.total_modules, before.total_modules, "total_modules 不应被 reset");
    zassert_equal(after.active_modules, before.active_modules, "active_modules 不应被 reset");
    zassert_equal(after.error_modules, before.error_modules, "error_modules 不应被 reset");
    /* 事件计数器应被清零 */
    zassert_equal(after.events_processed, 0U, "events_processed 应清零");
    zassert_equal(after.events_dropped, 0U, "events_dropped 应清零");

    (void) module_manager_unregister(id1);
    (void) module_manager_unregister(id2);
}

/* ---------- #10：dump_info 在注销后不崩溃 ---------- */
ZTEST(module_manager, test_dump_info_safe_under_unregister) {
    uint32_t id = 0U;
    zassert_equal(module_manager_register(&stub_interface, NULL, &id), 0, NULL);
    zassert_equal(module_manager_start_module(id), 0, NULL);

    /* 直接 dump：name 应可见 */
    module_manager_dump_info();

    /* 注销后再 dump：不崩溃（修复前会有 UAF 风险） */
    zassert_equal(module_manager_unregister(id), 0, NULL);
    module_manager_dump_info();
}

/* Zephyr: setup, before_each, after_each, suite_teardown — 每测清理须在 arg5 */
ZTEST_SUITE(module_manager, NULL, module_manager_suite_setup, NULL, module_manager_test_teardown,
            module_manager_suite_teardown);
