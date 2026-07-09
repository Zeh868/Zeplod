/**
 * @file connectivity_backend_cellular.c
 * @brief 蜂窝连接后端骨架（通用 Zephyr modem 子系统，需按具体硬件适配）
 *
 * 面向 Zephyr modem 子系统的通用骨架实现：init 尝试通过 net_if_get_first_by_type() 匹配
 * PPP L2 接口（CONFIG_NET_L2_PPP 开启时，多数 UART modem 驱动的常见形态），否则回退取
 * 默认网络接口；注册 net_mgmt 监听 NET_EVENT_L4_CONNECTED/DISCONNECTED，connect 拉起
 * 接口后有界等待 L4 连接建立。
 *
 * @par 局限（重要，务必阅读）
 * Zephyr 并无跨厂商统一的“取蜂窝 modem 接口”API：不同 modem 驱动（SIMCom/Quectel 等）
 * 绑定后可能表现为 PPP 接口，也可能是 offloaded socket 接口，因此 cellular_find_iface()
 * 只是尽力而为的通用回退，不保证在具体硬件上精确匹配到蜂窝接口（多网卡场景下尤其
 * 可能取错）。真实部署时请：
 * 1. 在设备树中绑定具体 modem 驱动（如 SIMCom/Quectel）并配置 APN；
 * 2. 按所选 modem 驱动实际的接口获取方式（通常见于其驱动文档/示例）替换/细化
 *    cellular_find_iface() 与 cellular_is_available() 的实现。
 * 本仓库不具备蜂窝硬件，本后端仅保证编译通过，未经真实设备验证。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-07-09
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-07-09       1.0            zeh            优先级注册表 + failover：新增蜂窝后端骨架
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

LOG_MODULE_REGISTER(connectivity_backend_cellular, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

/** 蜂窝后端上下文：net_mgmt 事件回调 + 有界等待信号量 */
typedef struct {
    struct net_if*                 iface;        /**< modem 对应网络接口（见文件头局限说明） */
    struct net_mgmt_event_callback mgmt_cb;       /**< NET_EVENT_L4_CONNECTED/DISCONNECTED 回调登记块 */
    struct k_sem                   connect_sem;   /**< connect() 内有界等待用信号量 */
    bool                            l4_connected; /**< 由事件回调更新的 L4 连接状态缓存 */
    bool                            inited;       /**< init 是否已成功 */
} connectivity_cellular_ctx_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static connectivity_cellular_ctx_t g_cell_ctx;
static connectivity_backend_ops_t  g_cell_ops;

/* =============================================================================
 * 接口发现（骨架，见文件头局限说明）
 * ============================================================================= */

/**
 * @brief 尽力而为地找出蜂窝 modem 对应的网络接口
 * @return 找到的接口；找不到任何候选时返回 NULL
 */
static struct net_if* cellular_find_iface(void) {
    struct net_if* iface = NULL;

#if IS_ENABLED(CONFIG_NET_L2_PPP)
    iface = net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));
#endif
    if (iface == NULL) {
        /* 回退：取默认网络接口。局限见文件头说明——多网卡场景下可能非蜂窝接口 */
        iface = net_if_get_default();
    }
    return iface;
}

/* =============================================================================
 * net_mgmt 事件回调
 * ============================================================================= */

/** net_mgmt 事件分发入口：仅处理本后端持有的接口，忽略其他网络接口上的同名事件 */
static void cellular_mgmt_event_handler(struct net_mgmt_event_callback* cb, uint64_t mgmt_event,
                                        struct net_if* iface) {
    if (iface != g_cell_ctx.iface) {
        return;
    }

    switch (mgmt_event) {
        case NET_EVENT_L4_CONNECTED:
            g_cell_ctx.l4_connected = true;
            LOG_INF("Cellular L4 connected");
            k_sem_give(&g_cell_ctx.connect_sem);
            break;
        case NET_EVENT_L4_DISCONNECTED:
            g_cell_ctx.l4_connected = false;
            LOG_INF("Cellular L4 disconnected");
            k_sem_give(&g_cell_ctx.connect_sem);
            break;
        default:
            break;
    }

    ARG_UNUSED(cb);
}

/* =============================================================================
 * 后端回调
 * ============================================================================= */

