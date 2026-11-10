# Phase 2C: Mesa and XP x64 ICD

Status (November 10, 2026): Mesa is pinned and the XP x64 softpipe ICD builds
with XP-era imports. System WGL creates a current context. An external bridge
now intercepts bounded flat-color triangle/quad batches from Mesa's softpipe
vbuf callback, submits kernel-built C797 draws into one target, waits for each
fence, reads the target back, and places those pixels in the WGL framebuffer.
The RTX 3070 hardware run verifies green, red, and clear-background pixels for
two GL quads. The ICD still reports `softpipe`; clears, depth-tested draws, the
normal cube, full-size window targets, and unsupported state remain software.
This is a restricted hardware bridge, not a general Mesa hardware backend.

Latest hardware result: `test_logs/v3.0.0.1-20261110-081227.log`. The bridge
now uses the application HDC supplied at `DrvSetContext`, so its caps escape
reaches the miniport. Fenced C797 readback verifies a nine-tile color draw
(6,384 green, 528 red, 2,304 background pixels). The cube probe does not
submit a new batch after the preceding depth fault. A test-harness counter bug
had counted the prior color batch as cube evidence; this is fixed, and
`test_logs/v3.0.0.1-20261110-080658.log` now correctly reports
`MESA_GPU_CUBE_RESULT FAIL` with zero new batches. The Mesa renderer remains `softpipe`; unsupported
and general draws still execute in software. The zeta PTE now clears peer ID
bits 35:33 and sets `ATOMIC_DISABLE` bit 7, matching NVIDIA UVM's VER2 PTE
builder. Hardware still reports `3D-Z KIND Violation` and times out the depth
fence; firmware assertions rise from one to three, and the suite remains
`[OPENGL_RESULT] FAIL`. The format/KIND pair for
`PIPE_FORMAT_Z24X8_UNORM` is C797 `X8Z24` / KIND 3, matching Mesa NIL's table.
Alternate pairs did not resolve the zeta fault. Phase 2C remains incomplete.

## Pinned source and external patches

The `mesa/` submodule is pinned at
`cc2dcf9abdc6afc093e85d48e4cfa34eed60a01e` (Mesa `27.0.0-devel`). XP
customizations are stored in `mesa_patches/0001-xp-win32-shims.patch`, outside
the submodule. Apply them with:

```sh
./test/apply_mesa_xp_patches.sh
```

The script checks the pinned revision and avoids applying the patch twice. The
patch adds XP implementations for condition variables, one-time initialization,
futex waits, and shared/exclusive locks; it also disables Windows 10-only WGL
present callbacks, and links an XP `GetTickCount64` shim for MinGW winpthread.
The shim source is kept in `mesa_patches/`. The submodule can be returned to its pristine pinned state
with `git -C mesa restore --worktree --staged .` after building. Keep source
customizations in `mesa_patches/` so updating Mesa stays reviewable.

The condition-variable and shared/exclusive-lock primitives have a repeatable
Wine smoke test at `test/test_mesa_xp_shims.sh`. It cross-compiles a Windows x64
test program and checks condition broadcast plus concurrent readers and
writers.

## Reproducible XP x64 software WGL build

With Meson, Ninja, and the MinGW x64 toolchain, run:

```sh
MESON=/path/to/meson NVX_MESA_BUILD_DIR=/tmp/mesa-xp64-build \
  ./test/build_mesa_xp64.sh
```

The generated DLL is
`/tmp/mesa-xp64-build/src/gallium/targets/wgl/nvxogl64.dll`. The cross build
uses Mesa's `min-windows-version=5` setting, `_WIN32_WINNT=0x0502`, and PE
subsystem version 5.2. XP `msvcrt.dll`
does not export `qsort_s`, so the build selects Mesa's fallback. The XP-safe
shim also removes `GetTickCount64` and Win7 condition-variable imports.
`test/audit_xp_imports.py` compares every named package import against exports in
the actual XP guest DLLs and rejects PE subsystem versions newer than 5.2.
It selects x64 exports from `System32` and x86 exports from `SysWOW64`. The
November 9 audit passes for `nvxmp.sys`, `nvxdisp.dll`, `nvxogl64.dll`, and
both x64 and x86 `test_gpu_accel.exe` builds. Neither package imports
`qsort_s`, `GetTickCount64`, or Vista condition-variable APIs. Wine smoke tests
pass for the XP shims and WGL ICD load/export resolution.

