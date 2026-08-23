/**
 * @file ota_module.c
 * @brief OTA 模块实现
 * @author zeh (china_qzh@163.com)
 * @version 1.5
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 1 初始版本
 * 2026-06-13       1.1            zeh            幂等 init、锁序、锁外发事件
 * 2026-06-13       1.2            zeh            传输后端 Kconfig；request_reboot
 * 2026-06-13       1.3            zeh            MCUmgr SMP 被动接入桥接
 * 2026-06-13       1.4            zeh            双 ingest 路径；会话互斥
 * 2026-06-13       1.5            zeh            start 回滚；MCUmgr abort/错误传播
 *
 */

#include <zeplod/ota_module.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#if IS_ENABLED(CONFIG_OTA_TRANSPORT_MCUMGR_SMP)
#include <zephyr/dfu/mcuboot.h>
#include "ota_module_mcumgr.h"
#elif IS_ENABLED(CONFIG_MCUBOOT_IMG_MANAGER)
#include <zephyr/dfu/mcuboot.h>
#endif

#include <errno.h>
#include <string.h>

#include <zeplod/app_config.h>
#include <zeplod/lock_order.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(ota_module, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef enum {
    OTA_INGEST_NONE = 0, /**< 无进行中的写入会话 */
    OTA_INGEST_MCUMGR,   /**< SMP 被动上传占用 */
    OTA_INGEST_ACTIVE,   /**< ota_module_begin_download 主动路径 */
} ota_ingest_owner_t;

typedef struct {
    ota_sm_t                   sm;
    const ota_transport_ops_t* active_transport; /**< Kconfig 选定的主动传输后端 */
    ota_ingest_owner_t         ingest_owner;
    module_status_t            status;
    size_t                     image_size; /**< 当前镜像已收字节（进度估算用） */
    struct k_mutex             lock;       /**< 保护状态字段（声明顺序须在 xfer_lock 之前，
                                                保证锁序 key 单调递增） */
    struct k_mutex             xfer_lock;  /**< 序列化传输层慢速操作（write/abort），见
                                                ota_module_write_chunk */
    bool                       lock_ready;
    bool                       events_registered;
    bool                       session_open; /**< 传输层 open 且未完成 close/abort */
} ota_module_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static ota_module_cb_t g_ota;

/* =============================================================================
 * 前置声明
 * ============================================================================= */

static void ota_lock(void);
static void ota_unlock(void);
static void ota_xfer_abort_locked(const ota_transport_ops_t* transport);
static void ota_build_progress(ota_progress_t* prog, ota_state_t state, int error_code);
static int  ota_publish_progress(const ota_progress_t* prog);
static void ota_reset_session_locked(void);
static int  ota_transport_fail_locked(int err, ota_progress_t* out_prog);
static int  ota_register_event_types(void);

/* =============================================================================
 * 锁与内部辅助
 * ============================================================================= */

static void ota_lock(void) {
    zepl_lock_enter(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_ota.lock);
    k_mutex_lock(&g_ota.lock, K_FOREVER);
}

static void ota_unlock(void) {
    k_mutex_unlock(&g_ota.lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_ota.lock);
}

/**
 * @brief 在持有 g_ota.lock 时序列化执行传输层 abort
 *
 * xfer_lock 与 ota_module_write_chunk 的锁外传输写互斥：write 期间并发 abort 不
 * 会在传输层（如 flash_img）产生并发操作。锁序 g_ota.lock → g_ota.xfer_lock：
 * 同层（RESOURCE）key 须单调不减，两成员按声明顺序排布保证地址递增。
 */
static void ota_xfer_abort_locked(const ota_transport_ops_t* transport) {
    if (transport == NULL || transport->abort == NULL) {
        return;
    }

    zepl_lock_enter(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_ota.xfer_lock);
    k_mutex_lock(&g_ota.xfer_lock, K_FOREVER);
    transport->abort((ota_transport_ops_t*) transport);
    k_mutex_unlock(&g_ota.xfer_lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_ota.xfer_lock);
}

/** 按状态与已写字节估算进度百分比 */
static uint8_t ota_percent_for_state(ota_state_t state, size_t image_size) {
    const size_t cap = (size_t) CONFIG_OTA_EXPECTED_IMAGE_SIZE;

    switch (state) {
    case OTA_STATE_IDLE:
        return 0U;
    case OTA_STATE_DOWNLOADING:
        if (cap == 0U) {
            return 0U;
        }
        if (image_size >= cap) {
            return 99U;
        }
        return (uint8_t) ((image_size * 100U) / cap);
    case OTA_STATE_VERIFYING:
        return 95U;
    case OTA_STATE_READY_REBOOT:
        return 100U;
    case OTA_STATE_ERROR:
    default:
        return 0U;
    }
}

