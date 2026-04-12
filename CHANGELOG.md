# Changelog

All notable changes to BWSL should be documented in this file.

## Unreleased

### Added

- Added `eval { ... }` blocks for compile-time statement expansion inside shader bodies.
- `eval { ... }` expands into ordinary AST blocks before IR lowering, so the rest of the compiler pipeline continues to operate on standard statements.
- Inside an `eval` block, the compiler now supports:
  - compile-time locals from `const` declarations
  - compile-time locals from explicit `eval` declarations
  - compile-time assignment to those locals
  - compile-time `if`
  - compile-time range / collection `for`
  - compile-time `loop`
  - emission of ordinary runtime statements with compile-time substitutions applied

### Syntax

```bwsl
vertex {
    float sum = 0.0;

    eval {
        const int taps = 4;
        eval int accumulator = 0;

        if (taps > 2) {
            sum += 1.0;
        }

        for (i in 0..taps) {
            accumulator = accumulator + i;
            sum += float(i);
        }

        loop (2) {
            accumulator = accumulator + 1;
        }

        sum += float(accumulator);
    }

    output.position = float4(sum, 0.0, 0.0, 1.0);
}
```

### Notes

- The current implementation is intentionally conservative and parser-driven. It is a step toward a fuller comptime system, not the final architecture.
- `eval { ... }` is currently executed as a compile-time expansion pass during parsing, rather than by a dedicated comptime interpreter subsystem.
- Runtime declarations inside an `eval` block should not currently shadow a visible compile-time binding with the same name.
