# NVIDIA Modern GPU Display Drivers for Windows XP (x86 & x64) & Windows 98 SE (Version 2.0.0.3)

XP x86 bringup (November 8): the display DLL loading failure was fixed by
removing mixed `NTOSKRNL.EXE`/`WIN32K.SYS` imports. A second fix commits native
scanout after primary surface mapping, correcting a missing step in the x86
path. VM logs now verify a 1080p surface, both display links, and GPU desktop
operations. The user confirmed both monitors display correctly on XP 32-bit
(primary 1080p, secondary 1440p), with the earlier striping cleared. This
confirmation is for the RTX 3070 VFIO VM; XP x64 also has prior bare-metal
validation. See `SUMMARY.txt` for test evidence.


This repository provides open experimental display drivers bringing modern **NVIDIA Turing, Ampere, and Ada Lovelace GPUs** (GeForce RTX 3070, RTX 3060 Ti, and 200+ GPUs in the upstream database) to legacy Windows operating systems:
1. **Windows XP Professional 32-bit (x86)** (XDDM 32-bit driver model, NT 5.1)
2. **Windows XP Professional x64 Edition** (XDDM 64-bit driver model, NT 5.2)
3. **Windows 98 Second Edition (SE)** (Ring 0 VxD + 16-bit DIB Engine driver model)

### Version 2.0.0.3 Architecture & Capabilities
Version 2 unifies modern NVIDIA GPU support across x86, x64, and Win9x platforms, integrating NVIDIA's official open-source kernel repository as a git submodule (`open-gpu-kernel-modules`, branch `535.113.01`):
- **Windows XP 32-bit (x86) Support:**
  - 32-bit XDDM video miniport driver (`nvxmp32.sys`) compiled with stdcall calling conventions.
  - Runtime hotpatch for 32-bit `videoprt.sys` `VpIsMultiFunctionSupported` (offset `0xed8c`), bypassing legacy PCI multifunction restrictions (Code 10 fix).
  - Authentic GSP-RM & SEC2 Falcon firmware bootstrap in 32-bit kernel mode with direct native GA104 modesetting.
  - 32-bit GDI display driver (`nvxdisp32.dll`) adhering to Windows XP SP3 `win32k.sys` DDI driver specifications (`DDI_DRIVER_VERSION_NT4` / `0x20000` handshake).
- **Upstream OpenRM HAL Shim (`nvx_openrm_shim.c` / `nvx_openrm_shim.h`):** Provides a clean Hardware Abstraction Layer mapping VideoPort (NT/2000/XP 32-bit & 64-bit) and VMM (Win98 SE VxD) to OpenRM primitives (`GPU_REG_RD32`/`GPU_REG_WR32`, memory manipulation, microsecond stalls, and GSP doorbell notifications).
- **Official OpenRM Message Queue (`msgq.c`):** Directly compiles and links upstream `src/common/shared/msgq/msgq.c` into both 64-bit and 32-bit driver binaries for bidirectional host-GSP messaging queues.
- **Upstream VBIOS BIT & Falcon Parser:** Uses official OpenRM specifications (`NV_VBIOS_BIT_HEADER_ID`, `NV_VBIOS_BIT_TOKEN_*`, `FALCON_UCODE_TABLE_*`, RSA-3K BCRT30 structures) for authentic VBIOS probing and FWSEC/FRTS booter execution.
- **Upstream Open-GPU Headers & Shims (`nvx_nvidia_open_shim.h`):** Official NVIDIA SDK classes (`clc57e.h`, `cla06f.h`, `clc36f.h`), controls (`ctrl0073.h`, `ctrl2080.h`, `ctrl5070system.h`), and swref registers (`dev_gsp.h`, `dev_falcon_v4.h`, `dev_disp.h`).
- **Comprehensive Upstream GPU Table (`nvx_gpu_table.h`):** Auto-generated from official `g_nv_name_released.h` database covering 216 modern GPUs for authentic hardware detection and string reporting.
- **Dynamic EDID Detailed Timing & Resolution Generation:** Eliminates hardcoded monitor resolution tables on both Windows XP and Windows 98 SE, reading native monitor modes directly from EDID detailed timing descriptors.

Both Windows XP x64 and Windows 98 SE are confirmed driving **dual physical monitors simultaneously** with an extended desktop:
- **Head 0 (Primary):** 1920×1080 @ 32bpp (DisplayPort / HDMI)
- **Head 1 (Secondary):** 2560×1440 @ 32bpp (DisplayPort)