static void ota_build_progress(ota_progress_t* prog, ota_state_t state, int error_code) {
    prog->state = state;
    prog->percent = ota_percent_for_state(state, g_ota.image_size);
    prog->error_code = error_code;
}

/** 发布 OTA 状态与进度事件（锁外调用） */
static int ota_publish_progress(const ota_progress_t* prog) {
    event_status_t st;

    st = event_publish_copy(EVENT_OTA_STATE_CHANGED, EVENT_PRIORITY_NORMAL, prog, sizeof(*prog));
    if (st != EVENT_OK) {
        LOG_WRN("OTA state event publish failed: %d", st);
        return -EIO;
    }

    st = event_publish_copy(EVENT_OTA_PROGRESS, EVENT_PRIORITY_NORMAL, prog, sizeof(*prog));
    if (st != EVENT_OK) {
        LOG_WRN("OTA progress event publish failed: %d", st);
        return -EIO;
    }

    return 0;
}

/**
 * @brief 中止当前会话并复位状态机
 *
 * 持锁调用：按 ingest 来源取消 MCUmgr 或主动传输，清空 image_size。
 */
static void ota_reset_session_locked(void) {
#if IS_ENABLED(CONFIG_OTA_TRANSPORT_MCUMGR_SMP)
    if (g_ota.session_open && g_ota.ingest_owner == OTA_INGEST_MCUMGR) {
        ota_transport_mcumgr_smp_cancel_upload();
    }
#endif
    if (g_ota.session_open && g_ota.ingest_owner == OTA_INGEST_ACTIVE) {
        ota_xfer_abort_locked(g_ota.active_transport);
    }
    g_ota.session_open = false;
    g_ota.ingest_owner = OTA_INGEST_NONE;
    ota_sm_init(&g_ota.sm);
    g_ota.image_size = 0U;
}

/** 传输失败路径：转 ERROR、abort 传输、释放 ingest（持锁） */
static int ota_transport_fail_locked(int err, ota_progress_t* out_prog) {
    (void) ota_sm_on_error(&g_ota.sm, err);
    ota_build_progress(out_prog, OTA_STATE_ERROR, err);
    if (g_ota.ingest_owner == OTA_INGEST_ACTIVE) {
        ota_xfer_abort_locked(g_ota.active_transport);
    }
    g_ota.session_open = false;
    g_ota.ingest_owner = OTA_INGEST_NONE;
    return err;
}

static int ota_register_event_types(void) {
    event_status_t st;

    if (g_ota.events_registered) {
        return 0;
    }

    st = event_register_type(EVENT_OTA_STATE_CHANGED, "ota_state");
    if (st != EVENT_OK) {
        LOG_ERR("Failed to register EVENT_OTA_STATE_CHANGED: %d", st);
        return -EIO;
    }

    st = event_register_type(EVENT_OTA_PROGRESS, "ota_progress");
    if (st != EVENT_OK) {
        LOG_ERR("Failed to register EVENT_OTA_PROGRESS: %d", st);
        return -EIO;
    }

    g_ota.events_registered = true;
    return 0;
}

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int ota_module_init(void* config) {
    int ret;

    ARG_UNUSED(config);

    if (g_ota.status != MODULE_STATUS_UNINITIALIZED) {
        return 0;
    }

    if (!g_ota.lock_ready) {
        k_mutex_init(&g_ota.lock);
        k_mutex_init(&g_ota.xfer_lock);
        g_ota.lock_ready = true;
    }

    ota_sm_init(&g_ota.sm);
#if IS_ENABLED(CONFIG_OTA_TRANSPORT_NULL)
    g_ota.active_transport = ota_transport_null_get();
#elif IS_ENABLED(CONFIG_OTA_TRANSPORT_ACTIVE)
    g_ota.active_transport = ota_transport_mcuboot_get();
#else
    g_ota.active_transport = NULL;
#endif
    g_ota.ingest_owner = OTA_INGEST_NONE;
    g_ota.image_size = 0U;
    g_ota.session_open = false;
    g_ota.status = MODULE_STATUS_INITIALIZED;

    ret = ota_register_event_types();
    if (ret != 0) {
        g_ota.status = MODULE_STATUS_UNINITIALIZED;
        return ret;
    }

    LOG_INF("OTA module initialized (mcumgr_smp=%d active=%d null=%d)",
#if IS_ENABLED(CONFIG_OTA_TRANSPORT_MCUMGR_SMP)
            1,
#else
            0,
#endif
#if IS_ENABLED(CONFIG_OTA_TRANSPORT_ACTIVE)
            1,
#else
            0,
#endif
#if IS_ENABLED(CONFIG_OTA_TRANSPORT_NULL)
            1);
#else
            0);
