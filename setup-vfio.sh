#!/usr/bin/env bash
set -euo pipefail

VFIO_CONF="/etc/modprobe.d/vfio.conf"
MODULES_FILE="/etc/modules"
GPU_ID="10de:2488"
AUDIO_ID="10de:228b"

if [ "$EUID" -ne 0 ]; then
    echo "Error: This script must be run as root (use sudo)." >&2
    exit 1
fi

enable_vfio() {
    echo "=== [1/4] Configuring early boot VFIO for RTX 3070 ==="
    cat <<EOF > "$VFIO_CONF"
# RTX 3070 (GA104) and HDMI Audio VFIO configuration
options vfio-pci ids=${GPU_ID},${AUDIO_ID}
softdep nvidia pre: vfio-pci
softdep nouveau pre: vfio-pci
softdep snd_hda_intel pre: vfio-pci
EOF
    echo "Wrote $VFIO_CONF"

    echo "=== [2/4] Ensuring vfio modules load on boot ==="
    for mod in vfio vfio_iommu_type1 vfio_pci; do
        if ! grep -q "^${mod}$" "$MODULES_FILE" 2>/dev/null; then
            echo "$mod" >> "$MODULES_FILE"
            echo "Added $mod to $MODULES_FILE"
        fi
    done

    echo "=== [3/4] Updating initramfs ==="
    update-initramfs -u

    echo "=== [4/4] Done! ==="
    echo "RTX 3070 is now configured to bind directly to VFIO at boot."
    echo ""
    read -rp "Do you want to reboot now to apply? [Y/n]: " resp
    if [[ ! "$resp" =~ ^[Nn]$ ]]; then
        echo "Rebooting..."
        reboot
    else
        echo "Please reboot manually when ready ('sudo reboot')."
    fi
}

disable_vfio() {
    echo "=== [1/3] Removing early boot VFIO configuration ==="
    rm -f "$VFIO_CONF"
    sed -i '/^vfio/d' "$MODULES_FILE" 2>/dev/null || true

    echo "=== [2/3] Updating initramfs ==="
    update-initramfs -u

    echo "=== [3/3] Done! ==="
    echo "VFIO early boot disabled. RTX 3070 will return to host NVIDIA driver on next boot."
    echo ""
    read -rp "Do you want to reboot now to apply? [Y/n]: " resp
    if [[ ! "$resp" =~ ^[Nn]$ ]]; then
        echo "Rebooting..."
        reboot
    else
        echo "Please reboot manually when ready ('sudo reboot')."
    fi
}

status_vfio() {
    echo "=== Current GPU Binding Status ==="
    echo -n "RTX 3070 GPU (01:00.0): "
    lspci -nnk -s 01:00.0 | grep "Kernel driver in use" || echo "None"
    echo -n "RTX 3070 Audio (01:00.1): "
    lspci -nnk -s 01:00.1 | grep "Kernel driver in use" || echo "None"
    echo -n "AMD iGPU (0d:00.0): "
    lspci -nnk -s 0d:00.0 | grep "Kernel driver in use" || echo "None"
    echo ""
    echo -n "VFIO Early Boot Config: "
    if [ -f "$VFIO_CONF" ]; then
        echo "ENABLED ($VFIO_CONF present)"
    else
        echo "DISABLED"
    fi
    echo -n "VFIO Group: "
    ls -l /dev/vfio/ 2>/dev/null | grep -v total || echo "No active VFIO groups"
}

case "${1:-}" in
    enable)
        enable_vfio
        ;;
    disable)
        disable_vfio
        ;;
    status)
        status_vfio
        ;;
    *)
        echo "Usage: sudo $0 {enable|disable|status}"
        exit 1
        ;;
esac

