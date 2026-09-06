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
make build
make build CONFIG=debug
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
make build TARGET_OS=windows TARGET_ARCH=x86_64  # requires a matching sysroot/toolchain
make wasm
```

## Testing

- Run `make test` before opening a pull request on macOS or Linux.
- On Windows, run `build.bat test` from `cmd.exe` or PowerShell.
- On macOS, `./tests/run_tests.sh --metal` is useful when you change Metal output or golden files.
- If you change compiler behavior, parser rules, IR, or backends, add or update regression tests.

For changes spanning the native backends, run both Metal-only validation and
the complete validator/equivalence suite. Metal-only mode exercises vertex
pulling; enabling GLES changes vertex-input lowering.

```bash
python3 tests/run_tests.py --metal
python3 tests/run_tests.py --all-validators --equivalence
make wasm
node tests/wasm_smoke.mjs
python3 tests/run_compiler_service_smoke.py
```

Equivalence tests require every backend by default. Add `expected_values` to
numerical regression specs so matching compiler mistakes cannot pass by agreeing
across backends. The suite labels comparisons without an independent expectation
as `differential only`. GLES validation also checks the direct emitter, with CPU
readback oracles enabled by `--equivalence`.

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

## macOS HLSL validation tools

The DXC 1.7 development build can crash on valid switch/loop shaders at its
default optimization level. Provision the pinned official DXC 1.9 SDK locally:

```sh
python3 scripts/provision_dxc_macos.py
export BWSL_DXC="$PWD/build/toolchains/dxc-1.9.0.5399/macOS/bin/dxc"
```

DXC 1.9 fixes those DXIL crashes, but its bundled SPIRV-Tools 2026.3 optimizer
crashes in `simplify-instructions` on three color-conversion round trips.
For these equivalence runs, explicitly select a standalone SPIRV-Tools 2023.2
`spirv-opt` (verified on macOS). This uses DXC `-O0` for translation followed
by standalone `spirv-opt -O`; DXIL validation keeps its normal optimization.
There is no automatic fallback or skipped backend. Both tool versions are logged.

```sh
spirv-opt --version  # verify the selected standalone optimizer
python3 tests/run_tests.py --dxc "$BWSL_DXC" --hlsl-spirv-opt "$(command -v spirv-opt)" --all-validators --equivalence
```

`BWSL_HLSL_SPIRV_OPT` supplies the same optimizer choice to focused QA scripts.
