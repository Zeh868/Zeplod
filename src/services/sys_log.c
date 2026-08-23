/**
 * @file sys_log.c
 * @brief 系统日志服务实现
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 *
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/util.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <zeplod/sys_log.h>

#if defined(CONFIG_SEGGER_RTT)
#include <SEGGER_RTT.h>
#endif

LOG_MODULE_REGISTER(sys_log, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * SIL-2: 配置验证宏
 * ============================================================================= */

/** 最小日志缓冲区大小 */
#ifndef SYS_LOG_MIN_BUFFER_SIZE
#define SYS_LOG_MIN_BUFFER_SIZE 4U
#endif

/** 最大日志缓冲区大小 */
#ifndef SYS_LOG_MAX_BUFFER_SIZE
#define SYS_LOG_MAX_BUFFER_SIZE 1024U
#endif

/** 最大模块名称长度 */
#ifndef SYS_LOG_MAX_MODULE_NAME_LEN
#define SYS_LOG_MAX_MODULE_NAME_LEN 32U
#endif

/** 目的地数组大小 */
#define SYS_LOG_DEST_COUNT 4U

/* =============================================================================
 * 内部定义
 * ============================================================================= */

#ifndef CONFIG_SYS_MEMORY_POOL_SIZE
#define CONFIG_SYS_MEMORY_POOL_SIZE 8192
#endif

#define MAX_LOG_ENTRIES MAX(SYS_LOG_MIN_BUFFER_SIZE, (CONFIG_SYS_MEMORY_POOL_SIZE / sizeof(sys_log_entry_t)))

/* ANSI 颜色代码 */
#define COLOR_RED       "\x1b[31m"
#define COLOR_YELLOW    "\x1b[33m"
#define COLOR_GREEN     "\x1b[32m"
#define COLOR_BLUE      "\x1b[34m"
#define COLOR_RESET     "\x1b[0m"

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {
    sys_log_config_t config;
    sys_log_entry_t* buffer;
    uint32_t         write_idx;
    uint32_t         read_idx;
    uint32_t         count;
    uint32_t         total_count;
    uint32_t         log_cap; /* 环形缓冲实际使用的条目数，<= MAX_LOG_ENTRIES */
    struct k_mutex   lock;
    sys_log_level_t  module_levels[16]; /* 与 module_names 槽位一一对应 */
    bool             destinations_enabled[4];
    char             module_names[16][SYS_LOG_MAX_MODULE_NAME_LEN]; /* 已注册模块名，空槽表示未使用 */
} sys_log_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static sys_log_cb_t    g_sys_log;
static sys_log_entry_t g_log_buffer_static[MAX_LOG_ENTRIES];

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static const char* level_to_string(sys_log_level_t level) {
    switch (level) {
    case SYS_LOG_LEVEL_ERR:
        return "ERROR";
    case SYS_LOG_LEVEL_WRN:
        return "WARN";
    case SYS_LOG_LEVEL_INF:
        return "INFO";
    case SYS_LOG_LEVEL_DBG:
        return "DEBUG";
    default:
        return "UNKNOWN";
    }
}

static const char* level_to_color(sys_log_level_t level) {
    if (!g_sys_log.config.enable_colors) {
        return "";
    }

    switch (level) {
    case SYS_LOG_LEVEL_ERR:
        return COLOR_RED;
    case SYS_LOG_LEVEL_WRN:
        return COLOR_YELLOW;
    case SYS_LOG_LEVEL_INF:
        return COLOR_GREEN;
    case SYS_LOG_LEVEL_DBG:
        return COLOR_BLUE;
    default:
        return COLOR_RESET;
    }
}

static void apply_destinations_mask(uint32_t mask) {
    memset(g_sys_log.destinations_enabled, 0, sizeof(g_sys_log.destinations_enabled));

    if (mask == 0U) {
        mask = SYS_LOG_DEST_CONSOLE | SYS_LOG_DEST_MEMORY;
    }
    if (mask == SYS_LOG_DEST_ALL || mask == 0xFFu) {
        mask = (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3);
    }
    for (int i = 0; i < 4; i++) {
        if (mask & (1U << i)) {
            g_sys_log.destinations_enabled[i] = true;
        }
    }
}

