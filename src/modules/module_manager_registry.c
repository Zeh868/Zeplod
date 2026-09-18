/**
 * @file module_manager_registry.c
 * @brief 模块注册表与查询
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-05-28
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-05-28       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            register/unregister 按阶段拆分辅助函数；drain 失败日志保留退订结果
 */

#include <zephyr/logging/log.h>
#include <string.h>
#include "module_manager_internal.h"

LOG_MODULE_DECLARE(module_manager, CONFIG_SYS_LOG_LEVEL);

module_info_t* find_module_by_id_locked(uint32_t module_id) {
    if (module_id == 0U) {
        return NULL;
    }

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        if (g_module_mgr.modules[i].id == module_id) {
            return &g_module_mgr.modules[i];
        }
    }

    return NULL;
}

uint32_t find_module_id_by_name_locked(const char* name) {
    if (name == NULL) {
        return 0U;
    }

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        if (g_module_mgr.modules[i].interface != NULL && g_module_mgr.modules[i].interface->name != NULL &&
            strcmp(g_module_mgr.modules[i].interface->name, name) == 0) {
            return g_module_mgr.modules[i].id;
        }
    }

    return 0U;
}

uint32_t allocate_unique_module_id(void) {
    for (uint32_t cand = 1U; cand != 0U; cand++) {
        bool taken = false;

        for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
            if (g_module_mgr.modules[i].status != MODULE_STATUS_UNINITIALIZED && g_module_mgr.modules[i].id == cand) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            return cand;
        }
    }
    return 0U;
}

void clear_module_slot_locked(module_info_t* info) {
    if (info == NULL) {
        return;
    }

    info->interface = NULL;
    info->config = NULL;
    info->internal_data = NULL;
    info->status = MODULE_STATUS_UNINITIALIZED;
    info->id = 0U;
    info->event_subscription_count = 0U;
    (void) memset(info->event_subscriptions, 0, sizeof(info->event_subscriptions));

    /* 清槽即代际变更；回绕时跳过保留值 0。 */
    info->generation = mm_generation_next(info->generation);

    info->op_state = MM_OP_IDLE;
    info->unregistering = false;
    /* 槽位清空即不再有回调在锁外执行；in_flight 与 draining 也随之重置。 */
    atomic_set(&info->in_flight, 0);
    atomic_set(&info->draining, 0);
}

/* 注册参数校验（无锁）：接口指针、模块名、init 函数缺一不可。 */
static int mm_register_validate(const module_interface_t* interface) {
    if (interface == NULL) {
        LOG_ERR("NULL interface pointer");
        return MODULE_ERR_INVALID_ARG;
    }
    if (interface->name == NULL || interface->name[0] == '\0') {
        LOG_ERR("Module name is NULL or empty");
        return MODULE_ERR_INVALID_ARG;
    }
    if (interface->init == NULL) {
        LOG_ERR("Module '%s' missing required init function", interface->name);
        return MODULE_ERR_INVALID_ARG;
    }
    return MODULE_OK;
}

/* 注册阶段一（持锁返回）：门控校验并占位槽位（置 INITIALIZING 防止注销路径触碰）。
 * 成功返回 MODULE_OK 且返回时持有管理器锁（调用方负责解锁，*out_info、*out_id 有效）；
 * 失败时已解锁并返回负错误码。 */
