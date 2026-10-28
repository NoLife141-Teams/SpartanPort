# GoW3 DLAA input prototype (experimental branch only)

Current integration state: [GoW3 integration review](gow3-integration-status.md).
The sections below also retain the history of the individual experiments.

This is **input research, not working DLAA**. Nothing is enabled in the launcher,
no NGX evaluation is added, no frame generation is added, and the normal rendering
path is unchanged when the diagnostic flag is unset. Existing temporal settings
remain disabled. Do not enable legacy Bloodborne-style camera extraction for GoW3.

## What is implemented

`SPARTAN_GOW3_INPUT_DIAGNOSTICS=1` opts into read-only CPU diagnostics in the
rasterizer. It matches the documented GoW3 depth-prepass vertex shader
`aed40011eb5259f3`, resource slot 1, exactly 144 bytes, depth-only draws with depth
writes enabled, and D32S8 depth. It reads the **constant-ring snapshot**, not the
original guest address. Unsupported non-ring bindings are counted as
`unsnapshotted` and invalidate the candidate frame; they are never dereferenced.
Other hashes/layouts are ignored rather than guessed.

The first 16 floats are interpreted as column-major view-projection. All 36 floats
must be finite, the VP must be invertible, and matching draws within a frame must
agree exactly on VP, depth target and dimensions. An ordered marker closes the
camera interval once per accepted flip, independently of the trace and legacy
upscaler enable state. Single-target display-pass transitions only describe
diagnostic groups; several may belong to one flip. Missing/invalid/conflicting frames discard continuity;
resolution changes warm up again. The observed dimensions are the **depth allocation**,
not yet a proven active viewport (alignment/overscan remains to validate).

`GoW3 inputs:` logs startup, status changes, every 120 accepted camera intervals and
active frame captures. Statuses: `missing`, `invalid`, `conflict`, `warming`,
`camera-candidate`. The latter means only two consecutive structurally consistent
camera samples. It does **not** validate scene identity, cuts, jitter, moving objects,
exposure, pre-HUD color, or readiness for NGX. Existing frame captures provide the
raw constants/pass resources needed to check that hypothesis. No new file dumps
are created by this flag.

`gow3_camera_inputs.h` also contains a CPU reference for inverse-VP reprojection
with D3D [0,1] depth and previous-minus-current pixel motion. It handles general
4x4 VP (not the legacy affine view/projection layout), rejects non-finite,
background, behind-camera and offscreen samples. It is **not dispatched on GPU**.

## Local synthetic tests (no game or GPU)

```sh
mkdir -p out/dlaa-tests
g++ -std=c++23 -O2 -UNDEBUG -Wall -Wextra -Werror -I. \
  tests/test_gow3_camera_inputs.cpp -o out/dlaa-tests/gow3-camera-inputs-test
out/dlaa-tests/gow3-camera-inputs-test
```

The same test is available as CMake target `gow3-camera-inputs-test` in a fully
configured build. Existing `motion-history-test`, `ui-composition-test`, and
`upscaler-support-test` are useful non-GPU regression checks. The camera-motion,
motion-shader and scene-resolution Vulkan tests exercise **legacy** paths; a pass
there does not prove GoW3 native DLAA.

## Isolated Windows/NVIDIA validation protocol

1. Use a separate checkout/build and separate runtime copy. Do not modify the
   original installation, saves or caches. Follow `packaging/windows/README.md`
   using existing toolchains/dependencies; review any installation separately.
2. Build the normal GPU/loader targets plus the new CPU target. Keep upscalers off.
   Run with `SPARTAN_GOW3_INPUT_DIAGNOSTICS=1` in this process only. Constant-ring
   snapshots require the existing asynchronous draw pipeline. If diagnostics report
   `unsnapshotted`, stop input validation; do not bypass the guard.
3. Use the existing frame-capture facility for stationary scenery, camera movement,
   a scene/camera cut, pause/menu, and a resolution change. Keep captures local and
   private (they may contain game data); never commit captures or game files.
