# Statements and Control Flow

Status: `stable`, with `eval` details still partly `provisional`

Primary implementation source:

- `phases/parser/bwsl_parser_soa.cpp`

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
- while loops
  ```bwsl
  while (cond) { ... }
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

A range evaluates and captures its start, end and step once on entry, in that
order. The default step is one. Positive steps visit increasing values and
negative signed steps visit decreasing values; a runtime-computed step follows
the same rule as a literal. `..` excludes the endpoint and `..=` includes it
when reached. A zero step performs no iterations. A step whose direction does
not approach the endpoint likewise performs no iterations. Changing the
original bound or step variables in the body does not change the captured
range. Iteration stops if its next addition would wrap the iterator's integer
type, including at an inclusive type-limit endpoint.

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
- `eval while (...)`
- `eval` function declarations

Eval syntax is parsed into AST markers and evaluated by a dedicated
post-parse comptime pass before IR lowering. The parser is responsible for
syntax and AST construction only; it does not execute eval blocks, unroll eval
loops, substitute eval locals, or emit expanded runtime statements.

The comptime pass owns:

- eval scopes and compile-time bindings
- execution of eval statements and control flow
- emitted runtime statement lists
- diagnostics for invalid compile-time execution
- expansion and clone budgets

IR lowering must not receive `eval` blocks or eval-marked control-flow nodes.
Leaving one behind is a compiler error.

### Eval Blocks

`eval { ... }` executes at compile time. Statements that are purely compile-time
state, such as eval declarations and assignments to eval bindings, affect only
the comptime environment. Runtime statements emitted from the block are cloned
into the surrounding runtime block after any visible comptime bindings have
been substituted.

Compile-time scopes are block-local. Runtime declarations are allowed to shadow
visible comptime bindings without accidentally substituting that runtime name.

### Eval Declarations and Assignments

`eval` variable declarations require an initializer:

```bwsl
eval int taps = 4;
```

The initializer must evaluate to a compile-time value and must be compatible
with the declared type. Assignments retain that declared type and use the same
compatibility rules: integers can promote to float, nonnegative signed integers
can convert to uint, and uint values within the signed range can convert to int.
Other scalar kinds and vector component types or widths must match; use an
explicit constructor for a conversion. Assignments to visible eval bindings are
executed by the comptime pass:

```bwsl
eval int sum = 0;
sum = sum + 1;
```

The current comptime value domain is limited to scalar and numeric vector literals
(`float2/3/4`, `int2/3/4`, and `uint2/3/4`) plus
existing enum, module, and variant constants. Richer comptime data is planned
but not currently part of the language.

### Eval Functions

Pipeline-scope eval functions can be called with compile-time arguments from eval
initializers or runtime expressions. Their result is substituted into emitted code.

```bwsl
eval taps :: (int quality) -> int {
    return quality * 2 + 1;
}
```

Function execution supports typed local declarations, assignments, scalar
increments/decrements, if statements, range loops, counted loops, and while loops.
Returns stop execution immediately, including within loops. Arguments and return
values must match their declared types using the eval assignment rules above.
Nested calls and recursion share the execution budgets and have a maximum call
depth of 128. An executed path without a return value is an error. Statements
that require runtime execution are rejected inside an evaluated function.

### Eval If

`eval if (condition)` requires `condition` to be a compile-time value. The
selected branch is executed or emitted by the comptime pass. A condition that
depends on runtime shader data is rejected.

Example:

```bwsl
eval if (taps > 2) {
    sum += 1.0;
}
```

Invalid example:

```bwsl
bool runtime_cond = attributes.position.x > 0.0;
eval if (runtime_cond) {
    sum += 1.0;
}
```

### Eval For

`eval for` range bounds and step values must be compile-time integers. The loop
is unrolled by executing the body once per compile-time iteration, with the
iterator bound as a compile-time integer in a loop-local scope.

```bwsl
eval for (i in 0..4) {
    sum += float(i);
}
```

C-style `for` loops are not supported inside eval contexts.

### Eval Loop

`eval loop (count)` requires a compile-time integer count. `eval loop { ... }
until (condition)` requires `condition` to be a compile-time value on each
iteration. Infinite eval loops without an `until` condition are rejected.

The pass enforces expansion budgets for executed statements, emitted
statements, loop iterations, cloned nodes, and clone depth.
