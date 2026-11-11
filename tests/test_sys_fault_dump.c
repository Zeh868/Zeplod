/**
 * @file test_sys_fault_dump.c
 * @brief sys_fault_dump 服务单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            初始版本
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <string.h>

#include <zeplod/sys_fault_dump.h>

ZTEST_SUITE(sys_fault_dump_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(sys_fault_dump_tests, test_record_and_export) {
    static const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    /* 环按默认 16×(记录头+payload) 已超过 512；用静态缓冲避免吃掉 ztest 栈。 */
    static uint8_t buf[8192];
    size_t         n = 0U;

    zassert_equal(sys_fault_dump_init(), 0, NULL);
    sys_fault_dump_clear();

    zassert_equal(sys_fault_dump_record(FAULT_DUMP_KIND_MODULE_ERROR, payload, sizeof(payload)), 0, NULL);
    zassert_equal(sys_fault_dump_export(buf, sizeof(buf), &n), 0, NULL);
    zassert_true(n >= sizeof(uint32_t) * 2U, NULL);
}

ZTEST(sys_fault_dump_tests, test_export_buffer_too_small) {
    uint8_t tiny[8];

    zassert_equal(sys_fault_dump_init(), 0, NULL);
    sys_fault_dump_clear();
    zassert_equal(sys_fault_dump_record(FAULT_DUMP_KIND_WDT_PRE_EXPIRE, NULL, 0U), 0, NULL);
    zassert_equal(sys_fault_dump_export(tiny, sizeof(tiny), NULL), -ENOMEM, NULL);
}

ZTEST(sys_fault_dump_tests, test_clear_ring) {
    zassert_equal(sys_fault_dump_init(), 0, NULL);
    zassert_equal(sys_fault_dump_record(FAULT_DUMP_KIND_OTA_ERROR, NULL, 0U), 0, NULL);
    sys_fault_dump_clear();
    zassert_equal(sys_fault_dump_record(FAULT_DUMP_KIND_MODULE_ERROR, NULL, 0U), 0, NULL);
}
