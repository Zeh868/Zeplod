#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: GPL-3.0
"""新业务模块脚手架生成器。

把"加一个业务模块要手改 5 处"变成一条命令：
  - 新建 <dir>/<name>.c  （最小可用骨架，Doxygen 中文注释）
  - 新建 include/zeplod/<name>.h
  - 修改 <dir>/CMakeLists.txt（追加 if/target_sources 块）
  - 修改根 Kconfig（追加 config <NAME>_ENABLE 开关）
  - 修改 include/zeplod/app_config.h（追加功能开关宏 + 优先级宏）

用法:
    python scripts/new_module.py <module_name> [--dir src/modules_examples]
                                               [--priority N]
                                               [--dry-run]
                                               [--force]

参数:
    module_name  模块名，必须 snake_case（^[a-z][a-z0-9_]*$）
    --dir        .c 文件落地目录（默认 src/modules_examples），.h 始终落 include/zeplod/
    --priority   SYS_INIT 优先级（55~98），省略则自动分配最小空位
    --dry-run    仅打印将生成/修改的内容，不写磁盘
    --force      允许覆盖已存在的 .c/.h（默认拒绝）

约束:
    - 纯标准库 Python 3.8+
    - 幂等：目标文件已含关键宏则跳过该处插入，不重复写
    - 任一锚点找不到：打印片段 + 以非 0 退出码退出
    - 不执行 git commit / push
"""

from __future__ import annotations

import argparse
import re
import sys
from datetime import date
from pathlib import Path

# ---------------------------------------------------------------------------
# 路径常量（相对脚本位置解析，不依赖 project_layout）
# ---------------------------------------------------------------------------

_SCRIPT_DIR = Path(__file__).resolve().parent
_ROOT = _SCRIPT_DIR.parent

APP_CONFIG_H = _ROOT / "include" / "zeplod" / "app_config.h"
ROOT_KCONFIG = _ROOT / "Kconfig"
INCLUDE_DIR = _ROOT / "include" / "zeplod"

PRIO_MIN = 55   # APP_INIT_PRIO_MODULE_MGR=54 之后
PRIO_MAX = 98   # APP_INIT_PRIO_APP_FINAL=99 之前

AUTHOR = "zeh (china_qzh@163.com)"
TODAY = date.today().strftime("%Y-%m-%d")

# ---------------------------------------------------------------------------
# 名称派生
# ---------------------------------------------------------------------------


def derive_names(module_name: str) -> dict:
    """从 snake_case 名称派生所有需要的变体。"""
    upper = module_name.upper()
    return {
        "name": module_name,           # foo
        "upper": upper,                # FOO
        "guard": f"{upper}_H",         # FOO_H
        "kconfig": f"CONFIG_{upper}_ENABLE",           # CONFIG_FOO_ENABLE
        "kconfig_sym": f"{upper}_ENABLE",              # FOO_ENABLE（Kconfig 符号）
        "app_enable_macro": f"APP_CONFIG_ENABLE_MODULE_{upper}",  # APP_CONFIG_ENABLE_MODULE_FOO
        "prio_macro": f"APP_INIT_PRIO_MODULE_{upper}", # APP_INIT_PRIO_MODULE_FOO
    }


# ---------------------------------------------------------------------------
# 优先级自动分配
# ---------------------------------------------------------------------------


def scan_used_priorities(app_config_text: str) -> set:
    """扫描 app_config.h 中已定义的 APP_INIT_PRIO_MODULE_* 值。"""
    pattern = re.compile(r"#define\s+APP_INIT_PRIO_MODULE_\w+\s+(\d+)")
    return {int(m.group(1)) for m in pattern.finditer(app_config_text)}


def pick_priority(app_config_text: str) -> int:
    """返回 [PRIO_MIN, PRIO_MAX] 中最小的未占用优先级。"""
    used = scan_used_priorities(app_config_text)
    for p in range(PRIO_MIN, PRIO_MAX + 1):
        if p not in used:
            return p
    raise ValueError(f"优先级区间 [{PRIO_MIN}, {PRIO_MAX}] 已全部占用，请手动指定 --priority")


# ---------------------------------------------------------------------------
# 模板生成
# ---------------------------------------------------------------------------


