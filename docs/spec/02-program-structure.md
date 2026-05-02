# Program Structure

Status: `stable`

Primary implementation source:

- `phases/parser/bwsl_parser_soa.cpp`

## File Kinds

BWSL source files are currently divided into two top-level kinds:

- `pipeline` files
- `module` files

```bwsl
pipeline Demo {
}

module Math {
}
```

## Pipeline Body

The parser currently accepts the following top-level declarations inside a
`pipeline` body:

- `import`
- `attributes { ... }`
- `variants { ... }`
- `compute_graph { ... }`
- `const`
- `pass "Name" { ... }`
- `eval` declarations
- `enum`
- inline `module`
- `struct`
- `constraint`
- top-level function declarations

Top-level declaration order is largely free.

## Pipeline Cardinality Rules

The parser currently enforces:

- at most one `variants` block per pipeline
- at most one `compute_graph` block per pipeline

## Module Body

The parser currently accepts the following top-level declarations inside a
`module` body:

- `import`
- function declarations
- `struct`
- `enum`
- `const`

## Imports

Imports name modules by identifier and may be comma-separated:

```bwsl
import Math
import Math, Noise
```

Module resolution depends on the compiler's configured module search paths.

## Pass Naming

Pass declarations use quoted string names:

```bwsl
pass "Main" {
}
```

This is part of the current concrete syntax, not just a documentation style.

## Inline Modules

The pipeline parser accepts inline `module` declarations. These are useful for
tests and local organization, but the main user-facing model remains separate
module files.

## Out of Scope

Backend ABI packaging and engine-side binding setup are outside this source
language structure section.