4. Check exact shader hash and resource-slot mapping, D32S8 depth association,
   stable VP across draws, consecutive-frame matrices, and active viewport versus
   allocation dimensions. Compare reference reprojection on static geometry,
   including motion direction, depth convention, far geometry and disocclusions.
5. Stop on missing/conflicting candidates, stale history, unmatched layouts or
   visual mismatch. A native DLAA dispatch remains blocked pending a reviewed
   GPU implementation plus validated object motion, jitter/reset behavior,
   pre-HUD scene color/composition and NGX resource lifetimes. Hardware feature
   availability by itself is not sufficient evidence.

## Validation limits

### Local bounded frame trace (off by default)

The CPU target `gow3-frame-trace-test` tests diagnostic accounting independently
of Vulkan and game data. Optional runtime tracing requires all three process
variables: `SPARTAN_GOW3_INPUT_DIAGNOSTICS=1`, `SPARTAN_GOW3_FRAME_TRACE=1`, and
`SPARTAN_GOW3_FRAME_TRACE_FILE` naming a **new absolute file in the isolated
session**. Its parent directory must already exist. Existing files are never
overwritten. Missing flags or an unusable destination leave tracing off.

The CSV retains at most 16,384 records (each under 1 KiB, plus a fixed header),
then counts dropped records without expanding the file. The header records
`retained`, `dropped`, and `io_errors`, refreshed at flip/presentation boundaries.
Any loss, I/O error, `unordered` marker, or unmatched flip ID makes a completeness
claim invalid. Initial and unfinished intervals are not complete observations.
File logging can perturb timing; this is not a performance benchmark.

`interval` is owned by the ordered draw consumer. A display transition uses that
interval ID; its terminating `interval` record maps it to an explicit `flip` token.
That token is copied into the VideoOut request and swap-thread closure, never
inferred from a global latest flip. EOP callbacks already execute in draw order;
non-EOP requests enqueue a camera-completion marker on the existing draw queue.
CSV order across threads is not necessarily lifecycle order: join by token.
Reused presentations carry the source token of `last_submit_frame`, not a new
frame. Minimized, acquire-failed and present-failed outcomes remain distinct.

`draws` and `shader` count graphics attempts reaching render-state preparation,
not completed GPU draws. `eligible` additionally passes the depth-draw predicate;
`snapshots`/`unsnapshotted` count the exact shader/slot/size/depth-format inputs
already inspected by the existing diagnostic. No extra guest memory is read.
Status uses Missing=0, Invalid=1, Conflict=2, Warming=3, Candidate=4. In CSV v2,
`display` records are diagnostic groups, never camera closures: their sample and
missing counts are deltas since the previous display group or accepted flip,
and their status is the previously completed camera status. Depth/dimensions
describe the display draw's attachment, not the earlier camera pass. The
`interval` record contains the completed camera status and total samples/missing
snapshots. Its dimensions are the diagnostic camera dimensions (no fresh valid
dimensions are implied by a missing/invalid status); its depth field is unset.

Interpret only complete, lossless intervals. CSV v2 `empty-display-with-camera`
and `multiple-display-groups` describe display transitions; they do not imply
extra history closures. `shader-absent`, `filtered`, `no-binding`, and
`snapshot-missing` separate other causes. Absence of the expected shader does
not identify a menu: no trustworthy native GoW3 pause flag is used. Retained v1
traces keep their original closure semantics and must not be parsed as v2.

### Local accepted-flip camera boundary correction

The retained v1 trace demonstrated multiple display groups (including empty
ones) within one accepted and presented flip. Camera history now completes once
at that accepted request's ordered marker, with `SPARTAN_GOW3_INPUT_DIAGNOSTICS=1`,
independently of the trace flag, file availability or journal capacity. Display
transitions only produce diagnostics. Queue rejection returns before token
creation; presentation, delayed presentation and redraw do not call completion.
A new accepted request for the same display buffer is still a new interval.

