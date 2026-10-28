# Target shader recovery and selected-draw metadata

Local investigation, 2026-10-27, based on the isolated experimental checkout at
`f58660558a03e74af057f9cdfa556e31346bbba9`. No game launch, save access, shader
execution, system changes, commit or publication was part of this step.

## Existing evidence recovered

The earlier F11/metadata captures do not contain shader instructions. The
isolated test runtime's own `user/cache/CUSA01623` directory does contain the
translated SPIR-V and version-7 shader metadata. `RegisterShaderBinary` names
SPIR-V files by program hash and permutation index; `RegisterShaderMeta` names
metadata by `HashCombine(program_hash, permutation_index)`.

Only the exact target's two files and matching metadata were copied to the new
ignored local directory `out/gow3-shader-evidence/`, with exclusive creation and
byte/hash verification. Source files were read directly. `DataBase::Open`, cache
warm-up, and runtime deserialization were not called: those paths can write or
clear a cache, create a Vulkan device, or register a serialized SRT walker.
No original installation or original checkout cache was opened.

| Vertex permutation | Pipeline stage key | SPIR-V bytes | SHA-256 |
| --- | --- | ---: | --- |
| 0 | `e43a5ea709d996ab` | 18996 | `edbd6d4cf628d1ca0cd7e88094d1859dde9f44f2ef253704efe73ba8f82cb912` |
| 1 | `e43a5ea709d996aa` | 19076 | `693ec10d0ae36f15bbc7918bf464931cf90cddafa1b282dcf9d8afc243eade36` |

Both are SPIR-V 1.6 modules for target `aed40011eb5259f3`. Both pass
`spirv-val --target-env vulkan1.3`; disassembly remains local, outside tracked
source files. These are emitted modules, not raw GCN. No raw GCN was recovered.
The shim's `IsPatchShaders()` returns false; no shader patch path was enabled.

The bounded offline metadata reader checks metadata/binary version 7, both
permutation identities, persistent program hash, hardware/software vertex stage,
resource counts and input lengths. An independently compiled, runtime-free ABI
helper supplies sizes/offsets for this local build. The parser does not construct
cached C++ objects, dereference cached pointers, load flattened guest data, or
register/execute the trailing SRT walker. This is intentionally ABI-specific,
not a general cache importer. Ten malformed/truncated/overlarge-count cases are
rejected.

For both cached vertex permutations, metadata records:

- `emulate_depth_negative_one_to_one=false`, `clip_disable=false`, motion=false;
- three buffer resources, no image/sampler resources, descriptor start zero;
- camera resource index 1 maps to expected descriptor set 0, binding 1 and
  push-offset index 1. Its four descriptor words come from flattened offsets
  12..15; it is a read-only guest resource. SPIR-V decorations independently
  confirm set 0/binding 1. Resource 0 is a guest buffer, resource 2 is ClipPlanes.

The existing pair names fragment shader `aed40011d135bdf8` for all 602 target
observations. Its one currently cached permutation (0) and matching metadata were
also copied/verified locally. The valid SPIR-V contains neither DepthReplacing
nor a FragDepth builtin. This is a fact about that cached module, not proof that
an unrecorded live permutation or different producer never wrote depth.

The old pair recorded raw shader hashes, not selected pipeline stage keys. It
therefore cannot decide between vertex permutations 0 and 1. Cache timestamps
and a current directory listing cannot retroactively prove that association.
The two native/no-fallback metadata records narrow the candidates but do not
replace an observed draw-to-permutation link. They also do not supply depth bias,
clamp, bounds, actual GPU pixels or the final pre-interface color.

## Minimal additional instrumentation

`SPARTAN_GOW3_SHADER_METADATA=1` adds shader details to the existing deferred
two-interval metadata capture only when both `SPARTAN_GOW3_INPUT_DIAGNOSTICS=1`
and `SPARTAN_GOW3_PAIR_METADATA=1` are also exactly enabled. It is disabled by
default and never exports shader programs. Existing absolute file/trigger paths,
ordered start boundary, 8192-draw budget, loss counters and exclusive file-open
rules remain in force. Shader-enabled captures declare schema v2.

Details apply only to the exact target vertex hash. Each pending draw owns:

- the VS and PS stage keys copied from its selected `GraphicsPipelineKey`;
- depth-clamp/depth-clip key bits and the instance's depth-clip-control,
  depth-clip-enable and unrestricted-range support flags;
- every target vertex buffer descriptor actually written by `BindBuffers`:
  resource index, destination binding, push-offset index, resource kind, host
  buffer identity label, descriptor offset/range and whether a ring binding was
  used. No pointed-to data or additional guest memory is read;
- exact float bits of the constant/clamp/effective-slope arguments supplied to
  `SetDepthBias`, plus its enable state, depth-bounds enable/range and an explicit
  dynamic-state observation flag. Disabled values are not claimed as active.

Bindings are capped at 32 per target draw; overflow is counted and makes the
evidence incomplete. A recorded target draw missing its key, dynamic observation
or camera binding likewise prevents a complete-window claim. Each Begin resets
the entire record, so another shader/permutation cannot inherit a previous key,
bias or binding. Existing missing/conflicting snapshot results are unchanged.

All accesses remain on the ordered draw consumer, including buffer descriptor
writes when texture preparation uses a helper. There is no global 'last shader'
association, compiler-thread hook, new lock/thread, GPU readback or resource
lifetime change. Bias arguments are computed once and passed unchanged to the
existing setter before their bits are copied into metadata.

The fragment export field deliberately says `inspect_exact_ps_spirv`.
`Info::stores` is not persisted by `InfoPersistent::Serialize`; reading the
zero-initialized field after a cache load would falsely claim no depth export.
Use the captured PS stage key to find/verify its exact cached module instead.

## Validation and remaining collection

Separate probe: `out/dlaa-shader-evidence/bin/spartanport-probe.exe`.
SHA-256: `FD5472235FFFBAED59E630D5D67E6F0053347399DDEEA3D5112841B9D1833723`.
The previous pair probe remains byte-identical. Compilation is limited to two
jobs. An initial build failure was restricted to the new ABI helper treating
existing upstream-header warnings as errors; narrowly allowing those warning
categories on that helper resolved it. The original build log is retained.

All twelve CPU tests pass: pair capture, clip pipeline, reprojection contract,
depth samples, deferred trace trigger, snapshot reasons, camera frames, frame
trace, ring stamps, camera inputs, motion history and UI composition. The pair
test additionally covers strict separate opt-in, exact shader filtering,
per-draw selected keys, copied bindings, dynamic reset, exact bias/bounds bits,
unknown PS export, preserved snapshot rejection, overflow and incomplete
evidence. Seven synthetic JSONL files parse independently; the two deliberately
incomplete shader draws correctly prevent a complete capture claim. All three
recovered modules pass static SPIR-V validation. No module was executed.

Evidence/results are in `out/gow3-shader-evidence/` and
`out/dlaa-shader-evidence/`. They are local ignored output, not publication
artifacts. Shader bodies and disassembly must stay local.

No additional game session is needed to recover these existing modules or
continue their offline algebra analysis. A coordinated new metadata collection
is necessary to bind an actual draw/frame to its selected permutation and the
previously unrecorded bias/clamp/bounds state. The prepared probe can collect that
in one deferred two-interval window after the user is in gameplay, with the new
flag enabled and all upscalers/motion/jitter still disabled. Do not launch it
without coordination. Actual per-sample depth/image correspondences and GPU
completion remain a later, distinct requirement; DLAA is not validated.
