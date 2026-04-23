# Statements and Control Flow

Status: `stable`, with `eval` details still partly `provisional`

Primary implementation source:

- `bwsl_parser_soa.cpp`

## Blocks

Blocks are delimited by braces and contain zero or more statements:

```bwsl
{
    statement;
    other_statement;
}
```

## Core Statements

The current parser accepts:

- variable declarations
- `const` declarations
- expression statements
- `if` / `else`
- `for`
- `foreach`
- `loop`
- `switch`
- `return`
- `break`
- `skip`
- `discard`
- `eval` forms

`skip` is the language's continue-like control-flow statement.

## If Statements

The parser supports:

- block bodies
- single-statement bodies without braces
- chained `else if`

## Return Variants

The current syntax surface includes:

- `return;`
- `return expr;`
- `return if (cond);`
- `return expr if (cond);`

These conditional return forms are real parser syntax and should be treated as
part of the current language.

## Break and Skip Variants

The parser supports:

- `break;`
- `break if (cond);`
- `skip;`
- `skip if (cond);`

## For and Foreach

The current parser accepts several loop families:

- C-style loops
  ```bwsl
  for (int i = 0; i < n; i++) { ... }
  ```
- range loops
  ```bwsl
  for (i in 0..n) { ... }
  for (i in 0..=n) { ... }
  for (i in 0..n by 4) { ... }
  ```
- collection loops
  ```bwsl
  for (item in values) { ... }
  for (values) { ... } // implicit `it`
  ```
- multi-range `foreach`
  ```bwsl
  foreach (x in 0..w, y in 0..h) { ... }
  ```

## Loop

The current parser accepts:

- counted loop: `loop (count) { ... }`
- indefinite loop: `loop { ... }`
- post-condition form: `loop { ... } until (cond)`

## Switch

The current parser accepts:

- `switch (expr) { ... }`
- `case` labels with one or more comma-separated values
- `default`

## Discard

`discard;` is accepted as a statement form. It is intended for fragment-stage
control flow.

## Eval Forms

The current implementation supports:

- `eval { ... }`
- `eval` variable declarations
- `eval if (...)`
- `eval for (...)`
- `eval loop (...)`
- `eval` function declarations

Important note: the README explicitly says the compile-time execution model is
expected to evolve. The syntax is stable enough to document, but some deeper
semantic wording remains provisional.
