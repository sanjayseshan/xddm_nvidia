#!/usr/bin/env bash
# Collect after normal guest shutdown; keep benchmark output in vm_serial.log.
set -euo pipefail
NVX_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NVX_IMAGE="${NVX_XP32_IMAGE:-$NVX_ROOT/winxp_x32.qcow2}"
NVX_OUTPUT="${NVX_RESULT_LOG:-$NVX_ROOT/vm_serial.log}"
qemu-img info "$NVX_IMAGE" >/dev/null
NVX_COLLECT_DIR="$(mktemp -d /tmp/nvx-graphics-results.XXXXXX)"
trap 'rm -rf "$NVX_COLLECT_DIR"' EXIT
guestfish --ro -a "$NVX_IMAGE" -i <<GUEST
download /graphics_test_results.txt "$NVX_COLLECT_DIR/results.txt"
download /test_accel.log "$NVX_COLLECT_DIR/suite.log"
download /wglgears_console.log "$NVX_COLLECT_DIR/gears.log"
GUEST
python3 - "$NVX_COLLECT_DIR" "$NVX_OUTPUT" <<'PY'
from pathlib import Path
import sys
folder, output = map(Path, sys.argv[1:])
with output.open('a') as log:
    log.write('\n===== Guest graphics logs collected after shutdown =====\n')
    for name in ('results.txt', 'suite.log', 'gears.log'):
        log.write('\n--- '+name+' ---\n')
        log.write((folder/name).read_text(errors='replace'))
print('Appended guest graphics results to', output)
PY
