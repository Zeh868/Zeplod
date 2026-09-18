/**
 * @file test_example_module_a.c
 * @brief 示例模块 A 单元测试
 * @author OpenClaw Agent
 * @version 1.2
 * @date 2026-04-10
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-10       1.0            agent          为 example_module_a 编写完整测试用例
 * 2026-05-28       1.1            zeh            异步等待改用 ztest_sync（P0）
 * 2026-09-18       1.2            zeh            类型/宏定义上移修复先用后定义；删除
 *                                                 未注册的 stub_a 死代码；并发用例改真
 *                                                 线程并发；reset 补效果断言；同步删除
 *                                                 src 侧 buffer_size 字段；本地
 *                                                 sensor_sample_t 对齐内部定义（含 quality）
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>
#include <string.h>
#include <zeplod/event_dispatcher.h>
#include <zeplod/event_system.h>
#include <zeplod/example_module_a.h>
#include <zeplod/module_manager.h>
#include "ztest_sync.h"

LOG_MODULE_REGISTER(test_example_module_a);

/* =============================================================================
 * 测试夹具 (Test Fixtures)
 * ============================================================================= */

static void* test_suite_setup(void) {
    int ret;

    /* 全局初始化 - 允许重复初始化（返回 -EALREADY） */
    ret = event_system_init();
    zassert_true(ret == EVENT_OK || ret == -EALREADY, "事件系统初始化失败: %d", ret);

    ret = event_system_start();
    zassert_true(ret == EVENT_OK || ret == -EALREADY, "事件系统启动失败: %d", ret);

    /* 初始化并启动事件分发器（关键！否则事件不会被处理） */
    ret = event_dispatcher_init(NULL);
    zassert_true(ret == EVENT_OK || ret == -EALREADY, "事件分发器初始化失败: %d", ret);

    ret = event_dispatcher_start();
    zassert_true(ret == EVENT_OK || ret == -EALREADY, "事件分发器启动失败: %d", ret);

    ret = module_manager_init();
    zassert_true(ret == 0 || ret == -EALREADY, "模块管理器初始化失败: %d", ret);

    ret = module_manager_start();
    zassert_true(ret == 0 || ret == -EALREADY, "模块管理器启动失败: %d", ret);

    return NULL;
}

static void test_suite_teardown(void* fixture) {
    (void) fixture;
    module_manager_shutdown();
    event_dispatcher_stop();
    event_system_stop();
}

/* =============================================================================
 * 本地定义（与 example_module_a.c 内部定义保持一致）
 * ============================================================================= */

/* 控制命令（example_module_a.c 内部约定的 1/2/3） */
#define CMD_SET_RATE 1
#define CMD_GET_RATE 2
#define CMD_RESET    3

/* 无效命令探针：落在合法命令集合之外 */
#define CMD_INVALID 9999U

/* 异步等待上限：覆盖最慢采样周期（10ms）的富余量 */
#define MODULE_A_DATA_WAIT_MS 2000U

/* 并发用例参数：工作线程数与每线程读写轮数 */
#define MODULE_A_CONCURRENT_WORKERS 2
#define MODULE_A_CONCURRENT_ITERS   20

/* 传感器采样数据类型（与内部缓冲条目同布局，保证 get_data 步长一致） */
typedef struct {
    int32_t  value;
    uint32_t timestamp;
    uint8_t  quality;
} sensor_sample_t;

/* =============================================================================
 * 辅助函数
 * ============================================================================= */

typedef struct {
    sensor_sample_t* samples;
    size_t           samples_bytes;
    int              bytes_read;
} module_a_get_data_ctx_t;

static bool module_a_has_sample_data(void* ctx) {
    module_a_get_data_ctx_t* probe = ctx;

    probe->bytes_read = example_module_a_get_data(probe->samples, probe->samples_bytes);
    return probe->bytes_read > 0;
}

static bool module_a_sample_rate_is_500(void* ctx) {
    ARG_UNUSED(ctx);
    uint32_t rate = 0;

    example_module_a_control(CMD_GET_RATE, &rate);
    return rate == 500U;
}

/* =============================================================================
 * 测试用例：模块初始化与生命周期
 * ============================================================================= */

