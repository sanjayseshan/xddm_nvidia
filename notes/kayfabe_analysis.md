# Kayfabe as an analysis reference for XP OpenGL bring-up

Reviewed November 11, 2026: <https://github.com/reindertpelsma/kayfabe/tree/master>

## What it is

Kayfabe is a QEMU virtual NVIDIA device for running NVIDIA's unmodified guest
driver. The QEMU device emulates the guest-facing NVIDIA PCI/GSP/RM contract;
GPU work is serviced on the host through its NVIDIA driver. Its current design
is Rust core plus a QEMU C device. Its documented architecture separates the
BAR0/register control plane, guest channel work, memory/VA management, host RM
operations, and display. It distinguishes passthrough, translated, and
emulated channel kinds and documents completion paths separately.

This differs from this project in the main dependency boundary. Our XP guest
owns a VFIO-passed GA104 and the XP miniport submits C797 work. The target is
Mesa's Gallium NVC0 OpenGL driver using that miniport through NVX. Kayfabe's
host NVIDIA driver and virtualized-device architecture do not provide an XP
miniport, an XP-compatible NVIDIA user driver, or an NVC0 Gallium winsys.
Current Kayfabe evidence describes Windows 11 with the stock 580.88 driver;
it does not establish XP support. The architectures therefore are not
drop-in alternatives and Kayfabe is not a path to reuse its GPU backend here.

## Using its emulated device as a pathway oracle

The emulated architecture can still help identify *candidate pathways*. A
Kayfabe run with a stock NVIDIA driver can show which PCI/GSP-facing
initialization, engine/channel classes, workload operations, and completion
events the modern driver exercises for a given application. Its recorded
gates and trace/census tooling can help turn a broad claim such as “3D works”
into smaller questions about initialization, channel birth, submission,
completion, and display. Those observations can guide which NVX capabilities
and hardware probes we should build next, and which milestones a successful
path must cross.

Use this as a source of hypotheses, not as a direct trace oracle for our Mesa
path. Kayfabe observes the stock NVIDIA driver's interface to its emulated
device; this project needs Mesa Nouveau's NVC0 state and command path through
our XP miniport. The two drivers can use different interfaces, command
streams, synchronization, and OS services. In particular, Kayfabe does not
show that a modern stock NVIDIA driver can load on XP, and its traces do not
substitute for observing Mesa/NVC0 commands on the GA104. Where practical,
compare equivalent *workload gates* (device init, context/channel creation,
resource setup, draw, fence, pixels), not assume byte-for-byte protocol
equivalence.

## Stage matrix: Kayfabe concepts mapped to XP/NVC0

The stage names below are comparison aids, not assertions that Kayfabe and
Nouveau use the same protocol. “Covered” means there is evidence for the
listed XP operation only; it does not imply the whole stage is production
ready.

| Stage | Kayfabe reference concept | XP/NVC0 evidence now | Status | Next XP gate |
|---|---|---|---|---|
| Adapter discovery | Emulated NVIDIA PCI/GSP device and synthesized facts | HDC-backed NVX caps identify RTX 3070 (`2488`, GA104 `0x170`), in `v3.0.0.2-20261111-114618.log` | Covered for identity; no Nouveau screen | Host test rejects unsupported identity/capabilities; hardware check that HDC/device lifetime is correct |
| 3D channel/context | Guest channel birth and channel-group ownership | Miniport C56F scheduling / C797 probes pass; no Gallium Nouveau context exists | Partial, below Gallium | Add an NVX-owned context create/destroy API and host lifecycle/fault tests; then create/destroy a Gallium NVC0 context without drawing |
| Resource creation and mapping | Kayfabe's one vidmem store and mirrored host VA mappings | NVX opaque BO allocation, bounded CE upload/download, CPU shadow map, and fence-aware lifetime; mock-tested | Partial staging only; no NVC0 resource manager, placement, or surface mapping | Test multiple BOs, bounds, ownership, map modes, delayed completion, and destroy-on-in-flight; then integrate BOs into NVC0 screen initialization |
| Command submission | Kayfabe separates passthrough/translated/emulated channels | XP accepts semantic clear/draw operations and one exactly validated NVC0 clear packet, then builds trusted C797 work | No generic Mesa batch submission | Implement a bounded NVC0 batch ABI with client-owned resources, relocation records, method allowlist, kernel-side validation/patching, and host parser tests |
| Progress and completion | GPU writes completion or host event becomes guest interrupt | XP fences pass on bounded draws; historical WGL bridge runs show GP_GET/GP_PUT stalls | Partial and intermittent | Batch-ID telemetry: enqueue, GP_GET advance, GPU fence, timeout/fault classification; mock ordering tests plus repeated hardware stress |
| Pixel result | Kayfabe gates compare workload correctness and host/guest outcomes | Standalone clear/triangle probes pass; latest bridge no-depth cube matches, depth cube differs | Narrow probes pass; OpenGL path fails | Native NVC0 shader triangle with deterministic readback; then depth, texture, blend, and cube tests |
| Presentation | Kayfabe has a separate emulated display plane and broker | XP WGL path currently presents softpipe framebuffer and performs bridge readback | No native Nouveau GPU-owned presentation | Add validated present/scanout operation and compare rendered target to XP window output without per-draw CPU readback |

