#!/usr/bin/env bash
set -euo pipefail

XORG_CONF="/etc/X11/xorg.conf"
XORG_AMD_CONF="/etc/X11/xorg.conf.amdgpu"
XORG_NVIDIA_CONF="/etc/X11/xorg.conf.nvidia"
BLACKLIST_CONF="/etc/modprobe.d/blacklist.conf"
DM_SERVICE="gdm3" # change if using lightdm, sddm, etc.

if [ "$EUID" -ne 0 ]; then
    echo "Error: This script must be run as root (use sudo)." >&2
    exit 1
fi

stop_x11() {
    echo "Terminating existing X11 / Display Manager sessions..."

    # 1. Stop display manager via systemctl if systemd is running
    if command -v systemctl >/dev/null 2>&1 && systemctl is-system-running >/dev/null 2>&1; then
        for dm in "$DM_SERVICE" gdm3 gdm lightdm sddm nodm; do
            if systemctl is-active --quiet "$dm"; then
                echo "Stopping service $dm..."
                systemctl stop "$dm" 2>/dev/null || true
            fi
        done
    fi

    # 2. Terminate any remaining Xorg / X servers
    if pgrep -x Xorg >/dev/null 2>&1 || pgrep -x X >/dev/null 2>&1; then
        echo "Killing active Xorg processes..."
        killall -15 Xorg X 2>/dev/null || true
        sleep 1
        killall -9 Xorg X 2>/dev/null || true
    fi
}

start_x11() {
    echo "Restarting Display Manager (${DM_SERVICE})..."
    if command -v systemctl >/dev/null 2>&1 && systemctl is-system-running >/dev/null 2>&1; then
        systemctl start "$DM_SERVICE" 2>/dev/null || systemctl restart "$DM_SERVICE"
    else
        service "$DM_SERVICE" start 2>/dev/null || "/etc/init.d/${DM_SERVICE}" start 2>/dev/null || echo "Could not start ${DM_SERVICE} automatically. Please start it manually."
    fi
}

show_status() {
    echo "=== Current GPU Status ==="
    echo -n "Active Driver for AMD iGPU (0d:00.0): "
    lspci -k -s 0d:00.0 | grep "Kernel driver in use" || echo "None"
    echo -n "Active Driver for NVIDIA GPU (01:00.0): "
    lspci -k -s 01:00.0 | grep "Kernel driver in use" || echo "None"
    
    echo -n "Xorg Configuration: "
    if [ -f "$XORG_CONF" ]; then
        grep -i "Driver" "$XORG_CONF" | head -n 1
    else
        echo "No /etc/X11/xorg.conf (Auto-detect / modesetting / amdgpu)"
    fi
}

switch_to_amd() {
    echo "[1/5] Stopping existing X11 / Display Manager..."
    stop_x11

    echo "[2/5] Unblacklisting amdgpu in ${BLACKLIST_CONF}..."
    if [ -f "$BLACKLIST_CONF" ]; then
        sed -i 's/^blacklist amdgpu/#blacklist amdgpu/' "$BLACKLIST_CONF"
        sed -i 's/^blacklist amdkfd/#blacklist amdkfd/' "$BLACKLIST_CONF"
    fi

    echo "[3/5] Loading amdgpu kernel module..."
    modprobe amdgpu || true

    echo "[4/5] Switching X11 configuration to AMD iGPU..."
    if [ -f "$XORG_CONF" ] && [ ! -f "$XORG_NVIDIA_CONF" ]; then
        cp -a "$XORG_CONF" "$XORG_NVIDIA_CONF"
    fi

    # Create AMD iGPU Xorg configuration targeting PCI:13:0:0 (0d:00.0 in decimal)
    cat <<'EOF' > "$XORG_CONF"
Section "Device"
    Identifier     "AMD_iGPU"
    Driver         "amdgpu"
    BusID          "PCI:13:0:0"
EndSection

Section "Screen"
    Identifier     "Screen0"
    Device         "AMD_iGPU"
EndSection
EOF

    echo "[5/5] Done! Remember to plug your monitor cable into your motherboard's video port."
    echo ""
    read -rp "Do you want to start the display manager (${DM_SERVICE}) now? [Y/n]: " resp
    if [[ ! "$resp" =~ ^[Nn]$ ]]; then
        start_x11
    else
        echo "Display manager stopped. Run 'sudo systemctl start ${DM_SERVICE}' when ready."
    fi
}

switch_to_nvidia() {
    echo "[1/4] Stopping existing X11 / Display Manager..."
    stop_x11

    echo "[2/4] Restoring original NVIDIA xorg.conf..."
    if [ -f "$XORG_NVIDIA_CONF" ]; then
        mv "$XORG_NVIDIA_CONF" "$XORG_CONF"
    else
        echo "Warning: $XORG_NVIDIA_CONF not found. Restoring previous backup if available."
        [ -f "${XORG_CONF}.backup" ] && cp -a "${XORG_CONF}.backup" "$XORG_CONF"
    fi

    echo "[3/4] Re-enabling amdgpu blacklist & unloading module..."
    if [ -f "$BLACKLIST_CONF" ]; then
        sed -i 's/^#*blacklist amdgpu/blacklist amdgpu/' "$BLACKLIST_CONF"
        sed -i 's/^#*blacklist amdkfd/blacklist amdkfd/' "$BLACKLIST_CONF"
    fi
    modprobe -r amdgpu 2>/dev/null || true

    echo "[4/4] Done! Remember to move your monitor cable back to the NVIDIA GPU."
    echo ""
    read -rp "Do you want to start the display manager (${DM_SERVICE}) now? [Y/n]: " resp
    if [[ ! "$resp" =~ ^[Nn]$ ]]; then
        start_x11
    else
        echo "Display manager stopped. Run 'sudo systemctl start ${DM_SERVICE}' when ready."
    fi
}

case "${1:-}" in
    amd)
        switch_to_amd
        ;;
    nvidia)
        switch_to_nvidia
        ;;
    status)
        show_status
        ;;
    *)
        echo "Usage: sudo $0 {amd|nvidia|status}"
        exit 1
        ;;
esac