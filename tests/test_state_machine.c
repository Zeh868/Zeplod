/**
 * @file test_state_machine.c
 * @brief 状态机辅助器单元测试（默认/自定义后端、合法与非法迁移）
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-05-23
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-05-23       1.0            zeh             初始版本
 * 2026-09-18       1.1            zeh             补全文件头模板
 *
 */

#include <zephyr/ztest.h>

#include <errno.h>

#include <zeplod/state_machine.h>

typedef struct {
    const char* boot_name;
    const char* live_name;
} custom_backend_context_t;

static bool custom_backend_can_transition(const zepl_state_machine_t* machine, zepl_state_t from, zepl_state_t to) {
    (void) machine;

    return from == ZEP_STATE_UNINIT && to == ZEP_STATE_RUNNING;
}

static int custom_backend_try_transition(zepl_state_machine_t* machine, zepl_state_t next_state) {
    if (machine == NULL) {
        return -EINVAL;
    }

    if (!custom_backend_can_transition(machine, machine->current, next_state)) {
        return -EPERM;
    }

    machine->current = next_state;
    return 0;
}

static const char* custom_backend_state_name(const zepl_state_machine_t* machine, zepl_state_t state) {
    const custom_backend_context_t* ctx = zepl_state_machine_get_backend_context(machine);

    if (ctx == NULL) {
        return "CUSTOM";
    }

    switch (state) {
    case ZEP_STATE_UNINIT:
        return ctx->boot_name != NULL ? ctx->boot_name : "BOOT";
    case ZEP_STATE_RUNNING:
        return ctx->live_name != NULL ? ctx->live_name : "LIVE";
    default:
        return "CUSTOM";
    }
}

static bool custom_backend_is_terminal(const zepl_state_machine_t* machine, zepl_state_t state) {
    (void) machine;

    return state == ZEP_STATE_RUNNING;
}

static const zepl_state_machine_backend_t g_custom_backend = {
    .can_transition = custom_backend_can_transition,
    .try_transition = custom_backend_try_transition,
    .state_name = custom_backend_state_name,
    .is_terminal = custom_backend_is_terminal,
};

ZTEST(state_machine, test_state_machine_progression) {
    zepl_state_machine_t machine;

    zepl_state_machine_init(&machine, ZEP_STATE_UNINIT);

    zassert_equal(zepl_state_machine_get(&machine), ZEP_STATE_UNINIT, NULL);
    zassert_true(zepl_state_can_transition(ZEP_STATE_UNINIT, ZEP_STATE_INITED), NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_INITED), 0, NULL);
    zassert_equal(zepl_state_machine_get(&machine), ZEP_STATE_INITED, NULL);

    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_STARTING), 0, NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_RUNNING), 0, NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_STOPPING), 0, NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_STOPPED), 0, NULL);
    zassert_true(zepl_state_can_transition(ZEP_STATE_STOPPED, ZEP_STATE_UNINIT), NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_UNINIT), 0, NULL);
    zassert_equal(zepl_state_machine_get(&machine), ZEP_STATE_UNINIT, NULL);

    zassert_true(zepl_state_is_terminal(ZEP_STATE_STOPPED), NULL);
    zassert_str_equal(zepl_state_name(ZEP_STATE_STOPPED), "STOPPED", NULL);
}

ZTEST(state_machine, test_state_machine_backend_override) {
    zepl_state_machine_t                  machine;
    static const custom_backend_context_t context = {
        .boot_name = "BOOT",
        .live_name = "LIVE",
    };

    zepl_state_machine_init_with_backend(&machine, ZEP_STATE_UNINIT, &g_custom_backend, (void*) &context);

    zassert_equal(zepl_state_machine_get_backend(&machine), &g_custom_backend, NULL);
    zassert_equal(zepl_state_machine_get_backend_context(&machine), (void*) &context, NULL);
    zassert_str_equal(zepl_state_machine_get_name(&machine, ZEP_STATE_UNINIT), "BOOT", NULL);
    zassert_true(zepl_state_machine_can_transition(&machine, ZEP_STATE_UNINIT, ZEP_STATE_RUNNING), NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_RUNNING), 0, NULL);
    zassert_equal(zepl_state_machine_get(&machine), ZEP_STATE_RUNNING, NULL);
    zassert_true(zepl_state_machine_is_terminal(&machine, ZEP_STATE_RUNNING), NULL);
    zassert_str_equal(zepl_state_machine_get_name(&machine, ZEP_STATE_RUNNING), "LIVE", NULL);
    zassert_false(zepl_state_machine_can_transition(&machine, ZEP_STATE_RUNNING, ZEP_STATE_STOPPED), NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_STOPPED), -EPERM, NULL);
}

ZTEST(state_machine, test_state_machine_invalid_transition) {
    zepl_state_machine_t machine;

    zepl_state_machine_init(&machine, ZEP_STATE_RUNNING);

    zassert_false(zepl_state_can_transition(ZEP_STATE_RUNNING, ZEP_STATE_INITED), NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_INITED), -EPERM, NULL);
    zassert_equal(zepl_state_machine_get(&machine), ZEP_STATE_RUNNING, NULL);
}

ZTEST(state_machine, test_state_machine_shutdown_transition) {
    zepl_state_machine_t machine;

    zepl_state_machine_init(&machine, ZEP_STATE_INITED);

    zassert_true(zepl_state_can_transition(ZEP_STATE_INITED, ZEP_STATE_UNINIT), NULL);
    zassert_equal(zepl_state_machine_try_transition(&machine, ZEP_STATE_UNINIT), 0, NULL);
    zassert_equal(zepl_state_machine_get(&machine), ZEP_STATE_UNINIT, NULL);
}

ZTEST_SUITE(state_machine, NULL, NULL, NULL, NULL, NULL);
