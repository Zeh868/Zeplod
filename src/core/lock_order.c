/**
 * @file lock_order.c
 * @brief 锁顺序校验辅助实现
 *
 * 为各线程维护锁嵌套栈，在 enter/exit 时校验层级单调递增与同 key 配对释放。
 * 供事件系统等多锁模块在调试构建中检测违规加锁顺序。
 *
 * 主要功能：
 * - 线程锁状态注册表（自旋锁保护）
 * - zepl_lock_enter / zepl_lock_exit 与 token 封装
 * - 当前线程锁栈查询与重置
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-05-23
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-05-23       1.0            zeh            初始版本
 *
 */

#include <zeplod/lock_order.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>

#include <string.h>

LOG_MODULE_REGISTER(zepl_lock_order, LOG_LEVEL_WRN);

#if defined(CONFIG_ZEPL_LOCK_ORDER) && (CONFIG_ZEPL_LOCK_ORDER == 1)

/* =============================================================================
 * 完整实现（CONFIG_ZEPL_LOCK_ORDER=y）
 * ============================================================================= */

#if defined(CONFIG_ZEPL_LOCK_ORDER_STRICT) && (CONFIG_ZEPL_LOCK_ORDER_STRICT == 1)
#define LOCK_ORDER_STRICT_ENABLED 1
#else
#define LOCK_ORDER_STRICT_ENABLED 0
#endif

