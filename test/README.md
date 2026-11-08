# Test Infrastructure

This directory contains the automated test suites, host unit tests, and virtualized runner scripts for testing the NVIDIA XDDM and VxD display drivers.

## Test Suites & Scripts

### 1. `run_all_tests.sh`
Executes all host unit tests and builds both 64-bit and 32-bit drivers in one step:
- Driver host unit tests (`src_driver/tests/`): MMU, display timing, sequencer, RPC, firmware, GSP mock, VBE, OpenRM HAL shim, VBIOS BIT/Falcon parser.
- Miniport build: `nvxmp.sys` (64-bit).
- Display driver build: `nvxdisp.dll` (64-bit).
- VxD driver build: `nvx.vxd` (32-bit).

```bash
./test/run_all_tests.sh
```

### 2. `test_full_driver.sh`
Performs end-to-end integration testing:
- Runs host unit tests and compiles production driver binaries.
- Synchronizes drivers to the offline `winxp_x64.qcow2` disk image without root permissions.
- Launches the VM session and parses hardware verification logs.

```bash
./test/test_full_driver.sh
```

### 3. `run_vfio_xp_autoretry.sh`
Monitors and auto-retries Windows XP x64 VM bringup with RTX 3070 VFIO passthrough until active driver boot and dual-display configuration is confirmed via the serial log.

```bash
./test/run_vfio_xp_autoretry.sh
```

### 4. `run_vfio_win98_autoretry.sh`
Monitors and auto-retries Windows 98 SE VM bringup with RTX 3070 VFIO passthrough until Ring 0 VxD and DIB engine initialization is confirmed via the serial log.

```bash
./test/run_vfio_win98_autoretry.sh
```

### 5. `reset_test_gpu.sh`
Python script for cycling PCIe power state and verifying isolated VFIO bindings for the RTX 3070 GPU hierarchy.

```bash
sudo ./test/reset_test_gpu.sh
# Or read-only verification:
./test/reset_test_gpu.sh --check
```

### 6. Emulated QEMU Runners (No VFIO / Cirrus)
- `run_test_vm.sh`: Launches basic QEMU VM for Windows XP.
- `run_test_vm_win98.sh`: Launches basic QEMU VM for Windows 98 SE.
