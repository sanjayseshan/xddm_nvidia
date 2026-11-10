#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mesa_dir="$repo_root/mesa"
patch_files="$repo_root/mesa_patches/0001-xp-win32-shims.patch
$repo_root/mesa_patches/0002-nvx-gallium-bridge.patch"
expected_revision=cc2dcf9abdc6afc093e85d48e4cfa34eed60a01e

actual_revision=$(git -C "$mesa_dir" rev-parse HEAD)
if [ "$actual_revision" != "$expected_revision" ]; then
    echo "Mesa revision mismatch: expected $expected_revision, got $actual_revision" >&2
    exit 1
fi

printf '%s\n' "$patch_files" | while IFS= read -r patch_file; do
    if git -C "$mesa_dir" apply --unidiff-zero --reverse --check "$patch_file" >/dev/null 2>&1; then
        echo "$(basename "$patch_file") already applied."
        continue
    fi

    git -C "$mesa_dir" apply --unidiff-zero --check "$patch_file"
    git -C "$mesa_dir" apply --unidiff-zero "$patch_file"
    echo "Applied $(basename "$patch_file")."
done
