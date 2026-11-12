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

For a short hardware 3D pass, run the same suite in rapid mode. It keeps the
host checks and package audit, then boots XP and runs the bounded C797 clear
and full-size clear matrix. This mode intentionally skips the OpenGL cube
comparison and writes `[MESA_GPU_CUBE_RESULT] SKIPPED reason=rapid_3d_mode` to
the guest log. Run the command without `NVX_RAPID_3D=1` to run the cube tests:

```bash
NVX_RAPID_3D=1 NVX_AUTO_RETRY=1 NVX_GR_CHANNEL_PROBE=1 \
NVX_GR_PROMOTE_PROBE=1 NVX_GR_SUBMISSION_PROBE=1 ./test/test_full_driver.sh
```

The guest test reports Kayfabe-inspired workload gates independently:
resource allocation/release, upload/download round trip, batch and relocation
validation, rejection of malformed packets, native submission/fence/readback
at both 64x16 and 632x453 extents, a four-color translated-clear matrix, and
four native full-size color cases sampled at the origin, center, and far corner.
Require `[NVX_3D_RAPID_RESULT] PASS`; for more detail,
inspect the `[NVX_3D_STAGE]` lines in the generated `test_logs` file. These
tests exercise the existing NVX boundary and fixed C797 clear profile; they do
not test a native Gallium Nouveau screen or arbitrary NVC0 batches.

### 3. `run_vfio_xp_autoretry.sh`
Monitors and auto-retries Windows XP x64 VM bringup with RTX 3070 VFIO passthrough until guest driver activity appears in the serial log. It launches `run_vfio_vm.sh` for every attempt, so it uses the same Logitech evdev input devices, passthrough devices, and QEMU settings as a direct run. Set `MAX_RETRIES` and `BOOT_TIMEOUT` to adjust retry behavior. When run through `test_full_driver.sh`, set `NVX_AUTO_RETRY=1` to enable this path.

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

### 6. VM Launchers (VFIO Passthrough)
- `run_vfio_vm.sh`: Launches Windows XP x64 VM with dedicated RTX 3070 VFIO passthrough and the Logitech keyboard/mouse selected by `NVX_LOGITECH_KBD` and `NVX_LOGITECH_MOUSE` (defaulting to the USB receiver event devices under `/dev/input/by-id/`).
- `run_vfio_vm_win98.sh`: Launches Windows 98 SE VM with dedicated RTX 3070 VFIO passthrough.

### 7. Disk Image Updaters
- `update_disk_driver_user.sh`: Rootless disk updater for `winxp_x64.qcow2` using `guestmount`.
- `update_disk_driver.sh`: Root disk updater for `winxp_x64.qcow2` using NBD.
- `update_win98_driver.sh`: Deploys VxD, display drivers, and INI configuration to `win98_small.img` using `guestfish`.
- `update_xp_driver.sh`: Copies built XP drivers to a mounted Windows XP partition.

### 8. Host GPU / VFIO Management
- `setup-vfio.sh`: Configures modprobe and modules for RTX 3070 VFIO isolation.
- `switch-gpu.sh`: Switches between host display and VFIO passthrough.
- `dynamic_vfio_switch.sh`: Dynamically binds/unbinds RTX 3070 between host drivers and `vfio-pci`.
- `capture_vbios.sh`: Dumps GA104 VBIOS ROM from PCI sysfs node.

### 9. Emulated QEMU Runners (No VFIO / Cirrus)
- `run_test_vm.sh`: Launches basic QEMU VM for Windows XP.
- `run_test_vm_win98.sh`: Launches basic QEMU VM for Windows 98 SE.
