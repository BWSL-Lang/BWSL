# Intrinsics and Built-ins

Status: `stable` for the catalog, with some stage-policy details still
`provisional`

Primary implementation sources:

- `core/bwsl_stdlib.h`
- `phases/ir_lowering/bwsl_ir_lowering.h`

## Source of Truth

The canonical intrinsic catalog is the table in `core/bwsl_stdlib.h`.

That table currently defines families for:

- math
- trigonometry
- vector operations
- matrix operations
- derivatives
- texture operations
- synchronization
- wave/subgroup operations
- atomics
- bit operations
- control-flow selection
- boolean reductions
- float classification

## Families

Current intrinsic names include, among others:

- math: `lerp`, `smoothstep`, `saturate`, `fract`, `step`, `clamp`, `sign`,
  `abs`, `min`, `max`, `floor`, `ceil`, `round`, `mod`, `fmod`, `pow`, `sqrt`,
  `rsqrt`, `rcp`, `exp`, `exp2`, `log`, `log2`, `trunc`, `fma`
- trig: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sincos`,
  `sinh`, `cosh`, `tanh`, `degrees`, `radians`
- vector and matrix: `dot`, `cross`, `normalize`, `length`, `distance`,
  `reflect`, `refract`, `faceforward`, `transpose`, `determinant`, `inverse`
- derivatives: `ddx`, `ddy`, `ddx_fine`, `ddy_fine`, `ddx_coarse`,
  `ddy_coarse`, `fwidth`, `fwidth_fine`, `fwidth_coarse`
- texture and image: `sample`, `sample_lod`, `sample_bias`, `sample_grad`,
  `sample_cmp`, `gather`, `load`, `store`, `texture_size`, `texture_levels`
- synchronization: `barrier`, `memoryBarrier`, `storageBarrier`
- wave: `wave_sum`, `wave_product`, `wave_min`, `wave_max`, `wave_all`,
  `wave_any`, `wave_broadcast`, `wave_read_first`
- atomics: `atomic_add`, `atomic_min`, `atomic_max`, `atomic_and`,
  `atomic_or`, `atomic_xor`, `atomic_exchange`, `atomic_cmp_exchange`
- bit ops: `count_bits`, `reverse_bits`, `first_bit_low`, `first_bit_high`,
  `bitfield_extract`, `bitfield_insert`, `asfloat`, `asint`, `asuint`,
  `pack_unorm2x16`, `unpack_unorm2x16`, `pack_unorm4x8`,
  `unpack_unorm4x8`, `pack_snorm2x16`, `unpack_snorm2x16`,
  `pack_snorm4x8`, `unpack_snorm4x8`, `pack_half2x16`,
  `unpack_half2x16`
- control/select: `select`
- reductions and classification: `any`, `all`, `isnan`, `isinf`, `isfinite`,
  `isnormal`

BWSL intrinsic names are intentionally opinionated; backend-specific spellings
are handled during target emission rather than accepted as source-level aliases.

## Arity and Validation

The parser validates intrinsic argument counts using the metadata in
`core/bwsl_stdlib.h`.

Some texture intrinsics also support multiple call shapes. For example, the
current implementation supports both combined and split texture/sampler forms
for some sampling operations.

## Stage Restrictions

Currently enforced restrictions include:

- derivative intrinsics are fragment-only
- `sample_bias` is fragment-only
- `barrier`, `memoryBarrier`, and `storageBarrier` are rejected outside compute
  stages during lowering
- built-in input names such as `input.vertex_id` and `input.global_id` are
  stage-restricted as described in the shader-I/O section

## Compute-Oriented Intrinsics

Wave and atomic intrinsics are clearly part of the compute feature surface and
are exercised by tests and docs in that role.

The exact validation boundary for every one of those intrinsics is still partly
implementation-defined, because some policy lives in lowering rather than in a
single declarative front-end rule set.

## `select`

The current intrinsic form is:

```bwsl
select(false_value, true_value, condition)
```

This ordering is part of the implementation today and should be documented
exactly as such.
