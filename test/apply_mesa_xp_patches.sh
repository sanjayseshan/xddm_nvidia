#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mesa_dir="$repo_root/mesa"
patch_file="$repo_root/mesa_patches/0001-xp-win32-shims.patch"
expected_revision=cc2dcf9abdc6afc093e85d48e4cfa34eed60a01e

actual_revision=$(git -C "$mesa_dir" rev-parse HEAD)
if [ "$actual_revision" != "$expected_revision" ]; then
    echo "Mesa revision mismatch: expected $expected_revision, got $actual_revision" >&2
    exit 1
fi

if git -C "$mesa_dir" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
    echo "Mesa XP compatibility patch already applied."
    exit 0
fi

git -C "$mesa_dir" apply --check "$patch_file"
git -C "$mesa_dir" apply "$patch_file"
echo "Applied Mesa XP compatibility patch."