#define LOCK_ORDER_STRICT_ASSERT(cond, fmt, ...)                                                                       \
    do {                                                                                                               \
        if (LOCK_ORDER_STRICT_ENABLED) {                                                                               \
            __ASSERT((cond), fmt, ##__VA_ARGS__);                                                                      \
        }                                                                                                              \
    } while (0)

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

/** 栈中单层锁记录（与 zepl_lock_token_t 字段一致） */
typedef struct {
    zepl_lock_level_t level;
    uintptr_t         key;
} held_lock_t;

/** 单线程锁嵌套状态 */
typedef struct {
    bool        active; /**< 槽位是否已绑定线程 */
    k_tid_t     tid;    /**< 所属线程 ID */
    uint8_t     depth;  /**< 当前栈深度 */
    held_lock_t stack[ZEP_LOCK_ORDER_MAX_DEPTH];
} thread_lock_state_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

/** 各线程锁栈登记（固定容量，由 g_registry_lock 保护） */
static thread_lock_state_t g_thread_states[ZEP_LOCK_ORDER_MAX_THREADS];

/** 保护 g_thread_states 注册表的自旋锁 */
static struct k_spinlock g_registry_lock;

/* =============================================================================
 * 内部函数
 * ============================================================================= */

/**
 * @brief 检查 token 的 level 是否在合法枚举范围内
 */
static bool token_is_valid(zepl_lock_token_t token) {
    return token.level >= ZEP_LOCK_LEVEL_GLOBAL && token.level <= ZEP_LOCK_LEVEL_RESOURCE;
}

/**
 * @brief 查找或分配当前线程的锁栈状态
 *
 * @param tid 目标线程 ID
 * @param create_if_missing true 时若无记录则占用空闲槽位
 * @return 状态指针，未找到且未创建时返回 NULL
 *
 * @note 调用方须已持有 g_registry_lock
 */
static thread_lock_state_t* find_state_locked(k_tid_t tid, bool create_if_missing) {
    thread_lock_state_t* free_slot = NULL;

    for (size_t i = 0; i < ZEP_LOCK_ORDER_MAX_THREADS; i++) {
        if (g_thread_states[i].active && g_thread_states[i].tid == tid) {
            return &g_thread_states[i];
        }

        if (!g_thread_states[i].active && free_slot == NULL) {
            free_slot = &g_thread_states[i];
        }
    }

    if (!create_if_missing || free_slot == NULL) {
        return NULL;
    }

    free_slot->active = true;
    free_slot->tid = tid;
    free_slot->depth = 0U;
    memset(free_slot->stack, 0, sizeof(free_slot->stack));
    return free_slot;
}

/**
 * @brief 判断入栈是否符合顺序：新锁层级须高于栈顶，或同层 key 不小于栈顶
 */
static bool is_push_order_valid(const thread_lock_state_t* state, zepl_lock_token_t token) {
    if (state == NULL) {
        return true;
    }

    if (state->depth == 0U) {
        return true;
    }

    const held_lock_t* top = &state->stack[state->depth - 1U];

    if (token.level > top->level) {
        return true;
    }

    if (token.level == top->level && token.key >= top->key) {
        return true;
    }

    return false;
}

/**
 * @brief 判断出栈是否与栈顶完全匹配（LIFO）
 */
static bool is_pop_order_valid(const thread_lock_state_t* state, zepl_lock_token_t token) {
    if (state == NULL || state->depth == 0U) {
        return false;
    }

    const held_lock_t* top = &state->stack[state->depth - 1U];
    return top->level == token.level && top->key == token.key;
}

/* =============================================================================
 * 锁顺序 API 实现
 * ============================================================================= */

bool zepl_lock_order_is_valid(zepl_lock_level_t level, uintptr_t key) {
    if (k_is_in_isr()) {
        return false;
    }

    const zepl_lock_token_t token = {.level = level, .key = key};

    if (!token_is_valid(token)) {
        return false;
    }

    k_spinlock_key_t     spin_key = k_spin_lock(&g_registry_lock);
    thread_lock_state_t* state = find_state_locked(k_current_get(), false);
    const bool           valid = is_push_order_valid(state, token);
    k_spin_unlock(&g_registry_lock, spin_key);

    return valid;
}

void zepl_lock_enter(zepl_lock_level_t level, uintptr_t key) {
    const zepl_lock_token_t token = {.level = level, .key = key};

    if (k_is_in_isr()) {
        LOG_WRN("lock-order: enter from ISR is not supported");
        LOCK_ORDER_STRICT_ASSERT(false, "lock-order: enter from ISR");
        return;
    }

    if (!token_is_valid(token)) {
        LOG_WRN("lock-order: invalid token level=%u key=%p", (unsigned int) token.level, (void*) token.key);
        LOCK_ORDER_STRICT_ASSERT(false, "lock-order: invalid token");
        return;
    }

    k_spinlock_key_t     spin_key = k_spin_lock(&g_registry_lock);
    thread_lock_state_t* state = find_state_locked(k_current_get(), true);

    if (state == NULL) {
        k_spin_unlock(&g_registry_lock, spin_key);
        LOG_ERR("lock-order: registry exhausted");
        LOCK_ORDER_STRICT_ASSERT(false, "lock-order: registry exhausted");
        return;
    }

    if (!is_push_order_valid(state, token)) {
        const uint8_t           snapshot_depth = state->depth;
        const zepl_lock_level_t snapshot_top_level = state->stack[state->depth - 1U].level;
        const uintptr_t         snapshot_top_key = state->stack[state->depth - 1U].key;

        k_spin_unlock(&g_registry_lock, spin_key);
        LOG_ERR("lock-order violation: level=%u key=%p depth=%u top=(%u,%p)", (unsigned int) token.level,
                (void*) token.key, snapshot_depth, (unsigned int) snapshot_top_level, (void*) snapshot_top_key);
        LOCK_ORDER_STRICT_ASSERT(false, "lock-order violation on enter");
        return;
    }

    if (state->depth >= ZEP_LOCK_ORDER_MAX_DEPTH) {
        k_spin_unlock(&g_registry_lock, spin_key);
        LOG_ERR("lock-order: stack overflow");
        LOCK_ORDER_STRICT_ASSERT(false, "lock-order: stack overflow");
        return;
    }

    state->stack[state->depth].level = token.level;
    state->stack[state->depth].key = token.key;
    state->depth++;

    k_spin_unlock(&g_registry_lock, spin_key);
}

void zepl_lock_exit(zepl_lock_level_t level, uintptr_t key) {
    const zepl_lock_token_t token = {.level = level, .key = key};

    if (k_is_in_isr()) {
        LOG_WRN("lock-order: exit from ISR is not supported");
        LOCK_ORDER_STRICT_ASSERT(false, "lock-order: exit from ISR");
        return;
    }

    if (!token_is_valid(token)) {
        LOG_WRN("lock-order: invalid token level=%u key=%p", (unsigned int) token.level, (void*) token.key);
        LOCK_ORDER_STRICT_ASSERT(false, "lock-order: invalid token");
        return;
    }

    k_spinlock_key_t     spin_key = k_spin_lock(&g_registry_lock);
    thread_lock_state_t* state = find_state_locked(k_current_get(), false);

    if (!is_pop_order_valid(state, token)) {
        k_spin_unlock(&g_registry_lock, spin_key);
        LOG_ERR("lock-order violation on exit: level=%u key=%p", (unsigned int) token.level, (void*) token.key);
        LOCK_ORDER_STRICT_ASSERT(false, "lock-order violation on exit");
        return;
    }

    state->depth--;
    if (state->depth == 0U) {
        state->active = false;
        state->tid = NULL;
        memset(state->stack, 0, sizeof(state->stack));
    } else {
        state->stack[state->depth].level = 0U;
        state->stack[state->depth].key = 0U;
    }

    k_spin_unlock(&g_registry_lock, spin_key);
}

void zepl_lock_enter_token(zepl_lock_token_t token) {
    zepl_lock_enter(token.level, token.key);
}

void zepl_lock_exit_token(zepl_lock_token_t token) {
    zepl_lock_exit(token.level, token.key);
}

void zepl_lock_reset_current_thread(void) {
    if (k_is_in_isr()) {
        return;
    }

    k_spinlock_key_t     spin_key = k_spin_lock(&g_registry_lock);
    thread_lock_state_t* state = find_state_locked(k_current_get(), false);
    if (state != NULL) {
        state->active = false;
        state->tid = NULL;
        state->depth = 0U;
        memset(state->stack, 0, sizeof(state->stack));
    }
    k_spin_unlock(&g_registry_lock, spin_key);
}

zepl_lock_token_t zepl_lock_current_token(void) {
    zepl_lock_token_t token = {.level = 0U, .key = 0U};

    if (k_is_in_isr()) {
        return token;
    }

    k_spinlock_key_t     spin_key = k_spin_lock(&g_registry_lock);
    thread_lock_state_t* state = find_state_locked(k_current_get(), false);

    if (state != NULL && state->depth > 0U) {
        token.level = state->stack[state->depth - 1U].level;
        token.key = state->stack[state->depth - 1U].key;
    }

    k_spin_unlock(&g_registry_lock, spin_key);
    return token;
}

zepl_lock_level_t zepl_lock_current_level(void) {
    return zepl_lock_current_token().level;
}

uintptr_t zepl_lock_current_key(void) {
    return zepl_lock_current_token().key;
}

uint8_t zepl_lock_current_depth(void) {
    uint8_t depth = 0U;

    if (k_is_in_isr()) {
        return depth;
    }

    k_spinlock_key_t     spin_key = k_spin_lock(&g_registry_lock);
    thread_lock_state_t* state = find_state_locked(k_current_get(), false);
    if (state != NULL) {
        depth = state->depth;
    }
    k_spin_unlock(&g_registry_lock, spin_key);

    return depth;
}

#else /* CONFIG_ZEPL_LOCK_ORDER not enabled */

/* =============================================================================
 * 空实现（CONFIG_ZEPL_LOCK_ORDER=n）：节省 ~4.4 KB RAM 及自旋锁开销
 * 所有对外符号保持与完整实现一致，链接器可正常解析。
 * ============================================================================= */

bool zepl_lock_order_is_valid(zepl_lock_level_t level, uintptr_t key) {
    ARG_UNUSED(level);
    ARG_UNUSED(key);
    return true;
}

void zepl_lock_enter(zepl_lock_level_t level, uintptr_t key) {
    ARG_UNUSED(level);
    ARG_UNUSED(key);
}

void zepl_lock_exit(zepl_lock_level_t level, uintptr_t key) {
    ARG_UNUSED(level);
    ARG_UNUSED(key);
}

void zepl_lock_enter_token(zepl_lock_token_t token) {
    ARG_UNUSED(token);
}

void zepl_lock_exit_token(zepl_lock_token_t token) {
    ARG_UNUSED(token);
}

void zepl_lock_reset_current_thread(void) {}

zepl_lock_token_t zepl_lock_current_token(void) {
    zepl_lock_token_t token = {.level = 0U, .key = 0U};

    return token;
}

zepl_lock_level_t zepl_lock_current_level(void) {
    return 0U;
}

uintptr_t zepl_lock_current_key(void) {
    return 0U;
}

uint8_t zepl_lock_current_depth(void) {
    return 0U;
}

#endif /* CONFIG_ZEPL_LOCK_ORDER */