EOP callbacks on stage B complete directly in draw order. Non-EOP requests reach
stage A via `SendCommand`, then copy their identity into `RunInOrder`. Serial
stage A uses that same function's drain-before-execution fallback. Unexpected
threads report an unordered marker without touching camera state. Request IDs
are atomic identities, not an assumed ordering of A/B execution. No new threads,
guest-memory reads, queue synchronization or rendering decisions are introduced.

`gow3-camera-frames-test` covers 2..5 display groups per accepted flip, both
request paths, serial/queued ordering models, trace off/full, queue rejection,
shader absence, missing snapshots, matrix/depth conflicts, buffer changes and
delayed/reused presentation. Synthetic tests use production state/route helpers;
they do not exercise real GPU scheduling. Missing snapshots and conflicts still
invalidate the entire interval. This correction is not proof of projection,
scene identity, cuts, viewport, depth reuse, object motion or NGX compatibility.
DLAA, jitter and legacy camera-motion dispatch remain disabled for collection.

### Snapshot rejection provenance

The accepted-flip v2 session demonstrated the corrected grouping, but its
retained prefix ended before the later snapshot failures. Later stdout records
show missing snapshots; they cannot establish which producer guard rejected
the copy. Do not assume allocation pressure or bypass those guards.

With input diagnostics enabled, stage A now tags the existing reserved packet
word for the exact vertex shader/slot with its first existing rejection:
empty source/range, writable buffer, oversized range, GPU-modified region,
pending write, or failed ring allocation. It can also record ring disabled or
unavailable. Stage B consumes that packet's tag and separately identifies no
packet, missing stage/binding, or a producer copy absent from the consumer.
The packet size, copy decisions, short-circuit query order and synchronization
are unchanged. No extra guest memory or cache query is read for the diagnostic.

Per-camera-interval counts appear in the existing status log line and in CSV v3
`snapshot_reasons`; v1/v2 files retain their old schemas. Only inputs passing
the existing shader/slot/length/depth guards contribute. Any missing snapshot
still invalidates the interval. Counters reset at the accepted flip, never at
a display transition; no latest global reason can label another draw. The
journal retains its 16,384-record budget and loss/error counters. The existing
status log frequency is unchanged, so its counts are observations, not a
complete presentation ledger after trace saturation.

`gow3-snapshot-reasons-test` verifies strict flag parsing, rejection attribution,
no recovery from a missing snapshot within the same interval, exact-input
guards, reset, and bounded CSV formatting. Constant-ring publication additionally
has a 200,000-step delayed-GPU/limited-queue synthetic model. These tests cannot
identify the cause of already collected runtime failures or validate Vulkan
timing. The next user session only needs to reach the existing checkpoint and
move briefly to reproduce the missing-snapshot phase. Use the deferred trace
window below to preserve the useful segment; the reason-annotated status log
also remains available if the bounded trace fills.

The later deferred session `deferred-trace-20261027T113416Z` retained 3,275
complete gameplay intervals with 1,053,912 copied inputs and no snapshot
rejections. Its retained window lasted 54.666547 seconds and extended about
29 seconds beyond the supplied movement cue. Startup status logs, outside that
window, contained 143 invalid records and 62,617 `ring-space-unavailable`
observations. Status lines are sampled, so these are not full startup totals.
That reason identifies the failed allocation gate after Retire; it does not
identify whether GPU completion, producer/consumer lag or stamp publication
limited progress. No ring size or synchronization change follows from it.

### Deferred trace window (CSV v4, explicit opt-in)

Set the existing three trace variables and, additionally,
`SPARTAN_GOW3_FRAME_TRACE_TRIGGER` to a **session-local absolute file path**, for
example `<session>/triggers/trace.start`. Set the output to a distinct new
absolute file, for example `<session>/logs/gow3-frame-trace.csv`. Both parent
directories must already exist. Missing/disabled flags create no output; without
a trigger path, the earlier immediate trace mode remains available.

The session starts armed, with no journal file or consumed event budget. Once
the player has actually resumed the checkpoint, create the empty trigger file
once. It may also exist before startup: activation still waits for an ordered
accepted flip. A regular file is required; a directory/symlink is not a trigger.
Neither the trigger nor an existing output is removed or overwritten. The
journal uses exclusive creation at activation; an opening failure stops that
attempt without retries. Repeated triggers do not reopen/reset the journal.

