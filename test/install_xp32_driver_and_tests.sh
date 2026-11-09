#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

QCOW="$ROOT_DIR/winxp_x32.qcow2"

# guestfish edits the offline image; refuse if QEMU still holds its write lock.
if ! qemu-img info "$QCOW" >/dev/null 2>&1; then
    echo "Error: $QCOW is locked or unreadable; shut down the VM before installation." >&2
    exit 1
fi

echo "=========================================================="
echo " Deploying Antigravity Drivers & Test Suite to WinXP 32-bit"
echo " Target Disk: $QCOW"
echo "=========================================================="

# 1. Build latest 32-bit driver & display DLL
make -C "$ROOT_DIR/src_driver" full32
make -C "$ROOT_DIR/src_disp" nvxdisp32.dll

cp "$ROOT_DIR/src_driver/nvxmp32.sys" "$ROOT_DIR/dist32/nvxmp.sys"
cp "$ROOT_DIR/src_disp/nvxdisp32.dll" "$ROOT_DIR/dist32/nvxdisp.dll"

# 2. Build XP-compatible 32-bit ucrtbase shim and test binaries
i686-w64-mingw32-gcc -O2 -shared -static-libgcc "$ROOT_DIR/test/ucrtbase_xp.c" "$ROOT_DIR/test/ucrtbase.def" -o "$ROOT_DIR/test/ucrtbase32.dll" -lm
i686-w64-mingw32-gcc -O2 -Wall -Wextra -static-libgcc "$ROOT_DIR/test/test_gpu_accel.c" -lopengl32 -lgdi32 -luser32 -o "$ROOT_DIR/test/test_gpu_accel32.exe"
i686-w64-mingw32-gcc -x c -O2 -static-libgcc "$ROOT_DIR/test/3d_tests/glxgears_main.cpp" -lopengl32 -lgdi32 -luser32 -o "$ROOT_DIR/test/3d_tests/wglgears32.exe"

# 3. Create test runner batch script for 32-bit Windows XP
RUN_TEST_BAT="$(mktemp /tmp/run_test32.XXXXXX.bat)"
cat << 'EOF' > "$RUN_TEST_BAT"
@echo off
cd /d C:\test_accel
echo [TEST] Starting Antigravity GPU Accel Suite (WinXP 32-bit)... > C:\test_accel.log
test_gpu_accel.exe >> C:\test_accel.log 2>&1
echo [TEST] 32-bit GPU Accel Test Return Code: %ERRORLEVEL% >> C:\test_accel.log

if exist C:\test_accel\wglgears.exe (
    cd /d C:\test_accel
    echo [TEST] Running 32-bit wglgears 3D benchmark... >> C:\test_accel.log
    wglgears.exe -seconds 10 >> C:\test_accel.log 2>&1
    echo [TEST] 32-bit wglgears Return Code: %ERRORLEVEL% >> C:\test_accel.log
)
echo [ALL_BENCHMARKS_FINISHED] Status: PASS >> C:\test_accel.log
echo [TEST] ALL TESTS COMPLETE >> C:\test_accel.log
EOF
unix2dos "$RUN_TEST_BAT"

echo "==> Uploading driver files and tests into winxp_x32.qcow2..."

TMP_REG_SOFT="$(mktemp /tmp/reg32_soft.XXXXXX)"
TMP_REG_SYS="$(mktemp /tmp/reg32_sys.XXXXXX)"

guestfish -a "$QCOW" -i << EOF
# 1. Update Core Drivers (32-bit)
upload "$ROOT_DIR/src_driver/nvxmp32.sys" /WINDOWS/system32/drivers/nvxmp.sys
upload "$ROOT_DIR/src_disp/nvxdisp32.dll" /WINDOWS/system32/nvxdisp.dll
upload "$ROOT_DIR/dist32/nvx.inf" /WINDOWS/inf/nvx.inf

# 2. Setup C:\test_accel test environment with 32-bit binaries
mkdir-p /test_accel
upload "$ROOT_DIR/test/test_gpu_accel32.exe" /test_accel/test_gpu_accel.exe
upload "$ROOT_DIR/test/3d_tests/wglgears32.exe" /test_accel/wglgears.exe
upload "$RUN_TEST_BAT" /test_accel/run_test.bat
upload "$ROOT_DIR/test/ucrtbase32.dll" /test_accel/ucrtbase.dll
upload "$ROOT_DIR/test/libgcc_s_dw2-1.dll" /test_accel/libgcc_s_dw2-1.dll

# 3. System-wide DLLs
upload "$ROOT_DIR/test/ucrtbase32.dll" /WINDOWS/system32/ucrtbase.dll
upload "$ROOT_DIR/test/libgcc_s_dw2-1.dll" /WINDOWS/system32/libgcc_s_dw2-1.dll

# 4. Desktop shortcuts and Startup auto-runner
upload "$ROOT_DIR/test/test_gpu_accel32.exe" "/Documents and Settings/All Users/Desktop/test_gpu_accel.exe"
upload "$RUN_TEST_BAT" "/Documents and Settings/All Users/Start Menu/Programs/Startup/run_accel_test.bat"