**Bare-metal validation** was confirmed on an **AMD Ryzen AM5 platform with an EVGA GeForce RTX 3070 XC3 Ultra LHR**. Both drivers also operate inside QEMU/KVM virtual machines with GPU passthrough via Linux VFIO.

![Windows running on Ryzen AM5 with an RTX 3070 and both monitors active](20261104_205049.jpg)

---

## Architectural Comparison

| Feature | Windows XP (x86 & x64) | Windows 98 SE |
|---|---|---|
| **Driver Architecture** | Windows 2000/XP Display Driver Model (XDDM 32-bit & 64-bit) | Hybrid Ring 0 VxD + 16-bit GDI Display Driver |
| **Kernel Component** | `nvxmp.sys` (32-bit / 64-bit video miniport driver) | `nvx.vxd` (32-bit LE Virtual Device Driver) |
| **Display Component** | `nvxdisp.dll` (32-bit / 64-bit GDI display driver) | `nvx.drv` (Head 0) & `nvx2.drv` (Head 1) (16-bit NE) |
| **2D Acceleration Engine** | Software GDI + GPU solid fills, eligible copies and DMA uploads | Microsoft DIB Engine (`DIBENG.DLL`) + RAM shadow buffer |
| **Hardware Bootstrap** | Authentic GSP-RM firmware booted via Falcon/SEC2 | Authentic GSP-RM firmware booted via Falcon/SEC2 |
| **Multi-Monitor Method** | Windows XP XDDM multi-head scanout surfaces / DualView | Windows 98 Desktop Virtualization (`attach_sec.exe`) |
| **Tested Resolutions** | 1920×1080 + 2560×1440 @ 32bpp True Color | 1920×1080 + 2560×1440 @ 32bpp True Color |

---

## Windows XP x64 Architecture

The Windows XP x64 driver consists of a 64-bit XDDM video miniport (`nvxmp.sys`) and a GDI display driver (`nvxdisp.dll`):
- **GSP-RM Firmware Bootstrap:** On system start, the miniport initializes the GA104 Falcon security coprocessor (SEC2), uploads the NVIDIA GSP-RM firmware payload from embedded binary storage, and establishes bidirectional command queues over DMA.
- **Dual Display Scanout:** The miniport configures two hardware display heads using GSP display RPC calls.
- **Copy Engine & 2D Fills:** A GSP-managed GPU Copy Engine (Ampere CE) performs solid fills and framebuffer updates for contiguous row segments. Unaccelerated complex rasterization falls back cleanly to CPU GDI routines.

---

## Windows 98 SE Architecture

Windows 98 SE uses a specialized dual-driver layout designed to run on top of modern GPU architectures:
- **VideoPort Shim Layer (`shim/videoprt_shim.c`):** An emulation layer implementing 22 Windows NT `VideoPort*` APIs on top of Windows 98 VMM (Virtual Machine Manager) ring 0 services. This allows the core 32-bit GA104 miniport engine (`src_driver/`) to run natively inside `nvx.vxd`.
- **DIB Engine Integration:** Both `nvx.drv` and `nvx2.drv` report `C1_DIBENGINE` (`0x0010`) and initialize standard `DIBENGINE` (`0x5250 'RP'`) PDEVICE descriptors, which is mandatory for Windows 98 to permit desktop extension across multiple monitors.
- **Shadow Buffer & Burst-Flush:** Head 0 maintains a linear shadow buffer in system RAM. Drawing calls are handled by `DIBENG.DLL` into RAM, and driver hooks in `BeginAccess` / `EndAccess` compute dirty bounding boxes and flush updated pixels to physical VRAM in high-speed burst copies.
- **Multi-Monitor Utility (`attach_sec.exe`):** A Win32 utility leveraging `ChangeDisplaySettingsExA` (`CDS_UPDATEREGISTRY`) to dynamically attach the secondary adapter (`\\.\Display2`) at `(1920, 0)` on demand.

For detailed design notes, see [src_win98/README.md](src_win98/README.md).

---

## Compatibility and System Requirements

