# Windows 98 SE GA104 Display Driver Project Plan

## Goal
Port the NVIDIA GA104 (RTX 3070) driver to **Windows 98 SE**, bringing up the **1080p primary display** first, followed by multi-monitor support.

## Architecture

```
+-----------------------------------------------------------+
| Windows 98 SE GDI / USER (16-bit / 32-bit Ring 3)         |
+-----------------------------------------------------------+
                             |
                    Calls standard GDI API
                             v
+-----------------------------------------------------------+
| 16-bit Display Driver: NVX.DRV / FRAMEBUF.DRV             |
| - Communicates with DIBENG.DLL for software acceleration  |
| - Issues DeviceIOControl / VDD services to NVX.VXD        |
+-----------------------------------------------------------+
                             |
                  Ring 3 -> Ring 0 (VxD Call)
                             v
+-----------------------------------------------------------+
| 32-bit Virtual Device Driver (Ring 0): NVX.VXD            |
|                                                           |
| +-------------------------------------------------------+ |
| | VideoPort Shim Layer (videoprt_shim.c)                | |
| | - VideoPortGetDeviceBase -> _MapPhysToLinear          | |
| | - VideoPortAllocateCommonBuffer -> _PageAllocate      | |
| | - VideoPortMapMemory -> Map VRAM aperture (BAR1)      | |
| | - VideoPortRead/WriteRegisterUlong -> Direct MMIO     | |
| | - VideoPortStallExecution -> I/O delay loop           | |
| +-------------------------------------------------------+ |
|                            |                              |
| +-------------------------------------------------------+ |
| | Core GA104 / GSP Hardware Engine (32-bit x86)         | |
| | - nvxmp.c (Miniport init & mode setting)              | |
| | - nvx_gsp.c (Falcon bootloader, GSP firmware, RPC)    | |
| | - nvx_fw.c (Firmware image extraction)                | |
| | - nvx_rpc.c (Command queues & RPC protocol)           | |
| | - nvx_mmu.c (GPU MMU & Page tables)                   | |
| | - nvx_seq.c (Hardware sequencing)                     | |
| | - nvx_vbios.c (VBIOS display table parsing)           | |
| +-------------------------------------------------------+ |
+-----------------------------------------------------------+
                             |
                    PCI MMIO / DMA / BAR1
                             v
+-----------------------------------------------------------+
| Hardware: NVIDIA GeForce RTX 3070 (GA104)                 |
| - Head 0: 1920x1080 @ 60Hz (Primary HDMI/DP)              |
| - Head 1: 2560x1440 @ 60Hz (Secondary DP - Phase 2)       |
+-----------------------------------------------------------+
```

## Implementation Phases
1. **Phase 1: VideoPort Shim for Windows 98 Ring 0**
   - Provide the 22 `VideoPort*` API shims mapped to standard 32-bit x86 protected mode / VMM services.
2. **Phase 2: 32-bit x86 Miniport Compilation**
   - Compile `src_driver/` into a 32-bit static library linked against the VideoPort shim.
3. **Phase 3: VxD Packaging & Dispatcher**
   - Provide the LE/VxD header (`NVX.VXD`) and DeviceIOControl interface.
4. **Phase 4: Framebuffer Driver Hook (`NVX.DRV`)**
   - Use the standard DIB Engine interface (`DIBENG.DLL`) to bind Windows 98 desktop to the 1080p linear framebuffer.
5. **Phase 5: Boot & Test on `win98_small.img`**
   - Mount disk with `guestfish`, install driver & INF, and verify boot.

