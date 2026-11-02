#!/usr/bin/env bash
set -euo pipefail

GPU="0000:01:00.0"
AUDIO="0000:01:00.1"

if [ "$EUID" -ne 0 ]; then
    echo "Error: This script must be run as root (use sudo)." >&2
    exit 1
fi

switch_to_vm() {
    echo "=== Safely Isolating RTX 3070 for VM Passthrough ==="

    echo "[1/5] Unbinding Audio device ($AUDIO)..."
    if [ -d "/sys/bus/pci/devices/$AUDIO/driver" ]; then
        echo "$AUDIO" > "/sys/bus/pci/devices/$AUDIO/driver/unbind" || true
    fi

    echo "[2/5] Unbinding GPU ($GPU) from NVIDIA driver..."
    if [ -d "/sys/bus/pci/devices/$GPU/driver" ]; then
        echo "$GPU" > "/sys/bus/pci/devices/$GPU/driver/unbind" || true
    fi

    echo "[3/5] Setting driver override to vfio-pci..."
    modprobe vfio
    modprobe vfio_pci
    echo "vfio-pci" > "/sys/bus/pci/devices/$GPU/driver_override"
    echo "vfio-pci" > "/sys/bus/pci/devices/$AUDIO/driver_override"

    echo "[4/5] Binding devices to vfio-pci..."
    echo "$GPU" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null || true
    echo "$AUDIO" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null || true

    # Keep VFIO access limited to the VM group.
    chgrp kvm /dev/vfio/* 2>/dev/null || true
    chmod 660 /dev/vfio/* 2>/dev/null || true

    echo "[5/5] Done! Status:"
    echo -n "GPU Status: "
    lspci -nnk -s "$GPU" | grep "Kernel driver in use" || echo "None"
    echo -n "Audio Status: "
    lspci -nnk -s "$AUDIO" | grep "Kernel driver in use" || echo "None"
    echo "VFIO Group: $(ls -l /dev/vfio/ | grep -v total || true)"
}

switch_to_host() {
    echo "=== Returning RTX 3070 to Host ==="

    echo "[1/4] Unbinding from vfio-pci..."
    if [ -d "/sys/bus/pci/devices/$GPU/driver" ]; then
        echo "$GPU" > "/sys/bus/pci/devices/$GPU/driver/unbind" || true
    fi
    if [ -d "/sys/bus/pci/devices/$AUDIO/driver" ]; then
        echo "$AUDIO" > "/sys/bus/pci/devices/$AUDIO/driver/unbind" || true
    fi

    echo "[2/4] Resetting driver override..."
    echo "" > "/sys/bus/pci/devices/$GPU/driver_override"
    echo "" > "/sys/bus/pci/devices/$AUDIO/driver_override"

    echo "[3/4] Re-binding to host drivers..."
    modprobe nvidia 2>/dev/null || true
    modprobe snd_hda_intel 2>/dev/null || true
    echo "$GPU" > /sys/bus/pci/drivers/nvidia/bind 2>/dev/null || true
    echo "$AUDIO" > /sys/bus/pci/drivers/snd_hda_intel/bind 2>/dev/null || true

    echo "[4/4] Done! Status:"
    lspci -nnk -s "$GPU" | grep "Kernel driver in use" || echo "None"
}

case "${1:-}" in
    vm)
        switch_to_vm
        ;;
    host)
        switch_to_host
        ;;
    *)
        echo "Usage: sudo $0 {vm|host}"
        exit 1
        ;;
esac
