#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cc=${MINGW_CC:-x86_64-w64-mingw32-gcc}
wine=${WINE:-wine}
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/nvx-mesa-xp-shim.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

"$repo_root/test/apply_mesa_xp_patches.sh"
"$cc" -static -O2 -o "$tmp_dir/mesa_xp_shim_smoke.exe" \
  "$repo_root/test/mesa_xp_shim_smoke.c"
WINEDEBUG=${WINEDEBUG:--all} "$wine" "$tmp_dir/mesa_xp_shim_smoke.exe"
