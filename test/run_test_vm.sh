#!/bin/bash
# Launch Windows XP x64 VM for testing RTX 3070 XDDM Driver
# Attach nvx_driver_full.iso as secondary CD-ROM (ide1-cd1)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

qemu-system-x86_64 \
    -enable-kvm \
    -cpu host \
    -smp 2 \
    -m 2048 \
    -vga cirrus \
    -drive file=winxp_x64.qcow2,format=qcow2,media=disk,if=ide,index=0 \
    -drive file=nvx_driver_full.iso,media=cdrom,if=ide,index=1 \
    -boot c \
    -net nic,model=rtl8139 -net user \
    -serial stdio \
    -qmp unix:/tmp/qmp-sock,server,nowait
