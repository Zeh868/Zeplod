/**
 * @file remote_ops_backend_null.c
 * @brief Null 远程运维后端（RAM 缓存最近 JSON）
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 5 初始版本
 * 2026-09-18       1.1            zeh            补文件修改日志
 *
 */

#include <zeplod/remote_ops_backend.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/** RAM 环形缓存：保存最近一次导出的诊断 JSON（测试用） */
typedef struct {
    char   buf[CONFIG_REMOTE_OPS_EXPORT_BUF_SIZE];
    size_t len; /**< 有效 JSON 长度（不含 '\0'） */
    bool   inited;
} remote_ops_null_ctx_t;

static remote_ops_null_ctx_t    g_null_ctx;
static remote_ops_backend_ops_t g_null_ops;

static int null_init(remote_ops_backend_ops_t* ops) {
    remote_ops_null_ctx_t* ctx = (remote_ops_null_ctx_t*) ops->ctx;

    if (ctx == NULL) {
        return -EINVAL;
    }

    ctx->len = 0U;
    ctx->buf[0] = '\0';
    ctx->inited = true;
    return 0;
}

static int null_deinit(remote_ops_backend_ops_t* ops) {
    remote_ops_null_ctx_t* ctx = (remote_ops_null_ctx_t*) ops->ctx;

    if (ctx == NULL) {
        return -EINVAL;
    }

    ctx->len = 0U;
    ctx->buf[0] = '\0';
    ctx->inited = false;
    return 0;
}

static int null_get_last_export(remote_ops_backend_ops_t* ops, char* out, size_t out_len) {
    remote_ops_null_ctx_t* ctx = (remote_ops_null_ctx_t*) ops->ctx;

    if (ctx == NULL || !ctx->inited || ctx->len == 0U) {
        return -ENOENT;
    }
    if (out == NULL || out_len == 0U) {
        return -EINVAL;
    }
    if (ctx->len >= out_len) {
        return -ENOMEM;
    }

    memcpy(out, ctx->buf, ctx->len + 1U);
    return 0;
}

/** 将 JSON 写入 RAM 缓存，供 get_last_export 读取 */
static int null_export_diag(remote_ops_backend_ops_t* ops, const char* json, size_t json_len) {
    remote_ops_null_ctx_t* ctx = (remote_ops_null_ctx_t*) ops->ctx;

    if (ctx == NULL || !ctx->inited || json == NULL) {
        return -EINVAL;
    }
    if (json_len >= sizeof(ctx->buf)) {
        return -ENOMEM;
    }

    memcpy(ctx->buf, json, json_len);
    ctx->buf[json_len] = '\0';
    ctx->len = json_len;
    return 0;
}

const remote_ops_backend_ops_t* remote_ops_backend_null_get(void) {
    g_null_ops.ctx = &g_null_ctx;
    g_null_ops.init = null_init;
    g_null_ops.deinit = null_deinit;
    g_null_ops.export_diag = null_export_diag;
    g_null_ops.get_last_export = null_get_last_export;
    return &g_null_ops;
}
