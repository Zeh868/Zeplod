/**
 * @file module_manager_internal.h
 * @brief 模块管理器内部共享（不对外公开）
 * @author zeh (china_qzh@163.com)
 * @version 1.3
 * @date 2026-06-06
 *
 * @par 修改日志:
 *    Date         Version        Author          Description
 * 2026-06-06       1.2            zeh            增加 planner 快照与 in_flight 排空协议
 * 2026-09-18       1.3            zeh            删除全工程只写不读的 initialized/running 冗余字段；快照栈上限具名
 */

#ifndef MODULE_MANAGER_INTERNAL_H
#define MODULE_MANAGER_INTERNAL_H

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zeplod/module_base.h>
#include <zeplod/module_manager.h>
#include <zeplod/state_machine.h>

#define MM_MODULE_NAME_MAX  32
#define MM_DRAIN_TIMEOUT_MS 1000U

/**
 * @brief 模块快照栈占用上限（字节）。
 *
 * foreach/dump/shutdown 等路径会按 CONFIG_MAX_MODULES 在栈上拷贝模块条目，
 * 由 module_manager.c 的 BUILD_ASSERT 强制执行。
 */
#define MM_MAX_SNAPSHOT_BYTES 3072U

#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
#define MM_ADJ_ROW_WORDS ((CONFIG_MAX_MODULES + 31) / 32)
#endif

typedef struct {
    module_info_t         modules[CONFIG_MAX_MODULES];
    uint32_t              module_count;
    module_mgr_stats_t    stats;
    module_mgr_callback_t callback;
    void*                 callback_user_data;
    struct k_mutex        lock;
    zepl_state_machine_t  lifecycle;
#if IS_ENABLED(CONFIG_MODULE_MANAGER_RUNTIME_DEPENDENCIES)
    uint32_t topo_adj_scratch[CONFIG_MAX_MODULES][MM_ADJ_ROW_WORDS];
#endif
} module_manager_cb_t;

extern module_manager_cb_t g_module_mgr;
extern atomic_t            g_module_mgr_shutting_down;
extern atomic_t            g_module_mgr_initialized;

void mm_copy_module_name(char* dst, const char* src);

void         module_manager_lock(void);
void         module_manager_unlock(void);
zepl_state_t module_manager_lifecycle_state_locked(void);

module_info_t* find_module_by_id_locked(uint32_t module_id);
uint32_t       find_module_id_by_name_locked(const char* name);
uint32_t       allocate_unique_module_id(void);
uint32_t       allocate_event_cookie_locked(void);
void           clear_module_slot_locked(module_info_t* info);

uint8_t module_event_detach_locked(module_info_t* info, event_type_t* types_out, uint32_t* ids_out, uint8_t max_out);
int     module_event_unsubscribe_batch(const event_type_t* types, const uint32_t* ids, uint8_t count);
void    module_mgr_notify_callback(uint32_t module_id, module_mgr_event_t evt);

int find_event_sub_index(const module_info_t* info, event_type_t type);
int event_status_to_module_error(event_status_t status);

/**
 * @brief 将稳定订阅 cookie 编码为事件系统 user_data。
 */
static inline void* mm_event_token_make(uint32_t cookie) {
    return (void*) (uintptr_t) cookie;
}

/**
 * @brief 解码稳定订阅 cookie。0 为非法 token。
 */
static inline bool mm_event_token_decode(void* token, uint32_t* out_cookie) {
    const uintptr_t raw = (uintptr_t) token;
    if (raw == 0U || raw > UINT32_MAX || out_cookie == NULL) {
        return false;
    }
    *out_cookie = (uint32_t) raw;
    return true;
}

/**
 * @brief 等待槽位 in_flight 计数归零。
 *
 * 由注销路径在调用 shutdown_fn 之前调用，确保没有锁外回调正在执行。
 * 超时返回 MODULE_ERR_TIMEOUT；调用方不得继续释放模块资源。
 */
int mm_wait_in_flight_drain(module_info_t* info, uint32_t timeout_ms);

/**
 * @brief 检查 manager 生命周期是否允许注册/启动/订阅操作。
 *
 * 拒绝 UNINIT（应由调用方 atomic 检查覆盖）/ STOPPING / ERROR。
 * 允许 INITED / STARTING / RUNNING / STOPPED。
 */
static inline bool module_manager_lifecycle_allows_op(zepl_state_t s) {
    switch (s) {
    case ZEP_STATE_INITED:
    case ZEP_STATE_STARTING:
    case ZEP_STATE_RUNNING:
    case ZEP_STATE_STOPPED:
        return true;
    default:
        return false;
    }
}
#endif /* MODULE_MANAGER_INTERNAL_H */
