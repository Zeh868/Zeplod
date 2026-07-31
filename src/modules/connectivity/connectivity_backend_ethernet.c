/**
 * @file connectivity_backend_ethernet.c
 * @brief 以太网连接后端（有线网口目标，如带 RMII/MAC 外设的板级）
 *
 * 基于 Zephyr net_mgmt / net_if：init 通过 net_if_get_first_by_type() 取以太网 L2 接口，
 * 注册 NET_EVENT_IF_UP / NET_EVENT_IF_DOWN / NET_EVENT_IPV4_ADDR_ADD 事件回调维护链路
 * 状态缓存；connect 确保接口 up 并（在 CONFIG_NET_DHCPV4 开启时）驱动 DHCP，随后有界
 * 等待获得 IPv4 地址再返回，使 connectivity_module 在 connect() 返回后立即查询
 * is_link_up() 的既有同步语义依旧成立。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-07-09
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-07-09       1.0            zeh            优先级注册表 + failover：新增以太网后端
 *
 */

#include <zeplod/connectivity_backend.h>

#include <errno.h>
#include <stddef.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_l2.h>
#include <zephyr/net/net_mgmt.h>

#if IS_ENABLED(CONFIG_NET_DHCPV4)
#include <zephyr/net/dhcpv4.h>
#endif

