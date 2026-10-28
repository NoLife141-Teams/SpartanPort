# GoW3 clip/depth investigation (local prototype, 2026-10-27)

This is an offline source investigation and synthetic test result. It does not
validate a GoW3 matrix convention, image reprojection, object motion, or DLAA.
No renderer decision or camera-history behavior changes in this step. No game
was launched, no saves were read, and no game data was published.

Follow-up: the isolated shader cache later yielded two target SPIR-V permutations.
See [target shader recovery](gow3-shader-evidence.md) for verified bindings,
remaining draw-association limits and the prepared metadata-only instrument.

## What the collected metadata proves

The completed metadata pair has two ordered intervals (10306/10307), each with
301 copied camera observations, no missing snapshot or conflict. Its depth
attachment is D32S8, 1920x1080, two samples. The exact final viewport is
`[0,1080,1920,-1080,0,1]`; clip mode is `MinusWToW` and clipping is not disabled.
These are values from the recorded draw path, not inferred from matrix contents.
The local pair and its earlier analysis remain unchanged. These CPU records do
not supply GPU depth values, a completed pixel readback, actual sample locations,
the final pre-interface color, or the program producing the vertex position.

## Source chain and the missing link

1. `Rasterizer::BindBuffers` in `vk_rasterizer.cpp` iterates `stage.buffers`.
   Its `buffer_index` is the diagnostic slot; it is not automatically a guest
   hardware slot or a global Vulkan descriptor binding number. The existing
   snapshot observation selects the exact VS hash, resource index 1 and 144 bytes,
   then reads only the already copied constant-ring range. No reinterpretation
   or matrix transpose happens in that copy. `ObserveGow3Inputs` passes the same
   copy to the diagnostic. The first 16 floats are an experimental CPU model.
2. `S_BUFFER_LOAD_DWORD` in `frontend/translate/scalar_memory.cpp` constructs the
   buffer descriptor and produces `ReadConstBuffer` operations at dword offsets.
   `PatchBufferSharp` in `ir/passes/resource_patching_pass.cpp` replaces the handle
   with the discovered resource-list index. This generic machinery does not say
   which offsets the specific GoW3 program uses, its dot-product order, whether
   it applies an object transform, or whether it alters clip Z afterward.
3. `Translator::EmitExport` in `frontend/translate/export.cpp` routes the source
   VGPR components through `IR::ExportPosition`. `ir/position.h` sends Position0
   directly to `SetAttribute`; `EmitSetAttribute` in the SPIR-V backend stores
   those components in the position builtin. There is no generic automatic
   interpretation of a 16-float buffer as a camera matrix at this point.
4. The actual GCN/SPIR-V body/permutation of VS `aed40011eb5259f3` is absent from
   the inspected isolated capture artifacts. F11 text records hashes/constants,
   not shader instructions. Shader dumping was disabled in the pair session.
   `PipelineCache::DumpShader` returns immediately unless `IsDumpShaders()` is
   true; the existing general facility writes stage/hash/permutation `.bin` and
   `.spv` files under `ShaderDir/dumps`. No shader cache or save was inspected to
   fill this gap. A shader hash and finite/invertible constants cannot establish
   the missing program transformation.

## Runtime transformation after Position0

`BuildRuntimeInfo` in `vk_pipeline_cache.cpp` sets
`emulate_depth_negative_one_to_one` only for MinusWToW when the runtime lacks
depth-clip-control support. The two intended paths are:

- Native: `vk_graphics_pipeline.cpp` supplies
  `VkPipelineViewportDepthClipControlCreateInfoEXT::negativeOneToOne=true`.
  The exported position remains unchanged and the clip volume includes `-w<=z<=w`.
- Fallback: `EmitEpilogue` calls `ConvertDepthMode` in `emit_spirv_special.cpp`,
  storing `position.z=(position.z+position.w)*0.5`. Vulkan then uses `0<=z'<=w`.

The vertex epilogue applies the fallback after its optional motion output and
before the separate clip-disabled screen-position conversion. The captured
`clip_disabled=0` excludes the latter conversion for these draws. Motion effects
remain disabled. The exact enabled extension/permutation was not recorded in
the metadata; both paths have now been tested independently on synthetic data.

`UpdateViewportScissorState` uses `minDepth=zoffset-zscale` and
`maxDepth=zoffset+zscale` for MinusWToW; ZeroToW uses `zoffset` and
`zoffset+zscale`. Without unrestricted depth-range support, the current code
applies `max(minDepth,0)` and `min(maxDepth,1)`. The final viewport therefore
does not uniquely determine the original guest registers.

Let `q=z_clip/w_clip` be the position **exported by the vertex program**, before
the runtime's optional fallback. For an ordinary interpolated fragment without
depth replacement/bias/clamping, with final range `[a,b]`, both MinusW paths give
`D=a+(b-a)*(q+1)/2`. ZeroToW instead gives `D=a+(b-a)*q`.
For the observed final range `[0,1]`, the conditional inverse is `q=2D-1`.
This is not proof that multiplying the captured 16 floats by a world position
produces that `q`.