The XP hardware run reports `OPENGL_GETINFO` support and the expected `nvxdisp`
name. Direct DLL loading, Mesa screen initialization, pixel-format setup, and
direct context creation succeed. XP's system WGL dispatcher loads the ICD and
resolves its required exports, but still returns `wglCreateContext` failure;
the regular OpenGL audit reports `[OPENGL_RESULT] FAIL`. The November 10
callback-trace run `test_logs/v3.0.0.1-20261110-010419.log` confirms the
dispatcher resolves `DrvValidateVersion`, `DrvSetCallbackProcs`, and
`DrvCreateContext`, but invokes none of those callbacks before failing. A
direct call through the same resolved `DrvCreateContext` pointer succeeds.
Skipping the separate direct Mesa frontend test does not change the failure,
so that diagnostic was not the cause. A test-only copy of
Mesa's `libgl-gdi/opengl32.dll`, named `C:\\test_accel\\nvxmesa.dll`, bypasses
that dispatcher and is loaded explicitly by the test harness. In
`test_logs/v3.0.0.1-20261109-233451.log`, it created a context, reported
`Mesa / softpipe`, cleared and read back `32,64,191,255`, and swapped
successfully. This frontend is diagnostic only; XP's System32 `opengl32.dll`
is untouched. A 520-byte XP legacy escape query returns the expected driver
name, as does the 532-byte query; XP import and PE subsystem audits pass.
Normal XP application dispatch is not yet working.

The November 10 follow-up run `test_logs/v3.0.0.1-20261110-011940.log` enabled
the display driver's pixel-format DDI hooks for the ICD build. XP then selected
pixel format 1 with flags `0x25`, so `PFD_GENERIC_FORMAT` was cleared. The
system WGL `wglCreateContext` still failed, while the direct `DrvCreateContext`
call succeeded after explicit ICD initialization. This narrows the remaining
dispatcher failure beyond GDI Generic format selection; the non-generic display
DDI configuration should remain enabled for further ICD tests. The next run,
`test_logs/v3.0.0.1-20261110-012907.log`, additionally omits Mesa's
Vista-era `PFD_SUPPORT_COMPOSITION` flag in the XP build. It still selects
format 1 with `0x25` flags and still fails system `wglCreateContext`, so that
flag was not the remaining cause. The bounded GPU 3D test passed again; the
firmware assertion count was one with no MMU fault.

The selected Gallium driver is `softpipe`, which renders on the CPU. The
miniport ABI supports owned buffers, bounded uploads, fixed clear and
diagnostic-triangle operations, a bounded client-supplied triangle, and fences.
It does not accept Mesa-generated shader programs or general draw streams.

The preceding full hardware run `test_logs/v3.0.0.1-20261110-022255.log` verifies the
first general-purpose resource transfer primitive. The 3D escape ABI accepts a
fixed upload header plus at most 0xF000 bytes, validates the allocation ID and
offset/length while marking the buffer busy, then stages data through the
verified Copy Engine path. The test uploaded 256 patterned bytes at allocation
offset 0x1000; the miniport copied them back from GPU memory and recorded zero
mismatches. The run also passed the fixed clear/triangle/fence test, XP
import/subsystem audit, and 2D checks. OpenGL still reports `SOFTWARE`; the
startup firmware assertion count remains one.

The follow-up run `test_logs/v3.0.0.1-20261110-023854.log` verifies a bounded
client-supplied draw. XP uploads three 24-byte-stride vertices to a separate
allocation and submits the target and vertex-buffer IDs. The miniport copies
the 72 vertex bytes through the verified CE path, rejects components outside
[0,1], and constructs all shader/method/semaphore state itself. The RTX 3070
rasterized the changed triangle footprint; readback found 288 drawn pixels and
736 white background pixels, fence 6 completed, and a subsequent caps query
advertised client-triangle support. The guest asserted this capability. XP
import audits and 2D checks also passed.
The OpenGL result remains `SOFTWARE`: this is a client-to-GPU primitive, not a
Mesa Gallium backend.

