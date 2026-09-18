/**
 * @file module_manager_compat.c
 * @brief 模块管理器兼容层实现（标准版）
 * @author zeh (china_qzh@163.com)
 * @version 1.2
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-09-18       1.2            zeh            错误日志补 ret 值；零初始化改 memset
 *
 */

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
#include <zeplod/app_config.h>
#include <zeplod/module_manager.h>
#include <zeplod/module_manager_compat.h>

LOG_MODULE_REGISTER(module_manager_compat, CONFIG_SYS_LOG_LEVEL);

int module_compat_init(const module_compat_config_t* config) {
    ARG_UNUSED(config);

    int ret = module_manager_init();

    if (ret != 0) {
        LOG_ERR("Failed to init module_manager: %d", ret);
        return ret;
    }
    LOG_DBG("Module manager (standard) initialized");
    return 0;
}

int module_compat_start(void) {
    int ret = module_manager_start();

    if (ret != 0) {
        LOG_ERR("Failed to start module_manager: %d", ret);
        return ret;
    }
    return 0;
}

int module_compat_stop(void) {
    int ret = module_manager_stop();

    if (ret != 0) {
        LOG_ERR("Failed to stop module_manager: %d", ret);
        return ret;
    }
    return 0;
}

int module_compat_shutdown(void) {
    int ret = module_manager_shutdown();

    if (ret != 0) {
        LOG_ERR("Failed to shutdown module_manager: %d", ret);
        return ret;
    }
    return 0;
}

void module_compat_get_stats(module_compat_stats_t* stats) {
    if (stats == NULL) {
        return;
    }

    (void) memset(stats, 0, sizeof(*stats));

    module_mgr_stats_t std_stats;

    (void) memset(&std_stats, 0, sizeof(std_stats));

    module_manager_get_stats(&std_stats);

    stats->total_modules = std_stats.total_modules;
    stats->active_modules = std_stats.active_modules;
    stats->error_modules = std_stats.error_modules;
    stats->events_processed = std_stats.events_processed;
    stats->events_dropped = std_stats.events_dropped;
}

void module_compat_reset_stats(void) {
    module_manager_reset_stats();
}

/* =============================================================================
 * SYS_INIT 自动注册
 * ============================================================================= */

static int module_compat_auto_register(void) {
    int ret = module_compat_init(NULL);

    if (ret != 0) {
        LOG_ERR("Failed to init module manager compat");
        return -EIO;
    }

    ret = module_compat_start();
    if (ret != 0) {
        LOG_ERR("Failed to start module manager compat");
        return -EIO;
    }

    LOG_DBG("Module manager compat initialized and started");
    return 0;
}

#if !IS_ENABLED(CONFIG_MODULE_MANAGER_COMPAT_NO_AUTO_INIT)
SYS_INIT(module_compat_auto_register, POST_KERNEL, APP_INIT_PRIO_MODULE_MGR);
#endif