def gen_c_source(names: dict, priority: int) -> str:
    n = names["name"]
    u = names["upper"]
    app_enable = names["app_enable_macro"]
    prio_macro = names["prio_macro"]

    return f"""\
/**
 * @file {n}.c
 * @brief 模块 {n} 实现
 * @author {AUTHOR}
 * @version 1.0
 * @date {TODAY}
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * {TODAY}       1.0            zeh            正式发布
 *
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
#include <zeplod/app_config.h>
#include <zeplod/module_manager.h>
#include <zeplod/{n}.h>

LOG_MODULE_REGISTER({n}, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * 内部数据结构
 * ============================================================================= */

typedef struct {{
    {n}_config_t config;
    module_status_t status;
}} {n}_cb_t;

/* =============================================================================
 * 静态变量
 * ============================================================================= */

static {n}_cb_t g_{n};

/* =============================================================================
 * 模块接口实现
 * ============================================================================= */

int {n}_init(void* config) {{
    LOG_INF("Initializing {n}...");
    memset(&g_{n}, 0, sizeof(g_{n}));
    if (config != NULL) {{
        g_{n}.config = *({n}_config_t*) config;
    }}
    g_{n}.status = MODULE_STATUS_INITIALIZED;
    LOG_INF("{n} initialized");
    return 0;
}}

int {n}_start(void) {{
    if (g_{n}.status != MODULE_STATUS_INITIALIZED && g_{n}.status != MODULE_STATUS_STOPPED) {{
        return -1;
    }}
    g_{n}.status = MODULE_STATUS_RUNNING;
    LOG_INF("{n} started");
    return 0;
}}

int {n}_stop(void) {{
    if (g_{n}.status != MODULE_STATUS_RUNNING) {{
        return 0;
    }}
    g_{n}.status = MODULE_STATUS_STOPPED;
    LOG_INF("{n} stopped");
    return 0;
}}

int {n}_shutdown(void) {{
    {n}_stop();
    g_{n}.status = MODULE_STATUS_UNINITIALIZED;
    LOG_INF("{n} shutdown");
    return 0;
}}

void {n}_on_event(const event_t* event, void* user_data) {{
    if (event == NULL || user_data == NULL) {{
        return;
    }}
    switch (event->type) {{
    default:
        LOG_DBG("Unhandled event type: %d", event->type);
        break;
    }}
}}

module_status_t {n}_get_status(void) {{
    return g_{n}.status;
}}

int {n}_control(int cmd, void* arg) {{
    switch (cmd) {{
    default:
        return -1;
    }}
}}

/* =============================================================================
 * 模块接口声明
 * ============================================================================= */

const module_interface_t {n}_interface = {{
    .name       = "{n}",
    .version    = MODULE_VERSION(1, 0, 0),
    .priority   = MODULE_PRIORITY_NORMAL,
    .depends_on = NULL,
    .init       = {n}_init,
    .start      = {n}_start,
    .stop       = {n}_stop,
    .shutdown   = {n}_shutdown,
    .on_event   = {n}_on_event,
    .get_status = {n}_get_status,
    .control    = {n}_control,
}};

const module_interface_t* {n}_get_interface(void) {{
    return &{n}_interface;
}}

#if {app_enable}
static int {n}_auto_register(void) {{
    uint32_t module_id;
    {n}_config_t config = {{0}};
    if (module_manager_register({n}_get_interface(), &config, &module_id) != 0) {{
        LOG_ERR("module_manager_register {n} failed");
        return -EIO;
    }}
    LOG_INF("Registered {n} (id=%u)", module_id);
    return 0;
}}

SYS_INIT({n}_auto_register, POST_KERNEL, {prio_macro});
#endif
"""