Historical ICD trace run `test_logs/v3.0.0.1-20261110-015003.log` reads
the Mesa-side ICD trace both immediately after `wglCreateContext` and again
after explicit direct ICD probes. The first read has no trace file, while the
second records `DrvSetPixelFormat` and `DrvCreateContext`; this proves the
instrumented Mesa DLL can write the file and that those direct entry points
work. The system WGL call still fails with error 2 before any instrumented Mesa
callback is entered. The ICD package and XP import/subsystem audits pass, but
the WGL-to-ICD handoff remains unresolved. The same run passes the bounded
GA104 triangle and fence test, while its firmware assertion count remains one.

The November 10 01:59 run, `test_logs/v3.0.0.1-20261110-015903.log`, tests a
Mesa-side lazy initialization change in `DrvCreateContext`. This handles the
case where XP/GDI pixel-format routing bypasses Mesa's `DrvDescribePixelFormat`;
the direct context probe succeeds without separately calling `stw_init_screen`
or Mesa's `DrvSetPixelFormat`. XP system WGL still fails before that callback.
The follow-up trace-enabled run,
`test_logs/v3.0.0.1-20261110-020217.log`, passes the actual descriptor returned
by `DescribePixelFormat` into `SetPixelFormat`. XP reports 38 formats and flags
`0x425`, then still fails `wglCreateContext` without entering an ICD callback.
This makes the direct Mesa initialization sequence more robust but does not
change the XP dispatcher failure.

The first successful system-WGL run, `test_logs/v3.0.0.1-20261110-020644.log`,
changes the legacy XP ICD registration and OPENGL_GETINFO metadata to the
version convention used by XP display ICDs: interface `Version=2` and driver
`DriverVersion=0x00010000`, matching the registry, INF, display escape, and
`DrvValidateVersion` argument. With those values XP enters Mesa's ICD callbacks:
the trace records `DrvValidateVersion`, `DrvSetCallbackProcs`, and repeated
pixel-format calls. `SetPixelFormat` succeeds for format 87; system
`wglCreateContext`/`wglMakeCurrent` succeed; the XP test renders 100 frames and
reads back a red center pixel with `GL_NO_ERROR`. The renderer is
`Mesa / softpipe`, so this resolves system ICD dispatch but does not provide
RTX rendering. The same run passes the bounded GA104 triangle/fence and the
XP import/subsystem audits; one startup firmware assertion remains.

Validation commands/results:

```sh
./test/test_mesa_xp_shims.sh
./test/test_mesa_wgl_load.sh /tmp/mesa-xp64-build3/src/gallium/targets/wgl/nvxogl64.dll
x86_64-w64-mingw32-objdump -p /tmp/mesa-xp64-build3/src/gallium/targets/wgl/nvxogl64.dll
```

Both Mesa smoke tests pass. The November 10 full hardware test log is
`test_logs/v3.0.0.1-20261110-010419.log`: x64 and x86 XP import/subsystem
audits pass, the boot-time and bounded GR triangle tests pass, and display/2D
checks pass. The startup firmware assertion count remains one. Every mandatory
XP ICD export is present; direct initialization and context creation succeed,
but the system `wglCreateContext` still fails before invoking ICD callbacks.
The DC retains ICD pixel format 1 after `SetPixelFormat`, and
`DescribePixelFormat` reports 38 formats in this system-dispatch path. Thus
the failure persists with the ICD-selected format, and pixel-format selection
is not the cause. The separate Mesa frontend test was skipped for this run;
skipping it does not change the WGL failure compared with the previous run.
Registering the test frontend itself as the ICD (`NVX_OPENGL_ICD_DLL=nvxmesa.dll`)
did not change the XP dispatch result; the diagnostic run is
`test_logs/v3.0.0.1-20261109-234402.log`. That rules out a simple choice between
the standalone WGL ICD target and the libgl-gdi frontend as the context-creation
failure.

The November 10 01:04 direct hardware run,
`test_logs/v3.0.0.1-20261110-010419.log`, enables
`NVC7B5_LAUNCH_DMA_MULTI_LINE_ENABLE` in the shared CE copy path. This corrects
the prior one-row readback artifact: the white `CLEAR_SURFACE` operation and
CE prefill each verify all 1,024 pixels, and target readback covers the full
64x16 surface. The bounded triangle operation now uses three vertices and
`TRIANGLES` (the earlier four-vertex strip formed a full-target rectangle).
The guest reports `[NVX_3D_RESULT] PASS`, with 512 green RGB pixels and 512
white pixels across the full surface, and its fence signals. The shader's
green pixels have alpha zero, which the verifier now accepts while requiring
the complete green/white partition. Startup firmware assertion count remains
one and no MMU fault is reported. The earlier 64-green/960-black result in
`test_logs/v3.0.0.1-20261110-002907.log` was caused by the CE path copying only
the first 256-byte row and is not valid full-target evidence.