This matrix identifies the missing path more precisely than “implement
Kayfabe on XP”: device facts and some low-level probes already exist, while
context/resource ownership, general validated submission, Gallium screen
creation, and native presentation remain the major work.

## Useful lessons for this project

1. **Keep service boundaries explicit.** Kayfabe documents device discovery,
   channel lifecycle, memory mapping, submission, completion, and display as
   distinct responsibilities. Use the same decomposition when specifying the
   XP NVX-backed Gallium winsys. The current NVX device and BO adapters cover
   only discovery and a limited staging-transfer primitive.
2. **Track evidence per layer.** A device identity result is not screen
   creation; a passing channel probe is not a Gallium draw; an individual
   successful draw is not a stable workload. Record each gate independently
   and tie it to the exact source revision and hardware log.
3. **Treat synchronization as part of the design.** Kayfabe's documented
   USER-ring relay work distinguishes doorbell, scan, synchronization witness,
   and completion behavior. Our intermittent C797 stall likewise needs
   per-batch submission/consumption/fence evidence. Keep the debugging probes
   bounded and isolate whether a stall occurs before consumption, during GPU
   execution, or at completion.
4. **Measure end-to-end correctness and latency.** The cube's pixel comparison,
   frame completion, and per-frame stalls should remain separate measures.
   For a real Gallium path, add a small shader-backed pixel test and report
   submission, fence, readback/presentation, and total frame time separately.

## What not to copy

Kayfabe's channel and host-driver model is built around a QEMU device and an
NVIDIA host RM. Its passthrough channel mode is not a safe submission design
for this XP miniport. Continue to reject arbitrary user pushbuffers: native
NVC0 integration here needs owned resources, validated relocations and packet
methods, kernel-owned channel state, and fences. Do not copy Kayfabe's RM/GSP
emulation, Rust/QEMU integration, host-driver calls, or its platform-specific
channel forwarding into the XP ICD.

## Concrete testbed use

Use Kayfabe as an architecture, pathway, and test-design reference, not as a
dependency or runtime in the XP test loop. The actionable experiments for this
repo are:

- Define separate gates for NVX device discovery, Gallium screen/context
  creation, BO/resource lifetime, validated NVC0 batch submission, shader draw,
  and presentation.
- Make an operation matrix from Kayfabe's documented device/channel/workload
  stages, then map each item to one of: already covered by XP/NVX, needs a
  bounded hardware probe, irrelevant to Gallium NVC0, or unavailable because
  the guest OS/driver interface differs. Treat each mapping as a hypothesis
  until an XP hardware result confirms it.
- When Kayfabe's public traces expose a useful high-level sequence, record the
  observed stage and expected completion evidence, then design a separate
  Mesa/NVC0 probe for the equivalent behavior. Do not replay or blindly
  forward captured command bytes.
