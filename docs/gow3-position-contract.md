# Offline Position0 contract, 2026-10-27

Scope: the two previously recovered local SPIR-V modules for vertex program
`aed40011eb5259f3`, permutations 0 and 1. Their hashes and provenance are in
`gow3-shader-evidence.md`. No shader or game was executed in this analysis.
Disassembly and complete expression graphs remain in ignored local `out/`.

## Established algebra

Let C be the float interpretation of the 36 captured 32-bit camera-binding
words. The branch condition is the **raw unsigned bits** of word 32 != 0, not
a floating-point comparison. Let p be location-0 xyz. Its w is unused.

For r = 0,1,2, the object-transformed point is:

```
q[r] = T[r]*p.x + (T[4+r]*p.y + (T[8+r]*p.z + 1*T[12+r]))
```

For r = 0,1,2,3, the four values stored to Position0 are:

```
clip[r] = C[r]*q.x + (C[4+r]*q.y + (C[8+r]*q.z + C[12+r]))
```

Thus C[0..15] is consumed in column-major order, with homogeneous input w=1
after the affine object transformation. The shader's algebra alone does not
prove what coordinate basis the game assigns to q. There is no subsequent
division, clamp or Z remap in either module's Position0 dependency slice.
Other outputs/clip distances are not claimed irrelevant to rasterization.

When raw word 32 is zero, T[k]=C[16+k]. Only matrix indices
0,1,2,4,5,6,8,9,10,12,13,14 contribute. The bottom row and C[33..35] do not
contribute to Position0; this does **not** relax the runtime's 144-byte guard.

Otherwise T is a weighted palette from buffer binding 0. Each matrix occupies
16 float words. For every used k, the emitted addition order is:

```
T[k] = w3*B[16*i3+k] + (w2*B[16*i2+k] +
       (w0*B[16*i0+k] + w1*B[16*i1+k]))
```

Weights are not normalized. In permutation 1, weights are float location-1
xyzw; indices are location-2 signed integer components bitcast through float
back to unsigned bits. This is not numeric float-to-integer conversion.

Permutation 0 specializes both those inputs to (0,0,0,1). Independent scalar
inspection of its cached vertex specialization confirms component selectors
[Zero,Zero,Zero,One], versus [Red,Green,Blue,Alpha] in permutation 1. The ABI
helper matters: NumberClass is one byte and component mapping begins at byte 5,
not byte 8. Position selects [Red,Green,Blue,One] in both permutations.

The default permutation's final index is therefore the bit pattern 0x3f800000,
whose 32-bit multiplication by 16 yields 0xf8000000. A finite CPU palette must
reject it. This is **not a demonstrated runtime defect**: no live permutation
association was captured, and GPU bounds/robustness/optimization were not
established. OpSelect is value selection; the static graph contains palette
loads. Do not assume a false condition prevents those loads from executing.
The CPU mathematical evaluator intentionally does not model eager GPU loads.

## Relation to the existing real pair

In the existing 602 target observations, raw word 32 is zero 139 times and
nonzero 463 times (232 and 231 nonzero observations in the respective intervals).
The earlier pair omitted selected permutation keys, so these counts cannot be
assigned to one cached permutation. They demonstrate why both paths matter.
The palette and actual vertices were not snapshotted.

Both recovered VS metadata records disable shader emulation of MinusWToW Z.
Together with the observed MinusWToW clip setting, this narrows the expected
native Vulkan conversion, but does not by itself establish the stored depth:
active permutation, native clip-control support, bias, clamp, bounds, producer,
sample identity and later writes still matter. The prepared metadata captures
selected keys and actual depth-state arguments to test those conditions.

The CPU reprojection helper currently assumes ZeroToW depth and a top-left full
viewport. It has not been connected to these raw matrices. The legacy motion
shader still expects its old view/projection format. Neither path is enabled.
Camera-only reprojection cannot recover independently moving objects or palette
changes, even with perfectly known camera conventions.

## Validation

- 32 static assertions compare all four Position0 trees and twelve palette
  coefficients in both permutations. Only associative/commutative **unsigned
  address addition** is normalized; floating-point arithmetic order is retained.
- `gow3-vertex-contract-test`: 216 synthetic affine/palette/projection cases
  checked against independent analytic transforms and a camera projection
  oracle, maximum NDC error 1.45159276e-7. Covers unnormalized weights, raw
  negative-zero flag bits, ignored words/w, missing finite palette indices,
  negative MinusW clip Z and a moving-object counterexample.
- The test compiles with `-ffp-contract=off`; it does not claim GPU bit-for-bit
  equivalence or validate the real game's geometry, depth or motion.
- Previous clip/depth and offscreen Vulkan fixture results remain in
  `gow3-clip-depth-analysis.md`; they are synthetic, not a game validation.

Local evidence: `out/gow3-position-analysis/verified-contract.json`,
`out/dlaa-vertex-contract/test.log`, and the grouped validation results.

The next discriminating observation is an exact draw/permutation/buffer/state
association, followed by a synchronized per-sample depth and color experiment.
The grouped collection plan separates these two readiness levels.