static int cellular_init(connectivity_backend_ops_t* ops) {
    connectivity_cellular_ctx_t* ctx = (connectivity_cellular_ctx_t*) ops->ctx;

    if (ctx == NULL) {
        return -EINVAL;
    }

    ctx->iface = cellular_find_iface();
    if (ctx->iface == NULL) {
        LOG_ERR("no cellular-capable network interface found");
        return -ENODEV;
    }

    k_sem_init(&ctx->connect_sem, 0, 1);
    net_mgmt_init_event_callback(&ctx->mgmt_cb, cellular_mgmt_event_handler,
                                 NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED);
    net_mgmt_add_event_callback(&ctx->mgmt_cb);

    ctx->l4_connected = false;
    ctx->inited = true;
    return 0;
}

/**
 * @brief 拉起接口并有界等待 modem 达成 L4 连接
 *
 * 等待上限由 CONFIG_CONNECTIVITY_BACKEND_CELLULAR_CONNECT_TIMEOUT_MS 控制（默认高于
 * Wi-Fi/以太网，因注网/PPP 协商通常更慢）。真实注网/协商过程由具体 modem 驱动 + Zephyr
 * 网络子系统完成，本后端仅负责拉起接口并等待 NET_EVENT_L4_CONNECTED；本仓库无蜂窝
 * 硬件，未经真实设备验证，真实行为取决于所绑定的 modem 驱动实现。
 */
static int cellular_connect(connectivity_backend_ops_t* ops) {
    connectivity_cellular_ctx_t* ctx = (connectivity_cellular_ctx_t*) ops->ctx;
    int                          ret;

    if (ctx == NULL || !ctx->inited) {
        return -EINVAL;
    }

    if (ctx->l4_connected) {
        return 0; /* 幂等：已连接直接成功 */
    }

    if (!net_if_is_up(ctx->iface)) {
        ret = net_if_up(ctx->iface);
        if (ret != 0 && ret != -EALREADY) {
            LOG_ERR("net_if_up failed: %d", ret);
            return ret;
        }
    }

    k_sem_reset(&ctx->connect_sem);

    ret = k_sem_take(&ctx->connect_sem, K_MSEC(CONFIG_CONNECTIVITY_BACKEND_CELLULAR_CONNECT_TIMEOUT_MS));
    if (ret != 0) {
        LOG_WRN("Cellular L4 connect wait timed out after %d ms",
               CONFIG_CONNECTIVITY_BACKEND_CELLULAR_CONNECT_TIMEOUT_MS);
        return -ETIMEDOUT;
    }

    return ctx->l4_connected ? 0 : -EIO;
}

static int cellular_disconnect(connectivity_backend_ops_t* ops) {
    connectivity_cellular_ctx_t* ctx = (connectivity_cellular_ctx_t*) ops->ctx;
    int                          ret;

    if (ctx == NULL) {
        return -EINVAL;
    }
    if (!ctx->l4_connected) {
        return 0;
    }

    ret = net_if_down(ctx->iface);
    ctx->l4_connected = false;
    return ret;
}

static bool cellular_is_link_up(const connectivity_backend_ops_t* ops) {
    const connectivity_cellular_ctx_t* ctx = (const connectivity_cellular_ctx_t*) ops->ctx;

    if (ctx == NULL) {
        return false;
    }
    return ctx->l4_connected;
}

/**
 * @brief 查询 modem 硬件是否可用
 *
 * 通用骨架无法感知具体 modem 芯片的注网状态/信号强度，此处仅判断接口是否已找到；
 * 真实部署建议结合所选 modem 驱动提供的状态查询（如 RSSI、SIM/注册状态）细化实现。
 */
static bool cellular_is_available(const connectivity_backend_ops_t* ops) {
    const connectivity_cellular_ctx_t* ctx = (const connectivity_cellular_ctx_t*) ops->ctx;

    return ctx != NULL && ctx->iface != NULL;
}

/* =============================================================================
 * 公开 API
 * ============================================================================= */

/** 填充 ops 表并返回单例（每次调用刷新函数指针） */
const connectivity_backend_ops_t* connectivity_backend_cellular_get(void) {
    g_cell_ops.ctx = &g_cell_ctx;
    g_cell_ops.init = cellular_init;
    g_cell_ops.connect = cellular_connect;
    g_cell_ops.disconnect = cellular_disconnect;
    g_cell_ops.is_link_up = cellular_is_link_up;
    g_cell_ops.is_available = cellular_is_available;
    return &g_cell_ops;
}