The test frontend also passes the complete OpenGL cube workload when deployed
as an application-local `opengl32.dll` in `C:\\test_accel`, without replacing
XP's System32 copy. Enable this test layout with `NVX_MESA_APPLOCAL=1` when
running `test/test_full_driver.sh`. The run
`test_logs/v3.0.0.1-20261109-234722.log` reports `[OPENGL_RESULT] SOFTWARE`,
Mesa/softpipe, 100 rendered frames, and a red center-pixel readback with
`GL_NO_ERROR`. This gives XP applications that can carry the side-by-side DLL
a tested software WGL path while the system ICD route is unresolved. It does
not provide RTX acceleration.

The separate `dist32/` driver package and x86 test programs also pass an import
audit against the XP x86 guest's `System32` exports. The 32-bit miniport and
display DLL use subsystem 5.1; the test executables use 5.2. There is no x86
Mesa ICD in `dist32/` yet; the Mesa WGL package remains x64-only.

## Hardware backend and remaining gates

Mesa's NVK/Nouveau drivers currently depend on Linux Vulkan/DRM interfaces and
do not provide a Windows Gallium backend. The available WGL frontend therefore
cannot reach the GA104 miniport. Completing hardware OpenGL requires:

1. An XP-compatible Gallium driver or Vulkan device layer that translates Mesa
   state into an NVX device interface.
2. An XP Gallium backend that maps supported GL state and vertex arrays to the
   validated resource and client-triangle ABI, and presents GPU render targets.
3. Expand the safe kernel-built draw set and add resource/context lifetime
   management; do not expose client pushbuffers or shader code.
4. An XP guest test that verifies an NVK/GA104 renderer string, a real OpenGL
   draw, completed fences, rendered pixel readback, no new firmware assertions
   or MMU faults, and stable display/2D operation.

The Phase 2B baseline remains
`test_logs/v3.0.0.1-20261109-195815.log`: it verifies an in-kernel Ampere
triangle and pixel readback but reports software OpenGL. Do not register or
describe the current `softpipe` ICD as hardware accelerated.

## Bounded WGL bridge update

The latest full hardware run,
`test_logs/v3.0.0.1-20261110-035522.log`, exercises the normal system WGL ICD
with two flat-color `GL_QUADS` in a 64x16 viewport. Mesa's vbuf callback
decomposes the quads into four triangles. The bridge clears one GPU target,
submits each triangle through the new preserve-target operation, waits on each
fence, performs one final GPU readback, and returns success so softpipe skips
that draw. Readback reports 156 green pixels, 156 red pixels, and 712 black
background pixels. The same run passes the fixed GR triangle test, XP import
audit, 2D checks, and dual-head verification; firmware assertions remain at the
startup count of one and no MMU fault is logged.

This validates more than the one-triangle probe but does not meet the full
Phase 2C goal. The 100-frame cube remains on softpipe because depth testing is
enabled. The external bridge now tiles aligned viewports whose dimensions are
multiples of 64x16; a 576x16 probe spans nine targets. It still handles only a
bounded batch of flat-color triangles with a kernel-owned shader, and there is
no GPU presentation path for general surfaces. Continue expanding renderer,
state, and resource support without exposing user pushbuffers or shader
programs.


## Latest hardware verification

Run `test_logs/v3.0.0.1-20261110-043538.log` passes a 576x16 WGL draw
spanning nine 64x16 GPU tiles. Readback verifies 6,384 green pixels across all
nine tiles, 528 red pixels in the last tile, and 2,304 black background pixels.
The cube batch also submits to the GPU, and the following miniport fixed and
client-triangle checks pass. The kernel invalidates C797 texture data after
updating its shared one-texel shader resource, preventing a stale red texel in
later draws. The suite still reports
`[TEST_COMPLETE] Status: SOFTWARE_3D_ONLY`: general depth-tested rendering uses
softpipe. XP import checks pass, firmware assertion count remains at its startup
value of one, no MMU fault is logged, and `mesa/` remains at its pinned clean
revision.

