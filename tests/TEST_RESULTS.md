# BWSL Test Report

Snapshot from a local run of:

```bash
python3 tests/run_tests.py
```

on April 9, 2026.

## Summary

- `129 passed`
- `0 failed`
- `2 skipped`

## Skipped

- `modules_basic`
- `modules_structs`

These are skipped because they are module files, not pipeline entry files. The
test runner treats files starting with `module` as support files rather than
standalone compile targets.

## Coverage Highlights

The current regression suite covers:

- graphics passes, multipass pipelines, and source-declared resources
- compute shaders, workgroup sizes, shared memory, barriers, atomics, and wave operations
- structs, arrays, matrix and vector construction, swizzles, and member access
- overloads, scoped functions, inline returns, and stage-function selection
- modules, module-qualified access, and engine-style imported libraries
- constraint-based generics and type-pattern dispatch
- enums, flag enums, payload enums, and enum `eval` methods
- pointer syntax and pointer control flow
- storage buffers, sampled textures, storage images, and texture writes

For the current state, rerun `python3 tests/run_tests.py`.