On that ordered flip the journal atomically writes `start,incomplete` followed
by its `interval,incomplete`. The activation flip, interval ID, draw serial and
camera-frame number are retained. The camera accumulator and its acceptance
decisions are never reset. The activation interval is incomplete because its
earlier display/lifecycle records were outside the capture window. Later events
from old presentations retain their original tokens; do not associate them
with the latest flip or infer ordering from ID magnitude. Use only intervals
after the start with complete matching interval/queued/dequeued/present records.
Some requests straddling activation can also have missing lifecycle records.

Before activation, suppressed events are outside the window, not journal loss.
After activation, the same 16,384-record cap and exact loss/error counters apply,
including the start record. Concurrent writers cannot precede or interleave the
two activation records. File polling happens only at the existing ordered flip
callback; there is no new thread or rendering synchronization. The log prints
the activation flip/interval so the controller can confirm collection started.
No game is launched by creating the trigger.

For the next collection: launch the isolated probe armed, let the player load
the checkpoint without spending the journal budget, create `trace.start` after
the player confirms gameplay, then record a few stationary seconds and a short
movement. Monitor retained/dropped counts. No F11 or game-data upload is needed.
`gow3-trace-trigger-test` covers before/after-start triggers, repeated/disabled
activation, exclusive output creation, unchanged IDs/history, late events,
concurrent activation/writers, saturation and exact losses.

Rejection labels deliberately describe only producer observations:
`gpu-dirty-range` is a conservative dirty-range query, not proof that the exact
camera bytes were GPU-written; `queued-write-overlap` is queued recording work,
not a GPU-completion diagnosis; `ring-space-unavailable` means Allocate returned
no space after its existing retirement attempt, not VRAM exhaustion or a proved
stamp bug. `declared-writable` describes the resource declaration. `copied`
does not validate matrix contents. Unknown/no-context cases remain explicit.

Cloud baseline: standalone motion-history, UI-composition and upscaler-support
checks passed. The new synthetic GoW3 input/reprojection test passed strict C++23
compilation and ASan/UBSan (LeakSanitizer unavailable under container ptrace). Full `build.sh` stopped at its dependency preflight: this executor
has no CMake, Ninja, glslangValidator or Vulkan/SDL3 development pkg-config entries.
Consequently production rasterizer compilation, Vulkan shader execution,
Windows/NVIDIA NGX initialization and in-game quality/performance are not claimed.

### Offline reprojection and depth-sample contracts (27 October)

The CPU reference is a hypothesis: column-major VP, clip depth [0,1], stored
depth equal to clip z/w, top-left pixel centers, a full viewport of unchanged
extent, normal-Z clear at 1, and motion in render pixels, previous minus current.
Finite/invertible matrices and `camera-candidate` do not establish those facts.
The legacy `camera_motion.comp` still expects separate view/projection constants
and `texture2D`, not the new VP or a multisample texture. `RecordMotion` binds the
provided depth view directly; it does not perform a multisample resolve there.
Do not enable debug motion or connect the GoW3 VP to that shader.

Evidence from the existing local F11 captures at 02:27:36, 02:27:40 and 02:27:42
UTC: pass #7 contains shader `aed40011eb5259f3`, no color attachment, and a
1920x1080 D32SfloatS8Uint depth attachment with census `c0 d2 r2` and shading 0.
The census records a guest depth count of two and the pipeline sample key. It
does not record the actual host image sample count, individual sample positions,
depth values or a color/depth resolve association. No game data is in the tests.
The deferred CSV carries no matrix values, viewport or per-sample depth either.

`tests/gow3_projection_fixture.h` supplies invented world points and an independent
forward oracle: world-to-view rotation/translation, perspective and viewport
mapping, without calling the production inverse/transform/reprojection helpers.
Two standalone targets are excluded from the default build:

