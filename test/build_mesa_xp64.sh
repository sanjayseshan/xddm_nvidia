#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MESON="${MESON:-/tmp/nvx-meson-venv/bin/meson}"
if [ ! -x "$MESON" ] && command -v meson >/dev/null 2>&1; then
    MESON="$(command -v meson)"
fi
NINJA="${NINJA:-ninja}"
BUILD_DIR="${NVX_MESA_BUILD_DIR:-${TMPDIR:-/tmp}/nvx-mesa-xp64-build}"
XP_CROSS_FILE="${BUILD_DIR}.ini"
DLL="$BUILD_DIR/src/gallium/targets/wgl/nvxogl64.dll"
MESA_WGL_DLL="$BUILD_DIR/src/gallium/targets/libgl-gdi/opengl32.dll"
# The external XP patch lowers Mesa's minimum-version option to 5 (Windows XP).
# The cross file sets _WIN32_WINNT/WINVER to 0x0502 for XP x64; final PE imports
# and subsystem versions are checked by audit_xp_imports.py.
MESA_MIN_WINDOWS_VERSION=5
# Include Nouveau in the normal XP ICD package so WGL exercises the native
# Gallium screen factory. Softpipe remains available as a deliberate fallback
# while the NVX channel/submission boundary is being brought up.
MESA_GALLIUM_DRIVERS="${NVX_MESA_GALLIUM_DRIVERS:-nouveau,softpipe}"
MESA_VULKAN_DRIVERS="${NVX_MESA_VULKAN_DRIVERS:-}"
MESA_CLC="${NVX_MESA_CLC:-auto}"
MESON_VERSION_FILE="$BUILD_DIR/.nvx-meson-version"
MESON_VERSION="$($MESON --version)"

mkdir -p "$BUILD_DIR"
python3 - "$ROOT/test/mesa_xp64.ini" "$XP_CROSS_FILE" \
    "$ROOT/mesa_patches/xp_include" "${NVX_RUSTC:-}" \
    "${NVX_RUST_TARGET:-x86_64-pc-windows-gnu}" <<'PY'
from pathlib import Path
import sys

source, destination, include_dir, rustc, rust_target = sys.argv[1:]
text = Path(source).read_text()
for option in ("c_args", "cpp_args"):
    prefix = f"{option} = ["
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if line.startswith(prefix):
            lines[index] = line[:-1] + f", '-I{include_dir}']"
            break
    else:
        raise SystemExit(f"missing {option} in Mesa cross file")
    text = "\n".join(lines) + "\n"
Path(destination).write_text(text)
if rustc:
    text = Path(destination).read_text()
    text = text.replace('[binaries]\n',
                        '[binaries]\nrust = ' +
                        repr([rustc, '--target', rust_target]) + '\n', 1)
    Path(destination).write_text(text)
PY

restore_mesa_sources() {
    rm -f "$ROOT/mesa/src/gallium/winsys/nouveau/drm/nvx_xf86drm.c"
    for patch_file in \
        "$ROOT/mesa_patches/0026-nouveau-xp-render-target-trace.patch" \
        "$ROOT/mesa_patches/0025-nouveau-xp-force-cpu-prep.patch" \
        "$ROOT/mesa_patches/0024-nouveau-xp-linear-kind-tiled-depth.patch" \
        "$ROOT/mesa_patches/0023-nouveau-xp-wgl-gart-targets.patch" \
        "$ROOT/mesa_patches/0022-nouveau-xp-miptree-allocation-trace.patch" \
        "$ROOT/mesa_patches/0021-nouveau-xp-swap-path-diagnostics.patch" \
        "$ROOT/mesa_patches/0020-nouveau-xp-async-swap.patch" \
        "$ROOT/mesa_patches/0019-nouveau-xp-preserve-pushbuf-limits-on-error.patch" \
        "$ROOT/mesa_patches/0018-nouveau-xp-pushbuf-validation-trace.patch" \
        "$ROOT/mesa_patches/0017-nouveau-xp-nvc0-tsc-stage-trace.patch" \
        "$ROOT/mesa_patches/0016-nouveau-xp-context-stage-trace.patch" \
        "$ROOT/mesa_patches/0014-nouveau-xp-screen-failure-trace.patch" \
        "$ROOT/mesa_patches/0015-nouveau-xp-winsys-init-stage-trace.patch" \
        "$ROOT/mesa_patches/0013-nouveau-xp-flush-mapped-pushbuf.patch" \
        "$ROOT/mesa_patches/0012-nouveau-xp-nvx-bo-map.patch" \
        "$ROOT/mesa_patches/0011-nouveau-use-os-mman.patch" \
        "$ROOT/mesa_patches/0010-nouveau-xp-disable-drm-sharing.patch" \
        "$ROOT/mesa_patches/0009-nouveau-xp-mmap-unsupported.patch" \
        "$ROOT/mesa_patches/0008-nouveau-xp-hdc-entry.patch" \
        "$ROOT/mesa_patches/0007-nouveau-xp-pointer-width.patch" \
        "$ROOT/mesa_patches/0006-nouveau-xp-xf86drm-compat.patch" \
        "$ROOT/mesa_patches/0005-wgl-nouveau-present.patch" \
        "$ROOT/mesa_patches/0004-nouveau-wgl-drop-unused-renderonly.patch" \
        "$ROOT/mesa_patches/0003-nouveau-wgl-exclude-nvk-winsys.patch" \
        "$ROOT/mesa_patches/0002-nvx-gallium-bridge.patch" \
        "$ROOT/mesa_patches/0001-xp-win32-shims.patch"; do
        if git -C "$ROOT/mesa" apply --unidiff-zero --reverse --check "$patch_file" >/dev/null 2>&1; then
            git -C "$ROOT/mesa" apply --unidiff-zero --reverse "$patch_file"
        fi
    done
}
trap restore_mesa_sources EXIT
"$ROOT/test/apply_mesa_xp_patches.sh"
if [[ -e "$ROOT/mesa/src/gallium/winsys/nouveau/drm/nvx_xf86drm.c" ]]; then
    echo "Refusing to overwrite existing Nouveau source: nvx_xf86drm.c" >&2
    exit 1
