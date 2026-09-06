# Lexical Structure

Status: `stable`

Primary implementation sources:

- `phases/lexing/bwsl_token_defs.h`
- `phases/lexing/bwsl_lexer.cpp`

## Character Classes

Identifiers use an ASCII-only lexical rule in the current lexer:

- identifier start: `A-Z`, `a-z`, `_`
- identifier continuation: identifier start plus `0-9`

Unicode identifiers are not currently part of the language.

## Whitespace

The lexer ignores spaces, tabs, carriage returns, and newlines except where they
separate tokens or affect source locations.

## Comments

The current lexer accepts:

- line comments: `// ...`
- block comments: `/* ... */`

Block comments are nestable in the current implementation.

## String Literals

String literals are delimited by double quotes:

```bwsl
"Main"
"Shadow"
```

Backslash escapes are lexically accepted. The lexer also accepts newlines inside
string literals until a closing quote is found; that behavior is currently
implementation-defined rather than a polished language feature.

## Numeric Literals

The current lexer accepts:

- decimal integers: `42`
- unsigned integers: `42u`
- hexadecimal integers: `0xFFu`
- binary integers: `0b1010`
- decimal floats: `1.0`, `10.`, `0.25`
- scientific notation: `1e3`, `2.5e-4`
- optional float suffix: `1.0f`

Integer spellings must contain a complete value in the 32-bit unsigned payload
range `0`–`4294967295`; a `u`/`U` suffix selects `uint`, while unsuffixed
payloads are interpreted as signed 32-bit integers. Leading zeroes remain
decimal (`010` is ten). Empty radix prefixes, invalid radix digits and values
outside that payload range are rejected. Floating literals must round to a
finite float32 value; overflow is rejected, subnormals are accepted, and values
below the representable range may round to zero.

Two lexical ambiguities are handled in favor of existing language syntax:

- `0..10` tokenizes as `NUMBER`, `DOT_DOT`, `NUMBER`
- `value.x` tokenizes as member access, not a floating literal

## Keywords and Reserved Words

The token set includes keywords for:

- declarations such as `pipeline`, `module`, `struct`, `enum`, `constraint`
- shader constructs such as `pass`, `vertex`, `fragment`, `compute`
- control flow such as `if`, `else`, `for`, `foreach`, `loop`, `switch`
- compile-time features such as `eval`, `variants`, `rules`, `require`,
  `conflict`

Some tokens exist for syntax that is not part of the supported surface today,
such as `where`. Presence in the token set does not by itself make a feature
normative.

## Punctuation and Operators

The lexer recognizes, among others:

- grouping and indexing: `(` `)` `{` `}` `[` `]`
- separators: `,` `;`
- member and range punctuation: `.` `..` `..=`
- namespace and function punctuation: `::` `->`
- ternary punctuation: `?` `:`
- arithmetic, logical, bitwise, shift, and assignment operators
- prefix/postfix `++` and `--`

## Decorators

Attribute decorators are lexed through `@name` tokenization. The currently
documented attribute decorators are `@compressed(...)` and `@instance`.
