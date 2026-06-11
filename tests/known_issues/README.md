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

## 4. Const-name array sizes only work for struct fields (`const_array_size_local.bwsl`)

`docs/language.md` ("Structs and Arrays") says array sizes can be "integer
literals or constant names that resolve at compile time". That is only true
for struct fields (`bwsl_parser_soa_types.inl`, "Expected array size (number
or constant)"). Local and `shared` declarations require a literal:
`Consume(TokenType::NUMBER, "Expected array size")` in
`bwsl_parser_soa_statements.inl`.
Once fixed: extend `tests/unsorted/arrays_const_sized.bwsl` (see note).

## 5. GLES backend emits ES 3.1 builtins in `#version 300 es` shaders

`unpackUnorm4x8` / `packUnorm4x8` etc. require ES 3.10, but the GLES output
declares `#version 300 es`, so `glslangValidator` rejects it. Reproduce with
existing tests — no dedicated repro file needed:

```bash
python3 tests/run_tests.py --gles
# GLES FAIL: module_compression_intrinsics, attributes_compressed_instance, ...
```

Fix is either a `#version 310 es` bump when packing builtins are used, or a
polyfill in the GLES backend (`phases/backends/gles/`).
