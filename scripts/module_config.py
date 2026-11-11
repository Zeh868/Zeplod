#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
模块配置管理脚本
用于管理项目中的模块启用/禁用配置

用法:
    python scripts/module_config.py list                    # 列出所有可用模块
    python scripts/module_config.py enable <module_name>     # 启用指定模块
    python scripts/module_config.py disable <module_name>    # 禁用指定模块
    python scripts/module_config.py status                   # 显示当前模块状态
    python scripts/module_config.py generate                 # 生成 conf/custom_modules.conf
"""

import os
import sys
import re
import argparse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from project_layout import resolve_project_layout

_LAYOUT = resolve_project_layout()
PROJECT_ROOT = _LAYOUT.framework_root

# 模块定义
MODULES = {
    # 示例模块
    "EXAMPLE_MODULE_GPIO": {"name": "GPIO 示例模块", "default": False},
    "EXAMPLE_MODULE_UART": {"name": "UART 示例模块", "default": False},
    "EXAMPLE_MODULE_MULTI_DEP": {"name": "多依赖示例模块", "default": False},
    "EXAMPLE_MODULE_THREAD_IPC": {"name": "IPC 线程示例模块", "default": False},

    # 系统服务
    "SYS_WATCHDOG_ENABLE": {"name": "系统看门狗", "default": True},
    "APP_KV_PERSIST": {"name": "应用 KV 持久化", "default": False},
    "THREAD_IPC_SERVICE": {"name": "线程 IPC 服务", "default": False},
    "THREAD_IPC_SERVICE_EVENT_BRIDGE": {"name": "IPC 事件桥", "default": False},
}


def read_current_config():
    """读取 prj.conf 中的当前配置"""
    prj_conf = PROJECT_ROOT / "prj.conf"
    if not prj_conf.exists():
        return {}

    config = {}
    with open(prj_conf, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            match = re.match(r'CONFIG_(\w+)=(y|n)', line)
            if match:
                config[match.group(1)] = match.group(2) == 'y'

    return config


def list_modules():
    """列出所有可用模块"""
    print("\n" + "="*60)
    print("可用模块列表")
    print("="*60)

    current_config = read_current_config()

    for module_id, info in MODULES.items():
        is_enabled = current_config.get(module_id, info.get("default", False))
        status = "[启用]" if is_enabled else "[禁用]"

        print(f"  {status:6} {module_id:40} {info['name']}")

    print("\n" + "="*60)


def show_status():
    """显示当前模块状态"""
    print("\n" + "="*60)
    print("当前模块状态")
    print("="*60)

    current_config = read_current_config()

    enabled = []
    disabled = []

    for module_id, info in MODULES.items():
        is_enabled = current_config.get(module_id, info.get("default", False))
        if is_enabled:
            enabled.append((module_id, info))
        else:
            disabled.append((module_id, info))

    print(f"\n已启用的模块 ({len(enabled)}):")
    for module_id, info in enabled:
        print(f"  ✓ {module_id} - {info['name']}")

    print(f"\n已禁用的模块 ({len(disabled)}):")
    for module_id, info in disabled:
        print(f"  ✗ {module_id} - {info['name']}")

    print("\n" + "="*60)


def enable_module(module_name):
    """启用指定模块"""
    if module_name not in MODULES:
        print(f"错误: 未知模块 '{module_name}'")
        print("可用的模块:")
        for m in MODULES.keys():
            print(f"  {m}")
        return False

    # 读取 prj.conf
    prj_conf = PROJECT_ROOT / "prj.conf"
    if not prj_conf.exists():
        print(f"错误: prj.conf 不存在")
        return False

    with open(prj_conf, 'r', encoding='utf-8') as f:
        content = f.read()

    # 检查是否已有此配置
    config_line = f"CONFIG_{module_name}="
    if config_line in content:
        # 更新现有配置
        content = re.sub(
            f"{config_line}[yn]",
            f"{config_line}y",
            content
        )
        print(f"已更新: {module_name} -> y")
    else:
        # 添加新配置
        content += f"\n{config_line}y\n"
        print(f"已添加: {module_name} -> y")

    # 写回文件
    with open(prj_conf, 'w', encoding='utf-8') as f:
        f.write(content)

    print(f"模块 '{module_name}' 已启用")
    return True


def disable_module(module_name):
    """禁用指定模块"""
    if module_name not in MODULES:
        print(f"错误: 未知模块 '{module_name}'")
        return False

    # 读取 prj.conf
    prj_conf = PROJECT_ROOT / "prj.conf"
    if not prj_conf.exists():
        print(f"错误: prj.conf 不存在")
        return False

    with open(prj_conf, 'r', encoding='utf-8') as f:
        content = f.read()

    # 检查是否已有此配置
    config_line = f"CONFIG_{module_name}="
    if config_line in content:
        # 更新现有配置
        content = re.sub(
            f"{config_line}[yn]",
            f"{config_line}n",
            content
        )
        print(f"模块 '{module_name}' 已禁用")
    else:
        # 添加新配置
        content += f"\n{config_line}n\n"
        print(f"已添加: {module_name} -> n")

    # 写回文件
    with open(prj_conf, 'w', encoding='utf-8') as f:
        f.write(content)

    return True


def generate_custom_conf(output_name="prj_custom.conf"):
    """生成自定义配置文件"""
    current_config = read_current_config()

    output_file = PROJECT_ROOT / output_name
    output_file.parent.mkdir(parents=True, exist_ok=True)

    with open(output_file, 'w', encoding='utf-8') as f:
        f.write("# 自定义模块配置文件\n")
        f.write(f"# 生成时间: {__import__('datetime').datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write("#\n")
        f.write("# 使用方法:\n")
        f.write("#   west build -b <board> -DCONF_FILE=\"prj.conf;{output_name}\" .\n\n".format(output_name=output_name))

        for module_id, is_enabled in sorted(current_config.items()):
            if module_id in MODULES:
                info = MODULES[module_id]
                f.write(f"# {info['name']}\n")
                f.write(f"CONFIG_{module_id}={'y' if is_enabled else 'n'}\n\n")

    print(f"已生成自定义配置文件: {output_file}")
    print(f"使用方法: west build -b <board> -DCONF_FILE=\"prj.conf;{output_name}\" .")


def main():
    parser = argparse.ArgumentParser(description="模块配置管理工具")
    subparsers = parser.add_subparsers(dest="command", help="命令")

    # list 命令
    subparsers.add_parser("list", help="列出所有可用模块")

    # status 命令
    subparsers.add_parser("status", help="显示当前模块状态")

    # enable 命令
    enable_parser = subparsers.add_parser("enable", help="启用指定模块")
    enable_parser.add_argument("module", help="模块名称")

    # disable 命令
    disable_parser = subparsers.add_parser("disable", help="禁用指定模块")
    disable_parser.add_argument("module", help="模块名称")

    # generate 命令
    generate_parser = subparsers.add_parser("generate", help="生成自定义配置文件")
    generate_parser.add_argument("--output", default="conf/custom_modules.conf", help="输出文件路径（相对仓库根）")

    args = parser.parse_args()

    if args.command == "list":
        list_modules()
    elif args.command == "status":
        show_status()
    elif args.command == "enable":
        enable_module(args.module)
    elif args.command == "disable":
        disable_module(args.module)
    elif args.command == "generate":
        generate_custom_conf(args.output)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
