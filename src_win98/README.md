# NVIDIA GA104 Display Driver for Windows 98 SE (Dual-Monitor)

This experimental driver brings up **Windows 98 Second Edition (SE)** on modern **NVIDIA Ampere GA104 hardware** (GeForce RTX 3070 / RTX 3060 Ti) with **full multi-monitor dual-display support**.

Both physical displays are driven simultaneously in 32-bit True Color:
- **Head 0 (Primary):** 1920×1080 @ 32bpp (DisplayPort / HDMI)
- **Head 1 (Secondary):** 2560×1440 @ 32bpp (DisplayPort)
- Full desktop extension across both monitors using standard Windows 98 multi-monitor display virtualization.

---

## Architecture Overview

Windows 98 SE uses a hybrid 16-bit / 32-bit driver architecture combining a 16-bit GDI display driver (`.DRV`), Microsoft's DIB Engine (`DIBENG.DLL`), and a 32-bit Ring 0 Virtual Device Driver (`.VXD`).

```
+-------------------------------------------------------------------------+
|                  Windows 98 SE User / GDI Subsystem                     |
|            Multi-Monitor Desktop Virtualization (Desktop 0 & 1)         |
+-------------------------------------------------------------------------+
             |                                             |
     Calls GDI Display #1                          Calls GDI Display #2
             v                                             v
+-------------------------+                   +-------------------------+
|  Primary: NVX.DRV       |                   |  Secondary: NVX2.DRV    |
|  16-bit NE Display Drv  |                   |  16-bit NE Display Drv  |
|  - C1_DIBENGINE (0x10)  |                   |  - C1_DIBENGINE (0x10)  |
|  - RAM Shadow Buffer    |                   |  - DIB Engine PDEVICE   |
|  - Begin/EndAccess hook |                   |  - Linear Framebuffer   |
|  - Burst-flush to VRAM  |                   |  - Direct scanout       |
+-------------------------+                   +-------------------------+
             \                                             /
              \----------> DIBENG.DLL (GDI 2D) <----------/
                                   |
                  Ring 3 -> Ring 0 | DeviceIoControl / VDD
                                   v
+-------------------------------------------------------------------------+
|                   Virtual Device Driver: NVX.VXD (Ring 0)               |
|                                                                         |
|  +-------------------------------------------------------------------+  |
|  | VideoPort Shim Layer (shim/videoprt_shim.c)                       |  |
|  | - VideoPortGetDeviceBase       -> _MapPhysToLinear                |  |
|  | - VideoPortAllocateCommonBuffer-> _PageAllocate / _PageLock       |  |
|  | - VideoPortMapMemory           -> Map physical BAR1 VRAM aperture |  |
|  | - VideoPortRead/WriteRegister* -> Direct MMIO                     |  |
|  | - VideoPortStallExecution      -> Hardware I/O microsecond delay  |  |
|  +-------------------------------------------------------------------+  |
|                                  |                                      |
|  +-------------------------------------------------------------------+  |
|  | Core GA104 Miniport Engine (Compiled 32-bit x86 from src_driver/) |  |
|  | - nvxmp.c      : Miniport lifecycle, mode setting, dual-head scan |  |
|  | - nvx_gsp.c    : Falcon bootloader, SEC2 booter, GSP firmware     |  |
|  | - nvx_fw.c     : Authenticated firmware extraction                |  |
|  | - nvx_rpc.c    : GSP command queues and message channels          |  |
|  | - nvx_mmu.c    : Ampere GPU MMU page tables & context structures  |  |
|  | - nvx_seq.c    : Hardware sequencing and initialization           |  |
|  | - nvx_vbios.c  : VBIOS parsing and display connector discovery    |  |
|  +-------------------------------------------------------------------+  |
+-------------------------------------------------------------------------+
                                   |
                     PCI MMIO / DMA / BAR1 Aperture
                                   v
+-------------------------------------------------------------------------+
|                Hardware: NVIDIA GeForce RTX 3070 (GA104)                |
|  - Head 0: 1920×1080 @ 60Hz 32bpp Primary Display                       |
|  - Head 1: 2560×1440 @ 60Hz 32bpp Secondary Extended Display            |
|  - GSP-RM firmware running on onboard Falcon coprocessor                |
|  - Ampere Copy Engine DMA channels for hardware acceleration            |
+-------------------------------------------------------------------------+
```

### Key Technical Innovations

1. **NT VideoPort Shim on Windows 98 VMM:**
   - The driver reuses the authentic 32-bit C miniport source code from the NT/XP driver (`src_driver/`).
   - A translation layer (`shim/videoprt_shim.c`) emulates the 22 core Windows NT `VideoPort*` APIs using Windows 98 VMM (Virtual Machine Manager) services, including contiguous physical memory allocation, physical-to-linear virtual memory mapping, and registry configuration queries.

2. **GSP-RM Firmware Bootstrap in Ring 0:**
   - At system boot, `NVX.VXD` maps the GA104 BAR0 MMU registers, uploads the authentic NVIDIA SEC2 and GSP firmware payloads into DMA-coherent memory, executes the Falcon bootloader, and establishes bidirectional RPC communication with the onboard RISC-V GSP-RM coprocessor.

3. **DIB Engine Integration with Shadow Buffer & Dirty-Flush:**
   - Both `NVX.DRV` and `NVX2.DRV` export `C1_DIBENGINE` (capability flag `0x0010`) and populate a standard `DIBENGINE` structure (`0x5250 'RP'`) required by Windows 98 to permit desktop extension across multiple monitors.
   - For Head 0, a system RAM shadow buffer is maintained to accelerate window composition. Drawing operations are processed by `DIBENG.DLL` into system RAM, and driver hooks in `BeginAccess` / `EndAccess` (`my_BeginAccess` and `my_EndAccess`) compute dirty bounding rectangles and burst-flush updated pixels to physical VRAM using high-performance block copies.

