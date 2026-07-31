#!/usr/bin/env python3
"""静态分析脚本：扫描业务模块 .c 文件中的 depends_on 声明，检测循环依赖与悬空依赖。

无需编译、无需 Zephyr 工具链，纯标准库运行。适合开发期与 CI 阶段。

用法示例::

    python scripts/check_module_deps.py
    python scripts/check_module_deps.py --root src/modules_examples
    python scripts/check_module_deps.py --verbose
    python scripts/check_module_deps.py --strict

退出码：
    0  无问题
    1  仅有悬空依赖或解析警告（--strict 时视为失败）
    2  存在循环依赖（始终视为 CI 硬错误）
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple


# ---------------------------------------------------------------------------
# 正则：依赖数组
# ---------------------------------------------------------------------------

# 匹配：static const char* const <ident>[] = { ... };
# 允许跨行、任意空白
_RE_DEPS_ARRAY = re.compile(
    r'static\s+const\s+char\s*\*\s*const\s+(\w+)\s*\[\s*\]\s*=\s*\{([^}]*)\}\s*;',
    re.DOTALL,
)

# 匹配数组元素中的字符串字面量（排除 NULL 关键字）
_RE_STRING_LITERAL = re.compile(r'"([^"]*)"')

# ---------------------------------------------------------------------------
# 正则：module_interface_t 初始化块
# ---------------------------------------------------------------------------

# 模块接口起始行：[static] const module_interface_t <ident> =
_RE_IFACE_START = re.compile(
    r'\bconst\s+module_interface_t\s+(\w+)\s*='
)

# 从已截取的初始化块中提取 .name = "..."
_RE_NAME_FIELD = re.compile(r'\.name\s*=\s*"([^"]*)"')

# 从已截取的初始化块中提取 .depends_on = <value>
_RE_DEPENDS_FIELD = re.compile(r'\.depends_on\s*=\s*(\w+|\([\w\s]*\))')

# ---------------------------------------------------------------------------
# 正则：宏调用
# ---------------------------------------------------------------------------

# 带依赖数组的宏族（第 2 参数为 deps_array 标识符）：
#   DECLARE_MODULE_INTERFACE_WITH_DEPS(mod, arr)
#   DECLARE_MODULE_INTERFACE_WITH_DEPS_VERSION_MIN(mod, arr, ver)
#   DECLARE_MODULE_INTERFACE_MINIMAL_WITH_DEPS(mod, arr)
#   DECLARE_MODULE_INTERFACE_MINIMAL_WITH_DEPS_VERSION_MIN(mod, arr, ver)
_RE_MACRO_DECL = re.compile(
    r'\bDECLARE_MODULE_INTERFACE(?:_MINIMAL)?_WITH_DEPS(?:_VERSION_MIN)?\s*\(\s*(\w+)\s*,\s*(\w+)'
)

# 无依赖宏族（.depends_on = NULL）：
#   DECLARE_MODULE_INTERFACE(mod_name)
#   DECLARE_MODULE_INTERFACE_MINIMAL(mod_name)
_RE_MACRO_DECL_NODEP = re.compile(
    r'\bDECLARE_MODULE_INTERFACE(?:_MINIMAL)?\s*\(\s*(\w+)\s*\)'
)


# ---------------------------------------------------------------------------
# 花括号配平：截取初始化块
# ---------------------------------------------------------------------------

def _extract_brace_block(text: str, start: int) -> Optional[str]:
    """从 start 位置开始，找到第一个 '{' 并做配平，返回含首尾花括号的子串。

    若未找到起始 '{' 或花括号不平衡则返回 None。
    """
    idx = text.find('{', start)
    if idx == -1:
        return None

    depth = 0
    in_string = False
    escape_next = False

    for i in range(idx, len(text)):
        ch = text[i]

        if escape_next:
            escape_next = False
            continue

        if ch == '\\' and in_string:
            escape_next = True
            continue

        if ch == '"':
            in_string = not in_string
            continue

        if in_string:
            continue

        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return text[idx:i + 1]

    return None  # 配平失败


# ---------------------------------------------------------------------------
# 解析单个 .c 文件
# ---------------------------------------------------------------------------

def parse_c_file(path: Path) -> Tuple[
    Dict[str, Optional[str]],   # name -> depends_on_ident (None 表示 NULL)
    Dict[str, List[str]],       # array_ident -> [dep_name, ...]
]:
    """解析 C 文件，返回 (模块名->依赖标识符表, 依赖数组表)。

    模块名来自 `.name = "..."` 字段或宏参数字符串化。
    依赖标识符 None 代表 .depends_on = NULL。
    """
    text = path.read_text(encoding='utf-8', errors='replace')

    # ---- 1. 提取所有 static const char* const <ident>[] = {...} 数组 ----
    arrays: Dict[str, List[str]] = {}
    for m in _RE_DEPS_ARRAY.finditer(text):
        ident = m.group(1)
        body = m.group(2)
        names = _RE_STRING_LITERAL.findall(body)
        arrays[ident] = names  # NULL 已被 findall 自然排除

    # ---- 2. 解析宏调用形式 ----
    modules: Dict[str, Optional[str]] = {}

    for m in _RE_MACRO_DECL.finditer(text):
        mod_name = m.group(1)   # 宏参数即模块名（宏内用 #mod_name 字符串化）
        deps_ident = m.group(2)
        modules[mod_name] = deps_ident

    for m in _RE_MACRO_DECL_NODEP.finditer(text):
        mod_name = m.group(1)
        # 只有当该名字未被带依赖宏覆盖时才插入
        if mod_name not in modules:
            modules[mod_name] = None

    # ---- 3. 解析显式 module_interface_t ... = { ... } 初始化块 ----
    for m in _RE_IFACE_START.finditer(text):
        iface_ident = m.group(1)
        block = _extract_brace_block(text, m.end())
        if block is None:
            continue

        name_m = _RE_NAME_FIELD.search(block)
        if name_m is None:
            continue
        mod_name = name_m.group(1)

        dep_m = _RE_DEPENDS_FIELD.search(block)
        if dep_m is None:
            dep_ident: Optional[str] = None
        else:
            raw = dep_m.group(1).strip().strip('()')
            dep_ident = None if raw == 'NULL' else raw

        # 宏已处理过的跳过（避免覆盖）
        if mod_name not in modules:
            modules[mod_name] = dep_ident

    return modules, arrays


# ---------------------------------------------------------------------------
# 扫描目录
# ---------------------------------------------------------------------------

def scan_root(root: Path) -> Tuple[
    Dict[str, str],         # mod_name -> source_file (用于报告)
    Dict[str, List[str]],   # mod_name -> [dep_name, ...]
    List[str],              # 警告列表
]:
    """递归扫描 root 下所有 .c 文件，构建模块依赖图。"""
    all_modules: Dict[str, Optional[str]] = {}   # mod_name -> deps_ident / None
    all_arrays: Dict[str, List[str]] = {}        # array_ident -> [dep_name, ...]
    mod_source: Dict[str, str] = {}              # mod_name -> file path string
    warnings: List[str] = []

    for c_file in sorted(root.rglob('*.c')):
        modules, arrays = parse_c_file(c_file)

        for mod_name, dep_ident in modules.items():
            if mod_name in all_modules:
                warnings.append(
                    f"重复模块名 '{mod_name}'：已在 {mod_source[mod_name]}，"
                    f"再次出现于 {c_file}"
                )
            all_modules[mod_name] = dep_ident
            mod_source[mod_name] = str(c_file)

        all_arrays.update(arrays)

    # 展开依赖标识符 -> 依赖名列表
    graph: Dict[str, List[str]] = {}
    for mod_name, dep_ident in all_modules.items():
        if dep_ident is None:
            graph[mod_name] = []
        elif dep_ident in all_arrays:
            graph[mod_name] = list(all_arrays[dep_ident])
        else:
            warnings.append(
                f"模块 '{mod_name}'（{mod_source[mod_name]}）的 .depends_on 指向标识符"
                f" '{dep_ident}'，但未找到对应数组定义 —— 需人工确认"
            )
            graph[mod_name] = []

    return mod_source, graph, warnings


# ---------------------------------------------------------------------------
# 图检测
# ---------------------------------------------------------------------------

def find_cycles(graph: Dict[str, List[str]]) -> List[List[str]]:
    """用 DFS 着色法检测有向图中所有环，返回环路径列表（每条路径首尾相同）。"""
    WHITE, GRAY, BLACK = 0, 1, 2
    color: Dict[str, int] = {n: WHITE for n in graph}
    cycles: List[List[str]] = []
    stack: List[str] = []
    seen_cycles: Set[frozenset] = set()

    def dfs(node: str) -> None:
        color[node] = GRAY
        stack.append(node)

        for neighbor in graph.get(node, []):
            if neighbor not in color:
                # 邻居不在已知节点中（悬空依赖），跳过
                continue
            if color[neighbor] == GRAY:
                # 找到环：从 stack 中定位环起点
                cycle_start = stack.index(neighbor)
                cycle = stack[cycle_start:] + [neighbor]
                key = frozenset(cycle[:-1])
                if key not in seen_cycles:
                    seen_cycles.add(key)
                    cycles.append(cycle)
            elif color[neighbor] == WHITE:
                dfs(neighbor)

        stack.pop()
        color[node] = BLACK

    for node in graph:
        if color[node] == WHITE:
            dfs(node)

    return cycles


def find_dangling(graph: Dict[str, List[str]]) -> List[Tuple[str, str]]:
    """返回悬空依赖列表：(依赖方模块名, 不存在的被依赖名)。"""
    known = set(graph.keys())
    dangling: List[Tuple[str, str]] = []
    for mod, deps in graph.items():
        for dep in deps:
            if dep not in known:
                dangling.append((mod, dep))
    return dangling


# ---------------------------------------------------------------------------
# 主程序
# ---------------------------------------------------------------------------

def build_arg_parser() -> argparse.ArgumentParser:
    """构建命令行参数解析器。"""
    parser = argparse.ArgumentParser(
        description=(
            '静态分析业务模块 .c 文件中的 module_interface_t depends_on 声明，'
            '检测循环依赖与悬空依赖。无需编译，无需 Zephyr 工具链。'
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            '退出码：\n'
            '  0  无问题\n'
            '  1  仅有悬空依赖或解析警告（--strict 时视为失败，默认仅警告）\n'
            '  2  存在循环依赖（始终为硬错误）\n'
        ),
    )
    parser.add_argument(
        '--root',
        default='src',
        metavar='DIR',
        help='递归扫描的根目录（默认：src）',
    )
    parser.add_argument(
        '--verbose', '-v',
        action='store_true',
        help='打印完整依赖图（每个模块及其依赖列表）',
    )
    parser.add_argument(
        '--strict',
        action='store_true',
        help='悬空依赖与解析警告也视为失败（退出码 1），默认仅打印警告',
    )
    return parser


def main() -> int:
    """脚本入口：解析参数、执行扫描、输出报告并返回退出码。"""
    parser = build_arg_parser()
    args = parser.parse_args()

    root = Path(args.root)
    if not root.is_dir():
        print(f'错误：扫描根目录不存在或不是目录：{root}', file=sys.stderr)
        return 2

    # ---- 扫描 ----
    mod_source, graph, warnings = scan_root(root)

    total_modules = len(graph)
    total_edges = sum(len(deps) for deps in graph.values())

    # ---- 基本摘要 ----
    print(f'扫描根目录：{root.resolve()}')
    print(f'发现模块：{total_modules} 个，依赖边：{total_edges} 条')
    print()

    # ---- 详细图 ----
    if args.verbose:
        print('=== 完整依赖图 ===')
        for mod in sorted(graph):
            deps = graph[mod]
            dep_str = ', '.join(deps) if deps else '（无依赖）'
            print(f'  {mod} -> {dep_str}')
        print()

    # ---- 解析警告 ----
    if warnings:
        print(f'=== 解析警告（{len(warnings)} 条）===')
        for w in warnings:
            print(f'  [WARN] {w}')
        print()

    # ---- 悬空依赖 ----
    dangling = find_dangling(graph)
    if dangling:
        print(f'=== 悬空依赖（{len(dangling)} 条）===')
        for mod, dep in sorted(dangling):
            print(f'  [DANGLE] {mod} -> 未知模块 "{dep}"')
        print()

    # ---- 循环依赖 ----
    cycles = find_cycles(graph)
    if cycles:
        print(f'=== 循环依赖（{len(cycles)} 个环）===')
        for cycle in cycles:
            print(f'  [CYCLE] {" -> ".join(cycle)}')
        print()

    # ---- 结论 ----
    if cycles:
        print('结论：检测到循环依赖，退出码 2（CI 硬错误）。')
        return 2

    if dangling or warnings:
        if args.strict:
            print('结论：存在悬空依赖或解析警告，--strict 模式下退出码 1。')
            return 1
        else:
            print('结论：存在悬空依赖或解析警告（使用 --strict 可将其视为失败）。')
            return 0

    print('结论：未发现循环依赖或悬空依赖，一切正常。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
