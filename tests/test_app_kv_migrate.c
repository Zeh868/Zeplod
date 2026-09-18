/**
 * @file test_app_kv_migrate.c
 * @brief app_kv schema 迁移钩子单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            新增恰好走满 MAX_STEPS 迁移链用例
 *                                                 （off-by-one 修复防复发）
 *
 */

#include <zephyr/ztest.h>

#include <zeplod/app_kv.h>

ZTEST_SUITE(app_kv_migrate_tests, NULL, NULL, NULL, NULL, NULL);

static int g_migrate_called;
static int g_chain_step_calls;

static int migrate_stub(uint32_t from_ver, uint32_t to_ver, void* user_data) {
    int* called = (int*) user_data;

    zassert_equal(from_ver, 1U, NULL);
    zassert_equal(to_ver, 2U, NULL);
    zassert_not_null(called, NULL);
    (*called)++;
    return 0;
}

/* 满链用例的通用步进桩：版本迁移本身由框架完成，桩只负责计数与防重入 */
static int chain_step_stub(uint32_t from_ver, uint32_t to_ver, void* user_data) {
    ARG_UNUSED(from_ver);
    ARG_UNUSED(to_ver);
    ARG_UNUSED(user_data);

    g_chain_step_calls++;
    zassert_true(g_chain_step_calls <= APP_KV_MIGRATE_MAX_STEPS, "迁移步被重复执行（疑似环链）");
    return 0;
}

static void setup_kv(void) {
    app_kv_init();
    /* 迁移表是进程级静态状态且无注销接口，用例间必须重置（产品构建无此钩子） */
    app_kv_test_reset_migrations();
    g_migrate_called = 0;
    g_chain_step_calls = 0;
}

ZTEST(app_kv_migrate_tests, test_migrate_callback) {
    setup_kv();

    zassert_equal(app_kv_register_migrate(1U, 2U, migrate_stub, &g_migrate_called), APP_OK, NULL);
    zassert_equal(app_kv_set_schema_version(1U), APP_OK, NULL);
    zassert_equal(app_kv_run_migrations(), APP_OK, NULL);
    zassert_equal(g_migrate_called, 1, NULL);
    zassert_equal(app_kv_get_schema_version(), 2U, NULL);
}

ZTEST(app_kv_migrate_tests, test_migrate_chain) {
    setup_kv();

    zassert_equal(app_kv_register_migrate(1U, 2U, migrate_stub, &g_migrate_called), APP_OK, NULL);
    zassert_equal(app_kv_set_schema_version(1U), APP_OK, NULL);
    zassert_equal(app_kv_run_migrations(), APP_OK, NULL);
    zassert_equal(app_kv_run_migrations(), APP_OK, NULL);
    zassert_equal(g_migrate_called, 1, NULL);
    zassert_equal(app_kv_get_schema_version(), 2U, NULL);
}

ZTEST(app_kv_migrate_tests, test_register_invalid_range) {
    setup_kv();
    zassert_equal(app_kv_register_migrate(2U, 2U, migrate_stub, &g_migrate_called), APP_ERR_INVALID_PARAM, NULL);
    zassert_equal(app_kv_register_migrate(3U, 1U, migrate_stub, &g_migrate_called), APP_ERR_INVALID_PARAM, NULL);
}

ZTEST(app_kv_migrate_tests, test_register_duplicate) {
    setup_kv();
    zassert_equal(app_kv_register_migrate(1U, 2U, migrate_stub, &g_migrate_called), APP_OK, NULL);
    zassert_equal(app_kv_register_migrate(1U, 2U, migrate_stub, &g_migrate_called), APP_ERR_ALREADY_EXISTS, NULL);
}

/**
 * @brief 恰好走满 APP_KV_MIGRATE_MAX_STEPS 步的链应成功（off-by-one 防复发）
 *
 * 历史缺陷：run_migrations 循环把"走满上限"误判为异常链而失败。修复后契约：
 * 链长等于步数上限时全部执行并返回 APP_OK，版本到达链终值。
 */
ZTEST(app_kv_migrate_tests, test_chain_exactly_max_steps_succeeds) {
    setup_kv();

    /* 注册 1→2→…→(MAX_STEPS+1) 的满额链，占满全部迁移槽位 */
    for (uint32_t v = 1U; v <= APP_KV_MIGRATE_MAX_STEPS; v++) {
        zassert_equal(app_kv_register_migrate(v, v + 1U, chain_step_stub, NULL), APP_OK, "注册第 %u 步失败",
                      (unsigned int) v);
    }

    zassert_equal(app_kv_set_schema_version(1U), APP_OK, NULL);
    zassert_equal(app_kv_run_migrations(), APP_OK, "恰好走满步数的迁移链应返回成功");
    zassert_equal(g_chain_step_calls, APP_KV_MIGRATE_MAX_STEPS, "每步应恰好执行一次");
    zassert_equal(app_kv_get_schema_version(), (uint32_t) APP_KV_MIGRATE_MAX_STEPS + 1U, "版本号应到达链终值");

    /* 链耗尽后重复执行应幂等成功，且不再触发任何迁移步 */
    zassert_equal(app_kv_run_migrations(), APP_OK, NULL);
    zassert_equal(g_chain_step_calls, APP_KV_MIGRATE_MAX_STEPS, "幂等重跑不应再执行迁移步");
}