- **Tested GPU:** EVGA GeForce RTX 3070 XC3 Ultra LHR (GA104, PCI Device ID `10de:2488`).
- **Compatible GPUs:** GA104-based cards (GeForce RTX 3070, RTX 3060 Ti, GA104 RTX 3060).
- **Tested Host Platform:** AMD Ryzen 7000 series on AM5 motherboard (bare metal) and QEMU/KVM with VFIO GPU passthrough.
- **Monitors Tested:** Dual DisplayPort setup running 1920×1080 (Primary) and 2560×1440 (Secondary) at 60 Hz.
- **Scope & Limitations:** 2D desktop acceleration, solid fills, and multi-monitor desktop extension are supported. An experimental XP x64 GR diagnostic verifies Ampere triangle rasterization and rendered-pixel readback on hardware. This diagnostic is not a user-mode rendering interface; an OpenGL ICD, general OpenGL draw submission, and Direct3D acceleration remain unimplemented. Video decode acceleration (NVDEC) and audio over HDMI/DP are also unimplemented.

---

## Graphics validation (XP x86)

The x86 VM passes solid-fill and screen-copy pixel checks on both monitors.
The miniport log confirms real Ampere Copy Engine submissions. Other GDI
operations still use software rasterization with GPU uploads; this is partial
2D acceleration, not a complete DirectDraw/Direct3D driver.

Hardware OpenGL is **not implemented on either XP architecture**. The XP x64
miniport has an experimental bounded 3D escape ABI for capability query,
framebuffer allocation/release, a kernel-constructed C797 surface-clear
submission, and completion-fence wait. A separate initialization-time GR probe
submits an Ampere triangle and reads back its rasterized pixels, but this is
not a user-mode OpenGL draw path. The guest still reports Microsoft `GDI
Generic`, a software OpenGL renderer. The old test ignored failed swaps/device
creation and printed an unconditional PASS. The corrected suite checks those
failures, reports the actual renderer, and returns a failing exit code for
unavailable Direct3D 9 errors. On this driver, `0x8876086A` is
`D3DERR_NOTAVAILABLE`: the display driver has no Direct3D HAL, so the test now
reports this as an expected unavailable feature rather than an OpenGL or Copy
Engine failure.
Experimental OpenGL DDI hooks are disabled by default so the working Windows
software renderer can handle buffer swaps.

With XP shut down, prepare the same tests for the x86 image:

```sh
./test/deploy_xp32_graphics_tests.sh
./test/run_vfio_vm_xp32.sh
# Shut XP down normally after the one-time tests finish, then collect:
./test/collect_xp32_graphics_results.sh
```

The one-time run checks primary 2D/OpenGL/D3D9, secondary 2D, and ten seconds
of wglgears. Driver and suite output appear in `vm_serial.log`; collection
appends the guest result files and gears output there after shutdown. It does
not add a serial-port dependency to the gears benchmark. Both test programs
are also compiled for x64 from the same corrected sources.

Full hardware OpenGL needs a GR channel/context and resource/synchronization
path exposed through a bounded user/kernel interface, then a GPU shader
compiler and rendering backend, and an XP x64 OpenGL ICD. The current ABI only
accepts a fixed safe clear operation; it does not accept arbitrary user command
streams. A Direct3D HAL or translation layer is a separate task.
The x86 and x64 miniports can share a fixed-width kernel submission ABI, while
each XP architecture needs its own matching OpenGL ICD binary. Copy Engine
initialization and pixel-format registration alone do not provide these.

### Hardware OpenGL roadmap