The follow-up full hardware run `test_logs/v3.0.0.1-20261110-044114.log`
reproduces that result from the current worktree. The nine-tile WGL draw passes
with 6,384 green pixels, 528 red pixels and 2,304 background pixels; the cube
probe submits one GPU batch; fixed/client-triangle and semaphore checks pass;
and both display heads plus 2D checks remain healthy. XP x64/x86 package import
and subsystem audits pass. Firmware assertions remain at the startup count of
one and there is no MMU fault. The bridge trace explicitly rejects the normal
depth-tested cube (`reject: depth test`), and the run ends in
`SOFTWARE_3D_ONLY`. This confirms the next renderer milestone is a validated
depth target and depth-test state in the kernel-built C797 submission, followed
by Mesa bridge integration and readback verification. The current 64x16 color
target and scanout copy path still require CPU readback/presentation.

## Depth target investigation (November 10, 2026)

The full hardware run `test_logs/v3.0.0.1-20261110-062947.log` rebuilds Mesa
27.0.0-devel with the external XP patches in a clean XP x64 build directory.
The build sets `min-windows-version=5`, and the resulting package passes the
XP x64 import/subsystem audit. The Mesa submodule is clean after the build.

The Mesa two-quad path passes again: GPU readback verifies 6,384 green pixels,
528 red pixels, and 2,304 background pixels. The fixed C797 triangle, 2D, and
dual-head display checks also pass. Depth readback through the miniport's CE
path has zero mismatches, but a C797 depth-tested draw still raises
`3D-Z KIND Violation` and ends with `[TEST_COMPLETE] Status: FAIL`. Firmware
assertions rise from the startup value of one to three; no MMU fault is logged.

The investigation changed depth pages from a second 4 KB alias to their
existing 64 KB PTE, then tested both C797 `X8Z24`/PTE-kind 3 and `Z24S8`/kind 5
pairs. Both still fail on the zeta access. A diagnostic with depth testing and
writes disabled raises no kind exception; enabling depth comparison with
writes disabled reproduces it. The remaining failure is in the zeta read/
compare path, not ordinary color rasterization or CE access. Full
depth-tested rendering and GPU-backed cube presentation remain incomplete.

Follow-up PTE experiment `test_logs/v3.0.0.1-20261110-063441.log` was
previously interpreted as adding NVIDIA UVM's `ATOMIC_DISABLE` attribute.
That PTE (`0x00000038002E4001`) instead sets bit 35, which VER2 assigns to the
video peer-ID field; `ATOMIC_DISABLE` is bit 7. The actual bit-7 and zero-peer
encoding is tested in `test_logs/v3.0.0.1-20261110-081227.log` and still
raises the same zeta exception.

Depth address-encoding experiment `test_logs/v3.0.0.1-20261110-064222.log`
used `offset >> 8` only for the zeta leaf, based on NVIDIA UVM's VER2 video
address construction. The run still raised `3D-Z KIND Violation` on multiple
GPCs, timed out the depth fence, and ended `FAIL`; the Mesa depth bridge did
not copy pixels. The nine-tile color readback remained correct
(6,384 green, 528 red, 2,304 background), and 2D/display verification passed.
The experiment is reverted because it encoded only the depth mapping in a
different address convention from the existing GPU VRAM leaves. UVM also uses
generic-memory PTE kind for generic mappings, while this path sets a zeta kind;
that difference is documented for investigation, not yet established as the
cause. Do not treat this run as depth or general OpenGL support.

The zeta pitch experiment `test_logs/v3.0.0.1-20261110-064631.log` changes the
64x16 32-bit depth pitch from 1024 to 256 bytes, matching Nouveau's
`nvc0_miptree` row-pitch calculation. Hardware still reports the same zeta
kind violation on multiple GPCs and the depth fence times out; Mesa depth
readback fails. The nine-tile color result and 2D/display checks pass. The
pitch change is not retained as a fix. Nouveau's framebuffer setup confirms
that C797 `SET_ZT_SELECT` aliases `ZETA_ENABLE`, and the current two-GOB
vertical tile mode (`0x10`) matches Nouveau's helper for this 64x16 target.
Next compare the actual VRAM PTE address/kind against the driver's physical
allocation and match the selected zeta format to that exact kind.