static int mm_register_reserve(const module_interface_t* interface, void* config, module_info_t** out_info,
                               uint32_t* out_id) {
    module_manager_lock();

    const zepl_state_t mgr_state = module_manager_lifecycle_state_locked();
    if (atomic_get(&g_module_mgr_shutting_down) != 0 || !module_manager_lifecycle_allows_op(mgr_state)) {
        module_manager_unlock();
        LOG_WRN("Module manager lifecycle (%s) does not allow register", zepl_state_name(mgr_state));
        return MODULE_ERR_INVALID_STATE;
    }

    if (g_module_mgr.module_count >= CONFIG_MAX_MODULES) {
        module_manager_unlock();
        LOG_ERR("Maximum module count (%d) reached", CONFIG_MAX_MODULES);
        return MODULE_ERR_NO_MEM;
    }

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        if (g_module_mgr.modules[i].status != MODULE_STATUS_UNINITIALIZED &&
            g_module_mgr.modules[i].interface != NULL && g_module_mgr.modules[i].interface->name != NULL &&
            strcmp(g_module_mgr.modules[i].interface->name, interface->name) == 0) {
            module_manager_unlock();
            LOG_WRN("Module '%s' already registered", interface->name);
            return MODULE_ERR_ALREADY_EXISTS;
        }
    }

    module_info_t* info = NULL;

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        if (g_module_mgr.modules[i].status == MODULE_STATUS_UNINITIALIZED) {
            info = &g_module_mgr.modules[i];
            break;
        }
    }

    if (info == NULL) {
        module_manager_unlock();
        LOG_ERR("No free module slot available");
        return MODULE_ERR_NO_MEM;
    }

    const uint32_t new_id = allocate_unique_module_id();

    if (new_id == 0U) {
        module_manager_unlock();
        LOG_ERR("No free module ID available");
        return MODULE_ERR_NO_MEM;
    }

    info->interface = interface;
    info->config = config;
    info->internal_data = NULL;
    info->status = MODULE_STATUS_INITIALIZING;
    info->id = new_id;
    info->event_subscription_count = 0U;
    info->op_state = MM_OP_IDLE;
    info->unregistering = false;
    atomic_set(&info->in_flight, 0);
    atomic_set(&info->draining, 0);
    (void) memset(info->event_subscriptions, 0, sizeof(info->event_subscriptions));

    *out_info = info;
    *out_id = new_id;
    return MODULE_OK;
}

/* 注册阶段二（持锁重入校验）：init 回调返回后按「槽位身份三段校验 → init 结果 →
 * 超时 → 状态门控」依次裁决。失败路径清槽并在**锁外**调用 shutdown 回收模块自建资源；
 * 成功时提交计数并按需写出 ID（已解锁）。 */
static int mm_register_commit(const module_interface_t* interface, uint32_t new_id, uint16_t slot_index,
                              uint32_t slot_generation, int init_ret, uint32_t elapsed, uint32_t* out_module_id) {
    int (*shutdown_fn)(void) = interface->shutdown;

    module_manager_lock();

    module_info_t* info = find_module_by_id_locked(new_id);
    /* 三段校验：槽位未被清空、仍处于 INITIALIZING、interface 未被改写。
     * 这与 lifecycle.c 的 start/stop 协议保持一致。 */
    if (info == NULL || info->status != MODULE_STATUS_INITIALIZING || info->interface != interface ||
        info->generation != slot_generation || (uint16_t) (info - g_module_mgr.modules) != slot_index) {
        module_manager_unlock();
        LOG_ERR("Module '%s' slot lost or raced during init", interface->name);
        return MODULE_ERR_IO;
    }

    int ret = MODULE_OK;

    if (init_ret != MODULE_OK) {
        LOG_ERR("Module '%s' init failed: %d", interface->name, init_ret);
        ret = init_ret;
    } else if (CONFIG_MODULE_INIT_TIMEOUT_MS > 0 && elapsed > (uint32_t) CONFIG_MODULE_INIT_TIMEOUT_MS) {
        LOG_ERR("Module '%s' init exceeded timeout (%u ms)", interface->name, (unsigned int) elapsed);
        ret = MODULE_ERR_TIMEOUT;
    } else if (atomic_get(&g_module_mgr_shutting_down) != 0 ||
               !module_manager_lifecycle_allows_op(module_manager_lifecycle_state_locked())) {
        ret = MODULE_ERR_INVALID_STATE;
    }

    if (ret != MODULE_OK) {
        clear_module_slot_locked(info);
        module_manager_unlock();
        if (shutdown_fn != NULL) {
            (void) shutdown_fn();
        }
        return ret;
    }

    info->status = MODULE_STATUS_INITIALIZED;
    g_module_mgr.module_count++;
    g_module_mgr.stats.total_modules++;

    /* 仅在注册最终成功后才写出 ID，避免失败/超时/状态拒绝路径误置输出参数。 */
    if (out_module_id != NULL) {
        *out_module_id = new_id;
    }

    module_manager_unlock();
    return MODULE_OK;
}

