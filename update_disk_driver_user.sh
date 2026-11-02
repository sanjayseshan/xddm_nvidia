#!/usr/bin/env bash
set -euo pipefail

NVX_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NVX_QCOW="$NVX_ROOT/winxp_x64.qcow2"
NVX_PACKAGE="$NVX_ROOT/dist"
if [ "${1:-}" = full ]; then
    NVX_PACKAGE="$NVX_ROOT/dist_full"
elif [ "$#" -ne 0 ]; then
    echo "Usage: $0 [full]" >&2
    exit 1
fi

for tool in guestmount guestunmount mountpoint; do
    command -v "$tool" >/dev/null || { echo "Missing $tool; see ROOTLESS_TESTING.md." >&2; exit 1; }
done
if pgrep -f "qemu-system.*${NVX_QCOW}" >/dev/null; then
    echo "Refusing to update a disk used by a running VM." >&2
    exit 1
fi
for file in "$NVX_PACKAGE/nvxmp.sys" "$NVX_PACKAGE/nvxdisp.dll"; do
    [ -f "$file" ] || { echo "Missing package file: $file" >&2; exit 1; }
done

NVX_MOUNT="$(mktemp -d /tmp/nvx-xp-mount.XXXXXX)"
cleanup() {
    if mountpoint -q "$NVX_MOUNT"; then guestunmount "$NVX_MOUNT" || true; fi
    rmdir "$NVX_MOUNT" 2>/dev/null || true
}
trap cleanup EXIT

echo "==> Mounting XP image with unprivileged libguestfs/FUSE"
guestmount --rw -a "$NVX_QCOW" -i "$NVX_MOUNT"
NVX_SYSTEM32="$(find "$NVX_MOUNT" -maxdepth 2 -type d -ipath '*/windows/system32' -print -quit)"
NVX_DRIVERS="$(find "$NVX_MOUNT" -maxdepth 3 -type d -ipath '*/windows/system32/drivers' -print -quit)"
if [ -z "$NVX_SYSTEM32" ] || [ -z "$NVX_DRIVERS" ]; then
    echo "Could not locate the Windows system directories." >&2
    exit 1
fi

cp -v "$NVX_PACKAGE/nvxmp.sys" "$NVX_DRIVERS/nvxmp.sys"
cp -v "$NVX_PACKAGE/nvxdisp.dll" "$NVX_SYSTEM32/nvxdisp.dll"
sync
guestunmount "$NVX_MOUNT"
echo "==> XP image updated without root"
