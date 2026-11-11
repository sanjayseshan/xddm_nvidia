# NVIDIA Modern GPU Display Drivers for Windows 2000/XP (x86 & x64) & Windows 98SE/ME

This project develops display drivers for modern NVIDIA (RTX) GPUs running on legacy versions of Windows. The driver stack is built against the official NVIDIA Open GPU Kernel Modules (`open-gpu-kernel-modules`, driver version 535.113.01) and boots authentic NVIDIA GSP-RM firmware.

### Operating System Support
* **Windows XP Professional x64 Edition (NT 5.2):** Tested on physical bare metal and VFIO VM passthrough.
* **Windows XP Professional x86 (NT 5.1):** Tested on VFIO VM passthrough.
* **Windows 98 Second Edition (SE, 4.10):** Tested on VFIO VM passthrough with multi-monitor desktop virtualization.
* **Windows ME (4.90) / Windows 2000 (NT 5.0) / Windows Vista (NT 6.0) / Windows 7 (NT 6.1):** Untested, but expected compatible via XDDM (Windows 2000 Display Driver Model) or Win98SE fallback.

### Hardware Validation & Target GPUs
* **Host Platform:** AMD AM5 (Ryzen 9000 series, IOMMU / VFIO passthrough)
* **Tested GPU:** NVIDIA GeForce RTX 3070 (Ampere GA104)
* **Tested Displays:** Dual-monitor setup (1x 2560×1440 @ 32bpp True Color + 1x 1920×1080 @ 32bpp True Color)
* **Supported Architectures (NVIDIA 535.113.01):**
  * GTX 16 series (Turing TU10x, untested)
  * RTX 20 series (Turing TU10x, untested)
  * RTX 30 series (Ampere GA10x, tested GA104)
  * RTX 40 series (Ada Lovelace AD10x, untested)

---

## Architectural Comparison

| Feature | Windows XP (x86) | Windows XP (x64) | Windows 98 SE |
| :--- | :--- | :--- | :--- |
| **Driver Architecture** | Windows 2000/XP Display Driver Model (XDDM 32-bit) | Windows XP Display Driver Model (XDDM 64-bit) | Hybrid Ring 0 VxD + 16-bit GDI Display Driver |
| **Kernel Component** | `nvxmp.sys` (32-bit video miniport driver, built as `nvxmp32.sys`) | `nvxmp.sys` (64-bit video miniport driver) | `nvx.vxd` (32-bit LE Virtual Device Driver) |
| **Display Component** | `nvxdisp.dll` (32-bit GDI display driver, built as `nvxdisp32.dll`) | `nvxdisp.dll` (64-bit GDI display driver) | `nvx.drv` (Head 0) & `nvx2.drv` (Head 1) (16-bit NE) |
| **2D Acceleration Engine** | Full Hardware Acceleration (Ampere Copy Engine) | Full Hardware Acceleration (Ampere Copy Engine) | Partial Acceleration (DIB Engine RAM shadow buffer + burst VRAM flush) |
| **3D Acceleration Engine** | Work in progress / Software fallback | Partial 3D via Mesa WGL bridge & native C797 miniport submit; Gallium NVC0 port in progress | Software rasterization |
| **Hardware Bootstrap** | Authentic GSP-RM firmware booted via Falcon/SEC2 | Authentic GSP-RM firmware booted via Falcon/SEC2 | Authentic GSP-RM firmware booted via Falcon/SEC2 |
| **Multi-Monitor Method** | Windows XP XDDM multi-head scanout surfaces / DualView | Windows XP XDDM multi-head scanout surfaces / DualView | Dual display drivers (`nvx.drv` + `nvx2.drv`) & desktop extension via `attach_sec.exe` |
| **Tested Resolutions** | 1920×1080 + 2560×1440 @ 32bpp True Color | 1920×1080 + 2560×1440 @ 32bpp True Color | 1920×1080 + 2560×1440 @ 32bpp True Color |

---

## Windows XP x64 Architecture