int module_manager_register(const module_interface_t* interface, void* config, uint32_t* module_id) {
    if (!atomic_get(&g_module_mgr_initialized)) {
        LOG_ERR("Module manager not initialized");
        return MODULE_ERR_NOT_INITIALIZED;
    }

    const int check_ret = mm_register_validate(interface);
    if (check_ret != MODULE_OK) {
        return check_ret;
    }

    module_info_t* info = NULL;
    uint32_t       new_id = 0U;

    const int reserve_ret = mm_register_reserve(interface, config, &info, &new_id);
    if (reserve_ret != MODULE_OK) {
        return reserve_ret;
    }

    const uint16_t slot_index = (uint16_t) (info - g_module_mgr.modules);
    const uint32_t slot_generation = info->generation;
    int (*init_fn)(void*) = interface->init;

    /* 锁外回调：init 可能耗时，管理器锁不得跨 init 持有。 */
    module_manager_unlock();

    const uint32_t t0 = k_uptime_get_32();
    const int      iret = init_fn(config);
    const uint32_t elapsed = k_uptime_get_32() - t0;

    const int commit_ret = mm_register_commit(interface, new_id, slot_index, slot_generation, iret, elapsed, module_id);
    if (commit_ret != MODULE_OK) {
        return commit_ret;
    }

    LOG_DBG("Module registered: %s (id=%u)", interface->name, (unsigned int) new_id);
    module_mgr_notify_callback(new_id, MODULE_MGR_EVENT_REGISTERED);
    return MODULE_OK;
}

/* unregister 各阶段的槽位身份快照：锁外回调返回后用 (id, iface, gen, slot) 重入校验。 */
typedef struct {
    uint32_t                  module_id;
    const module_interface_t* iface; /**< 冻结时捕获的模块接口，重入校验基准 */
    uint32_t                  gen;   /**< 冻结时捕获的槽位 generation */
    uint16_t                  slot;  /**< 冻结时捕获的槽位下标 */
    module_info_t*            info;  /**< 槽位指针；冻结期内本调用独占槽位，不会失效 */
    int (*stop_fn)(void);            /**< RUNNING 状态下需调用的业务 stop，可为 NULL */
    int (*shutdown_fn)(void);        /**< 清槽前需调用的业务 shutdown，可为 NULL */
} mm_unregister_ctx_t;

/* 阶段一（持锁→解锁）：校验并冻结槽位（unregistering + STOPPING + draining）。
 * 成功返回 MODULE_OK（返回时已解锁，槽位保持冻结）；失败已解锁并返回负错误码。 */
static int mm_unregister_freeze(mm_unregister_ctx_t* ctx) {
    module_manager_lock();

    module_info_t* info = find_module_by_id_locked(ctx->module_id);

    if (info == NULL) {
        module_manager_unlock();
        return MODULE_ERR_NOT_FOUND;
    }

    /* 初始化期间拒绝注销：调用方需等待 init 完成；重入/操作中/退订中同样拒绝。 */
    if (info->status == MODULE_STATUS_INITIALIZING || info->unregistering || info->op_state != MM_OP_IDLE) {
        module_manager_unlock();
        return MODULE_ERR_BUSY;
    }
    for (uint8_t i = 0U; i < info->event_subscription_count; i++) {
        if (info->event_subscriptions[i].removing) {
            module_manager_unlock();
            return MODULE_ERR_BUSY;
        }
    }

    ctx->iface = info->interface;
    ctx->gen = info->generation;
    ctx->slot = (uint16_t) (info - g_module_mgr.modules);
    ctx->info = info;
    ctx->stop_fn = (info->status == MODULE_STATUS_RUNNING && info->interface != NULL) ? info->interface->stop : NULL;
    ctx->shutdown_fn = (info->interface != NULL) ? info->interface->shutdown : NULL;

    /* 冻结：阻断并发 start/stop/unregister/新订阅触碰本槽位。 */
    info->unregistering = true;
    info->op_state = MM_OP_STOPPING;
    atomic_set(&info->draining, 1);

    module_manager_unlock();
    return MODULE_OK;
}

