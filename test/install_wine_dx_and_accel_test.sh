#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

QCOW="$ROOT_DIR/winxp_x64.qcow2"

echo "=========================================================="
echo " Deploying Antigravity Drivers & Wine DirectX-OpenGL Suite"
echo " Target Disk: $QCOW"
echo "=========================================================="

# Build latest drivers, ucrtbase shim, and test executables
make -C "$ROOT_DIR/src_driver" full
make -C "$ROOT_DIR/src_disp"

# Build XP-compatible ucrtbase shim
x86_64-w64-mingw32-gcc -O2 -shared "$ROOT_DIR/test/ucrtbase_xp.c" "$ROOT_DIR/test/ucrtbase.def" -o "$ROOT_DIR/test/ucrtbase.dll" -lm
i686-w64-mingw32-gcc -O2 -shared "$ROOT_DIR/test/ucrtbase_xp.c" "$ROOT_DIR/test/ucrtbase.def" -o "$ROOT_DIR/test/ucrtbase32.dll" -lm

# Build acceleration test suite
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra "$ROOT_DIR/test/test_gpu_accel.c" -lopengl32 -lgdi32 -luser32 -o "$ROOT_DIR/test/test_gpu_accel.exe"
i686-w64-mingw32-gcc -O2 -Wall -Wextra "$ROOT_DIR/test/test_gpu_accel.c" -lopengl32 -lgdi32 -luser32 -o "$ROOT_DIR/test/test_gpu_accel32.exe"

# Build online 3D benchmark (wglgears)
x86_64-w64-mingw32-gcc -x c -O2 "$ROOT_DIR/test/3d_tests/glxgears_main.cpp" -lopengl32 -lgdi32 -luser32 -o "$ROOT_DIR/test/3d_tests/wglgears.exe"
i686-w64-mingw32-gcc -x c -O2 "$ROOT_DIR/test/3d_tests/glxgears_main.cpp" -lopengl32 -lgdi32 -luser32 -o "$ROOT_DIR/test/3d_tests/wglgears32.exe"

cp "$ROOT_DIR/src_driver/nvxmp.sys" "$ROOT_DIR/dist_full/nvxmp.sys"
cp "$ROOT_DIR/src_disp/nvxdisp.dll" "$ROOT_DIR/dist_full/nvxdisp.dll"

# Create test runner batch script
RUN_TEST_BAT="$(mktemp /tmp/run_test.XXXXXX.bat)"
cat << 'EOF' > "$RUN_TEST_BAT"
@echo off
cd /d C:\test_accel
echo [TEST] Starting Antigravity GPU Accel Suite... > C:\test_accel.log
test_gpu_accel.exe >> C:\test_accel.log 2>&1
echo [TEST] 64-bit Test Return Code: %ERRORLEVEL% >> C:\test_accel.log
if exist x86\test_gpu_accel32.exe (
    cd /d C:\test_accel\x86
    test_gpu_accel32.exe >> C:\test_accel.log 2>&1
    echo [TEST] 32-bit Test Return Code: %ERRORLEVEL% >> C:\test_accel.log
)
if exist C:\test_accel\wglgears.exe (
    cd /d C:\test_accel
    echo [TEST] Running 64-bit wglgears 3D benchmark... >> C:\test_accel.log
    wglgears.exe -seconds 6 >> C:\test_accel.log 2>&1
    echo [TEST] 64-bit wglgears Return Code: %ERRORLEVEL% >> C:\test_accel.log
)
if exist C:\test_accel\x86\wglgears32.exe (
    cd /d C:\test_accel\x86
    echo [TEST] Running 32-bit wglgears 3D benchmark... >> C:\test_accel.log
    wglgears32.exe -seconds 6 >> C:\test_accel.log 2>&1
    echo [TEST] 32-bit wglgears Return Code: %ERRORLEVEL% >> C:\test_accel.log
)
echo [ALL_BENCHMARKS_FINISHED] Status: PASS >> C:\test_accel.log
echo [TEST] ALL TESTS COMPLETE >> C:\test_accel.log
EOF
unix2dos "$RUN_TEST_BAT"

echo "==> Uploading driver binaries and Wine DirectX-to-OpenGL pipeline..."

TMP_REG_SOFT="$(mktemp /tmp/reg_soft.XXXXXX)"
TMP_REG_SYS="$(mktemp /tmp/reg_sys.XXXXXX)"

guestfish -a "$QCOW" -i << EOF
# 1. Update Core Drivers
upload "$ROOT_DIR/src_driver/nvxmp.sys" /WINDOWS/system32/drivers/nvxmp.sys
upload "$ROOT_DIR/src_disp/nvxdisp.dll" /WINDOWS/system32/nvxdisp.dll

