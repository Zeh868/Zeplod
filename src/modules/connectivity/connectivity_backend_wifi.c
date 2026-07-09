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
 * @version 1.1
 * @date 2026-07-09
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-07-09       1.0            zeh            ESP32-C6 Wi-Fi 集成：新增 Wi-Fi 后端
 * 2026-07-09       1.1            zeh            打通首连触发链路：实现 is_available()，
 *                                                 未配网（无已保存凭据）时不再被 connect_auto()/
 *                                                 failover 空转尝试连接
 * 2026-07-10       1.2            zeh            iface 惰性获取：init 时 Wi-Fi net_if 尚未就绪
 *                                                 （驱动 init 晚于 connectivity 模块）不再返回失败，
 *                                                 避免整模块注册失败而永不 RUNNING；改在
 *                                                 connect/is_available 首次需要时获取并缓存
 * 2026-07-10       1.3            zeh            链路状态改读 net_if 真实 oper 状态（对齐 eth/cellular），
 *                                                 不再依赖易脱节的事件缓存；connect 检测到已关联则直接
 *                                                 返回成功，避免对活链路重复 CONNECT 造成抖动与误判 ERROR
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

/**
 * @brief 惰性获取并缓存 Wi-Fi 网络接口
 *
 * ESP32 等平台的 Wi-Fi net_if 由驱动在网络子系统初始化阶段创建，其 init 优先级可能晚于
 * 本后端所属 connectivity 模块的注册/初始化时机（APP_INIT_PRIO_MODULE_CONNECTIVITY=55）。
 * 故 init 时 net_if_get_first_wifi() 可能返回 NULL；本函数在首次真正需要 iface（连接 /
 * 可用性查询）时再获取并缓存，规避「因启动时序导致后端 init 失败 → connectivity 模块整体
 * 注册失败 → 永不进入 RUNNING」这一连锁问题。
 *
 * @param ctx Wi-Fi 后端上下文
 * @return 已持有可用 iface 返回 true，否则 false
 */
static bool wifi_ensure_iface(connectivity_wifi_ctx_t* ctx) {
    if (ctx->iface == NULL) {
        ctx->iface = net_if_get_first_wifi();
    }
    return ctx->iface != NULL;
}

/**
 * @brief 查询 Wi-Fi 链路是否真正处于已关联/可用状态
 *
 * 直接读网络接口的 admin/oper 状态（与 ethernet/cellular 后端一致），而非依赖由 net_mgmt
 * 事件更新的缓存标志 ctx->link_up。缓存易与真实链路脱节（如驱动侧自行重连、瞬时事件把缓存
 * 清零），会导致 connectivity 误判链路已断而反复对活链路重发 CONNECT，进而 deauth/抖动并
 * 误标 ERROR。oper==UP 表示已关联且载波就绪，与 `net iface` 显示的 oper 状态一致。
 *
 * @param iface Wi-Fi 网络接口（可为 NULL）
 * @return 链路真实可用返回 true，否则 false
 */
static bool wifi_link_really_up(struct net_if* iface) {
    return iface != NULL && net_if_is_up(iface) && net_if_oper_state(iface) == NET_IF_OPER_UP;
}

static int wifi_init(connectivity_backend_ops_t* ops) {
    connectivity_wifi_ctx_t* ctx = (connectivity_wifi_ctx_t*) ops->ctx;

    if (ctx == NULL) {
        return -EINVAL;
    }

    k_sem_init(&ctx->connect_sem, 0, 1);
    /* net_mgmt 回调按事件掩码全局登记，与具体 iface 无关，可在 iface 尚未就绪时先登记 */
    net_mgmt_init_event_callback(&ctx->mgmt_cb, wifi_mgmt_event_handler,
                                 NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&ctx->mgmt_cb);

    ctx->link_up = false;
    ctx->iface = NULL;
    /* iface 允许暂缺：不因缺 net_if 让 init 失败，改为惰性获取（见 wifi_ensure_iface） */
    if (!wifi_ensure_iface(ctx)) {
        LOG_WRN("Wi-Fi iface not ready at init; will acquire lazily on first use");
    }
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

    if (!wifi_ensure_iface(ctx)) {
        LOG_ERR("Wi-Fi iface still unavailable; cannot connect");
        return -ENODEV;
    }

    /* 已经关联则直接成功：避免对活链路重复发起 CONNECT 造成 deauth/重关联抖动，也让
       connect_auto()/failover 在链路已 up 时收敛到 UP，而非反复重连并误标 ERROR */
    if (wifi_link_really_up(ctx->iface)) {
        ctx->link_up = true;
        LOG_DBG("Wi-Fi already connected; skip re-connect");
        return 0;
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
    /* 以 net_if 真实状态为准，而非事件缓存 ctx->link_up，避免二者脱节导致的误判 */
    return wifi_link_really_up(ctx->iface);
}

/**
 * @brief 查询 Wi-Fi 后端当前是否可用（已配网、持有可用 SSID/PSK）
 *
 * 未配网（provisioning 尚无已保存凭据）时返回 false，使 connect_auto()/failover 跳过本
 * 后端，避免每次尝试都注定因缺凭据而失败并刷错误日志。provisioning_module_get_credentials()
 * 仅读取 app_kv 的 RAM 缓存（互斥锁保护，不做阻塞 flash I/O），满足 is_available 约定的
 * 非阻塞要求。
 */
static bool wifi_is_available(const connectivity_backend_ops_t* ops) {
    /* 需惰性获取 iface（会写 ctx->iface），故按可变指针访问全局上下文；ops->ctx 指向可变的
       g_wifi_ctx，去掉 const 限定安全 */
    connectivity_wifi_ctx_t* ctx = (connectivity_wifi_ctx_t*) ops->ctx;
    char                     ssid[PROVISIONING_WIFI_SSID_MAX_LEN];
    char                     psk[PROVISIONING_WIFI_PSK_MAX_LEN];

    if (ctx == NULL || !ctx->inited) {
        return false;
    }
    /* iface 未就绪时视为不可用：让 connect_auto()/failover 跳过并于后续轮询重试，
       直至 Wi-Fi 驱动建好 net_if */
    if (!wifi_ensure_iface(ctx)) {
        return false;
    }
    return provisioning_module_get_credentials(ssid, sizeof(ssid), psk, sizeof(psk)) == 0;
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
    g_wifi_ops.is_available = wifi_is_available;
    return &g_wifi_ops;
}