/** 调用方须已持有 g_sys_log.lock */
static sys_log_level_t effective_level_for_module_locked(const char* module) {
    if (module == NULL || module[0] == '\0') {
        return g_sys_log.config.default_level;
    }

    for (int i = 0; i < 16; i++) {
        if (g_sys_log.module_names[i][0] == '\0') {
            continue;
        }
        if (strncmp(g_sys_log.module_names[i], module, SYS_LOG_MAX_MODULE_NAME_LEN) == 0) {
            return g_sys_log.module_levels[i];
        }
    }

    return g_sys_log.config.default_level;
}

static void add_entry(sys_log_level_t level, const char* module, const char* msg, uint32_t timestamp) {
    k_mutex_lock(&g_sys_log.lock, K_FOREVER);

    const uint32_t   cap = g_sys_log.log_cap ? g_sys_log.log_cap : 1U;
    sys_log_entry_t* entry = &g_sys_log.buffer[g_sys_log.write_idx];

    entry->timestamp = timestamp;
    entry->level = level;

    /* SIL-2: 安全复制模块名称,避免悬垂指针 */
    if (module != NULL) {
        size_t mod_len = strnlen(module, SYS_LOG_MAX_MODULE_NAME_LEN - 1);
        memcpy(entry->module_name, module, mod_len);
        entry->module_name[mod_len] = '\0';
    } else {
        entry->module_name[0] = '\0';
    }

    strncpy(entry->message, msg, SYS_LOG_MSG_MAX_LEN - 1);
    entry->message[SYS_LOG_MSG_MAX_LEN - 1] = '\0';

    g_sys_log.write_idx = (g_sys_log.write_idx + 1U) % cap;

    if (g_sys_log.count < cap) {
        g_sys_log.count++;
    } else {
        /* 缓冲区满，前移读索引 */
        g_sys_log.read_idx = (g_sys_log.read_idx + 1U) % cap;
    }

    g_sys_log.total_count++;

    k_mutex_unlock(&g_sys_log.lock);
}

static void output_to_printk(sys_log_level_t level, const char* module, const char* msg, uint32_t timestamp) {
    const char* color = level_to_color(level);
    const char* reset = g_sys_log.config.enable_colors ? COLOR_RESET : "";
    const char* mod_str = (module != NULL) ? module : "N/A";

    if (g_sys_log.config.enable_timestamp) {
        printk("%s[%08d]%s ", color, timestamp, reset);
    }

    if (g_sys_log.config.enable_module_name) {
        printk("%s[%s]%s ", color, mod_str, reset);
    }

    printk("%s%s%s\n", color, msg, reset);
}

#if defined(CONFIG_SEGGER_RTT)
static void output_to_rtt(sys_log_level_t level, const char* module, const char* msg, uint32_t timestamp) {
    ARG_UNUSED(level);

    char buf[SYS_LOG_MSG_MAX_LEN + 96];
    int  n = snprintf(buf, sizeof(buf), "[%08u][%s] %s\n", timestamp, (module != NULL) ? module : "-", msg);
    if (n > 0) {
        SEGGER_RTT_Write(0, buf, (unsigned) MIN((size_t) n, sizeof(buf)));
    }
}
#endif

static void emit_log_line(sys_log_level_t level, const char* module, const char* msg, uint32_t timestamp) {
    bool dest_console;
    bool dest_memory;
    bool dest_uart;
    bool dest_rtt;

    k_mutex_lock(&g_sys_log.lock, K_FOREVER);
    dest_console = g_sys_log.destinations_enabled[0];
    dest_memory = g_sys_log.destinations_enabled[1];
    dest_rtt = g_sys_log.destinations_enabled[2];
    dest_uart = g_sys_log.destinations_enabled[3];
    k_mutex_unlock(&g_sys_log.lock);

    if (dest_memory) {
        add_entry(level, module, msg, timestamp);
    }

    if (dest_console || dest_uart) {
        output_to_printk(level, module, msg, timestamp);
    }

#if defined(CONFIG_SEGGER_RTT)
    if (dest_rtt) {
        output_to_rtt(level, module, msg, timestamp);
    }
#endif
}

/* =============================================================================
 * API 实现
 * ============================================================================= */

