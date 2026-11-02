#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

NVX_DRIVER_ISO="${NVX_DRIVER_ISO:-$SCRIPT_DIR/nvx_driver_full.iso}"
NVX_SERIAL_LOG="${NVX_SERIAL_LOG:-$SCRIPT_DIR/vm_serial.log}"

if [ "$EUID" -eq 0 ]; then
    chmod 660 /dev/vfio/* 2>/dev/null || true
fi

LOGITECH_KBD="/dev/input/by-id/usb-Logitech_USB_Receiver-if02-event-kbd"
LOGITECH_MOUSE="/dev/input/by-id/usb-Logitech_USB_Receiver-if02-event-mouse"

shopt -s nullglob
VFIO_GROUP_NODES=(/dev/vfio/[0-9]*)
if [ "${#VFIO_GROUP_NODES[@]}" -eq 0 ]; then
    echo "Error: no VFIO IOMMU group node is available. See ROOTLESS_TESTING.md." >&2
    exit 1
fi
for dev in /dev/vfio/vfio "${VFIO_GROUP_NODES[@]}" "$LOGITECH_KBD" "$LOGITECH_MOUSE"; do
    if [ ! -r "$dev" ] || [ ! -w "$dev" ]; then
        echo "Error: $dev is not accessible to $(id -un). See ROOTLESS_TESTING.md." >&2
        exit 1
    fi
done

echo "=================================================================="
echo " Starting Windows XP x64 VM with RTX 3070 VFIO Passthrough        "
echo "=================================================================="
echo "Display Output: Monitor plugged into the RTX 3070"
echo "Driver CD attached: $NVX_DRIVER_ISO"
echo "Serial log: $NVX_SERIAL_LOG"
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
    -cdrom "$NVX_DRIVER_ISO" \
    -object input-linux,id=logitech_kbd,evdev="$LOGITECH_KBD",grab_all=on,repeat=on \
    -object input-linux,id=logitech_mouse,evdev="$LOGITECH_MOUSE" \
    -serial "file:$NVX_SERIAL_LOG" \
    -no-reboot \
    -monitor stdio
