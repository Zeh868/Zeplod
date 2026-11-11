#!/bin/bash
# =============================================================================
# Zephyr environment setup script (Linux/macOS)
# =============================================================================
# Usage: source scripts/setup_env.sh
# =============================================================================

set -e

echo "============================================"
echo "Zephyr environment setup"
echo "============================================"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/project_layout.sh"
initialize_zephyr_project_layout "${SCRIPT_DIR}"
write_zephyr_project_banner

CONFIG_FILE="${ZP_CONFIG_FILE}"

# Non-zero status stops via set -e, whether sourced or executed.
_exit_script() {
    return "$1"
}

_trim() {
    local s="$1"
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

if [ ! -f "$CONFIG_FILE" ]; then
    echo "Error: zephyr_config.env not found."
    echo "Please copy zephyr_config.env.template to zephyr_config.env and edit paths."
    _exit_script 1
fi

echo "Loading configuration from zephyr_config.env..."
while IFS= read -r line || [ -n "$line" ]; do
    line="${line%$'\r'}"
    case "$line" in
        ''|'#'*)
            continue
            ;;
    esac

    case "$line" in
        *=*)
            key="$(_trim "${line%%=*}")"
            value="$(_trim "${line#*=}")"

            if [[ "$key" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]]; then
                printf -v "$key" '%s' "$value"
                export "${key?}"
            fi
            ;;
    esac
done < "$CONFIG_FILE"

if [ -n "$VIRTUAL_ENV_PATH" ]; then
    VENV_ACTIVATE="$VIRTUAL_ENV_PATH/bin/activate"
    if [ -f "$VENV_ACTIVATE" ]; then
        # shellcheck disable=SC1090
        source "$VENV_ACTIVATE"
        echo "Activated virtual environment: $VENV_ACTIVATE"
    else
        echo "Warning: virtual env activation script not found: $VENV_ACTIVATE"
    fi
fi

if [ -z "$ZEPHYR_BASE" ]; then
    echo "Error: ZEPHYR_BASE is not set in config."
    _exit_script 1
fi

if [ -z "$ZEPHYR_SDK_INSTALL_DIR" ]; then
    echo "Error: ZEPHYR_SDK_INSTALL_DIR is not set in config."
    _exit_script 1
fi

if [ ! -d "$ZEPHYR_BASE" ]; then
    echo "Error: ZEPHYR_BASE path does not exist: $ZEPHYR_BASE"
    _exit_script 1
fi

if [ ! -d "$ZEPHYR_SDK_INSTALL_DIR" ]; then
    echo "Error: ZEPHYR_SDK_INSTALL_DIR path does not exist: $ZEPHYR_SDK_INSTALL_DIR"
    _exit_script 1
fi

export ZEPHYR_BASE
export ZEPHYR_SDK_INSTALL_DIR

if [ -d "$ZEPHYR_SDK_INSTALL_DIR/arm-zephyr-eabi/bin" ]; then
    export PATH="$ZEPHYR_SDK_INSTALL_DIR/arm-zephyr-eabi/bin:$PATH"
fi

if [ -d "$ZEPHYR_SDK_INSTALL_DIR/tools/bin" ]; then
    export PATH="$ZEPHYR_SDK_INSTALL_DIR/tools/bin:$PATH"
fi

if [ -f "$ZEPHYR_BASE/scripts/env.sh" ]; then
    echo "Running Zephyr environment script..."
    # Path comes from the local zephyr_config.env; this Zephyr file is not in the repo.
    # shellcheck disable=SC1090,SC1091
    source "$ZEPHYR_BASE/scripts/env.sh"
fi

echo "============================================"
echo "Environment configured successfully."
echo "============================================"
echo "ZEPHYR_BASE=$ZEPHYR_BASE"
echo "ZEPHYR_SDK_INSTALL_DIR=$ZEPHYR_SDK_INSTALL_DIR"
echo "============================================"
echo ""
echo "You can now build:"
if [ "${ZP_MODE}" = "app" ]; then
    echo "  west build -b ${DEFAULT_BOARD:-<your_board>} -d build .    # from app root"
    echo "  west build -b ${DEFAULT_BOARD:-<your_board>} -d build framework   # framework only"
else
    echo "  west build -b ${DEFAULT_BOARD:-<your_board>} -d build ."
fi
echo ""
