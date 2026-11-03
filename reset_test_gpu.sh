#!/usr/bin/env python3
"""Reset only the idle VFIO RTX 3070; restore reset-method preference afterward."""
from pathlib import Path
import os
import subprocess
import sys

workspace = Path(__file__).resolve().parent
pci = Path('/sys/bus/pci/devices')
gpu = pci / '0000:01:00.0'
functions = ['0000:01:00.0', '0000:01:00.1']

def require(condition, message):
    if not condition:
        raise SystemExit(message)

require(sys.argv[1:] in ([], ['--check']), 'Usage: reset_test_gpu.sh [--check]')
require((gpu/'vendor').read_text().strip() == '0x10de', 'Refusing: device is not NVIDIA.')
require((gpu/'device').read_text().strip() == '0x2488', 'Refusing: device is not the expected RTX 3070.')
require(sorted(p.name for p in (gpu/'iommu_group/devices').iterdir()) == functions,
        'Refusing: IOMMU group includes devices outside the test GPU.')
for name in functions:
    require((pci/name/'driver').resolve().name == 'vfio-pci',
            'Refusing: both GPU functions must be bound to vfio-pci.')
methods = gpu/'reset_method'
saved = methods.read_text().strip()
require('bus' in saved.split(), 'Refusing: PCI bus reset is unavailable.')
print('Verified isolated VFIO RTX 3070 at 01:00.0/.1. Reset methods:', saved)
if sys.argv[1:] == ['--check']:
    print('Read-only check completed; no reset performed.')
    raise SystemExit(0)
require(os.geteuid() == 0, 'Root is required for PCI reset. Run: sudo ./reset_test_gpu.sh')
subprocess.run(['qemu-img', 'info', str(workspace/'winxp_x64.qcow2')],
               check=True, stdout=subprocess.DEVNULL)
group = (gpu/'iommu_group').resolve().name
busy = subprocess.run(['fuser', '/dev/vfio/'+group], capture_output=True)
require(busy.returncode == 1, 'Refusing: VFIO group is in use or its users cannot be checked.')
try:
    methods.write_text('bus')
    (gpu/'reset').write_text('1')
    print('RTX PCI bus reset completed. You can now run ./test_full_driver.sh')
finally:
    methods.write_text(saved)
