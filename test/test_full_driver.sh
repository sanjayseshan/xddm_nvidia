#!/usr/bin/env bash
set -euo pipefail

NVX_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NVX_ARCHIVE="$NVX_ROOT/test_logs"
NVX_STAMP="$(date +%Y%m%d-%H%M%S)"
NVX_TEST_VERSION="2.0.0.62"
NVX_RUN_LOG="$NVX_ARCHIVE/v$NVX_TEST_VERSION-$NVX_STAMP.log"

mkdir -p "$NVX_ARCHIVE"
if [ -f "$NVX_ROOT/vm_serial.log" ]; then
    cp "$NVX_ROOT/vm_serial.log" "$NVX_ARCHIVE/before-v$NVX_TEST_VERSION-$NVX_STAMP.log"
fi

echo "==> Running host tests and building the RM-managed GR-channel probe v$NVX_TEST_VERSION (no ISO)"
if [ "${NVX_GR_CHANNEL_PROBE:-0}" = "1" ]; then
    NVX_DRIVER_BUILD_TARGET=full-gr-probe
    NVX_DRIVER_BUILD_ARGS=("NVX_ENABLE_GR_CHANNEL_PROBE=1")
    echo "==> Enabling the isolated Ampere GR allocation probe (no rendering submission)"
    if [ "${NVX_GR_PROMOTE_PROBE:-0}" = "1" ]; then
        NVX_DRIVER_BUILD_ARGS+=("NVX_ENABLE_GR_PROMOTE_PROBE=1")
        echo "==> Enabling isolated GR context initialization and VA-binding probe"
    fi
    if [ "${NVX_GR_SUBMISSION_PROBE:-0}" = "1" ]; then
        if [ "${NVX_GR_PROMOTE_PROBE:-0}" != "1" ]; then
            echo "GR submission probe requires NVX_GR_PROMOTE_PROBE=1." >&2
            exit 1
        fi
        NVX_DRIVER_BUILD_ARGS+=("NVX_ENABLE_GR_SUBMISSION_PROBE=1")
        echo "==> Enabling isolated GR semaphore-completion probe"
    fi
    if [ "${NVX_GR_UNPRIVILEGED_CONTEXT_PROBE:-0}" = "1" ]; then
        if [ "${NVX_GR_SUBMISSION_PROBE:-0}" != "1" ]; then
            echo "Unprivileged context probe requires the GR submission probe." >&2
            exit 1
        fi
        NVX_DRIVER_BUILD_ARGS+=("NVX_ENABLE_GR_UNPRIVILEGED_CONTEXT_PROBE=1")
        echo "==> Testing unprivileged GR PTEs in this isolated diagnostic build"
    fi
else
    NVX_DRIVER_BUILD_TARGET=full
    NVX_DRIVER_BUILD_ARGS=()
fi
if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
    sudo -u "$SUDO_USER" make -C "$NVX_ROOT/src_driver" check "$NVX_DRIVER_BUILD_TARGET" "${NVX_DRIVER_BUILD_ARGS[@]}"
    sudo -u "$SUDO_USER" cp "$NVX_ROOT/src_driver/nvxmp.sys" "$NVX_ROOT/dist_full/nvxmp.sys"
else
    make -C "$NVX_ROOT/src_driver" check "$NVX_DRIVER_BUILD_TARGET" "${NVX_DRIVER_BUILD_ARGS[@]}"
    cp "$NVX_ROOT/src_driver/nvxmp.sys" "$NVX_ROOT/dist_full/nvxmp.sys"
fi
make -C "$NVX_ROOT/src_disp"
make -C "$NVX_ROOT/src_disp" check
cp "$NVX_ROOT/src_disp/nvxdisp.dll" "$NVX_ROOT/dist_full/nvxdisp.dll"

if [ ! -f "$NVX_ROOT/dist_full/nvxdisp.dll" ]; then
    echo "Refusing test: dist_full/nvxdisp.dll is missing." >&2
    exit 1
fi
echo "==> Verified miniport/display package pairing"
sha256sum "$NVX_ROOT/dist_full/nvxmp.sys" "$NVX_ROOT/dist_full/nvxdisp.dll"

echo "==> Updating the offline XP guest with dist_full"
"$NVX_ROOT/test/update_disk_driver_user.sh" full

if [ ! -e /dev/vfio/vfio ]; then
    echo "VFIO is not ready. See ROOTLESS_TESTING.md." >&2
    exit 1
fi

