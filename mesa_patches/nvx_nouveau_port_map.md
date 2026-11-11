# Nouveau NVC0 to XP NVX port map

Status: source/API mapping and first narrow clear-packet bridge; no Mesa
submodule files were changed. The pinned NVC0 compiler target cross-builds
for XP x64. A manually constructed packet matching NVC0's clear sequence
passed the miniport and RTX 3070 test, but no runtime NVC0 callback emits it.

NullMoth's documented Mac stack uses Apple's OpenGL-on-Metal frontend, a
Metal-to-SPIR-V translator, Mesa NVK/NAK, and NVIDIA's open kernel modules
ported to XNU; it does not run Nouveau Gallium NVC0. Its reusable principle is
keeping Mesa's backend while supplying the target OS boundary. The current XP
NVC0/Gallium experiment applies that principle to Mesa's OpenGL-capable
Gallium/NVC0 path. A more literal Metal-style layering would be GL-over-Zink
over-Vulkan/NVK and would require an XP NVK winsys. Source: NullMoth's
[HOW-IT-WORKS.md](https://github.com/nullmoth/nvidia-macos-driver/blob/main/docs/HOW-IT-WORKS.md).

## What already maps

| NVC0 expectation | NVX state today | Port consequence |
| --- | --- | --- |
| Chipset detection and NVC0 screen selection | Device is known to be GA104/C797 through the XP display driver | A dedicated NVX `pipe_screen` factory can select NVC0 without Nouveau's DRM fd discovery. |
| Shader compiler/codegen | `src/gallium/drivers/nouveau/codegen` compiled with the MinGW XP target | Reuse this code. Keep it as unmodified Mesa source and link it into the NVX Gallium driver. |
| GPU buffer allocation | `NVX_3D_ALLOC_BUFFER` returns opaque `BufferId` and GPU VA; allocations live in a bounded VRAM window | Can back some `nouveau_bo`-like resources, but current metadata/layout and usage set is narrow. Introduce a real per-context resource table and references. |
| Host-to-GPU upload | `NVX_3D_UPLOAD_BUFFER` copies bounded chunks through the CE | Can provide initial staging uploads. NVC0 also writes mapped GART buffers directly, which needs a CPU-visible allocation/map contract. |
| Fence wait | `NVX_3D_WAIT_FENCE` waits for NVX's internal submission fence | Can back Gallium flush fences after lifetime and multi-context semantics are defined. |
| Simple GPU operations | `NVX_3D_SUBMIT` accepts a closed set of driver-built clear and triangle requests | Useful for probes only; it is not a generic NVC0 command submission path. |

## Implemented narrow clear bridge

`NVX_ESCAPE_3D_NVC0_CLEAR` / `IOCTL_NVX_3D_NVC0_CLEAR` is ABI v10. The
miniport accepts exactly 23 dwords and checks each NVC0 packet header and
payload, the target's opaque ID and exact GPU VA, allocation extent/layout,
single-layer RGBA8 format, full-surface scissor, zero-sample mode, color-only
clear mask, and UNORM float range. It converts the four float color values to
packed AARRGGBB, then calls a miniport-owned C797 clear builder; it never
submits the user packet words. `test/test_gpu_accel.c` constructs the matching
packet, waits for the returned fence, and reads back all 1024 pixels.

Evidence: `make -C src_driver check` passed including malformed-packet host
tests and x86/x64 ABI compilation; `make -C src_driver full` and
`make -C src_disp all` built the XP packages. The full real-hardware run
`test_logs/v3.0.0.1-20261111-095704.log` reports
`[NVX_NVC0_CLEAR_RESULT] PASS`, exact 1024/1024 pixel match to `0xFF8040FF`,
fence 2, and no MMU fault. This is protocol and hardware evidence for one
operation; the packet was assembled by the test, not emitted by NVC0 Gallium.

## NVC0 calls that do not have an XP equivalent yet

The relevant source is `mesa/src/gallium/winsys/nouveau/drm/nouveau.h`,
`mesa/src/gallium/winsys/nouveau/drm/nouveau.c`, and
`mesa/src/gallium/drivers/nouveau/nouveau_winsys.h`.

- `nouveau_device_new` and `nouveau_client_new` discover/own a DRM device and
  client. XP needs a driver-created NVX device tied to the WGL adapter and its
  lifetime.
- `nouveau_object_new` creates channel engine objects and class bindings.
  The current miniport binds its own C797 objects on its one managed channel;
  there is no client object-creation API.
- `nouveau_bo_new/map/wait/ref` includes placement, mapping, and synchronization
  expectations beyond the current opaque allocation handle and chunked copy
  escape. BO lifetime must remain pinned until the last referencing fence.
- `nouveau_pushbuf_space/refn/reloc/validate/kick` builds packets, tracks BO
  references, resolves relocations, validates and submits batches. NVX currently
  accepts only semantic operation IDs and generates packets in kernel mode.
  There is no pushbuffer upload/execute escape.
- NVC0 emits GPU writes into arbitrary BOs for fences, queries, and state. The
  current fence is miniport-owned; READBACK is restricted to a fixed test-sized
  surface. It cannot serve as general mapped BO memory.

The include failure at `mesa/include/drm-uapi/drm.h` (`sys/ioccom.h` is absent
in MinGW) is only the first portability error. Supplying that header would
mask a platform include and still leave all of the missing DRM operations
above. Do not treat an ioctl-header shim as a winsys port.

## Required safe submission boundary

Do not add an escape that blindly executes an application-provided pushbuffer.
NVC0's stream contains method headers, state writes, addresses, and relocation
sites. A viable interface needs kernel-owned context/channel state and
validated references. The next ABI design should include:

1. Opaque per-process resource IDs with create/destroy, supported layouts,
   map/unmap or staging transfer, and references held through completion.
2. A batch containing bounded command words plus a relocation table of
   `(resource_id, offset, access, packet_word, address_kind)` entries. Validate
   every range and relocation against resources owned by that client.
3. A method allowlist and packet parser for the C797 classes and subchannels
   used by NVC0. Reject context switching, privileged classes, unsupported
   methods, malformed packet lengths, out-of-range addresses, and unreferenced
   address writes before touching the GPFIFO.
4. Kernel-side patching of validated resource references to GPU VAs, channel
   ownership, batch submission, monotonically increasing completion fences,
   and deferred destruction after the last fence.
5. Bounded tests that first submit a NVC0-generated clear, then a shader-backed
   triangle, then resource transitions and presentation. Only advertise each
   Gallium cap after that operation is implemented and pixel-checked.

This parser and ownership model is a significant miniport feature. An
alternative with a smaller first milestone is a new NVX Gallium driver that
reuses NVC0 codegen but translates only a deliberately small set of Gallium
operations to kernel-built C797 batches. It would reuse less NVC0 frontend
code, but keeps the current kernel's trusted command generation and avoids
pretending its enum-based test ABI is a general winsys.

### First batch inspection: NVC0 render-target clear

The pinned source's `nvc0_clear_render_target` in
`mesa/src/gallium/drivers/nouveau/nvc0/nvc0_surface.c` is a useful first
sample. It pushes a write reference to the target BO, then emits:

1. `CLEAR_COLOR(0)` with four float payload dwords.
2. `SCREEN_SCISSOR_HORIZ` with two packed rectangle dwords.
3. `RT_CONTROL = 1`.
4. `RT_ADDRESS_HIGH(0)` with nine payload dwords. The first two encode the
   target GPU address and are a BO relocation pair; the remaining payloads
   describe width, height, format, tile/layout, layer/depth, and stride.
5. `MULTISAMPLE_MODE`, conditional-mode state as needed, and
   `CLEAR_BUFFERS` with color mask `0x3c` and layer in bits 10 onward.
6. A conditional-mode restore when a render condition is disabled.

The Gallium framebuffer clear callback at lines around 700 also first calls
`nvc0_state_validate_3d(...FRAMEBUFFER)`, which can emit a larger dirty-state
stream and target binding. The dedicated clear-render-target callback is a
better bootstrap because it explicitly binds the target and carries one
obvious address relocation. It still assumes prior channel/class/context
initialization and must not be mistaken for a complete frame.

This sample suggests a practical first parser gate: accept only the specific
`0xC797` subchannel/method sequence and payload sizes used above, require one
write relocation at the target address pair, verify that the supplied
resource is a render target with matching format/layout/extent, bounds-check
the scissor and layer, and reject any extra packet or relocation. The kernel
can then patch the resource address and submit. This provides a concrete
starting point for an allowlist; it does not yet cover shader draws, which
need many more method ranges, executable/code BOs, vertex/index inputs, and
shader/resource address validation.

## Build evidence and next gate

`test/build_mesa_xp64.sh` can select `nouveau,softpipe` through
`NVX_MESA_GALLIUM_DRIVERS`. Configuration succeeded and the isolated
`src/gallium/drivers/nouveau/codegen` target compiled and linked with MinGW
XP x64. The full Nouveau driver build stops in the Linux DRM/UAPI boundary;
no XP runtime or GL acceleration is claimed by this experiment.

Next gate: expose an XP winsys entry that creates an NVC0 screen/context, and
connect the Gallium clear callback to this validated packet escape using
resource IDs and layout metadata from that winsys. The current test constructs
the packet manually, so it verifies encoding, kernel parsing, translation, and
hardware pixels but not NVC0 runtime integration. Continue with BO mapping and
transfers, context/object setup, fences, then shader-backed draw submission;
do not advertise general GL acceleration before those gates pass. Keep the
Mesa submodule pinned and clean; all adaptations belong in `mesa_patches/` and
`src_driver/`.
