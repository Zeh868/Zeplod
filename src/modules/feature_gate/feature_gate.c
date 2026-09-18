/**
 * @file feature_gate.c
 * @brief 功能开关模块实现
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 5 初始版本
 * 2026-06-13       1.1            zeh            license 变更事件发布
 * 2026-09-18       1.2            zeh            补文件修改日志；注明 token 比较非恒时
 *                                                 （非安全 DRM 定位的已知取舍）；格式对齐
 *                                                 仓库基线
 *
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
#include <zeplod/app_config.h>
#include <zeplod/feature_gate.h>
#include <zeplod/lock_order.h>
#include <zeplod/module_manager.h>

LOG_MODULE_REGISTER(feature_gate, CONFIG_SYS_LOG_LEVEL);

typedef struct {
    bool            license_valid;
    module_status_t module_status;
    struct k_mutex  lock;
    bool            lock_ready;
    bool            events_registered;

} feature_gate_cb_t;

static feature_gate_cb_t g_gate;

static void gate_lock(void) {
    zepl_lock_enter(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_gate.lock);

    k_mutex_lock(&g_gate.lock, K_FOREVER);
}

static void gate_unlock(void) {
    k_mutex_unlock(&g_gate.lock);

    zepl_lock_exit(ZEP_LOCK_LEVEL_RESOURCE, (uintptr_t) &g_gate.lock);
}

static bool gate_token_matches(const char* token) {
    if (token == NULL) {
        return false;
    }

    /* 已知取舍：strcmp 为非恒时比较，理论上可被时序侧信道逐字节探测。
       本模块定位为商业功能开关（非安全 DRM），不抵御对抗性攻击者，
       不为恒时比较引入额外复杂度；如未来承载真正的授权控制，须改用
       恒时比较（如逐字节 OR 累积差异后统一判定） */
    return strcmp(token, CONFIG_FEATURE_GATE_LICENSE) == 0;
}

static int gate_register_event_types(void) {
    event_status_t st;

    if (g_gate.events_registered) {
        return 0;
    }

    st = event_register_type(EVENT_FEATURE_GATE_LICENSE_CHANGED, "feat_license");

    if (st != EVENT_OK) {
        LOG_ERR("register EVENT_FEATURE_GATE_LICENSE_CHANGED failed: %d", st);

        return -EIO;
    }

    g_gate.events_registered = true;

    return 0;
}

static int gate_publish_license(bool valid) {
    event_status_t ev_st;

    ev_st = event_publish_copy(EVENT_FEATURE_GATE_LICENSE_CHANGED, EVENT_PRIORITY_NORMAL, &valid, sizeof(valid));

    if (ev_st != EVENT_OK) {
        LOG_WRN("feature_gate license event failed: %d", ev_st);

        return -EIO;
    }

    return 0;
}

static bool gate_feature_requires_license(const char* feature_name) {
    if (feature_name == NULL) {
        return true;
    }

    if (strcmp(feature_name, FEATURE_GATE_NAME_CORE) == 0) {
        return false;
    }

#if IS_ENABLED(CONFIG_FEATURE_GATE_SLOT_CLOUD)
    if (strcmp(feature_name, FEATURE_GATE_NAME_CLOUD) == 0) {
        return true;
    }
#endif

#if IS_ENABLED(CONFIG_FEATURE_GATE_SLOT_REMOTE)
    if (strcmp(feature_name, FEATURE_GATE_NAME_REMOTE) == 0) {
        return true;
    }
#endif

    return true;
}

static bool gate_feature_compiled_in(const char* feature_name) {
    if (feature_name == NULL) {
        return false;
    }

    if (strcmp(feature_name, FEATURE_GATE_NAME_CORE) == 0) {
        return true;
    }

#if IS_ENABLED(CONFIG_FEATURE_GATE_SLOT_CLOUD)
    if (strcmp(feature_name, FEATURE_GATE_NAME_CLOUD) == 0) {
        return true;
    }
#endif

#if IS_ENABLED(CONFIG_FEATURE_GATE_SLOT_REMOTE)
    if (strcmp(feature_name, FEATURE_GATE_NAME_REMOTE) == 0) {
        return true;
    }
#endif

    return false;
}

