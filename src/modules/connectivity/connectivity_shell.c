/**
 * @file connectivity_shell.c
 * @brief 连接管理 Shell 命令
 *
 * 提供 `conn up` 用于手动触发一次按优先级择优连接（connectivity_module_connect_auto()），
 * 以及 `conn status` 用于查看当前连接状态、链路类型与错误码；作为开机自连/配网即连
 * 之外的手动兜底入口（例如凭据更新后不想等下一次周期轮询，或需要在现场快速确认链路）。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-07-09
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-07-09       1.0            zeh            打通首连触发链路：新增连接管理 Shell 命令
 *
 */

#include <zeplod/connectivity_module.h>

#include <errno.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_SHELL) && IS_ENABLED(CONFIG_CONNECTIVITY_SHELL)

/** 链路类型可读名称，下标须与 connectivity_link_type_t 取值一致 */
static const char* const conn_link_type_names[] = {
    [CONNECTIVITY_LINK_NONE] = "none",
    [CONNECTIVITY_LINK_WIFI] = "wifi",
    [CONNECTIVITY_LINK_ETHERNET] = "ethernet",
    [CONNECTIVITY_LINK_BLE] = "ble",
    [CONNECTIVITY_LINK_CELLULAR] = "cellular",
};

/** 连接状态可读名称，下标须与 connectivity_state_t 取值一致 */
static const char* const conn_state_names[] = {
    [CONNECTIVITY_STATE_DOWN] = "DOWN",
    [CONNECTIVITY_STATE_CONNECTING] = "CONNECTING",
    [CONNECTIVITY_STATE_UP] = "UP",
    [CONNECTIVITY_STATE_ERROR] = "ERROR",
};

/**
 * @brief `conn up`：手动触发一次按优先级择优连接
 * @note connectivity_module_connect_auto() 在调用方线程内同步执行、直至某后端 connect()
 * 返回或全部候选失败，可能阻塞数秒（受各后端 connect 超时上限约束）；shell 命令上下文
 * 允许这种阻塞，与事件回调必须异步投递的约束不同。
 */
static int cmd_conn_up(const struct shell* shell, size_t argc, char** argv) {
    int ret;

    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    ret = connectivity_module_connect_auto();
    if (ret != 0) {
        shell_error(shell, "connect_auto failed: %d", ret);
        return -EIO;
    }

    shell_print(shell, "connect_auto succeeded");
    return 0;
}

/** `conn status`：显示当前连接状态、链路类型与错误码 */
static int cmd_conn_status(const struct shell* shell, size_t argc, char** argv) {
    connectivity_status_t st;

    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    if (connectivity_module_get_state(&st) != 0) {
        shell_error(shell, "get_state failed");
        return -EIO;
    }

    shell_print(shell, "state: %s",
               ((size_t) st.state < ARRAY_SIZE(conn_state_names) && conn_state_names[st.state] != NULL)
                   ? conn_state_names[st.state]
                   : "UNKNOWN");
    shell_print(shell, "link_type: %s",
               ((size_t) st.link_type < ARRAY_SIZE(conn_link_type_names) && conn_link_type_names[st.link_type] != NULL)
                   ? conn_link_type_names[st.link_type]
                   : "UNKNOWN");
    shell_print(shell, "error_code: %d", st.error_code);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_conn,
                               SHELL_CMD(up, NULL, "Trigger a priority-ordered connect attempt (blocking)",
                                        cmd_conn_up),
                               SHELL_CMD(status, NULL, "Show connectivity state / link type / error code",
                                        cmd_conn_status),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(conn, &sub_conn, "Connectivity commands", NULL);
#endif /* CONFIG_SHELL && CONFIG_CONNECTIVITY_SHELL */
