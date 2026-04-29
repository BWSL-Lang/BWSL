# BWSL Specification Draft

This directory is the canonical specification draft for BWSL as implemented in
this repository.

The intent is to keep the spec next to the compiler, semantic analysis, and
regression suite so language changes can be reviewed together with parser and
backend changes.

## Normative Priority

Until BWSL has versioned language releases, the normative priority is:

1. Documents in `docs/spec/`
2. Compiler implementation in this repository when the spec is incomplete
3. Regression and error tests in `tests/`

The public docs site in `bwsl-docs` is useful reference material, but it is
non-normative. It may simplify syntax, omit edge cases, or lag implementation.

## Main Sources

- Lexical and token surface: `bwsl_token_defs.h`, `bwsl_lexer.cpp`
- Syntax and AST construction: `bwsl_parser_soa.cpp`, `bwsl_parser_soa.h`
- Core type model: `bwsl_types.h`
- Intrinsics and stage metadata: `bwsl_stdlib.h`
- Built-ins and shader I/O semantics: `bwsl_ir_lowering.h`
- Conformance coverage: `tests/run_tests.py`, `tests/*.bwsl`,
  `tests/error_cases/*.bwsl`

## Document Map

- [00-status.md](00-status.md)
- [01-lexical-structure.md](01-lexical-structure.md)
- [02-program-structure.md](02-program-structure.md)
- [03-types-and-declarations.md](03-types-and-declarations.md)
- [04-expressions.md](04-expressions.md)
- [05-statements-and-control-flow.md](05-statements-and-control-flow.md)
- [06-functions-modules-and-generics.md](06-functions-modules-and-generics.md)
- [07-enums-and-pattern-matching.md](07-enums-and-pattern-matching.md)
- [08-pipelines-passes-and-shader-io.md](08-pipelines-passes-and-shader-io.md)
- [09-intrinsics-and-builtins.md](09-intrinsics-and-builtins.md)
- [10-conformance.md](10-conformance.md)

## Scope

This draft covers the BWSL source language. Pipeline resources are declared in
source with `resources { ... }` blocks and selected per pass with
`use resources { ... }`.

## Editing Rule

When adding or changing language behavior:

1. Update the relevant spec section.
2. Update the compiler.
3. Add or adjust regression tests.

If those three cannot land together, the new behavior should be called out as
provisional in [00-status.md](00-status.md).