4. **Multi-Monitor Secondary Attach Utility (`ATTACH_SEC.EXE`):**
   - A dedicated 32-bit utility (`attach_sec.c`) interfaces with the Windows 98 Multi-Monitor API (`EnumDisplayDevicesA` and `ChangeDisplaySettingsExA`).
   - It programmatically initializes `\\.\Display2` (`NVX2.DRV`), positions it at coordinates `(1920, 0)` immediately to the right of Head 0, and updates the active desktop display virtualization.

---

## Driver Components

| File | Description |
|---|---|
| `dist_win98/nvx.drv` | Primary 16-bit GDI display driver (Head 0, 1920×1080 @ 32bpp) |
| `dist_win98/nvx2.drv` | Secondary 16-bit GDI display driver (Head 1, 2560×1440 @ 32bpp) |
| `dist_win98/nvx.vxd` | 32-bit Ring 0 Virtual Device Driver with VideoPort shim and GA104 GSP miniport |
| `dist_win98/nvx.inf` | Windows 98 Plug-and-Play setup information file |
| `dist_win98/attach_sec.exe` | Win32 utility to attach and enable the secondary monitor |
| `src_win98/nvx_reg.reg` | Recommended registry configuration for modes, refresh rates, and settings |
| `src_win98/qipnp.reg` | Quick-install PCI PNP registry configuration helper |

---

## Build Instructions

### Prerequisites
On a Debian or Ubuntu x86-64 build host, install the following tools:
- **GCC / MinGW-w64** (32-bit and 64-bit cross-compilers):
  ```sh
  sudo apt-get install build-essential gcc-mingw-w64-i686 gcc-mingw-w64-x86-64 binutils-mingw-w64
  ```
- **Open Watcom C/C++** (v1.9 or v2.0):
  Ensure `owenv.sh` points to your Open Watcom installation:
  ```sh
  export WATCOM=/usr/bin/watcom
  export PATH=$WATCOM/binl:$PATH
  export INCLUDE=$WATCOM/h
  ```
- **JWasm** (free MASM-compatible x86 assembler)
- **Python 3**

### 1. Build the 16-bit Display Drivers (`nvx.drv` and `nvx2.drv`)
```sh
cd src_win98/winvbe_drv
./build.sh
```
This compiles the assembly and C sources using Open Watcom (`wcc` / `wasm` / `wlink`) and generates `dist_win98/nvx.drv` and `dist_win98/nvx2.drv`.

### 2. Build the Ring 0 Driver (`nvx.vxd`)
```sh
cd src_win98
./build_vxd.sh
```
This compiles the 32-bit miniport engine, VideoPort shim, and VxD entry stubs, and outputs `dist_win98/nvx.vxd`.

### 3. Build the Secondary Monitor Attach Utility (`attach_sec.exe`)
```sh
i686-w64-mingw32-gcc -O2 -s -o dist_win98/attach_sec.exe src_win98/attach_sec.c -luser32
```

---

## Installation Guide

### Option A: Clean Installation via Device Manager
1. Copy the driver files (`nvx.inf`, `nvx.drv`, `nvx2.drv`, `nvx.vxd`, `attach_sec.exe`) to a directory on your Windows 98 machine (e.g., `C:\DRIVERS\NVX`).
2. Right-click on **My Computer** -> **Properties** -> **Device Manager**.
3. Expand **Display adapters**, double-click **Standard PCI Graphics Adapter (VGA)**, and select the **Driver** tab.
4. Click **Update Driver...** -> **Specify the location of the driver** -> **Browse** to `C:\DRIVERS\NVX`.
5. Select **NVIDIA GeForce RTX 3070** (or corresponding model) and click **Next** to install.
6. When prompted, restart Windows 98.

### Option B: Enabling Multi-Monitor Extended Desktop
1. Upon booting into Windows 98, the primary monitor (Head 0) will display the desktop at 1920×1080 in 32-bit True Color.
2. To enable the secondary monitor (Head 1 at 2560×1440):
   - **Method 1 (Command line or startup):** Run `ATTACH_SEC.EXE`. The utility enumerates `\\.\Display2`, positions it at `(1920, 0)`, and activates the extended desktop. You can add this to `AUTOEXEC.BAT` or the `StartUp` folder.
   - **Method 2 (Display Properties GUI):** Right-click the desktop -> **Properties** -> **Settings**. Click on **Monitor 2**, check **"Extend my Windows desktop onto this monitor"**, select **2560 by 1440 pixels** and **True Color (32 bit)**, then click **Apply**.

### Option C: Automated Offline Disk Update (for VMs or Dual-Boot)
If running Windows 98 under QEMU/KVM with VFIO or dual-booting with Linux, run:
```sh
./update_win98_driver.sh
```
This script automatically mounts the guest disk image, copies updated driver binaries into `C:\WINDOWS\SYSTEM`, and verifies binary checksums.

---

## Credits & Acknowledgments

- **NVIDIA Corporation** for Ampere GA104 GPU hardware, GSP-RM firmware, SEC2 bootloader, and published open-source kernel modules.
- **The Linux Nouveau Project** for invaluable documentation on NVIDIA Falcon microcode loading, RPC queue mechanics, and display engine registers.
- **Michal Necasek & Jmh (WINVBE Project)** for the 16-bit modular DIB Engine display driver base.
- **nullmoth ([nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver))** for community reverse-engineering references.
- **Sanjay Seshan** for physical GPU and multi-monitor testing, reverse engineering, hardware validation, and project leadership.
- **GPT & Gemini** models for architectural collaboration, code generation, and iterative debugging assistance.