#endif
    return 0;
}

int ota_module_start(void) {
    LOG_INF("Starting OTA module...");

    ota_lock();
    if (g_ota.status == MODULE_STATUS_UNINITIALIZED) {
        ota_unlock();
        return APP_ERR_INIT;
    }
    if (g_ota.status == MODULE_STATUS_RUNNING) {
        ota_unlock();
        return 0;
    }
    ota_unlock();

#if IS_ENABLED(CONFIG_OTA_TRANSPORT_MCUMGR_SMP)
    /* 锁外 arm SMP 钩子；失败须回滚已打开的 ingest */
    {
        const ota_transport_ops_t* smp = ota_transport_mcumgr_smp_get();
        int                        hook_ret;

        if (smp != NULL && smp->open != NULL) {
            hook_ret = smp->open((ota_transport_ops_t*) smp);
            if (hook_ret != 0) {
                LOG_ERR("MCUmgr SMP ingest arm failed: %d", hook_ret);
                return APP_ERR_OTA_TRANSPORT;
            }
        }
    }
#endif

    ota_lock();
    if (g_ota.status == MODULE_STATUS_UNINITIALIZED) {
        ota_unlock();
#if IS_ENABLED(CONFIG_OTA_TRANSPORT_MCUMGR_SMP)
        /* start 中途失败：关闭已 arm 的 SMP 钩子 */
        {
            const ota_transport_ops_t* smp = ota_transport_mcumgr_smp_get();

            if (smp != NULL && smp->close != NULL) {
                (void) smp->close((ota_transport_ops_t*) smp);
            }
        }
#endif
        return APP_ERR_INIT;
    }
    g_ota.status = MODULE_STATUS_RUNNING;
    ota_unlock();

    LOG_INF("OTA module started");
    return 0;
}

int ota_module_stop(void) {
    LOG_INF("Stopping OTA module...");

    ota_lock();
    ota_reset_session_locked();
    g_ota.status = MODULE_STATUS_STOPPED;
    ota_unlock();

#if IS_ENABLED(CONFIG_OTA_TRANSPORT_MCUMGR_SMP)
    {
        const ota_transport_ops_t* smp = ota_transport_mcumgr_smp_get();

        if (smp != NULL && smp->close != NULL) {
            (void) smp->close((ota_transport_ops_t*) smp);
        }
    }
#endif

    LOG_INF("OTA module stopped");
    return 0;
}

int ota_module_shutdown(void) {
    (void) ota_module_stop();

    ota_lock();
    g_ota.status = MODULE_STATUS_UNINITIALIZED;
    ota_unlock();

    LOG_INF("OTA module shutdown");
    return 0;
}

void ota_module_on_event(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
}

module_status_t ota_module_get_status(void) {
    module_status_t st;

    ota_lock();
    st = g_ota.status;
    ota_unlock();
    return st;
}

int ota_module_control(int cmd, void* arg) {
    ARG_UNUSED(cmd);
    ARG_UNUSED(arg);
    return -ENOTSUP;
}

/* =============================================================================
 * 模块专用 API
 * ============================================================================= */

