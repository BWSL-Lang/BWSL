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

## 2. Implicit `it` iterator unusable (`implicit_it_unparsed.bwsl`)

`docs/spec/05-statements-and-control-flow.md` documents
`for (values) { ... }` with an implicit `it` variable. The lexer reserves
`it` as `TokenType::IT` (`phases/lexing/bwsl_lexer.cpp:452`), and the parser
builds the collection loop with an `it` identifier
(`bwsl_parser_soa_variants_eval.inl`, "Collection iteration with implicit
'it' variable") — but no parser rule ever accepts `TokenType::IT` as an
expression, so any body that references `it` fails with "Expected
expression". Either the lexer should stop tokenizing `it` specially or the
expression parser must accept it.
Once fixed: extend `tests/unsorted/loops_foreach_multirange.bwsl` (see note).

## 3. Struct fields that are arrays of vectors/matrices miscompile (`struct_vector_array_invalid_spirv.bwsl`)

A struct field like `float4 v[4]` or `mat4 bones[4]` parses and lowers, but
reading an element back produces mistyped registers and invalid SPIR-V, e.g.:

```
error: Expected arithmetic operands to be of Result Type: FMul operand index 2
error: Expected total number of given components to be equal to the size of Result Type vector
```

Scalar arrays (`float w[4]`) and arrays of structs (`Pose[2] poses`) work.
The bug is in struct-array element lowering (`phases/ir_lowering/`,
struct GEP/load path). The repro fails even with constant indices and no
control flow.
Once fixed: upgrade `tests/unsorted/arrays_const_sized.bwsl` and
`tests/unsorted/structs_array_of_structs_methods.bwsl` to use vector/matrix
element arrays.

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