def gen_h_header(names: dict) -> str:
    n = names["name"]
    guard = names["guard"]

    return f"""\
/**
 * @file {n}.h
 * @brief 模块 {n} 头文件
 * @author {AUTHOR}
 * @version 1.0
 * @date {TODAY}
 *
 * @par 修改日志:
 *
 *    Date         Version        Author          Description
 * {TODAY}       1.0            zeh            正式发布
 *
 */
#ifndef {guard}
#define {guard}

#include <zeplod/module_base.h>

#ifdef __cplusplus
extern "C" {{
#endif

/* =============================================================================
 * 模块配置
 * ============================================================================= */

/**
 * @brief 模块 {n} 配置结构体（按需扩展字段）
 */
typedef struct {{
    uint32_t reserved; /**< 占位保留，按需替换为实际配置字段 */
}} {n}_config_t;

/* =============================================================================
 * 模块接口（在 .c 文件中实现）
 * ============================================================================= */

/** @brief 模块初始化 */
int {n}_init(void* config);

/** @brief 模块启动 */
int {n}_start(void);

/** @brief 模块停止 */
int {n}_stop(void);

/** @brief 模块关闭 */
int {n}_shutdown(void);

/** @brief 事件处理器 */
void {n}_on_event(const event_t* event, void* user_data);

/** @brief 获取模块状态 */
module_status_t {n}_get_status(void);

/** @brief 模块控制 */
int {n}_control(int cmd, void* arg);

/** @brief 获取模块接口指针 */
const module_interface_t* {n}_get_interface(void);

#ifdef __cplusplus
}}
#endif

#endif /* {guard} */
"""


# ---------------------------------------------------------------------------
# 文件修改辅助
# ---------------------------------------------------------------------------


def _indent_of_line(line: str) -> str:
    """返回行的前导空白。"""
    return line[: len(line) - len(line.lstrip())]


def insert_cmake_block(cmake_text: str, names: dict) -> tuple[str, bool]:
    """
    在 CMakeLists.txt 末尾的最后一个 endif() 之后插入新模块的 if 块。
    若已含 CONFIG_<UPPER>_ENABLE 则跳过（幂等）。
    返回 (new_text, inserted)。
    """
    kconfig_sym = names["kconfig_sym"]
    n = names["name"]
    snippet = f"\nif(CONFIG_{kconfig_sym})\n    target_sources(app PRIVATE {n}.c)\nendif()\n"

    if f"CONFIG_{kconfig_sym}" in cmake_text:
        return cmake_text, False  # 已存在，跳过

    # 追加到文件末尾
    new_text = cmake_text.rstrip("\n") + "\n" + snippet
    return new_text, True


def insert_kconfig_entry(kconfig_text: str, names: dict) -> tuple[str, bool]:
    """
    在 Kconfig 的 MODULE_MANAGER 或 EXAMPLE_MODULE_*_ENABLE 区块附近插入新开关。
    若已含 kconfig_sym 则跳过（幂等）。
    返回 (new_text, inserted)。
    """
    kconfig_sym = names["kconfig_sym"]
    n = names["name"]
    upper = names["upper"]

    if kconfig_sym in kconfig_text:
        return kconfig_text, False

    snippet = (
        f"\nconfig {kconfig_sym}\n"
        f"\tbool \"Enable {n} module\"\n"
        f"\tdepends on MODULE_MANAGER\n"
        f"\tdefault n\n"
        f"\thelp\n"
        f"\t  Enable the {n} module.\n"
    )

    # 锚点策略：找 EXAMPLE_MODULE_B_ENABLE 的 endmenu 之前插入（Module Manager 菜单末尾）
    # 具体找 "config EXAMPLE_MODULE_B_ENABLE" 之后的 endmenu，在 endmenu 之前插入
    anchor_pattern = re.compile(
        r"(config EXAMPLE_MODULE_B_ENABLE\b.*?)(^\s*endmenu)", re.DOTALL | re.MULTILINE
    )
    m = anchor_pattern.search(kconfig_text)
    if m:
        insert_pos = m.start(2)
        new_text = kconfig_text[:insert_pos] + snippet + "\n" + kconfig_text[insert_pos:]
        return new_text, True

    # 备选锚点：任意 *_ENABLE bool 开关后面的 endmenu
    alt = re.search(r"(config \w+_ENABLE\b.*?)(^\s*endmenu)", kconfig_text, re.DOTALL | re.MULTILINE)
    if alt:
        insert_pos = alt.start(2)
        new_text = kconfig_text[:insert_pos] + snippet + "\n" + kconfig_text[insert_pos:]
        return new_text, True

    return kconfig_text, None  # None 表示找不到锚点


