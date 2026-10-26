# GoW3 DLAA input prototype (experimental branch only)

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
agree exactly on VP, depth target and dimensions. A single-target display-pass transition closes
the frame once per contiguous group of display draws (direct or indirect), independently of the legacy
upscaler enable state. Missing/invalid/conflicting frames discard continuity;
resolution changes warm up again. The observed dimensions are the **depth allocation**,
not yet a proven active viewport (alignment/overscan remains to validate).

`GoW3 inputs:` logs startup, status changes, every 120 display transitions and
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

Cloud baseline: standalone motion-history, UI-composition and upscaler-support
checks passed. The new synthetic GoW3 input/reprojection test passed strict C++23
compilation and ASan/UBSan (LeakSanitizer unavailable under container ptrace). Full `build.sh` stopped at its dependency preflight: this executor
has no CMake, Ninja, glslangValidator or Vulkan/SDL3 development pkg-config entries.
Consequently production rasterizer compilation, Vulkan shader execution,
Windows/NVIDIA NGX initialization and in-game quality/performance are not claimed.
