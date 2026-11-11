#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mesa_dir="$repo_root/mesa"
patch_files="$repo_root/mesa_patches/0001-xp-win32-shims.patch
$repo_root/mesa_patches/0002-nvx-gallium-bridge.patch
$repo_root/mesa_patches/0003-nouveau-wgl-exclude-nvk-winsys.patch
$repo_root/mesa_patches/0004-nouveau-wgl-drop-unused-renderonly.patch
$repo_root/mesa_patches/0005-wgl-nouveau-present.patch
$repo_root/mesa_patches/0006-nouveau-xp-xf86drm-compat.patch
$repo_root/mesa_patches/0007-nouveau-xp-pointer-width.patch
$repo_root/mesa_patches/0008-nouveau-xp-hdc-entry.patch
$repo_root/mesa_patches/0009-nouveau-xp-mmap-unsupported.patch
$repo_root/mesa_patches/0010-nouveau-xp-disable-drm-sharing.patch
$repo_root/mesa_patches/0011-nouveau-use-os-mman.patch
$repo_root/mesa_patches/0012-nouveau-xp-nvx-bo-map.patch
$repo_root/mesa_patches/0013-nouveau-xp-flush-mapped-pushbuf.patch"
expected_revision=cc2dcf9abdc6afc093e85d48e4cfa34eed60a01e

actual_revision=$(git -C "$mesa_dir" rev-parse HEAD)
if [ "$actual_revision" != "$expected_revision" ]; then
    echo "Mesa revision mismatch: expected $expected_revision, got $actual_revision" >&2
    exit 1
fi

if [ -n "$(git -C "$mesa_dir" status --porcelain)" ]; then
    echo "Mesa submodule must be clean before applying XP patches." >&2
    exit 1
fi

printf '%s\n' "$patch_files" | while IFS= read -r patch_file; do
    git -C "$mesa_dir" apply --unidiff-zero --check "$patch_file"
    git -C "$mesa_dir" apply --unidiff-zero "$patch_file"
    echo "Applied $(basename "$patch_file")."
done
