# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Build CLI compiler (default, outputs to build/bwslc)
make bwslc

# Build with debug symbols
make bwslc-debug

# Build native Windows CLI compiler with MSVC
make bwslc-msvc

# Cross-compile Windows CLI compiler from macOS/Linux with Zig
make bwslc-win-zig

# Windows helper without GNU Make
build.bat
build.bat bwslc-debug

# Build WebAssembly module (outputs to build/wasm/)
make wasm

# Run regression tests
make test

# Clean all build artifacts
make clean
```

**Prerequisites:** macOS/Linux: clang++ (or another C++20 compiler); Windows: Visual Studio Build Tools / Developer Command Prompt (`cl.exe`); Zig for `bwslc-win-zig`; Emscripten SDK for WASM builds.

## CLI Usage

```bash
./build/bwslc shader.bwsl              # SPIR-V only
./build/bwslc shader.bwsl -all         # All output formats
./build/bwslc shader.bwsl -metal       # SPIR-V + Metal
./build/bwslc shader.bwsl -hlsl        # SPIR-V + HLSL
./build/bwslc shader.bwsl -glsl        # SPIR-V + GLSL 450
./build/bwslc shader.bwsl -gles        # SPIR-V + GLSL ES 300 (WebGL)
./build/bwslc shader.bwsl -modules ./modules -v  # Add module path, verbose
```

## Repository Layout

```
core/      Shared types, arena/mem-pool, symbol table, module cache, registries,
           reflection, compiler service, middleware
phases/    The compilation pipeline, one directory per phase
tools/     Executables and external-library wrappers (bwslc, fuzzer, equiv runner,
           SPIRV-Cross wrapper, WASM entry point)
modules/   Bundled standard library shader modules (math, noise, PBR, etc.)
tests/     Regression, equivalence, error-case, golden, fuzz-regression, and
           engine-style test corpora plus the Python test runner
fuzz/      libFuzzer corpus, dictionary, and run logs
vendor/    Git-submoduled third-party dependencies
docs/      Language reference and spec drafts
```

## Architecture Overview

BWSL is a shader language compiler with this pipeline:

```
Source (.bwsl)
  → Lexer            (phases/lexing)
  → Parser           (phases/parser)        ─→ AST (SoA)
  → Eval / Comptime  (phases/evaluation)
  → IR generation    (phases/ir_generation) ─→ register-based IR
  → IR lowering      (phases/ir_lowering)
  → CFG              (phases/control_flow)
  → SSA              (phases/ssa)
  → SPIR-V backend   (phases/backends/spirv)
  → Cross-compile    (SPIRV-Cross → Metal/HLSL/GLSL,
                      phases/backends/gles for GLSL ES / WebGL)