The Windows XP x64 driver consists of a 64-bit XDDM video miniport (`nvxmp.sys`) and a 64-bit GDI display driver (`nvxdisp.dll`):
- **GSP-RM Firmware Bootstrap:** On system start, the miniport initializes the GA104 Falcon security coprocessor (SEC2), loads and authenticates the NVIDIA GSP-RM firmware payload (`gsp_ga104.bin`) from embedded storage, and sets up bidirectional message queues over DMA.
- **Dual Display Scanout:** The miniport configures two hardware display heads simultaneously via GSP display RPC calls.
- **Copy Engine & 2D Fills:** A GSP-managed GPU Copy Engine (Ampere CE) performs solid color fills, surface blits, and framebuffer updates. Complex unaccelerated rasterization falls back cleanly to CPU GDI routines.
- **OpenGL & 3D Acceleration:** Implements fixed-width user/kernel 3D ABI (`nvx_3d_abi.h`) supporting owned GPU VRAM allocation, validated C797 method execution, inline memory writes (I2M), fast hardware clears, and hardware triangle rasterization. Uses an XP-compatible Mesa WGL ICD (`nvxogl64.dll`) with ongoing work to port Mesa's Nouveau NVC0 Gallium backend.

---

## Windows XP x86 Architecture

The Windows XP x86 driver provides identical feature parity with the 64-bit driver using a 32-bit XDDM miniport and GDI display driver:
- **32-bit Video Miniport (`nvxmp32.sys`):** Compiled with 32-bit MinGW (`i686-w64-mingw32-gcc`), using the same core GA104 GSP-RM engine, OpenRM HAL shims, RPC queues, and Ampere Copy Engine as the 64-bit driver. Deployed as `nvxmp.sys`.
- **32-bit GDI Display Driver (`nvxdisp32.dll`):** 32-bit display driver providing standard GDI hooks (`DrvEnableDriver`, `DrvEnableSurface`, `DrvBitBlt`, `DrvTextOut`, `DrvCopyBits`). Export definitions are validated against legacy Windows XP 32-bit `win32k.sys` entry points. Deployed as `nvxdisp.dll`.
- **2D Copy Engine Acceleration:** Shared codebase with 64-bit, enabling hardware-accelerated solid fills, rect copies, and dual-head scanout surfaces.

---

## Windows 98 SE Architecture

Windows 98 SE uses a specialized dual-driver layout designed to run on top of modern GPU architectures:
- **VideoPort Shim Layer (`src_win98/shim/videoprt_shim.c`):** An emulation layer implementing 22 Windows NT `VideoPort*` APIs on top of Windows 98 VMM (Virtual Machine Manager) ring 0 services. This allows the core 32-bit GA104 miniport engine (`src_driver/`) to run natively inside `nvx.vxd`.
- **DIB Engine Integration:** Both `nvx.drv` and `nvx2.drv` report `C1_DIBENGINE` (`0x0010`) and initialize standard `DIBENGINE` (`0x5250 'RP'`) PDEVICE descriptors, which is mandatory for Windows 98 to permit desktop extension across multiple monitors.
- **Shadow Buffer & Burst-Flush:** Head 0 maintains a linear shadow buffer in system RAM. Drawing calls are handled by `DIBENG.DLL` into RAM, and driver hooks in `BeginAccess` / `EndAccess` compute dirty bounding boxes and flush updated pixels to physical VRAM in high-speed burst copies.
- **Multi-Monitor Utility (`attach_sec.exe`):** A Win32 utility leveraging `ChangeDisplaySettingsExA` (`CDS_UPDATEREGISTRY`) to dynamically attach the secondary adapter (`\\.\Display2`) at `(1920, 0)` on demand.

For detailed design notes, see [src_win98/README.md](src_win98/README.md).

---

## Build Instructions

