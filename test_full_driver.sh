#!/usr/bin/env bash
set -euo pipefail

NVX_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NVX_ARCHIVE="$NVX_ROOT/test_logs"
NVX_STAMP="$(date +%Y%m%d-%H%M%S)"
NVX_RUN_LOG="$NVX_ARCHIVE/v40-$NVX_STAMP.log"

mkdir -p "$NVX_ARCHIVE"
if [ -f "$NVX_ROOT/vm_serial.log" ]; then
    cp "$NVX_ROOT/vm_serial.log" "$NVX_ARCHIVE/before-v40-$NVX_STAMP.log"
fi

echo "==> Running host tests and building FWSEC signature-selection diagnostic v40"
if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
    sudo -u "$SUDO_USER" make -C "$NVX_ROOT/src_driver" check package-full
else
    make -C "$NVX_ROOT/src_driver" check package-full
fi

if ! cmp -s "$NVX_ROOT/dist/nvxdisp.dll" "$NVX_ROOT/dist_full/nvxdisp.dll"; then
    echo "Refusing test: dist_full/nvxdisp.dll does not match the known-working display DLL." >&2
    exit 1
fi
echo "==> Verified miniport/display package pairing"
sha256sum "$NVX_ROOT/dist_full/nvxmp.sys" "$NVX_ROOT/dist_full/nvxdisp.dll"

echo "==> Updating the offline XP guest with dist_full"
if [ "$EUID" -eq 0 ]; then
    "$NVX_ROOT/update_disk_driver.sh" full
else
    "$NVX_ROOT/update_disk_driver_user.sh" full
fi

if [ ! -e /dev/vfio/vfio ]; then
    echo "VFIO is not ready. See ROOTLESS_TESTING.md." >&2
    exit 1
fi

finish() {
    echo
    echo "==> Result log: $NVX_RUN_LOG"
    if [ -f "$NVX_RUN_LOG" ]; then
        grep -E "Miniport Driver|\[SEC2\]|\[GSP\]|\[RESET\]" "$NVX_RUN_LOG" | tail -80 || true
        if grep -q "NVX \[FWSEC\]: desc=" "$NVX_RUN_LOG"; then
            echo "RESULT: complete PROM and FWSEC descriptor parsed read-only."
        elif grep -q "FWSEC/FRTS is required" "$NVX_RUN_LOG"; then
            echo "RESULT: GSP writes remained gated; inspect VBIOS probe status."
        elif grep -q "Mailbox0: 0x0000000000000091" "$NVX_RUN_LOG"; then
            echo "RESULT: SEC2 executed, but Booter Load returned 0x91."
        elif grep -q "Verified: 0xA0A0A0A0A0A0A0A0" "$NVX_RUN_LOG"; then
            echo "RESULT: WPR verification passed."
        elif grep -q "\[RESET\]" "$NVX_RUN_LOG"; then
            echo "RESULT: Windows invoked the reset callback; record the visible STOP code."
        else
            echo "RESULT: inconclusive; preserve the complete log and visible screen state."
        fi
    fi
}
trap finish EXIT

echo "==> Launching VFIO VM. QEMU will not reboot after a guest crash."
echo "==> At the QEMU monitor, use 'quit' after recording the screen/STOP code."
NVX_DRIVER_ISO="$NVX_ROOT/nvx_driver_full.iso" \
NVX_SERIAL_LOG="$NVX_RUN_LOG" \
    "$NVX_ROOT/run_vfio_vm.sh"