int sys_log_init(const sys_log_config_t* config) {
    LOG_DBG("Initializing system log...");

    /* 并发日志进行中时拒绝重新初始化：memset 会摧毁 g_sys_log.lock 与环形缓冲游标。
     * BSS 零初始化的 k_mutex 在 Zephyr 中可直接加锁；首次调用时同样适用。
     * 已持锁：memset 后在下方统一 k_mutex_init 重建，无需释放旧锁。 */
    if (k_mutex_lock(&g_sys_log.lock, K_MSEC(1)) != 0) {
        LOG_ERR("sys_log_init rejected: concurrent logging in progress");
        return -EBUSY;
    }

    /* SIL-2: 清零全局控制块 */
    memset(&g_sys_log, 0, sizeof(g_sys_log));

    /* 设置默认配置 */
    if (config != NULL) {
        /* SIL-2: 验证配置参数 */
        if (config->memory_buffer_size > 0 && config->memory_buffer_size < sizeof(sys_log_entry_t)) {
            LOG_ERR("Invalid memory_buffer_size: %u", config->memory_buffer_size);
            return -EINVAL;
        }
        g_sys_log.config = *config;
    } else {
        g_sys_log.config.default_level = SYS_LOG_LEVEL_INF;
        g_sys_log.config.destinations = SYS_LOG_DEST_CONSOLE | SYS_LOG_DEST_MEMORY;
        g_sys_log.config.enable_timestamp = true;
        g_sys_log.config.enable_colors = true;
        g_sys_log.config.enable_module_name = true;
        g_sys_log.config.memory_buffer_size = CONFIG_SYS_MEMORY_POOL_SIZE;
    }

    /* 初始化缓冲区 */
    g_sys_log.buffer = g_log_buffer_static;
    g_sys_log.write_idx = 0;
    g_sys_log.read_idx = 0;
    g_sys_log.count = 0;
    g_sys_log.total_count = 0;

    {
        size_t ring_bytes = (size_t) g_sys_log.config.memory_buffer_size;
        if (ring_bytes < sizeof(sys_log_entry_t)) {
            ring_bytes = (size_t) sizeof(sys_log_entry_t) * SYS_LOG_MIN_BUFFER_SIZE;
        }
        uint32_t cap = (uint32_t) (ring_bytes / sizeof(sys_log_entry_t));
        if (cap > (uint32_t) MAX_LOG_ENTRIES) {
            cap = (uint32_t) MAX_LOG_ENTRIES;
        }
        if (cap < SYS_LOG_MIN_BUFFER_SIZE) {
            cap = MIN((uint32_t) MAX_LOG_ENTRIES, SYS_LOG_MIN_BUFFER_SIZE);
        }
        if (cap == 0U) {
            cap = 1U;
        }
        g_sys_log.log_cap = cap;
    }

    /* SIL-2: 验证编译期环形容量与配置合理性 */
    if (MAX_LOG_ENTRIES < SYS_LOG_MIN_BUFFER_SIZE || MAX_LOG_ENTRIES > SYS_LOG_MAX_BUFFER_SIZE) {
        LOG_WRN("MAX_LOG_ENTRIES %u outside reasonable range [%u, %u]", MAX_LOG_ENTRIES, SYS_LOG_MIN_BUFFER_SIZE,
                SYS_LOG_MAX_BUFFER_SIZE);
    }
    LOG_DBG("System log ring capacity: %u entries (static max %u)", g_sys_log.log_cap, (uint32_t) MAX_LOG_ENTRIES);

    k_mutex_init(&g_sys_log.lock);

    apply_destinations_mask(g_sys_log.config.destinations);

    /* 初始化模块级别 */
    for (int i = 0; i < 16; i++) {
        g_sys_log.module_levels[i] = g_sys_log.config.default_level;
    }

    LOG_DBG("System log initialized");
    return 0;
}

