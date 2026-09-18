/**
 * @file ota_transport_mcumgr_smp.c
 * @brief MCUmgr SMP 被动 OTA 接入（UART / BLE / UDP 由 Zephyr 多传输承载）
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 4 初始版本
 * 2026-06-13       1.1            zeh            DFU_STARTED 错误传播；取消上传与 disarm
 * 2026-09-18       1.2            zeh            DFU_CHUNK 进度改读 req->off/img_data.len
 *                                                 （upload_check 无 offset/size 字段，旧码
 *                                                 无法编译）；进度终点防回卷校验；
 *                                                 cancel_upload 注明锁序约束
 *
 */

#include <zeplod/ota_transport.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "ota_module_mcumgr.h"

LOG_MODULE_REGISTER(ota_transport_mcumgr_smp, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    bool                 hooks_registered;
    struct mgmt_callback mgmt_cb;
} ota_mcumgr_smp_ctx_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static ota_mcumgr_smp_ctx_t g_mcumgr_ctx;
static ota_transport_ops_t  g_mcumgr_ops;

/* =============================================================================
 * MCUmgr 回调
 * ============================================================================= */

static enum mgmt_cb_return ota_mcumgr_mgmt_cb(uint32_t event, enum mgmt_cb_return prev_status, int32_t* rc,
                                              uint16_t* group, bool* abort_more, void* data, size_t data_size) {
    ARG_UNUSED(group);

    if (prev_status != MGMT_CB_OK) {
        return MGMT_CB_OK;
    }

    switch (event) {
    case MGMT_EVT_OP_IMG_MGMT_DFU_STARTED:
        if (!ota_module_mcumgr_try_claim_session()) {
            if (rc != NULL) {
                *rc = MGMT_ERR_EACCESSDENIED;
            }
            if (abort_more != NULL) {
                *abort_more = true;
            }
            return MGMT_CB_ERROR_RC;
        }
        if (!ota_module_mcumgr_on_dfu_started()) {
            if (rc != NULL) {
                *rc = MGMT_ERR_EUNKNOWN;
            }
            if (abort_more != NULL) {
                *abort_more = true;
            }
            return MGMT_CB_ERROR_RC;
        }
        break;

    case MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK:
        if (!ota_module_mcumgr_is_accepting()) {
            if (rc != NULL) {
                *rc = MGMT_ERR_EACCESSDENIED;
            }
            if (abort_more != NULL) {
                *abort_more = true;
            }
            return MGMT_CB_ERROR_RC;
        }
        if (data != NULL && data_size >= sizeof(struct img_mgmt_upload_check)) {
            const struct img_mgmt_upload_check* chk = (const struct img_mgmt_upload_check*) data;

            /* upload_check 携带 action/req 指针：进度终点 = 本 chunk 偏移 + 数据长度。
               off/len 来自外部 SMP 报文，先防回卷（off + len 溢出即报文非法），拒绝该 chunk */
            if (chk->req != NULL) {
                size_t off = chk->req->off;
                size_t len = chk->req->img_data.len;

                if (off > (SIZE_MAX - len)) {
                    if (rc != NULL) {
                        *rc = MGMT_ERR_EINVAL;
                    }
                    if (abort_more != NULL) {
                        *abort_more = true;
                    }
                    return MGMT_CB_ERROR_RC;
                }
                ota_module_mcumgr_on_chunk_progress(off + len);
            }
        }
        break;

    case MGMT_EVT_OP_IMG_MGMT_DFU_PENDING:
        ota_module_mcumgr_on_dfu_pending();
        break;

    case MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED:
        ota_module_mcumgr_on_dfu_stopped();
        break;

    default:
        break;
    }

    return MGMT_CB_OK;
}

/* =============================================================================
 * 被动接入（镜像由 MCUmgr/img_mgmt 写入 Flash）
 * ============================================================================= */

static int mcumgr_smp_open(ota_transport_ops_t* ops) {
    ota_mcumgr_smp_ctx_t* ctx = (ota_mcumgr_smp_ctx_t*) ops->ctx;

    if (ctx == NULL) {
        return -EINVAL;
    }
    if (ctx->hooks_registered) {
        return 0;
    }

    ctx->mgmt_cb.callback = ota_mcumgr_mgmt_cb;
    ctx->mgmt_cb.event_id = (MGMT_EVT_OP_IMG_MGMT_DFU_STARTED | MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED |
                             MGMT_EVT_OP_IMG_MGMT_DFU_PENDING | MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK);
    mgmt_callback_register(&ctx->mgmt_cb);
    ctx->hooks_registered = true;

    LOG_INF("MCUmgr SMP ingest armed (use mcumgr image upload on enabled transports)");
    return 0;
}

static int mcumgr_smp_write_chunk(ota_transport_ops_t* ops, size_t offset, const uint8_t* data, size_t len) {
    ARG_UNUSED(ops);
    ARG_UNUSED(offset);
    ARG_UNUSED(data);
    ARG_UNUSED(len);
    return -ENOTSUP;
}

static int mcumgr_smp_verify(ota_transport_ops_t* ops) {
    ARG_UNUSED(ops);
    return -ENOTSUP;
}

static int mcumgr_smp_close(ota_transport_ops_t* ops) {
    ota_mcumgr_smp_ctx_t* ctx = (ota_mcumgr_smp_ctx_t*) ops->ctx;

    if (ctx == NULL) {
        return -EINVAL;
    }
    if (!ctx->hooks_registered) {
        return 0;
    }

    mgmt_callback_unregister(&ctx->mgmt_cb);
    ctx->hooks_registered = false;
    LOG_INF("MCUmgr SMP ingest disarmed");
    return 0;
}

static void mcumgr_smp_abort(ota_transport_ops_t* ops) {
    ARG_UNUSED(ops);
    ota_transport_mcumgr_smp_cancel_upload();
    ota_module_mcumgr_on_dfu_stopped();
}

void ota_transport_mcumgr_smp_cancel_upload(void) {
#if defined(CONFIG_MCUMGR_GRP_IMG_MUTEX)
    /* img_mgmt_reset_upload 内部获取 img_mgmt mutex。锁序约束（见 ota_module.c 文件头）：
       本函数不得在 ota 模块持有 g_ota.lock 时调用，否则与
       "img_mgmt 持锁回调 → g_ota.lock" 构成 ABBA 死锁 */
    img_mgmt_reset_upload();
#endif
}

/* =============================================================================
 * 公开 API
 * ============================================================================= */

const ota_transport_ops_t* ota_transport_mcumgr_smp_get(void) {
    g_mcumgr_ops.ctx = &g_mcumgr_ctx;
    g_mcumgr_ops.open = mcumgr_smp_open;
    g_mcumgr_ops.write_chunk = mcumgr_smp_write_chunk;
    g_mcumgr_ops.verify = mcumgr_smp_verify;
    g_mcumgr_ops.close = mcumgr_smp_close;
    g_mcumgr_ops.abort = mcumgr_smp_abort;
    return &g_mcumgr_ops;
}
