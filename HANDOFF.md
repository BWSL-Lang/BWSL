Compiler handoff
================

Snapshot of compiler robustness, known-open bugs, and next-step plans as of
2026-04-20 (second session). Read this when resuming work on the compiler
itself (as opposed to `fuzz/HANDOFF.md`, which covers fuzz-harness state).

TL;DR
-----

- 323 regression tests pass, 122 equivalence tests pass, 50 golden files
  match. Every HANDOFF-known compiler bug now fixed, plus a long tail
  of issues discovered by fuzz-style regression tests:
  pointer-to-struct-field (OP_LOCAL_FIELD_PTR), recursion diagnostic,
  trailing-dot literal, matrix-matrix add/sub/div, matrix-column
  write-then-read, swizzle-on-scalar, bitwise / shift / modulo
  compound-assign, determinant result type, brace-list array
  initializer, general out-of-order swizzle writes (`v.wz = …`),
  SSA phi operand + dead-block cleanup for unreachable latches,
  spurious stderr spam from overload-resolution debug prints, SSA
  register-type OOB when inlining many calls of a multi-return
  helper (undercounted estimatedNewRegs), scalar-on-left matrix
  multiply (`s * M`), integer-vector comparison result type
  (`ivec < ivec` → bvec not bool), nested-block variable shadowing
  (`{ float x = …; }` inside an outer `int x` no longer leaks the
  inner binding). Only external bug is wave-ops cross-compile
  (SPIRV-Cross gap).
- 1-hour fuzz runs produce 0–1 crashes at the moment; coverage plateaued
  around 19,600–19,700 edges. Any remaining crashes are narrow.
- Three of the four HANDOFF-known bugs are now fixed: pointer-to-struct-field
  lowers to OP_LOCAL_FIELD_PTR + OpAccessChain; recursion emits a clean
  diagnostic at IR-lowering time and short-circuits before SPIR-V emission;
  trailing-dot float literals (`10.`, `0.`) now lex correctly while still
  preserving the range operator (`0..10`) and member access (`v.x`).
- Only Wave-ops-cross-compile (SPIRV-Cross gap, not our bug) remains.
- Main gaps: raster-stage equivalence (vertex/fragment needs new runner
  infrastructure), and a long tail of feature combinations that haven't
  been probed.

Known-open bugs
---------------

### 1. `pointer-to-struct-field` (FIXED 2026-04-20)

Original capture: `tests/fuzz_regressions/pointer_to_struct_field.bwsl`.

```bwsl
struct V { float3 pos; float w; };
V v;
float3^ p = ^v.pos;   // used to be rejected by SPIR-V validator
p^ = float3(10.0, 20.0, 30.0);
```

Fix: new IR opcode `OP_LOCAL_FIELD_PTR` carrying `(base_struct_reg,
field_index, struct_type_hash-in-metadata)`. IR lowering detects
`ADDRESS_OF(member_access on struct-typed local)` and emits this opcode
instead of `OP_LOCAL_VAR_PTR` on the extracted by-value copy. The SPIR-V
backend allocates an `OpVariable` of the base struct type, keeps it
synced via `OpStore` at the ADDRESS_OF site, and emits
`OpAccessChain %_ptr_Function_FieldType %var %field_const` as the
pointer value. `OP_LOCAL_LOAD` / `OP_LOCAL_STORE` honour a new
`STORAGE_IS_FIELD_PTR` flag — when set, they route through
`spirvIds[ptr_reg]` (the access chain) instead of `localVarIds[src_reg]`.
SSA was extended to treat `OP_LOCAL_FIELD_PTR` as an operand-renaming op
with operand 1 as a literal field index.

Coverage: `tests/equivalence/test_pointers_struct_field.bwsl` and
`tests/equivalence/test_pointers_struct_field_control_flow.bwsl`
(if/else and short loop against the same pattern). The fuzz-regression
stub was updated to a compile-success test that exercises both vector
and scalar field pointers.

### 2. Wave operations cross-compile to Metal / GLSL

Captured implicitly by `test_wave_ops.bwsl` passing only on `(spirv)`.