void sys_log_set_level(const char* module, sys_log_level_t level) {
    k_mutex_lock(&g_sys_log.lock, K_FOREVER);

    if (module == NULL || module[0] == '\0') {
        g_sys_log.config.default_level = level;
        for (int i = 0; i < 16; i++) {
            g_sys_log.module_levels[i] = level;
        }
        memset(g_sys_log.module_names, 0, sizeof(g_sys_log.module_names));
        k_mutex_unlock(&g_sys_log.lock);
        return;
    }

    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (g_sys_log.module_names[i][0] != '\0' &&
            strncmp(g_sys_log.module_names[i], module, SYS_LOG_MAX_MODULE_NAME_LEN) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < 16; i++) {
            if (g_sys_log.module_names[i][0] == '\0') {
                slot = i;
                break;
            }
        }
    }
    if (slot >= 0) {
        /* 复制最多 SYS_LOG_MAX_MODULE_NAME_LEN 字节，不强制 null 终止；
         * 数组初始化时已清零，短名称自然以 \0 结尾。
         * 查找时用 strncmp(..., SYS_LOG_MAX_MODULE_NAME_LEN) 保证一致性。 */
        strncpy(g_sys_log.module_names[slot], module, SYS_LOG_MAX_MODULE_NAME_LEN);
        g_sys_log.module_levels[slot] = level;
    } else {
        LOG_WRN("sys_log_set_level: module table full (max 16), ignoring '%s'", module);
    }

    k_mutex_unlock(&g_sys_log.lock);
}

sys_log_level_t sys_log_get_level(const char* module) {
    k_mutex_lock(&g_sys_log.lock, K_FOREVER);
    sys_log_level_t lv = effective_level_for_module_locked(module);
    k_mutex_unlock(&g_sys_log.lock);
    return lv;
}

void sys_log_set_destination(sys_log_dest_mask_t dest, bool enable) {
    k_mutex_lock(&g_sys_log.lock, K_FOREVER);

    if (dest == SYS_LOG_DEST_ALL) {
        for (int i = 0; i < 4; i++) {
            g_sys_log.destinations_enabled[i] = enable;
        }
        k_mutex_unlock(&g_sys_log.lock);
        return;
    }

    for (int i = 0; i < 4; i++) {
        if (dest & (1U << i)) {
            g_sys_log.destinations_enabled[i] = enable;
        }
    }

    k_mutex_unlock(&g_sys_log.lock);
}

void sys_log_print(sys_log_level_t level, const char* module, const char* format, ...) {
    k_mutex_lock(&g_sys_log.lock, K_FOREVER);
    sys_log_level_t thresh = effective_level_for_module_locked(module);
    k_mutex_unlock(&g_sys_log.lock);

    if (level > thresh) {
        return;
    }

    char    msg[SYS_LOG_MSG_MAX_LEN];
    va_list args;

    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);

    uint32_t ts = k_uptime_get_32();
    emit_log_line(level, module, msg, ts);
}

void sys_log_hexdump(sys_log_level_t level, const char* module, const void* data, size_t len, bool ascii) {
    k_mutex_lock(&g_sys_log.lock, K_FOREVER);
    sys_log_level_t thresh = effective_level_for_module_locked(module);
    k_mutex_unlock(&g_sys_log.lock);

    if (level > thresh) {
        return;
    }

    const uint8_t* bytes = (const uint8_t*) data;
    char           line[80];

    for (size_t i = 0; i < len; i += 16) {
        size_t chunk = MIN(16, len - i);
        int    pos = 0;

        /* 地址 */
        pos += snprintf(line + pos, sizeof(line) - pos, "%08X: ", (uint32_t) i);

        /* 十六进制字节 */
        for (size_t j = 0; j < 16; j++) {
            if (j < chunk) {
                pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", bytes[i + j]);
            } else {
                pos += snprintf(line + pos, sizeof(line) - pos, "   ");
            }
            if (j == 7)
                pos += snprintf(line + pos, sizeof(line) - pos, " ");
        }

        /* ASCII */
        if (ascii) {
            pos += snprintf(line + pos, sizeof(line) - pos, " |");
            for (size_t j = 0; j < chunk; j++) {
                char c = bytes[i + j];
                pos += snprintf(line + pos, sizeof(line) - pos, "%c", (c >= 32 && c < 127) ? c : '.');
            }
            pos += snprintf(line + pos, sizeof(line) - pos, "|");
        }

        /* 记录该行 */
        sys_log_print(level, module, "%s", line);
    }
}

