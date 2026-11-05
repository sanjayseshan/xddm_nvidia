# Driver project context

Updated November 4, 2026. Workspace: `/home/sanjay/temp/xddm`.

## Objective and current user report

Build a full Windows XP x64 NVIDIA GA104 driver, progressing from reliable
native display output through complete 2D acceleration and eventually 3D.
The immediate task is restoring the second monitor on native XP.

The user confirms the current primary desktop is correct at 1920×1080.
Both monitors appear as PnP devices (1080p primary and 2560×1440 secondary),
and Extend is selected, but the secondary is black or stays in standby.
Both monitors connect to the RTX 3070. Earlier v164/v169 VFIO VM tests
successfully drove both monitors.

## Workflow and preferences

- Commit each driver version using the globally configured Git author:
  Sanjay Seshan <sanjay.seshan@outlook.com>.
- Keep README.md and a single cumulative SUMMARY.txt; avoid per-version status documents.
- Native XP updates use `./update_xp_driver.sh`. It builds both binaries,
  installs them into an offline writable XP mount, and compares installed hashes.
- XP volume: `/media/sanjay/winxp1`; check its current mount options before
  installation. It was read-only at earlier inspections. Do not assume that
  it is writable or remount it without checking that XP was cleanly shut down.
- Use soft VM shutdown when possible. VFIO GPU launches may take several tries.
  Do not open a QCOW image already held by another QEMU process.
- No COM1 logging is requested: the physical machine has no serial connection.
  Buffer early kernel logs in resident memory until disk I/O is available.
- AI credits should name GPT and Gemini.
- ISO generation is generally unnecessary. Generated binaries, ZIPs, VM disks,
  reference checkouts and logs are ignored by Git.

## Files and build/test commands

- `src_driver/nvxmp.c`: VideoPort miniport, modes, DualView, IOCTLs and logging.
- `src_driver/nvx_gsp.c`: FWSEC/GSP/RPC, CE, native display and DP setup.
- `src_driver/nvx_display.h`: display protocol helpers and timing decoding.
- `src_disp/`: XDDM GDI display driver and diagnostics.
- `dist_full/nvx.inf`: package version/install configuration.
- `README.md`, `SUMMARY.txt`, `FIRMWARE_PROVENANCE.md`: project documentation.
- Native logs: `/media/sanjay/winxp1/WINDOWS/nvxmp.log` and `nvxdisp.log`.
- Other native diagnostics: `WINDOWS/setupapi.log` and
  `WINDOWS/system32/config/SysEvent.Evt` under the same mount.
- VM logs: `test_logs/`; `vm_serial.log` may still link to the old v169 run.

```sh
make -C src_driver full
make -C src_disp
make -C src_driver check
make -C src_disp check
./update_xp_driver.sh
# VM test path, when appropriate:
./test_full_driver.sh
```

Local reference source exists in ignored `open-gpu-kernel-modules/`,
`linux-drm/`, and `nvidia-macos-driver/`. Use source evidence to check register
and ABI assumptions. `rg` was unavailable; fall back to grep/find.

## Established history and limits

- v169: user-confirmed clean dual-monitor desktop in VM, GPU presentation
  and solid fills. Most GDI rasterization remains software. OpenGL 1.1
  reported by XP does not establish GPU 3D acceleration. No OpenGL ICD,
  Direct3D, Vulkan or video decoder implementation is established.
- v171 native: GSP starts, but CE probe timed out; a direct CPU framebuffer
  fallback accepted a surface while physical output stayed black.
- v172 removed that unverified fallback. A later mounted run with a v172
  banner included uncached allocation and expanded CE diagnostics, so do not
  equate banner alone with a particular committed source snapshot.
- Native DMA at 0x023C1000..0x053C1000 was below 4 GiB: Above-4G rejection
  did not explain that timeout. Do not advise disabling BIOS Above-4G solely
  on that theory.
- v173..v175 changed doorbells and cache/coherency attributes. Current native
  primary output works, so continue from current code rather than reverting
  blindly to the old VM environment.
- v176..v182 adjusted primary base, DualView enumeration, secondary activation,
  DP wake and full dual-head EVO state. Primary framebuffer is at VRAM offset 0;
  secondary at +32 MiB. v182's claim that secondary was resolved was premature.

## Latest actual evidence: v182 native log

The inspected log begins with a v182 banner. Display Core, Window0 and Window2
DMA NOPs were consumed. GSP and CE initialization succeed. The secondary DP
sink wakes and trains four lanes at HBR2 (link rate 0x14), but every ownership
commit reports `ownership updateNotifier=0`. The function returns at that
point, before submitting the full native head/window modeset.
RM reports Head 0 display 0x800 with link lock and Head 1 display 0.
The secondary GDI surface is nevertheless accepted at 2560×1440 with GPU VA
0x22000000; successful surface creation does not prove output activation.

## Current work: v183 candidate

At the start of this work Git HEAD was `1674fc5` (v182), clean. v183 edits:

- Fix RAMHT hash: NVIDIA `instmemHashFunc_v03_00` includes client ID bits;
  our context word included the client but our bucket hash omitted it.
  See `open-gpu-kernel-modules/src/nvidia/src/kernel/gpu/disp/inst_mem/arch/v03/disp_inst_mem_0300.c`.
  For handle 0xd1500001/channel 0/client 0xc1d00001, expected slot is 0x113,
  while the previous helper produced 0x117. Enforce 32-byte descriptor alignment.
- Request coherent display pushbuffer memory (`push.cache=0`, RM CACHED).
- Set notifier DMA target to coherent PCI (`0x47` rather than `0x46`).
  Register definitions are in `src/common/inc/swref/published/disp/v03_00/dev_disp.h`.
- On ownership/dual-update timeout, log Core/Window0/Window2 PUT, GET,
  exception status/data/code and supervisor state without clearing registers.
- Restore full four-lane link checks and reject zero secondary display IDs
  during verification.
- Update version/banner/INF to 183, update documentation and test vectors.

These are evidence-based candidate corrections, not a confirmed hardware fix.
The v183 miniport/display builds and both host test suites passed. Matching
binaries are staged in dist_full. The v183 changes are committed using the
configured global author. Install with the native update script only when the offline volume
is writable. On the next physical run, require ownership notifier completion,
full dual-head update completion and visible secondary output. If ownership
still times out, inspect the new EVO exception and supervisor snapshots to
distinguish object lookup failure, command fetch failure and modeset supervision.

This context file is a handoff snapshot; inspect Git status, current source,
mount state and newest logs before resuming because the workspace has advanced
through additional versions between conversation turns.


## v184 follow-up: comparison with the working VM

The user requested restoring behavior from the last known working VM version.
The decisive observed difference is ordering: v169 performs final BIOS 1080p
mode selection before native display channel creation; v177-v182 creates
channels in HwVidInitialize, then calls VBE SET_MODE afterward without clearing
NativeDisplayChannelsReady. This can invalidate the native channel/instance
state while the driver continues using its cached handles.

v184 removes early channel creation, enforces a primary surface mapping before
native initialization, and adds acknowledged RM display-channel teardown before
later BIOS resets. Failure to free a channel blocks BIOS reset and prevents
reinitialization over live objects. Restoring a previously active dual desktop
after a primary mode change is handled explicitly. The primary base stays zero;
v183 hash and coherence fixes remain. New host regressions cover this lifecycle.
The physical log inspected for this work is still v182, so hardware confirmation
is pending. Latest sources and staged binaries are v184; use update_xp_driver.sh
once the offline native volume is writable. Do not infer hardware success from
host mocks or treat the old VM log as a fresh native run.
