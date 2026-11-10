#!/usr/bin/env bash
set -euo pipefail

NVX_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NVX_QCOW="$NVX_ROOT/winxp_x64.qcow2"
NVX_PACKAGE="$NVX_ROOT/dist"
NVX_OPENGL_ICD_DLL="${NVX_OPENGL_ICD_DLL:-nvxogl64.dll}"
NVX_MESA_APPLOCAL="${NVX_MESA_APPLOCAL:-0}"
if [ "${1:-}" = full ]; then
    NVX_PACKAGE="$NVX_ROOT/dist_full"
elif [ "$#" -ne 0 ]; then
    echo "Usage: $0 [full]" >&2
    exit 1
fi
case "$NVX_OPENGL_ICD_DLL" in
    nvxogl64.dll|nvxmesa.dll) ;;
    *) echo "Unsupported NVX_OPENGL_ICD_DLL: $NVX_OPENGL_ICD_DLL" >&2; exit 1 ;;
esac
case "$NVX_MESA_APPLOCAL" in
    0|1) ;;
    *) echo "NVX_MESA_APPLOCAL must be 0 or 1" >&2; exit 1 ;;
esac

for tool in guestmount guestunmount mountpoint qemu-img; do
    command -v "$tool" >/dev/null || { echo "Missing $tool; see ROOTLESS_TESTING.md." >&2; exit 1; }
done
if pgrep -f "qemu-system.*${NVX_QCOW}" >/dev/null; then
    echo "Refusing to update a disk used by a running VM." >&2
    exit 1
fi
# QEMU may use a relative image path or be hidden from pgrep's namespace.
# qemu-img checks the actual image lock before starting a writable appliance.
if ! qemu-img info "$NVX_QCOW" >/dev/null; then
    echo "Refusing to update a locked or unreadable XP disk." >&2
    exit 1
fi
for file in "$NVX_PACKAGE/nvxmp.sys" "$NVX_PACKAGE/nvxdisp.dll"; do
    [ -f "$file" ] || { echo "Missing package file: $file" >&2; exit 1; }
done

NVX_MOUNT="$(mktemp -d /tmp/nvx-xp-mount.XXXXXX)"
NVX_GUESTMOUNT_PID="${NVX_MOUNT}.pid"
wait_for_release() {
    local pid=""
    [ -f "$NVX_GUESTMOUNT_PID" ] && pid="$(cat "$NVX_GUESTMOUNT_PID")"
    if [ -n "$pid" ]; then
        for _ in $(seq 1 100); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$pid" 2>/dev/null; then
            echo "guestmount process $pid did not exit; refusing to launch QEMU." >&2
            return 1
        fi
    fi
    for _ in $(seq 1 100); do
        qemu-img info "$NVX_QCOW" >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    echo "QCOW write lock was not released; refusing to launch QEMU." >&2
    return 1
}
cleanup() {
    if mountpoint -q "$NVX_MOUNT"; then guestunmount --retry=10 "$NVX_MOUNT" || true; fi
    wait_for_release || true
    rm -f "$NVX_GUESTMOUNT_PID"
    rmdir "$NVX_MOUNT" 2>/dev/null || true
}
trap cleanup EXIT

echo "==> Mounting XP image with unprivileged libguestfs/FUSE"
guestmount --rw --pid-file "$NVX_GUESTMOUNT_PID" -a "$NVX_QCOW" -i "$NVX_MOUNT"
NVX_SYSTEM32="$(find "$NVX_MOUNT" -maxdepth 2 -type d -ipath '*/windows/system32' -print -quit)"
NVX_DRIVERS="$(find "$NVX_MOUNT" -maxdepth 3 -type d -ipath '*/windows/system32/drivers' -print -quit)"
if [ -z "$NVX_SYSTEM32" ] || [ -z "$NVX_DRIVERS" ]; then
    echo "Could not locate the Windows system directories." >&2
    exit 1
fi
echo "==> Auditing every package import against this XP installation"
NVX_AUDIT_ARGS=(--package "$NVX_PACKAGE" "$NVX_SYSTEM32" "$NVX_DRIVERS")
if [ -f "$NVX_ROOT/test/test_gpu_accel32.exe" ] || \
   [ -f "$NVX_ROOT/test/3d_tests/wglgears32.exe" ]; then
    NVX_AUDIT_ARGS+=(--x86-system-dir "$NVX_MOUNT/WINDOWS/SysWOW64")
fi
if [ -f "$NVX_ROOT/test/test_gpu_accel32.exe" ]; then
    NVX_AUDIT_ARGS+=(--package "$NVX_ROOT/test/test_gpu_accel32.exe")
fi
if [ -f "$NVX_ROOT/test/3d_tests/wglgears32.exe" ]; then
    NVX_AUDIT_ARGS+=(--package "$NVX_ROOT/test/3d_tests/wglgears32.exe")
fi
python3 "$NVX_ROOT/test/audit_xp_imports.py" "${NVX_AUDIT_ARGS[@]}"

cp -v "$NVX_PACKAGE/nvxmp.sys" "$NVX_DRIVERS/nvxmp.sys"
cp -v "$NVX_PACKAGE/nvxdisp.dll" "$NVX_SYSTEM32/nvxdisp.dll"
if [ -f "$NVX_PACKAGE/nvxogl64.dll" ]; then
    cp -v "$NVX_PACKAGE/nvxogl64.dll" "$NVX_SYSTEM32/nvxogl64.dll"
    NVX_SOFTWARE_HIVE="$(find "$NVX_MOUNT" -maxdepth 4 -type f -ipath '*/windows/system32/config/software' -print -quit)"
    if [ -z "$NVX_SOFTWARE_HIVE" ]; then
        echo "Could not locate the offline SOFTWARE registry hive for the Mesa ICD." >&2
        exit 1
    fi
hivexregedit --merge --prefix 'HKEY_LOCAL_MACHINE\SOFTWARE' "$NVX_SOFTWARE_HIVE" <<REGFILE
Windows Registry Editor Version 5.00

[HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\nvxdisp]
"Dll"="$NVX_OPENGL_ICD_DLL"
"Version"=dword:00000002
"DriverVersion"=dword:00010000
"Flags"=dword:00000001
REGFILE
fi
if [ -f "$NVX_PACKAGE/nvxmesa.dll" ]; then
    mkdir -p "$NVX_MOUNT/test_accel"
    cp -v "$NVX_PACKAGE/nvxmesa.dll" "$NVX_MOUNT/test_accel/nvxmesa.dll"
    if [ "$NVX_MESA_APPLOCAL" = "1" ]; then
        cp -v "$NVX_PACKAGE/nvxmesa.dll" "$NVX_MOUNT/test_accel/opengl32.dll"
    else
        rm -f "$NVX_MOUNT/test_accel/opengl32.dll"
    fi
fi
if [ -f "$NVX_PACKAGE/test_gpu_accel.exe" ]; then
    mkdir -p "$NVX_MOUNT/test_accel"
    cp -v "$NVX_PACKAGE/test_gpu_accel.exe" "$NVX_MOUNT/test_accel/test_gpu_accel.exe"
fi
sync
guestunmount --retry=10 "$NVX_MOUNT"
wait_for_release
rm -f "$NVX_GUESTMOUNT_PID"
echo "==> XP image updated without root"
