# Phase 2C: Mesa and XP x64 ICD evaluation

Status: feasibility pass; no Mesa source is vendored or linked into the driver.

## Mesa revision and Windows path

The upstream Mesa checkout used for this read-only evaluation is
`cc2dcf9abdc6afc093e85d48e4cfa34eed60a01e` (Mesa `27.0.0-devel`, shallow
checkout at `https://gitlab.freedesktop.org/mesa/mesa.git`). Its Windows build
has a Gallium WGL front end and a WGL DLL target:

- `src/gallium/frontends/wgl/stw_context.c` implements the Windows ICD context
  lifecycle, including `DrvCreateContext` and `DrvSetContext`.
- `src/gallium/frontends/wgl/gldrv.h` declares the ICD entry points and
  callback ABI.
- `src/gallium/targets/wgl/meson.build` builds the WGL DLL from the selected
  Gallium drivers.
- `src/gallium/meson.build` includes the WGL targets on Windows.

This is useful code for the WGL/ICD boundary. It does not provide the needed
Ampere backend: the Windows Gallium target is assembled from Windows-compatible
Gallium drivers, while Mesa NVK and Nouveau depend on Linux Vulkan/DRM
interfaces. Neither backend can currently submit through this repository's
miniport ABI. The ABI safely supports bounded allocations, a kernel-built
surface clear, and fences; it does not accept general Mesa-generated GPU
commands.

## Cross-build result

A Windows x64 Meson cross configuration was attempted with MinGW GCC and an XP
target (`_WIN32_WINNT=0x0502`). Meson stopped before dependency/configuration
checks because `x86_64-w64-mingw32-g++` is absent. The host has the C compiler,
but not the C++ cross compiler required by Mesa. Installing the missing system
package requires administrator credentials, so this environment could not
complete the build. No Mesa source or generated files were added to this repo.

The attempted configuration selected Windows, OpenGL, and `llvmpipe` only. Even
if it builds, it would validate Mesa's software WGL/ICD packaging, not RTX 3070
hardware acceleration. A useful hardware build needs an XP-facing Gallium pipe
driver or Vulkan implementation that translates Mesa's rendering work to the
validated miniport operations and is tested on the guest.

## Next implementation gates

1. Install `g++-mingw-w64-x86-64` and Mesa's Windows build dependencies; repeat
   the pinned WGL cross-build with XP API guards and record unsupported Windows
   APIs found by link and runtime checks.
2. Define an XP ICD-to-miniport bridge with context, surface, allocation,
   submission, and fence ownership. Keep it fixed-width and validate all
   user-provided sizes and handles.
3. Extend the kernel submission path from its current fixed clear operation to
   a bounded draw interface before connecting Mesa-generated work. The
   initialization-time triangle probe is diagnostic and is not a user-mode
   submission API.
4. Add an ICD only after it can create a context and submit at least one actual
   OpenGL draw through that interface. Do not register a software-only DLL as
   hardware accelerated.

The Phase 2B hardware evidence is
`test_logs/v3.0.0.1-20261109-195815.log`: triangle rasterization read back 960
black pixels against 64 white clear pixels, the semaphore completed, and the
display/2D checks passed. The guest still reports software OpenGL. The log
records one startup firmware assertion; no new assertion or MMU fault was
reported for the verified draw.