def insert_app_config_enable(app_config_text: str, names: dict) -> tuple[str, bool]:
    """
    在 app_config.h 的示例模块开关区块末尾插入 #ifdef CONFIG_FOO_ENABLE ... 块。
    若已含 app_enable_macro 则跳过（幂等）。
    返回 (new_text, inserted)。
    """
    app_enable = names["app_enable_macro"]
    kconfig_sym = names["kconfig_sym"]
    n_upper = names["upper"]

    if app_enable in app_config_text:
        return app_config_text, False

    snippet = (
        f"\n#ifdef CONFIG_{kconfig_sym}\n"
        f"#define {app_enable} 1\n"
        f"#else\n"
        f"#define {app_enable} 0\n"
        f"#endif\n"
    )

    # 锚点：找 APP_CONFIG_ENABLE_MODULE_B 之后的空行，插在其后
    anchor = "APP_CONFIG_ENABLE_MODULE_B"
    if anchor in app_config_text:
        # 找到 #endif 那行（APP_CONFIG_ENABLE_MODULE_B 块的结束）
        pattern = re.compile(
            r"(#ifdef CONFIG_EXAMPLE_MODULE_B_ENABLE.*?#endif\n)", re.DOTALL
        )
        m = pattern.search(app_config_text)
        if m:
            insert_pos = m.end()
            new_text = app_config_text[:insert_pos] + snippet + app_config_text[insert_pos:]
            return new_text, True

    # 备选：找最后一个 APP_CONFIG_ENABLE_MODULE_* 的 #endif\n 之后
    pattern2 = re.compile(r"(#define APP_CONFIG_ENABLE_MODULE_\w+ [01]\n#endif\n)")
    matches = list(pattern2.finditer(app_config_text))
    if matches:
        last = matches[-1]
        insert_pos = last.end()
        new_text = app_config_text[:insert_pos] + snippet + app_config_text[insert_pos:]
        return new_text, True

    return app_config_text, None  # None = 锚点未找到


def insert_app_config_prio(app_config_text: str, names: dict, priority: int) -> tuple[str, bool]:
    """
    在 APP_INIT_PRIO_APP_FINAL 定义行之前插入优先级宏。
    若已含 prio_macro 则跳过（幂等）。
    返回 (new_text, inserted)。
    """
    prio_macro = names["prio_macro"]

    if prio_macro in app_config_text:
        return app_config_text, False

    # 计算对齐宽度（参照现有最长宏名）
    prio_pattern = re.compile(r"(#define APP_INIT_PRIO_\w+)\s+(\d+)")
    all_macros = prio_pattern.findall(app_config_text)
    max_len = max((len(m) for m, _ in all_macros), default=40) if all_macros else 40
    # 对齐到 max_len，最少留 2 空格
    col_width = max(max_len + 2, len(f"#define {prio_macro}") + 2)
    define_line = f"#define {prio_macro}"
    padding = " " * (col_width - len(define_line))
    snippet = f"{define_line}{padding}{priority}\n"

    # 锚点：APP_INIT_PRIO_APP_FINAL
    anchor = "#define APP_INIT_PRIO_APP_FINAL"
    idx = app_config_text.find(anchor)
    if idx == -1:
        return app_config_text, None  # 锚点未找到

    new_text = app_config_text[:idx] + snippet + app_config_text[idx:]
    return new_text, True


# ---------------------------------------------------------------------------
# 主逻辑
# ---------------------------------------------------------------------------


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Zeplod 新业务模块脚手架生成器——把手改 5 处变成一条命令。",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "示例:\n"
            "  python scripts/new_module.py my_sensor\n"
            "  python scripts/new_module.py my_sensor --dir src/modules_examples --priority 70\n"
            "  python scripts/new_module.py my_sensor --dry-run\n"
        ),
    )
    parser.add_argument(
        "module_name",
        help="模块名（snake_case，如 my_sensor）",
    )
    parser.add_argument(
        "--dir",
        default="src/modules_examples",
        help=".c 文件落地目录（默认 src/modules_examples）",
    )
    parser.add_argument(
        "--priority",
        type=int,
        default=None,
        help=f"SYS_INIT 优先级数字（{PRIO_MIN}~{PRIO_MAX}），省略则自动分配",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="只打印将创建/修改的内容，不落盘",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="允许覆盖已存在的 .c/.h（默认拒绝）",
    )
    return parser.parse_args()


