# Phase 2C: Mesa and XP x64 ICD evaluation

Status: Mesa is pinned as a submodule; its software WGL DLL builds for the
default Windows target. The XP-targeted build is blocked by newer Win32 APIs
and imports.

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

The successful software WGL build is reproducible with Meson, Ninja, and the
x64 MinGW toolchain using `test/mesa_win64.ini`:

```sh
meson setup /tmp/mesa-win64-build mesa --cross-file test/mesa_win64.ini \
  -Dplatforms=windows -Dgallium-drivers=softpipe -Dvulkan-drivers= \
  -Dllvm=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled \
  -Dopengl=true -Dgallium-wgl-dll-name=nvxogl64
ninja -C /tmp/mesa-win64-build
```

The XP API-guarded attempt uses `test/mesa_xp64.ini` with the same options and
fails in Mesa's Win32 threading implementation as described below.

A Windows x64 Meson cross configuration was attempted with MinGW GCC and an XP
target (`_WIN32_WINNT=0x0502`). The host has the MinGW C++ compiler; no source
or generated build files are stored in the project tree beyond the pinned Mesa
submodule.

The successful configuration selected Windows, OpenGL, and `softpipe` only.
The initial `llvmpipe` configuration correctly failed because that driver
requires LLVM. The Windows `softpipe` configuration generated 114 targets and
downloaded its zlib fallback, but compiling stopped in Mesa's
`src/c11/impl/threads_win32.c`: `InitOnceExecuteOnce`,
`InitializeConditionVariable`, `WakeConditionVariable`, and
`SleepConditionVariableCS` are undeclared with XP API guards. Mesa's
`meson.build` also unconditionally defines `_WIN32_WINNT` and `WINVER` as
`0x0A00` for Windows builds. Overriding these to XP exposes the unavailable
APIs, so this is a real compatibility port requirement, not just a missing
cross-compiler setting.

This build targets Mesa `softpipe`, so it validates only software WGL/ICD
packaging. A useful hardware build needs an XP-facing Gallium pipe driver or
Vulkan implementation that translates Mesa's rendering work to the validated
miniport operations and is tested on the guest.

With Mesa's default Windows API target and MinGW's POSIX C++ toolchain, all 962
targets build, including `nvxogl64.dll`. Its exports include the WGL ICD
context and callback methods `DrvCreateContext`, `DrvSetContext`,
`DrvGetProcAddress`, and `DrvSetCallbackProcs`. This verifies Windows x64 WGL
DLL packaging only. The DLL imports `api-ms-win-core-synch-l1-2-0.dll` (including
`WaitOnAddress` and condition-variable APIs), `libwinpthread-1.dll`, and
`libz-1.dll`, so it is not XP-loadable as built. A Windows subsystem version of
5.2 does not make those imports available on XP.

## Next implementation gates

1. Port Mesa's Windows API and runtime dependencies to XP-compatible
   primitives, then audit the complete import set.
2. Select and implement an XP-facing Gallium backend. The WGL build currently
   uses `softpipe`; NVK/Nouveau require Linux Vulkan/DRM and do not plug in.
3. Define a fixed-width XP ICD-to-miniport bridge for contexts, surfaces,
   allocations, draw submission, and fences. Extend kernel submission beyond
   the fixed clear operation before connecting Mesa-generated draws.
4. Add an ICD only after it can create a context and submit at least one actual
   OpenGL draw through that interface. Do not register a software-only DLL as
   hardware accelerated.

The Phase 2B hardware evidence is
`test_logs/v3.0.0.1-20261109-195815.log`: triangle rasterization read back 960
black pixels against 64 white clear pixels, the semaphore completed, and the
display/2D checks passed. The guest still reports software OpenGL. The log
records one startup firmware assertion; no new assertion or MMU fault was
reported for the verified draw.