int ota_module_begin_update(void) {
    int            ret;
    ota_progress_t prog;
    ota_state_t    state;

    ota_lock();

    if (g_ota.status == MODULE_STATUS_UNINITIALIZED) {
        ota_unlock();
        return APP_ERR_INIT;
    }

    /*
     * 主动 ingest（begin/write_chunk/finish）依赖编译期选定的 active_transport：
     * OTA_TRANSPORT_NULL（ztest 模拟）或 OTA_TRANSPORT_ACTIVE（mcuboot flash_img）
     * 二者互斥（Kconfig 中 OTA_TRANSPORT_ACTIVE 嵌于 `if !OTA_TRANSPORT_NULL`），
     * 但均会在 ota_module_init() 中把 g_ota.active_transport 设为非空。仅当二者
     * 均未启用（例如只开了 MCUMGR_SMP 被动路径）时，才在此提前拦截，
     * 避免误入下方对 active_transport 的空指针判断才报错。
     */
#if !IS_ENABLED(CONFIG_OTA_TRANSPORT_ACTIVE) && !IS_ENABLED(CONFIG_OTA_TRANSPORT_NULL)
    ota_unlock();
    LOG_WRN("Active ingest disabled; use MCUmgr SMP or enable CONFIG_OTA_TRANSPORT_ACTIVE/_NULL");
    return APP_ERR_OTA_TRANSPORT;
#endif

    if (g_ota.ingest_owner == OTA_INGEST_MCUMGR) {
        ota_unlock();
        return APP_ERR_OTA_INVALID_STATE;
    }

    if (g_ota.active_transport == NULL || g_ota.active_transport->open == NULL) {
        ota_unlock();
        return APP_ERR_OTA_TRANSPORT;
    }

    state = ota_sm_get_state(&g_ota.sm);
    if (state == OTA_STATE_ERROR || state == OTA_STATE_READY_REBOOT) {
        ota_reset_session_locked();
    }

    ret = ota_sm_on_download_start(&g_ota.sm);
    if (ret != 0) {
        ota_unlock();
        return APP_ERR_OTA_INVALID_STATE;
    }

    ret = g_ota.active_transport->open((ota_transport_ops_t*) g_ota.active_transport);
    if (ret != 0) {
        ret = ota_transport_fail_locked(ret, &prog);
        ota_unlock();
        (void) ota_publish_progress(&prog);
        return APP_ERR_OTA_TRANSPORT;
    }

    g_ota.ingest_owner = OTA_INGEST_ACTIVE;
    g_ota.session_open = true;
    g_ota.image_size = 0U;
    ota_build_progress(&prog, OTA_STATE_DOWNLOADING, 0);
    ota_unlock();
    (void) ota_publish_progress(&prog);
    return 0;
}

int ota_module_write_chunk(size_t offset, const uint8_t* data, size_t len) {
    int                         ret;
    ota_progress_t              prog;
    const ota_transport_ops_t*  transport;

    if (data == NULL || len == 0U) {
        return APP_ERR_INVALID_PARAM;
    }

    ota_lock();

    if (ota_sm_get_state(&g_ota.sm) != OTA_STATE_DOWNLOADING || !g_ota.session_open ||
        g_ota.ingest_owner != OTA_INGEST_ACTIVE) {
        ota_unlock();
        return APP_ERR_OTA_INVALID_STATE;
    }
    transport = g_ota.active_transport;
    ota_unlock();

    /* 传输写（flash_img，慢速 IO）在模块锁外执行，避免阻塞 get_state/control 等查询。
     * xfer_lock 保证与 abort/stop 路径的传输层操作互斥（见 ota_xfer_abort_locked）。
     * 会话有效性在写前、写后各校验一次；写期间被并发 abort 的结果会被作废丢弃。 */
    zepl_lock_enter(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_ota.xfer_lock);
    k_mutex_lock(&g_ota.xfer_lock, K_FOREVER);
    ret = transport->write_chunk((ota_transport_ops_t*) transport, offset, data, len);
    k_mutex_unlock(&g_ota.xfer_lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_ota.xfer_lock);

    ota_lock();

    if (ota_sm_get_state(&g_ota.sm) != OTA_STATE_DOWNLOADING || !g_ota.session_open ||
        g_ota.ingest_owner != OTA_INGEST_ACTIVE || g_ota.active_transport != transport) {
        ota_unlock();
        return APP_ERR_OTA_INVALID_STATE;
    }

    if (ret != 0) {
        ret = ota_transport_fail_locked(ret, &prog);
        ota_unlock();
        (void) ota_publish_progress(&prog);
        return ret;
    }

    if (offset + len > g_ota.image_size) {
        g_ota.image_size = offset + len;
    }
    ota_build_progress(&prog, OTA_STATE_DOWNLOADING, 0);
    ota_unlock();
    (void) ota_publish_progress(&prog);
    return 0;
}