/**
 * @brief 测试模块初始化 - 空配置
 */
ZTEST(example_module_a, test_init_with_null_config) {
    int ret;

    /* 使用 NULL 配置初始化 */
    ret = example_module_a_init(NULL);
    zassert_equal(ret, 0, "NULL 配置初始化应返回 0");

    /* 验证默认配置 */
    module_status_t status = example_module_a_get_status();
    zassert_equal(status, MODULE_STATUS_INITIALIZED, "初始化后状态应为 INITIALIZED");
}

/**
 * @brief 测试模块初始化 - 有效配置
 */
ZTEST(example_module_a, test_init_with_valid_config) {
    example_module_a_config_t config = {.sample_rate_ms = 50, .enable_filtering = false};

    /* 先关闭模块（如果之前已启动） */
    example_module_a_stop();
    example_module_a_shutdown();

    int ret = example_module_a_init(&config);
    zassert_equal(ret, 0, "有效配置初始化应返回 0");

    module_status_t status = example_module_a_get_status();
    zassert_equal(status, MODULE_STATUS_INITIALIZED, "初始化后状态应为 INITIALIZED");
}

/**
 * @brief 测试模块初始化 - 无效配置（sample_rate_ms = 0）
 */
ZTEST(example_module_a, test_init_with_invalid_config) {
    example_module_a_config_t config = {.sample_rate_ms = 0, /* 无效值 */
                                        .enable_filtering = false};

    /* 模块应能初始化，但后续操作会用到默认值 */
    int ret = example_module_a_init(&config);
    zassert_equal(ret, 0, "初始化应返回 0（配置检查在运行时进行）");
}

/* =============================================================================
 * 测试用例：模块启动与停止
 * ============================================================================= */

/**
 * @brief 测试模块启动
 */
ZTEST(example_module_a, test_start) {
    /* 先初始化 */
    example_module_a_init(NULL);

    int ret = example_module_a_start();
    zassert_equal(ret, 0, "模块启动应返回 0");

    module_status_t status = example_module_a_get_status();
    zassert_equal(status, MODULE_STATUS_RUNNING, "启动后状态应为 RUNNING");

    /* 清理 */
    example_module_a_stop();
    example_module_a_shutdown();
}

/**
 * @brief 测试模块重复启动
 */
ZTEST(example_module_a, test_start_twice) {
    example_module_a_init(NULL);

    /* 第一次启动 */
    zassert_equal(example_module_a_start(), 0, "第一次启动应成功");

    /* 第二次启动应返回错误 */
    int ret = example_module_a_start();
    zassert_true(ret != 0, "重复启动应返回错误");

    /* 清理 */
    example_module_a_stop();
    example_module_a_shutdown();
}

/**
 * @brief 测试模块停止
 */
ZTEST(example_module_a, test_stop) {
    example_module_a_init(NULL);
    example_module_a_start();

    int ret = example_module_a_stop();
    zassert_equal(ret, 0, "模块停止应返回 0");

    module_status_t status = example_module_a_get_status();
    zassert_equal(status, MODULE_STATUS_STOPPED, "停止后状态应为 STOPPED");

    example_module_a_shutdown();
}

/**
 * @brief 测试模块停止后重新启动
 */
ZTEST(example_module_a, test_restart) {
    example_module_a_init(NULL);
    example_module_a_start();
    example_module_a_stop();

    /* 重新启动 */
    int ret = example_module_a_start();
    zassert_equal(ret, 0, "重启应成功");

    module_status_t status = example_module_a_get_status();
    zassert_equal(status, MODULE_STATUS_RUNNING, "重启后状态应为 RUNNING");

    example_module_a_stop();
    example_module_a_shutdown();
}

/* =============================================================================
 * 测试用例：模块控制命令
 * ============================================================================= */

/**
 * @brief 测试设置采样率
 */
ZTEST(example_module_a, test_control_set_rate) {
    example_module_a_init(NULL);

    uint32_t rate = 200;
    int      ret = example_module_a_control(CMD_SET_RATE, &rate);
    zassert_equal(ret, 0, "设置采样率应返回 0");

    /* 验证 */
    uint32_t get_rate = 0;
    ret = example_module_a_control(CMD_GET_RATE, &get_rate);
    zassert_equal(ret, 0, "获取采样率应返回 0");
    zassert_equal(get_rate, 200, "采样率应已更新");

    example_module_a_shutdown();
}