fi
cp "$ROOT/mesa_patches/nvx_xf86drm.c" \
   "$ROOT/mesa/src/gallium/winsys/nouveau/drm/nvx_xf86drm.c"
if [[ -f "$BUILD_DIR/meson-private/coredata.dat" &&
      -f "$MESON_VERSION_FILE" &&
      "$(cat "$MESON_VERSION_FILE")" == "$MESON_VERSION" ]]; then
    "$MESON" setup --reconfigure "$BUILD_DIR" "$ROOT/mesa" \
        --cross-file "$XP_CROSS_FILE" \
        --buildtype=release -Dstrip=true -Dplatforms=windows \
        -Dgallium-drivers="$MESA_GALLIUM_DRIVERS" -Dvulkan-drivers="$MESA_VULKAN_DRIVERS" -Dllvm=disabled \
        -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=true \
        -Dmesa-clc="$MESA_CLC" \
        -Dgallium-wgl-dll-name=nvxogl64 -Ddefault_library=static \
        -Dmin-windows-version="$MESA_MIN_WINDOWS_VERSION"
elif [[ -f "$BUILD_DIR/meson-private/coredata.dat" ]]; then
    # Meson can regenerate coredata from an older executable while retaining
    # stale option metadata. Wipe once when the Meson version changes so XP's
    # patched min-windows-version range is read from the current source tree.
    "$MESON" setup --wipe "$BUILD_DIR" "$ROOT/mesa" \
        --cross-file "$XP_CROSS_FILE" \
        --buildtype=release -Dstrip=true -Dplatforms=windows \
        -Dgallium-drivers="$MESA_GALLIUM_DRIVERS" -Dvulkan-drivers="$MESA_VULKAN_DRIVERS" -Dllvm=disabled \
        -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=true \
        -Dmesa-clc="$MESA_CLC" \
        -Dgallium-wgl-dll-name=nvxogl64 -Ddefault_library=static \
        -Dmin-windows-version="$MESA_MIN_WINDOWS_VERSION"
else
    "$MESON" setup "$BUILD_DIR" "$ROOT/mesa" \
        --cross-file "$XP_CROSS_FILE" \
        --buildtype=release -Dstrip=true -Dplatforms=windows \
        -Dgallium-drivers="$MESA_GALLIUM_DRIVERS" -Dvulkan-drivers="$MESA_VULKAN_DRIVERS" -Dllvm=disabled \
        -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=true \
        -Dmesa-clc="$MESA_CLC" \
        -Dgallium-wgl-dll-name=nvxogl64 -Ddefault_library=static \
        -Dmin-windows-version="$MESA_MIN_WINDOWS_VERSION"
fi
printf '%s\n' "$MESON_VERSION" > "$MESON_VERSION_FILE"
"$NINJA" -C "$BUILD_DIR" -j"${JOBS:-4}"

if [[ ! -s "$DLL" ]]; then
    echo "Mesa WGL ICD was not produced: $DLL" >&2
    exit 1
fi
if [[ ! -s "$MESA_WGL_DLL" ]]; then
    echo "Mesa WGL frontend was not produced: $MESA_WGL_DLL" >&2
    exit 1
fi
imports=$(x86_64-w64-mingw32-objdump -p "$DLL" | sed -n 's/^[[:space:]]*DLL Name: //p')
if printf '%s\n' "$imports" | grep -Eiq 'api-ms-win|winpthread|libz'; then
    echo "Mesa ICD has non-XP runtime imports:" >&2
    printf '%s\n' "$imports" >&2
    exit 1
fi

install -m 0644 "$DLL" "$ROOT/dist_full/nvxogl64.dll"
install -m 0644 "$MESA_WGL_DLL" "$ROOT/dist_full/nvxmesa.dll"
echo "Built XP x64 Mesa WGL ICD ($MESA_GALLIUM_DRIVERS): $ROOT/dist_full/nvxogl64.dll"
echo "Built direct Mesa WGL diagnostic frontend: $ROOT/dist_full/nvxmesa.dll"
echo "Imported DLLs:"
printf '%s\n' "$imports"
