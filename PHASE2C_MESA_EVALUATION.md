# Phase 2C: Mesa and XP x64 ICD

Status: Mesa is pinned and the XP x64 software WGL ICD builds and loads. The
hardware accelerated Mesa backend and ICD-to-miniport draw bridge are not
implemented, so Phase 2C is still in progress.

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
present callbacks. The submodule can be returned to its pristine pinned state
with `git -C mesa restore --worktree --staged .` after building. Keep source
customizations in `mesa_patches/` so updating Mesa stays reviewable.

The condition-variable and shared/exclusive-lock primitives have a repeatable
Wine smoke test at `test/test_mesa_xp_shims.sh`. It cross-compiles a Windows x64
test program and checks condition broadcast plus concurrent readers and
writers.

## Reproducible XP x64 software WGL build

With Meson, Ninja, the MinGW x64 toolchain, and the repository's configured
cross file:

```sh
./test/apply_mesa_xp_patches.sh
meson setup /tmp/mesa-xp64-build mesa --cross-file test/mesa_xp64.ini \
  -Dplatforms=windows -Dgallium-drivers=softpipe -Dvulkan-drivers= \
  -Dllvm=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled \
  -Dopengl=true -Dgallium-wgl-dll-name=nvxogl64 -Ddefault_library=static \
  -Dmin-windows-version=7
ninja -C /tmp/mesa-xp64-build
```

The generated DLL is
`/tmp/mesa-xp64-build/src/gallium/targets/wgl/nvxogl64.dll`. The MinGW
cross-build completed all 876 tasks. Its PE subsystem version is 5.2 and its
only imported DLLs are `GDI32.dll`, `KERNEL32.dll`, `msvcrt.dll`, and
`USER32.dll`; it has no `api-ms-win-*`, Vista condition-variable, or dynamic
winpthreads dependency. A small x64 Windows loader executable ran under Wine,
loaded the DLL, and resolved `DrvCreateContext`, `DrvSetContext`,
`DrvGetProcAddress`, `DrvSetCallbackProcs`, and `DrvSwapBuffers`.

This validates build, dependency audit, DLL loading, and export resolution. It
does not prove the DLL can create a working XP context or render on the GPU.
The selected Gallium driver is `softpipe`, which renders on the CPU. The
miniport ABI supports bounded allocations, a fixed clear operation, and fences;
it does not accept Mesa-generated shader programs or general draw streams.

Validation commands/results:

```sh
./test/test_mesa_xp_shims.sh
./test/test_mesa_wgl_load.sh /tmp/mesa-xp64-build2/src/gallium/targets/wgl/nvxogl64.dll
x86_64-w64-mingw32-objdump -p /tmp/mesa-xp64-build2/src/gallium/targets/wgl/nvxogl64.dll
```

Both smoke tests pass. The loader check resolves `DrvCreateContext`,
`DrvSetContext`, `DrvGetProcAddress`, `DrvSetCallbackProcs`, and
`DrvSwapBuffers`. The audited import list is limited to GDI32, KERNEL32,
msvcrt, and USER32.

## Hardware backend and remaining gates

Mesa's NVK/Nouveau drivers currently depend on Linux Vulkan/DRM interfaces and
do not provide a Windows Gallium backend. The available WGL frontend therefore
cannot reach the GA104 miniport. Completing hardware OpenGL requires:

1. An XP-compatible Gallium driver or Vulkan device layer that translates Mesa
   state into an NVX device interface.
2. A fixed-width ICD-to-miniport ABI for contexts, resource handles, submission,
   and fences, with kernel-side validation and command construction.
3. General safe GR submission beyond the fixed clear operation.
4. An XP guest test that verifies an NVK/GA104 renderer string, a real OpenGL
   draw, completed fences, rendered pixel readback, no new firmware assertions
   or MMU faults, and stable display/2D operation.

The Phase 2B baseline remains
`test_logs/v3.0.0.1-20261109-195815.log`: it verifies an in-kernel Ampere
triangle and pixel readback but reports software OpenGL. Do not register or
describe the current `softpipe` ICD as hardware accelerated.
