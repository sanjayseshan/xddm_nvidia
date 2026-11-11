#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MESON="${MESON:-meson}"
NINJA="${NINJA:-ninja}"
BUILD_DIR="${NVX_MESA_BUILD_DIR:-${TMPDIR:-/tmp}/nvx-mesa-xp64-build}"
XP_CROSS_FILE="$BUILD_DIR/mesa_xp64.ini"
DLL="$BUILD_DIR/src/gallium/targets/wgl/nvxogl64.dll"
MESA_WGL_DLL="$BUILD_DIR/src/gallium/targets/libgl-gdi/opengl32.dll"
# The external XP patch lowers Mesa's minimum-version option to 5 (Windows XP).
# The cross file sets _WIN32_WINNT/WINVER to 0x0502 for XP x64; final PE imports
# and subsystem versions are checked by audit_xp_imports.py.
MESA_MIN_WINDOWS_VERSION=5
MESA_GALLIUM_DRIVERS="${NVX_MESA_GALLIUM_DRIVERS:-softpipe}"

mkdir -p "$BUILD_DIR"
python3 - "$ROOT/test/mesa_xp64.ini" "$XP_CROSS_FILE" \
    "$ROOT/mesa_patches/xp_include" <<'PY'
from pathlib import Path
import sys

source, destination, include_dir = sys.argv[1:]
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
PY

"$ROOT/test/apply_mesa_xp_patches.sh"
restore_mesa_sources() {
    rm -f "$ROOT/mesa/src/gallium/winsys/nouveau/drm/nvx_xf86drm.c"
    for patch_file in \
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
if [[ -e "$ROOT/mesa/src/gallium/winsys/nouveau/drm/nvx_xf86drm.c" ]]; then
    echo "Refusing to overwrite existing Nouveau source: nvx_xf86drm.c" >&2
    exit 1
fi
cp "$ROOT/mesa_patches/nvx_xf86drm.c" \
   "$ROOT/mesa/src/gallium/winsys/nouveau/drm/nvx_xf86drm.c"
if [[ -f "$BUILD_DIR/meson-private/coredata.dat" ]]; then
    "$MESON" setup --reconfigure "$BUILD_DIR" "$ROOT/mesa" \
        --cross-file "$XP_CROSS_FILE" \
        --buildtype=release -Dstrip=true -Dplatforms=windows \
        -Dgallium-drivers="$MESA_GALLIUM_DRIVERS" -Dvulkan-drivers= -Dllvm=disabled \
        -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=true \
        -Dgallium-wgl-dll-name=nvxogl64 -Ddefault_library=static \
        -Dmin-windows-version="$MESA_MIN_WINDOWS_VERSION"
else
    "$MESON" setup "$BUILD_DIR" "$ROOT/mesa" \
        --cross-file "$XP_CROSS_FILE" \
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
echo "Built XP x64 Mesa WGL ICD ($MESA_GALLIUM_DRIVERS): $ROOT/dist_full/nvxogl64.dll"
echo "Built direct Mesa WGL diagnostic frontend: $ROOT/dist_full/nvxmesa.dll"
echo "Imported DLLs:"
printf '%s\n' "$imports"
