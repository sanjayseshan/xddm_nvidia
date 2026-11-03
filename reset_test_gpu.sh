#!/usr/bin/env python3
"""Reset the idle RTX-only PCI hierarchy using the kernel bridge reset API."""
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
bridge = gpu.resolve().parent
require((bridge/'class').read_text().strip() == '0x060400',
        'Refusing: GPU parent is not a PCI bridge.')
# IOMMU isolation alone does not establish the scope of a bridge reset.
# Inspect all PCI descendants, including devices behind nested bridges.
descendants = sorted(p.name for p in pci.iterdir()
                     if p.resolve() != bridge and p.resolve().is_relative_to(bridge))
require(descendants == functions,
        'Refusing: bridge contains devices outside the two RTX functions: ' + str(descendants))
reset = bridge/'reset_subordinate'
require(reset.exists(), 'Refusing: kernel bridge reset_subordinate API is unavailable.')
print('Verified isolated VFIO RTX 3070 at 01:00.0/.1. Reset methods:', saved)
print('Verified RTX-only bridge:', bridge.name)
if sys.argv[1:] == ['--check']:
    print('Read-only check completed; no reset performed.')
    raise SystemExit(0)
require(os.geteuid() == 0, 'Root is required for PCI reset. Run: sudo ./reset_test_gpu.sh')
subprocess.run(['qemu-img', 'info', str(workspace/'winxp_x64.qcow2')],
               check=True, stdout=subprocess.DEVNULL)
group = (gpu/'iommu_group').resolve().name
busy = subprocess.run(['fuser', '/dev/vfio/'+group], capture_output=True)
require(busy.returncode == 1, 'Refusing: VFIO group is in use or its users cannot be checked.')
# reset_method's per-function bus probe rejects multifunction siblings. The
# bridge API saves/restores PCI state for the complete subordinate hierarchy.
# Do not change reset_method preferences, unbind drivers, or poke bridge config.
try:
    fd = os.open(reset, os.O_WRONLY)
    try:
        count = os.write(fd, b'1\n')
        if count != 2:
            raise OSError('short sysfs reset write; reset outcome is unknown')
    finally:
        os.close(fd)
except OSError as error:
    raise SystemExit('RTX bridge reset failed: ' + str(error) +
                     '. Do not assume the GPU was reset. Reset preferences were not changed.')
print('RTX PCI bridge reset completed. You can now run ./test_full_driver.sh')
