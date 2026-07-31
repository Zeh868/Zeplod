/**
 * @file provisioning_shell.c
 * @brief 配网 Shell 命令
 *
 * 提供 `prov set-wifi <ssid> <psk>` 用于手工注入 Wi-Fi 凭据（经 provisioning_module_begin()
 * 落盘并推进配网状态机），以及 `prov show` 用于查看当前配网状态与已保存 SSID（不回显 PSK）。
 *
 * @author zeh (china_qzh@163.com)
 * @version 1.0
 * @date 2026-07-09
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-07-09       1.0            zeh            ESP32-C6 Wi-Fi 集成：新增配网 Shell 命令
 *
 */

#include <zeplod/provisioning_module.h>

#include <errno.h>

#include <zephyr/shell/shell.h>

#if defined(CONFIG_SHELL) && IS_ENABLED(CONFIG_PROVISIONING_SHELL)

/** `prov set-wifi <ssid> <psk>`：注入凭据并驱动配网状态机进入 PROVISIONED */
static int cmd_prov_set_wifi(const struct shell* shell, size_t argc, char** argv) {
    provisioning_credentials_t creds;
    int                        ret;

    if (argc != 3) {
        shell_print(shell, "Usage: prov set-wifi <ssid> <psk>");
        return -EINVAL;
    }

    creds.ssid = argv[1];
    creds.psk = argv[2];

    ret = provisioning_module_begin(&creds);
    if (ret != 0) {
        shell_error(shell, "set-wifi failed: %d", ret);
        return -EIO;
    }

    shell_print(shell, "Wi-Fi credentials saved (ssid=%s)", creds.ssid);
    return 0;
}

/** `prov show`：显示配网状态与已保存 SSID（PSK 出于安全考虑不回显） */
static int cmd_prov_show(const struct shell* shell, size_t argc, char** argv) {
    provisioning_state_t state;
    char                  ssid[PROVISIONING_WIFI_SSID_MAX_LEN];
    char                  psk[PROVISIONING_WIFI_PSK_MAX_LEN];
    static const char* const state_names[] = {
        [PROVISIONING_STATE_UNPROVISIONED] = "UNPROVISIONED",
        [PROVISIONING_STATE_IN_PROGRESS] = "IN_PROGRESS",
        [PROVISIONING_STATE_PROVISIONED] = "PROVISIONED",
        [PROVISIONING_STATE_ERROR] = "ERROR",
    };

    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    if (provisioning_module_get_state(&state) != 0) {
        shell_error(shell, "get_state failed");
        return -EIO;
    }
    shell_print(shell, "state: %s",
               (state < ARRAY_SIZE(state_names) && state_names[state] != NULL) ? state_names[state] : "UNKNOWN");

    if (provisioning_module_get_credentials(ssid, sizeof(ssid), psk, sizeof(psk)) == 0) {
        shell_print(shell, "wifi.ssid: %s", ssid);
        shell_print(shell, "wifi.psk: <hidden>");
    } else {
        shell_print(shell, "wifi credentials: not configured");
    }

    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_prov,
                               SHELL_CMD_ARG(set-wifi, NULL, "Set Wi-Fi SSID/PSK and provision", cmd_prov_set_wifi, 3,
                                             0),
                               SHELL_CMD(show, NULL, "Show provisioning state and saved SSID", cmd_prov_show),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(prov, &sub_prov, "Provisioning commands", NULL);
#endif /* CONFIG_SHELL && CONFIG_PROVISIONING_SHELL */
