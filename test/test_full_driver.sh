#!/usr/bin/env bash
set -euo pipefail

NVX_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NVX_ARCHIVE="$NVX_ROOT/test_logs"
NVX_STAMP="$(date +%Y%m%d-%H%M%S)"
NVX_TEST_VERSION="3.0.0.148"
NVX_REQUIRE_HW_OPENGL="${NVX_REQUIRE_HW_OPENGL:-1}"
NVX_RUN_LOG="$NVX_ARCHIVE/v$NVX_TEST_VERSION-$NVX_STAMP.log"
NVX_TEST_FLAGS=()
NVX_MESA_TEST_FLAGS=()
if [ "${NVX_TRACE_ICD_CALLBACKS:-0}" = "1" ]; then
    NVX_TEST_FLAGS+=("-DNVX_TRACE_ICD_CALLBACKS=1")
fi
if [ "${NVX_MESA_APPLOCAL:-0}" = "1" ]; then
    NVX_TEST_FLAGS+=("-DNVX_MESA_APPLOCAL_FRONTEND=1")
fi
if [ "${NVX_GR_SUBMISSION_PROBE:-0}" = "1" ]; then
    NVX_TEST_FLAGS+=("-DNVX_REQUIRE_3D_ABI=1")
fi
if [ "${NVX_MESA_GALLIUM_BRIDGE:-0}" = "1" ]; then
    NVX_MESA_TEST_FLAGS+=("-DNVX_MESA_GALLIUM_BRIDGE_PROBE=1" "-DNVX_MESA_ZF32_DIAGNOSTIC=1")
    if [ "${NVX_MESA_DISABLE_DEPTH:-0}" = "1" ]; then
        NVX_MESA_TEST_FLAGS+=("-DNVX_MESA_BRIDGE_DISABLE_DEPTH=1")
    fi
    case "${NVX_MESA_DEPTH_MODE:-}" in
        "") ;;
        2|3)
            if [ "${NVX_MESA_DISABLE_DEPTH:-0}" = "1" ]; then
                echo "Choose either NVX_MESA_DISABLE_DEPTH or NVX_MESA_DEPTH_MODE." >&2
                exit 1
            fi
            NVX_MESA_TEST_FLAGS+=("-DNVX_MESA_DEPTH_MODE_OVERRIDE=${NVX_MESA_DEPTH_MODE}")
            ;;
        *)
            echo "NVX_MESA_DEPTH_MODE must be 2 (zeta attach only) or 3 (depth test without writes)." >&2
            exit 1
            ;;
    esac
fi

mkdir -p "$NVX_ARCHIVE"
if [ -f "$NVX_ROOT/vm_serial.log" ]; then
    cp "$NVX_ROOT/vm_serial.log" "$NVX_ARCHIVE/before-v$NVX_TEST_VERSION-$NVX_STAMP.log"
fi

echo "==> Running host tests and building the RM-managed GR-channel probe v$NVX_TEST_VERSION (no ISO)"
python3 "$NVX_ROOT/test/audit_ampere_vertex_shader.py"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -static-libgcc "${NVX_TEST_FLAGS[@]}" "${NVX_MESA_TEST_FLAGS[@]}" \
    -DNVX_TEST_NOUVEAU_PUSHBUF=1 -I"$NVX_ROOT/mesa_patches" \
    "$NVX_ROOT/test/test_gpu_accel.c" \
    "$NVX_ROOT/mesa_patches/nvx_winsys.c" \
    "$NVX_ROOT/mesa_patches/nvx_winsys_win32.c" \
    "$NVX_ROOT/mesa_patches/nvx_nouveau_bo.c" \
    "$NVX_ROOT/mesa_patches/nvx_nouveau_pushbuf.c" \
    -lopengl32 -lgdi32 -luser32 \
    -o "$NVX_ROOT/test/test_gpu_accel.exe"