/**
 * @brief 测试获取采样率
 */
ZTEST(example_module_a, test_control_get_rate) {
    example_module_a_config_t config = {.sample_rate_ms = 150, .enable_filtering = true};

    example_module_a_init(&config);

    uint32_t rate = 0;
    int      ret = example_module_a_control(CMD_GET_RATE, &rate);
    zassert_equal(ret, 0, "获取采样率应返回 0");
    zassert_equal(rate, 150, "采样率应匹配配置");

    example_module_a_shutdown();
}

/**
 * @brief 测试重置模块
 *
 * 复位效果可观测：确认产出后让环形缓冲积累待读样本，先停止采集线程消除
 * 并发写入干扰，再执行 RESET——若复位未清空缓冲，此处会读到残留样本。
 */
ZTEST(example_module_a, test_control_reset) {
    example_module_a_config_t config = {.sample_rate_ms = 10, .enable_filtering = true};
    sensor_sample_t           samples[4];
    module_a_get_data_ctx_t   probe = {
        .samples = samples,
        .samples_bytes = sizeof(samples),
        .bytes_read = 0,
    };
    int8_t leftover[64];

    example_module_a_init(&config);
    zassert_equal(example_module_a_start(), 0, "启动应成功");

    /* 确认采样线程确实在产出 */
    zassert_true(ztest_wait_until(module_a_has_sample_data, &probe, MODULE_A_DATA_WAIT_MS), "应先有样本产出");

    /* 静置积累待读样本：10ms 采样周期下 50ms 至少新增 3 条 */
    k_msleep(50);

    zassert_equal(example_module_a_stop(), 0, "停止应成功");
    zassert_equal(example_module_a_control(CMD_RESET, NULL), 0, "重置应返回 0");

    /* 复位效果断言：待读样本应被清空 */
    zassert_equal(example_module_a_get_data(leftover, sizeof(leftover)), 0, "复位后不应残留可读样本");

    example_module_a_shutdown();
}

/**
 * @brief 测试无效命令
 */
ZTEST(example_module_a, test_control_invalid_cmd) {
    example_module_a_init(NULL);

    int ret = example_module_a_control(CMD_INVALID, NULL); /* 无效命令 */
    zassert_true(ret != 0, "无效命令应返回错误");

    example_module_a_shutdown();
}

/**
 * @brief 测试 NULL 参数
 */
ZTEST(example_module_a, test_control_null_arg) {
    example_module_a_init(NULL);

    /* SET_RATE 需要非 NULL 参数 */
    int ret = example_module_a_control(CMD_SET_RATE, NULL);
    zassert_true(ret != 0, "SET_RATE NULL 参数应返回错误");

    /* GET_RATE 需要非 NULL 参数 */
    ret = example_module_a_control(CMD_GET_RATE, NULL);
    zassert_true(ret != 0, "GET_RATE NULL 参数应返回错误");

    example_module_a_shutdown();
}

/* =============================================================================
 * 测试用例：模块 API
 * ============================================================================= */

/**
 * @brief 测试获取接口
 */
ZTEST(example_module_a, test_get_interface) {
    const module_interface_t* iface = example_module_a_get_interface();

    zassert_not_null(iface, "接口不应为 NULL");
    zassert_true(strcmp(iface->name, "example_module_a") == 0, "接口名称应匹配");
    zassert_true(iface->version == MODULE_VERSION(1, 0, 0), "版本号应匹配");
}

/**
 * @brief 测试获取数据 - 空缓冲区
 */
ZTEST(example_module_a, test_get_data_null) {
    example_module_a_init(NULL);
    example_module_a_start();

    /* NULL 数据应返回错误 */
    int ret = example_module_a_get_data(NULL, 100);
    zassert_true(ret < 0, "NULL 数据应返回错误");

    /* 长度为 0 应返回错误 */
    int8_t dummy[64];
    ret = example_module_a_get_data(dummy, 0);
    zassert_true(ret < 0, "长度为 0 应返回错误");

    example_module_a_stop();
    example_module_a_shutdown();
}