- `gow3-reprojection-contract-test`: 108 asymmetric forward-projected cases
  over three resolutions, four depths and translated/rotated cameras. It tests
  motion sign, half-pixel offsets, matrix transposition, Y inversion, viewport
  offsets/allocation extents, unequal viewport sizes, [-1,1] versus [0,1] clip
  depth, non-default depth range and reversed-Z interior geometry. Test-only
  adapters demonstrate the required coordinate/depth mappings; they do not
  change the production API or acceptance of resolution changes.
- `gow3-depth-samples-test`: synthetic two-sample positions and two surfaces,
  uniform-depth control, sample identity/position, normal/reversed-Z ordering,
  clear/foreground mixtures and nonfinite depth. Averaging depth from surfaces
  at z=2 and z=20 reconstructs z=40/11, not either surface or their arithmetic
  mean. Sample-zero, min, max and mean cannot identify the matching color surface
  without coverage/occlusion evidence. No resolve policy is selected.

The moving oracle detects convention errors that a stationary self-pair hides:
even consistently transposed matrices can cancel to zero motion. Reversed-Z
also exposes the current normal-Z clear assumption: exact depth zero can pass,
while depth one is rejected. An initial endpoint fixture using an arbitrary
rotated matrix was rejected by floating-point roundoff at the far clip plane;
the clear-policy test now uses exact binary coefficients to isolate that rule.
Strict clip boundaries remain unchanged and are not a robust clear classifier.

Native Clang 23.1.3, CMake/Ninja, two jobs: both new tests and the existing eight
CPU tests passed. The forward oracle's maximum error was 4.1e-13 pixels. Results,
including the first endpoint assertion and its corrected fixture rerun, are in
`out/dlaa-reprojection-contract/`. This phase changes only tests, build targets,
documentation and explanatory comments. The prior probe executable is unchanged;
no graphics, game, readback, upscaler or new runtime instrumentation was executed.

### Next discriminating check and acceptance criteria

Before another player session, a small offscreen Vulkan fixture should exercise
the intended diagnostic sampling path with a known two-sample D32S8 attachment,
two static surfaces and a sloped surface, known sample positions, forward camera
motion and one discontinuity/clear edge. Verify the actual host image/view sample
counts and supported sample locations rather than importing the guest count.
Compare each retained sample's reprojection to its known world-point oracle;
keep sample identity and coverage instead of silently averaging. A candidate
tolerance is 1e-3 pixel for interior points in this well-conditioned float fixture;
test clip/clear endpoints separately and record the error distribution. A one-sample
control must also pass. A successful synthetic GPU fixture validates this path
only, not the game's matrix convention. This proposed fixture was subsequently
authorized, implemented and executed as recorded below. Game collection still
requires separate coordination.

The smallest subsequent game diagnostic would retain one adjacent complete pair
of accepted-flip intervals, plus a small local edge/interior depth sample region.
Transport explicit flip, draw and attachment-generation identities. Use the
already-stable constant snapshot (bit-preserving VP), never an extra guest read.
Record the same draw's viewport scale/offset including signed height, depth
range, clip-space mode, target extent, actual sample count/locations, depth clear
and compare state, and the point at which that attachment is used or resolved
for scene color. Merely logging two matrices is insufficient: they can fit
multiple conventions without known geometry or pixel correspondences.

Accept a game convention only if known static correspondences across the pair
discriminate transpose/Y/depth alternatives, remain consistent at multiple
depths, and preserve sample/attachment identity. Validate background, disocclusion,
dynamic objects, cuts and menus separately. Fail closed on snapshot loss or an
ambiguous surface; never turn this evidence into a DLAA readiness label. Any
readback containing game content stays local. The present tests request no new
player gestures and add no Vulkan readback/synchronization implementation.

### Minimal ring-pressure measurements, before any proposed fix

No size, allocation, retirement or synchronization changes are made. A future
bounded opt-in diagnostic should capture the first few failed allocation episodes
and their recovery, with counts/high-water marks for the rest and a loss counter:

- Stage A, inside the existing allocation/retirement path: requested size and
  alignment, position, aligned/wrapped end, retired before/after the existing
  Retire, and the oldest published stamp end/tick and its existing IsFree result.
  Include total traffic and padding from all constants, not only the camera's
  144-byte input. Do not issue an additional semaphore refresh or wait.
