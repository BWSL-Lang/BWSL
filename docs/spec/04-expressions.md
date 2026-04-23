# Expressions

Status: `stable`

Primary implementation source:

- `bwsl_parser_soa.cpp`

## Primary Expressions

The current expression grammar includes:

- literals
- identifiers
- parenthesized expressions
- constructor calls such as `float3(...)`
- namespace roots such as `resources`, `attributes`, `variants`, `self`

## Postfix Forms

The parser currently supports these postfix operations, from tightest-binding
upward:

- function call: `f(...)`
- member access: `value.member`
- array access: `value[index]`
- module-qualified access: `Module::member`, `Module::Enum::Variant`
- postfix increment and decrement: `x++`, `x--`
- postfix dereference: `ptr^`

## Prefix Unary Forms

The parser currently supports:

- logical not: `!x`
- unary plus: `+x`
- numeric negation: `-x`
- prefix increment and decrement: `++x`, `--x`
- bitwise not: `~x`
- address-of: `^x`

The same `^` token is therefore used in three positions:

- prefix address-of
- postfix dereference
- infix bitwise XOR

## Binary Operators

The parser's current precedence order, from tighter to looser, is:

1. postfix operations
2. unary operations
3. `*`, `/`, `%`
4. `+`, `-`
5. `<<`, `>>`
6. `<`, `<=`, `>`, `>=`
7. `==`, `!=`
8. `&`
9. `^`
10. `|`
11. `&&`
12. `||`
13. ternary `?:`
14. assignment and compound assignment

Assignment is right-associative. Ternary is also parsed right-associatively.

## Compound Assignment

The parser accepts:

- `=`
- `+=`, `-=`, `*=`, `/=`, `%=`
- `&=`, `|=`, `^=`
- `<<=`, `>>=`

Compound assignments are lowered as ordinary assignment of the corresponding
binary expression.

## Special Namespaces

Several names receive special treatment in expression parsing and lowering:

- `attributes.*`
- `input.*`
- `output.*`
- `resources.*`
- `variants.*`
- `self`

These are part of the language, not user-defined module names.

## Module and Enum Qualification

The parser currently resolves:

- module member access such as `Math::square`
- module constants such as `Math::PI`
- enum member access such as `Channels::Red`
- module-qualified enum member access such as `Module::Channels::Red`

For plain enums, member access currently resolves to the corresponding numeric
literal during front-end processing.

## Swizzles

Member-access syntax is also used for vector swizzles. The current implementation
supports:

- single-component swizzles such as `.x`, `.y`, `.z`, `.w`, `.r`, `.g`, `.b`,
  `.a`
- multi-component swizzles of length 2 to 4

Mixed `xyzw` and `rgba` naming within the same swizzle should be treated as
invalid.

## Current Known Restriction

The error suite currently treats ternary expressions with pointer operands as
unsupported.