```

### Phase Layout (phases/)

- **lexing/** — `bwsl_lexer.{h,cpp}`, `bwsl_token_defs.h`, `bwsl_token_stream.h`. Tokenizer producing a SoA `TokenStream`.
- **parser/** — `bwsl_parser_soa.{h,cpp}`. Recursive-descent parser. Emits the SoA AST.
- **evaluation/** — `bwsl_eval_soa.{h,cpp}`, `bwsl_eval_cache.cpp`, `bwsl_comptime_interpreter.{h,cpp}`. Compile-time evaluation and `eval { ... }` block expansion.
- **ir_generation/** — `bwsl_ir_gen.{h,cpp}`, `bwsl_ir_analysis.{h,cpp}`, `bwsl_compute_graph.{h,cpp}`. The register-based IR data structures and analyses, plus compute-pipeline graph construction.
- **ir_lowering/** — `bwsl_ir_lowering.h` plus `*.inl` shards (`_calls`, `_control`, `_core`, `_expr`, `_lvalues`, `_types`). AST → IR lowering, split across `.inl` files included from the header to keep each file reviewable.
- **control_flow/** — `bwsl_cfg.{h,cpp}`. Control-flow graph construction over the IR.
- **ssa/** — `bwsl_ssa.{h,cpp}`, `bwsl_ssa_verify.cpp`. SSA conversion with phi insertion and renaming, plus a verifier pass.
- **backends/spirv/** — `bwsl_spirv_backend.{h,cpp}`. SPIR-V binary emission. Driven by an opcode table (`IR_TO_SPV_OP_TABLE`).
- **backends/gles/** — `bwsl_gles_backend.{h,cpp}`. Direct GLSL ES emission for WebGL targets where SPIRV-Cross's GLSL output is insufficient.

### Core (core/)

Shared building blocks consumed by every phase:

- `bwsl_defs.h` — Type aliases (`u8`–`u64`, `s8`–`s64`, `f32`, `f64`).
- `bwsl_arena.h`, `bwsl_mem_pool.h` — Bump-arena allocator and per-type pools. Almost all compiler memory routes through these.
- `bwsl_types.h`, `bwsl_ast_common.h`, `bwsl_compiler_types.h` — Shared type/AST primitives (e.g. `CoreType`, `ArenaString`, `NodeRef`).
- `bwsl_symbol_table.h` — Scope, overload, and module-aware symbol resolution.
- `bwsl_module_cache.{h,cpp}`, `bwsl_module_integration.h`, `bwsl_common_modules.h` — Module loading and caching.
- `bwsl_custom_type_registry.{h,cpp}`, `bwsl_cast_registry.{h,cpp}`, `bwsl_variant_system.{h,cpp}`, `bwsl_variant_analysis.{h,cpp}` — User-type and variant specialization machinery.
- `bwsl_stdlib.h` — Built-in intrinsic function table.
- `bwsl_reflection_json.h`, `bwsl_resource_reflection.h`, `bwsl_render_config.h` — Reflection and pipeline metadata emitted alongside generated shaders.
- `bwsl_compiler_service.h`, `bwsl_compiler_service_core.h`, `middleware/` — Embeddable runtime compiler services. `bwsl_compiler_service.h` is the Metal-flavored entry point; `bwsl_compiler_service_core.h` is platform-agnostic.

### Cross-Compilation

- `tools/spirv_cross_wrapper.cpp` — Wrapper around the SPIRV-Cross library, compiled separately from the unity build to avoid macro conflicts (`u32`, `f32`, `f64` defined by `bwsl_defs.h` collide with SPIRV-Cross member names).

### Tools (tools/)

- `bwslc.cpp` — CLI entry point. Acts as the unity-build aggregator; `#include`s every implementation `.cpp` from `core/` and `phases/`.
- `bwslc_fuzz.cpp` — libFuzzer harness.
- `equiv_runner.cpp` — Vulkan-based equivalence runner: compiles a `.bwsl` through every backend and dispatches them on the GPU to compare results byte-for-byte.
- `bwsl_wasm.cpp`, `defs_wasm.h`, `render_config_wasm.h`, `wasm_build/` — Emscripten entry point and supporting headers for the WASM build.

## Build System Details

Uses **unity build** pattern: `tools/bwslc.cpp` directly `#include`s every implementation `.cpp` from `phases/` and `core/`. SPIRV-Cross wrapper is compiled as a separate translation unit due to macro conflicts.

WASM builds use `tools/defs_wasm.h` for BSD type compatibility, but SPIRV-Cross must NOT include it (conflicts with member names).

## Testing

```bash
# Run the regression suite
make test
# or
python3 tests/run_tests.py

# Equivalence suite (requires Vulkan + dxc + glslang)
./tests/run_tests.sh --equivalence
```

Test categories under `tests/`:

- top-level `*.bwsl` — regression / smoke tests
- `equivalence/` — multi-backend GPU equivalence tests
- `error_cases/` — inputs that must fail with a specific diagnostic
- `ast_json/` — fixtures whose `-ast-json` node positions are asserted
- `fuzz_regressions/` — minimized inputs from past fuzzer crashes
- `golden/` — frozen Metal output for diffing
- `from_engine/`, `prod_shaders/` — engine-derived shaders
- `variant_errors/`, `performance/`, `ideal/` — targeted suites

## Vendored Dependencies (git submodules)

- `vendor/SPIRV-Cross/` — Shader cross-compilation
- `vendor/SPIRV-Tools/` — SPIR-V validation
- `vendor/SPIRV-Headers/` — SPIR-V definitions
- `vendor/simde/` — Portable SIMD intrinsics

## Standard Library Modules

Located in `modules/`: `math.bwsl`, `noise.bwsl`, `Random.bwsl`, `PBR_module.bwsl`, etc.

## Type Aliases

Defined in `core/bwsl_defs.h`: `u8`, `u16`, `u32`, `u64`, `s8`, `s16`, `s32`, `s64`, `f32`, `f64`.

## Coding Style

See [CODING_STANDARDS.md](CODING_STANDARDS.md). The codebase is data-oriented: data lives in plain structs (frequently SoA), operations live in free functions inside namespaces, and all allocation routes through the arena. Prefer that style when adding code.
