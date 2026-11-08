#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

NVX_DRIVER_ISO="${NVX_DRIVER_ISO:-$SCRIPT_DIR/nvx_driver_full.iso}"
NVX_SERIAL_LOG="${NVX_SERIAL_LOG:-$SCRIPT_DIR/vm_serial.log}"
NVX_VM_MEMORY_MB="${NVX_VM_MEMORY_MB:-3072}"

NVX_QEMU_CDROM_ARGS=()
if [ -f "$NVX_DRIVER_ISO" ]; then
    NVX_QEMU_CDROM_ARGS=(-cdrom "$NVX_DRIVER_ISO")
    NVX_CDROM_STATUS="$NVX_DRIVER_ISO"
else
    NVX_CDROM_STATUS="not attached (optional)"
fi

if ! [[ "$NVX_VM_MEMORY_MB" =~ ^[0-9]+$ ]] || [ "$NVX_VM_MEMORY_MB" -lt 1024 ]; then
    echo "Error: NVX_VM_MEMORY_MB must be an integer of at least 1024." >&2
    exit 1
fi
NVX_MEMLOCK_KB="$(ulimit -Sl)"
if [ "$NVX_MEMLOCK_KB" != unlimited ]; then
    NVX_REQUIRED_MEMLOCK_KB=$((NVX_VM_MEMORY_MB * 1024 + 262144))
    if [ "$NVX_MEMLOCK_KB" -lt "$NVX_REQUIRED_MEMLOCK_KB" ]; then
        echo "Error: memlock limit ${NVX_MEMLOCK_KB}KB is too small for ${NVX_VM_MEMORY_MB}MB VFIO RAM." >&2
        echo "Use a smaller NVX_VM_MEMORY_MB or apply the memlock setup in ROOTLESS_TESTING.md." >&2
        exit 1
    fi
fi

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

for _ in $(seq 1 50); do
    qemu-img info "$SCRIPT_DIR/winxp_x64.qcow2" >/dev/null 2>&1 && break
    sleep 0.1
done
if ! qemu-img info "$SCRIPT_DIR/winxp_x64.qcow2" >/dev/null 2>&1; then
    echo "Error: winxp_x64.qcow2 is still locked; refusing to start QEMU." >&2
    exit 1
fi

echo "=================================================================="
echo " Starting Windows XP x64 VM with RTX 3070 VFIO Passthrough        "
echo "=================================================================="
echo "Display Output: Monitor plugged into the RTX 3070"
echo "Driver CD: $NVX_CDROM_STATUS"
echo "Serial log: $NVX_SERIAL_LOG"
echo "Guest RAM: ${NVX_VM_MEMORY_MB} MB (memlock: ${NVX_MEMLOCK_KB} KB)"
echo "Input: Dedicated Logitech Keyboard & Mouse (PS/2 mapping)"
echo "------------------------------------------------------------------"

qemu-system-x86_64 \
    -enable-kvm \
    -m "$NVX_VM_MEMORY_MB" \
    -cpu host,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic,hv_time \
    -smp 4 \
    -vga none \
    -net none \
    -device vfio-pci,host=01:00.0,multifunction=on,x-vga=on,addr=02.0 \
    -device vfio-pci,host=01:00.1,addr=02.1 \
    -drive file=winxp_x64.qcow2,format=qcow2,if=ide \
    "${NVX_QEMU_CDROM_ARGS[@]}" \
    -object input-linux,id=logitech_kbd,evdev="$LOGITECH_KBD",grab_all=on,repeat=on \
    -object input-linux,id=logitech_mouse,evdev="$LOGITECH_MOUSE" \
    -serial "file:$NVX_SERIAL_LOG" \
    -no-reboot \
    -monitor unix:/tmp/qemu-monitor.sock,server,nowait \
    -monitor stdio
