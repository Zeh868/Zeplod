/**
 * @file connectivity_backend_cellular.c
 * @brief 蜂窝连接后端（驱动 Zephyr 通用 modem_cellular 框架的 PPP 网口）
 *
 * 本后端与具体 modem 型号解耦，驱动 Zephyr `CONFIG_MODEM_CELLULAR` 通用蜂窝驱动统一暴露的
 * PPP 网络接口的生命周期。modem_cellular 已内建支持 Quectel（BG9x/EG25-G 等）、SIMCom
 * （SIM7080/A76xx）、u-blox（SARA-R4/R5、LARA-R6）、Telit、Sierra HL78xx 等常见模块，
 * 用户只需在设备树绑定对应 compatible 的 modem 节点并配置 APN（驱动 Kconfig
 * `CONFIG_MODEM_CELLULAR_APN`），本后端无需为每颗芯片改动。
 *
 * @par 连接语义（对齐 Zephyr samples/net/cellular_modem）
 * - init：取 PPP L2 接口（`net_if_get_first_by_type(PPP)`），注册
 *   NET_EVENT_L4_CONNECTED/DISCONNECTED 回调（由连接管理器 conn_mgr 产生）。
 * - connect：若启用 PM_DEVICE 且设备树含标准 `modem` 别名，先 `pm_device_action_run(RESUME)`
 *   唤醒 modem；再 `net_if_up()` 触发附着/拨号；有界等待 L4 连接建立
 *   （超时 `CONFIG_CONNECTIVITY_BACKEND_CELLULAR_CONNECT_TIMEOUT_MS`，默认 30s）。
 * - disconnect：`net_if_down()` 断开；启用 PM_DEVICE 时再 `pm_device_action_run(SUSPEND)`。
 * - is_link_up：依 conn_mgr L4 事件维护的缓存。
 * - is_available：有 PPP 网口，且（若有 modem 别名）modem 设备就绪。
 *
 * @par 局限
 * L4 连接/断开事件由 conn_mgr 以“全局是否至少一路 L4 可达”粒度产生（非严格按接口）；在多后端
 * 共存场景下，若已有更高优先级链路处于 L4 连接态，本 PPP 口再连上可能不会再触发一次
 * NET_EVENT_L4_CONNECTED。但在本模块的 failover 状态机中，仅当无更高优先级链路可用时才会
 * 尝试蜂窝 connect（蜂窝优先级低于以太网/Wi-Fi，也不会抢占它们），故该边界在实际调用序中
 * 基本不出现；如需严格按接口跟踪，可改用接口级 net_mgmt 事件，留作未决项。
 * 无 modem 硬件时本后端仅保证编译通过，未经真实设备端到端验证。
 *
 * @author zeh (china_qzh@163.com)
 * @version 2.0
 * @date 2026-07-09
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-07-09       1.0            zeh            优先级注册表 + failover：新增蜂窝后端骨架
 * 2026-07-09       2.0            zeh            做实：驱动 modem_cellular PPP 网口生命周期，去除默认口兜底
 *
 */

#include <zeplod/connectivity_backend.h>

#include <errno.h>
#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_l2.h>
#include <zephyr/net/net_mgmt.h>

#if IS_ENABLED(CONFIG_PM_DEVICE)
#include <zephyr/pm/device.h>
#endif

