#!/bin/bash
set -e

if [ "$EUID" -ne 0 ]; then
  echo "Please run as root: sudo $0"
  exit 1
fi

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QCOW="$DIR/winxp_x64.qcow2"
PACKAGE_DIR="$DIR/dist"
if [ "${1:-}" = "full" ]; then
  PACKAGE_DIR="$DIR/dist_full"
elif [ "$#" -ne 0 ]; then
  echo "Usage: sudo $0 [full]"
  exit 1
fi
SRC_SYS="$PACKAGE_DIR/nvxmp.sys"
SRC_DISP="$PACKAGE_DIR/nvxdisp.dll"
GPUZ="$DIR/GPU-Z.2.71.0.exe"
MNT="/tmp/winxp_qcow_mnt"

cleanup() {
  sync
  echo "==> Cleaning up mounts and NBD devices..."
  if mountpoint -q "$MNT"; then
    umount "$MNT" || true
  fi
  qemu-nbd --disconnect /dev/nbd0 2>/dev/null || true
  rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

if [ ! -f "$SRC_SYS" ] || [ ! -f "$SRC_DISP" ]; then
  echo "Error: Driver binaries missing from $PACKAGE_DIR! Build them first."
  exit 1
fi

echo "==> Selected driver package: $PACKAGE_DIR"

echo "==> Loading NBD module..."
modprobe nbd max_part=8

if ! mountpoint -q "$MNT"; then
  # Ensure clean nbd state
  qemu-nbd --disconnect /dev/nbd0 2>/dev/null || true
  sleep 1

  echo "==> Connecting $QCOW to /dev/nbd0..."
  qemu-nbd --connect=/dev/nbd0 "$QCOW"
  sleep 1

  DEV="/dev/nbd0p1"
  if [ ! -b "$DEV" ]; then
    DEV="/dev/nbd0"
  fi

  mkdir -p "$MNT"
  echo "==> Mounting $DEV to $MNT..."
  mount "$DEV" "$MNT"
fi

echo "==> Locating Windows directories..."
SYS32_DIR=$(find "$MNT" -maxdepth 2 -ipath "$MNT/windows/system32" | head -n 1)
DRV_DIR=$(find "$MNT" -maxdepth 3 -ipath "$MNT/windows/system32/drivers" | head -n 1)

if [ -z "$SYS32_DIR" ] || [ -z "$DRV_DIR" ]; then
  echo "Error: Could not find Windows/system32 on the disk!"
  exit 1
fi

echo "==> Replacing miniport driver (nvxmp.sys)..."
cp -v "$SRC_SYS" "$DRV_DIR/nvxmp.sys"

echo "==> Replacing display driver (nvxdisp.dll)..."
cp -v "$SRC_DISP" "$SYS32_DIR/nvxdisp.dll"

if [ -f "$GPUZ" ]; then
  echo "==> Copying GPU-Z to Windows Desktop directories..."
  find "$MNT" -maxdepth 3 -ipath "*/Desktop" -type d -print0 | while IFS= read -r -d '' dt; do
    echo "Copying GPU-Z to: $dt"
    cp -v "$GPUZ" "$dt/" || true
  done
fi

# Update any backup copies (LastGood, dllcache) if present
find "$MNT" \( -iname "nvxdisp.dll" -o -iname "nvxmp.sys" \) -print0 | while IFS= read -r -d '' bak; do
  if [[ "$bak" == *nvxdisp.dll ]]; then
    echo "Updating backup: $bak"
    cp "$SRC_DISP" "$bak"
  elif [[ "$bak" == *nvxmp.sys ]]; then
    echo "Updating backup: $bak"
    cp "$SRC_SYS" "$bak"
  fi
done

sync
echo "==> SUCCESS! Disk image updated cleanly."