/**
 * @brief 测试获取数据 - 正常运行
 */
ZTEST(example_module_a, test_get_data_normal) {
    example_module_a_config_t config = {.sample_rate_ms = 10, /* 10ms 采样率，加快测试 */
                                        .enable_filtering = true};

    example_module_a_init(&config);
    example_module_a_start();

    sensor_sample_t         samples[10];
    module_a_get_data_ctx_t probe = {
        .samples = samples,
        .samples_bytes = sizeof(samples),
        .bytes_read = 0,
    };

    zassert_true(ztest_wait_until(module_a_has_sample_data, &probe, MODULE_A_DATA_WAIT_MS), "应采集到至少一条样本");
    zassert_true(probe.bytes_read > 0, "获取数据应返回正字节数");

    example_module_a_stop();
    example_module_a_shutdown();
}

/**
 * @brief 测试设置采样率 API
 */
ZTEST(example_module_a, test_set_rate) {
    example_module_a_init(NULL);

    /* 有效采样率 */
    int ret = example_module_a_set_rate(100);
    zassert_equal(ret, 0, "设置有效采样率应返回 0");

    /* 无效采样率 */
    ret = example_module_a_set_rate(0);
    zassert_true(ret != 0, "设置 0 采样率应返回错误");

    example_module_a_shutdown();
}

/* =============================================================================
 * 测试用例：事件处理
 * ============================================================================= */

/**
 * @brief 测试事件处理 - SENSOR_CONFIG 事件
 */
ZTEST(example_module_a, test_event_sensor_config) {
    example_module_a_init(NULL);
    example_module_a_start();

    /* 发布配置事件 */
    uint32_t new_rate = 500;
    event_publish_copy(EVENT_TYPE_SENSOR_CONFIG, EVENT_PRIORITY_NORMAL, &new_rate, sizeof(new_rate));

    zassert_true(ztest_wait_until(module_a_sample_rate_is_500, NULL, MODULE_A_DATA_WAIT_MS),
                 "事件处理后采样率应更新为 500");

    uint32_t rate = 0;
    example_module_a_control(CMD_GET_RATE, &rate);
    zassert_equal(rate, 500, "采样率应已更新为 500");

    example_module_a_stop();
    example_module_a_shutdown();
}

/**
 * @brief 测试事件处理 - NULL 事件
 */
ZTEST(example_module_a, test_event_null) {
    /* 传入 NULL 应不崩溃 */
    example_module_a_on_event(NULL, NULL);
    /* 不应崩溃 */
}

/**
 * @brief 测试事件处理 - NULL user_data
 */
ZTEST(example_module_a, test_event_null_user_data) {
    event_t event = {0};
    event.type = EVENT_TYPE_SENSOR_CONFIG;
    event.priority = EVENT_PRIORITY_NORMAL;
    event.data_len = 0;

    /* 传入 NULL user_data 应不崩溃 */
    example_module_a_on_event(&event, NULL);
    /* 不应崩溃 */
}

/* =============================================================================
 * 测试用例：模块状态转换
 * ============================================================================= */

/**
 * @brief 测试完整生命周期
 */
ZTEST(example_module_a, test_lifecycle) {
    module_status_t status;

    /* 1. 初始状态（刚初始化后） */
    example_module_a_init(NULL);
    status = example_module_a_get_status();
    zassert_equal(status, MODULE_STATUS_INITIALIZED, "初始化后应为 INITIALIZED");

    /* 2. 启动 */
    example_module_a_start();
    status = example_module_a_get_status();
    zassert_equal(status, MODULE_STATUS_RUNNING, "启动后应为 RUNNING");

    /* 3. 停止 */
    example_module_a_stop();
    status = example_module_a_get_status();
    zassert_equal(status, MODULE_STATUS_STOPPED, "停止后应为 STOPPED");

    /* 4. 关闭 */
    example_module_a_shutdown();
    status = example_module_a_get_status();
    zassert_equal(status, MODULE_STATUS_UNINITIALIZED, "关闭后应为 UNINITIALIZED");
}

/**
 * @brief 测试未初始化就启动
 */
