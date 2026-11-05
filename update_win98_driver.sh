#!/usr/bin/env bash
set -euo pipefail

NVX_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WIN98_IMG="$NVX_ROOT/win98_small.img"
PKG_DIR="$NVX_ROOT/dist_win98"

if [ ! -f "$WIN98_IMG" ]; then
    echo "Error: $WIN98_IMG not found!" >&2
    exit 1
fi

for f in "$PKG_DIR/nvx.inf" "$PKG_DIR/nvx.drv" "$PKG_DIR/nvx.vxd"; do
    if [ ! -f "$f" ]; then
        echo "Error: missing required package file $f" >&2
        exit 1
    fi
done

echo "==> Deploying NVX Windows 98 SE driver package to $WIN98_IMG"

guestfish --rw -a "$WIN98_IMG" -m /dev/sda1 <<EOF
mkdir-p /WINDOWS/INF
mkdir-p /WINDOWS/SYSTEM
mkdir-p /DRIVERS/NVX
upload "$PKG_DIR/nvx.inf" /WINDOWS/INF/nvx.inf
upload "$PKG_DIR/nvx.inf" /DRIVERS/NVX/nvx.inf
upload "$PKG_DIR/nvx.drv" /WINDOWS/SYSTEM/nvx.drv
upload "$PKG_DIR/nvx.drv" /DRIVERS/NVX/nvx.drv
upload "$PKG_DIR/nvx.vxd" /WINDOWS/SYSTEM/nvx.vxd
upload "$PKG_DIR/nvx.vxd" /DRIVERS/NVX/nvx.vxd
sync
EOF

echo "==> Driver files successfully installed to Windows 98 disk image."

