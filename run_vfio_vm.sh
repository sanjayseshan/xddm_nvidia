#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ "$EUID" -ne 0 ]; then
    echo "Error: This script requires root privileges to access VFIO." >&2
    echo "Please run: sudo $0" >&2
    exit 1
fi

chmod 666 /dev/vfio/* 2>/dev/null || true

LOGITECH_KBD="/dev/input/by-id/usb-Logitech_USB_Receiver-if02-event-kbd"
LOGITECH_MOUSE="/dev/input/by-id/usb-Logitech_USB_Receiver-if02-event-mouse"

echo "=================================================================="
echo " Starting Windows XP x64 VM with RTX 3070 VFIO Passthrough        "
echo "=================================================================="
echo "Display Output: Monitor plugged into the RTX 3070"
echo "Driver CD attached: nvx_driver.iso"
echo "Input: Dedicated Logitech Keyboard & Mouse (PS/2 mapping)"
echo "------------------------------------------------------------------"

qemu-system-x86_64 \
    -enable-kvm \
    -m 4096 \
    -cpu host,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic,hv_time \
    -smp 4 \
    -vga none \
    -net none \
    -device vfio-pci,host=01:00.0,multifunction=on,x-vga=on,addr=02.0 \
    -device vfio-pci,host=01:00.1,addr=02.1 \
    -drive file=winxp_x64.qcow2,format=qcow2,if=ide \
    -cdrom nvx_driver.iso \
    -object input-linux,id=logitech_kbd,evdev="$LOGITECH_KBD",grab_all=on,repeat=on \
    -object input-linux,id=logitech_mouse,evdev="$LOGITECH_MOUSE" \
    -serial file:vm_serial.log \
    -monitor stdio