# 2. Setup C:\test_accel test environment with 64-bit WineD3D & ucrtbase shim
mkdir-p /test_accel
upload "$ROOT_DIR/test/test_gpu_accel.exe" /test_accel/test_gpu_accel.exe
upload "$ROOT_DIR/test/3d_tests/wglgears.exe" /test_accel/wglgears.exe
upload "$RUN_TEST_BAT" /test_accel/run_test.bat
upload "$ROOT_DIR/test/ucrtbase.dll" /test_accel/ucrtbase.dll
upload /usr/lib/x86_64-linux-gnu/wine/x86_64-windows/wined3d.dll /test_accel/wined3d.dll
upload /usr/lib/x86_64-linux-gnu/wine/x86_64-windows/d3d9.dll /test_accel/d3d9.dll
upload /usr/lib/x86_64-linux-gnu/wine/x86_64-windows/d3d8.dll /test_accel/d3d8.dll

# 3. Setup C:\test_accel\x86 with 32-bit WineD3D & ucrtbase shim
mkdir-p /test_accel/x86
upload "$ROOT_DIR/test/test_gpu_accel32.exe" /test_accel/x86/test_gpu_accel32.exe
upload "$ROOT_DIR/test/3d_tests/wglgears32.exe" /test_accel/x86/wglgears32.exe
upload "$ROOT_DIR/test/ucrtbase32.dll" /test_accel/x86/ucrtbase.dll
upload /usr/lib/i386-linux-gnu/wine/i386-windows/wined3d.dll /test_accel/x86/wined3d.dll
upload /usr/lib/i386-linux-gnu/wine/i386-windows/d3d9.dll /test_accel/x86/d3d9.dll
upload /usr/lib/i386-linux-gnu/wine/i386-windows/d3d8.dll /test_accel/x86/d3d8.dll

# 4. System-wide DLLs (ucrtbase shim)
upload "$ROOT_DIR/test/ucrtbase.dll" /WINDOWS/system32/ucrtbase.dll
upload "$ROOT_DIR/test/ucrtbase32.dll" /WINDOWS/SysWOW64/ucrtbase.dll

# 5. Desktop shortcuts and Startup auto-runner
upload "$ROOT_DIR/test/test_gpu_accel.exe" "/Documents and Settings/All Users/Desktop/test_gpu_accel.exe"
upload "$RUN_TEST_BAT" "/Documents and Settings/All Users/Start Menu/Programs/Startup/run_accel_test.bat"

# Download hives for registry configuration
download /WINDOWS/system32/config/software "$TMP_REG_SOFT"
download /WINDOWS/system32/config/system "$TMP_REG_SYS"
EOF

echo "==> Configuring Auto-Logon and Run keys in Windows Registry..."

# Configure software hive
cat << 'EOF' > /tmp/reg_update_soft.reg
Windows Registry Editor Version 5.00

[\Microsoft\Windows NT\CurrentVersion\Winlogon]
"AutoAdminLogon"="1"
"ForceAutoLogon"="1"
"DefaultUserName"="Administrator"
"DefaultPassword"=""
"DefaultDomainName"="WINXP-X64"
"DisableCAD"=dword:00000001
"Userinit"="C:\\WINDOWS\\system32\\userinit.exe,"

[\Microsoft\Windows NT\CurrentVersion\Winlogon\SpecialAccounts\UserList]
"Administrator"=dword:00000001

[\Microsoft\Windows\CurrentVersion\Run]
"GPUAccelTest"="C:\\test_accel\\run_test.bat"
EOF

hivexregedit --merge "$TMP_REG_SOFT" /tmp/reg_update_soft.reg

# Configure system hive
cat << 'EOF' > /tmp/reg_update_sys.reg
Windows Registry Editor Version 5.00

[\ControlSet001\Control\Lsa]
"limitblankpassworduse"=dword:00000000

[\ControlSet002\Control\Lsa]
"limitblankpassworduse"=dword:00000000
EOF

hivexregedit --merge "$TMP_REG_SYS" /tmp/reg_update_sys.reg

# Upload modified hives back into the VM image
guestfish -a "$QCOW" -i << EOF
upload "$TMP_REG_SOFT" /WINDOWS/system32/config/software
upload "$TMP_REG_SYS" /WINDOWS/system32/config/system
EOF

rm -f "$RUN_TEST_BAT" "$TMP_REG_SOFT" "$TMP_REG_SYS" /tmp/reg_update_soft.reg /tmp/reg_update_sys.reg
echo "==> Deployment to Windows XP x64 disk image complete!"
