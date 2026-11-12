# Phase 2C: Mesa OpenGL hardware acceleration on XP x64

Updated November 11, 2026.

## Status

Phase 2C remains incomplete. The XP x64 Mesa ICD builds, loads through system
WGL, creates contexts, and runs normal OpenGL applications. Its selected Gallium
renderer is `softpipe`, however, so normal application frames are CPU rendered.

The project-owned bridge in `../mesa_patches/nvx_wgl_bridge.c` intercepts a small
subset of softpipe post-vertex triangles and submits equivalent C797 operations
to the GA104. Bounded pixel, depth, cube, and batched-triangle probes verify
real GPU writes, fences, and readback. This is diagnostic acceleration; it does
not make the ordinary Mesa renderer hardware accelerated.

The bridge now routes eligible full-target color clears through the validated
NVC0-format clear escape. The `../test_logs/v3.0.0.2-20261111-101720.log` run
records 104 successful clears and 106 GPU batches; its no-depth full-size cube
matches the softpipe reference. The depth-enabled cube still fails comparison
(`reference_diff=11876`). This extends the existing prototype bridge and does
not change the fact that Mesa selects softpipe for general OpenGL.

The latest safe-path full RTX 3070 run is
`../test_logs/v3.0.0.1-20261110-145245.log`. XP import/subsystem audits,
pixel/depth/cube probes, the six-vertex C797 batch, 2D checks, and display
checks pass. The suite reports zero failures. Firmware assertion count remains
at its startup value of one, with no MMU or GR channel fault. The ordinary app
workload makes 103 bridge callbacks but adds no GPU batches; its 632x453
viewport exceeds the guarded 64-tile limit. It reports
`[OPENGL_RESULT] SOFTWARE`, and the suite ends `SOFTWARE_3D_ONLY`.

The one-large-draw experiment in
`../test_logs/v3.0.0.1-20261110-144802.log` is reverted. Its repeated synchronous
per-tile depth/color clears, draws, and readbacks grew firmware assertions from
one to five and faulted GR. Do not re-enable this path; implement reusable
targets and batched submission before application-sized draws.

The earlier XP startup errors are resolved. The ICD imports no unavailable
`GetTickCount64`, `qsort_s`, or Vista condition-variable entry points; the
application initializes and regular system WGL creates its context. The full
package import audit is in `../test/audit_xp_imports.py`; XP compatibility shims
and their Mesa patch remain outside the Mesa submodule in `../mesa_patches/`.

The latest full hardware run is
`../test_logs/v3.0.0.8-20261111-204936.log`. The Mesa WGL ICD built, passed the
XP import/subsystem audit, installed into the offline guest, and loaded during
the run. The Nouveau bridge found the GA104 device, but `NOUVEAU_CHANNEL_ALLOC`
is still unsupported, so `screens=0`, no NVC0 screen or draw was created, and
ordinary OpenGL reports `[OPENGL_RESULT] FAIL` / SOFTWARE. Separate NVX
triangle and native-clear diagnostics passed. This verifies package build/load
and the failure boundary, not accelerated OpenGL.

After making HDC setup idempotent and adding client/resource teardown before
adapter rebinding, rebuilt and reran the full package at
`../test_logs/v3.0.0.8-20261111-210029.log`. The XP package/import checks and
hardware 3D tests still pass. Ordinary OpenGL still fails at
`NOUVEAU_CHANNEL_ALLOC`; `screens=0`, confirming the lifecycle fix did not
create a native screen or claim acceleration.

## Existing driver assessment

Mesa's NVC0 Gallium driver contains the relevant C797 command construction,
Ampere shader compiler, vertex formats, and block-linear surface calculations.
The pinned upstream WGL target does not connect that driver: it creates a GDI
software winsys and exposes softpipe, with optional D3D12 and Zink paths. The
Nouveau hardware driver expects Linux DRM/Nouveau device, memory, channel, and
fence interfaces. An XP MinGW Meson configuration with
`-Dgallium-drivers=softpipe,nouveau` succeeds, and its NVC0/Nouveau shader
compiler static library cross-compiles (27 C++ objects). The full Nouveau
Gallium library does not: its device/winsys sources require POSIX memory
mapping and `xf86drm.h`. Enabling `gallium-drivers=nouveau` alone cannot
provide an XP WGL hardware renderer.