def validate_module_name(name: str) -> None:
    if not re.match(r"^[a-z][a-z0-9_]*$", name):
        print(
            f"错误：模块名 {name!r} 不符合 snake_case 规范。\n"
            "  要求：小写字母开头，只含小写字母、数字、下划线。\n"
            "  例如：my_sensor、foo2、data_bridge_v2",
            file=sys.stderr,
        )
        sys.exit(1)


def _print_dry(label: str, content: str) -> None:
    sep = "=" * 72
    print(f"\n{sep}")
    print(f"[DRY-RUN] {label}")
    print(sep)
    print(content)


def _write_or_dry(path: Path, content: str, dry_run: bool, label: str) -> None:
    if dry_run:
        _print_dry(f"创建文件: {path}", content)
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        print(f"  [创建] {path}")


def _patch_or_dry(
    path: Path,
    old_text: str,
    new_text: str,
    snippet: str,
    dry_run: bool,
    label: str,
    inserted: object,
) -> bool:
    """
    处理文件修改的打印/写入逻辑。
    inserted: True=将插入, False=已存在跳过, None=锚点未找到(错误)
    返回 True 表示成功，False 表示错误（锚点未找到）。
    """
    if inserted is False:
        print(f"  [跳过] {path.name}：已含目标标识，无需重复插入")
        return True
    if inserted is None:
        print(
            f"\n[警告] 无法自动插入到 {path}，请手动添加以下片段：\n"
            f"{'-'*60}\n{snippet}\n{'-'*60}",
            file=sys.stderr,
        )
        return False
    # inserted is True
    if dry_run:
        _print_dry(f"修改文件: {path} (将插入以下片段)", snippet)
    else:
        path.write_text(new_text, encoding="utf-8")
        print(f"  [修改] {path}")
    return True


