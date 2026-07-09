/**
 * @file connectivity_backend_wifi.c
 * @brief Wi-Fi 连接后端（ESP32-C6 等原生 Wi-Fi 目标）
 *
 * 基于 Zephyr net_mgmt / wifi_mgmt：init 注册 NET_EVENT_WIFI_CONNECT_RESULT /
 * NET_EVENT_WIFI_DISCONNECT_RESULT 事件回调；connect 从 provisioning 模块读取
 * 已保存的 SSID/PSK 并发起 NET_REQUEST_WIFI_CONNECT，随后有界等待回调结果再返回，
 * 使 connectivity_module 在后端 connect 返回后立即查询 is_link_up() 的既有同步语义
 * 依旧成立（真正的连接过程仍是异步完成，只是由本后端把等待收敛在 connect() 内）。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-07-09
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-07-09       1.0            zeh            ESP32-C6 Wi-Fi 集成：新增 Wi-Fi 后端
 *
 */

#include <zeplod/connectivity_backend.h>

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/util.h>

#include <zeplod/provisioning_module.h>

LOG_MODULE_REGISTER(connectivity_backend_wifi, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

/** Wi-Fi 后端上下文：net_mgmt 事件回调 + 有界等待信号量 */
typedef struct {
    struct net_if*                  iface;       /**< Wi-Fi 网络接口（net_if_get_first_wifi） */
    struct net_mgmt_event_callback  mgmt_cb;      /**< NET_EVENT_WIFI_* 事件回调登记块 */
    struct k_sem                    connect_sem;  /**< connect() 内有界等待用信号量 */
    bool                            link_up;      /**< 由事件回调更新的链路状态缓存 */
    bool                            inited;       /**< init 是否已成功 */
} connectivity_wifi_ctx_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static connectivity_wifi_ctx_t    g_wifi_ctx;
static connectivity_backend_ops_t g_wifi_ops;

/* =============================================================================
 * net_mgmt 事件回调
 * ============================================================================= */

/** 处理连接结果事件：按 wifi_status.status 更新 link_up，并唤醒 connect() 等待者 */
static void wifi_handle_connect_result(connectivity_wifi_ctx_t* ctx, struct net_mgmt_event_callback* cb) {
    const struct wifi_status* status = (const struct wifi_status*) cb->info;

    if (status != NULL && status->status == 0) {
        ctx->link_up = true;
        LOG_INF("Wi-Fi connected");
    } else {
        ctx->link_up = false;
        LOG_WRN("Wi-Fi connect failed (status=%d)", (status != NULL) ? status->status : -1);
    }
    k_sem_give(&ctx->connect_sem);
}

/** 处理断开事件：清 link_up；若 connect() 仍在等待也一并唤醒，避免误判为超时 */
static void wifi_handle_disconnect_result(connectivity_wifi_ctx_t* ctx) {
    ctx->link_up = false;
    LOG_INF("Wi-Fi disconnected");
    k_sem_give(&ctx->connect_sem);
}

/** net_mgmt 事件分发入口 */
static void wifi_mgmt_event_handler(struct net_mgmt_event_callback* cb, uint64_t mgmt_event, struct net_if* iface) {
    ARG_UNUSED(iface);

    switch (mgmt_event) {
        case NET_EVENT_WIFI_CONNECT_RESULT:
            wifi_handle_connect_result(&g_wifi_ctx, cb);
            break;
        case NET_EVENT_WIFI_DISCONNECT_RESULT:
            wifi_handle_disconnect_result(&g_wifi_ctx);
            break;
        default:
            break;
    }
}

/* =============================================================================
 * 后端回调
 * ============================================================================= */

static int wifi_init(connectivity_backend_ops_t* ops) {
    connectivity_wifi_ctx_t* ctx = (connectivity_wifi_ctx_t*) ops->ctx;

    if (ctx == NULL) {
        return -EINVAL;
    }

    ctx->iface = net_if_get_first_wifi();
    if (ctx->iface == NULL) {
        LOG_ERR("no Wi-Fi network interface found");
        return -ENODEV;
    }

    k_sem_init(&ctx->connect_sem, 0, 1);
    net_mgmt_init_event_callback(&ctx->mgmt_cb, wifi_mgmt_event_handler,
                                 NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&ctx->mgmt_cb);

    ctx->link_up = false;
    ctx->inited = true;
    return 0;
}

/**
 * @brief 从 provisioning 模块取凭据并发起连接，有界等待 net_mgmt 回调结果
 *
 * 等待上限由 CONFIG_CONNECTIVITY_BACKEND_WIFI_CONNECT_TIMEOUT_MS 控制；超时未收到
 * 回调时按失败处理（link_up 保持上次缓存值，通常为 false）。
 */
static int wifi_connect(connectivity_backend_ops_t* ops) {
    connectivity_wifi_ctx_t*       ctx = (connectivity_wifi_ctx_t*) ops->ctx;
    char                            ssid[PROVISIONING_WIFI_SSID_MAX_LEN];
    char                            psk[PROVISIONING_WIFI_PSK_MAX_LEN];
    struct wifi_connect_req_params params;
    int                             ret;

    if (ctx == NULL || !ctx->inited) {
        return -EINVAL;
    }

    ret = provisioning_module_get_credentials(ssid, sizeof(ssid), psk, sizeof(psk));
    if (ret != 0) {
        LOG_ERR("no Wi-Fi credentials configured (err=%d); inject via 'prov set-wifi <ssid> <psk>'", ret);
        return -ENOENT;
    }

    memset(&params, 0, sizeof(params));
    params.ssid = (const uint8_t*) ssid;
    params.ssid_length = (uint8_t) strlen(ssid);
    params.psk = (const uint8_t*) psk;
    params.psk_length = (uint8_t) strlen(psk);
    params.security = (params.psk_length > 0U) ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE;
    params.channel = WIFI_CHANNEL_ANY;
    /* 驱动侧超时（秒），与下方 k_sem 有界等待相互独立；两者均设置以保证收敛 */
    params.timeout = MAX(1, CONFIG_CONNECTIVITY_BACKEND_WIFI_CONNECT_TIMEOUT_MS / 1000);

    k_sem_reset(&ctx->connect_sem);

    ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, ctx->iface, &params, sizeof(params));
    if (ret != 0) {
        LOG_ERR("NET_REQUEST_WIFI_CONNECT failed: %d", ret);
        return ret;
    }

    ret = k_sem_take(&ctx->connect_sem, K_MSEC(CONFIG_CONNECTIVITY_BACKEND_WIFI_CONNECT_TIMEOUT_MS));
    if (ret != 0) {
        LOG_WRN("Wi-Fi connect timed out after %d ms", CONFIG_CONNECTIVITY_BACKEND_WIFI_CONNECT_TIMEOUT_MS);
        return -ETIMEDOUT;
    }

    return ctx->link_up ? 0 : -EIO;
}

static int wifi_disconnect(connectivity_backend_ops_t* ops) {
    connectivity_wifi_ctx_t* ctx = (connectivity_wifi_ctx_t*) ops->ctx;
    int                       ret;

    if (ctx == NULL) {
        return -EINVAL;
    }
    if (!ctx->link_up) {
        return 0;
    }

    ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, ctx->iface, NULL, 0);
    ctx->link_up = false;
    return ret;
}

static bool wifi_is_link_up(const connectivity_backend_ops_t* ops) {
    const connectivity_wifi_ctx_t* ctx = (const connectivity_wifi_ctx_t*) ops->ctx;

    if (ctx == NULL) {
        return false;
    }
    return ctx->link_up;
}

/* =============================================================================
 * 公开 API
 * ============================================================================= */

/** 填充 ops 表并返回单例（每次调用刷新函数指针） */
const connectivity_backend_ops_t* connectivity_backend_wifi_get(void) {
    g_wifi_ops.ctx = &g_wifi_ctx;
    g_wifi_ops.init = wifi_init;
    g_wifi_ops.connect = wifi_connect;
    g_wifi_ops.disconnect = wifi_disconnect;
    g_wifi_ops.is_link_up = wifi_is_link_up;
    return &g_wifi_ops;
}
