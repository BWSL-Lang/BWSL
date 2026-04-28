# Spec Status

This document defines the status vocabulary for the BWSL spec draft and records
which parts of the language are stable enough to be treated as normative today.

## Status Vocabulary

- `stable`
  The feature is implemented, covered by tests, and intended to be part of the
  supported user-facing language.
- `provisional`
  The feature exists in the implementation, but wording, validation, or user
  guidance is still expected to change.
- `implementation-defined`
  Current behavior is real and should be documented, but it is still expressed
  in terms of compiler behavior rather than a finished language rule.
- `planned`
  Mentioned for direction only. Not part of the current language.

## Current Normative Baseline

The current baseline is the language surface implemented by:

- `bwsl_lexer.cpp`
- `bwsl_parser_soa.cpp`
- `bwsl_types.h`
- `bwsl_stdlib.h`
- `bwsl_ir_lowering.h`
- `tests/run_tests.py` and the associated test corpus

This matches the intent already stated in `docs/language.md`: the supported
surface is the one exercised by the compiler and regression suite.

## Stable Areas

The following areas are mature enough for a normative draft:

- Lexical structure and tokenization
- Core declaration syntax for pipelines, modules, structs, functions, and passes
- Scalar, vector, matrix, array, and pointer source types
- Expression grammar and operator precedence
- Control-flow statements and loop forms
- Constraint-based generics
- Enum declarations, payload enums, and pattern-arm syntax
- Shader-stage composition for `vertex`, `fragment`, and `compute`
- Built-in shader namespaces: `attributes`, `input`, `output`, `resources`
- Variant declarations and rule syntax
- The intrinsic catalog exposed through `bwsl_stdlib.h`

## Provisional Areas

The following features should be treated as implemented but still provisional:

- `compute_graph`
  The parser accepts it, but there is not yet a settled public usage document.
- `eval` value model
  `eval` execution now runs as a post-parse comptime pass, but the supported
  value domain is still intentionally narrow: scalar and vector literals,
  existing constants, enum/module constants, and variant constants.
- `pass_block`
  The parser accepts `-> pass_block`, but the public docs do not yet treat it
  as a normal user-facing feature.
- Compatibility aliases such as `float3x3`
  The implementation accepts them in type resolution and constructors; the spec
  still needs a final decision on whether they are canonical syntax or
  compatibility aliases.
- Stage validation details for some compute-oriented intrinsics
  The overall feature set is clear, but some validation still lives partly in
  lowering rather than in a clean front-end rule system.

## Implementation-Defined Areas

These behaviors should be documented as they exist today without pretending the
wording is final:

- Exact overload resolution details beyond the tested cases
- Constant folding and the remaining early substitution of already-evaluated
  constants during parsing
- Auto-assignment details for enum values in flag-like enums
- Interaction between pattern-arm lowering and compile-time evaluation
- Some legacy or convenience syntax forms accepted by the parser but not yet
  standardized in the public docs

## Planned but Non-Normative

The following are direction signals only:

- Richer compile-time data and specialization facilities
- A separate formal spec track for render-config files

## Rule for Conflicts

If `bwsl-docs`, README examples, or older notes disagree with this spec draft,
the compiler repo wins. If the compiler and the spec draft disagree, the spec
must be updated immediately or the behavior must be marked provisional here.