The negative viewport height affects Y independently of depth: framebuffer
coordinates are `x_f=x+(x_clip/w+1)*width/2` and
`y_f=y+(y_clip/w+1)*height/2`. The captured viewport therefore has
`y_f=1080*(1-y_clip/w)/2`. Coordinates here are framebuffer sample locations;
pixel-center indices additionally subtract 0.5. Do not apply another Y flip or
another depth conversion merely because the height is negative.

These equations follow the [Vulkan viewport definition](https://docs.vulkan.org/refpages/latest/refpages/source/VkViewport.html)
and [depth clip control](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineViewportDepthClipControlCreateInfoEXT.html).

Further runtime paths prevent an unconditional equation for stored game depth:
`UpdateDepthStencilState` can enable polygon depth bias; the graphics pipeline can
enable depth clamping; fragment `ExportDepth` becomes the `FragDepth` builtin
and `DepthReplacing` execution mode. The current metadata does not record these
effective bias/clamp/depth-export states. Subsequent depth writers/copies and
coverage also matter when selecting a completed image. No assumption that these
paths are absent has been added to the renderer or diagnostics.

## Tests and measured results

`gow3-clip-pipeline-test` is a new CPU test. It covers 21 clipping cases, including
both boundaries and rejected points, and 216 independent forward-projection
cases with positive/negative viewport heights, viewport offsets and normal,
restricted and reversed depth ranges. Native and converted-position models
agree with the separate analytic camera oracle. Inverting the original MinusW
matrix with decoded depth and inverting a Z-row-converted matrix with normalized
depth recover the same known world point (maximum error `2.65751488e-12`).
Counterexamples reject direct stored-depth use, double conversion, wrong Y and
the assumption that clamped and unrestricted ranges have identical mappings.
A constant depth range is explicitly non-invertible.

The existing standalone `gow3-vulkan-depth-test` now queries and, only when
supported, enables `VK_EXT_depth_clip_control` on its own test device. It uses
shader specialization for the alternative fallback and matching test-only
unprojection. Unsupported native capability is reported, never silently replaced.
The 128-byte push-constant layout and separate per-sample reads remain unchanged.
No production shader, camera helper or GPU capture path was changed.

On the RTX 5070 Ti (Vulkan 1.4.357, driver raw 2604679168), the extension and
feature are available. All 72 offscreen 64x48 cases passed: the original eight,
plus MinusW native/fallback with both viewport signs, `[0,1]`/`[0.2,0.8]` depth
ranges and one/two samples. Scenes include sloped geometry, two surfaces with
clear background, negative clip-Z geometry inside the frustum, and geometry
wholly before the near plane. Each sample is checked against a separate analytic
world/plane projection, without a depth average or resolve.

- 331,776 samples checked (237,744 geometry, 94,032 clear).
- Maximum depth error vs oracle: `1.11764058e-7`.
- Maximum motion error vs oracle: `1.02517599e-5` pixels.
- 32 native/fallback comparisons: maximum depth difference `1.1920929e-7`,
  maximum motion difference `9.53674316e-6` pixels.
- All five generated SPIR-V modules pass `spirv-val --target-env vulkan1.1`.
- CPU `gow3-clip-pipeline-test`, `gow3-reprojection-contract-test` and
  `gow3-depth-samples-test` pass. Compilation uses Clang and Ninja `-j2`.
- The Vulkan validation layer is unavailable; zero debug-utils errors is not
  a validation-layer pass. No installation was performed.

Outputs and complete logs: `out/dlaa-clip-pipeline/`, including `results.json`,
`gpu-statistics.json` and `logs/gpu.stdout.log`. The existing pair-metadata probe
is byte-identical and was neither rebuilt nor launched. Tests choose conventions
explicitly; the current production CPU `Reproject` still expects a matching
ZeroToW matrix/depth contract and is not silently adapted to the raw candidate.

## Smallest discriminating next evidence

First obtain the exact target VS GCN and emitted SPIR-V permutation, together
with its resource-index/descriptor mapping and runtime clip-conversion flag.
Trace the loads from the 144-byte resource to the four Position0 exports, including
any intervening object transform or Z remap. Also identify the paired fragment
program's depth export, effective depth bias/clamp and selected viewport. A
bounded, exact-shader-only export tied to the observed permutation would avoid
the existing global dump facility's breadth and overwrite behavior; none was
implemented or activated in this step.

Only after that algebra is known, pair real per-sample depth values with exact
constants, sample locations and a proven completed producer/frame identity.
The independent reference must predict those samples and image correspondences,
not merely return zero motion for a self-pair. Pixel/color association and GPU
completion still need their own bounded capture design. No repeated identical
metadata collection or DLAA activation is justified by these synthetic results.