int ota_module_finish_update(void) {
    int            ret;
    ota_progress_t prog;

    ota_lock();

    if (ota_sm_get_state(&g_ota.sm) != OTA_STATE_DOWNLOADING || !g_ota.session_open ||
        g_ota.ingest_owner != OTA_INGEST_ACTIVE) {
        ota_unlock();
        return APP_ERR_OTA_INVALID_STATE;
    }

    ret = ota_sm_on_download_complete(&g_ota.sm);
    if (ret != 0) {
        ota_unlock();
        return APP_ERR_OTA_INVALID_STATE;
    }
    ota_build_progress(&prog, OTA_STATE_VERIFYING, 0);
    ota_unlock();
    (void) ota_publish_progress(&prog);

    ota_lock();
    /* 释放锁期间可能发生并发 abort/stop，重锁后须重新校验会话仍有效 */
    if (ota_sm_get_state(&g_ota.sm) != OTA_STATE_VERIFYING || !g_ota.session_open ||
        g_ota.ingest_owner != OTA_INGEST_ACTIVE) {
        ota_unlock();
        return APP_ERR_OTA_INVALID_STATE;
    }
    ret = g_ota.active_transport->verify((ota_transport_ops_t*) g_ota.active_transport);
    if (ret != 0) {
        ret = ota_transport_fail_locked(ret, &prog);
        ota_unlock();
        (void) ota_publish_progress(&prog);
        return ret;
    }

    ret = ota_sm_on_verify_ok(&g_ota.sm);
    if (ret != 0) {
        ota_unlock();
        return APP_ERR_OTA_INVALID_STATE;
    }

    if (g_ota.active_transport->close != NULL) {
        (void) g_ota.active_transport->close((ota_transport_ops_t*) g_ota.active_transport);
    }
    g_ota.session_open = false;
    g_ota.ingest_owner = OTA_INGEST_NONE;
    ota_build_progress(&prog, OTA_STATE_READY_REBOOT, 0);
    ota_unlock();
    (void) ota_publish_progress(&prog);
    return 0;
}

int ota_module_get_state(ota_state_t* out_state) {
    if (out_state == NULL) {
        return APP_ERR_INVALID_PARAM;
    }

    ota_lock();
    *out_state = ota_sm_get_state(&g_ota.sm);
    ota_unlock();
    return 0;
}

int ota_module_abort_update(void) {
    ota_progress_t prog;

    ota_lock();
    ota_reset_session_locked();
    ota_build_progress(&prog, OTA_STATE_IDLE, 0);
    ota_unlock();
    (void) ota_publish_progress(&prog);
    return 0;
}

int ota_module_request_reboot(void) {
    ota_state_t st;

    ota_lock();
    st = ota_sm_get_state(&g_ota.sm);
    ota_unlock();

    if (st != OTA_STATE_READY_REBOOT) {
        return APP_ERR_OTA_INVALID_STATE;
    }

    LOG_INF("OTA reboot requested");
    sys_reboot(SYS_REBOOT_WARM);
    return 0;
}

int ota_module_confirm_image(void) {
#if IS_ENABLED(CONFIG_MCUBOOT_IMG_MANAGER)
    int ret = boot_write_img_confirmed();

    if (ret != 0) {
        LOG_ERR("boot_write_img_confirmed failed: %d", ret);
        return APP_ERR_OTA_TRANSPORT;
    }
    LOG_INF("Running image confirmed; MCUboot rollback window closed");
    return 0;
#else
    LOG_WRN("ota_module_confirm_image requires CONFIG_MCUBOOT_IMG_MANAGER=y");
    return -ENOTSUP;
#endif
}

/* =============================================================================
 * MCUmgr img_mgmt 桥接（CONFIG_OTA_TRANSPORT_MCUMGR_SMP）
 * ============================================================================= */

#if IS_ENABLED(CONFIG_OTA_TRANSPORT_MCUMGR_SMP)
static void ota_mcumgr_fail_locked(int err, ota_progress_t* out_prog) {
    (void) ota_transport_fail_locked(err, out_prog);
}

bool ota_module_mcumgr_try_claim_session(void) {
    bool ok;

    ota_lock();
    ok = (g_ota.status == MODULE_STATUS_RUNNING) &&
         (g_ota.ingest_owner == OTA_INGEST_NONE || g_ota.ingest_owner == OTA_INGEST_MCUMGR);
    if (ok && g_ota.ingest_owner == OTA_INGEST_NONE) {
        g_ota.ingest_owner = OTA_INGEST_MCUMGR;
    }
    ota_unlock();
    return ok;
}

bool ota_module_mcumgr_is_accepting(void) {
    ota_state_t st;
    bool        ok;

    ota_lock();
    st = ota_sm_get_state(&g_ota.sm);
    ok = (g_ota.status == MODULE_STATUS_RUNNING) && (g_ota.ingest_owner == OTA_INGEST_MCUMGR) &&
         (st == OTA_STATE_IDLE || st == OTA_STATE_DOWNLOADING || st == OTA_STATE_VERIFYING);
    ota_unlock();
    return ok;
}

