/**
 * @file provisioning_module.c
 * @brief 配网模块实现
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 3 初始版本
 * 2026-09-18       1.1            zeh            reset() 同时删除持久化凭据并落盘；
 *                                                 set_credentials() PSK 写失败回写旧 SSID
 *
 */

#include <zeplod/provisioning_module.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <string.h>

#include <zeplod/app_config.h>
#include <zeplod/app_kv.h>
#include <zeplod/lock_order.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(provisioning_module, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 常量
 * ============================================================================= */

/** app_kv 中存储 SSID 的键名 */
#define PROV_KV_KEY_SSID "wifi.ssid"

/** app_kv 中存储 PSK 的键名 */
#define PROV_KV_KEY_PSK "wifi.psk"

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

/** 配网模块运行时控制块（单例 g_prov） */
typedef struct {
    provisioning_state_t state;             /**< 配网状态机当前阶段 */
    module_status_t      module_status;     /**< init/start/stop 生命周期 */
    struct k_mutex        lock;              /**< 保护 state 与 module_status */
    bool                 lock_ready;        /**< 互斥量是否已完成 k_mutex_init */
    bool                 events_registered; /**< EVENT_PROVISIONING_STATE_CHANGED 是否已注册 */
} provisioning_module_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static provisioning_module_cb_t g_prov;

/* =============================================================================
 * 前置声明
 * ============================================================================= */

static void prov_lock(void);
static void prov_unlock(void);
static int  prov_register_event_types(void);
static int  prov_publish_state(provisioning_state_t state, int err);

/* =============================================================================
 * 锁与内部辅助
 * ============================================================================= */

/** 获取模块锁（RESOURCE 层级，须与 zepl_lock_exit 配对） */
static void prov_lock(void) {
    zepl_lock_enter(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_prov.lock);
    k_mutex_lock(&g_prov.lock, K_FOREVER);
}

/** 释放模块锁 */
static void prov_unlock(void) {
    k_mutex_unlock(&g_prov.lock);
    zepl_lock_exit(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_prov.lock);
}

/**
 * @brief 向事件系统注册配网状态变化类型（幂等）
 * @return 0 成功；-EIO 注册失败
 */
static int prov_register_event_types(void) {
    event_status_t st;

    if (g_prov.events_registered) {
        return 0;
    }

    st = event_register_type(EVENT_PROVISIONING_STATE_CHANGED, "prov_state");
    if (st != EVENT_OK) {
        LOG_ERR("register EVENT_PROVISIONING_STATE_CHANGED failed: %d", st);
        return -EIO;
    }

    g_prov.events_registered = true;
    return 0;
}

/**
 * @brief 发布配网状态变化事件
 *
 * 须在锁外调用，避免事件分发线程回调再入本模块导致死锁。
 *
 * @param state 新状态
 * @param err   伴随错误码（成功为 0）
 * @return 0 成功；-EIO 发布失败（仅记日志，不中断调用方）
 */
static int prov_publish_state(provisioning_state_t state, int err) {
    provisioning_status_t st = {.state = state, .error_code = err};
    event_status_t        ev_st;

    ev_st = event_publish_copy(EVENT_PROVISIONING_STATE_CHANGED, EVENT_PRIORITY_NORMAL, &st, sizeof(st));
    if (ev_st != EVENT_OK) {
        LOG_WRN("provisioning state event publish failed: %d", ev_st);
        return -EIO;
    }
    return 0;
}

/* =============================================================================
 * 模块专用 API
 * ============================================================================= */

int provisioning_module_begin(const provisioning_credentials_t* creds) {
    int ret;

    prov_lock();
    if (g_prov.module_status == MODULE_STATUS_UNINITIALIZED) {
        prov_unlock();
        return APP_ERR_INIT;
    }
    if (g_prov.module_status != MODULE_STATUS_RUNNING) {
        prov_unlock();
        return APP_ERR_INIT;
    }
    /* 已配网则拒绝重复 begin */
    if (g_prov.state == PROVISIONING_STATE_PROVISIONED) {
        prov_unlock();
        return APP_ERR_PROVISIONING;
    }

    g_prov.state = PROVISIONING_STATE_IN_PROGRESS;
    prov_unlock();
    (void) prov_publish_state(PROVISIONING_STATE_IN_PROGRESS, 0);

    /* creds 非空时落盘凭据；失败则回退到 ERROR 态，不推进为 PROVISIONED */
    if (creds != NULL) {
        ret = provisioning_module_set_credentials(creds);
        if (ret != 0) {
            prov_lock();
            g_prov.state = PROVISIONING_STATE_ERROR;
            prov_unlock();
            (void) prov_publish_state(PROVISIONING_STATE_ERROR, ret);
            return ret;
        }
    }

    prov_lock();
    g_prov.state = PROVISIONING_STATE_PROVISIONED;
    prov_unlock();
    (void) prov_publish_state(PROVISIONING_STATE_PROVISIONED, 0);
    return 0;
}

