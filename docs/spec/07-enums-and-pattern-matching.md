# Enums and Pattern Matching

Status: `stable`, with some auto-value behavior still `implementation-defined`

Primary implementation source:

- `phases/parser/bwsl_parser_soa.cpp`

## Enum Declarations

Enums use:

```bwsl
enum Name {
    Variant
}
```

An enum may optionally declare an underlying integer type:

```bwsl
enum Channels : uint {
    Red = 0b0001,
    Green = 0b0010
}
```

The current parser accepts integer and unsigned-integer underlying types.

## Payload Enums

Enums may carry associated data:

```bwsl
enum Shape {
    Sphere(float radius),
    Box(float3 size)
}
```

Payload variants are part of the supported language.

## Variant Values

- A member written with `= value` has that value.
- A member without one has the previous member's value plus one, or 0 when it
  is the first, as in C: `enum E : uint { A = 3, B, C = 7, D }` gives A = 3,
  B = 4, C = 7 and D = 8.
- Values are of the underlying type, `int` or `uint`; `uint` when none is
  given. A written value must fit the type, so negative values need `: int`,
  and a member without a value can't follow the largest value of the type.
- Two members of an enum can't have the same value: a `match` or a comparison
  couldn't tell them apart.

A flag enum writes each member's value:

```bwsl
enum Channels : uint {
    Red   = 0b0001,
    Green = 0b0010,
    Blue  = 0b0100
}
```

## Enum Methods

Enum methods use the ordinary function syntax inside an enum body:

```bwsl
enum Shape {
    Sphere(float radius),

    eval area :: () -> float {
        Sphere(r): 3.14159 * r * r
    }
}
```

Current parser rules:

- `eval` must precede a method declaration if present
- methods do not declare `self` as a parameter
- `self` is available implicitly inside the body

## Pattern-Match Bodies

Enum methods may use arm syntax without an explicit `match` keyword:

```bwsl
Sphere(r): r
default: 0.0
```

Pattern arms support:

- variant-only arms
- variant-with-bindings arms
- wildcard `_` in bindings
- `default`
- expression bodies
- block bodies

## Pattern Matching in Functions

The same arm syntax is also used in ordinary functions that dispatch on enum
arguments. An implicit arm body matches the function's first enum parameter;
other parameters remain available in every arm.

```bwsl
score :: (Mode mode, float scale) -> float {
    A: scale
    B: { return scale * 2.0; }
}
```

Ordinary pattern functions require each variant to be covered once or a final
`default` arm. A payload arm may omit all bindings or name every payload field,
using `_` for ignored fields. Block arms must return a value on every path.
Only the selected arm executes, including its side effects.

## Enum Member Access

The current implementation resolves plain enum member access such as
`Channels::Red` to a literal value during front-end processing. Module-qualified
access such as `Graphics::Channels::Red` is also supported.