i686-w64-mingw32-gcc -O2 -Wall -Wextra -static-libgcc \
    -D_WIN32_WINNT=0x0502 -DWINVER=0x0502 \
    -DNVX_TEST_NOUVEAU_PUSHBUF=1 -I"$NVX_ROOT/mesa_patches" \
    "$NVX_ROOT/test/test_gpu_accel.c" \
    "$NVX_ROOT/mesa_patches/nvx_winsys.c" \
    "$NVX_ROOT/mesa_patches/nvx_winsys_win32.c" \
    "$NVX_ROOT/mesa_patches/nvx_nouveau_bo.c" \
    "$NVX_ROOT/mesa_patches/nvx_nouveau_pushbuf.c" \
    -lopengl32 -lgdi32 -luser32 \
    -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,2 \
    -o "$NVX_ROOT/test/test_gpu_accel32.exe"
cp "$NVX_ROOT/test/test_gpu_accel.exe" "$NVX_ROOT/dist_full/test_gpu_accel.exe"
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
if [ "${NVX_BUILD_MESA_ICD:-0}" = "1" ]; then
    make -C "$NVX_ROOT/src_disp" clean
    # The display DDI must advertise non-generic OpenGL pixel formats to GDI;
    # otherwise XP selects PFD_GENERIC_FORMAT and stays on GDI Generic before
    # it can dispatch context creation to the ICD.
    make -C "$NVX_ROOT/src_disp" NVX_ENABLE_EXPERIMENTAL_OPENGL_DDI=1
else
    make -C "$NVX_ROOT/src_disp"
fi
make -C "$NVX_ROOT/src_disp" check
cp "$NVX_ROOT/src_disp/nvxdisp.dll" "$NVX_ROOT/dist_full/nvxdisp.dll"

# Keep the offline package INF aligned with the version used for this test
# build.
NVX_DRIVER_DATE="$(date +%m/%d/%Y)"
sed -i -E "s|^DriverVer[[:space:]]*=.*|DriverVer   = $NVX_DRIVER_DATE, $NVX_TEST_VERSION|" \
    "$NVX_ROOT/dist_full/nvx.inf"

if [ "${NVX_BUILD_MESA_ICD:-0}" = "1" ]; then
    echo "==> Building the XP x64 Mesa WGL ICD (Nouveau with softpipe fallback)"
    "$NVX_ROOT/test/build_mesa_xp64.sh"
fi

if [ ! -f "$NVX_ROOT/dist_full/nvxdisp.dll" ]; then
    echo "Refusing test: dist_full/nvxdisp.dll is missing." >&2
    exit 1
fi
echo "==> Verified miniport/display package pairing"
sha256sum "$NVX_ROOT/dist_full/nvxmp.sys" "$NVX_ROOT/dist_full/nvxdisp.dll"
if [ -f "$NVX_ROOT/dist_full/nvxogl64.dll" ]; then
    echo "==> XP x64 OpenGL ICD package"
    sha256sum "$NVX_ROOT/dist_full/nvxogl64.dll"
fi

if [ "${NVX_RAPID_3D:-0}" = "1" ]; then
    echo "==> Guest test mode: focused 3D resource transfer, batch validation, 64x16 clear, full-size color matrix, fences, and readback cases"
fi
echo "==> Updating the offline XP guest with dist_full"
"$NVX_ROOT/test/update_disk_driver_user.sh" full

if [ ! -e /dev/vfio/vfio ]; then
    echo "VFIO is not ready. See ROOTLESS_TESTING.md." >&2
    exit 1
fi

