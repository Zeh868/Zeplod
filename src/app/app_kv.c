/**
 * @file app_kv.c
 * @brief 应用键值表实现
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            正式发布
 * 2026-07-10       1.1            zeh            修复持久化：flash 加载从 app_kv_init(优先级 11，
 *                                                 早于 flash 驱动)推迟到独立 SYS_INIT(flash 之后)，
 *                                                 避免开机 load 必然失败导致重启后已保存 KV 丢失
 *
 */

#include <zeplod/app_kv.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(app_kv, CONFIG_SYS_LOG_LEVEL);

#if APP_CONFIG_ENABLE_APP_KV && IS_ENABLED(CONFIG_APP_KV_PERSIST)
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#endif

#if APP_CONFIG_ENABLE_APP_KV
typedef struct {
    char key[APP_KV_KEY_MAX_LEN];
    char value[APP_KV_VALUE_MAX_LEN];
    bool in_use;
} app_kv_slot_t;

static struct k_mutex g_kv_lock;
static app_kv_slot_t  g_kv[APP_KV_MAX_ENTRIES];
static bool           g_kv_ready;
static uint32_t       g_kv_schema_version;

typedef struct {
    uint32_t          from_ver;
    uint32_t          to_ver;
    app_kv_migrate_fn fn;
    void*             user_data;
    bool              in_use;
} app_kv_migrate_step_t;

static app_kv_migrate_step_t g_kv_migrations[APP_KV_MIGRATE_MAX_STEPS];

