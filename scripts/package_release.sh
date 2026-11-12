#!/bin/bash
# 固件打包脚本
# 用于发布时打包构建产物

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/project_layout.sh"
initialize_zephyr_project_layout "${SCRIPT_DIR}"
write_zephyr_project_banner

PROJECT_ROOT="${ZP_WORK_ROOT}"
PACKAGE_NAME="$(get_zephyr_package_name)"

echo "============================================"
echo "Zephyr 固件打包工具"
echo "============================================"

# 默认值
VERSION=""
BUILD_DIR="build"
OUTPUT_DIR="release"

# 解析参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -v|--version)
            VERSION="$2"
            shift 2
            ;;
        -b|--build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        -o|--output)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        -h|--help)
            echo "用法：$0 [选项]"
            echo ""
            echo "选项:"
            echo "  -v, --version VER     版本号"
            echo "  -b, --build-dir DIR   构建目录（默认：build）"
            echo "  -o, --output DIR      输出目录（默认：release）"
            echo "  -h, --help            显示帮助"
            exit 0
            ;;
        *)
            echo "未知选项：$1"
            exit 1
            ;;
    esac
done

# 读取 APP_VERSION 文件
APP_VERSION="unknown"
APP_VERSION_FILE=""
if APP_VERSION_FILE="$(get_zephyr_app_version_file)"; then
    APP_VERSION=$(tr -d '[:space:]' < "$APP_VERSION_FILE")
fi

# 如果没有指定版本号，优先使用 APP_VERSION，然后尝试从 git 获取
if [ -z "$VERSION" ] || [ "$VERSION" = "unknown" ]; then
    VERSION="$APP_VERSION"
    if command -v git &> /dev/null; then
        GIT_VERSION=$(git describe --tags --always --dirty 2>/dev/null || true)
        if [ -n "$GIT_VERSION" ]; then
            VERSION="$GIT_VERSION"
        fi
    fi
fi

BUILD_DATE=$(date -u +"%Y-%m-%d %H:%M:%S")
BUILD_HOST=$(hostname)

echo "版本号：$VERSION"
echo "构建目录：$BUILD_DIR"
echo "输出目录：$OUTPUT_DIR"
echo ""

# 创建输出目录
mkdir -p "$PROJECT_ROOT/$OUTPUT_DIR"
OUTPUT_DIR="$PROJECT_ROOT/$OUTPUT_DIR"
BUILD_DIR="$PROJECT_ROOT/$BUILD_DIR"

# 复制固件文件
echo "正在复制固件文件..."

if [ -f "$BUILD_DIR/zephyr/zephyr.bin" ]; then
    cp "$BUILD_DIR/zephyr/zephyr.bin" "$OUTPUT_DIR/zephyr_${VERSION}.bin"
    echo "  ✓ zephyr.bin"
fi

if [ -f "$BUILD_DIR/zephyr/zephyr.elf" ]; then
    cp "$BUILD_DIR/zephyr/zephyr.elf" "$OUTPUT_DIR/zephyr_${VERSION}.elf"
    echo "  ✓ zephyr.elf"
fi

if [ -f "$BUILD_DIR/zephyr/zephyr.hex" ]; then
    cp "$BUILD_DIR/zephyr/zephyr.hex" "$OUTPUT_DIR/zephyr_${VERSION}.hex"
    echo "  ✓ zephyr.hex"
fi

if [ -f "$BUILD_DIR/zephyr/zephyr.map" ]; then
    cp "$BUILD_DIR/zephyr/zephyr.map" "$OUTPUT_DIR/zephyr_${VERSION}.map"
    echo "  ✓ zephyr.map"
fi

# 创建版本信息文件
echo "正在创建版本信息..."

# 获取文件大小
get_file_size() {
    if [ -f "$1" ]; then
        size=$(stat -c%s "$1" 2>/dev/null || stat -f%z "$1" 2>/dev/null || echo "0")
        echo "$size"
    else
        echo "0"
    fi
}

BIN_SIZE=$(get_file_size "$OUTPUT_DIR/zephyr_${VERSION}.bin")
ELF_SIZE=$(get_file_size "$OUTPUT_DIR/zephyr_${VERSION}.elf")
HEX_SIZE=$(get_file_size "$OUTPUT_DIR/zephyr_${VERSION}.hex")
MAP_SIZE=$(get_file_size "$OUTPUT_DIR/zephyr_${VERSION}.map")

cat > "$OUTPUT_DIR/release_info.txt" << EOF
================================================================================
${PACKAGE_NAME} - Release Package
================================================================================

版本信息
--------
  APP 版本：   $APP_VERSION
  Git 版本：   $VERSION
  编译时间：   $BUILD_DATE
  编译主机：   $BUILD_HOST

文件列表
--------
  zephyr_${VERSION}.bin  ($(printf "%'d" "$BIN_SIZE") bytes)
  zephyr_${VERSION}.elf  ($(printf "%'d" "$ELF_SIZE") bytes)
  zephyr_${VERSION}.hex  ($(printf "%'d" "$HEX_SIZE") bytes)
  zephyr_${VERSION}.map  ($(printf "%'d" "$MAP_SIZE") bytes)

================================================================================
EOF

echo "  ✓ release_info.txt"

# 复制 README 和许可证
cp "$PROJECT_ROOT/README.md" "$OUTPUT_DIR/" 2>/dev/null && echo "  ✓ README.md"
cp "$PROJECT_ROOT/LICENSE" "$OUTPUT_DIR/" 2>/dev/null && echo "  ✓ LICENSE"

# 创建压缩包
echo ""
echo "正在创建压缩包..."

ARCHIVE_NAME="${PACKAGE_NAME}_${VERSION}"

cd "$OUTPUT_DIR"

if command -v zip &> /dev/null; then
    zip -r "${ARCHIVE_NAME}.zip" . \
        -x "*.git*" \
        -x "*.o" \
        -x "*.d"
    echo "  ✓ ${ARCHIVE_NAME}.zip"
fi

if command -v tar &> /dev/null; then
    tar -czf "${ARCHIVE_NAME}.tar.gz" \
        --exclude='.git' \
        --exclude='*.o' \
        --exclude='*.d' \
        .
    echo "  ✓ ${ARCHIVE_NAME}.tar.gz"
fi

cd "$PROJECT_ROOT"

echo ""
echo "============================================"
echo "打包完成"
echo "============================================"
echo "输出目录：$OUTPUT_DIR"
echo ""
echo "文件列表:"
ls -la "$OUTPUT_DIR"
echo ""