The planned design follows the Mesa-backed layering used by the
[NullMoth macOS NVIDIA port](https://github.com/nullmoth/nvidia-macos-driver),
adapted to XP's OpenGL ICD/WGL and VideoPort interfaces. The macOS port routes
its native graphics API through a translator into Mesa NVK/NAK, then into an
OS-specific kernel driver. XP needs its own user/kernel shims; the macOS kexts
and plugin are not XP components.

1. **Pin the Mesa base and prove it builds.** Mesa is pinned as a submodule at
   `cc2dcf9abdc6afc093e85d48e4cfa34eed60a01e`. XP-specific changes live in
   `mesa_patches/` and are applied with `test/apply_mesa_xp_patches.sh`, leaving
   the submodule at its upstream revision. The Windows x64 `softpipe` WGL DLL
   now builds with XP-era imports and loads under Wine. This validates software
   ICD packaging only; no XP hardware backend is integrated. See
   `PHASE2C_MESA_EVALUATION.md`.
2. **Create the XP x64 platform shim.** Implement the Mesa-side OS, memory,
   synchronization, and device interfaces over an XP OpenGL ICD/WGL front end
   and a narrow miniport IOCTL ABI. Keep the ABI fixed-width, validate all
   user buffers, and use 64-bit GPU virtual addresses on both XP architectures.
3. **Bring up GR independently of display scanout.** The in-kernel probe has
   verified an Ampere triangle, a completion semaphore, and rendered-pixel
   readback while preserving display and Copy Engine paths. The probe remains
   diagnostic; user-mode command submission is not implemented.
4. **Connect Mesa rendering.** Mesa's Windows WGL frontend builds, but NVK/NAK
   and Nouveau depend on Linux Vulkan/DRM interfaces and do not form a Windows
   Gallium backend. Implement an XP-compatible Gallium driver or a Vulkan
   device shim over the fixed-width miniport interface before connecting the
   OpenGL state tracker. The current ICD is `softpipe` and renders on the CPU.
5. **Validate XP x64.** Require an NVK/GA104 renderer string, logged GR
   submissions and completed fences, correct rendered-pixel readback, and
   stable 1080p/1440p desktop output. Then run OpenGL applications directly
   and through WineD3D; WineD3D will translate Direct3D calls only after this
   ICD works.
6. **Expand to XP x86.** Build the matching 32-bit ICD and miniport against
   the same fixed-width ABI and rendering tests. Confirm resource handles and
   GPU addresses remain architecture-neutral.
7. **Evaluate Windows 98 SE last.** Add an OpenGL ICD entry layer and VxD/VMM
   shim for the same Mesa rendering backend. Reuse only the portable Mesa and
   GPU logic; XP WGL, XDDM, and VideoPort shims do not carry over directly.

The NullMoth repository's macOS-specific plugin, translator, and kernel
components are not dependencies for this port. Its README identifies those
components as PolyForm Noncommercial, while the NVK Mesa patch is MIT; this
project will use the architectural approach and upstream Mesa rather than
importing those platform-specific components. Mesa source is pinned for
evaluation, but no Mesa component is integrated into the driver yet. Its modern
NVK documentation lists Linux 6.6 as the kernel minimum, so XP compatibility
must be developed in the platform shim and verified by builds rather than
assumed.

## Build Instructions

### Prerequisites
On a Debian or Ubuntu x86-64 host, install the cross-compilers and build dependencies:
```sh
sudo apt-get install build-essential gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 binutils-mingw-w64
```
For the Windows 98 16-bit display driver, install [Open Watcom C/C++](http://openwatcom.org/) and JWasm.

### Building Windows XP x64 Driver
```sh
# Build 64-bit miniport with embedded GSP firmware
make -C src_driver full

# Build 64-bit GDI display driver
make -C src_disp

# Run host unit and regression checks
make -C src_driver check
make -C src_disp check
```
The output files are `src_driver/nvxmp.sys` and `src_disp/nvxdisp.dll`.

### Building Windows 98 SE Driver
```sh
# Set Open Watcom environment
source src_win98/owenv.sh

# Build 16-bit display drivers (nvx.drv & nvx2.drv)
cd src_win98/winvbe_drv && ./build.sh && cd ../..

# Build 32-bit Ring 0 VxD (nvx.vxd)
cd src_win98 && ./build_vxd.sh && cd ..

# Build secondary monitor attach utility
i686-w64-mingw32-gcc -O2 -s -o dist_win98/attach_sec.exe src_win98/attach_sec.c -luser32
```
The output files are generated in `dist_win98/`.

---

## Installation & Usage

### Installing on Windows XP x64
1. Download `nvx-driver-v1.1.0-xp-x64.zip` from [GitHub Releases](https://github.com/sanjayseshan/xddm_nvidia/releases) and extract its contents.
2. In Windows XP x64, open **Device Manager** -> **Display Adapters** -> **Standard VGA Graphics Adapter**.
3. Choose **Update Driver** -> **Have Disk...** and select `nvx.inf`.
4. Select the matching NVIDIA GeForce RTX model and reboot when prompted.
5. In **Display Properties** -> **Settings**, both displays can be arranged and enabled.

To update an existing installation directly from Linux:
```sh
XP_MOUNT=/mnt/xp ./test/update_xp_driver.sh
```

### Installing on Windows 98 SE
1. Download `nvx-driver-v1.1.0-win98se.zip` from [GitHub Releases](https://github.com/sanjayseshan/xddm_nvidia/releases) and extract its contents.
2. In Windows 98 SE, open **Device Manager** -> **Display Adapters** -> **Standard PCI Graphics Adapter (VGA)**.
3. Choose **Update Driver** -> **Specify location** and point to `nvx.inf`.
4. Select the NVIDIA GeForce RTX card and finish the wizard. Restart Windows 98.
5. Primary display boots to 1920×1080 @ 32bpp.
6. Run `ATTACH_SEC.EXE` (or use Display Properties) to activate the secondary monitor at 2560×1440 @ 32bpp.

To update an existing Windows 98 disk image offline:
```sh
./test/update_win98_driver.sh
```

---

## Hardware Testing & Virtualization

Automated test scripts in `test/` allow rapid iteration inside QEMU/KVM with the physical RTX 3070 bound to `vfio-pci`:
- `./test/run_all_tests.sh`: Runs all driver host unit tests and builds both 64-bit and 32-bit driver binaries.
- `./test/test_full_driver.sh`: Builds drivers, syncs to offline XP guest disk, and launches the VM.
- `NVX_AUTO_RETRY=1 ./test/test_full_driver.sh`: Same build/install path, then retries transient XP x64 VFIO GPU boot failures (up to `MAX_RETRIES`, default 10; `BOOT_TIMEOUT`, default 35 seconds) and requests ACPI shutdown after the graphics test completes.
- `./test/run_vfio_xp_autoretry.sh`: Auto-retrying Windows XP x64 VFIO bringup and monitoring.
- `./test/run_vfio_win98_autoretry.sh`: Auto-retrying Windows 98 SE VFIO bringup and monitoring.
- `./test/reset_test_gpu.sh`: Cycles GPU PCIe power state and restores host VFIO binding.

---

## Repository Structure

```
├── dist_full/           # Windows XP x64 release files (nvxmp.sys, nvxdisp.dll, nvx.inf, gsp_ga104.bin)
├── dist_win98/          # Windows 98 SE release files (nvx.drv, nvx2.drv, nvx.vxd, nvx.inf, attach_sec.exe)
├── src_driver/          # Unified GA104 miniport engine, GSP/RPC/MMU/Display code, and OpenRM HAL shim
├── src_disp/            # Windows XP x64 GDI display driver
├── src_win98/           # Windows 98 SE components:
│   ├── winvbe_drv/      # 16-bit DIB Engine display drivers (nvx.drv, nvx2.drv)
│   ├── shim/            # Ring 0 NT VideoPort to VMM translation layer
│   ├── attach_sec.c     # Win32 secondary monitor attachment utility
│   └── build_vxd.sh     # VxD compilation and packaging script
├── test/                # Test infrastructure, automated test runners, and QEMU/VFIO harness
├── open-gpu-kernel-modules/ # Official NVIDIA OpenRM upstream repository (submodule)
├── FIRMWARE_PROVENANCE.md # NVIDIA GSP/SEC2 firmware checksums and provenance
└── SUMMARY.txt          # Cumulative project hardware test log and history
```

---

## Credits & Acknowledgments

- **NVIDIA Corporation** for Ampere GA104 GPU hardware, GSP-RM and SEC2 microcode, and the [NVIDIA Open GPU Kernel Modules](https://github.com/NVIDIA/open-gpu-kernel-modules).
- **The Linux Nouveau DRM Team** for extensive documentation and public reference implementations of GSP initialization and sequencer logic.
- **Michal Necasek & Jmh (WINVBE Project)** for the open-source 16-bit DIB Engine Windows 98 display driver framework.
- **nullmoth ([nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver))** for community reverse-engineering notes on modern NVIDIA GPU architectures.
- **Sanjay Seshan** for physical GPU and multi-monitor validation, reverse engineering, hardware testing, and project creation.
- **GPT and Gemini** artificial intelligence models for paired programming, kernel shim engineering, and continuous debugging assistance.

---

## License & Firmware Notice

No project-wide open-source license has been specified. NVIDIA GSP-RM and SEC2 firmware components remain the intellectual property of NVIDIA Corporation and are subject to NVIDIA's licensing terms. Review [FIRMWARE_PROVENANCE.md](FIRMWARE_PROVENANCE.md) before redistributing.