int provisioning_module_reset(void) {
    int ret_ssid;
    int ret_psk;

    prov_lock();
    g_prov.state = PROVISIONING_STATE_UNPROVISIONED;
    prov_unlock();
    (void) prov_publish_state(PROVISIONING_STATE_UNPROVISIONED, 0);

    /* reset 语义为"恢复到未配网"：持久化凭据须一并删除，否则重启后 init 探测到
       app_kv 残留的 SSID/PSK 会再次把状态标成 PROVISIONED，reset 形同虚设 */
    ret_ssid = app_kv_remove(PROV_KV_KEY_SSID);
    ret_psk = app_kv_remove(PROV_KV_KEY_PSK);
    /* 键不存在（未配网即 reset）与 app_kv 未编译（DISABLED）均属正常，不视为失败 */
    if ((ret_ssid != APP_OK && ret_ssid != APP_ERR_NOT_FOUND && ret_ssid != APP_ERR_DISABLED) ||
        (ret_psk != APP_OK && ret_psk != APP_ERR_NOT_FOUND && ret_psk != APP_ERR_DISABLED)) {
        LOG_ERR("remove persisted credentials failed (ssid=%d psk=%d)", ret_ssid, ret_psk);
    }

#if IS_ENABLED(CONFIG_APP_KV_PERSIST)
    {
        int save_ret = app_kv_save();

        if (save_ret != APP_OK) {
            /* 落盘失败仅记日志：RAM 表已删除，但 flash 残留会在下次上电恢复凭据 */
            LOG_ERR("app_kv_save after reset failed: %d (stale credentials may reload on boot)", save_ret);
        }
    }
#endif

    return 0;
}

int provisioning_module_get_state(provisioning_state_t* out_state) {
    if (out_state == NULL) {
        return APP_ERR_INVALID_PARAM;
    }

    prov_lock();
    *out_state = g_prov.state;
    prov_unlock();
    return 0;
}

int provisioning_module_get_device_id(char* out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return APP_ERR_INVALID_PARAM;
    }

    /* 设备 ID 来自 Kconfig 编译期字符串，非运行时 NVS */
    if (strlen(CONFIG_PROVISIONING_DEVICE_ID) >= out_len) {
        return -ENOMEM;
    }

    (void) strncpy(out, CONFIG_PROVISIONING_DEVICE_ID, out_len - 1U);
    out[out_len - 1U] = '\0';
    return 0;
}

int provisioning_module_set_credentials(const provisioning_credentials_t* creds) {
    int  ret;
    char old_ssid[PROVISIONING_WIFI_SSID_MAX_LEN];
    bool has_old_ssid;

    if (creds == NULL || creds->ssid == NULL || creds->psk == NULL) {
        return APP_ERR_INVALID_PARAM;
    }
    if (strlen(creds->ssid) >= PROVISIONING_WIFI_SSID_MAX_LEN || strlen(creds->psk) >= PROVISIONING_WIFI_PSK_MAX_LEN) {
        return APP_ERR_INVALID_PARAM;
    }

    prov_lock();
    if (g_prov.module_status == MODULE_STATUS_UNINITIALIZED) {
        prov_unlock();
        return APP_ERR_INIT;
    }
    if (g_prov.module_status != MODULE_STATUS_RUNNING) {
        prov_unlock();
        return APP_ERR_INIT;
    }
    prov_unlock();

    /* 暂存旧 SSID：PSK 写失败时回写，避免留下"新 SSID + 旧 PSK"的错配组合——
       get_credentials 只看两键是否存在，半写状态会在下次上电被误判为已配网 */
    has_old_ssid = (app_kv_get(PROV_KV_KEY_SSID, old_ssid, sizeof(old_ssid)) == APP_OK);

    /* app_kv 内部自带互斥锁，且不会回调本模块，锁外调用避免不必要的嵌套持锁 */
    ret = app_kv_set(PROV_KV_KEY_SSID, creds->ssid);
    if (ret != APP_OK && ret != APP_ERR_DISABLED) {
        LOG_ERR("persist wifi.ssid failed: %d", ret);
        return ret;
    }
    ret = app_kv_set(PROV_KV_KEY_PSK, creds->psk);
    if (ret != APP_OK && ret != APP_ERR_DISABLED) {
        int rollback_ret;

        LOG_ERR("persist wifi.psk failed: %d", ret);
        if (has_old_ssid) {
            /* 回写旧 SSID，维持与新写入前的旧 PSK 成对一致；回滚失败保持报错返回，
               此时凭据处于不一致态，状态机不会进入 PROVISIONED（见 begin 的失败路径） */
            rollback_ret = app_kv_set(PROV_KV_KEY_SSID, old_ssid);
            if (rollback_ret != APP_OK && rollback_ret != APP_ERR_DISABLED) {
                LOG_ERR("rollback wifi.ssid failed: %d", rollback_ret);
            }
        }
        return ret;
    }

#if IS_ENABLED(CONFIG_APP_KV_PERSIST)
    ret = app_kv_save();
    if (ret != APP_OK) {
        /* 落盘失败仅记日志：RAM 表已更新，凭据在本次运行期间仍可用 */
        LOG_WRN("app_kv_save after wifi credentials update failed: %d", ret);
    }
#endif

    LOG_INF("Wi-Fi credentials updated (ssid=%s)", creds->ssid);
    return 0;
}

