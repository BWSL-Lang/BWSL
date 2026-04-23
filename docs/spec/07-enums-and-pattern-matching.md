# Enums and Pattern Matching

Status: `stable`, with some auto-value behavior still `implementation-defined`

Primary implementation source:

- `bwsl_parser_soa.cpp`

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

The implementation currently assigns enum values as follows:

- explicit `= value` is used when present
- otherwise plain enums auto-number sequentially from zero
- flag-like enums auto-assign powers of two

Current note: the implementation identifies a flag-like enum using compiler
heuristics. The final wording should be refined, but the current behavior must
still be documented.

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
arguments.

## Enum Member Access

The current implementation resolves plain enum member access such as
`Channels::Red` to a literal value during front-end processing. Module-qualified
access such as `Graphics::Channels::Red` is also supported.