uint32_t sys_log_get_entries(sys_log_entry_t* entries, uint32_t count, bool oldest_first) {
    if (entries == NULL || count == 0) {
        return 0;
    }

    k_mutex_lock(&g_sys_log.lock, K_FOREVER);

    uint32_t       available = g_sys_log.count;
    uint32_t       to_read = MIN(count, available);
    const uint32_t cap = g_sys_log.log_cap ? g_sys_log.log_cap : 1U;

    if (to_read == 0) {
        k_mutex_unlock(&g_sys_log.lock);
        return 0;
    }

    if (oldest_first) {
        /* 从读索引开始 */
        for (uint32_t i = 0; i < to_read; i++) {
            uint32_t idx = (g_sys_log.read_idx + i) % cap;
            entries[i] = g_sys_log.buffer[idx];
        }
    } else {
        /* 从写索引 - 1 开始（最新的在前）*/
        for (uint32_t i = 0; i < to_read; i++) {
            int32_t idx = (int32_t) g_sys_log.write_idx - 1 - (int32_t) i;
            if (idx < 0) {
                idx += (int32_t) cap;
            }
            entries[i] = g_sys_log.buffer[(uint32_t) idx];
        }
    }

    k_mutex_unlock(&g_sys_log.lock);
    return to_read;
}

void sys_log_clear_buffer(void) {
    k_mutex_lock(&g_sys_log.lock, K_FOREVER);
    g_sys_log.write_idx = 0;
    g_sys_log.read_idx = 0;
    g_sys_log.count = 0;
    k_mutex_unlock(&g_sys_log.lock);
}

uint32_t sys_log_get_count(void) {
    k_mutex_lock(&g_sys_log.lock, K_FOREVER);
    uint32_t c = g_sys_log.count;
    k_mutex_unlock(&g_sys_log.lock);
    return c;
}

void sys_log_dump(sys_log_level_t level_filter) {
    k_mutex_lock(&g_sys_log.lock, K_FOREVER);

    if (!g_sys_log.destinations_enabled[0] && !g_sys_log.destinations_enabled[3]) {
        k_mutex_unlock(&g_sys_log.lock);
        return;
    }

    const uint32_t cap = g_sys_log.log_cap ? g_sys_log.log_cap : 1U;
    const uint32_t total = g_sys_log.count;
    const uint32_t read_idx = g_sys_log.read_idx;

    k_mutex_unlock(&g_sys_log.lock);

    printk("\n=== Log Dump (max level: %d) ===\n", level_filter);

    /* 逐条持锁快照后锁外输出：dump 期间不再阻塞所有日志写入。
     * 代价是并发写入导致的环回覆盖可能使个别条目内容变化，诊断场景可接受。 */
    for (uint32_t i = 0; i < total; i++) {
        sys_log_entry_t entry;
        uint32_t        idx = (read_idx + i) % cap;

        k_mutex_lock(&g_sys_log.lock, K_FOREVER);
        entry = g_sys_log.buffer[idx];
        k_mutex_unlock(&g_sys_log.lock);

        if (level_filter != SYS_LOG_LEVEL_OFF && entry.level > level_filter) {
            continue;
        }

        printk("[%08d][%s][%s] %s\n", entry.timestamp, level_to_string(entry.level),
               (entry.module_name[0] != '\0') ? entry.module_name : "N/A", entry.message);
    }

    printk("=== End Log Dump ===\n\n");
}

/* =============================================================================
 * SYS_INIT 自动初始化
 * ============================================================================= */

#include <zeplod/app_config.h>

static int sys_log_auto_init(void) {
#if APP_CONFIG_ENABLE_LOGGING
    sys_log_config_t log_config = {.default_level = (sys_log_level_t) CONFIG_SYS_LOG_LEVEL,
                                   .destinations = SYS_LOG_DEST_CONSOLE | SYS_LOG_DEST_MEMORY,
                                   .enable_timestamp = true,
                                   .enable_colors = true,
                                   .enable_module_name = true,
                                   .memory_buffer_size = CONFIG_SYS_MEMORY_POOL_SIZE};
    sys_log_init(&log_config);
#endif
    return 0;
}

SYS_INIT(sys_log_auto_init, POST_KERNEL, APP_INIT_PRIO_SYS_LOG);
