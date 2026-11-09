#!/usr/bin/env bash
# Install a one-time test run without changing GPU bindings or driver registry.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
NVX_XP32_IMAGE="${NVX_XP32_IMAGE:-$ROOT_DIR/winxp_x32.qcow2}"
# QEMU's normal image open refuses a disk already held by the running VM.
qemu-img info "$NVX_XP32_IMAGE" >/dev/null
NVX_TEST_BATCH="$(mktemp /tmp/nvx-graphics-tests.XXXXXX.bat)"
trap 'rm -f "$NVX_TEST_BATCH"' EXIT
i686-w64-mingw32-gcc -O2 -Wall -Wextra -static-libgcc test/test_gpu_accel.c \
    -lopengl32 -lgdi32 -luser32 -o test/test_gpu_accel32.exe
i686-w64-mingw32-gcc -x c -O2 -static-libgcc test/3d_tests/glxgears_main.cpp \
    -lopengl32 -lgdi32 -luser32 -o test/3d_tests/wglgears32.exe
python3 - "$NVX_TEST_BATCH" <<'PY'
from pathlib import Path
import sys
Path(sys.argv[1]).write_bytes(b'''@echo off
cd /d C:\\test_accel
if exist C:\\test_accel.log del C:\\test_accel.log
test_gpu_accel.exe > C:\\test_accel_console.log 2>&1
echo SuiteExit=%ERRORLEVEL% > C:\\graphics_test_results.txt
test_gpu_accel.exe --2d-only --secondary >> C:\\test_accel_console.log 2>&1
echo Secondary2DExit=%ERRORLEVEL% >> C:\\graphics_test_results.txt
wglgears.exe -info -seconds 10 > C:\\wglgears_console.log 2>&1
echo GearsExit=%ERRORLEVEL% >> C:\\graphics_test_results.txt
echo [GRAPHICS_TESTS_FINISHED] >> C:\\graphics_test_results.txt
del "%~f0"
'''.replace(b'\n',b'\r\n'))
PY
guestfish --rw -a "$NVX_XP32_IMAGE" -i <<GUEST
mkdir-p /test_accel
upload test/test_gpu_accel32.exe /test_accel/test_gpu_accel.exe
upload test/3d_tests/wglgears32.exe /test_accel/wglgears.exe
upload "$NVX_TEST_BATCH" "/Documents and Settings/All Users/Start Menu/Programs/Startup/nvx_graphics_once.bat"
GUEST
echo "One-time graphics run installed; no drivers or bindings changed."
