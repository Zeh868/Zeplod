/**
 * @file ota_transport_mcuboot.c
 * @brief MCUboot flash_img OTA 传输后端（写入 secondary slot）
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 2 初始版本
 *
 */

#include <zeplod/ota_transport.h>

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(ota_transport_mcuboot, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

/** MCUboot secondary slot 写入上下文 */
typedef struct {
    struct flash_img_context ctx;       /**< Zephyr flash_img 会话 */
    size_t                   write_len; /**< 已顺序写入字节数 */
    bool                     open;      /**< 会话是否打开 */
} ota_mcuboot_ctx_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static ota_mcuboot_ctx_t   g_mcuboot_ctx;
static ota_transport_ops_t g_mcuboot_ops;

/* =============================================================================
 * 前置声明
 * ============================================================================= */

static int  mcuboot_open(ota_transport_ops_t* ops);
static int  mcuboot_write_chunk(ota_transport_ops_t* ops, size_t offset, const uint8_t* data, size_t len);
static int  mcuboot_verify(ota_transport_ops_t* ops);
static int  mcuboot_close(ota_transport_ops_t* ops);
static void mcuboot_abort(ota_transport_ops_t* ops);

/* =============================================================================
 * 传输回调
 * ============================================================================= */

/** 初始化 flash_img，目标为 MCUboot upload slot */
static int mcuboot_open(ota_transport_ops_t* ops) {
    ota_mcuboot_ctx_t* ctx = (ota_mcuboot_ctx_t*) ops->ctx;
    int                rc;

    if (ctx == NULL) {
        return -EINVAL;
    }

    rc = flash_img_init(&ctx->ctx);
    if (rc != 0) {
        LOG_ERR("flash_img_init failed: %d", rc);
        return rc;
    }

    ctx->write_len = 0U;
    ctx->open = true;
    LOG_INF("MCUboot transport open (upload slot %u)", flash_img_get_upload_slot());
    return 0;
}

static int mcuboot_write_chunk(ota_transport_ops_t* ops, size_t offset, const uint8_t* data, size_t len) {
    ota_mcuboot_ctx_t* ctx = (ota_mcuboot_ctx_t*) ops->ctx;
    int                rc;

    if (ctx == NULL || data == NULL || len == 0U) {
        return -EINVAL;
    }
    if (!ctx->open) {
        return -EIO;
    }
    /* flash_img 要求严格顺序写入，offset 须等于已写字节数 */
    if (offset != ctx->write_len) {
        LOG_ERR("MCUboot transport requires sequential writes (off=%zu, expected=%zu)", offset, ctx->write_len);
        return -EINVAL;
    }

    rc = flash_img_buffered_write(&ctx->ctx, data, len, false);
    if (rc != 0) {
        LOG_ERR("flash_img_buffered_write failed: %d", rc);
        return rc;
    }

    ctx->write_len += len;
    return 0;
}

static int mcuboot_verify(ota_transport_ops_t* ops) {
    ota_mcuboot_ctx_t* ctx = (ota_mcuboot_ctx_t*) ops->ctx;
    int                rc;

    if (ctx == NULL || !ctx->open) {
        return -EINVAL;
    }
    if (ctx->write_len == 0U) {
        return -ENODATA;
    }

    /* len=0 + flush=true 将缓冲刷入 flash 并完成镜像收尾 */
    rc = flash_img_buffered_write(&ctx->ctx, NULL, 0U, true);
    if (rc != 0) {
        LOG_ERR("flash_img flush failed: %d", rc);
        return rc;
    }

    if (flash_img_bytes_written(&ctx->ctx) == 0U) {
        return -ENODATA;
    }

    LOG_INF("MCUboot image flushed (%zu bytes written)", ctx->write_len);
    return 0;
}

static int mcuboot_close(ota_transport_ops_t* ops) {
    ota_mcuboot_ctx_t* ctx = (ota_mcuboot_ctx_t*) ops->ctx;
    int                rc;

    if (ctx == NULL) {
        return -EINVAL;
    }

    /* TEST 模式：下次启动试跑新镜像，失败可回滚。
     * 注意：TEST 镜像重启后若未在确认窗口内 boot_write_img_confirmed()
     * （见 ota_module_confirm_image()），再次重启将回滚到旧镜像。 */
    rc = boot_request_upgrade(BOOT_UPGRADE_TEST);
    if (rc != 0) {
        LOG_ERR("boot_request_upgrade failed: %d", rc);
        return rc;
    }

    ctx->open = false;
    LOG_INF("MCUboot test upgrade requested");
    return 0;
}

static void mcuboot_abort(ota_transport_ops_t* ops) {
    ota_mcuboot_ctx_t* ctx = (ota_mcuboot_ctx_t*) ops->ctx;

    if (ctx == NULL) {
        return;
    }

    /* 中止时擦除未完成的 upload slot，避免残留半包镜像 */
    if (ctx->open && ctx->write_len > 0U && ctx->ctx.flash_area != NULL) {
        const int rc = boot_erase_img_bank((uint8_t) ctx->ctx.flash_area->fa_id);

        if (rc != 0) {
            LOG_WRN("boot_erase_img_bank(%u) failed: %d", (unsigned int) ctx->ctx.flash_area->fa_id, rc);
        } else {
            LOG_INF("MCUboot upload slot erased after abort");
        }
    }

    ctx->open = false;
    ctx->write_len = 0U;
    memset(&ctx->ctx, 0, sizeof(ctx->ctx));
}

/* =============================================================================
 * 公开 API
 * ============================================================================= */

const ota_transport_ops_t* ota_transport_mcuboot_get(void) {
    g_mcuboot_ops.ctx = &g_mcuboot_ctx;
    g_mcuboot_ops.open = mcuboot_open;
    g_mcuboot_ops.write_chunk = mcuboot_write_chunk;
    g_mcuboot_ops.verify = mcuboot_verify;
    g_mcuboot_ops.close = mcuboot_close;
    g_mcuboot_ops.abort = mcuboot_abort;
    return &g_mcuboot_ops;
}