SPIRV-Cross fails to translate `OpGroupNonUniform*` opcodes into Metal
and GLSL variants of subgroup ops on this toolchain. HLSL cross-compile
works. Not a bug in our code — SPIRV-Cross gap. Two responses possible:

- Accept the 2-of-4-backend coverage for wave tests.
- Ship custom emitters for Metal / GLSL for the subgroup opcodes the
  language already exposes (`wave_sum`, `wave_all`, `wave_any`,
  `wave_broadcast`, etc.). Medium-size effort, confined to
  `bwsl_spirv_backend.cpp` / the cross-compile layer.

### 3. Recursion in user functions (FIXED 2026-04-20)

`IRLowering::TryInlineFunction` now keeps a stack of currently-inlining
function NodeRefs. On recursion it emits a single stderr diagnostic
("Error: recursion is not supported — …") and sets `recursionDiagnosed`
on the lowering. `bwslc` checks that flag post-lowering and aborts the
stage with a clear error before SSA / SPIR-V emission, so the cryptic
validator message (`Expected int scalar or vector type as operand`) no
longer surfaces. Exit code is 1 as expected.

Regression: `tests/fuzz_regressions/recursion_not_supported.bwsl`.

### 6. Parser rejects bitwise / shift / modulo compound-assign (FIXED 2026-04-20)

`bwsl_token_defs.h` gained `MODULO_ASSIGN`, `BITWISE_AND/OR/XOR_ASSIGN`,
`LEFT/RIGHT_SHIFT_ASSIGN`. The lexer matches them (ordering `>>=`
before `>=` matters — same for `<<=`). `ASSIGNMENT_OPERATORS` token
mask in `bwsl_types.h` includes them, the compound-assign desugar in
`ParseAssignment` maps them to `BinaryOpType::MODULO`/`BITWISE_*`/
`LEFT_SHIFT`/`RIGHT_SHIFT`, and the statement-level dispatcher in
`ParseStatement` recognises them as assignment-starting tokens.

Regression: `tests/fuzz_regressions/bitwise_compound_assign.bwsl`.

### 7. Swizzle-on-scalar and matrix-column-swizzle-after-write (FIXED 2026-04-20)

`LowerMemberAccess` single-component paths now return the source
register unchanged when the source CoreType is already scalar,
stopping the downstream OpCompositeExtract-on-non-composite from
reaching the validator. The chained-access site (`v.r.r`) skips the
shortcut when the object came from an ARRAY_ACCESS — struct-field
array loads carry a scalar IR type but a SPIR-V vector override, so
the extract is actually needed there.

Matrix-column-swizzle-after-write is fixed separately in
`LowerAssignment`: `M[const_i] = col_vec` on a local matrix variable
emits `OP_VEC_INSERT` (OpCompositeInsert) so the matrix type is
preserved across the write, letting subsequent `M[i]` reads yield a
full column. The shortcut is gated on the LHS being an
`IDENTIFIER[idx]` (not a `struct.field[idx]`) to avoid the pre-
existing confusion in struct-of-array-of-matrix fields where the
IR's CoreType incorrectly collapses the array layer.

Regressions: `tests/fuzz_regressions/swizzle_on_scalar.bwsl`,
`tests/fuzz_regressions/matrix_column_swizzle_after_write.bwsl`.

### 9. Out-of-order multi-component swizzle writes (FIXED 2026-04-20)

`v.wz = float2(a, b)`, `v.xzy = …` and similar descending / shuffled
write patterns used to fall through the swizzle-pattern table in
`LowerAssignment` and end up aliasing the whole vector to the value.
Replaced the fixed table with a general parser that accepts any
combination of `x/y/z/w` or `r/g/b/a` (rejecting mixed sets), builds
a per-component fromValue map, and emits a single OpVectorShuffle.

### 10. Brace-list array initializer (FIXED 2026-04-20)

`int arr[4] = { 10, 20, 30, 40 }` reached the parser in the statement-
level variable-decl path, which called `ParseExpression` and failed
on `{`. Added a short-circuit to `ParseArrayInitializer` when the
decl is an array and the next token is `{`. Applied to both the
pass-const path and the shader-body statement path so both
`pass { const int … }` and inline locals work.