finish() {
    echo
    echo "==> Result log: $NVX_RUN_LOG"
    if [ -f "$NVX_RUN_LOG" ]; then
        echo "==> Display hardware verification"
        if grep -q '\[DISPLAY-VERIFY\]' "$NVX_RUN_LOG"; then
            grep '\[DISPLAY-VERIFY\]' "$NVX_RUN_LOG" | tail -12 || true
            echo "DISPLAY RESULT: Inspect the latest head IDs, DP lane lock and UPDATE notifier above; GPU copy success alone does not verify scanout."
        else
            echo "DISPLAY RESULT: No display verification recorded; monitor activation is unverified."
        fi
        grep -E "Miniport Driver|\[SEC2\]|\[GSP\]|\[RM\]|\[GR\]|\[COPY\]|\[FB\]|\[FBMAP\]|\[RESET\]" "$NVX_RUN_LOG" | tail -80 || true
        if grep -q 'NVX \[GR\]: GR submission disabled' "$NVX_RUN_LOG"; then
            echo "GR RESULT: context layout queried; no graphics submission or rendering was tested."
        elif grep -q 'NVX \[GR\]: firmware assertions observed during submission' "$NVX_RUN_LOG"; then
            echo "GR RESULT: firmware assertions occurred during the probe; graphics execution is unverified."
        elif grep -q 'NVX \[GR\]: I2M data write verified' "$NVX_RUN_LOG"; then
            echo "GR RESULT: graphics pipeline data write and semaphore completed successfully!"
        elif grep -q 'NVX \[GR\]: GR semaphore release completed' "$NVX_RUN_LOG"; then
            echo "GR RESULT: graphics semaphore completed; no draw has been verified."
        elif grep -q 'NVX \[GR\]:' "$NVX_RUN_LOG"; then
            echo "GR RESULT: graphics engine probe ran but did not complete; inspect the GR diagnostics above."
        fi
        if grep -q 'NVX \[ACCEL\]: GPU Copy' "$NVX_RUN_LOG"; then
            grep 'NVX \[ACCEL\]: GPU Copy' "$NVX_RUN_LOG" | tail -8 || true
            echo "2D COPY RESULT: GPU screen-copy submissions were logged."
        else
            echo "2D COPY RESULT: no GPU screen-copy submission was logged; the CopyBits fast path may not have been exercised."
        fi
        if grep -q '\[2D\]: GPU solid fill' "$NVX_RUN_LOG"; then
            grep '\[2D\]: GPU solid fill' "$NVX_RUN_LOG" | tail -8 || true
            echo "2D RESULT: GPU solid fills were submitted; verify rectangles visually and inspect fallback/corruption behavior."
        else
            echo "2D RESULT: no GPU solid-fill submission was logged; the new GDI fast path was not exercised or did not activate."
        fi
        if grep -q 'NVX \[RM\]: MMU fault queued' "$NVX_RUN_LOG"; then
            echo "RESULT: GPU MMU fault reported; accelerated queues were disabled and native display may have fallen back."
        elif grep -q 'NVX \[COPY-Q\]: submit timeout' "$NVX_RUN_LOG"; then
            echo "RESULT: Copy Engine submission timed out after initialization; native display may have fallen back."
        elif grep -q 'RM INIT_DONE received' "$NVX_RUN_LOG"; then
            if grep -q 'GPU copy completed and verified' "$NVX_RUN_LOG"; then
                echo "RESULT: GPU copy completion verified."
            else
                echo "RESULT: RM INIT_DONE received; GPU execution is still unverified."
            fi
        elif grep -q 'Bootstrap incomplete; staging retained' "$NVX_RUN_LOG"; then
            echo "RESULT: FWSEC passed but GSP bootstrap remains incomplete; inspect SEC2/GSP diagnostics."
        elif grep -q "NVX \[FWSEC-EXEC\]: FRTS initialized and WPR2 validated" "$NVX_RUN_LOG"; then
            echo "RESULT: FWSEC executed; FRTS and WPR2 validation passed."
        elif grep -q "retained for hardware safety" "$NVX_RUN_LOG"; then
            echo "RESULT: FWSEC completion was ambiguous; DMA storage was safely retained."
        elif grep -q "execution did not validate" "$NVX_RUN_LOG"; then
            echo "RESULT: FWSEC halted/failed validation; GSP boot remained blocked."
        elif grep -q "NVX \[FWSEC-DMA\]: buffer scrubbed and released" "$NVX_RUN_LOG"; then
            echo "RESULT: coherent FWSEC DMA staging passed and the buffer was released."
        elif grep -q "NVX \[FWSEC\]: desc=" "$NVX_RUN_LOG"; then
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
echo "==> For a booted guest use 'system_powerdown' and wait for XP to exit."
echo "==> Use 'quit' only for a pre-boot hang or failed graceful shutdown."
ln -sfn "$NVX_RUN_LOG" "$NVX_ROOT/vm_serial.log"
if [ "${NVX_AUTO_RETRY:-0}" = "1" ]; then
    MAX_RETRIES="${MAX_RETRIES:-10}" BOOT_TIMEOUT="${BOOT_TIMEOUT:-35}" \
        NVX_DRIVER_ISO="${NVX_DRIVER_ISO:-$NVX_ROOT/nvx_driver_full.iso}" \
        NVX_SERIAL_LOG="$NVX_RUN_LOG" \
        "$NVX_ROOT/test/run_vfio_xp_autoretry.sh"
else
    NVX_DRIVER_ISO="${NVX_DRIVER_ISO:-$NVX_ROOT/nvx_driver_full.iso}" \
    NVX_SERIAL_LOG="$NVX_RUN_LOG" \
        "$NVX_ROOT/test/run_vfio_vm.sh"
fi
