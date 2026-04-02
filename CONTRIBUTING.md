# Contributing

Thanks for contributing to BWSL.

## Getting Set Up

Clone the repository with submodules:

```bash
git clone --recurse-submodules https://github.com/apresthus/BWSL.git
```

If you already cloned it without submodules:

```bash
git submodule update --init --recursive
```

## Building

On macOS or Linux:

```bash
make bwslc
make bwslc-debug
make test
```

On Windows:

```bat
build.bat bwslc
build.bat bwslc-debug
build.bat test
```

`build.bat` will locate and initialize MSVC automatically when possible.

Optional builds:

```bash
make bwslc-win-zig
make wasm
```

## Testing

- Run `make test` before opening a pull request on macOS or Linux.
- On Windows, run `build.bat test` from `cmd.exe` or PowerShell.
- On macOS, `./tests/run_tests.sh --metal` is useful when you change Metal output or golden files.
- If you change compiler behavior, parser rules, IR, or backends, add or update regression tests.

## Patch Guidelines

- Keep changes focused. Prefer one behavior change or refactor per pull request.
- Do not edit `vendor/` unless the pull request is explicitly updating a vendored dependency.
- Keep generated build products and shader outputs out of commits.
- If you change output formats or code generation intentionally, explain the change in the pull request and update any affected golden files.

Generated artifacts that should not be committed include:

- `build/`
- `tests/output/`
- `*.spv`
- `*.metal`
- `*.hlsl`
- `*.glsl`
- `*.gles`
- `*_pass*.json`
- `*.internals.json`

## Coding Notes

- BWSL uses a unity-build style for the CLI compiler: `tools/bwslc.cpp` includes the implementation units directly.
- `tools/spirv_cross_wrapper.cpp` is compiled separately to avoid macro conflicts with BWSL type aliases.
- Prefer small regression tests for bug fixes.

## Licensing

By submitting a contribution to BWSL, you agree that your contribution is
licensed under the Apache License, Version 2.0, unless you explicitly state
otherwise before submission.