def main() -> int:
    args = parse_args()

    validate_module_name(args.module_name)

    names = derive_names(args.module_name)
    module_dir = _ROOT / args.dir
    c_file = module_dir / f"{names['name']}.c"
    h_file = INCLUDE_DIR / f"{names['name']}.h"
    cmake_file = module_dir / "CMakeLists.txt"

    # ------------------------------------------------------------------ 优先级
    if args.priority is not None:
        if not (PRIO_MIN <= args.priority <= PRIO_MAX):
            print(
                f"错误：--priority {args.priority} 超出允许范围 [{PRIO_MIN}, {PRIO_MAX}]",
                file=sys.stderr,
            )
            return 1
        priority = args.priority
    else:
        if not APP_CONFIG_H.exists():
            print(f"错误：找不到 {APP_CONFIG_H}，请在项目根目录运行脚本", file=sys.stderr)
            return 1
        app_config_text_for_scan = APP_CONFIG_H.read_text(encoding="utf-8")
        try:
            priority = pick_priority(app_config_text_for_scan)
        except ValueError as e:
            print(f"错误：{e}", file=sys.stderr)
            return 1

    # ------------------------------------------------------------------ 检查目标文件
    errors = []
    if c_file.exists() and not args.force:
        errors.append(f"  .c 文件已存在：{c_file}（用 --force 强制覆盖）")
    if h_file.exists() and not args.force:
        errors.append(f"  .h 文件已存在：{h_file}（用 --force 强制覆盖）")
    if not cmake_file.exists():
        errors.append(f"  CMakeLists.txt 不存在：{cmake_file}")
    if not APP_CONFIG_H.exists():
        errors.append(f"  app_config.h 不存在：{APP_CONFIG_H}")
    if not ROOT_KCONFIG.exists():
        errors.append(f"  根 Kconfig 不存在：{ROOT_KCONFIG}")
    if errors:
        for e in errors:
            print(e, file=sys.stderr)
        return 1

    print(f"\n模块名: {names['name']}  |  上层目录: {module_dir}  |  优先级: {priority}")
    print(f"Kconfig 符号: {names['kconfig_sym']}  |  优先级宏: {names['prio_macro']}\n")

    # ------------------------------------------------------------------ 生成内容
    c_content = gen_c_source(names, priority)
    h_content = gen_h_header(names)

    cmake_text = cmake_file.read_text(encoding="utf-8")
    kconfig_text = ROOT_KCONFIG.read_text(encoding="utf-8")
    app_config_text = APP_CONFIG_H.read_text(encoding="utf-8")

    cmake_new, cmake_inserted = insert_cmake_block(cmake_text, names)
    kconfig_new, kconfig_inserted = insert_kconfig_entry(kconfig_text, names)
    app_cfg_enable_new, app_cfg_enable_inserted = insert_app_config_enable(app_config_text, names)

    # 优先级插入基于 enable 插入后的文本（enable 先改，prio 再改同一文本）
    interim_text = app_cfg_enable_new if app_cfg_enable_inserted is True else app_config_text
    app_cfg_prio_new, app_cfg_prio_inserted = insert_app_config_prio(interim_text, names, priority)

    # ------------------------------------------------------------------ 执行
    ok = True

    # 1. .c
    _write_or_dry(c_file, c_content, args.dry_run, "新建 .c")

    # 2. .h
    _write_or_dry(h_file, h_content, args.dry_run, "新建 .h")

    # 3. CMakeLists.txt
    cmake_snippet = f"if(CONFIG_{names['kconfig_sym']})\n    target_sources(app PRIVATE {names['name']}.c)\nendif()"
    ok = _patch_or_dry(cmake_file, cmake_text, cmake_new, cmake_snippet, args.dry_run,
                       "CMakeLists.txt", cmake_inserted) and ok

    # 4. Kconfig
    kconfig_snippet = (
        f"config {names['kconfig_sym']}\n"
        f"\tbool \"Enable {names['name']} module\"\n"
        f"\tdepends on MODULE_MANAGER\n"
        f"\tdefault n\n"
        f"\thelp\n"
        f"\t  Enable the {names['name']} module."
    )
    ok = _patch_or_dry(ROOT_KCONFIG, kconfig_text, kconfig_new, kconfig_snippet, args.dry_run,
                       "Kconfig", kconfig_inserted) and ok

    # 5a. app_config.h — enable 宏
    enable_snippet = (
        f"#ifdef CONFIG_{names['kconfig_sym']}\n"
        f"#define {names['app_enable_macro']} 1\n"
        f"#else\n"
        f"#define {names['app_enable_macro']} 0\n"
        f"#endif"
    )
    ok = _patch_or_dry(APP_CONFIG_H, app_config_text, app_cfg_enable_new, enable_snippet,
                       args.dry_run, "app_config.h (enable 宏)", app_cfg_enable_inserted) and ok

    # 5b. app_config.h — 优先级宏（写入 prio 修改结果，基于已含 enable 的中间文本）
    prio_snippet = f"#define {names['prio_macro']} {priority}"
    if not args.dry_run and app_cfg_enable_inserted is True:
        # enable 已写入，prio 要基于磁盘上最新内容再次插入
        current_text = APP_CONFIG_H.read_text(encoding="utf-8")
        app_cfg_prio_new2, app_cfg_prio_inserted2 = insert_app_config_prio(current_text, names, priority)
        ok = _patch_or_dry(APP_CONFIG_H, current_text, app_cfg_prio_new2, prio_snippet,
                           args.dry_run, "app_config.h (优先级宏)", app_cfg_prio_inserted2) and ok
    else:
        ok = _patch_or_dry(APP_CONFIG_H, interim_text, app_cfg_prio_new, prio_snippet,
                           args.dry_run, "app_config.h (优先级宏)", app_cfg_prio_inserted) and ok

    # ------------------------------------------------------------------ 总结
    print("\n" + "=" * 72)
    if args.dry_run:
        print("[DRY-RUN 完成] 以上为预览，未写入任何文件。")
    elif ok:
        print("生成完成！")
        print(f"  新建: {c_file}")
        print(f"  新建: {h_file}")
        print(f"  修改: {cmake_file}")
        print(f"  修改: {ROOT_KCONFIG}")
        print(f"  修改: {APP_CONFIG_H}")
        print(f"  选定优先级: {priority}（宏 {names['prio_macro']}）")
        print()
        print("后续建议：")
        print(f"  1. 在 tests/ 添加 test_{names['name']}.c 编写单元测试")
        print(f"  2. 在目标 conf/ 打开 CONFIG_{names['kconfig_sym']}=y 启用模块")
    else:
        print("[错误] 部分步骤失败，请按上方提示手动补充。")
        return 1
    print("=" * 72)

    return 0


if __name__ == "__main__":
    sys.exit(main())
