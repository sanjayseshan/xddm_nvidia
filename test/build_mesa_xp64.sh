#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MESON="${MESON:-meson}"
NINJA="${NINJA:-ninja}"
BUILD_DIR="${NVX_MESA_BUILD_DIR:-${TMPDIR:-/tmp}/nvx-mesa-xp64-build}"
DLL="$BUILD_DIR/src/gallium/targets/wgl/nvxogl64.dll"
MESA_WGL_DLL="$BUILD_DIR/src/gallium/targets/libgl-gdi/opengl32.dll"
# The pinned Mesa revision constrains this Meson option to >= 7. The actual
# XP target remains _WIN32_WINNT=0x0502 in mesa_xp64.ini; final PE imports and
# subsystem versions are checked against the XP guest by audit_xp_imports.py.
MESA_MIN_WINDOWS_VERSION=7
MESA_GALLIUM_DRIVERS="${NVX_MESA_GALLIUM_DRIVERS:-softpipe}"

"$ROOT/test/apply_mesa_xp_patches.sh"
restore_mesa_sources() {
    for patch_file in \
        "$ROOT/mesa_patches/0002-nvx-gallium-bridge.patch" \
        "$ROOT/mesa_patches/0001-xp-win32-shims.patch"; do
        if git -C "$ROOT/mesa" apply --unidiff-zero --reverse --check "$patch_file" >/dev/null 2>&1; then
            git -C "$ROOT/mesa" apply --unidiff-zero --reverse "$patch_file"
        fi
    done
}
trap restore_mesa_sources EXIT
if [[ -f "$BUILD_DIR/meson-private/coredata.dat" ]]; then
    "$MESON" setup --reconfigure "$BUILD_DIR" "$ROOT/mesa" \
        --cross-file "$ROOT/test/mesa_xp64.ini" \
        --buildtype=release -Dstrip=true -Dplatforms=windows \
        -Dgallium-drivers="$MESA_GALLIUM_DRIVERS" -Dvulkan-drivers= -Dllvm=disabled \
        -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=true \
        -Dgallium-wgl-dll-name=nvxogl64 -Ddefault_library=static \
        -Dmin-windows-version="$MESA_MIN_WINDOWS_VERSION"
else
    "$MESON" setup "$BUILD_DIR" "$ROOT/mesa" \
        --cross-file "$ROOT/test/mesa_xp64.ini" \
        --buildtype=release -Dstrip=true -Dplatforms=windows \
        -Dgallium-drivers="$MESA_GALLIUM_DRIVERS" -Dvulkan-drivers= -Dllvm=disabled \
        -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=true \
        -Dgallium-wgl-dll-name=nvxogl64 -Ddefault_library=static \
        -Dmin-windows-version="$MESA_MIN_WINDOWS_VERSION"
fi
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
echo "Built XP x64 Mesa softpipe ICD: $ROOT/dist_full/nvxogl64.dll"
echo "Built direct Mesa WGL diagnostic frontend: $ROOT/dist_full/nvxmesa.dll"
echo "Imported DLLs:"
printf '%s\n' "$imports"
