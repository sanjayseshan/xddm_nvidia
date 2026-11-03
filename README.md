# NVIDIA GA104 XDDM driver for Windows XP x64

This experimental driver brings up a Windows XP x64 desktop on NVIDIA Ampere
GA104 hardware. The current confirmed checkpoint is v164, with working native
output on two displays attached to an EVGA GeForce RTX 3070 XC3 Ultra LHR:
1920×1080 on the primary and 2560×1440 on the secondary.

## Project overview

The project contains a Windows XP/Server 2003 x64 XDDM video miniport
(`nvxmp.sys`) and GDI display driver (`nvxdisp.dll`). The miniport starts the
NVIDIA GSP firmware and configures native display heads. A GSP-managed GPU copy
engine uploads desktop framebuffer changes to separate scanout buffers for
each display.

GDI rasterization still happens in software on the CPU. GPU DMA is used to
present the desktop; this is not accelerated GDI rendering or a complete
graphics driver. OpenGL, Direct3D, video decoding, and 3D acceleration are not
implemented.

The current v164 confirmation is recorded in [SUMMARY.txt](SUMMARY.txt). The
matching NVIDIA firmware source and checksums are documented in
[FIRMWARE_PROVENANCE.md](FIRMWARE_PROVENANCE.md).

## Compatibility and limits

- **Guest OS:** Windows XP Professional x64 Edition (NT 5.2). Windows Server
  2003 x64 shares the target driver model but has not been validated here.
- **Tested GPU:** EVGA GeForce RTX 3070 XC3 Ultra LHR, GA104, PCI ID
  `10de:2488`, passed through to the guest with VFIO.
- **Tested display setup:** two DisplayPort displays, 1920×1080 primary and
  2560×1440 secondary. The tested EDID timings are approximately 60 Hz.
- **Not established:** 120 Hz operation, arbitrary resolutions, other Ampere
  boards, other GPU generations, HDMI, hot-plug/reconfiguration, accelerated
  GDI, OpenGL, or Direct3D.
- The kernel driver is experimental and unsigned. Use a disposable XP guest
  and a GPU reserved for passthrough. The test script writes the newly built
  driver into the configured guest disk before launching it.

## Build

On a Debian or Ubuntu x86-64 build host, install the cross compiler, linker
utilities, and host C compiler:

```sh
sudo apt-get install build-essential gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64
```

The driver links the NVIDIA GSP-RM firmware from
`dist_full/gsp_ga104.bin`. The repository currently includes the firmware
input used by the confirmed build; its NVIDIA release, source URL, and SHA-256
are in [FIRMWARE_PROVENANCE.md](FIRMWARE_PROVENANCE.md). Do not combine it with
firmware from a different NVIDIA driver release.

Build the miniport and display driver:

```sh
make -C src_driver full
make -C src_disp
```

The resulting files are `src_driver/nvxmp.sys` and `src_disp/nvxdisp.dll`.
The display driver's import library is generated from the checked-in
`src_disp/win32k.def`. The XP package install file is `dist_full/nvx.inf`.
`make -C src_driver check` runs the host-side parser, memory, RPC, display,
firmware, and mock GSP checks. It requires the matching GSP firmware input.

## Hardware test execution

The automated test path builds the drivers, copies them into an offline XP
guest disk, then starts QEMU/KVM with the RTX 3070 assigned through VFIO:

```sh
./test_full_driver.sh
```

It requires a prepared `winxp_x64.qcow2`, QEMU/KVM, VFIO access, and the
Linux guest-mount tools (`guestmount`, `guestunmount`, `mountpoint`, and
`qemu-img`). The launcher in this repository is configured for GPU PCI
function `01:00.0` and a particular Logitech USB input receiver; edit
`run_vfio_vm.sh` for another host. Do not attach the same QCOW image to two
QEMU processes. Logs are written under `test_logs/` and `vm_serial.log` points
to the latest run.

For unprivileged test execution on a Debian or Ubuntu host, install the VM and
guest-mount tools and grant the test user access to the relevant device
groups:

```sh
sudo apt-get install qemu-system-x86 qemu-utils libguestfs-tools
sudo usermod -aG kvm,input "$USER"
```

VFIO group nodes and the selected input devices also need user permissions.
For a dedicated lab host, a udev rule can grant access to the `kvm` and
`input` groups:

```udev
KERNEL=="vfio", SUBSYSTEM=="misc", GROUP="kvm", MODE="0660"
SUBSYSTEM=="vfio", GROUP="kvm", MODE="0660"
SUBSYSTEM=="input", KERNEL=="event*", GROUP="input", MODE="0660"
```

Save this as `/etc/udev/rules.d/70-nvx-test.rules`, reload udev rules, and log
out and in again. The input rule grants access to all input event devices;
limit it to the selected devices on shared systems. The scripts assume the
RTX 3070 functions are `01:00.0` and `01:00.1`. They must be bound to
`vfio-pci`, with the host GPU display output handled by another adapter. Check
`ulimit -l`; the default 3072 MiB guest requires at least 3.25 GiB of locked
memory. `NVX_VM_MEMORY_MB` can lower guest RAM if the host limit is smaller.

The test script's guest-disk update is guarded against a live QEMU write lock.
A responding XP guest should be shut down with `system_powerdown` through the
QEMU monitor. `setup-vfio.sh` changes host boot configuration and requires
root; review it before use.

The test script does not generate an ISO. An existing ISO can be selected with
`NVX_DRIVER_ISO=/path/to/driver.iso`; the guest disk itself receives the
current driver files before launch.

## Repository contents

- `src_driver/`: XP x64 video miniport, GSP/RPC/MMU/display code, and host
  checks.
- `src_disp/`: XDDM GDI display driver.
- `dist_full/nvx.inf`: install configuration for the full driver package.
- `test_full_driver.sh`, `run_vfio_vm.sh`, `update_disk_driver_user.sh`:
  hardware test and guest-update flow.
- `SUMMARY.txt`: single cumulative project status and changelog.

Generated `.sys`, `.dll`, object files, per-version snapshots, VM disks, logs,
and ISOs are intentionally excluded from version control.

## Credits

- **NVIDIA** for the GA10x GSP-RM and SEC2 firmware inputs and the published
  535.113.01 release.
- **NVIDIA Open GPU Kernel Modules** and **Linux Nouveau** for public reference
  material on GSP, memory management, copy engines, and native display methods.
- **GPT and Gemini** models assisted with code analysis, implementation, and
  debugging.
- **Sanjay Seshan** performed the physical GPU and monitor tests and confirmed
  the v164 dual-display result.

## License and firmware

No project-wide open-source license has been selected. The NVIDIA firmware
files remain NVIDIA components and are subject to their own terms. See
[FIRMWARE_PROVENANCE.md](FIRMWARE_PROVENANCE.md) before redistribution.
