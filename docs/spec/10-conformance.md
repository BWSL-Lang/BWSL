# Conformance

Status: `stable`

Primary conformance driver:

- `tests/run_tests.py`

## Purpose

The regression suite in this repository is the closest thing BWSL currently has
to an executable conformance suite.

The suite already covers:

- successful parsing and compilation for a broad surface area
- expected frontend and semantic failures
- variant reflection and specialization
- IR and backend translation expectations
- fuzz-found regressions

## Main Conformance Buckets

### Positive Feature Coverage

The `tests/*.bwsl` corpus covers the current supported language surface,
including:

- arrays, structs, and type conversions
- functions and overloads
- generics and type-pattern dispatch
- enums and payload enums
- pointers
- passes, shader stage assignment, and multipass behavior
- pass-block instantiation, interface mapping, and parameter substitution
- compute features, shared memory, atomics, and waves

### Error Cases

`tests/error_cases/*.bwsl` documents intentional rejections, including:

- invalid intrinsic arity
- unknown imports
- stage misuse of built-ins
- unsupported pointer ternaries
- recursion rejection
- duplicate compute blocks
- oversized arrays

### Variant Semantics

`tests/run_tests.py` contains explicit expectations for:

- declared variants
- implicit `has_<attribute>` variants
- specialization results
- illegal variant combinations

### Backend Expectations

The suite also checks selected IR/SPIR-V/HLSL/GLSL/Metal output properties for
specific tests.

## Editing Rule

If a language feature is added or materially changed:

1. update the relevant section in `docs/spec/`
2. add or update a `.bwsl` test
3. add or update an error test when applicable
4. add backend expectation checks when the feature materially affects lowering

## Current Gap

The existing tests are already a strong baseline, but they are not yet organized
as a formal versioned conformance suite. That should come later, after the spec
draft settles.