### Prerequisites
On a Debian or Ubuntu x86-64 host, install the cross-compilers and build dependencies:
```sh
sudo apt-get install build-essential gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 binutils-mingw-w64 genisoimage
```
For the Windows 98 16-bit display driver, install [Open Watcom C/C++](http://openwatcom.org/) and JWasm.

### Building Windows XP x64 Driver
```sh
# Build 64-bit miniport with embedded GSP firmware
make -C src_driver full

# Build 64-bit GDI display driver
make -C src_disp nvxdisp.dll

# Run host unit and regression checks
make -C src_driver check
make -C src_disp check
```
The output files are `src_driver/nvxmp.sys` and `src_disp/nvxdisp.dll` (packaged in `dist_full/`).

### Building Windows XP x86 Driver
```sh
# Build 32-bit miniport with embedded GSP firmware
make -C src_driver full32

# Build 32-bit GDI display driver
make -C src_disp nvxdisp32.dll
```
The output files are `src_driver/nvxmp32.sys` and `src_disp/nvxdisp32.dll` (packaged into `dist32/` as `nvxmp.sys` and `nvxdisp.dll`).

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
1. Copy the driver files from `dist_full/` or download the prebuilt release.
2. In Windows XP x64, open **Device Manager** -> **Display Adapters** -> **Standard VGA Graphics Adapter**.
3. Choose **Update Driver** -> **Install from a list or specific location** -> **Have Disk...** and select `nvx.inf`.
4. Select your NVIDIA GeForce RTX model and reboot when prompted.
5. In **Display Properties** -> **Settings**, configure resolutions and enable the secondary monitor.

To update an existing installation directly from Linux:
```sh
XP_MOUNT=/mnt/xp ./test/update_xp_driver.sh
```

### Installing on Windows XP x86
1. Copy the driver files from `dist32/` (`nvxmp.sys`, `nvxdisp.dll`, `nvx.inf`).
2. In Windows XP 32-bit, open **Device Manager** -> **Display Adapters** -> **Standard VGA Graphics Adapter**.
3. Choose **Update Driver** -> **Install from a list or specific location** -> **Have Disk...** and select `nvx.inf` from `dist32/`.
4. Select your NVIDIA GeForce RTX model and reboot when prompted.
5. In **Display Properties** -> **Settings**, arrange dual monitors and adjust resolution.

To deploy drivers and test binaries directly to an offline `winxp_x32.qcow2` VM disk:
```sh
./test/install_xp32_driver_and_tests.sh
```

### Installing on Windows 98 SE
1. Copy the driver files from `dist_win98/` (`nvx.drv`, `nvx2.drv`, `nvx.vxd`, `nvx.inf`, `attach_sec.exe`).
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

## Repository Structure

```
├── README.md           # Project overview and build instructions
├── dist_full/           # Windows XP x64 release package (nvxmp.sys, nvxdisp.dll, nvxogl64.dll, nvx.inf, gsp_ga104.bin)
├── dist32/              # Windows XP x86 release package (nvxmp.sys, nvxdisp.dll, nvx.inf)
├── dist_win98/          # Windows 98 SE release package (nvx.drv, nvx2.drv, nvx.vxd, nvx.inf, attach_sec.exe)
├── src_driver/          # Unified GA104 miniport engine, GSP/RPC/MMU/Display code, and OpenRM HAL shim
├── src_disp/            # Windows XP GDI display driver (both 64-bit and 32-bit builds)
├── src_win98/           # Windows 98 SE components:
│   ├── winvbe_drv/      # 16-bit DIB Engine display drivers (nvx.drv, nvx2.drv)
│   ├── shim/            # Ring 0 NT VideoPort to VMM translation layer
│   ├── attach_sec.c     # Win32 secondary monitor attachment utility
│   └── build_vxd.sh     # VxD compilation and packaging script
├── mesa_patches/        # Windows XP shims and softpipe WGL bridge (Mesa submodule stays clean)
├── test/                # Automated test infrastructure, regression runners, and QEMU/VFIO scripts
├── open-gpu-kernel-modules/ # NVIDIA OpenRM upstream repository (submodule)
├── notes/               # Project status, phase evaluation, hardware notes, and firmware provenance
│   ├── handoff.txt       # Current phase status and engineering roadmap
│   ├── SUMMARY.txt       # Cumulative hardware test history
│   ├── PHASE2C_MESA_EVALUATION.md
│   ├── nvx_nouveau_port_map.md
│   └── FIRMWARE_PROVENANCE.md
```

---

## Credits & Acknowledgments

- **NVIDIA Corporation** for Ampere GA104 GPU hardware, GSP-RM and SEC2 microcode, and the [NVIDIA Open GPU Kernel Modules](https://github.com/NVIDIA/open-gpu-kernel-modules).
- **The Linux Nouveau DRM Team** for extensive documentation and public reference implementations of GSP initialization and sequencer logic.
- **Michal Necasek & Jmh (WINVBE Project)** for the open-source 16-bit DIB Engine Windows 98 display driver framework.
- **nullmoth ([nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver))** for community reverse-engineering notes on modern NVIDIA GPU architectures.
- **Sanjay Seshan** for physical GPU and multi-monitor validation, reverse engineering, hardware testing, and project creation.
- **GPT and Gemini** artificial intelligence models for paired programming, kernel shim engineering, and continuous debugging assistance.
- **ReactOS** for XDDM implementation reference.

---

## License & Firmware Notice

MIT license. NVIDIA GSP-RM and SEC2 firmware components remain the intellectual property of NVIDIA Corporation and are subject to NVIDIA's licensing terms. Review [notes/FIRMWARE_PROVENANCE.md](notes/FIRMWARE_PROVENANCE.md) before redistributing.
