/**
 * @file remote_ops_shell.c
 * @brief 远程运维 Shell 命令
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 5 初始版本
 * 2026-09-18       1.1            zeh            补文件修改日志
 *
 */

#include <zeplod/remote_ops.h>

#include <errno.h>

#include <zephyr/shell/shell.h>

#if defined(CONFIG_SHELL) && IS_ENABLED(CONFIG_REMOTE_OPS_SHELL)
static int cmd_remote_export(const struct shell* shell, size_t argc, char** argv) {
    char buf[CONFIG_REMOTE_OPS_EXPORT_BUF_SIZE];

    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    if (remote_ops_export_diag() != 0) {
        shell_error(shell, "export failed");
        return -EIO;
    }
    if (remote_ops_get_last_export(buf, sizeof(buf)) != 0) {
        shell_error(shell, "readback failed");
        return -EIO;
    }
    shell_print(shell, "%s", buf);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_remote,
                               SHELL_CMD(export, NULL, "Export sys_diag JSON via backend", cmd_remote_export),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(remote, &sub_remote, "Remote operations hooks", NULL);
#endif
