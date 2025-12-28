# BWSL Test Report

Run: `./tests/run_tests.sh`

Summary: 28 passed, 4 failed, 2 skipped.

## High (missing or rejected core features)

- compute_basic: `compute` stage not accepted in pass body; parse errors cascade.
- compute_workgroups: `compute` stage not accepted in pass body.
- functions_overloading: function redefinition reported; overloading not supported.

## Medium (keyword/identifier restriction)

- edge_cases: `self` rejected as a variable name (reserved keyword).

## Low (not executed)

- modules_basic: skipped (module file).
- modules_structs: skipped (module file).

## Passed (compiled + validated)

- constants_folding
- control_if_else
- control_ternary
- functions_basic
- functions_scoping
- intrinsics_bit
- intrinsics_exp_trig
- intrinsics_math
- loops_for_cstyle
- loops_range
- loops_special
- matrices_intrinsics
- multipass
- operators_arithmetic
- operators_assignment
- operators_comparison
- operators_logical
- operators_unary
- shader_io
- ssa_basic
- ssa_complex
- ssa_control_flow
- ssa_loops
- type_conversions
- types_basic
- types_matrices
- vectors_operations
- vectors_swizzle