int feature_gate_apply_license(const char* token) {
    bool valid;

    if (token == NULL) {
        return APP_ERR_INVALID_PARAM;
    }

    gate_lock();

    if (g_gate.module_status == MODULE_STATUS_UNINITIALIZED) {
        gate_unlock();

        return APP_ERR_INIT;
    }

    gate_unlock();

    valid = gate_token_matches(token);

    gate_lock();

    g_gate.license_valid = valid;

    gate_unlock();

    (void) gate_publish_license(valid);

    LOG_INF("feature_gate license %s", valid ? "valid" : "invalid");

    return 0;
}

bool feature_gate_license_valid(void) {
    bool valid;

    gate_lock();

    if (g_gate.module_status == MODULE_STATUS_UNINITIALIZED) {
        gate_unlock();

        return false;
    }

    valid = g_gate.license_valid;

    gate_unlock();

    return valid;
}

bool feature_gate_is_enabled(const char* feature_name) {
    bool enabled;

    if (!gate_feature_compiled_in(feature_name)) {
        return false;
    }

    if (!gate_feature_requires_license(feature_name)) {
        return true;
    }

    gate_lock();

    if (g_gate.module_status == MODULE_STATUS_UNINITIALIZED) {
        gate_unlock();

        return false;
    }

    enabled = g_gate.license_valid;

    gate_unlock();

    return enabled;
}

int feature_gate_get_status_snapshot(feature_gate_status_t* out) {
    if (out == NULL) {
        return APP_ERR_INVALID_PARAM;
    }

    gate_lock();

    if (g_gate.module_status == MODULE_STATUS_UNINITIALIZED) {
        gate_unlock();

        return APP_ERR_INIT;
    }

    out->license_valid = g_gate.license_valid;

    gate_unlock();

    return 0;
}

int feature_gate_init(void* config) {
    int ret;

    ARG_UNUSED(config);

    if (g_gate.module_status != MODULE_STATUS_UNINITIALIZED) {
        return 0;
    }

    if (!g_gate.lock_ready) {
        k_mutex_init(&g_gate.lock);

        g_gate.lock_ready = true;
    }

    g_gate.license_valid = gate_token_matches(CONFIG_FEATURE_GATE_BOOT_LICENSE);

    g_gate.module_status = MODULE_STATUS_INITIALIZED;

    ret = gate_register_event_types();

    if (ret != 0) {
        g_gate.module_status = MODULE_STATUS_UNINITIALIZED;

        return APP_ERR_FEATURE_GATE;
    }

    LOG_INF("feature_gate initialized (license=%s)", g_gate.license_valid ? "valid" : "locked");

    return 0;
}

int feature_gate_start(void) {
    gate_lock();

    if (g_gate.module_status == MODULE_STATUS_UNINITIALIZED) {
        gate_unlock();

        return APP_ERR_INIT;
    }

    if (g_gate.module_status == MODULE_STATUS_RUNNING) {
        gate_unlock();

        return 0;
    }

    g_gate.module_status = MODULE_STATUS_RUNNING;

    gate_unlock();

    return 0;
}

int feature_gate_stop(void) {
    gate_lock();

    if (g_gate.module_status == MODULE_STATUS_RUNNING) {
        g_gate.module_status = MODULE_STATUS_STOPPED;
    }

    gate_unlock();

    return 0;
}

int feature_gate_shutdown(void) {
    gate_lock();

    g_gate.module_status = MODULE_STATUS_UNINITIALIZED;

    g_gate.license_valid = false;

    gate_unlock();

    return 0;
}

void feature_gate_on_event(const event_t* event, void* user_data) {
    ARG_UNUSED(event);

    ARG_UNUSED(user_data);
}

module_status_t feature_gate_get_status(void) {
    module_status_t st;

    gate_lock();

    st = g_gate.module_status;

    gate_unlock();

    return st;
}

int feature_gate_control(int cmd, void* arg) {
    ARG_UNUSED(cmd);

    ARG_UNUSED(arg);

    return -ENOTSUP;
}

DECLARE_MODULE_INTERFACE(feature_gate);

#if IS_ENABLED(CONFIG_FEATURE_GATE_MODULE_AUTOINIT)
static int feature_gate_auto_register(void) {
    uint32_t id;

    return module_manager_register(&feature_gate_interface, NULL, &id) ? -EIO : 0;
}

SYS_INIT(feature_gate_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_FEATURE_GATE);
#endif
