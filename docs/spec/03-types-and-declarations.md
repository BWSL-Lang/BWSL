# Types and Declarations

Status: `stable` with a few `provisional` compatibility aliases

Primary implementation sources:

- `phases/lexing/bwsl_token_defs.h`
- `core/bwsl_types.h`
- `phases/parser/bwsl_parser_soa.cpp`

## Canonical Source Types

The current source language includes these built-in scalar, vector, and matrix
type spellings:

- `bool`
- `int`, `int2`, `int3`, `int4`
- `uint`, `uint2`, `uint3`, `uint4`
- `float`, `float2`, `float3`, `float4`
- `mat2`, `mat3`, `mat4`
- `void`

The source language also includes resource-facing built-in type names such as:

- `texture2D`
- `texture3D`
- `textureCube`
- `texture2DArray`
- `sampler`

User-defined nominal types include:

- `struct` types
- `enum` types
- module-qualified custom types such as `Module::Type`

## Type Aliases

Scoped type aliases use `using Alias = Type`:

```bwsl
import PBR as BRDF
using Material = BRDF::PBRMaterial
```

Aliases may target built-in types, local custom types, or module-qualified
custom types. They are resolved before declaration type checking and do not
create a new nominal type.

## Variable Declarations

The language supports ordinary variable declarations and `const` declarations.

Current `const` rules include:

- `const` declarations require an initializer
- top-level and pass-level `const` values are parsed and may be compile-time
  evaluated when possible
- module-level `const` declarations currently require literal initializers

## Arrays

Fixed-size arrays are part of the current language surface.

Examples:

```bwsl
float[4] weights;
mat4[64] bones;
```

Current array-size rules:

- sizes must be compile-time values
- integer literals are accepted
- already-evaluated compile-time constants are accepted where supported by the
  parser
- the implementation currently rejects sizes above 256k elements

## Structs

Struct declarations are accepted at pipeline scope and module scope.

Current field forms include:

```bwsl
struct Light {
    float3 position;
    float intensity;
    mat4[64] bones;
}
```

Array sizes are attached to the type. The legacy postfix declarator form
`Type name[N]` is not part of the source grammar.

## Pointers

Pointer types are part of the current source language and use `^`:

- pointer type: `Type^`
- address-of: `^value`
- dereference: `ptr^`

Examples:

```bwsl
int x = 42;
int^ p = ^x;
int y = p^;
```

## Declaration Notes

The parser accepts a few declaration conveniences that the public docs do not
fully standardize yet. In particular:

- parameter declarations support more than one concrete syntax form
- some already-evaluated constants are still substituted during parsing for
  legacy constant-expression paths

Those details are documented further in later sections and should be treated as
implementation-defined until the spec wording hardens.
