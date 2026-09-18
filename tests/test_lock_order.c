/**
 * @file test_lock_order.c
 * @brief Unit tests for lock ordering helper.
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-12
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-06-12       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            补全文件头模板（author/version/date/修改日志）
 */

#include <zephyr/ztest.h>

#include <zeplod/lock_order.h>

static void lock_order_suite_teardown(void* fixture) {
    ARG_UNUSED(fixture);
    zepl_lock_reset_current_thread();
}

ZTEST(lock_order, test_valid_lock_order_sequence) {
    zepl_lock_reset_current_thread();

    zassert_true(zepl_lock_order_is_valid(ZEP_LOCK_LEVEL_STATE, 0x100U), NULL);
    zepl_lock_enter(ZEP_LOCK_LEVEL_STATE, 0x100U);
    zassert_equal(zepl_lock_current_depth(), 1U, NULL);
    zassert_equal(zepl_lock_current_level(), ZEP_LOCK_LEVEL_STATE, NULL);

    zassert_true(zepl_lock_order_is_valid(ZEP_LOCK_LEVEL_STATE, 0x200U), NULL);
    zepl_lock_enter(ZEP_LOCK_LEVEL_STATE, 0x200U);
    zassert_equal(zepl_lock_current_depth(), 2U, NULL);

    zassert_true(zepl_lock_order_is_valid(ZEP_LOCK_LEVEL_TABLE, 0x050U), NULL);
    zepl_lock_enter(ZEP_LOCK_LEVEL_TABLE, 0x050U);
    zassert_equal(zepl_lock_current_depth(), 3U, NULL);
    zassert_equal(zepl_lock_current_level(), ZEP_LOCK_LEVEL_TABLE, NULL);

    zepl_lock_exit(ZEP_LOCK_LEVEL_TABLE, 0x050U);
    zepl_lock_exit(ZEP_LOCK_LEVEL_STATE, 0x200U);
    zepl_lock_exit(ZEP_LOCK_LEVEL_STATE, 0x100U);

    zassert_equal(zepl_lock_current_depth(), 0U, NULL);
}

ZTEST(lock_order, test_invalid_lock_order_sequence) {
    zepl_lock_reset_current_thread();

    zepl_lock_enter(ZEP_LOCK_LEVEL_TABLE, 0x200U);

    zassert_false(zepl_lock_order_is_valid(ZEP_LOCK_LEVEL_STATE, 0x100U), NULL);
    zassert_false(zepl_lock_order_is_valid(ZEP_LOCK_LEVEL_TABLE, 0x100U), NULL);

    zepl_lock_exit(ZEP_LOCK_LEVEL_TABLE, 0x200U);
}

ZTEST(lock_order, test_ipc_service_lock_nesting_order) {
    /* 与 ipc_service：state_lock(L2) -> pending_lock(L3) -> block->lock(L4) 一致 */
    zepl_lock_reset_current_thread();

    zassert_true(zepl_lock_order_is_valid(ZEP_LOCK_LEVEL_STATE, 0xA00U), NULL);
    zepl_lock_enter(ZEP_LOCK_LEVEL_STATE, 0xA00U);

    zassert_true(zepl_lock_order_is_valid(ZEP_LOCK_LEVEL_TABLE, 0xB00U), NULL);
    zepl_lock_enter(ZEP_LOCK_LEVEL_TABLE, 0xB00U);

    zassert_true(zepl_lock_order_is_valid(ZEP_LOCK_LEVEL_ENTRY, 0xC00U), NULL);
    zepl_lock_enter(ZEP_LOCK_LEVEL_ENTRY, 0xC00U);

    zepl_lock_exit(ZEP_LOCK_LEVEL_ENTRY, 0xC00U);
    zepl_lock_exit(ZEP_LOCK_LEVEL_TABLE, 0xB00U);
    zepl_lock_exit(ZEP_LOCK_LEVEL_STATE, 0xA00U);

    zassert_equal(zepl_lock_current_depth(), 0U, NULL);
}

ZTEST_SUITE(lock_order, NULL, NULL, NULL, NULL, lock_order_suite_teardown);
