# Program Structure

Status: `stable`

Primary implementation source:

- `phases/parser/bwsl_parser_soa.cpp`

## File Structure

BWSL source files may contain file-scope `module` and `pipeline`
declarations. Modules may live in their own files or in the same file as a
pipeline.

File-scope modules are registered before pipelines are parsed, so a pipeline
can import a module declared elsewhere in the same source file.

```bwsl
module Math {
}

pipeline Demo {
    import Math
}
```

## Pipeline Body

The parser currently accepts the following top-level declarations inside a
`pipeline` body:

- `import`
- `using`
- `attributes { ... }`
- `variants { ... }`
- `compute_graph { ... }`
- `const`
- `pass "Name" { ... }`
- `eval` declarations
- `enum`
- `struct`
- `constraint`
- top-level function declarations

Top-level declaration order is largely free.

`module` declarations are not valid inside a `pipeline` body; declare them at
file scope instead.

## Pipeline Cardinality Rules

The parser currently enforces:

- at most one `variants` block per pipeline
- at most one `compute_graph` block per pipeline

## Module Body

The parser currently accepts the following top-level declarations inside a
`module` body:

- `import`
- `using`
- function declarations
- `struct`
- `enum`
- `const`

## Imports

Imports name modules by identifier and may be comma-separated:

```bwsl
import Math
import Math, Noise
import PBR as BRDF
```

Module resolution depends on the compiler's configured module search paths.

`using ModuleName` opts an already-imported module into unqualified lookup. It
does not import the module:

```bwsl
import Math as M
using M
```

`using Alias = Type` declares a scoped type alias:

```bwsl
import PBR as BRDF
using Material = BRDF::PBRMaterial
```

## Pass Naming

Pass declarations use quoted string names:

```bwsl
pass "Main" {
}
```

This is part of the current concrete syntax, not just a documentation style.

## Out of Scope

Backend ABI packaging and engine-side binding setup are outside this source
language structure section.