static int find_key_locked(const char* key) {
    for (int i = 0; i < APP_KV_MAX_ENTRIES; i++) {
        if (g_kv[i].in_use && strcmp(g_kv[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static void kv_strncpy_fill(char* dst, const char* src, size_t cap) {
    if (cap == 0U) {
        return;
    }
    (void) strncpy(dst, src, cap - 1U);
    dst[cap - 1U] = '\0';
}

#if IS_ENABLED(CONFIG_APP_KV_PERSIST)
static int find_free_locked(void) {
    for (int i = 0; i < APP_KV_MAX_ENTRIES; i++) {
        if (!g_kv[i].in_use) {
            return i;
        }
    }
    return -1;
}

#define KV_PERSIST_MAGIC   0x01764B41u
/*
 * 版本 2：每条记录头部由 klen(1) + vlen(1) 改为 klen(1) + vlen(2, LE)。
 * 旧版 vlen 为 8 位，无法表示 APP_KV_VALUE_MAX_LEN 允许的 >255 的值（最大 512），
 * 会截断长度字段导致保存后无法解码。改为 16 位长度并提升版本号；旧版本(1)的 blob
 * 在 decode 时因版本不匹配被安全拒绝（清表），不会误读为新格式。
 */
#define KV_PERSIST_VERSION 2U
#define KV_SETTINGS_KEY    "app_kv/d"

/** 持久化编解码缓冲（静态，避免 SYS_INIT/shell 栈上分配 APP_KV_PERSIST_BLOB_MAX） */
static uint8_t g_kv_persist_blob[APP_KV_PERSIST_BLOB_MAX];

/**
 * 持久化专用锁：保护 g_kv_persist_blob 在「编码/解码 → settings 读写」整个过程不被并发复用。
 * 锁序固定为 persist → kv（先持久化锁，再 KV 锁），任何路径不得反向，以避免死锁。
 */
static struct k_mutex g_kv_persist_lock;

static int kv_decode_into_slots(const uint8_t* buf, size_t len) {
    if (len < 8U) {
        return APP_ERR_INVALID_PARAM;
    }
    uint32_t magic = sys_get_le32(buf);
    if (magic != KV_PERSIST_MAGIC) {
        return APP_ERR_INVALID_PARAM;
    }
    uint16_t ver = sys_get_le16(buf + 4U);
    if (ver != KV_PERSIST_VERSION) {
        return APP_ERR_INVALID_PARAM;
    }
    uint16_t count = sys_get_le16(buf + 6U);
    if (count > (uint16_t) APP_KV_MAX_ENTRIES) {
        return APP_ERR_INVALID_PARAM;
    }

    memset(g_kv, 0, sizeof(g_kv));
    size_t off = 8U;

    for (uint16_t n = 0U; n < count; n++) {
        if (off + 3U > len) { /* 每条头部：klen(1) + vlen(2) */
            return APP_ERR_INVALID_PARAM;
        }
        uint8_t  klen = buf[off++];
        uint16_t vlen = sys_get_le16(buf + off);
        off += 2U;
        if (klen == 0U || klen >= APP_KV_KEY_MAX_LEN || vlen >= APP_KV_VALUE_MAX_LEN) {
            return APP_ERR_INVALID_PARAM;
        }
        if (off + (size_t) klen + 1U + (size_t) vlen + 1U > len) {
            return APP_ERR_INVALID_PARAM;
        }

        char tmp_key[APP_KV_KEY_MAX_LEN];
        char tmp_val[APP_KV_VALUE_MAX_LEN];

        memcpy(tmp_key, buf + off, (size_t) klen + 1U);
        off += (size_t) klen + 1U;
        memcpy(tmp_val, buf + off, (size_t) vlen + 1U);
        off += (size_t) vlen + 1U;

        if (tmp_key[klen] != '\0' || tmp_val[vlen] != '\0') {
            return APP_ERR_INVALID_PARAM;
        }

        int idx = find_key_locked(tmp_key);
        if (idx < 0) {
            idx = find_free_locked();
            if (idx < 0) {
                return APP_ERR_KV_FULL;
            }
            memcpy(g_kv[idx].key, tmp_key, sizeof(tmp_key));
        }
        memcpy(g_kv[idx].value, tmp_val, sizeof(tmp_val));
        g_kv[idx].in_use = true;
    }

    if (off != len) {
        return APP_ERR_INVALID_PARAM;
    }
    return APP_OK;
}

static int kv_encode_blob_locked(uint8_t* buf, size_t cap, size_t* out_len) {
    if (cap < 8U) {
        return APP_ERR_MEMORY;
    }

    uint16_t nused = 0U;
    for (int i = 0; i < APP_KV_MAX_ENTRIES; i++) {
        if (g_kv[i].in_use) {
            nused++;
        }
    }

    size_t off = 0U;
    sys_put_le32(KV_PERSIST_MAGIC, buf + off);
    off += 4U;
    sys_put_le16(KV_PERSIST_VERSION, buf + off);
    off += 2U;
    sys_put_le16(nused, buf + off);
    off += 2U;

    for (int i = 0; i < APP_KV_MAX_ENTRIES; i++) {
        if (!g_kv[i].in_use) {
            continue;
        }
        size_t klen = strlen(g_kv[i].key);
        size_t vlen = strlen(g_kv[i].value);
        if (klen == 0U || klen >= (size_t) APP_KV_KEY_MAX_LEN || vlen >= (size_t) APP_KV_VALUE_MAX_LEN) {
            return APP_ERR_INVALID_PARAM;
        }
        if (off + 3U + klen + 1U + vlen + 1U > cap) { /* 每条头部：klen(1) + vlen(2) */
            return APP_ERR_MEMORY;
        }
        buf[off++] = (uint8_t) klen;              /* key 长度 < 256，单字节足够 */
        sys_put_le16((uint16_t) vlen, buf + off); /* value 长度可达 512，需 16 位 */
        off += 2U;
        memcpy(buf + off, g_kv[i].key, klen + 1U);
        off += klen + 1U;
        memcpy(buf + off, g_kv[i].value, vlen + 1U);
        off += vlen + 1U;
    }

    *out_len = off;
    return APP_OK;
}
#endif /* CONFIG_APP_KV_PERSIST */

#if IS_ENABLED(CONFIG_APP_KV_PERSIST) && IS_ENABLED(CONFIG_APP_KV_PERSIST_AUTOSAVE)
#define KV_AUTOSAVE() (void) app_kv_save()
#else
#define KV_AUTOSAVE() ((void) 0)
#endif

void app_kv_init(void) {
    if (g_kv_ready) {
        return;
    }
    k_mutex_init(&g_kv_lock);
    memset(g_kv, 0, sizeof(g_kv));

#if IS_ENABLED(CONFIG_APP_KV_PERSIST)
    k_mutex_init(&g_kv_persist_lock);
    /* 本函数在 APP_INIT_PRIO_APP_KV(=11) 即完成 RAM 表就绪；但此刻 flash 驱动
       (CONFIG_FLASH_INIT_PRIORITY，通常 50) 尚未初始化，settings/NVS 不可访问。若在此加载
       持久值必然失败（settings_subsys_init 返回错误），导致重启后已保存的 KV 丢失。故将 flash
       加载推迟到 app_kv_restore_from_flash()（SYS_INIT，优先级晚于 flash 驱动、早于读取持久值
       的业务模块）执行。 */
#endif

    g_kv_ready = true;
}

#if IS_ENABLED(CONFIG_APP_KV_PERSIST)
/**
 * @brief flash 驱动就绪后从持久化存储恢复 KV（二次初始化）
 *
 * app_kv_init() 只完成 RAM 表就绪；受 SYS_INIT 优先级所限，其运行时 flash 驱动尚未初始化，
 * settings_subsys_init()/NVS 必失败，持久值无法读回（表现为重启后已保存的键丢失）。本 SYS_INIT
 * 的优先级 APP_INIT_PRIO_APP_KV_RESTORE 晚于 flash 驱动、早于首个读取持久值的业务模块（如
 * provisioning），在此调用 app_kv_load() 将持久化的键合并进 RAM 表。恢复失败不阻断内核初始化。
 *
 * @return 恒返回 0（SYS_INIT 语义：不因数据恢复失败而中止启动）
 */
static int app_kv_restore_from_flash(void) {
    int ret = app_kv_load();

    if (ret != APP_OK) {
        LOG_WRN("app_kv restore from flash failed: %d", ret);
    }
    return 0;
}

SYS_INIT(app_kv_restore_from_flash, POST_KERNEL, APP_INIT_PRIO_APP_KV_RESTORE);
#endif /* CONFIG_APP_KV_PERSIST */

int app_kv_register_migrate(uint32_t from_ver, uint32_t to_ver, app_kv_migrate_fn fn, void* user_data) {
    if (!g_kv_ready || fn == NULL || from_ver >= to_ver) {
        return APP_ERR_INVALID_PARAM;
    }

    k_mutex_lock(&g_kv_lock, K_FOREVER);
    for (int i = 0; i < APP_KV_MIGRATE_MAX_STEPS; i++) {
        if (g_kv_migrations[i].in_use && g_kv_migrations[i].from_ver == from_ver &&
            g_kv_migrations[i].to_ver == to_ver) {
            k_mutex_unlock(&g_kv_lock);
            return APP_ERR_ALREADY_EXISTS;
        }
    }

    int slot = -1;
    for (int i = 0; i < APP_KV_MIGRATE_MAX_STEPS; i++) {
        if (!g_kv_migrations[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        k_mutex_unlock(&g_kv_lock);
        return APP_ERR_KV_FULL;
    }

    g_kv_migrations[slot].from_ver = from_ver;
    g_kv_migrations[slot].to_ver = to_ver;
    g_kv_migrations[slot].fn = fn;
    g_kv_migrations[slot].user_data = user_data;
    g_kv_migrations[slot].in_use = true;
    k_mutex_unlock(&g_kv_lock);
    return APP_OK;
}

int app_kv_set_schema_version(uint32_t version) {
    if (!g_kv_ready) {
        return APP_ERR_INIT;
    }
    k_mutex_lock(&g_kv_lock, K_FOREVER);
    g_kv_schema_version = version;
    k_mutex_unlock(&g_kv_lock);
    return APP_OK;
}

uint32_t app_kv_get_schema_version(void) {
    uint32_t ver;

    if (!g_kv_ready) {
        return 0U;
    }
    k_mutex_lock(&g_kv_lock, K_FOREVER);
    ver = g_kv_schema_version;
    k_mutex_unlock(&g_kv_lock);
    return ver;
}

int app_kv_run_migrations(void) {
    if (!g_kv_ready) {
        return APP_ERR_INIT;
    }

    for (unsigned pass = 0U; pass < APP_KV_MIGRATE_MAX_STEPS; pass++) {
        app_kv_migrate_step_t step;
        bool                  found = false;

        k_mutex_lock(&g_kv_lock, K_FOREVER);
        for (int i = 0; i < APP_KV_MIGRATE_MAX_STEPS; i++) {
            if (!g_kv_migrations[i].in_use) {
                continue;
            }
            if (g_kv_migrations[i].from_ver == g_kv_schema_version) {
                step = g_kv_migrations[i];
                found = true;
                break;
            }
        }
        k_mutex_unlock(&g_kv_lock);

        if (!found) {
            return APP_OK;
        }

        if (step.fn == NULL) {
            return APP_ERR_INVALID_PARAM;
        }

        int ret = step.fn(step.from_ver, step.to_ver, step.user_data);
        if (ret != 0) {
            return ret;
        }

        k_mutex_lock(&g_kv_lock, K_FOREVER);
        if (g_kv_schema_version != step.from_ver) {
            k_mutex_unlock(&g_kv_lock);
            return APP_ERR_INVALID_PARAM;
        }
        g_kv_schema_version = step.to_ver;
        k_mutex_unlock(&g_kv_lock);
    }

    return APP_ERR_INVALID_PARAM;
}

int app_kv_set(const char* key, const char* value) {
    if (!g_kv_ready || key == NULL || value == NULL) {
        return APP_ERR_INVALID_PARAM;
    }
    if (key[0] == '\0') {
        return APP_ERR_INVALID_PARAM;
    }
    if (strlen(key) >= APP_KV_KEY_MAX_LEN || strlen(value) >= APP_KV_VALUE_MAX_LEN) {
        return APP_ERR_INVALID_PARAM;
    }

    k_mutex_lock(&g_kv_lock, K_FOREVER);

    int idx = -1;
    int free_idx = -1;
    for (int i = 0; i < APP_KV_MAX_ENTRIES; i++) {
        if (g_kv[i].in_use) {
            if (strcmp(g_kv[i].key, key) == 0) {
                idx = i;
                break;
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (idx < 0) {
        if (free_idx < 0) {
            k_mutex_unlock(&g_kv_lock);
            return APP_ERR_KV_FULL;
        }
        idx = free_idx;
    }

    kv_strncpy_fill(g_kv[idx].key, key, APP_KV_KEY_MAX_LEN);
    kv_strncpy_fill(g_kv[idx].value, value, APP_KV_VALUE_MAX_LEN);
    g_kv[idx].in_use = true;

    k_mutex_unlock(&g_kv_lock);
    KV_AUTOSAVE();
    return APP_OK;
}

int app_kv_get(const char* key, char* out, size_t out_len) {
    if (!g_kv_ready || key == NULL || out == NULL || out_len == 0U) {
        return APP_ERR_INVALID_PARAM;
    }

    k_mutex_lock(&g_kv_lock, K_FOREVER);
    int idx = find_key_locked(key);
    if (idx < 0) {
        k_mutex_unlock(&g_kv_lock);
        return APP_ERR_NOT_FOUND;
    }
    kv_strncpy_fill(out, g_kv[idx].value, out_len);
    k_mutex_unlock(&g_kv_lock);
    return APP_OK;
}

bool app_kv_has(const char* key) {
    if (!g_kv_ready || key == NULL) {
        return false;
    }
    k_mutex_lock(&g_kv_lock, K_FOREVER);
    int idx = find_key_locked(key);
    k_mutex_unlock(&g_kv_lock);
    return idx >= 0;
}

int app_kv_remove(const char* key) {
    if (!g_kv_ready || key == NULL) {
        return APP_ERR_INVALID_PARAM;
    }

    k_mutex_lock(&g_kv_lock, K_FOREVER);
    int idx = find_key_locked(key);
    if (idx < 0) {
        k_mutex_unlock(&g_kv_lock);
        return APP_ERR_NOT_FOUND;
    }
    memset(&g_kv[idx], 0, sizeof(g_kv[idx]));
    k_mutex_unlock(&g_kv_lock);
    KV_AUTOSAVE();
    return APP_OK;
}

void app_kv_clear(void) {
    if (!g_kv_ready) {
        return;
    }
    k_mutex_lock(&g_kv_lock, K_FOREVER);
    memset(g_kv, 0, sizeof(g_kv));
    k_mutex_unlock(&g_kv_lock);
    KV_AUTOSAVE();
}

size_t app_kv_count(void) {
    if (!g_kv_ready) {
        return 0U;
    }
    size_t n = 0U;
    k_mutex_lock(&g_kv_lock, K_FOREVER);
    for (int i = 0; i < APP_KV_MAX_ENTRIES; i++) {
        if (g_kv[i].in_use) {
            n++;
        }
    }
    k_mutex_unlock(&g_kv_lock);
    return n;
}

int app_kv_foreach(app_kv_visit_fn fn, void* user) {
    if (!g_kv_ready || fn == NULL) {
        return APP_ERR_INVALID_PARAM;
    }

    k_mutex_lock(&g_kv_lock, K_FOREVER);
    for (int i = 0; i < APP_KV_MAX_ENTRIES; i++) {
        if (!g_kv[i].in_use) {
            continue;
        }
        int r = fn(g_kv[i].key, g_kv[i].value, user);
        if (r != 0) {
            k_mutex_unlock(&g_kv_lock);
            return r;
        }
    }
    k_mutex_unlock(&g_kv_lock);
    return 0;
}

int app_kv_set_int32(const char* key, int32_t v) {
    char buf[16];
    (void) snprintf(buf, sizeof(buf), "%ld", (long) v);
    return app_kv_set(key, buf);
}

int app_kv_get_int32(const char* key, int32_t* out) {
    if (out == NULL) {
        return APP_ERR_INVALID_PARAM;
    }
    char buf[APP_KV_VALUE_MAX_LEN];
    int  ret = app_kv_get(key, buf, sizeof(buf));
    if (ret != APP_OK) {
        return ret;
    }
    errno = 0;
    char* end = NULL;
    long  lv = strtol(buf, &end, 10);
    if (end == buf || *end != '\0') {
        return APP_ERR_INVALID_PARAM;
    }
    if (errno == ERANGE || lv < (long) INT32_MIN || lv > (long) INT32_MAX) {
        return APP_ERR_INVALID_PARAM;
    }
    *out = (int32_t) lv;
    return APP_OK;
}

int app_kv_save(void) {
#if !IS_ENABLED(CONFIG_APP_KV_PERSIST)
    return APP_ERR_DISABLED;
#else
    if (!g_kv_ready) {
        return APP_ERR_INIT;
    }
    size_t len = 0U;
    /* persist 锁覆盖「编码 → settings_save_one」全程，避免其它 save/load 复用缓冲。 */
    k_mutex_lock(&g_kv_persist_lock, K_FOREVER);
    k_mutex_lock(&g_kv_lock, K_FOREVER);
    int enc = kv_encode_blob_locked(g_kv_persist_blob, sizeof(g_kv_persist_blob), &len);
    k_mutex_unlock(&g_kv_lock);
    if (enc != APP_OK) {
        k_mutex_unlock(&g_kv_persist_lock);
        return enc;
    }
    if (settings_subsys_init() != 0) {
        k_mutex_unlock(&g_kv_persist_lock);
        return APP_ERR_INIT;
    }
    int w = settings_save_one(KV_SETTINGS_KEY, g_kv_persist_blob, len);
    k_mutex_unlock(&g_kv_persist_lock);
    return w == 0 ? APP_OK : APP_ERR_IO;
#endif
}

int app_kv_load(void) {
#if !IS_ENABLED(CONFIG_APP_KV_PERSIST)
    return APP_ERR_DISABLED;
#else
    if (!g_kv_ready) {
        return APP_ERR_INIT;
    }
    if (settings_subsys_init() != 0) {
        return APP_ERR_INIT;
    }
    /* persist 锁覆盖「settings_load_one → 解码」全程，避免其它 save/load 复用缓冲。 */
    k_mutex_lock(&g_kv_persist_lock, K_FOREVER);
    ssize_t n = settings_load_one(KV_SETTINGS_KEY, g_kv_persist_blob, sizeof(g_kv_persist_blob));
    if (n < 0) {
        if (n == -ENOENT) {
            k_mutex_lock(&g_kv_lock, K_FOREVER);
            memset(g_kv, 0, sizeof(g_kv));
            k_mutex_unlock(&g_kv_lock);
            k_mutex_unlock(&g_kv_persist_lock);
            return APP_OK;
        }
        k_mutex_unlock(&g_kv_persist_lock);
        return APP_ERR_IO;
    }
    k_mutex_lock(&g_kv_lock, K_FOREVER);
    int d = kv_decode_into_slots(g_kv_persist_blob, (size_t) n);
    if (d != APP_OK) {
        memset(g_kv, 0, sizeof(g_kv));
    }
    k_mutex_unlock(&g_kv_lock);
    k_mutex_unlock(&g_kv_persist_lock);
    return d == APP_OK ? APP_OK : APP_ERR_INVALID_PARAM;
#endif
}
#else  /* !APP_CONFIG_ENABLE_APP_KV */

void app_kv_init(void) {}

int app_kv_set(const char* key, const char* value) {
    ARG_UNUSED(key);
    ARG_UNUSED(value);
    return APP_ERR_DISABLED;
}

int app_kv_get(const char* key, char* out, size_t out_len) {
    ARG_UNUSED(key);
    ARG_UNUSED(out);
    ARG_UNUSED(out_len);
    return APP_ERR_DISABLED;
}

bool app_kv_has(const char* key) {
    ARG_UNUSED(key);
    return false;
}

int app_kv_remove(const char* key) {
    ARG_UNUSED(key);
    return APP_ERR_DISABLED;
}

void app_kv_clear(void) {}

size_t app_kv_count(void) {
    return 0U;
}

int app_kv_foreach(app_kv_visit_fn fn, void* user) {
    ARG_UNUSED(fn);
    ARG_UNUSED(user);
    return APP_ERR_DISABLED;
}

int app_kv_set_int32(const char* key, int32_t v) {
    ARG_UNUSED(key);
    ARG_UNUSED(v);
    return APP_ERR_DISABLED;
}

int app_kv_get_int32(const char* key, int32_t* out) {
    ARG_UNUSED(key);
    ARG_UNUSED(out);
    return APP_ERR_DISABLED;
}

int app_kv_save(void) {
    return APP_ERR_DISABLED;
}

int app_kv_load(void) {
    return APP_ERR_DISABLED;
}

int app_kv_register_migrate(uint32_t from_ver, uint32_t to_ver, app_kv_migrate_fn fn, void* user_data) {
    ARG_UNUSED(from_ver);
    ARG_UNUSED(to_ver);
    ARG_UNUSED(fn);
    ARG_UNUSED(user_data);
    return APP_ERR_DISABLED;
}

int app_kv_set_schema_version(uint32_t version) {
    ARG_UNUSED(version);
    return APP_ERR_DISABLED;
}

uint32_t app_kv_get_schema_version(void) {
    return 0U;
}

int app_kv_run_migrations(void) {
    return APP_ERR_DISABLED;
}
#endif /* APP_CONFIG_ENABLE_APP_KV */
