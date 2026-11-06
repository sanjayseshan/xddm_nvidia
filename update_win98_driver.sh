#!/usr/bin/env bash
set -euo pipefail

NVX_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WIN98_IMG="$NVX_ROOT/win98_small.img"
PKG_DIR="$NVX_ROOT/dist_win98"

if [ ! -f "$WIN98_IMG" ]; then
    echo "Error: $WIN98_IMG not found!" >&2
    exit 1
fi

for f in "$PKG_DIR/nvx.inf" "$PKG_DIR/nvx.drv" "$PKG_DIR/nvx2.drv" "$PKG_DIR/nvx.vxd"; do
    if [ ! -f "$f" ]; then
        echo "Error: missing required package file $f" >&2
        exit 1
    fi
done

echo "==> Deploying NVX Windows 98 SE dual-head driver package to $WIN98_IMG"

TMP_SYS_INI="/tmp/win98_system_ini.tmp"
guestfish --ro -a "$WIN98_IMG" -m /dev/sda1 download /WINDOWS/SYSTEM.INI "$TMP_SYS_INI"

# Ensure [WINVBE] configuration is in SYSTEM.INI
if ! grep -q "\[WINVBE\]" "$TMP_SYS_INI"; then
    echo "Adding [WINVBE] configuration to SYSTEM.INI..."
    cat >> "$TMP_SYS_INI" <<'SYSINIBLOCK'

[WINVBE]
LfbAddr=0xE0000000
LfbCheck=0
Debug=2
DebugPort=0x3F8
Access=1
SYSINIBLOCK
fi

# Ensure [WINVBE2] configuration is in SYSTEM.INI
if ! grep -q "\[WINVBE2\]" "$TMP_SYS_INI"; then
    echo "Adding [WINVBE2] configuration to SYSTEM.INI..."
    cat >> "$TMP_SYS_INI" <<'SYSINIBLOCK2'

[WINVBE2]
LfbAddr=0xE2000000
LfbCheck=0
Debug=2
DebugPort=0x3F8
Access=1
Width=2560
Height=1440
Bpp=32
SYSINIBLOCK2
fi

# Ensure display.drv=nvx.drv in [boot]
sed -i 's/^display\.drv=.*/display.drv=nvx.drv/' "$TMP_SYS_INI"

guestfish --rw -a "$WIN98_IMG" -m /dev/sda1 <<EOF
mkdir-p /WINDOWS/INF
mkdir-p /WINDOWS/SYSTEM
mkdir-p /DRIVERS/NVX
upload "$PKG_DIR/nvx.inf" /WINDOWS/INF/nvx.inf
upload "$PKG_DIR/nvx.inf" /DRIVERS/NVX/nvx.inf
upload "$PKG_DIR/nvx.drv" /WINDOWS/SYSTEM/nvx.drv
upload "$PKG_DIR/nvx.drv" /DRIVERS/NVX/nvx.drv
upload "$PKG_DIR/nvx2.drv" /WINDOWS/SYSTEM/nvx2.drv
upload "$PKG_DIR/nvx2.drv" /DRIVERS/NVX/nvx2.drv
upload "$PKG_DIR/nvx.vxd" /WINDOWS/SYSTEM/nvx.vxd
upload "$PKG_DIR/nvx.vxd" /DRIVERS/NVX/nvx.vxd
upload "$NVX_ROOT/src_win98/nvx_reg.reg" /NVX_REG.REG
upload "$TMP_SYS_INI" /WINDOWS/SYSTEM.INI
sync
EOF

rm -f "$TMP_SYS_INI"

echo "==> Dual-head driver files successfully installed to Windows 98 disk image."
