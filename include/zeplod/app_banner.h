/**
 * @file app_banner.h
 * @brief 启动 Banner ASCII 艺术图案
 *
 * @note 每行建议不超过 80 列，避免串口终端换行错位。
 * @note 使用 \r\n 结尾以确保各种终端正确换行。
 * @author zeh (china_qzh@163.com)
 * @version 1.1
 * @date 2026-04-01
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * 2026-04-01       1.0            zeh            初始版本
 * 2026-09-18       1.1            zeh            补全文件头模板（作者/版本/日期/修改日志）
 *
 */

#ifndef APP_BANNER_H
#define APP_BANNER_H

#define APP_BANNER_LOGO                                                                                                \
    "\r\n"                                                                                                             \
    "                     .__             .___\r\n"                                                                    \
    "________ ____ ______ |  |   ____   __| _/\r\n"                                                                    \
    "\\___   // __ \\____ \\ |  |  /  _ \\ / __ |\r\n"                                                                 \
    " __/  /\\  ___/|  |_> >  |_(  <_> ) /_/ |\r\n"                                                                    \
    "/_____  \\___  >   __/|____/\\____/\\____ |\r\n"                                                                  \
    "      \\/    \\/|__|                    \\/\r\n"

#define APP_BANNER_INFO                                                                                                \
    "  %-14s %s\r\n"                                                                                                   \
    "  %-14s %s\r\n"                                                                                                   \
    "  %-14s %s / %s\r\n"                                                                                              \
    "  %-14s %s\r\n"                                                                                                   \
    "  %-14s 0x%06X\r\n"                                                                                               \
    "  %-14s %s\r\n"
#endif /* APP_BANNER_H */
