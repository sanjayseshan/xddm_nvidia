#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 /path/to/nvxogl64.dll" >&2
  exit 2
fi
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cc=${MINGW_CC:-x86_64-w64-mingw32-gcc}
wine=${WINE:-wine}
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/nvx-mesa-wgl-load.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

"$cc" -static -O2 -o "$tmp_dir/mesa_wgl_load_smoke.exe" \
  "$repo_root/test/mesa_wgl_load_smoke.c"
WINEDEBUG=${WINEDEBUG:--all} "$wine" \
  "$tmp_dir/mesa_wgl_load_smoke.exe" "$1"
