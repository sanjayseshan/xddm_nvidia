#!/bin/bash
# Launch Windows XP x32 VM for testing RTX 3070 XDDM Driver
# Attach nvx_driver_full.iso as secondary CD-ROM (ide1-cd1)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

qemu-system-x86_64 \
    -enable-kvm \
    -cpu host \
    -smp 2 \
    -m 2048 \
    -vga cirrus \
    -drive file=winxp_x32.qcow2,format=qcow2,media=disk,if=ide,index=0 \
    -drive file=en_windows_xp_professional_with_service_pack_3_x86_cd_vl_x14-73974.iso,media=cdrom,if=ide,index=1 \
    -boot c \
    -net nic,model=rtl8139 -net user \
    -serial stdio \
    -qmp unix:/tmp/qmp-sock,server,nowait