- Stage B, at the existing Stamp/Publish calls: consumed packet end, current tick,
  pending end/tick, published end/tick and queue-full publication counts. Read
  thread-owned fields only on that thread. A diagnostic packet serial copied
  from A to B must join records; a global "latest" tick or flip is insufficient.
- An allocated range not yet stamped can indicate recording lag; a consumed range
  still coalesced into an unpublished tick can indicate publication delay; an
  oldest published stamp with IsFree=false identifies the completion gate. These
  are competing measurements, not causes inferred from the current reason label.

Retain the original SPSC ownership and ordering. Do not add cross-thread reads of
non-atomic fields. If observation would require new synchronization, stop and
review that design before collecting. Only a joined failure/recovery episode can
justify changing the ring; `ring-space-unavailable` alone cannot.

### Native offscreen Vulkan fixture results

`gow3-vulkan-depth-test` is a standalone executable linked only to Vulkan (plus
the C++ runtime), excluded from the default build. It does not initialize the
probe, SDL, a surface, swapchain, game, NGX or user-data paths. Its three test-only
GLSL files live in `tests/shaders/gow3_depth_fixture.*`; the VP consumer is not
the game's legacy motion shader. Build with at most two jobs:

```
cmake --build <configured-gpu-build> --parallel 2 --target gow3-vulkan-depth-test
<output>/gow3-vulkan-depth-test <configured-gpu-build>/gow3-fixture-shaders
```

The fixture checks a hardware Vulkan 1.1 graphics/compute queue, 128-byte push
constants, the combined attachment/sampled usages of D32S8 and RGBA32F at each
requested sample count, and sample-rate shading for the two-sample case.
Unsupported cases print `UNSUPPORTED` and return 77; no sample-count/resolve
fallback is substituted. It uses existing tools only. SPIR-V is compiled for
Vulkan 1.1 and separately checked with the existing `spirv-val`.

The renderer draws known world-space quads into a 64x48 D32S8 attachment and a
same-count RGBA32F metadata attachment. A background pass records sample positions
without writing depth; geometry then uses LESS and clear=1. Sample shading records
`gl_SamplePosition`, sample ID and fragment depth. Compute reads each depth sample
individually using `texture2DMS`/`texelFetch` for 2x (plain texture for the 1x
control), applies the test VP matrices, and writes readback results. No depth
averaging or resolve is performed. The 1x position is the nominal pixel center;
2x positions are measured in the fragment shader and checked for stable identity.
The negative-height viewport deliberately implements the chosen top-left/Y-up
clip convention. This is not evidence for the game's viewport convention.

An independent CPU forward projection/ray-plane intersection checks every sample's
stored depth, coverage, clear/out-of-view rejection and valid reprojection. The
existing CPU Reproject is also compared on its supported interior coordinate
domain. Samples in the first row/column with center-relative coordinates below
zero are covered by the independent oracle, not forced through the production
CPU reference's narrower coordinate bounds. All geometries are static: moving
camera motion and a stationary-camera control are tested, not object motion.

Actual execution: NVIDIA GeForce RTX 5070 Ti, Vulkan 1.4.357, driver raw
2604679168, sampleRateShading=true, standardSampleLocations=true, push limit 256.
Both required formats/usages support sample mask 0xf (1/2/4/8); only 1x and 2x
were tested. Observed 2x locations: sample 0=(.75,.75), sample 1=(.25,.25).

| Samples | Cases | Sample records checked | Maximum depth error | Maximum motion error |
| --- | --- | --- | --- | --- |
| 1 | uniform stationary, uniform moving, two surfaces/clear, slope | 12,288 | 1.00872e-7 | 1.02518e-5 pixel |
| 2 | same four cases | 24,576 | 1.11764e-7 | 8.21262e-6 pixel |

