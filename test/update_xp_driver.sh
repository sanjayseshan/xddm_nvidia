#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -z "${XP_MOUNT:-}" ]]; then
    XP_MOUNT="/media/sanjay/winxp1"
    if [[ ! -d "$XP_MOUNT/WINDOWS/system32/drivers" ]] && command -v findmnt >/dev/null 2>&1; then
        XP_MOUNT="$(findmnt -rn -S /dev/sda3 -o TARGET | head -n 1 || true)"
    fi
fi
DRIVER_SOURCE="$ROOT/src_driver/nvxmp.sys"
DISPLAY_SOURCE="$ROOT/src_disp/nvxdisp.dll"
DRIVER_DEST="$XP_MOUNT/WINDOWS/system32/drivers/nvxmp.sys"
DISPLAY_DEST="$XP_MOUNT/WINDOWS/system32/nvxdisp.dll"
LOG_DEST="$XP_MOUNT/WINDOWS/nvxmp.log"
DISPLAY_LOG_DEST="$XP_MOUNT/WINDOWS/nvxdisp.log"

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "Missing x86_64-w64-mingw32-gcc; install the MinGW-w64 x86-64 toolchain." >&2
    exit 1
fi

make -C "$ROOT/src_driver" full
make -C "$ROOT/src_disp"

if [[ ! -d "$XP_MOUNT/WINDOWS/system32/drivers" ]]; then
    echo "XP Windows directory not found under $XP_MOUNT" >&2
    exit 1
fi
if [[ ! -w "$XP_MOUNT/WINDOWS/system32/drivers" ]]; then
    echo "XP mount is not writable: $XP_MOUNT" >&2
    echo "Remount the clean, fully shut down NTFS volume read-write, then rerun this script." >&2
    exit 1
fi

if command -v findmnt >/dev/null 2>&1; then
    MOUNT_OPTIONS="$(findmnt -rn -T "$XP_MOUNT" -o OPTIONS || true)"
    if [[ ",$MOUNT_OPTIONS," == *,ro,* ]]; then
        echo "XP mount is read-only: $XP_MOUNT" >&2
        echo "Remount the clean, fully shut down NTFS volume read-write, then rerun this script." >&2
        exit 1
    fi
fi

install -m 0644 "$DRIVER_SOURCE" "$DRIVER_DEST"
install -m 0644 "$DISPLAY_SOURCE" "$DISPLAY_DEST"
install -m 0644 "$DRIVER_SOURCE" "$ROOT/dist_full/nvxmp.sys"
install -m 0644 "$DISPLAY_SOURCE" "$ROOT/dist_full/nvxdisp.dll"

echo "Installed miniport: $DRIVER_DEST"
echo "Installed display DLL: $DISPLAY_DEST"
echo "Diagnostic log on next XP boot: $LOG_DEST"
echo "Independent display-DDI log: $DISPLAY_LOG_DEST"
sha256sum "$DRIVER_SOURCE" "$DRIVER_DEST" "$DISPLAY_SOURCE" "$DISPLAY_DEST"
cmp -s "$DRIVER_SOURCE" "$DRIVER_DEST"
cmp -s "$DISPLAY_SOURCE" "$DISPLAY_DEST"
echo "Installed binaries match the just-built workspace binaries."
