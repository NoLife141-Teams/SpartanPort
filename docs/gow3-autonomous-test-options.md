# Autonomous validation inventory (local, 2026-10-27)

The user wants autonomous tests, permits brief movement and the existing health
cheat in the isolated copy, and no longer requires a life/death detector. Those
later instructions supersede the life-state prerequisite in older design notes.
They do not establish a safe process/session binding or a verified load path.
This investigation did not launch a game, read saves, retry OpenProcess, send
input, modify runtime behavior, install anything, or change security settings.

## Existing mechanisms and their actual coverage

| Mechanism | Evidence | Coverage / limitation |
| --- | --- | --- |
| Native pad file and replay | `src/runtime_pad.c:552`, `:628`; existing native replay tests | Guest button/stick sampling exists. Finite replay with a terminal neutral sample is tested offline. Plain held file input has no lease on file disappearance; physical input resumes after replay. This is not an authenticated live session-control API. |
| `tools/press.sh` | Entire five-line script read | Writes shared `out/pad`, sleeps, then empties it. No session binding or crash-safe neutralization. Not executed. |
| `tools/restart.sh` | Source read | POSIX utilities, name-wide `pkill` including force kill, shared paths, two timed Cross presses and draw-count guesses. Does not implement this Windows isolated-save workflow. Not executed or adapted around the refused process check. |
| `tools/soak.sh` | Source read | Calls restart, alternates stick values and tests process existence. Its 'alive' output is process survival, not gameplay correctness. Not executed. |
| Save choice | `src/runtime_services.c:323`, `gpu/shim/spartanport_overlay.cpp:1569` | Load/Save/Delete are distinct dialog types; list UI consumes SDL keys/gamepad events. `choice_begin`, `choice_poll` and `choice_confirm` are internal presentation APIs, not an external command to load a named save. Even a single listed save is not automatically selected. |
| Probe command-line | `src/probe.c:930`, `:987` | CPU-only/import/Vulkan/capability diagnostics exist. No autoload/save-selection/gameplay benchmark option found in the inspected parser. Windows ignores `--timeout`; it is not an exit guarantee. |
| Computer Use | Installed window API documentation | Can target its own UI actions to a window. No transferable process/session attestation is exposed to the native script. |
| Loader fixtures | `tests/test_probe.py` | Tiny synthetic images exercise native-loader boundaries, not save loading or camera calibration. Not run here; no reason to invoke the probe for this math regression. |
| FSR benchmark | `gpu/tools/fsr4_bench.cpp` | Separate synthetic FSR provider benchmark that needs provider assets. It is not a DLAA/NGX test or a GoW3 replay; not run. |
| Standalone Vulkan tests | `test_gow3_vulkan_depth.cpp`, `test_gow3_vulkan_readback.cpp` | Existing independent two-sample depth/copy tests can exercise GPU contracts without game input. Prior results are documented separately; no new GPU run was performed in this investigation. |

The exact blocked live operation remains the read-only identity query
`OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 23840)`, error 5, before
any native command publication. The old process is closed. No supported
replacement with equivalent live identity guarantees has been established.
No process-inspection or input path was substituted for that refusal.

## New useful autonomous CPU replay

`tests/test_gow3_offline_sequence.cpp` feeds the real `CameraFrames` and
`Reproject` code deterministic synthetic host snapshots. It does not model
Windows input or claim to load the game. The reference uses independent
analytic world/camera projection rather than the tested matrix inverse.

The sequence combines:

- 240 intervals each for explicit ZeroToW and test-only converted MinusWToW;
- five display groups and three camera observations per normal accepted flip;
- stationary and moving camera segments, alternating EOP labels/buffers;
- an empty interval, failed snapshot with a later good copy, matrix conflict,
  resolution changes and recovery; rejected empty flip tokens stay inert;
- separate synthetic sample positions and two surfaces, without depth averaging;
- stale previous-frame negative controls and a deliberate camera discontinuity.

This is an ordered CPU consumer replay. It does not exercise actual producer
thread scheduling, EOP submission or presentation; the existing frame-order
tests cover their models separately. It is not a replay of the captured game's
pixels or command buffers. A candidate after the synthetic cut is explicitly
recorded as a limitation, not accepted as valid temporal blending.

Run `scripts/test_gow3_offline_sequence.ps1` with the existing Clang toolchain.
It compiles and executes five standalone tests serially, retains unique output
folders and restores its process PATH. It does not call the game launcher,
the native menu script, process-inspection APIs, or savedata routines.

Verified output: `out/offline-sequence-20261027T205024144Z/result.json`.
All five builds and tests passed with one compiler job:

- New replay: 480 intervals, 462 candidates, 12 warming intervals, 6 rejected;
  2,772 per-sample checks, maximum error 0.0000603890743 pixels;
  2,520 stale-pair counterexamples detected, two cuts not classified.
- Reprojection: 108 forward-oracle cases, maximum error about 4.1e-13 pixels.
- Depth samples: separate surfaces, phantom average-depth surface, clear/sample
  position and normal/reversed depth counterexamples pass.
- Clip pipeline: 21 clipping and 216 world/viewport cases; inverse error
  2.65751488e-12, direct/double conversion and Y counterexamples pass.
- Vertex contract: 216 affine/palette/MinusW cases; NDC error 1.45159276e-7.

## Independent progress and remaining work

These tests can run unattended now and detect math/history regressions without
the user or a game process. Existing local metadata can support offline checks
of associations, loss counts and matrix consistency; it cannot regenerate
missing GPU pixel contents, unknown sample locations or color before interface.
Synthetic Vulkan tests can continue to validate explicit GPU hypotheses, but
neither synthetic path demonstrates GoW3 correspondence or working DLAA.

A complete gameplay driver is not currently an existing supported facility.
A future design would need a reviewed session-bound launch/control lifecycle,
a typed load-only dialog adapter (never Save/Delete/New Game), bounded commands
and acknowledgements at the actual input consumer, explicit load confirmation,
and reliable owned-instance shutdown. It must not use old positive health as
proof or require the waived death detector. Enemy-induced motion must remain
separate from evidence of the commanded movement.

That is runtime integration, not merely another shell script. It has not been
implemented or approved here as a workaround to the refusal. Any proposed
identity mechanism must first be reviewed for the execution framework; do not
silently remove the existing check or claim filenames/log self-reports are an
equivalent authentication boundary. No further user play session is requested
by this investigation. DLAA remains disabled and unvalidated.
