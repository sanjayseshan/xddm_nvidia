#!/usr/bin/env bash
set -euo pipefail

NVX_PCI_ID="${1:-0000:01:00.0}"
NVX_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NVX_ROM_NODE="/sys/bus/pci/devices/$NVX_PCI_ID/rom"
NVX_OUTPUT="$NVX_ROOT/firmware/ga104-vbios.rom"

if [ "$EUID" -ne 0 ]; then
    echo "Run as root: sudo $0 [PCI_ID]" >&2
    exit 1
fi
if [ ! -e "$NVX_ROM_NODE" ]; then
    echo "PCI ROM node not found: $NVX_ROM_NODE" >&2
    exit 1
fi

mkdir -p "$NVX_ROOT/firmware"
disable_rom() {
    printf '0' > "$NVX_ROM_NODE" 2>/dev/null || true
}
trap disable_rom EXIT

printf '1' > "$NVX_ROM_NODE"
dd if="$NVX_ROM_NODE" of="$NVX_OUTPUT" bs=64K status=none

NVX_MAGIC="$(od -An -tx1 -N2 "$NVX_OUTPUT" | tr -d ' \n')"
NVX_SIZE="$(stat -c %s "$NVX_OUTPUT")"
if [ "$NVX_MAGIC" != "55aa" ] || [ "$NVX_SIZE" -lt 65536 ]; then
    echo "Invalid/incomplete ROM: magic=$NVX_MAGIC size=$NVX_SIZE" >&2
    exit 1
fi

chmod 0644 "$NVX_OUTPUT"
sha256sum "$NVX_OUTPUT"
echo "Captured $NVX_SIZE-byte VBIOS at $NVX_OUTPUT"