ZTEST(example_module_a, test_start_without_init) {
    /* 先确保处于未初始化状态 */
    example_module_a_shutdown();

    /* 未初始化就启动应返回错误 */
    int ret = example_module_a_start();
    zassert_true(ret != 0, "未初始化启动应返回错误");
}

/**
 * @brief 测试重复关闭
 */
ZTEST(example_module_a, test_shutdown_twice) {
    example_module_a_init(NULL);
    example_module_a_start();
    example_module_a_stop();

    /* 第一次关闭 */
    int ret = example_module_a_shutdown();
    zassert_equal(ret, 0, "第一次关闭应返回 0");

    /* 第二次关闭应安全（幂等） */
    ret = example_module_a_shutdown();
    zassert_equal(ret, 0, "重复关闭应返回 0");
}

/* =============================================================================
 * 测试用例：线程安全
 * ============================================================================= */

/* 并发工作线程的失败计数：任何 API 返回异常值即置位 */
static atomic_t g_conc_worker_errors;

/* 工作线程栈与线程控制块（静态分配，避免测试路径动态建线程） */
#define MODULE_A_CONC_STACK_SIZE 1024
static K_THREAD_STACK_ARRAY_DEFINE(g_conc_worker_stacks, MODULE_A_CONCURRENT_WORKERS, MODULE_A_CONC_STACK_SIZE);
static struct k_thread g_conc_worker_threads[MODULE_A_CONCURRENT_WORKERS];

static void conc_worker(void* p1, void* p2, void* p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    sensor_sample_t samples[4];

    for (int i = 0; i < MODULE_A_CONCURRENT_ITERS; i++) {
        /* 与采集线程并发读环形缓冲 */
        if (example_module_a_get_data(samples, sizeof(samples)) < 0) {
            atomic_inc(&g_conc_worker_errors);
        }
        /* 与采集线程/事件回调并发写采样率 */
        if (example_module_a_set_rate((uint32_t) (50 + (i % 5) * 10)) != 0) {
            atomic_inc(&g_conc_worker_errors);
        }
        k_yield();
    }
}

/**
 * @brief 测试并发访问
 *
 * 两个工作线程交错调用 get_data/set_rate，与采集线程形成三方并发；
 * 全部汇合后校验零错误返回、模块仍 RUNNING，且控制通道恢复确定状态。
 */
ZTEST(example_module_a, test_concurrent_access) {
    example_module_a_init(NULL);
    zassert_equal(example_module_a_start(), 0, "启动应成功");

    atomic_set(&g_conc_worker_errors, 0);

    for (uintptr_t i = 0; i < MODULE_A_CONCURRENT_WORKERS; i++) {
        k_tid_t tid = k_thread_create(&g_conc_worker_threads[i], g_conc_worker_stacks[i],
                                      K_THREAD_STACK_SIZEOF(g_conc_worker_stacks[i]), conc_worker, (void*) i, NULL,
                                      NULL, 5, 0, K_NO_WAIT);
        k_thread_name_set(tid, "mod_a_conc");
    }

    for (uint32_t i = 0; i < MODULE_A_CONCURRENT_WORKERS; i++) {
        zassert_equal(k_thread_join(&g_conc_worker_threads[i], K_SECONDS(2)), 0, "工作线程应正常退出");
    }

    zassert_equal(atomic_get(&g_conc_worker_errors), 0, "并发调用不应产生错误返回");
    zassert_equal(example_module_a_get_status(), MODULE_STATUS_RUNNING, "并发后模块应仍为 RUNNING");

    /* 汇合后单线程恢复确定状态：写入值不与工作线程取值集合（50..90）重叠 */
    zassert_equal(example_module_a_set_rate(100), 0, NULL);
    uint32_t rate = 0;
    zassert_equal(example_module_a_control(CMD_GET_RATE, &rate), 0, NULL);
    zassert_equal(rate, 100, "汇合后控制通道应恢复确定状态");

    zassert_equal(example_module_a_stop(), 0, NULL);
    zassert_equal(example_module_a_shutdown(), 0, NULL);
}

/* =============================================================================
 * 测试套件
 * ============================================================================= */

ZTEST_SUITE(example_module_a, NULL, test_suite_setup, NULL, NULL, test_suite_teardown);