LOG_MODULE_REGISTER(connectivity_backend_cellular, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * modem 设备（可选：设备树提供标准 "modem" 别名时用于电源管理）
 * ============================================================================= */

/**
 * 多数 modem_cellular 板级 overlay 会为 modem 节点加 `aliases { modem = &... };`
 * （见 Zephyr cellular_modem 示例）。有该别名时据其做 PM resume/suspend；无别名时
 * （如无 modem 硬件的 build-only 配置）退化为不做电源管理，仅驱动 PPP 口 admin up/down。
 */
#if DT_NODE_EXISTS(DT_ALIAS(modem))
#define CONN_CELL_HAS_MODEM_DEV 1
static const struct device* const g_cell_modem = DEVICE_DT_GET(DT_ALIAS(modem));
#else
#define CONN_CELL_HAS_MODEM_DEV 0
static const struct device* const g_cell_modem = NULL;
#endif

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

/** 蜂窝后端上下文：net_mgmt 事件回调 + 有界等待信号量 */
typedef struct {
    struct net_if*                 iface;        /**< modem 暴露的 PPP 网络接口 */
    struct net_mgmt_event_callback mgmt_cb;      /**< NET_EVENT_L4_CONNECTED/DISCONNECTED 回调登记块 */
    struct k_sem                   connect_sem;  /**< connect() 内有界等待用信号量 */
    bool                           l4_connected; /**< 由 conn_mgr L4 事件更新的连接状态缓存 */
    bool                           inited;       /**< init 是否已成功 */
} connectivity_cellular_ctx_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static connectivity_cellular_ctx_t g_cell_ctx;
static connectivity_backend_ops_t  g_cell_ops;

/* =============================================================================
 * 电源管理辅助（仅在 PM_DEVICE + 有 modem 别名时生效，否则为空操作）
 * ============================================================================= */

/** 唤醒 modem 设备（如启用 PM_DEVICE 且存在 modem 别名） */
static void cellular_modem_resume(void) {
#if IS_ENABLED(CONFIG_PM_DEVICE) && CONN_CELL_HAS_MODEM_DEV
    if (g_cell_modem != NULL && device_is_ready(g_cell_modem)) {
        (void) pm_device_action_run(g_cell_modem, PM_DEVICE_ACTION_RESUME);
    }
#endif
}

/** 挂起 modem 设备（如启用 PM_DEVICE 且存在 modem 别名） */
static void cellular_modem_suspend(void) {
#if IS_ENABLED(CONFIG_PM_DEVICE) && CONN_CELL_HAS_MODEM_DEV
    if (g_cell_modem != NULL && device_is_ready(g_cell_modem)) {
        (void) pm_device_action_run(g_cell_modem, PM_DEVICE_ACTION_SUSPEND);
    }
#endif
}

/* =============================================================================
 * 接口发现
 * ============================================================================= */

/**
 * @brief 取 modem_cellular 暴露的 PPP 网络接口
 * @return PPP 接口；不存在时返回 NULL（不再回退到默认接口，避免误用非蜂窝网口）
 */
static struct net_if* cellular_find_iface(void) {
    struct net_if* iface = net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));

    if (iface == NULL) {
        LOG_ERR("no PPP (cellular modem) interface found; bind a modem_cellular compatible node "
                "in devicetree (see docs) and enable CONFIG_MODEM_CELLULAR");
    }
    return iface;
}

/* =============================================================================
 * net_mgmt 事件回调
 * ============================================================================= */

/** conn_mgr L4 事件分发入口：仅处理本后端持有的 PPP 接口 */
static void cellular_mgmt_event_handler(struct net_mgmt_event_callback* cb, uint64_t mgmt_event, struct net_if* iface) {
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
 * @brief 唤醒 modem、拉起 PPP 口触发附着/拨号，有界等待 L4 连接建立
 *
 * 等待上限由 CONFIG_CONNECTIVITY_BACKEND_CELLULAR_CONNECT_TIMEOUT_MS 控制（默认 30s，高于
 * Wi-Fi/以太网，因注网 + PPP 协商通常更慢）。真正的附着/拨号由 modem_cellular 驱动 + PPP
 * 协议栈完成，本后端负责唤醒设备、拉起接口并等待 conn_mgr 的 NET_EVENT_L4_CONNECTED。
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

    /* 启用 PM_DEVICE 且有 modem 别名时先唤醒 modem，否则为空操作（驱动在 init 时自启） */
    cellular_modem_resume();

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

    ret = net_if_down(ctx->iface);
    ctx->l4_connected = false;
    /* 启用 PM_DEVICE 且有 modem 别名时挂起 modem 以省电，否则为空操作 */
    cellular_modem_suspend();
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
 * @brief 查询蜂窝硬件是否可用
 *
 * 有 modem 别名时以设备就绪（`device_is_ready`）作为“硬件在位”的近似；无别名时无法判断
 * 真实注网/信号状态，退化为“PPP 网口是否存在”。真实注网强度（RSSI）/SIM 状态可经
 * modem_cellular 的 cellular_get_signal() 等接口进一步细化，留作未决项。
 */
static bool cellular_is_available(const connectivity_backend_ops_t* ops) {
    const connectivity_cellular_ctx_t* ctx = (const connectivity_cellular_ctx_t*) ops->ctx;

    if (ctx == NULL || ctx->iface == NULL) {
        return false;
    }
#if CONN_CELL_HAS_MODEM_DEV
    return (g_cell_modem != NULL) && device_is_ready(g_cell_modem);
#else
    return true;
#endif
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