int provisioning_module_get_credentials(char* ssid, size_t ssid_len, char* psk, size_t psk_len) {
    int ret;

    if (ssid == NULL || ssid_len == 0U || psk == NULL || psk_len == 0U) {
        return APP_ERR_INVALID_PARAM;
    }

    ret = app_kv_get(PROV_KV_KEY_SSID, ssid, ssid_len);
    if (ret != APP_OK) {
        return APP_ERR_NOT_FOUND;
    }
    ret = app_kv_get(PROV_KV_KEY_PSK, psk, psk_len);
    if (ret != APP_OK) {
        return APP_ERR_NOT_FOUND;
    }
    return 0;
}

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int provisioning_module_init(void* config) {
    int ret;

    ARG_UNUSED(config);

    /* 幂等：重复 init 直接成功 */
    if (g_prov.module_status != MODULE_STATUS_UNINITIALIZED) {
        return 0;
    }

    if (!g_prov.lock_ready) {
        k_mutex_init(&g_prov.lock);
        g_prov.lock_ready = true;
    }

    g_prov.state = PROVISIONING_STATE_UNPROVISIONED;
    g_prov.module_status = MODULE_STATUS_INITIALIZED;

    ret = prov_register_event_types();
    if (ret != 0) {
        g_prov.module_status = MODULE_STATUS_UNINITIALIZED;
        return ret;
    }

    /* app_kv 早于本模块完成 SYS_INIT（含 CONFIG_APP_KV_PERSIST 时的 flash 加载），
     * 若已存在持久化的 Wi-Fi 凭据，视为设备已配网，跳过 begin() 直接进入 PROVISIONED */
    {
        char ssid_probe[PROVISIONING_WIFI_SSID_MAX_LEN];
        char psk_probe[PROVISIONING_WIFI_PSK_MAX_LEN];

        if (provisioning_module_get_credentials(ssid_probe, sizeof(ssid_probe), psk_probe, sizeof(psk_probe)) == 0) {
            g_prov.state = PROVISIONING_STATE_PROVISIONED;
            LOG_INF("Existing persisted Wi-Fi credentials found (ssid=%s); marking PROVISIONED", ssid_probe);
        }
    }

    LOG_INF("Provisioning module initialized");
    return 0;
}

int provisioning_module_start(void) {
    prov_lock();
    if (g_prov.module_status == MODULE_STATUS_UNINITIALIZED) {
        prov_unlock();
        return APP_ERR_INIT;
    }
    if (g_prov.module_status == MODULE_STATUS_RUNNING) {
        prov_unlock();
        return 0;
    }
    g_prov.module_status = MODULE_STATUS_RUNNING;
    prov_unlock();

    LOG_INF("Provisioning module started");
    return 0;
}

int provisioning_module_stop(void) {
    prov_lock();
    if (g_prov.module_status == MODULE_STATUS_RUNNING) {
        g_prov.module_status = MODULE_STATUS_STOPPED;
    }
    prov_unlock();
    return 0;
}

int provisioning_module_shutdown(void) {
    (void) provisioning_module_stop();

    prov_lock();
    g_prov.module_status = MODULE_STATUS_UNINITIALIZED;
    prov_unlock();
    return 0;
}

/** Phase 3：暂无订阅外部事件，占位供后续联动 connectivity 等模块 */
void provisioning_module_on_event(const event_t* event, void* user_data) {
    ARG_UNUSED(event);
    ARG_UNUSED(user_data);
}

module_status_t provisioning_module_get_status(void) {
    module_status_t st;

    prov_lock();
    st = g_prov.module_status;
    prov_unlock();
    return st;
}

int provisioning_module_control(int cmd, void* arg) {
    ARG_UNUSED(cmd);
    ARG_UNUSED(arg);
    return -ENOTSUP;
}

/* =============================================================================
 * 模块注册
 * ============================================================================= */

DECLARE_MODULE_INTERFACE(provisioning_module);

#if IS_ENABLED(CONFIG_PROVISIONING_MODULE_AUTOINIT)
/** SYS_INIT 钩子：将本模块注册到 module_manager */
static int provisioning_module_auto_register(void) {
    uint32_t id;

    return module_manager_register(&provisioning_module_interface, NULL, &id) ? -EIO : 0;
}

SYS_INIT(provisioning_module_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_PROVISIONING);
#endif
