/**
 * @file test_sys_secure_kv.c
 * @brief sys_secure_kv 服务单元测试
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            get 入参改本地可写缓冲，去除对
 *                                                 const 数据的类型压制
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <errno.h>
#include <string.h>

#include <zeplod/sys_secure_kv.h>

ZTEST_SUITE(sys_secure_kv_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(sys_secure_kv_tests, test_init_idempotent) {
    zassert_equal(sys_secure_kv_init(), 0, NULL);
    zassert_equal(sys_secure_kv_init(), 0, NULL);
}

ZTEST(sys_secure_kv_tests, test_set_get_roundtrip) {
    static const uint8_t secret[] = "zeplod_secret_token_v1";
    uint8_t              out[32];
    size_t               n = 0U;

    zassert_equal(sys_secure_kv_init(), 0, NULL);
    sys_secure_kv_clear();

    zassert_equal(sys_secure_kv_set("wifi_psk", secret, sizeof(secret)), 0, NULL);
    zassert_true(sys_secure_kv_has("wifi_psk"), NULL);

    zassert_equal(sys_secure_kv_get("wifi_psk", out, sizeof(out), &n), 0, NULL);
    zassert_equal(n, sizeof(secret), NULL);
    zassert_mem_equal(out, secret, sizeof(secret), NULL);
}

ZTEST(sys_secure_kv_tests, test_remove_and_missing_key) {
    static const uint8_t data[] = {0x01, 0x02};
    uint8_t              scratch[4]; /* 本地可写缓冲：get 输出参数不可指向 const 数据 */

    zassert_equal(sys_secure_kv_init(), 0, NULL);
    sys_secure_kv_clear();

    zassert_equal(sys_secure_kv_set("token", data, sizeof(data)), 0, NULL);
    zassert_equal(sys_secure_kv_remove("token"), 0, NULL);
    zassert_false(sys_secure_kv_has("token"), NULL);
    zassert_equal(sys_secure_kv_get("token", scratch, sizeof(scratch), NULL), -ENOENT, NULL);
}

ZTEST(sys_secure_kv_tests, test_clear_all) {
    static const uint8_t data[] = {0xAA};

    zassert_equal(sys_secure_kv_init(), 0, NULL);
    zassert_equal(sys_secure_kv_set("a", data, sizeof(data)), 0, NULL);
    zassert_equal(sys_secure_kv_set("b", data, sizeof(data)), 0, NULL);

    sys_secure_kv_clear();
    zassert_false(sys_secure_kv_has("a"), NULL);
    zassert_false(sys_secure_kv_has("b"), NULL);
}

ZTEST(sys_secure_kv_tests, test_get_buffer_too_small) {
    static const uint8_t secret[] = "long_secret_value_for_secure_kv";
    uint8_t              out[4];

    zassert_equal(sys_secure_kv_init(), 0, NULL);
    sys_secure_kv_clear();
    zassert_equal(sys_secure_kv_set("token", secret, sizeof(secret)), 0, NULL);
    zassert_equal(sys_secure_kv_get("token", out, sizeof(out), NULL), -ENOMEM, NULL);
}

ZTEST(sys_secure_kv_tests, test_reject_empty_value) {
    static const uint8_t byte = 0x01U;

    zassert_equal(sys_secure_kv_init(), 0, NULL);
    sys_secure_kv_clear();
    zassert_equal(sys_secure_kv_set("empty", &byte, 0U), -EINVAL, NULL);
}