The missing layer is an XP Windows Gallium winsys/resource implementation that
maps Gallium resources and contexts onto this project's miniport ABI. It must
then wire NVC0 to the new winsys, implement buffer and render/depth surface
lifetimes, submit NVC0-generated state and shaders, synchronize fences, and
present GPU-rendered targets. The current miniport ABI only supports bounded
diagnostic operations on a fixed 64x16 target, so those interfaces also need to
grow with validated pitch, block height, format, and target bounds. The Mesa
submodule must remain pristine; project-specific integration belongs in
`../mesa_patches/`.

The compiler-only XP cross-build used an external `sys/ioccom.h` shim at
`../mesa_patches/nouveau-xp-include/`; it only supplies DRM UAPI ioctl encodings
needed to compile headers and does not implement DRM calls or a GPU winsys.
Do not add the Nouveau DRM screens or advertise the driver as usable on XP on
the basis of this compile result. Treat Nouveau as a read-only reference;
project-owned XP winsys and Gallium work belongs in `../mesa_patches/`.

## Mesa pin and XP build

The `../mesa/` submodule is pinned at
`cc2dcf9abdc6afc093e85d48e4cfa34eed60a01e` (`27.0.0-devel`). Apply the XP
compatibility patch with:

```sh
./../test/apply_mesa_xp_patches.sh
```

Build the XP x64 WGL ICD with Meson, Ninja, and MinGW:

```sh
MESON=/path/to/meson NVX_MESA_BUILD_DIR=/tmp/mesa-xp64-build \
  ./../test/build_mesa_xp64.sh
```

The build selects `-Dgallium-drivers=nouveau,softpipe` and produces
`nvxogl64.dll` for XP subsystem 5.2. WGL attempts Nouveau first, but channel
allocation currently prevents screen creation; softpipe remains the ordinary
renderer. `../test/audit_xp_imports.py` checks the package against the guest's
actual exports. `../test/test_mesa_xp_shims.sh` and
`../test/test_mesa_wgl_load.sh` cover the XP shims and ICD load/export path.
When the Meson version differs from the one that created the `/tmp` build
directory, the build helper wipes and reconfigures that directory. Its
generated cross file is stored alongside the build directory so a wipe does
not remove it.

## Next implementation steps

1. Implement a real RM-owned Nouveau channel lifecycle behind
   `DRM_NOUVEAU_CHANNEL_ALLOC`, including its mapped pushbuffer, engine objects,
   and fence BO. Do not report a screen until the channel can submit and retire
   validated work.
2. Extend the miniport ABI from fixed diagnostic profiles to bounded NVC0
   batches with full method/subchannel/class validation, BO residency and
   access validation, kernel-owned command staging, and fence completion.
3. Implement remaining XP resource, context, mapping, and synchronization
   operations needed by Gallium, then connect the actual NVC0 screen and WGL
   presentation through external patches.
4. Bring up shader/state submission and normal triangle draws, then add
   textures, depth, blending, and full application viewport presentation.
5. Verify Antigravity's ordinary renderer reports hardware and its animation
   runs without new firmware assertions, MMU faults, or display/2D regressions.

Full hardware validation command:

```sh
PATH=/tmp/nvx-tools-venv/bin:$PATH NVX_AUTO_RETRY=1 \
NVX_BUILD_MESA_ICD=1 NVX_MESA_GALLIUM_BRIDGE=1 \
NVX_GR_CHANNEL_PROBE=1 NVX_GR_PROMOTE_PROBE=1 \
NVX_GR_SUBMISSION_PROBE=1 ./../test/test_full_driver.sh
```
