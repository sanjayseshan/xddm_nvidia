# Nouveau NVC0 to XP NVX port map

Status: source/API mapping, an XP device/BO adapter, and a narrow clear-packet
bridge; no Mesa submodule files were changed. The pinned NVC0 compiler target
cross-builds for XP x64.
The softpipe WGL bridge now constructs the NVC0-format clear packet from
Gallium clear state and its NVX resource metadata. On RTX 3070, the XP miniport
validated and translated 104 such clears in one 100-frame diagnostic run.
This is still not a packet emitted by Nouveau NVC0 Gallium and does not provide
a native NVC0 winsys or shader-backed general OpenGL.

NullMoth's documented Mac stack uses Apple's OpenGL-on-Metal frontend, a
Metal-to-SPIR-V translator, Mesa NVK/NAK, and NVIDIA's open kernel modules
ported to XNU; it does not run Nouveau Gallium NVC0. Its reusable principle is
keeping Mesa's backend while supplying the target OS boundary. The current XP
NVC0/Gallium experiment applies that principle to Mesa's OpenGL-capable
Gallium/NVC0 path. A more literal Metal-style layering would be GL-over-Zink
over-Vulkan/NVK and would require an XP NVK winsys. Source: NullMoth's
[HOW-IT-WORKS.md](https://github.com/nullmoth/nvidia-macos-driver/blob/main/docs/HOW-IT-WORKS.md).

## Latest XP cross-build audit (November 11, 2026)

Meson 1.12.1 is installed in `/tmp/nvx-meson-venv`. The standard
`NVX_MESA_GALLIUM_DRIVERS=softpipe` XP x64 package completed, producing
`dist_full/nvxogl64.dll` and `dist_full/nvxmesa.dll`; imports are limited to
GDI32, KERNEL32, msvcrt, and USER32, and the PE subsystem is 5.2. This was freshly rebuilt as 869 Ninja targets with Meson 1.12.1; the PE subsystem is 5.2, the imported DLL set remains GDI32, KERNEL32, msvcrt, and USER32, and the Mesa submodule returned clean. This is a successful baseline build, not a Nouveau runtime result.

The Nouveau-enabled build configured Mesa 27.0.0-devel and compiled the NVC0
compiler, Gallium, softpipe, and most legacy winsys sources. It failed when
`src/gallium/winsys/nouveau/drm/nouveau.c` included `<xf86drm.h>`. Full output is
in `../test_logs/mesa-nouveau-xp64-20261111.log`. The command line's final
`_WIN32_WINNT` and `WINVER` definitions are both `0x0502`. No Nouveau ICD was
produced. The XP build script reversed external patches on failure, and the
Mesa submodule is clean.

External patches `0003-nouveau-wgl-exclude-nvk-winsys.patch` and
`0004-nouveau-wgl-drop-unused-renderonly.patch` keep NVK-only and unused DRM
includes out of this Gallium build. The remaining error is the actual Linux
libdrm device/API boundary, not an ioctl-number header. Replace Gallium NVC0's
legacy DRM device/channel/BO/pushbuffer/fence services with the XP NVX winsys;
do not add a fake `xf86drm.h` that postpones that port.

This source split matters to the port plan: direct OpenGL uses Gallium NVC0 and
the legacy Gallium Nouveau winsys (`src/gallium/winsys/nouveau/drm/nouveau.c`,
`nouveau_drm_winsys.c`). NVK uses the separate shared `src/nouveau/winsys` and
Vulkan/NVK; OpenGL over NVK would add Zink and is not the direct Gallium OpenGL
route. Use NVK's OS-boundary design as a reference, not its winsys as a drop-in
for NVC0. The next implementation boundary is the legacy Gallium winsys API:
device/channel/object setup, BO ownership and mapping/transfers, pushbuffer
relocation, validated submission, fences, then WGL screen creation and
presentation. The current NVX semantic-operation ABI cannot satisfy NVC0's
pushbuffer API.

### Batch and relocation structure validator

The common ABI now defines a 32-byte bounded batch header and fixed-width
`NVX_3D_RELOCATION` records. `Nvx3dValidateNvc0Batch()` parses a copied batch
with exact total-size checks, limits it to 4096 dwords and 256 relocations,
accepts only structurally valid NVC0 type-2/type-6 method packets and type-8
immediate headers, and rejects truncated/zero-length packets. Each relocation
must target a packet payload word exactly once and reference a BO ID owned by
the batch client token with valid access bits, range, reserved fields, and
non-overflowing GPUVA. `NvxNouveauBoBuildRelocation()` constructs records from
a live adapter BO; `NvxNouveauBoGetAddress()` provides checked CPU-side
resolution.

ASan/UBSan host tests cover valid streams plus malformed packet types/counts,
truncated input, bad tokens, header/immediate relocation targets, duplicate
relocation words, missing/wrong BO IDs, invalid ranges/access, reserved fields,
and overflow. `make -C src_driver check` passed with x86/x64 MinGW builds. The
XP x64 softpipe ICD rebuilt all 621 Ninja actions; its PE subsystem/import
audit passed and Mesa remained clean.

This is a structural preflight only. It does not validate allowed C797
methods/subchannels, prove that each address method has a relocation, or submit
commands. The miniport does not consume this batch format yet, and Mesa NVC0
does not call the adapter. Never treat a structurally valid batch as safe to
execute until the kernel-side allowlist and per-method relocation checks are
implemented.

## Current NVC0 integration audit (November 11, 2026)

The standalone NVX device/BO adapters still have no Mesa NVC0 callsites. The
WGL HDC discovery call is currently part of the optional diagnostic bridge; it
does not select or construct a Gallium Nouveau screen.

`nouveau_drm_screen_create()` is an fd-based factory. Its static screen creator
constructs a DRM client/device before selecting `nvc0_screen_create()`. The
NVC0 initializer then calls `nouveau_screen_init()`, creates a channel and
pushbuffer, allocates/maps a fence BO, creates SW/M2MF/copy/2D/3D class objects,
and emits initialization methods before returning a screen. Those calls occur
before the first GL draw, so wiring the existing BO adapter alone cannot
bootstrap the screen. The current miniport owns one channel and constructs
commands for a closed set of operations; it has no Gallium client channel or
pushbuffer submission service.

The first mandatory winsys call in `nouveau_screen_init()` is
`nouveau_object_new(&dev->object, 0, NOUVEAU_FIFO_CHANNEL_CLASS, ...)`.
Client and pushbuffer creation happen only after a FIFO channel is returned.
`nvc0_screen_create()` then immediately allocates/maps a fence BO, creates
SW/M2MF/copy/2D/3D engine objects, and kicks initialization methods. An
HDC-to-device probe, BO wrapper, or WGL presentation branch cannot substitute
for that bootstrap. The next backend contract must define whether Gallium owns
or shares the current graphics channel, and provide engine-object binding plus
ordered, fenced pushbuffer submission before the screen factory can return a
usable `pipe_screen`.

`make -C src_driver check` was rerun after the latest adapter changes. The
ASan/UBSan host tests passed, including multi-BO mapping/lifetime checks, and
the adapter and ABI sources compiled with both x86 and x64 MinGW. This verifies
the current boundary helpers only; it does not test Mesa callsites, channel
creation, command submission, or guest execution.

The first useful integration milestone is therefore a real NVX-created
`nouveau_device`/client/channel/pushbuffer contract, with kernel-owned context
and explicit BO-reference/relocation validation. It must be exercised by
NVC0's existing `nvc0_screen_create()` initialization stream. Preserve Mesa's
NVC0 state and compiler code; do not route general OpenGL back through the
softpipe bridge or bypass the miniport validator with raw user pushbuffers.

## Project direction

Prioritize integrating Mesa Nouveau NVC0 as the long-term OpenGL path. The active WGL bridge remains softpipe plus custom NVX operations and is not Nouveau, even where its miniport emits C797 work. Keep it only as a diagnostic/prototype while building the genuine Nouveau OS boundary. The target is to preserve Mesa's Nouveau state translation, NVC0 shader compiler, and command generation, and replace DRM-facing device, BO, channel, submission, and fence services with XP/NVX equivalents. Avoid extending the bridge into a second renderer. Do not claim Nouveau acceleration until a Gallium-created NVC0 screen submits a NVC0-generated command stream and a shader-backed draw passes a pixel comparison on XP hardware.

## Current NVX transport helper (November 11, 2026)

`../mesa_patches/nvx_winsys.[ch]` and `nvx_winsys_win32.[ch]` centralize XP Escape calls for caps, opaque buffer allocation/free, bounded uploads/downloads, and fence waits. New `nvx_nouveau_bo.[ch]` wraps that transport with BO references, last-use fence waits, a CPU shadow map, download-on-map, and upload-on-write-unmap. `../test/nvx_winsys_test.c` exercises transfer chunking and BO mapping/lifetime with mocked transport; the mock supports multiple simultaneous BOs with independent storage and checks content isolation, unique handles/GPU VAs, and independent fence-before-free ordering. `make -C src_driver check` passes under ASan/UBSan, and the BO source compiles with x86 and x64 MinGW. This is a BO adapter primitive only: it is not yet wired into Mesa's `nouveau.c`, and there is still no Nouveau device/context/object integration, BO placement/import/export, relocatable pushbuffer, or generic NVC0 submission.

The latest hardware log `../test_logs/v3.0.0.2-20261111-114618.log` confirms the active renderer is still the bridge path: 106 callbacks/batches and 104 validated clears are logged; the no-depth cube matched its reference (`reference_diff=0`), while the depth-enabled cube differed by 11876 pixels. ABI 12 reported `PCI=2488`, `chipset=0x170`, `VRAM-aperture=67108864`, and `GART=0`. WGL's HDC-backed Nouveau device adapter reported `ready=1`, proving the HDC-to-NVX discovery call works in the guest. All 100 WGL frames completed. The separate NVC0 clear and 3D probes passed; firmware assertions remained at the startup count of one. This is not evidence that Nouveau is rendering OpenGL: WGL still uses softpipe and its diagnostic bridge.

### ABI 12 hardware identity and adapter integration

The current ABI is version 12. `NVX_3D_QUERY_CAPS` reports the PCI device ID,
GA104 chipset ID, mapped VRAM aperture, and GART size. The 64 MiB value is the
miniport's current GPUVA aperture, not physical VRAM capacity; the adapter names
it `vram_aperture_bytes` so it cannot be mistaken for Nouveau's total
`vram_size`. `nvx_nouveau_device.[ch]` validates this identity and the
capabilities needed by the initial adapter;
`nvx_nouveau_bo.[ch]` provides opaque allocation, shadow mapping, bounded
transfers, and fence-aware lifetime. Both adapters are linked into the XP Mesa
WGL target by patch `0001`; host mock tests and x86/x64 MinGW compilation pass.

The device adapter also requires download capability because BO mapping
preserves existing contents through a CE download, including write mappings.
The mock test rejects a device without download support. `make -C src_driver
check` passes with ASan/UBSan and x86/x64 MinGW adapter builds.

WGL now calls the device adapter during screen setup and confirms RTX 3070
identity from the active HDC. This does not create a Nouveau screen: the ICD
still selects softpipe, the BO adapter is not wired into Nouveau, and there is
no Nouveau-owned channel, object binding, pushbuffer relocation, or native
screen. The next bring-up work is the XP-owned context/channel and safe
submission contract, then WGL screen creation when it can satisfy NVC0's
initializer. The softpipe bridge stall is not a proxy for native Nouveau
progress.

## What already maps

| NVC0 expectation | NVX state today | Port consequence |
| --- | --- | --- |
| Chipset detection and NVC0 screen selection | Device is known to be GA104/C797 through the XP display driver | A dedicated NVX `pipe_screen` factory can select NVC0 without Nouveau's DRM fd discovery. |
| Shader compiler/codegen | `src/gallium/drivers/nouveau/codegen` compiled with the MinGW XP target | Reuse this code. Keep it as unmodified Mesa source and link it into the NVX Gallium driver. |
| GPU buffer allocation | `NVX_3D_ALLOC_BUFFER` returns opaque `BufferId` and GPU VA; allocations live in a bounded VRAM window | Can back some `nouveau_bo`-like resources, but current metadata/layout and usage set is narrow. Introduce a real per-context resource table and references. |
| Host-to-GPU upload | `NVX_3D_UPLOAD_BUFFER` copies bounded chunks through the CE | Can provide initial staging uploads. NVC0 also writes mapped GART buffers directly; `nvx_nouveau_bo` now provides a CPU shadow map and writeback primitive, but it is not connected to Mesa BOs. |
| Fence wait | `NVX_3D_WAIT_FENCE` waits for NVX's internal submission fence | `nvx_nouveau_bo` waits before CPU map and destruction; Gallium flush fence integration and multi-context semantics remain to be implemented. |
| Simple GPU operations | `NVX_3D_SUBMIT` accepts a closed set of driver-built clear and triangle requests | Useful for probes only; it is not a generic NVC0 command submission path. |

## Implemented narrow clear bridge

`NVX_ESCAPE_3D_NVC0_CLEAR` / `IOCTL_NVX_3D_NVC0_CLEAR` is the semantic
translation path. `NVX_ESCAPE_3D_NVC0_NATIVE_CLEAR` is a second, exact-profile
execution path. Both accept only a tightly validated 28-dword packet. The
translation path converts the clear color into a trusted kernel-built C797
operation; the native path submits the validated NVC0 words directly and adds
kernel-owned idle/fence methods. It does not allow arbitrary NVC0 packets.
The miniport verifies every method header and payload, target handle/address,
allocation extent/layout, single-layer RGBA8 format, full-surface scissor,
zero-sample mode, clear mask, and UNORM float range. The rapid test verifies
direct and translated output independently.

`NVX_ESCAPE_3D_NVC0_CLEAR` / `IOCTL_NVX_3D_NVC0_CLEAR` was introduced in ABI v10. The
miniport accepts exactly 28 dwords and checks each NVC0 packet header and
payload, the target's opaque ID and exact GPU VA, allocation extent/layout,
single-layer RGBA8 format, full-surface scissor, zero-sample mode, color-only
clear mask, and UNORM float range. It converts the four float color values to
packed AARRGGBB, then calls a miniport-owned C797 clear builder; it never
submits the user packet words on this translation path. The new native path
uses the same validation but submits exactly those words with kernel-added
completion methods. `../test/test_gpu_accel.c` exercises both paths, waits for
each returned fence, and reads back all 1024 pixels.

Evidence: `make -C src_driver check` passed including malformed-packet host
tests and x86/x64 ABI compilation; `make -C src_driver full` and
`make -C src_disp all` built the XP packages. The independent probe in
`../test_logs/v3.0.0.2-20261111-101720.log` reports
`[NVX_NVC0_CLEAR_RESULT] PASS`, exact 1024/1024 pixel match to `0xFF8040FF`,
fence 871. In the same run the opt-in Mesa bridge status recorded 104 successful
NVC0 clears across 106 GPU batches; the 632x453 no-depth reference comparison
passed with zero differing pixels. The depth-enabled cube comparison still
failed, and general OpenGL remains softpipe plus a limited custom draw bridge.

## Current pinned source entry points confirmed in the XP build

The XP build conditionally excludes the NVK shared winsys when Vulkan Nouveau is disabled. Gallium NVC0's legacy winsys remains a distinct, required port surface. Its `nouveau.c` contains the device, channel, BO, relocation, and GEM pushbuffer operations; `nouveau_drm_winsys.c` discovers a screen from a DRM file descriptor. WGL's current factory in `mesa/src/gallium/targets/wgl/wgl.c` has no Nouveau branch and only receives the active HDC.

This pinpoints the integration seam: an HDC-created XP device and a replacement for the legacy Gallium Nouveau DRM winsys, then explicit WGL selection/presentation. Adding POSIX headers or implementing only `drmCommand*` would leave channel ownership, BO mapping/transfer, relocation submission, and presentation unresolved.

## Libdrm boundary inventory from the pinned source

The XP compile failure is only where the compiler first reaches the Linux boundary. Direct source inspection shows the required port is broader:

- Legacy `nouveau.c`: libdrm device/NVIF initialization, channel/object lifecycle, GEM BO allocation/map, BO references, pushbuffer relocation and GEM_PUSHBUF submission. Replace these operations with an HDC-created NVX device and a validated miniport ABI; do not shim `xf86drm.h` to pretend DRM is available.
- `nouveau_drm_winsys.c`: file-descriptor screen discovery. Add an XP entry point that receives the active WGL adapter and creates the NVC0 screen from the XP device.
- NVK's `src/nouveau/winsys/nouveau_device.c`, `nouveau_context.c`, and `nouveau_bo.c`: separate shared Vulkan winsys services using DRM/NVIF, VM_BIND, `mmap`, and libdrm. Keep this out of an NVC0-only XP build; use it as a design reference if later evaluating NVK/Zink.

The active WGL build also does not yet register or link `driver_nouveau` into `src/gallium/targets/wgl/wgl.c`. The work order is to replace the legacy Gallium DRM device/context/BO/pushbuffer services with the XP boundary, then link/select the screen through WGL, and then validate native NVC0 submissions and presentation on XP. A compile-only ioctl shim is insufficient for these runtime requirements.

## NVC0 calls that do not have an XP equivalent yet

The relevant source is `../mesa/src/gallium/winsys/nouveau/drm/nouveau.h`,
`../mesa/src/gallium/winsys/nouveau/drm/nouveau.c`, and
`../mesa/src/gallium/drivers/nouveau/nouveau_winsys.h`.

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

The include failure at `../mesa/include/drm-uapi/drm.h` (`sys/ioccom.h` is absent
in MinGW) is only the first portability error. Supplying that header would
mask a platform include and still leave all of the missing DRM operations
above. Do not treat an ioctl-header shim as a winsys port.

## WGL integration boundary

The pinned WGL screen factory is `../mesa/src/gallium/targets/wgl/wgl.c`.
`wgl_screen_create_by_name` currently knows llvmpipe, D3D12, Zink, and
softpipe, but has no Nouveau/NVC0 case. When `GALLIUM_DRIVER=nouveau` is set,
the requested driver therefore cannot produce a screen. The WGL presentation
switch also has no Nouveau case; it only handles llvmpipe, D3D12, Zink, and
softpipe. Thus enabling `nouveau` in the Meson driver list alone cannot route
an XP WGL context to NVC0 or present its target.

The required external Mesa delta needs an XP factory taking the active `HDC`
and an NVX winsys/device, plus a present path that copies or flips the
GPU-owned target through a validated miniport operation. Package these as a
patch in `../mesa_patches/` and apply/reverse it in `../test/build_mesa_xp64.sh`; do
not edit the Mesa submodule in place. The factory must not claim success until
it can create and destroy a real NVC0 context with resources and fences. The
presentation path should avoid per-draw CPU readback.

## NVC0 screen bootstrap dependencies

Source inspection of the pinned implementation shows why wiring only the new
BO helper cannot create a screen. `nouveau_drm_screen_create()` currently
creates a DRM client and device, selects `nvc0_screen_create()` by chipset, and
then calls into `nouveau_screen_init()`. That base initializer creates a FIFO
channel object, client, pushbuffer, and VRAM/GART memory managers before the
NVC0 screen allocates resources. `nvc0_screen_create()` then allocates and maps
a fence BO, binds the software/M2MF/copy/2D/3D classes, and emits initial
pushbuffer state. The first actual NVC0 screen therefore needs a coordinated
device, channel/object, BO, pushbuffer, and fence contract; a passing BO map
test by itself cannot advance screen creation.

The next integration milestone should create/destroy an NVX-backed Gallium
device and report honest GA104 capabilities, then allocate the channel's
pushbuffer and fence resources and submit the initialization stream through a
validated batch ABI. Only after this bootstrap passes should Gallium NVC0's
resource allocator be switched to `nvx_nouveau_bo`, followed by a native NVC0
clear. The generic batch ABI must validate method packets and every address
relocation against client-owned BOs; simply forwarding `nouveau_pushbuf_kick()`
data to the miniport is not acceptable.

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
`../mesa/src/gallium/drivers/nouveau/nvc0/nvc0_surface.c` is a useful first
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

`../test/build_mesa_xp64.sh` can select `nouveau,softpipe` through
`NVX_MESA_GALLIUM_DRIVERS`. The direct XP x64 build
`/tmp/nvx-mesa-nouveau-wgl` configured successfully and compiled Nouveau
headers/codegen, but stopped at `src/gallium/winsys/nouveau/drm/nouveau.c:7`
because `xf86drm.h` is unavailable for the MinGW target. That source then
uses DRM version/device discovery, NVIF object/channel allocation, GEM BO
allocation/map/wait, and GEM pushbuffer ioctls throughout the file. Merely
adding an include shim would get past this first error but would not connect
those operations to NVX or make an NVC0 screen usable. Mesa's patch script
restored the submodule cleanly after the build.

Current WGL ordering is still `GALLIUM_DRIVER` override, NVX probe, then
software drivers. With no explicit `GALLIUM_DRIVER`, the NVX probe returns
true but its WGL hook calls `softpipe_create_screen()`. Thus the selected
Gallium screen and `GL_RENDERER` remain softpipe even though NVX batch probes
may separately submit bounded operations. There is no actual Nouveau screen
selection yet, and no XP runtime or Nouveau GL acceleration is claimed by this
build probe.

## XP replacement boundary for Nouveau DRM calls

The build failure is only the first portability break. The pinned
`nouveau.c` directly performs the following DRM work before and during NVC0
screen initialization:

| Nouveau API path | Current Linux operation | XP/NVX requirement |
| --- | --- | --- |
| `nouveau_drm_new()` | `drmGetVersion(fd)` | Bind an HDC-backed NVX transport and query actual adapter/ABI capabilities. |
| `nouveau_device_new()` | `DRM_NOUVEAU_NVIF`, `drmGetDevice2()`, `DRM_NOUVEAU_GETPARAM` | Construct real device info from NVX caps, PCI identity, VRAM aperture, engine class, and limits. |
| `nouveau_object_channel_new()` | `DRM_NOUVEAU_CHANNEL_ALLOC` | Define explicit ownership of the currently miniport-owned C56F channel, or implement client channels. |
| `nouveau_object_subchan_new()` / `nouveau_object_mclass()` | NVIF new/class-discovery ioctls | Bind and discover only classes the XP kernel actually created and supports. |
| `nouveau_bo_new()` / `nouveau_bo_map()` | GEM_NEW plus shared DRM mmap | Map BO allocation/layout and CPU synchronization to NVX-owned resources; the existing shadow map is not direct GPU mapping. |
| `nouveau_pushbuf_new()` / `nouveau_pushbuf_kick()` | GEM pushbuffer setup/submission | Keep relocation references alive, apply a kernel method allowlist, patch addresses, submit, and return a completion fence. Structural validation alone is not submission. |

This mapping rules out adding only an `xf86drm.h` compatibility header. The
next implementation unit must replace or conditionally adapt the Nouveau DRM
backend's operation layer to XP transport semantics, and extend the miniport
where the current NVX ABI has no equivalent. In particular, the current
miniport has no generic client channel/object lifecycle and its batch escape is
validation-only. Do not let the WGL factory call `nvc0_screen_create()` until
those operations can succeed; its initialization immediately allocates and
maps a fence BO, creates SW/M2MF/copy/2D/3D objects, and kicks setup methods.

Next gate: create the XP winsys/device boundary and external WGL factory/present
patch so an XP WGL context can request Nouveau and either create a genuine
NVC0 screen or fail explicitly. Then connect the Gallium clear callback to the
validated escape using resource IDs and layout metadata from that winsys. The
current test constructs the packet manually, so it verifies encoding, kernel
parsing, translation, and hardware pixels but not NVC0 runtime integration.
Continue with BO mapping and transfers, context/object setup, fences, then
shader-backed draw submission; do not advertise general GL acceleration
before those gates pass. Keep the Mesa submodule pinned and clean; all
adaptations belong in `../mesa_patches/` and `../src_driver/`.

## NVC0 screen-start call requirements (verified against pinned sources)

A direct screen factory cannot succeed by wiring only `NvxNouveauDeviceInitHdc()` and `NvxNouveauBo*()`. `nouveau_screen_init()` first creates a Nouveau FIFO channel, a client and a 512 KiB mapped pushbuffer, then creates both a `NOUVEAU_BO_GART | NOUVEAU_BO_MAP` memory manager and a VRAM manager. For GA104, `nvc0_screen_create()` immediately requests a 4 KiB GART-mapped fence BO, maps it, and creates/binds SW, M2MF, copy, 2D, and C797 objects. It then emits a long initialization stream and allocates additional text, uniform, TLS, and texture-cache BOs before the screen can return successfully.

The current XP caps report a 64 MiB GPUVA aperture and `GartBytes=0`; the current device adapter explicitly rejects nonzero GART and the BO adapter has no placement flags or direct mapped allocation. Those are concrete startup blockers before WGL can select this screen. An XP backend must provide channel/client lifetime, class discovery/object binding, a mapped fence mechanism (or an explicit NVC0 fence-update adaptation), VRAM allocation over the bounded aperture, and validated pushbuffer submit with address relocations. The 512 KiB init pushbuffer includes methods from multiple subchannels; the structural batch parser alone does not authorize it. Do not set a fabricated `gart_size` or claim the current shadow map is a GPU-visible mapping.

Pinned source references: `mesa/src/gallium/drivers/nouveau/nouveau_screen.c` (`nouveau_screen_init`, lines around 299-430), `mesa/src/gallium/drivers/nouveau/nvc0/nvc0_screen.c` (`nvc0_screen_create`, lines around 770-1100), and `mesa/src/gallium/winsys/nouveau/drm/nouveau.c` (device/object/BO/pushbuffer backend). This inventory supersedes the shorter bootstrap summary above; the source files remain unmodified in the Mesa submodule.

## User-side NVC0 batch preflight adapter (November 11, 2026)

Added `nvx_nouveau_pushbuf.[ch]` outside the Mesa submodule. It stages bounded NVC0 dwords, records checked BO-relative relocations through `nvx_nouveau_bo`, serializes the fixed-width batch ABI, and calls the XP validation escape. `NvxWinsysValidateBatch()` checks the exact envelope before the miniport repeats validation against its live BO table. The API is named `Validate`, not `Kick`: neither this adapter nor the miniport executes the submitted dwords.

`test/nvx_winsys_test.c` now drives this adapter through a mock XP transport under ASan/UBSan; the positive stream and malformed packet rejection pass. x86/x64 MinGW compiles pass. `make -C src_driver check` passed. The external `0001` Mesa build patch links the adapter into the XP WGL ICD; Meson rebuilt all 622 targets and the XP import/subsystem audit passed, with the Mesa submodule clean.

The real-hardware run `../test_logs/v3.0.0.2-20261111-124944.log` used the HDC-backed adapter to serialize two relocations for the live target allocation and pass miniport preflight. It then rejected wrong ABI, foreign token, and out-of-range relocation cases; the separate trusted C797 clear/fence/readback matched all 1024 pixels (`0xFF8040FF`, fence 2). GR triangle and semaphore probes passed. This run proves the XP transport and batch preflight path end to end. It does not prove that NVC0/Gallium generated this batch or that arbitrary methods can be executed. Remaining blockers are method/subchannel allowlisting, per-address-method relocation rules, a Nouveau-owned channel/object contract, safe kernel batch submission, mapped fence/resource integration, and WGL selection/presentation of an NVC0 screen.
