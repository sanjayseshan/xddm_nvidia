#!/bin/bash
# Launch Windows XP x64 VM for testing RTX 3070 XDDM Driver
# Attach nvx_driver_full.iso as secondary CD-ROM (ide1-cd1)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DISPLAY=:2 qemu-system-x86_64 \
    -enable-kvm \
    -cpu host \
    -smp 2 \
    -m 2048 \
    -vga cirrus \
    -drive file=win98_small.img,format=raw,if=ide \
    -boot c \
    -net nic,model=rtl8139 -net user \
    -serial "file:vm_serial.log" \
    -qmp unix:/tmp/qmp-sock,server,nowait \
    -monitor stdio