### 11. SSA phi + unreachable-block cleanup (FIXED 2026-04-20)

A loop whose body unconditionally `break`s leaves the latch block
unreachable from entry. The dominator-tree Rename pass didn't visit
it, so the phi operand slot for the latch edge stayed at the
0xFFFF initial marker (0x8000 bit set → float-constant sentinel in
the SPIR-V backend) and instructions inside the unreachable block
kept raw pre-SSA register refs. SSA now tracks which blocks the
traversal touches; a post-Rename fix-up substitutes every still-
0xFFFF phi operand with a typed undef register, and NOPs every
non-terminator instruction in unvisited blocks.

### 12. Spurious stderr debug prints (FIXED 2026-04-20)

`TryInlineFunction` printed `DEBUG: Param[%u] type mismatch` on
every candidate rejection during overload resolution, plus a wall
of `DEBUG: Pipeline has N functions: …` when a lookup bottomed
out. Both removed. Overload rejection is a normal part of
resolution; downstream error paths handle truly-unresolved names.

### 13. SSA register-type OOB on many multi-return inline calls (FIXED 2026-04-20)

`ConvertToSSA` estimated new-register capacity as
`phiCount + variableCount*4 + 16`. When a helper with N return
statements got inlined M times, SSA needed ≈ N*M fresh registers
for `{returnReg, flagReg}` defs, which often exceeded the heuristic.
Subsequent `state.AllocateNewRegister()` calls returned indices
past `registerTypes`' capacity; the type-write silently dropped
(guarded by `reg < registerCount`) and the backend fell through to
its `CoreType::FLOAT` default, emitting `OpCopyObject %float %true`
for a bool slot. Replaced the heuristic with
`sum(variables[v].definitionCount) + phiCount + sum(phi predCount) + 64`.

### 14. Scalar * matrix multiply (FIXED 2026-04-20)

`LowerBinaryOp` had matrix-on-left and vector-on-left cases for
matrix multiply but no scalar-on-left branch. `s * M` fell into
the scalar arithmetic path which emitted `OpFMul %float` on a
matrix operand. Added a `(scalar, matrix)` case that swaps
operands and emits `OP_MAT_SCALE` so scalar*matrix matches the
semantics of matrix*scalar.

### 15. Integer-vector comparison result type (FIXED 2026-04-20)

`OpSLessThan` / `OpULessThan` etc. take vector operands and must
produce a matching bvec result — the int/uint paths used a
hard-coded `GetTypeId(CoreType::BOOL)` (scalar) regardless of
operand width. `ivec3 < ivec3` validated-failed with "Expected
vector sizes of Result Type and the operands to be equal".
Ported the float-compare path's vector-width detection: peek each
operand's CoreType, pick the matching `CoreType::BOOLN`, and use
that as the comparison result type.

### 16. Nested-block variable shadowing (FIXED 2026-04-20)

`LowerBlock` iterates statements without saving / restoring the
`variableRegisters` map, so an inner-scope `{ float x = …; }`
overwrote the outer `int x`'s binding and every subsequent
reference in the outer scope resolved to the stale inner slot —
typically with a FADD-on-int or similar type-mixed failure.
LowerBlock now snapshots `variableRegisters` + `variableStructTypes`
on entry and restores them on exit. The parser's symbol table
already handled scopes correctly; only the lowering's flat map
was leaking.

### 17. Loop iterator leaks out of loop scope (FIXED 2026-04-20 third session)

A separate variant of #16: `LowerForCStyle`, `LowerForRange`, and
`LowerForCollection` snapshotted the variable map *after* lowering
the init statement / iterator binding, so the iterator's slot
remained in `variableRegisters` after the loop. An outer
`float x` followed by `for (int x = …) { … }` left the hash mapped
to the int register past the loop, and the next read of the outer
`x` came back as int while the SSA/backend still believed it was
float. Moved the snapshot to *before* init / iterator declaration
and the restore to after the entire loop block in all three
for-loop variants.

Regression: `tests/equivalence/test_loop_var_scope.bwsl`.

