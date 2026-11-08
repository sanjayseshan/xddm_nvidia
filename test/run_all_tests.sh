#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=========================================================="
echo " Running All Antigravity Driver & Hardware HAL Tests"
echo "=========================================================="

echo "==> 1. Running Video Miniport Driver & OpenRM host unit tests..."
make -C "$ROOT_DIR/src_driver" check

echo "==> 2. Building 64-bit production miniport (nvxmp.sys)..."
make -C "$ROOT_DIR/src_driver" full

echo "==> 3. Running Display Driver unit tests & build (nvxdisp.dll)..."
make -C "$ROOT_DIR/src_disp" check

echo "==> 4. Building 32-bit Windows 98 SE VxD (nvx.vxd)..."
bash "$ROOT_DIR/src_win98/build_vxd.sh"

echo "=========================================================="
echo " All driver test suites and binary builds PASSED!"
echo "=========================================================="