LOG_MODULE_REGISTER(connectivity_backend_ethernet, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

/** 以太网后端上下文：net_mgmt 事件回调 + 有界等待信号量 */
typedef struct {
    struct net_if*                 iface;        /**< 以太网网络接口 */
    struct net_mgmt_event_callback mgmt_cb;      /**< NET_EVENT_IF_UP/DOWN、IPV4_ADDR_ADD 回调登记块 */
    struct k_sem                   ip_sem;       /**< connect() 内等待 IPv4 地址就绪的信号量 */
    bool                           iface_up;     /**< 由 IF_UP/IF_DOWN 事件更新的接口状态缓存 */
    bool                           has_ipv4;     /**< 由 IPV4_ADDR_ADD/IF_DOWN 事件更新的地址状态缓存 */
    bool                           dhcp_started; /**< DHCP 客户端是否已启动，避免重复 start */
    bool                           inited;       /**< init 是否已成功 */
} connectivity_eth_ctx_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static connectivity_eth_ctx_t     g_eth_ctx;
static connectivity_backend_ops_t g_eth_ops;

/* =============================================================================
 * net_mgmt 事件回调
 * ============================================================================= */

/** net_mgmt 事件分发入口：仅处理本后端持有的接口，忽略其他网络接口上的同名事件 */
static void eth_mgmt_event_handler(struct net_mgmt_event_callback* cb, uint64_t mgmt_event, struct net_if* iface) {
    if (iface != g_eth_ctx.iface) {
        return;
    }

    switch (mgmt_event) {
        case NET_EVENT_IF_UP:
            g_eth_ctx.iface_up = true;
            LOG_INF("Ethernet interface up");
            break;
        case NET_EVENT_IF_DOWN:
            g_eth_ctx.iface_up = false;
            g_eth_ctx.has_ipv4 = false;
            LOG_INF("Ethernet interface down");
            k_sem_give(&g_eth_ctx.ip_sem); /* 唤醒可能仍在等待地址的 connect() */
            break;
        case NET_EVENT_IPV4_ADDR_ADD:
            g_eth_ctx.has_ipv4 = true;
            LOG_INF("Ethernet IPv4 address bound");
            k_sem_give(&g_eth_ctx.ip_sem);
            break;
        default:
            break;
    }

    ARG_UNUSED(cb);
}

/* =============================================================================
 * 后端回调
 * ============================================================================= */

static int eth_init(connectivity_backend_ops_t* ops) {
    connectivity_eth_ctx_t* ctx = (connectivity_eth_ctx_t*) ops->ctx;

    if (ctx == NULL) {
        return -EINVAL;
    }

    ctx->iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
    if (ctx->iface == NULL) {
        LOG_ERR("no Ethernet network interface found");
        return -ENODEV;
    }

    k_sem_init(&ctx->ip_sem, 0, 1);
    net_mgmt_init_event_callback(&ctx->mgmt_cb, eth_mgmt_event_handler,
                                 NET_EVENT_IF_UP | NET_EVENT_IF_DOWN | NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&ctx->mgmt_cb);

    ctx->iface_up = net_if_is_up(ctx->iface);
    ctx->has_ipv4 = false;
    ctx->dhcp_started = false;
    ctx->inited = true;
    return 0;
}

/**
 * @brief 确保接口 up 并（若启用 DHCPv4）驱动地址获取，有界等待 IPv4 地址就绪
 *
 * 等待上限由 CONFIG_CONNECTIVITY_BACKEND_ETHERNET_CONNECT_TIMEOUT_MS 控制；未开启
 * CONFIG_NET_DHCPV4 时不主动发起 DHCP，仅等待其他途径（如静态 IP 配置）触发的
 * NET_EVENT_IPV4_ADDR_ADD 事件，超时未收到则按失败处理。
 */
static int eth_connect(connectivity_backend_ops_t* ops) {
    connectivity_eth_ctx_t* ctx = (connectivity_eth_ctx_t*) ops->ctx;
    int                     ret;

    if (ctx == NULL || !ctx->inited) {
        return -EINVAL;
    }

    if (ctx->has_ipv4) {
        return 0; /* 已持有地址（如上次 connect 未断开），幂等直接成功 */
    }

    if (!ctx->iface_up) {
        ret = net_if_up(ctx->iface);
        if (ret != 0 && ret != -EALREADY) {
            LOG_ERR("net_if_up failed: %d", ret);
            return ret;
        }
        ctx->iface_up = true;
    }

#if IS_ENABLED(CONFIG_NET_DHCPV4)
    if (!ctx->dhcp_started) {
        net_dhcpv4_start(ctx->iface);
        ctx->dhcp_started = true;
    }
#endif

    k_sem_reset(&ctx->ip_sem);

    ret = k_sem_take(&ctx->ip_sem, K_MSEC(CONFIG_CONNECTIVITY_BACKEND_ETHERNET_CONNECT_TIMEOUT_MS));
    if (ret != 0) {
        LOG_WRN("Ethernet IPv4 address wait timed out after %d ms",
                CONFIG_CONNECTIVITY_BACKEND_ETHERNET_CONNECT_TIMEOUT_MS);
        return -ETIMEDOUT;
    }

    return ctx->has_ipv4 ? 0 : -EIO;
}

static int eth_disconnect(connectivity_backend_ops_t* ops) {
    connectivity_eth_ctx_t* ctx = (connectivity_eth_ctx_t*) ops->ctx;

    if (ctx == NULL) {
        return -EINVAL;
    }
    if (!ctx->iface_up && !ctx->has_ipv4) {
        return 0;
    }

#if IS_ENABLED(CONFIG_NET_DHCPV4)
    if (ctx->dhcp_started) {
        net_dhcpv4_stop(ctx->iface);
        ctx->dhcp_started = false;
    }
#endif
    ctx->has_ipv4 = false;
    /* 仅停用 DHCP/清缓存，不强制下线物理接口：以太网线缆通常保持插着，交由后续
       connect() 或 IF_UP/DOWN 事件驱动的缓存更新自然收敛，避免误伤共享该 iface 的
       其他子系统 */
    return 0;
}

static bool eth_is_link_up(const connectivity_backend_ops_t* ops) {
    const connectivity_eth_ctx_t* ctx = (const connectivity_eth_ctx_t*) ops->ctx;

    if (ctx == NULL) {
        return false;
    }
    return ctx->iface_up && ctx->has_ipv4;
}

/** 载波存在即视为“硬件可用”，即便当前未 up 也允许 connect_auto/failover 尝试拉起 */
static bool eth_is_available(const connectivity_backend_ops_t* ops) {
    const connectivity_eth_ctx_t* ctx = (const connectivity_eth_ctx_t*) ops->ctx;

    if (ctx == NULL || ctx->iface == NULL) {
        return false;
    }
    return net_if_is_carrier_ok(ctx->iface);
}

/* =============================================================================
 * 公开 API
 * ============================================================================= */

/** 填充 ops 表并返回单例（每次调用刷新函数指针） */
const connectivity_backend_ops_t* connectivity_backend_ethernet_get(void) {
    g_eth_ops.ctx = &g_eth_ctx;
    g_eth_ops.init = eth_init;
    g_eth_ops.connect = eth_connect;
    g_eth_ops.disconnect = eth_disconnect;
    g_eth_ops.is_link_up = eth_is_link_up;
    g_eth_ops.is_available = eth_is_available;
    return &g_eth_ops;
}
