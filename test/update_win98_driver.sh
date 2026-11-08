#!/usr/bin/env bash
set -euo pipefail

NVX_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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
dos2unix "$TMP_SYS_INI"

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
DibEngine=1
ShadowBuffer=1
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
DibEngine=1
ShadowBuffer=1
SYSINIBLOCK2
fi

# Ensure display.drv=nvx.drv in [boot] and remove any broken *vdd override
sed -i 's/^display\.drv=.*/display.drv=nvx.drv/' "$TMP_SYS_INI"
sed -i '/\*vdd=/d' "$TMP_SYS_INI"

# Ensure mouse, keyboard, and stability limits in [386Enh]
sed -i '/NoMouseWarning=1/d' "$TMP_SYS_INI"
sed -i 's/^mouse=.*/mouse=*vmouse/' "$TMP_SYS_INI"
if ! grep -qi "^mouse=" "$TMP_SYS_INI"; then
    sed -i '/\[386Enh\]/a mouse=*vmouse' "$TMP_SYS_INI"
fi
if ! grep -qi "keyboard=\*vkd" "$TMP_SYS_INI"; then
    sed -i '/\[386Enh\]/a keyboard=*vkd' "$TMP_SYS_INI"
fi
if ! grep -qi "32BitDiskAccess=" "$TMP_SYS_INI"; then
    sed -i '/\[386Enh\]/a 32BitDiskAccess=Off' "$TMP_SYS_INI"
fi
if ! grep -qi "cr0wpoff\.vxd" "$TMP_SYS_INI"; then
    sed -i '/\[386Enh\]/a device=cr0wpoff.vxd' "$TMP_SYS_INI"
fi
if ! grep -qi "FastA20=" "$TMP_SYS_INI"; then
    sed -i '/\[386Enh\]/a FastA20=1\nA20Mode=1' "$TMP_SYS_INI"
fi
sed -i '/MaxPhysPage=/d' "$TMP_SYS_INI"
sed -i '/\[386Enh\]/a MaxPhysPage=30000' "$TMP_SYS_INI"
sed -i '/MaxFileCache=/d' "$TMP_SYS_INI"
sed -i '/\[386Enh\]/a MaxFileCache=524288' "$TMP_SYS_INI"
sed -i '/device=nvx\.vxd/d' "$TMP_SYS_INI"
sed -i '/display=/d' "$TMP_SYS_INI"
sed -i '/\[386Enh\]/a display=*vdd,*vflatd' "$TMP_SYS_INI"
sed -i '/minivdd=/d' "$TMP_SYS_INI"
sed -i '/\[386Enh\]/a minivdd=nvx.vxd' "$TMP_SYS_INI"
sed -i '/\[386Enh\]/a device=nvx.vxd' "$TMP_SYS_INI"
sed -i 's/^Debug=.*/Debug=2/' "$TMP_SYS_INI"
sed -i '/DibEngine=/d; /ShadowBuffer=/d' "$TMP_SYS_INI"
sed -i '/\[WINVBE\]/a DibEngine=1\nShadowBuffer=1' "$TMP_SYS_INI"
sed -i '/\[WINVBE2\]/a DibEngine=1\nShadowBuffer=1' "$TMP_SYS_INI"

unix2dos "$TMP_SYS_INI"

TMP_MSDOS_SYS="/tmp/win98_msdos_sys.tmp"
guestfish --ro -a "$WIN98_IMG" -m /dev/sda1 download /MSDOS.SYS "$TMP_MSDOS_SYS"
dos2unix "$TMP_MSDOS_SYS"
sed -i 's/^BootGUI=.*/BootGUI=1/' "$TMP_MSDOS_SYS"
sed -i 's/^BootMenu=.*/BootMenu=0/' "$TMP_MSDOS_SYS"
sed -i 's/^Logo=.*/Logo=0/' "$TMP_MSDOS_SYS"
unix2dos "$TMP_MSDOS_SYS"