finish() {
    echo
    echo "==> Result log: $NVX_RUN_LOG"
    if [ -f "$NVX_ROOT/winxp_x64.qcow2" ] && command -v guestfish >/dev/null 2>&1; then
        for guest_log in mesa_bridge.log nvx_nouveau_trace.log; do
            echo "==> Guest OpenGL trace: $guest_log"
            guestfish --ro -a "$NVX_ROOT/winxp_x64.qcow2" -i cat "/test_accel/$guest_log" \
                >> "$NVX_RUN_LOG" 2>/dev/null || true
        done
    fi
    if [ -f "$NVX_RUN_LOG" ]; then
        echo "==> Display hardware verification"
        if grep -q '\[DISPLAY-VERIFY\]' "$NVX_RUN_LOG"; then
            grep '\[DISPLAY-VERIFY\]' "$NVX_RUN_LOG" | tail -12 || true
            echo "DISPLAY RESULT: Inspect the latest head IDs, DP lane lock and UPDATE notifier above; GPU copy success alone does not verify scanout."
        else
            echo "DISPLAY RESULT: No display verification recorded; monitor activation is unverified."
        fi
        if grep -q '\[OPENGL_RESULT\]' "$NVX_RUN_LOG"; then
            grep '\[OPENGL_RESULT\]' "$NVX_RUN_LOG" | tail -1
            if grep -q '\[OPENGL_RESULT\] RENDERED' "$NVX_RUN_LOG"; then
                echo "OPENGL RESULT: rendered through a non-software OpenGL renderer; verify renderer name and GPU activity."
            elif grep -q '\[OPENGL_RESULT\] SOFTWARE' "$NVX_RUN_LOG"; then
                echo "OPENGL RESULT: software renderer only; hardware OpenGL is not operational."
            else
                echo "OPENGL RESULT: test failed or did not finish."
            fi
        else
            echo "OPENGL RESULT: no current renderer classification was recorded."
        fi
        if grep -q '\[NVX_3D_RESULT\]' "$NVX_RUN_LOG"; then
            grep '\[NVX_3D_RESULT\]' "$NVX_RUN_LOG" | tail -1
        elif grep -q '\[NVX_3D_RAPID_RESULT\]' "$NVX_RUN_LOG"; then
            grep '\[NVX_3D_RAPID_RESULT\]' "$NVX_RUN_LOG" | tail -1
        else
            echo "NVX 3D RESULT: no user allocation/submission test result was recorded."
        fi
        grep -E "Miniport Driver|\[SEC2\]|\[GSP\]|\[RM\]|\[GR\]|\[COPY\]|\[FB\]|\[FBMAP\]|\[RESET\]" "$NVX_RUN_LOG" | tail -80 || true
        if grep -q 'NVX \[GR\]: GR submission disabled' "$NVX_RUN_LOG"; then
            echo "GR RESULT: context layout queried; no graphics submission or rendering was tested."
        elif grep -q 'NVX \[GR\]: firmware assertions observed during submission' "$NVX_RUN_LOG"; then
            echo "GR RESULT: firmware assertions occurred during the probe; graphics execution is unverified."
        elif grep -q 'NVX \[GR\]: green triangle pixel readback verified' "$NVX_RUN_LOG"; then
            echo "GR RESULT: hardware triangle draw, green pixel readback, and semaphore completed successfully!"
        elif grep -q 'NVX \[GR\]: black triangle pixel readback verified' "$NVX_RUN_LOG"; then
            echo "GR RESULT: hardware triangle rasterization, black pixel readback, and semaphore completed; texture output is not verified."
        elif grep -q 'NVX \[GR\]: triangle probe produced no verified colored pixels' "$NVX_RUN_LOG"; then
            echo "GR RESULT: clear readback passed, but the triangle produced no verified colored pixels."
        elif grep -qE 'NVX \[GR\]: 3D CLEAR_SURFACE (verified|readback verified)' "$NVX_RUN_LOG"; then
            echo "GR RESULT: 3D rasterizer clear and semaphore completed successfully!"
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

if [ "$NVX_REQUIRE_HW_OPENGL" = "1" ] &&
   ! grep -q '\[OPENGL_RESULT\] RENDERED' "$NVX_RUN_LOG"; then
    echo "ERROR: hardware OpenGL was not proven; see $NVX_RUN_LOG" >&2
    exit 1
fi