Follow-up pitch test `test_logs/v3.0.0.1-20261110-064631.log` set the 64x16
depth array pitch to 256 bytes based on Nouveau's small-surface row-pitch
calculation. The same multi-GPC `3D-Z KIND Violation` and depth fence timeout
remain; the pitch edit has been reverted. Color, 2D, and display checks pass.

Follow-up matched-format test `test_logs/v3.0.0.1-20261110-064946.log` rebuilt
the XP x64 Mesa ICD and paired `PIPE_FORMAT_Z24X8_UNORM` with C797
`Z24S8`/PTE kind 5, as Nouveau's format-kind table indicates. This still
raises the same zeta kind violation and fails depth readback; color, 2D, and
display checks pass. The temporary ABI format selector has been removed.
The format/kind mismatch was real but is not sufficient to explain the fault.

Hardware run `test_logs/v3.0.0.1-20261110-070254.log` adds a strict Mesa
bridge audit and a WGL-screen startup trace marker. The system WGL context
reports `Mesa` / `softpipe`, but the bridge trace file is absent, so the
expected 6,384 green, 528 red, and 2,304 background pixels do not prove GPU
rendering. Strict color, depth, cube, and overall OpenGL checks now fail; the
depth diagnostic still raises `3D-Z KIND Violation` on multiple GPCs. The
ICD DLL contains the bridge symbols and passes the XP import/subsystem audit,
but the WGL screen callback did not write its startup marker. The runtime
problem is therefore earlier than bridge format selection: establish that XP
WGL actually initializes the patched Gallium screen and reaches the softpipe
vbuf callback before further claims about Mesa-to-GPU rendering.

The depth PTE kind field was moved from bits 39:36 to the documented VER2 PTE
KIND bits 63:56, matching the NVIDIA header and the driver's own MMU decoder.
The resulting PTE is logged as `0x03000008002E4001`; this correct encoding did
not remove the hardware zeta violation. Keep investigating the zeta mapping
and surface state independently of the uninitialized Mesa bridge.


### Depth and viewport continuation (November 10, 2026)

The corrected bridge XY input preserves the nine-tile color result. The
Z24X8 allocation maps to C797 X8Z24 and PTE kind 3, but
`test_logs/v3.0.0.1-20261110-102041.log` still raises a zeta kind violation
and times out, with firmware assertions rising from one to three. The ZF32
diagnostic uses generic-memory PTE kind 6 and avoids that exception
(`102438.log`); red rasterizes, but depth readback reports zero over the
covered area and the nearer green triangle fails LESS. Follow-up
`103146.log` reproduces this with viewport scaling disabled, without new
firmware assertions or MMU faults. Viewport-enabled identity and NDC
experiments lose color coverage and have been reverted. Phase 2C remains
incomplete: clip-Z output, depth storage/readback, general shader/state
translation, and GPU presentation still need hardware proof.


Cube evidence clarification: the bounded 64x16 cube probe with depth testing
disabled reports `MESA_GPU_CUBE_RESULT PASS`, 46 colored pixels, and one new
GPU batch in `test_logs/v3.0.0.1-20261110-103146.log`. A successful bridge
batch is fenced and copied back into softpipe, proving that at least part of
this restricted cube workload reaches C797. The full-size depth-tested cube
and general OpenGL workload remain unproven; the suite still fails its depth
check.

### Latest color and cube result, depth still blocked (November 10, 2026)

`test_logs/v3.0.0.1-20261110-105101.log` demonstrates working viewport
transformation for the restricted color batches. After allowing signed X/Y
vertex inputs within +/-1/8 in the miniport, the nine-tile readback passes
(6,384 green, 528 red, 2,304 background); the bounded cube probe passes with
one new GPU batch; and fenced GPU readback is copied into Mesa's tile. The
full-size depth-tested path still fails: all three intermediate samples remain
red, ZF32 values read as zero over covered pixels, and `[OPENGL_RESULT]` is
FAIL. Firmware assertions remain at the startup count of one and the log has
no MMU fault. Next isolate clip-Z output and ZF32 write/readback, then continue
general Mesa shader/state translation and GPU presentation.