All eight cases passed with thresholds 2e-6 depth and 1e-3 pixel. Of 36,864
records, 33,338 have valid motion, 2,256 are clear and 1,270 project outside the
previous view. The 2x edge case contains 96 mixed-depth pixels (near/far and
far/clear boundaries). Stored depth equals sample-shaded fragment depth in these
cases. Maximum stationary drift is 4.26497e-6 pixel; maximum GPU/CPU-reference
motion difference is 8.05177e-6 pixel. The five SPIR-V binaries pass validation.

`VK_LAYER_KHRONOS_validation` is unavailable and was not installed. Debug-utils
reported zero errors/warnings, but this is NOT a Vulkan validation-layer pass.
Execution checks API return codes, bounded fence completion and numerical
results. It does not prove all synchronization/lifetime rules through validation.
First and final run logs, capabilities, shader validation and result summaries
are retained under `out/dlaa-vulkan-depth/`. Compilation used Clang 23.1.3 and
Ninja -j2. The game/probe was neither rebuilt nor launched in this phase.

Remaining discriminant: establish the real game's exact matrix/viewport/depth
mapping and attachment/sample identity, using the bounded adjacent-pair diagnostic
above before choosing a color/depth association or resolve policy. The synthetic
fixture proves this particular per-sample path on the tested GPU; it does not
validate GoW3 reprojection, temporal depth reuse, scene cuts, disocclusions,
dynamic objects, the legacy shader, NGX or DLAA. The ring remains unchanged.

### Targeted adjacent-pair metadata capture (prepared, not run in game)

There is a remaining data/lifetime boundary before a complete synchronized
pixel capture can be implemented safely. No verified GoW3 pre-interface marker
exists in this prototype. The legacy movie-shader hint and "last target before
a display draw" are not proofs of pre-HUD color. The texture cache owns backing
images in a deque and can change sample backings or reuse/alias content. Saving
a VkImage handle does not retain its lifetime or preserve its pixels. A future
GPU readback must be recorded at a validated point, keep staging and resources
alive until that submission completes, and handle per-sample depth explicitly.
No such copy, new fence/wait, thread or resource-retention mechanism is added here.

What the existing evidence supplies:

| Source | Available | Missing or ambiguous |
| --- | --- | --- |
| Deferred CSV v4 | explicit accepted-flip/presentation identities, loss counters, camera status/reasons | no matrix bits, viewport, host attachment identity or pixels |
| F11 analyzer | pass order, shaders, target addresses/formats, sample census, selected constants | constants rounded to six significant digits; display-based frame boundaries; no exact pair identity or depth readback |
| New pair metadata | exact existing slot-1 bytes and the same draw's state/attachments, mapped to ordered closing flips | no pixels, no sample positions, no proven pre-UI color, no complete compute/transfer resource graph |

The new `gow3_pair_capture.*` is opt-in and off by default. It requires both
`SPARTAN_GOW3_INPUT_DIAGNOSTICS=1` and `SPARTAN_GOW3_PAIR_METADATA=1`, plus distinct
absolute paths `SPARTAN_GOW3_PAIR_FILE` and `SPARTAN_GOW3_PAIR_TRIGGER`. Both parent
directories must already exist. Use new session-local paths such as
`<session>/logs/pair.jsonl` and `<session>/triggers/pair.start`. Existing output is
never overwritten. Missing/disabled flags create no capture object/file; invalid
paths disable this capture. There is no immediate/startup mode.

After the player is actually in game, a future coordinator can create the empty
trigger file. No trigger was created for a game in this development phase.
Only the existing ordered accepted-flip callback checks it. The opening marker
retains that preceding flip's full token and excludes its already elapsed
interval. The next two ordered intervals are collected, then the file is flushed
and the session is done. A trigger already present before startup follows the
same rule. A directory/symlink is not a trigger; repeated triggers do not reopen
or reset a capture. Rejected requests do not reach this callback; presentation
and redraw cannot close the window. An unexpected duplicate closing token fails
the capture instead of manufacturing a second interval.

