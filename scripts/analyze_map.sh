#!/bin/bash
# shellcheck shell=bash
# =============================================================================
# Zephyr Map 文件分析脚本 (Linux/macOS Bash)
# =============================================================================
# 用法：./scripts/analyze_map.sh [map 文件路径]
# =============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/project_layout.sh"
initialize_zephyr_project_layout "${SCRIPT_DIR}"
write_zephyr_project_banner

PROJECT_ROOT="${ZP_WORK_ROOT}"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

# 帮助信息
show_help() {
    echo "用法：$0 [选项]"
    echo ""
    echo "选项:"
    echo "  -f, --file PATH    Map 文件路径（默认：release/*.map 中最新的文件）"
    echo "  -h, --help         显示帮助"
    exit 0
}

# 解析参数
MAP_FILE=""
while [[ $# -gt 0 ]]; do
    case $1 in
        -f|--file)
            MAP_FILE="$2"
            shift 2
            ;;
        -h|--help)
            show_help
            ;;
        *)
            echo "未知选项：$1"
            show_help
            ;;
    esac
done

# 查找 map 文件（目录下取最近修改的 .map；此处用 ls 简写，忽略 SC2012）
if [ -z "$MAP_FILE" ]; then
    RELEASE_DIR="$PROJECT_ROOT/release"
    if [ -d "$RELEASE_DIR" ]; then
        # shellcheck disable=SC2012
        MAP_FILE=$(ls -t "$RELEASE_DIR"/*.map 2>/dev/null | head -n 1)
    fi
fi

if [ -z "$MAP_FILE" ] || [ ! -f "$MAP_FILE" ]; then
    echo -e "${RED}错误：找不到 Map 文件！${NC}"
    echo "用法：$0 -f <path>"
    exit 1
fi

echo "============================================"
echo "Zephyr Map 文件分析工具"
echo "============================================"
echo "分析文件：$MAP_FILE"
echo ""

# =============================================================================
# 解析 BSS 段
# =============================================================================
echo "============================================"
echo "1. BSS 段 (未初始化数据) 分布 - Top 20"
echo "============================================"

printf "%-35s %15s %12s %s\n" "Variable" "Size (Bytes)" "Size (KB)" "Module"
printf "%-35s %15s %12s %s\n" "───────────────────────────────────" "───────────────" "────────────" "────────────────────"

# 提取 BSS 数据并排序
grep -oE '\.bss\.[^ ]+ +0x[0-9a-fA-F]+ +0x[0-9a-fA-F]+ +[^ ]+' "$MAP_FILE" | \
while read -r line; do
    var=$(echo "$line" | awk '{print $1}' | sed 's/\.bss\.//')
    size_hex=$(echo "$line" | awk '{print $3}')
    size_dec=$((size_hex))
    module=$(echo "$line" | awk '{print $4}' | xargs basename 2>/dev/null | sed 's/\.obj$//')
    size_kb=$(echo "scale=2; $size_dec / 1024" | bc)
    printf "%-35s %15d %12s %s\n" "$var" "$size_dec" "$size_kb" "$module"
done | sort -t' ' -k2 -rn | head -n 20

# BSS 总计
TOTAL_BSS=$(grep -oE '\.bss\.[^ ]+ +0x[0-9a-fA-F]+ +0x[0-9a-fA-F]+ +[^ ]+' "$MAP_FILE" | \
    awk '{sum += strtonum($3)} END {print sum}')
echo ""
echo "BSS 段总计：$TOTAL_BSS 字节 ($(echo "scale=2; $TOTAL_BSS / 1024" | bc) KB)"
echo ""

# =============================================================================
# 解析 NOINIT 段
# =============================================================================
echo "============================================"
echo "2. NOINIT 段 (不初始化数据) 分布"
echo "============================================"

printf "%-50s %15s %12s\n" "Section" "Size (Bytes)" "Size (KB)"
printf "%-50s %15s %12s\n" "──────────────────────────────────────────────────" "───────────────" "────────────"

grep -oE '\.noinit\.[^ ]+ +0x[0-9a-fA-F]+ +0x[0-9a-fA-F]+' "$MAP_FILE" | \
while read -r line; do
    section=$(echo "$line" | awk '{print $1}' | sed 's/\.noinit\.//' | sed 's/^WEST_TOPDIR\///')
    size_hex=$(echo "$line" | awk '{print $3}')
    size_dec=$((size_hex))
    size_kb=$(echo "scale=2; $size_dec / 1024" | bc)
    printf "%-50s %15d %12s\n" "$section" "$size_dec" "$size_kb"
done | sort -t' ' -k2 -rn

TOTAL_NOINIT=$(grep -oE '\.noinit\.[^ ]+ +0x[0-9a-fA-F]+ +0x[0-9a-fA-F]+' "$MAP_FILE" | \
    awk '{sum += strtonum($3)} END {print sum}')
echo ""
echo "NOINIT 段总计：$TOTAL_NOINIT 字节 ($(echo "scale=2; $TOTAL_NOINIT / 1024" | bc) KB)"
echo ""

# =============================================================================
# 解析代码段 (按模块统计)
# =============================================================================
echo "============================================"
echo "3. 代码段 (.text) 按模块分布"
echo "============================================"

printf "%-30s %15s %12s\n" "Module" "Size (Bytes)" "Size (KB)"
printf "%-30s %15s %12s\n" "──────────────────────────────" "───────────────" "────────────"

# 按模块统计代码大小
declare -A TEXT_SIZE
while read -r line; do
    size_hex=$(echo "$line" | awk '{print $3}')
    size_dec=$((size_hex))
    module=$(echo "$line" | awk '{print $4}' | xargs basename 2>/dev/null | sed 's/\.obj$//')
    if [ -n "$module" ]; then
        current=${TEXT_SIZE[$module]:-0}
        TEXT_SIZE[$module]=$((current + size_dec))
    fi
done < <(grep -oE '\.text[^ ]* +0x[0-9a-fA-F]+ +0x[0-9a-fA-F]+ +[^ ]+\.obj' "$MAP_FILE")

for module in "${!TEXT_SIZE[@]}"; do
    size=${TEXT_SIZE[$module]}
    size_kb=$(echo "scale=2; $size / 1024" | bc)
    printf "%-30s %15d %12s\n" "$module" "$size" "$size_kb"
done | sort -t' ' -k2 -rn

TOTAL_TEXT=$(grep -oE '\.text[^ ]* +0x[0-9a-fA-F]+ +0x[0-9a-fA-F]+ +[^ ]+\.obj' "$MAP_FILE" | \
    awk '{sum += strtonum($3)} END {print sum}')
echo ""
echo "代码段总计：$TOTAL_TEXT 字节 ($(echo "scale=2; $TOTAL_TEXT / 1024" | bc) KB)"
echo ""

# =============================================================================
# 解析只读数据段
# =============================================================================
echo "============================================"
echo "4. 只读数据段 (.rodata) 分布 - Top 15"
echo "============================================"

printf "%-30s %15s %12s\n" "Module" "Size (Bytes)" "Size (KB)"
printf "%-30s %15s %12s\n" "──────────────────────────────" "───────────────" "────────────"

declare -A RODATA_SIZE
while read -r line; do
    size_hex=$(echo "$line" | awk '{print $3}')
    size_dec=$((size_hex))
    module=$(echo "$line" | awk '{print $4}' | xargs basename 2>/dev/null | sed 's/\.obj$//')
    if [ -n "$module" ]; then
        current=${RODATA_SIZE[$module]:-0}
        RODATA_SIZE[$module]=$((current + size_dec))
    fi
done < <(grep -oE '\.rodata[^ ]* +0x[0-9a-fA-F]+ +0x[0-9a-fA-F]+ +[^ ]+' "$MAP_FILE")

for module in "${!RODATA_SIZE[@]}"; do
    size=${RODATA_SIZE[$module]}
    if [ "$size" -gt 0 ]; then
        size_kb=$(echo "scale=2; $size / 1024" | bc)
        printf "%-30s %15d %12s\n" "$module" "$size" "$size_kb"
    fi
done | sort -t' ' -k2 -rn | head -n 15

TOTAL_RODATA=$(grep -oE '\.rodata[^ ]* +0x[0-9a-fA-F]+ +0x[0-9a-fA-F]+ +[^ ]+' "$MAP_FILE" | \
    awk '{sum += strtonum($3)} END {print sum}')
echo ""
echo "只读数据段总计：$TOTAL_RODATA 字节 ($(echo "scale=2; $TOTAL_RODATA / 1024" | bc) KB)"
echo ""

# =============================================================================
# 内存使用总结
# =============================================================================
echo "============================================"
echo "5. 内存使用总结"
echo "============================================"

# RAM 计算 (假设 192KB)
RAM_TOTAL=$((192 * 1024))
RAM_USED=$((TOTAL_BSS + TOTAL_NOINIT))
RAM_PERCENT=$(echo "scale=2; ($RAM_USED * 100) / $RAM_TOTAL" | bc)
RAM_REMAINING=$((RAM_TOTAL - RAM_USED))

echo "RAM 使用情况:"
printf "  BSS 段：     %12d 字节 (%10.2f KB)\n" "$TOTAL_BSS" "$(echo "scale=2; $TOTAL_BSS / 1024" | bc)"
printf "  NOINIT 段：  %12d 字节 (%10.2f KB)\n" "$TOTAL_NOINIT" "$(echo "scale=2; $TOTAL_NOINIT / 1024" | bc)"
echo "  ─────────────────────────────"
printf "  已使用：     %12d 字节 (%10.2f KB) (%s%%)\n" "$RAM_USED" "$(echo "scale=2; $RAM_USED / 1024" | bc)" "$RAM_PERCENT"
printf "  总容量：     %12d 字节 (%10.2f KB)\n" "$RAM_TOTAL" "$(echo "scale=2; $RAM_TOTAL / 1024" | bc)"
printf "  剩余可用：   %12d 字节 (%10.2f KB)\n" "$RAM_REMAINING" "$(echo "scale=2; $RAM_REMAINING / 1024" | bc)"
echo ""

# Flash 计算 (假设 2MB)
FLASH_TOTAL=$((2 * 1024 * 1024))
FLASH_USED=$((TOTAL_TEXT + TOTAL_RODATA))
FLASH_PERCENT=$(echo "scale=2; ($FLASH_USED * 100) / $FLASH_TOTAL" | bc)
FLASH_REMAINING=$((FLASH_TOTAL - FLASH_USED))

echo "Flash 使用情况:"
printf "  代码段：     %12d 字节 (%10.2f KB)\n" "$TOTAL_TEXT" "$(echo "scale=2; $TOTAL_TEXT / 1024" | bc)"
printf "  只读数据：   %12d 字节 (%10.2f KB)\n" "$TOTAL_RODATA" "$(echo "scale=2; $TOTAL_RODATA / 1024" | bc)"
echo "  ─────────────────────────────"
printf "  已使用：     %12d 字节 (%10.2f KB) (%s%%)\n" "$FLASH_USED" "$(echo "scale=2; $FLASH_USED / 1024" | bc)" "$FLASH_PERCENT"
printf "  总容量：     %12d 字节 (%10.2f KB)\n" "$FLASH_TOTAL" "$(echo "scale=2; $FLASH_TOTAL / 1024" | bc)"
printf "  剩余可用：   %12d 字节 (%10.2f KB)\n" "$FLASH_REMAINING" "$(echo "scale=2; $FLASH_REMAINING / 1024" | bc)"
echo ""

# =============================================================================
# 输出到文件
# =============================================================================
OUTPUT_FILE="$PROJECT_ROOT/release/memory_analysis.txt"
{
    echo "============================================"
    echo "Zephyr Map 文件分析报告"
    echo "============================================"
    echo "分析文件：$MAP_FILE"
    echo "生成时间：$(date '+%Y-%m-%d %H:%M:%S')"
    echo ""
    echo "=== BSS 段 Top 20 ==="
    echo ""
    echo "=== 代码段按模块 ==="
    echo ""
    echo "=== 内存使用总结 ==="
    echo "RAM 使用：$RAM_USED 字节 ($(echo "scale=2; $RAM_USED / 1024" | bc) KB) / $RAM_TOTAL 字节 ($(echo "scale=2; $RAM_TOTAL / 1024" | bc) KB) = ${RAM_PERCENT}%"
    echo "Flash 使用：$FLASH_USED 字节 ($(echo "scale=2; $FLASH_USED / 1024" | bc) KB) / $FLASH_TOTAL 字节 ($(echo "scale=2; $FLASH_TOTAL / 1024" | bc) KB) = ${FLASH_PERCENT}%"
} > "$OUTPUT_FILE"

echo -e "${GREEN}详细报告已保存到：$OUTPUT_FILE${NC}"
echo ""