bool ota_module_mcumgr_on_dfu_started(void) {
    ota_progress_t prog;
    ota_state_t    st;
    int            ret;

    ota_lock();
    st = ota_sm_get_state(&g_ota.sm);
    if (st == OTA_STATE_ERROR || st == OTA_STATE_READY_REBOOT) {
        ota_reset_session_locked();
        g_ota.ingest_owner = OTA_INGEST_MCUMGR;
    }

    ret = ota_sm_on_download_start(&g_ota.sm);
    if (ret != 0) {
        g_ota.ingest_owner = OTA_INGEST_NONE;
        ota_unlock();
        return false;
    }

    g_ota.session_open = true;
    g_ota.image_size = 0U;
    ota_build_progress(&prog, OTA_STATE_DOWNLOADING, 0);
    ota_unlock();
    (void) ota_publish_progress(&prog);
    return true;
}

void ota_module_mcumgr_on_chunk_progress(size_t image_size) {
    ota_progress_t prog;

    ota_lock();
    if (!g_ota.session_open || g_ota.ingest_owner != OTA_INGEST_MCUMGR ||
        ota_sm_get_state(&g_ota.sm) != OTA_STATE_DOWNLOADING) {
        ota_unlock();
        return;
    }

    g_ota.image_size = image_size;
    ota_build_progress(&prog, OTA_STATE_DOWNLOADING, 0);
    ota_unlock();
    (void) ota_publish_progress(&prog);
}

void ota_module_mcumgr_on_dfu_pending(void) {
    ota_progress_t prog;
    int            ret;

    ota_lock();
    if (!g_ota.session_open || g_ota.ingest_owner != OTA_INGEST_MCUMGR) {
        ota_mcumgr_fail_locked(-EINVAL, &prog);
        ota_unlock();
        (void) ota_publish_progress(&prog);
        return;
    }

    ret = ota_sm_on_download_complete(&g_ota.sm);
    if (ret != 0) {
        ota_mcumgr_fail_locked(ret, &prog);
        ota_unlock();
        (void) ota_publish_progress(&prog);
        return;
    }
    ota_build_progress(&prog, OTA_STATE_VERIFYING, 0);
    ota_unlock();
    (void) ota_publish_progress(&prog);

    ota_lock();
    if (!g_ota.session_open || g_ota.ingest_owner != OTA_INGEST_MCUMGR ||
        ota_sm_get_state(&g_ota.sm) != OTA_STATE_VERIFYING) {
        ota_mcumgr_fail_locked(-EINVAL, &prog);
        ota_unlock();
        (void) ota_publish_progress(&prog);
        return;
    }

    ret = ota_sm_on_verify_ok(&g_ota.sm);
    if (ret != 0) {
        ota_mcumgr_fail_locked(ret, &prog);
        ota_unlock();
        (void) ota_publish_progress(&prog);
        return;
    }

    /* TEST 升级保留 MCUboot 安全回滚窗口：新镜像重启后若未 confirm
     * （ota_module_confirm_image() 或外部 mcumgr），再次重启将回滚旧镜像。 */
    ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
    if (ret != 0) {
        ret = ota_transport_fail_locked(ret, &prog);
        ota_unlock();
        (void) ota_publish_progress(&prog);
        return;
    }

    g_ota.session_open = false;
    g_ota.ingest_owner = OTA_INGEST_NONE;
    ota_build_progress(&prog, OTA_STATE_READY_REBOOT, 0);
    ota_unlock();
    (void) ota_publish_progress(&prog);
}

void ota_module_mcumgr_on_dfu_stopped(void) {
    ota_progress_t prog;

    ota_lock();
    ota_reset_session_locked();
    ota_build_progress(&prog, OTA_STATE_IDLE, 0);
    ota_unlock();
    (void) ota_publish_progress(&prog);
}
#endif /* CONFIG_OTA_TRANSPORT_MCUMGR_SMP */

/* =============================================================================
 * 模块注册
 * ============================================================================= */

DECLARE_MODULE_INTERFACE(ota_module);

#if IS_ENABLED(CONFIG_OTA_MODULE_AUTOINIT)
static int ota_module_auto_register(void) {
    uint32_t id;

    return module_manager_register(&ota_module_interface, NULL, &id) ? -EIO : 0;
}

SYS_INIT(ota_module_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_OTA);
#endif
