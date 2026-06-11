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

## 6. Scalar float varyings emit a type-mismatched OpStore

`output.brightness = 0.75;` in a vertex stage declares the varying interface
variable with a type that does not match the stored scalar, so spirv-val
rejects the stage (`OpStore Pointer ... type does not match Object`).
Vector varyings (`float2`/`float3`/`float4`) work. Repro:
`scalar_varying_store.bwsl`.

## 7. Loop `break` after a `switch` in the same loop body

A conditional `break` that follows a `switch` statement inside a loop emits an
`OpBranchConditional` whose condition ID is never defined. Either construct
alone is fine; the combination is broken. Repro: `loop_break_after_switch.bwsl`.

## 8. Vector comparison assigned to scalar `bool`

`bool eq = a == b;` with vector operands is accepted by the front end, then
produces a scalar `OpSelect` over a `bool3` condition, which spirv-val rejects.
`all(a == b)` / `any(a == b)` work correctly (covered by
`tests/unsorted/bool_vectors_select.bwsl`); the scalar assignment needs a
front-end diagnostic. Repro: `vec_compare_scalar_bool.bwsl`.

## 9. `sample()` in vertex stages emits ImplicitLod

`sample(...)` in a vertex stage emits `OpImageSampleImplicitLod`, which SPIR-V
forbids outside fragment-like execution models. Needs either a diagnostic
pointing at `sample_lod` or automatic lowering to explicit LOD 0.
`sample_lod(...)` works in vertex today (covered by
`tests/unsorted/vertex_texture_fetch_lod.bwsl`). Repro:
`sample_implicit_lod_vertex.bwsl`.

## 10. `mat3(mat4)` truncation constructor miscompiles

The conversion constructor treats the source matrix as a scalar constituent
and emits `OpCompositeConstruct %v3float %m %m %m`, which is invalid. Needs
real column truncation or a rejection diagnostic. Repro: `mat3_from_mat4.bwsl`.

## 11. Missing front-end semantic validation (wrong-accepts)

These invalid programs compile without any diagnostic and produce valid (but
semantically wrong or meaningless) SPIR-V. Each has a `wrong_accept_*.bwsl`
repro that currently **compiles successfully**:

- `wrong_accept_break_outside_loop.bwsl` — `break`/`continue` outside any loop
- `wrong_accept_const_reassign.bwsl` — assignment to a pipeline-scope `const`
- `wrong_accept_duplicate_params.bwsl` — two function parameters with one name
- `wrong_accept_missing_return.bwsl` — non-void function with a fall-through
  path that returns no value
- `wrong_accept_swizzle_dup_write.bwsl` — write mask with repeated component
  (`v.xx = ...`)
- `wrong_accept_vec_ctor_arity.bwsl` — `float4(a, b, c)` silently zero-pads
  the missing component
- `wrong_accept_attribute_write.bwsl` — assignment to `attributes.*` inputs
- `wrong_accept_array_oob_const.bwsl` — statically out-of-bounds and negative
  constant array indices

Also in this family but caught late by the embedded spirv-val (so they do fail,
just with a poor diagnostic): unknown struct fields, out-of-range swizzles on
narrow vectors, mismatched ternary arm types, and bool-to-float assignment.
Those are locked in as `tests/error_cases/*` with the expectation
`"SPIR-V validation failed"`; when proper front-end diagnostics are added,
update the expected messages in `tests/run_tests.py`.

## Notable limitations (rejected, possibly by design)

Recorded here so future test writers don't re-discover them:

- Array-typed function parameters (`f :: (float vals[4]) -> ...`) fail to
  parse ("Expected ')' after parameters"). Workaround: wrap in a struct.
- `texture2D`/`sampler` function parameters fail to parse.
- Negative `case` labels are rejected ("switch case values must be
  compile-time literals") even though `-1` is a constant expression.
- Comma lists in `for` init/increment clauses are rejected.
- Leading-dot float literals (`.5`) and digit separators (`1_000`) are
  rejected; trailing-dot (`1.`) is accepted.
- Pipeline-scope `eval` consts: `const int BAD = 1 / 0;` is accepted and
  folds silently; integer literal overflow (`2147483648`) is accepted and
  wraps.
