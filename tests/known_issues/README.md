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
stores into `float[4] w` fields were dropped (the old path stored into
an extracted copy); these are fixed by the same change.

Coverage: `tests/unsorted/structs_vector_matrix_array_fields.bwsl`
(mat4/float4/float element arrays, constant and dynamic indices) and
`tests/unsorted/arrays_const_sized.bwsl`.

Follow-up gaps are also fixed: writes through an element of a struct-array
field now extract the element, insert the changed member, insert the element
back into the parent struct, and store the parent. Whole-array reads of struct
fields preserve the source struct/field metadata, so inlined functions that
receive `s.field` index through `OP_STRUCT_ARRAY_EXTRACT` instead of
materializing a standalone array value.

Regression coverage: `tests/equivalence/test_nested_access_array_chain.bwsl`
now checks both `s.items[i].weight = x` write-through and passing `s.items`
to a function as `Cell[4]`, with expected numeric output in
`tests/equivalence/test_nested_access_array_chain.json`.

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

## 6. ~~Conditional loop break after a switch emits an undefined condition ID~~ (FIXED)

When every arm of a loop-body switch ended in a terminator (`break`/`skip`),
the switch merge block became unreachable, and a conditional loop `break`
following the switch failed SPIR-V validation with
`ID '[%N]' has not been defined`. The SSA unreachable-block cleanup in
`phases/ssa/bwsl_ssa.cpp` NOPed out the instruction computing the break
condition but kept the `OP_BRANCH` terminator, which still referenced the
now-undefined raw register. A switch in a loop without a following break was
fine, and a loop break without such a switch was fine — only the combination
broke.

Fixed in the same cleanup pass by pointing the condition/selector operand of
kept `OP_BRANCH`/`OP_SWITCH` terminators at a typed undef register (the
backend emits a properly defined `OpUndef` for it). The same investigation
found that phis placed in those unreachable blocks kept an *uninitialized*
result register, which could collide with a renamed register and produce
`Id N is defined more than once`; they now get a fresh result register.

Coverage lives in `tests/unsorted/loop_break_after_switch.bwsl`
(all-arms-terminating switch + `if (...) break;`, a second switch inside the
unreachable region + `break if`, the `skip if` form, and a reachable-merge
control case).

## 7. ~~`return` in a loop-body switch case breaks SPIR-V block structure~~ (FIXED)

A `return` (or `discard`) inside a switch case in a loop body, with any
statement after the switch, failed SPIR-V validation with
`Branch must appear in a block`. `LowerSwitch`
(`phases/ir_lowering/bwsl_ir_lowering_control.inl`) unconditionally emits each
case's `OP_JUMP` to the switch merge, so a case body ending in `OP_RET` is
followed by a dead jump. `CFGBuilder::FindLeaders`
(`phases/control_flow/bwsl_cfg.cpp`) started a new block after every
terminator *except* `OP_RET`/`OP_DISCARD`, so the dead jump shared a block
with the return and the backend emitted `OpBranch` directly after `OpReturn`
with no `OpLabel` between them. Every loop form (for/while/range/`loop`) was
affected; the same case body ending in `break`/`skip` was fine because those
terminators already started a new block.

Fixed by marking the instruction after `OP_RET`/`OP_DISCARD` as a leader too.
The dead jump becomes its own unreachable block, which the existing SSA
unreachable-block cleanup (issue 6) already handles.

Coverage lives in `tests/unsorted/loop_switch_case_return.bwsl` (return in a
middle case and in the default arm across all four loop forms, `discard` in a
fragment-stage case, and a no-loop control case).