### 18. Matrix-scalar arithmetic — ADD/SUB/DIV (FIXED 2026-04-20 third session)

`LowerBinaryOp`'s matrix path handled only `MULTIPLY` with a scalar.
`mat OP scalar` for `+`, `-`, `/` (and the scalar-on-left forms)
fell through to the scalar arith path which emitted `OpFAdd` / etc
between a matrix and a scalar, failing SPIR-V validation. Now
splats the scalar to a column-sized vector and emits per-column
`OpFAdd` / `OpFSub` / `OpFDiv` then reconstructs the matrix via
`OP_MAT_CONSTRUCT`, symmetrically for both operand orders.

Regression: `tests/equivalence/test_matrix_scalar_arith.bwsl`.

### 19. Vector-boolean logical ops emitted bitwise / integer SPIR-V (FIXED 2026-04-20 third session)

`!`, `&&`, `||`, `!=` on `bool2/3/4` operands reached the SPIR-V
backend tagged as scalar-bool or generic-integer operations:
- `!bvec3` produced a scalar `OpLogicalNot` with a vector operand
  (validator: "Expected operand to be of Result Type").
- `bvec3 && bvec3` / `bvec3 || bvec3` fell into the bitwise path
  (`OpBitwiseAnd` on a `v3bool` result → "Expected int scalar or
  vector type as Result Type").
- `bvec3 != bvec3` hit the int-comparison path (`OpINotEqual` →
  "Expected operands to be scalar or vector int").

Fixes split across lowering and backend:
- `LowerUnaryOp::NOT` now preserves the operand's vector width
  when picking the result type (mirroring `NEGATE`).
- `SPIRVBuilder` extends its type check for `OP_AND` / `OP_OR` /
  `OP_INE` to accept `BOOL2/3/4` and dispatch `OpLogicalAnd` /
  `OpLogicalOr` / `OpLogicalNotEqual`.

Regression: `tests/equivalence/test_bool_vec_logical.bwsl`.

### 20. Matrix-typed ternary invoked OpSelect on composite (FIXED 2026-04-20 third session)

`cond ? matA : matB` reached `OpSelect` with a matrix result type,
which SPIR-V ≤ 1.3 rejects ("Expected scalar or vector type as
Result Type: Select"). Added a per-column decomposition in
`LowerExpression`'s TERNARY path, mirroring the existing struct-
ternary fix: extract each column with `OP_VEC_EXTRACT`, emit
`OP_SELECT` on the two column vectors, then rebuild via
`OP_MAT_CONSTRUCT`.

Regression: `tests/equivalence/test_ternary_matrix.bwsl`.

### 8. Determinant on mat3 produces undef (FIXED 2026-04-20)

`OP_MAT_DET` fell through to the generic 1-operand GLSL.std.450
handler which used the operand's matrix type as the result type.
Downstream consumers then failed validation with "ID has not been
defined" on the fabricated matrix-typed determinant value. Added a
dedicated case that emits `OpExtInst GLSL.std.450 Determinant` with
result type `float`.

### 5. Matrix-matrix addition lowers to OP_NOP (FIXED 2026-04-20)

Captured: `tests/fuzz_regressions/mat_mat_add_nop.bwsl`.

```bwsl
mat3 A = mat3(...);
mat3 B = mat3(...);
mat3 sum = A + B;     // lowers to `NOP r11 <-` in IR
float s = sum[0].x;   // downstream sees undef, SPIR-V validation fails
```

Symptoms: SPIR-V validation reports `Reached non-composite type while
indexes still remain to be traversed` on an OpCompositeExtract whose
source is an undefined OpNop result. Fix path: add FADD-on-matrix
(and likely FSUB/FMUL-element-wise) handling in
`bwsl_ir_lowering.h::LowerBinaryOp` and route to either
`OpFAdd` / `OpFSub` per column or a per-column MatrixTimesMatrix style
decomposition. SPIR-V's plain `OpFAdd` does accept matrix operands, so
the simplest fix is emitting it directly rather than decomposing.

Workaround: do the addition per-column:

```bwsl
mat3 sum = mat3(A[0] + B[0], A[1] + B[1], A[2] + B[2]);
```

### 4. `10.` trailing-dot decimal literal (FIXED 2026-04-20)

`Lexer::ScanNumber` now accepts a trailing dot when the next character
isn't an identifier start (member access) or another dot (range
operator). Covers `10.`, `0.`, `float3(1., 2., 3.)` while preserving
`v.x` and `0..10`.

Regression: `tests/fuzz_regressions/trailing_dot_literal.bwsl`.

Completed this session (2026-04-20 continuation)
------------------------------------------------

1.  Pointer-to-struct-field: `OP_LOCAL_FIELD_PTR` + `OpAccessChain`
    backend wiring + `STORAGE_IS_FIELD_PTR` flag for the
    `OP_LOCAL_LOAD`/`OP_LOCAL_STORE` dispatch.
2.  Recursion diagnostic at IR-lowering time; short-circuit in `bwslc`
    driver to suppress downstream validator noise.
3.  Trailing-dot float literal support in the lexer.
4.  +11 equivalence tests:
    - Bug-fix coverage: `test_pointers_struct_field`,
      `test_pointers_struct_field_control_flow`,
      `test_pointers_in_loop`, `test_ternary_struct_deep`.
    - Nested access: `test_nested_access_4level` (struct-in-struct at
      4 depths, mixed scalar / vector / int at each), `test_nested_access_matrix_field`
      (mat3 / mat4 stored as struct fields, column + element access,
      whole-column writes), `test_nested_access_array_chain`
      (dynamic index into array-of-struct, loop reduce across field
      chains, aliased write-through-index).
    - Math: `test_math_inverse_trig` (full asin/acos domain + all
      atan2 quadrants + axis cases + sin(asin)/cos(acos) round-trip),
      `test_math_exp_log_roundtrip` (exp/log/exp2/log2/pow/sqrt/rsqrt
      identities), `test_math_bitops_vec` (int3 / uint3 AND/OR/XOR/NOT,
      scalar-vec shifts, `count_bits` / `first_bit_high` /
      `first_bit_low`), `test_math_matrix_identities` (transpose
      anti-homomorphism, inverse round-trip, scalar-multiply
      distribution, associativity of matrix-vector multiply).
5.  +3 fuzz regressions from the bug-fix pass: `recursion_not_supported`,
    `trailing_dot_literal`, `mat_mat_add_nop`. Updated
    `pointer_to_struct_field` to a compile-success pattern.
6.  +8 error_cases (each with an expected error substring in
    `run_tests.py`):
    - `workgroup_id_wrong_stage`, `local_id_wrong_stage` — compute
      built-ins in non-compute stages.
    - `dereference_non_pointer` — `a ^ -1` ambiguity surfaced cleanly.
    - `recursion_not_supported` — promoted from fuzz_regressions to
      error_cases so the error-message text is enforced.
    - `const_redeclared` — "Variable already declared in this scope".
    - `compute_with_vertex_stage` — "Compute passes cannot include
      vertex/fragment stages".
    - `duplicate_compute_block` — "Only one compute block is allowed
      per pass".
    - `array_size_overflow` — "Invalid array size. Max 256k elements".
7.  +13 fuzz regressions stressing existing codegen paths:
    - `deep_swizzle_chain`, `nested_ternary_deep`,
      `pointer_reassign_loop`, `struct_field_ptr_compound`,
      `chain_fn_return_array`, `dynamic_index_write`,
      `nested_if_early_return`, `many_phi_variables`,
      `nested_loops_break_continue`, `compound_ops_all`,
      `variant_default_compute`, `enum_sumtype_method_chain`,
      `matrix_negate_column_write` — compile cleanly.
    - `swizzle_on_scalar`, `matrix_column_swizzle_after_write`,
      `bitwise_compound_assign` — pre-existing bugs discovered while
      writing tests; exit 1 cleanly and are documented as known-open.

Completed third session (2026-04-20)
------------------------------------

1.  Loop iterator scoping in all three for-loop variants
    (`LowerForCStyle` / `LowerForRange` / `LowerForCollection`).
2.  Matrix-scalar arithmetic for `+ - /` in both operand orders
    via per-column splat + reconstruct.
3.  Vector-boolean `!` preserves bvec width; backend routes
    vector `&&` / `||` / `!=` through the logical opcodes.
4.  Matrix-typed ternary decomposes to per-column `OpSelect` +
    `OP_MAT_CONSTRUCT`.
5.  +4 equivalence tests covering each of the above
    (`test_loop_var_scope`, `test_matrix_scalar_arith`,
    `test_bool_vec_logical`, `test_ternary_matrix`).

### Known remaining silent-bad-codegen patterns

Found while probing but not fixed this session:

- **Pointer-typed ternary.** `cond ? ^a : ^b` loses the pointer
  type at the ternary result. Downstream `result^` emits
  "dereference applied to a non-pointer value" or builds invalid
  IR. Deferred — needs pointer-type propagation through the
  TERNARY lowering path and possibly a dedicated `OP_PTR_SELECT`
  opcode since SPIR-V `OpSelect` with pointers requires
  `VariablePointers` capability.
- **Positional struct constructor** (`V(f3, 4.0)` for
  `struct V { float3 pos; float w; }`). Produces `OpUndef` result
  — the function-call lowering has `OP_MAT_CONSTRUCT` /
  `OP_VEC_CONSTRUCT` paths but no struct-constructor path, so
  user-struct-name calls fall through into the generic
  user-function path and emit `OP_CALL` with an unresolved
  callee. Users currently work around with per-field assignment
  (`V v; v.pos = ...; v.w = ...;`), and at least one regression
  test (`tests/fuzz_regressions/struct_copy_semantics.bwsl`)
  uses that form, so this is a new-feature gap more than a
  regression.
- **Fuzz-only silent SIGABRT under ASan.** A 120s sweep produced
  one crash (`fuzz/crashes/crash-e41e91997d91458ed78f08b1ce30013e6a670780`)
  that fires only inside the libFuzzer wrapper — `bwslc` on the
  same input exits cleanly with code 1. ASan/UBSan seem to
  detect something without flushing the report. Kept as a crash
  artifact; next session should rebuild the fuzzer with
  `-fsanitize=address,undefined -O1 -fno-omit-frame-pointer`
  and a live stderr dump to catch the actual diagnostic.

Completed previous session (for reference)
------------------------------------------

1.  Parser `ParseStruct` OOB read on duplicate module declarations (BUS).
2.  `g_customTypes` dangling-pointer UAF across compilations in fuzz /
    batch workflows.
3.  Eval expansion combinatorial explosion (nested `foreach`).
4.  `IRLowering` stack overflow in `LowerExpression` (deep AST).
5.  `IRProgram::instructionCount` uninitialized -> 49 M-byte garbage
    read in the fuzz harness's post-lowering loop.
6.  IR memory pool writing past chunk end for allocations larger than
    `CHUNK_SIZE`.
7.  SSA `IdentifyVariables` treating `OP_LOCAL_STORE` (dest=0) as a
    register definition, corrupting later uses of `r0` (thread index).
8.  `LowerMemberAccess`'s any-expression-object path fell through on
    struct fields — `fn().field` returned the whole struct.
9.  `GENERIC_V` / `GENERIC_T` / `GENERIC_U` hash collision with user
    struct names `V`, `T`, `U`.
10. Ternary with struct operands — pre-1.4 `OpSelect` doesn't take
    composites; now decomposes into per-field
    `extract / select / insert`.
11. Dynamic vector component write (`v[i] = x`) emitted
    `OpCompositeInsert` with a runtime literal; now routed through
    `OpVectorInsertDynamic` via a new `OP_VEC_INSERT_DYNAMIC` opcode
    with SSA/backend wiring.
12. Matrix negation emitted `OpSNegate` (int) on float matrices; now
    `OpMatrixTimesScalar(m, -1.0)`.
13. `isnan` / `isinf` intrinsics added (full stack: stdlib table, IR
    opcodes, IR lowering return-type inference, SPIR-V emission).
14. Variants in compute shaders — parser dropped `identifierKind` on
    clone; IR lowering had no handler for `variants.X` access.
15. `LowerUnaryOp::ADDRESS_OF` / `DEREFERENCE` OOB-indexed
    `registerStorageInfo` on constant-encoded operands.
16. `LowerFunctionCall` enum-method-call OOB-indexed
    `registerStructTypes` on constant-encoded receivers.
17. `SPIRVBuilder::GrowCurrentFunction` crashed on arena exhaustion
    (null memcpy overlap).

Testing infrastructure status
-----------------------------

### Regression suite (`tests/run_tests.sh`)

166 tests. Covers: every backend's smoke-compile, golden-file matching
across Metal/HLSL/GLSL/GLES, fuzz regressions, variant specialization
cases, shader-stage-fn inlining, pointer manipulation, compute kernels,
modules, enums (scalar + sum type), variants, intrinsics.

### Equivalence suite (`tests/run_tests.sh --equivalence`)

122 tests. Runs each compute shader through native SPIR-V, HLSL →
SPIR-V (via dxc), GLSL → SPIR-V (via glslang) and compares byte-level
output via Vulkan dispatch. Supports multi-pass pipelines with memory
barriers between compute passes.

Gaps to fill:

- **Pointer-chain-in-loop**: `ptr^` operations inside nested loops where
  the pointer itself is phi'd. Write a targeted SSA stress test.
- **Atomic ops on SSBO struct fields** (currently only shared-memory
  atomics are equiv-tested).
- **Tessellation / geometry stages** — not part of compute equivalence;
  would need raster-capture infrastructure (see multi-pass gap below).
- **Texture sampling equivalence** — needs texture binding setup in
  `equiv_runner.cpp`.
- **Ternary returning ternary**: `a ? (b ? s1 : s2) : s3` with struct
  operands (the per-field decomposition is single-level).
- **Variants with runtime inputs** — we've tested default-valued
  variants; multiple variant configurations producing different SPIR-V
  would need a runner loop over all combinations.

### Fuzzing (`fuzz/HANDOFF.md` is the detailed file)

1-hour runs currently surface 0–1 crash. Coverage has plateaued around
19,600–19,700 edges; diminishing returns on pure fuzzing for crash
hunting. Remaining value: regressions, not discovery.

Plan to expand test coverage
----------------------------

### Short-term (≤ 1 session each)

1. **Promote known-limitation workarounds into tests.** For the 3–4
   open-bug regressions, write a second test (`*_workaround.bwsl`) that
   exercises the working form so we don't accidentally break that too.
2. **Ternary-of-struct-chain-deep**: test 3-level nested ternary returning
   struct, each level exercising a different struct-shape combination.
   Our per-field decomposition is probably not nesting-safe.
3. **Pointer + loop + phi cross-product**. 6–8 tests varying:
   (ptr assigned once / reassigned) × (write in body / write after
   break) × (scalar / vector / struct pointee).
4. **Generic instantiation coverage**: every type in each constraint,
   chained via generic → generic → intrinsic. Current tests exercise a
   subset.

### Medium-term (multi-session)

5. **Raster-stage equivalence runner**. Extend `equiv_runner.cpp` to set
   up a framebuffer attachment, run a 1-triangle vertex+fragment
   pipeline, read back the color target. Unlocks real equivalence
   coverage for `variants_basic`, `multipass` (the real raster form),
   `modules_pbr_lighting`, `shader_stage_fn_*`.
6. **Texture equivalence**. Pre-bake a small texture to disk, bind as a
   sampler in `equiv_runner.cpp`, test `sample` / `sample_lod` /
   `gather`. Gates textures out of "compiles but uncovered" status.

### Long-term

7. **Semantic property tests**. Rather than hand-picked input tables,
   generate random float inputs and assert invariants (`length(v) >= 0`,
   `length(normalize(v)) ≈ 1`, `dot(normalize(a), normalize(b)) ∈
   [-1,1]`) across backends. Useful for catching drift from fast-math
   flags the user might flip on later.

Plan to make the compiler more robust
-------------------------------------

### Immediate

1. **Fix `pointer-to-struct-field`** (see bug #1 above). Medium effort,
   well-scoped.
2. **Emit a diagnostic on function recursion** (see #3). Small,
   well-scoped, removes a class of confusing validator errors.
3. **Tighten arena-exhaustion error paths.** We plugged the worst
   crashes this session, but several helpers still assume
   `arena->Allocate` succeeds. A quick audit + `if (!ptr) return` guards
   in: `bwsl_ast_common.h` (`ArenaArray::Push`), `bwsl_ast_soa.h`
   (various `Make*`), `bwsl_spirv_backend.cpp` (`Grow*` helpers not yet
   patched). Goal: no crash on OOM, just a compile error.

### Near-term

4. **Diagnostics instead of silent bad IR.** In several of this
   session's fixes, the backend logged `fprintf(stderr, "Error: ...")`
   but continued emission. Thread these through `parser.hadError`
   / return early so bwslc always exits 1 cleanly with a single
   actionable error.
5. **SSA pass: auditable opcode table.** `shouldRenameOperands` is a
   long OR chain; it's already caught us twice (OP_LOCAL_STORE's
   dest=0, OP_VEC_INSERT_DYNAMIC). Convert to a per-opcode metadata
   table (same pattern as `IR_TO_SPV_OP_TABLE`) with fields:
   `defines_register`, `operand_0_is_reg`, `operand_1_is_literal`, etc.
   Adding a new opcode then forces a deliberate entry rather than
   inheriting wrong defaults.
6. **IR dump for post-every-pass debugging.** Currently `-dump-ir`
   prints pre- and post-SSA. Add per-pass dumps (CFG, DCE, variant
   specialization, inlining) behind a flag. Massive diagnostic ROI on
   future bug hunts.

### Structural

7. **Codegen bug patterns.** Post-mortem notes on this session's fixes
   cluster around two root causes:
   - **Constant-encoded register indices leaking into storage arrays.**
     Several `MAX_REGISTERS`-sized arrays are indexed by a u16 that can
     have `0x4000` / `0x8000` / `0xC000` bits set (constant markers).
     Fix once at the indexing helper: `SafeRegIndex(u16 reg, u32 cap)`
     returning `cap - 1` with a warning instead of letting the OOB
     propagate. Then every indexing site becomes safe by default.
   - **Missing type propagation from non-identifier objects.**
     `LowerMemberAccess`'s any-expression path only handled swizzles;
     the struct-field fix this session added the missing case. Audit
     other code that branches on `access.object.Type() == IDENTIFIER`
     for the same omission pattern (array access on `fn().field`,
     method calls on `(a + b).value`, etc.).
8. **Make `CompilationContext` responsible for ALL cross-iteration
   state.** `g_customTypes` was a UAF landmine; the fix was to reset
   it in the context constructor, but a future static global will
   repeat the bug. Convert `g_customTypes` to a field of
   `CompilationContext` and pass a reference wherever it's needed.
   Similarly audit `g_additionalModuleSearchPaths`.
9. **Equivalence-runner robustness.** Currently a single failing
   backend aborts the test. Collect all backend results and report the
   full divergence matrix on failure — makes which-backend-is-wrong
   immediately obvious without re-running.

Acceptance criteria for "compiler is robust"
--------------------------------------------

- No crash on any input ≤ 32 KiB in a 4-hour fork-mode fuzz run.
- Every codegen path emits valid SPIR-V or returns exit code 1 with a
  single coherent error message. No silent bad-codegen-then-validator-
  complains paths.
- Equivalence suite reaches 150+ tests, covering at least one example
  per language feature in both a simple and a chained-combined form.
- Arena OOM is a clean compile error, never a crash.

One-liner to pick back up
-------------------------

```bash
make -j bwslc bwslc-fuzz equiv_runner && \
  ./tests/run_tests.sh && \
  ./tests/run_tests.sh --equivalence
```

If both suites are green, the best next step is one of: (a) fix
pointer-to-struct-field; (b) build the raster-stage equivalence runner;
(c) hunt the next layer of language-combination probes (see gaps above
and the end of the last-session notes in chat history).
