/**
 * @file factory_mode_shell.c
 * @brief 工厂产测 Shell 命令
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-06-13
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-06-13       1.0            zeh            Phase 4 初始版本
 * 2026-09-18       1.1            zeh            补文件修改日志
 *
 */

#include <zeplod/factory_mode.h>

#include <errno.h>
#include <string.h>

#include <zephyr/shell/shell.h>

#if defined(CONFIG_SHELL) && IS_ENABLED(CONFIG_FACTORY_MODE_SHELL)
static const char* factory_state_name(factory_state_t st) {
    switch (st) {
    case FACTORY_STATE_INACTIVE:
        return "inactive";
    case FACTORY_STATE_ACTIVE:
        return "active";
    case FACTORY_STATE_PASSED:
        return "passed";
    case FACTORY_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static int cmd_factory_enter(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    int ret = factory_mode_enter();
    if (ret != 0) {
        shell_error(shell, "enter failed: %d", ret);
        return ret;
    }
    shell_print(shell, "factory mode active");
    return 0;
}

static int cmd_factory_exit(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    int ret = factory_mode_exit();
    if (ret != 0) {
        shell_error(shell, "exit failed: %d", ret);
        return ret;
    }
    shell_print(shell, "factory mode inactive");
    return 0;
}

static int cmd_factory_status(const struct shell* shell, size_t argc, char** argv) {
    factory_status_t st;

    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    if (factory_mode_get_state(&st) != 0) {
        shell_error(shell, "state unavailable");
        return -EIO;
    }
    shell_print(shell, "state=%s gpio_passed=%s", factory_state_name(st.state), st.gpio_passed ? "yes" : "no");
    return 0;
}

static int cmd_factory_gpio_loopback(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    int ret = factory_mode_run_gpio_loopback();
    if (ret != 0) {
        shell_error(shell, "gpio loopback failed: %d", ret);
        return ret;
    }
    shell_print(shell, "gpio loopback ok");
    return 0;
}

static int cmd_factory_cal_set(const struct shell* shell, size_t argc, char** argv) {
    if (argc < 3) {
        shell_print(shell, "Usage: factory cal set <key> <value>");
        return -EINVAL;
    }

    int ret = factory_mode_set_calibration(argv[1], argv[2]);
    if (ret != 0) {
        shell_error(shell, "cal set failed: %d", ret);
        return ret;
    }
    shell_print(shell, "cal stored");
    return 0;
}

static int cmd_factory_cal_get(const struct shell* shell, size_t argc, char** argv) {
    char buf[CONFIG_FACTORY_MODE_CAL_VALUE_MAX_LEN];

    if (argc < 2) {
        shell_print(shell, "Usage: factory cal get <key>");
        return -EINVAL;
    }

    int ret = factory_mode_get_calibration(argv[1], buf, sizeof(buf));
    if (ret != 0) {
        shell_error(shell, "cal get failed: %d", ret);
        return ret;
    }
    shell_print(shell, "%s=%s", argv[1], buf);
    return 0;
}

static int cmd_factory_finalize(const struct shell* shell, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    int ret = factory_mode_finalize_pass();
    if (ret != 0) {
        shell_error(shell, "finalize failed: %d", ret);
        return ret;
    }
    shell_print(shell, "factory pass committed");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_factory_cal,
                               SHELL_CMD(set, NULL, "Set calibration <key> <value>", cmd_factory_cal_set),
                               SHELL_CMD(get, NULL, "Get calibration <key>", cmd_factory_cal_get),
                               SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_factory, SHELL_CMD(enter, NULL, "Enter factory test mode", cmd_factory_enter),
                               SHELL_CMD(exit, NULL, "Exit factory test mode", cmd_factory_exit),
                               SHELL_CMD(status, NULL, "Show factory state", cmd_factory_status),
                               SHELL_CMD(gpio, NULL, "Run GPIO loopback test", cmd_factory_gpio_loopback),
                               SHELL_CMD(cal, &sub_factory_cal, "Calibration data", NULL),
                               SHELL_CMD(finalize, NULL, "Commit cal to app_kv and mark passed", cmd_factory_finalize),
                               SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(factory, &sub_factory, "Factory test commands", NULL);
#endif /* CONFIG_SHELL && CONFIG_FACTORY_MODE_SHELL */