TMP_CONFIG_SYS="/tmp/win98_config_sys.tmp"
cat << 'EOF' > "$TMP_CONFIG_SYS"
SHELL=C:\WINDOWS\COMMAND.COM C:\WINDOWS /P
DOS=HIGH,UMB
DEVICE=C:\WINDOWS\HIMEM.SYS /TESTMEM:OFF
EOF
unix2dos "$TMP_CONFIG_SYS"

TMP_WIN_INI="/tmp/win98_win_ini.tmp"
guestfish --ro -a "$WIN98_IMG" -m /dev/sda1 download /WINDOWS/WIN.INI "$TMP_WIN_INI"
dos2unix "$TMP_WIN_INI"
sed -i 's|^run=.*|run=C:\\WINDOWS\\ATTACH_SEC.EXE|' "$TMP_WIN_INI"
unix2dos "$TMP_WIN_INI"

guestfish --rw -a "$WIN98_IMG" -m /dev/sda1 <<EOF
mkdir-p /WINDOWS/INF
mkdir-p /WINDOWS/SYSTEM
mkdir-p /DRIVERS/NVX
rm-f "/WINDOWS/Start Menu/Programs/StartUp/DISMISS.BAT"
rm-f /WINDOWS/AUTONEXT.EXE
rm-f /WINDOWS/SYSTEM/AUTONEXT.EXE
rm-f /AUTONEXT.EXE
rm-f /DISMISS.BAT
upload "$PKG_DIR/nvx.inf" /WINDOWS/INF/nvx.inf
upload "$PKG_DIR/nvx.inf" /DRIVERS/NVX/nvx.inf
upload "$PKG_DIR/nvx.drv" /WINDOWS/SYSTEM/nvx.drv
upload "$PKG_DIR/nvx.drv" /WINDOWS/nvx.drv
upload "$PKG_DIR/nvx.drv" /DRIVERS/NVX/nvx.drv
upload "$PKG_DIR/nvx2.drv" /WINDOWS/SYSTEM/nvx2.drv
upload "$PKG_DIR/nvx2.drv" /WINDOWS/nvx2.drv
upload "$PKG_DIR/nvx2.drv" /DRIVERS/NVX/nvx2.drv
upload "$PKG_DIR/nvx.vxd" /WINDOWS/SYSTEM/nvx.vxd
upload "$PKG_DIR/nvx.vxd" /WINDOWS/SYSTEM/VMM32/nvx.vxd
upload "$PKG_DIR/nvx.vxd" /DRIVERS/NVX/nvx.vxd
upload "$NVX_ROOT/dist_full/gsp_ga104.bin" /WINDOWS/SYSTEM/GSP_GA104.BIN
upload "$NVX_ROOT/dist_full/gsp_ga104.bin" /GSP_GA104.BIN
upload "$PKG_DIR/attach_sec.exe" /WINDOWS/ATTACH_SEC.EXE
upload "$PKG_DIR/attach_sec.exe" /DRIVERS/NVX/attach_sec.exe
upload "$PKG_DIR/attach_sec.exe" /ATTACH2.EXE
upload "$NVX_ROOT/src_win98/nvx_reg.reg" /NVX_REG.REG
upload "$NVX_ROOT/src_win98/qipnp.reg" /QIPNP.REG
upload "$NVX_ROOT/src_win98/slog.com" /SLOG.COM
upload "$NVX_ROOT/src_win98/slog.com" /WINDOWS/SLOG.COM
upload "$NVX_ROOT/src_win98/autoexec.bat" /AUTOEXEC.BAT
upload "$TMP_CONFIG_SYS" /CONFIG.SYS
upload "$TMP_SYS_INI" /WINDOWS/SYSTEM.INI
upload "$TMP_MSDOS_SYS" /MSDOS.SYS
upload "$TMP_WIN_INI" /WINDOWS/WIN.INI
cp /WINDOWS/SYSTEM/MSMOUSE.VXD /WINDOWS/SYSTEM/MOUSE.VXD
sync
EOF

rm -f "$TMP_SYS_INI" "$TMP_MSDOS_SYS" "$TMP_CONFIG_SYS" "$TMP_WIN_INI"

echo "==> Dual-head driver files successfully installed to Windows 98 disk image."
