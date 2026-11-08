#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

NVX_VM_MEMORY_MB="${NVX_VM_MEMORY_MB:-512}"
NVX_IMG="${1:-$ROOT_DIR/win98_small.img}"

if [ ! -f "$NVX_IMG" ]; then
    echo "Error: $NVX_IMG not found!" >&2
    exit 1
fi

qemu-system-x86_64 \
    -enable-kvm \
    -cpu host \
    -smp 2 \
    -m "$NVX_VM_MEMORY_MB" \
    -vga cirrus \
    -drive file="$NVX_IMG",format=raw,if=ide \
    -boot c \
    -net nic,model=rtl8139 -net user \
    -serial "file:$ROOT_DIR/vm_serial.log" \
    -qmp unix:/tmp/qmp-win98-test.sock,server,nowait \
    -monitor unix:/tmp/qemu-monitor.sock,server,nowait \
    -monitor stdio