- For each submitted batch, log a stable batch ID, resource IDs, command-word
  and relocation counts, submit fence, observed GPU progress, completion result,
  and elapsed times. Keep logs bounded and avoid dumping sensitive buffers.
- Build a minimal native NVC0 clear then shader-triangle test with pixel
  comparison. Run it before the cube so failures localize to the winsys,
  compiler/state setup, depth state, or presentation.
- Preserve exact hardware logs and package/import audits for each gate; report
  intermittent failures and fallback separately from passes.

The first executable host-side testbench now lives in
`test/nvx_winsys_test.c`. It models the escape boundary, records event order,
and exercises multiple simultaneously live BOs through allocation,
upload/download, fence wait, mapping, reference release, and free. Each mock BO
has independent storage and size; tests verify distinct handles/addresses and
that writes to one allocation do not affect the other. Failure cases verify
that a failed fence wait prevents mapping and final destruction; event
assertions verify wait-before-download and wait-before-free. It runs as part of
`make -C src_driver check` under ASan/UBSan. This tests our current NVX/BO
contract only; it does not emulate a GPU, validate an NVC0 command stream, or
prove Gallium integration.

The rapid guest test in `test/test_gpu_accel.c` now covers the next useful
contract boundary. It checks accepted/rejected batch metadata and relocations,
rejects malformed NVC0 clear methods, invalid UNORM values, and target
addresses that do not match the owned BO. The exact 28-dword clear profile now
has a separate native escape: the miniport validates it, submits those NVC0
words on the managed C797 channel, and appends its own idle and semaphore
methods. The November 11 run
`../test_logs/v3.0.0.2-20261111-131418.log` passed direct GPU execution with
1024 exact pixels (`0xFF7F40FF`) and fence 2. Four translated color controls
also passed with exact pixels and fences 4/6/8/10. Malformed header, negative
UNORM component, mismatched target address, batch version/token, and relocation
range cases were rejected. The GR triangle/semaphore probe passed; startup
logged one firmware assertion and no MMU fault. This is a real but very narrow
GPU packet submission gate: arbitrary batches remain preflight-only, and it
does not prove Gallium integration or shader-backed OpenGL.

This follow-up adds per-stage log markers and rejects relocation records with
nonzero reserved fields, duplicate command words, or unknown access bits; host
ABI tests also reject zero access and nonzero reserved fields. The rapid run
passed resource allocation/release, batch preflight, native submission/fence/
pixel readback, and the four-color translated-clear matrix. The first QEMU
launch hit a KVM emulation error; auto-retry attempt 2 completed. The direct
clear profile was separately tried against a 632x453 Gallium target and
produced a 6,436-pixel reference mismatch, so it remains limited to the
64x16 test target. These tests follow Kayfabe's stage-based evidence model;
they do not reuse its command streams or claim a native Gallium screen.

Rapid mode now also runs the 1,024-byte owned-BO upload/download round trip
before packet tests. The subsequent run
`../test_logs/v3.0.0.2-20261111-131916.log` passed that resource-transfer
stage, all bounded-clear stages, and the GR triangle/semaphore gate. This
narrows the next missing resource proof to Nouveau-compatible mapping and
GPU-backed resource use; the transfer test still exercises the NVX staging
ABI rather than Gallium's `nouveau_bo_map` implementation.

The next testbench should exercise a narrow generated NVC0 triangle only after
the project implements a safe, bounded method/subchannel allowlist and a
submission path. Until then, do not treat batch preflight as GPU execution.

The next implementation remains an XP/NVX replacement for the legacy Gallium
Nouveau DRM winsys and WGL screen selection/presentation. Kayfabe can inform
the boundary and evidence discipline, but does not remove that porting work.

## Sources checked

- Kayfabe repository README: project status, intended model, and recorded
  Windows 11 / Linux evidence.
- `ARCHITECTURE.md`: QEMU/device planes, channel kinds, memory model, host RM
  boundary, and crate map.
- `docs/FAQ.md`: comparison with API forwarding, Mesa/NVK, and the Windows
  private NVIDIA driver interface.

These are project-authored descriptions and measurements, not an independent
review of Kayfabe's implementation or security properties.
