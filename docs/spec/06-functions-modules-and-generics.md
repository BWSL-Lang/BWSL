# Functions, Modules, and Generics

Status: `stable` for the current constraint-based model

Primary implementation sources:

- `phases/parser/bwsl_parser_soa.cpp`
- `core/bwsl_types.h`

## Function Declaration Form

Functions use the `name :: (params) -> return_type` form:

```bwsl
square :: (float x) -> float {
    return x * x;
}
```

## Parameter Syntax

The parser currently accepts more than one parameter spelling, including:

- C-style: `Type name`
- colon form: `name: Type`
- module-qualified types: `Module::Type name`
- anonymous parameters in some contexts

The spec should treat `Type name` as canonical for now, while recording the
other accepted spellings as implementation-defined compatibility syntax.

## Return Types

Function return types currently include:

- built-in source types
- custom and module-qualified types
- `vertex_function`
- `fragment_function`
- `compute_function`

The parser also accepts `pass_block`, which should currently be treated as
provisional.

## Function Scope

Functions can currently appear in:

- pipeline scope
- pass scope
- module scope
- enum bodies as methods

Overloading is supported and keyed by parameter types.

## Stage-Returning Functions

The language currently allows helpers that return stage blocks:

```bwsl
makeVertex :: () -> vertex_function {
    vertex {
        output.position = float4(0.0, 0.0, 0.0, 1.0);
    }
}
```

These helpers can be assigned into pass stages.

## Modules

Modules are declared with `module Name { ... }` and imported with `import Name`.
Imports may use an alias:

```bwsl
import Math as M
```

Module-qualified access uses `::`:

```bwsl
Math::square(2.0)
Math::PI
M::square(2.0)
```

`using M` makes an already-imported module available for unqualified lookup in
the current pipeline or module scope.

Module bodies currently accept imports, `using`, functions, structs, enums, and
consts.

## Constraint-Based Generics

BWSL's supported generic system is constraint-based.

Example:

```bwsl
constraint FloatVectors = float2 | float3 | float4;

scale :: (FloatVectors v, float s) -> FloatVectors {
    return v * s;
}
```

Constraints currently support unions of built-in types and previously defined
constraints.

## Type Pattern Dispatch

Generic function bodies may use type-arm dispatch:

```bwsl
componentSum :: (FloatVectors v) -> float {
    float2: v.x + v.y
    float3: v.x + v.y + v.z
    default: 0.0
}
```

Arms may be expressions or blocks.

## Unsupported Generic Syntax

The token set includes some future-facing pieces such as `where`, but the
current supported surface does not include:

- angle-bracket generic parameters like `foo<T>`
- `where` clauses