/* 阶段三（锁外 stop 返回后，持锁重入校验）。stop 失败：回滚状态与统计、解除冻结并
 * 上报 ERROR，返回 stop_ret；stop 成功：置 STOPPED、收集订阅快照后解锁返回 MODULE_OK。 */
static int mm_unregister_after_stop(mm_unregister_ctx_t* ctx, int stop_ret, event_type_t* types_out, uint32_t* ids_out,
                                    uint8_t* out_sub_n) {
    module_manager_lock();

    module_info_t* info = find_module_by_id_locked(ctx->module_id);
    if (info == NULL || info->interface != ctx->iface || info->generation != ctx->gen ||
        (uint16_t) (info - g_module_mgr.modules) != ctx->slot) {
        module_manager_unlock();
        return MODULE_ERR_IO;
    }
    ctx->info = info;

    if (stop_ret != MODULE_OK) {
        LOG_ERR("Module unregister aborted: stop failed (id=%u ret=%d)", (unsigned int) ctx->module_id, stop_ret);
        if (info->status == MODULE_STATUS_RUNNING) {
            info->status = MODULE_STATUS_ERROR;
            g_module_mgr.stats.error_modules++;
            if (g_module_mgr.stats.active_modules > 0U) {
                g_module_mgr.stats.active_modules--;
            }
        }
        info->op_state = MM_OP_IDLE;
        info->unregistering = false;
        atomic_set(&info->draining, 0);
        module_manager_unlock();
        module_mgr_notify_callback(ctx->module_id, MODULE_MGR_EVENT_ERROR);
        return stop_ret;
    }

    if (info->status == MODULE_STATUS_RUNNING) {
        info->status = MODULE_STATUS_STOPPED;
        if (g_module_mgr.stats.active_modules > 0U) {
            g_module_mgr.stats.active_modules--;
        }
    }

    *out_sub_n = module_event_detach_locked(info, types_out, ids_out, CONFIG_MODULE_MAX_EVENT_SUBSCRIPTIONS);
    module_manager_unlock();
    return MODULE_OK;
}

/* drain 失败回滚：恢复槽位可重试注销的状态；保持 draining=1（模块已停止且订阅身份
 * 已撤销，继续阻断新分发），等待调用方稍后重试注销。 */
static void mm_unregister_rollback_drain(const mm_unregister_ctx_t* ctx) {
    module_manager_lock();
    module_info_t* info = find_module_by_id_locked(ctx->module_id);
    if (info != NULL && info->interface == ctx->iface && info->generation == ctx->gen) {
        info->op_state = MM_OP_IDLE;
        info->unregistering = false;
    }
    module_manager_unlock();
}

/* 阶段末（持锁）：最终重入校验、扣减计数并清槽。返回已解锁状态：MODULE_OK 或 MODULE_ERR_IO。 */
static int mm_unregister_commit(const mm_unregister_ctx_t* ctx) {
    module_manager_lock();

    module_info_t* info = find_module_by_id_locked(ctx->module_id);
    if (info == NULL || info->interface != ctx->iface || info->generation != ctx->gen ||
        (uint16_t) (info - g_module_mgr.modules) != ctx->slot) {
        module_manager_unlock();
        return MODULE_ERR_IO;
    }

    if (g_module_mgr.module_count > 0U) {
        g_module_mgr.module_count--;
    }
    if (g_module_mgr.stats.total_modules > 0U) {
        g_module_mgr.stats.total_modules--;
    }
    if (info->status == MODULE_STATUS_ERROR && g_module_mgr.stats.error_modules > 0U) {
        g_module_mgr.stats.error_modules--;
    }

    clear_module_slot_locked(info);
    module_manager_unlock();
    return MODULE_OK;
}

