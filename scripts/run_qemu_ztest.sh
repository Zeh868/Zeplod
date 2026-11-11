#!/usr/bin/env bash
# Run tests/ ztest on qemu_riscv32 (QEMU). This is not native_sim or native_posix.
# Physical PHY, board peripherals, and wall-clock timing are not covered.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "${SCRIPT_DIR}/qemu_ztest_run.py" "$@"
