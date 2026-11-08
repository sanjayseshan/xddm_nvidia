#!/bin/bash
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
qemu-system-x86_64 \
    -enable-kvm \
    -cpu host \
    -smp 2 \
    -m 2048 \
    -vga cirrus \
    -drive file=winxp_x64.qcow2,format=qcow2,media=disk,if=ide,index=0 \
    -drive file=en_win_xp_pro_x64_with_sp2_vl_x13-41611.iso,media=cdrom,if=ide,index=1 \
    -fda unattend.img \
    -boot d \
    -net nic,model=rtl8139 -net user \
    -vnc :1 \
    -qmp unix:/tmp/qmp-sock,server,nowait