int module_manager_unregister(uint32_t module_id) {
    mm_unregister_ctx_t ctx = {.module_id = module_id};

    if (!atomic_get(&g_module_mgr_initialized)) {
        return MODULE_ERR_NOT_INITIALIZED;
    }
    if (module_id == 0U) {
        return MODULE_ERR_INVALID_ARG;
    }

    const int freeze_ret = mm_unregister_freeze(&ctx);
    if (freeze_ret != MODULE_OK) {
        return freeze_ret;
    }

    /* 锁外回调：stop 失败时 after_stop 已完成状态回滚。 */
    const int stop_ret = (ctx.stop_fn != NULL) ? ctx.stop_fn() : MODULE_OK;

    event_type_t types[CONFIG_MODULE_MAX_EVENT_SUBSCRIPTIONS];
    uint32_t     ids[CONFIG_MODULE_MAX_EVENT_SUBSCRIPTIONS];
    uint8_t      sub_n = 0U;

    const int after_stop_ret = mm_unregister_after_stop(&ctx, stop_ret, types, ids, &sub_n);
    if (after_stop_ret != MODULE_OK) {
        return after_stop_ret;
    }

    const int unsubscribe_ret = module_event_unsubscribe_batch(types, ids, sub_n);

    /* 槽位冻结期内 ctx.info 指向的数组元素不会失效，drain 只读其 in_flight 原子计数。 */
    const int drain_ret = mm_wait_in_flight_drain(ctx.info, MM_DRAIN_TIMEOUT_MS);
    if (drain_ret != MODULE_OK) {
        /* 返回值以 drain 结果为准；unsubscribe 结果不能无声丢失，合并到同一条日志留痕。 */
        LOG_WRN("unregister id=%u: in-flight drain failed (%d), pending unsubscribe result=%d",
                (unsigned int) module_id, drain_ret, unsubscribe_ret);
        mm_unregister_rollback_drain(&ctx);
        return drain_ret;
    }

    if (ctx.shutdown_fn != NULL) {
        const int shutdown_ret = ctx.shutdown_fn();
        if (shutdown_ret != MODULE_OK) {
            LOG_WRN("Module shutdown id=%u returned %d", (unsigned int) module_id, shutdown_ret);
        }
    }

    const int commit_ret = mm_unregister_commit(&ctx);
    if (commit_ret != MODULE_OK) {
        return commit_ret;
    }

    LOG_DBG("Module unregistered: id=%u", (unsigned int) module_id);
    module_mgr_notify_callback(module_id, MODULE_MGR_EVENT_UNREGISTERED);
    return unsubscribe_ret;
}

int module_manager_get_module_info(uint32_t module_id, module_info_t* out) {
    if (!atomic_get(&g_module_mgr_initialized) || module_id == 0U || out == NULL) {
        return MODULE_ERR_INVALID_ARG;
    }

    module_manager_lock();

    module_info_t* info = find_module_by_id_locked(module_id);

    if (info == NULL) {
        module_manager_unlock();
        return MODULE_ERR_NOT_FOUND;
    }

    *out = *info;
    module_manager_unlock();
    return MODULE_OK;
}

uint32_t module_manager_get_id_by_name(const char* name) {
    if (!atomic_get(&g_module_mgr_initialized) || name == NULL) {
        return 0U;
    }

    module_manager_lock();

    const uint32_t id = find_module_id_by_name_locked(name);

    module_manager_unlock();
    return id;
}

void module_manager_foreach(void (*callback)(module_info_t*, void*), void* user_data) {
    if (!atomic_get(&g_module_mgr_initialized) || callback == NULL) {
        return;
    }

    module_info_t snapshot[CONFIG_MAX_MODULES];

    module_manager_lock();

    int n = 0;

    for (int i = 0; i < CONFIG_MAX_MODULES; i++) {
        if (g_module_mgr.modules[i].status != MODULE_STATUS_UNINITIALIZED) {
            snapshot[n++] = g_module_mgr.modules[i];
        }
    }

    module_manager_unlock();

    for (int i = 0; i < n; i++) {
        callback(&snapshot[i], user_data);
    }
}
