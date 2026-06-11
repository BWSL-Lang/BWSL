# Known Issues

Minimized reproductions of compiler bugs found while extending the test
suite (2026-06-11). These files are **not** picked up by `tests/run_tests.py`
— they are kept here so each bug has a ready-made repro and can be promoted
into `tests/unsorted/` (or `tests/equivalence/`) the moment it is fixed.

Each repro compiles with:

```bash
./build/bwslc tests/known_issues/<file>.bwsl -modules modules -o /tmp/out
```

## 1. ~~`sample_cmp` emits no SPIR-V~~ (FIXED)

Fixed by emitting `OpImageSampleDrefImplicitLod` (scalar result, splatted to
float4) for `OP_TEX_SAMPLE_CMP` in
`phases/backends/spirv/bwsl_spirv_backend_ops.inl`. Coverage lives in
`tests/unsorted/texture_sample_grad_cmp_gather.bwsl` (both implicit- and
explicit-sampler forms).

## 2. ~~Implicit `it` iterator unusable~~ (FIXED)

Fixed by retiring the `it` keyword: the lexer now emits a plain IDENTIFIER
(`phases/lexing/bwsl_lexer.cpp`), so body references bind to the implicit
iterator by name exactly like the named `for (x in collection)` form. Also
removed a bogus `AddSymbol` in the implicit branch that registered the
collection's own name and broke its array-length lookup. Coverage lives in
`tests/unsorted/loops_foreach_multirange.bwsl` (implicit `it`, nested
shadowing, and `it` as an ordinary variable name).

## 3. ~~Struct fields that are arrays of vectors/matrices miscompile~~ (FIXED)

Fixed with fused two-level access opcodes (`OP_STRUCT_ARRAY_EXTRACT` /
`OP_STRUCT_ARRAY_INSERT`): `s.field[i]` reads and writes now address
`struct.field[elem]` in a single instruction (multi-index
OpCompositeExtract/Insert for constant indices, a pre-declared
Function-storage scratch variable + OpAccessChain for dynamic indices),
and writes reconstitute the struct via store-back. This also avoids
materializing array-typed temporaries, which SPIRV-Cross MSL cannot
assign. Investigation showed scalar arrays were silently broken too —
stores into `float w[4]` fields were dropped (the old path stored into
an extracted copy); these are fixed by the same change.

Coverage: `tests/unsorted/structs_vector_matrix_array_fields.bwsl`
(mat4/float4/float element arrays, constant and dynamic indices) and
`tests/unsorted/arrays_const_sized.bwsl`.

Remaining gaps (pre-existing, narrower):
- Nested chains that write *through* an element of a struct-array field
  (`s.frames[i].time = x` where `frames` is an array field) still go
  through the old extract/ARRAY_STORE path and may drop the write.
- Whole-array reads of struct fields (e.g. passing `s.bones` to a
  function) still materialize the array value, which SPIRV-Cross MSL
  cannot always assign.

## 4. ~~Const-name array sizes only work for struct fields~~ (FIXED)

Fixed by routing array-size parsing through a shared parser helper that accepts
integer literals or compile-time integer constant names. Coverage lives in
`tests/unsorted/arrays_const_sized.bwsl` for struct fields, local arrays,
block-local const-sized arrays, and shared arrays.

## 5. ~~GLES backend emits ES 3.1 builtins in `#version 300 es` shaders~~ (FIXED)

Fixed by replacing ES 3.10-only 4x8 pack/unpack builtins with `bwsl_` polyfill
calls for `#version 300 es` output. The SPIRV-Cross wrapper patches generated
GLES source before validation.

Coverage lives in the existing packing tests: `attributes_compressed_instance`,
`module_compression_intrinsics`, `modules_debug_spaces_sampling_packing`, and
the pack/unpack fuzz regressions. `python3 tests/run_tests.py --compiler
./build/bwslc --gles --no-spirv-val` validates all GLES outputs with
`glslangValidator`.

## 6. ~~Scalar float varyings fail SPIR-V validation~~ (FIXED)

`output.brightness = 0.75;` in a vertex stage declared the varying interface
variable as float4 (the fallback) while storing a scalar float, so spirv-val
rejected it with "OpStore Pointer's type does not match Object's type".
Constant operands are encoded as pseudo-registers with the type in the high
bits (`0x8000` float, `0x4000` int, `0x2000` uint, `0xC000` bool — see
`IRLowering::GetRegisterType`), so the `OP_STORE_OUTPUT` case in
`phases/ir_generation/bwsl_ir_analysis.cpp` never found them in
`registerTypes` and left `outputTypes[slot]` unset. Fixed by decoding the
constant prefix there. Varyings fed from real registers (vectors, computed
scalars) already carried the right type.

Coverage lives in `tests/unsorted/varyings_scalar.bwsl` (constant-fed scalar,
expression-fed scalar, and a vector varying for location assignment).