Each draw in the instrumented direct/indirect recording path has a local draw
serial and interval ID. The terminating interval row carries the copied accepted
flip token and camera-frame status. IDs are associated by these rows, never by
the latest global counter or numerical order of EOP/non-EOP IDs. For presentation
outcomes, join that token to the existing v4 trace when available; this file alone
does not prove that the interval was displayed.

The 144 bytes are copied only from the already-stable ring snapshot, inside the
unchanged exact shader/stage/slot/length/depth guards. All 36 words are serialized
as hexadecimal integers, preserving NaNs, signed zero and the uninterpreted tail;
the first 16 words are not declared a calibrated matrix. Missing copies keep
their existing rejection reason. Repeated observations count all reasons and
missing copies, flag disagreements and preserve the first observation rather
than healing it. No new guest-memory access or fallback is used.

Per draw, metadata records:

- shader hashes, actual bound color mask versus shader exports, clip-space mode,
  clip-disabled state, the pipeline sample key and guest depth sample count;
- the exact float bits of every viewport passed to dynamic state, with its
  scissor, including signed height and min/max depth; viewport index selection
  inside a game shader is not inferred;
- texture-cache image UID, alias generation, guest address as a label only,
  backing image handle/format/extent/create-info sample count, render-view handle
  and requested mip/layer, for depth and each bound color target;
- the depth identity at camera observation separately from its later render
  binding, so sample-backing/rebind changes are visible;
- effective depth-test/write/compare settings, requested clear value when the
  render state requests a clear, render extent, indirect/square-pass flags and
  an explicitly legacy UI hint. A requested clear does not prove it executed.

Viewports are copied from the final values supplied to the scheduler, not
recomputed by a diagnostic formula. Render bindings are captured after resource
binding and render-state preparation. `recorded=1` means draw commands have been
recorded on the CPU path, not GPU completion. Failed binds remain `recorded=0`
attempts; they must not be treated as executed geometry. Indirect command count
and square-pass flags do not claim knowledge of GPU-written draw arguments.
Proxy/scaled paths are explicitly marked incomplete instead of claiming that
logical backing metadata describes a proxy image.

Only one pending draw is retained in CPU memory. A fixed 8,192-draw file budget
covers the two intervals; additional draws increment an exact drop counter.
Start/closing/end markers are outside that draw budget. A partial shutdown,
I/O error, lost draw or incomplete viewport/proxy metadata prevents a complete
window claim. Consumers must also require a valid final end marker and complete
interval joins. `complete_instrumented_draw_window` describes that narrow path:
compute, transfers and filtered/HLE operations are absent, not silently assumed
unchanged. Every end marker explicitly says `pixels_captured=false`,
`pre_ui_color_proven=false` and `sample_locations_captured=false`.

All mutable capture state is confined to the same ordered draw consumer as the
camera input diagnostic (stage B, or the drained serial fallback). Binding workers
and presentation threads never access it. Values are copied; no guest pointers
or GPU resource owners are retained. Camera/history decisions, ring allocation,
publication and retirement are unchanged. File I/O can perturb timing; this is
not a performance measurement.

`gow3-pair-capture-test` covers disabled/noncanonical flags, late and preexisting
triggers, two complete intervals, exact word copies, source mutation, missing and
conflicting snapshots, reused addresses with distinct UIDs/backings, EOP token
order, abandoned draws, capacity/loss accounting, partial shutdown, duplicate
boundaries, invalid paths and preserving an existing output. Synthetic JSONL is
also parsed and its joins/bit patterns checked independently. The separate probe
and eleven CPU tests compile/pass with two jobs; no game has run this instrument.
Build/results live in `out/dlaa-pair-metadata/`.

The next useful decision is to inspect this bounded metadata pair, together with
existing pass/shader information, for stable candidate color/depth relationships.
It can discriminate viewport/clip/sample-count and backing-identity assumptions,
but cannot itself prove pre-interface color or actual depth values. Before adding
pixels, choose and review the exact producer/consumer point and a bounded staging
lifetime/completion design. Do not ask for repeated identical gameplay captures
or enable DLAA to compensate for these missing facts. Keep resulting game metadata
and any future pixel/shader data local; no upload is part of this protocol.