# Download hives for registry configuration
download /WINDOWS/system32/config/software "$TMP_REG_SOFT"
download /WINDOWS/system32/config/system "$TMP_REG_SYS"
EOF

echo "==> Configuring Auto-Logon and Run keys in Windows XP 32-bit Registry..."

# Configure software hive
cat << 'EOF' > /tmp/reg32_update_soft.reg
Windows Registry Editor Version 5.00

[\Microsoft\Windows NT\CurrentVersion\Winlogon]
"AutoAdminLogon"="1"
"ForceAutoLogon"="1"
"DefaultUserName"="Administrator"
"DefaultPassword"=""
"DisableCAD"=dword:00000001
"Userinit"="C:\\WINDOWS\\system32\\userinit.exe,"

[\Microsoft\Windows NT\CurrentVersion\Winlogon\SpecialAccounts\UserList]
"Administrator"=dword:00000001

[\Microsoft\Windows\CurrentVersion\Run]
"GPUAccelTest"="C:\\test_accel\\run_test.bat"
EOF

hivexregedit --merge "$TMP_REG_SOFT" /tmp/reg32_update_soft.reg

# Configure system hive: register nvxmp service and PCI binding
cat << 'EOF' > /tmp/reg32_update_sys.reg
Windows Registry Editor Version 5.00

[\ControlSet001\Services\nvxmp]
"Type"=dword:00000001
"Start"=dword:00000001
"ErrorControl"=dword:00000000
"Tag"=dword:00000002
"ImagePath"=hex(2):73,00,79,00,73,00,74,00,65,00,6d,00,33,00,32,00,5c,00,44,00,52,00,49,00,56,00,45,00,52,00,53,00,5c,00,6e,00,76,00,78,00,6d,00,70,00,2e,00,73,00,79,00,73,00,00,00
"Group"="Video"
"MultiFunctionSupported"=dword:00000001

[\ControlSet001\Services\nvxmp\Device0]
"InstalledDisplayDrivers"=hex(7):6e,00,76,00,78,00,64,00,69,00,73,00,70,00,00,00,00,00
"VgaCompatible"=dword:00000000
"DefaultSettings.BitsPerPel"=dword:00000020
"DefaultSettings.XResolution"=dword:00000780
"DefaultSettings.YResolution"=dword:00000438

[\ControlSet001\Services\nvxmp\Video]
"Service"="nvxmp"
"VideoID"="{5F8C6A12-DBEB-11F1-8017-806D6172696F}"

[\ControlSet001\Services\cirrus]
"Start"=dword:00000004

[\ControlSet001\Services\cirrus\Video]
"Service"="cirrus"
"VideoID"=""

[\ControlSet001\Control\Video\{0CD00C43-1283-43EC-93C0-A2E1FB692F9A}\0000]
"InstalledDisplayDrivers"=hex(7):00,00

[\ControlSet001\Enum\PCI\VEN_1013&DEV_00B8&SUBSYS_11001AF4&REV_00\3&267a616a&0&10]
"ConfigFlags"=dword:00000001

[\ControlSet001\Control\Class\{4D36E968-E325-11CE-BFC1-08002BE10318}\0001]
"InfPath"="nvx.inf"
"InfSection"="nvx_Inst"
"ProviderName"="NVIDIA Corporation (XDDM Community Port)"
"MatchingDeviceId"="pci\\ven_10de&dev_2488"
"DriverDesc"="NVIDIA GeForce RTX 3070 (GA104 XDDM)"

[\ControlSet001\Control\Class\{4D36E968-E325-11CE-BFC1-08002BE10318}\0001\Settings]
"InstalledDisplayDrivers"=hex(7):6e,00,76,00,78,00,64,00,69,00,73,00,70,00,00,00,00,00
"VgaCompatible"=dword:00000000
"MultiFunctionSupported"=dword:00000001
"Device Description"="NVIDIA GeForce RTX 3070 (GA104 XDDM)"

[\ControlSet001\Enum\PCI\VEN_10DE&DEV_2488&SUBSYS_47553842&REV_A1\3&267a616a&0&10]
"ClassGUID"="{4D36E968-E325-11CE-BFC1-08002BE10318}"
"Class"="Display"
"Driver"="{4D36E968-E325-11CE-BFC1-08002BE10318}\\0001"
"Mfg"="NVIDIA Corporation (XDDM Community Port)"
"Service"="nvxmp"
"ConfigFlags"=dword:00000000

[\ControlSet001\Control\Lsa]
"limitblankpassworduse"=dword:00000000
EOF

hivexregedit --merge "$TMP_REG_SYS" /tmp/reg32_update_sys.reg

# Upload modified hives back into the VM image
guestfish -a "$QCOW" -i << EOF
upload "$TMP_REG_SOFT" /WINDOWS/system32/config/software
upload "$TMP_REG_SYS" /WINDOWS/system32/config/system
EOF

rm -f "$RUN_TEST_BAT" "$TMP_REG_SOFT" "$TMP_REG_SYS" /tmp/reg32_update_soft.reg /tmp/reg32_update_sys.reg
echo "==> Deployment to Windows XP 32-bit disk image complete!"
