# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Build CLI compiler (default, outputs to build/bwslc)
make bwslc

# Build with debug symbols
make bwslc-debug

# Build WebAssembly module (outputs to build/wasm/)
make wasm

# Clean all build artifacts
make clean
```

**Prerequisites:** clang++ with C++20 support; Emscripten SDK for WASM builds.

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

## Architecture Overview

BWSL is a shader language compiler with this pipeline:

```
Source (.bwsl) → Lexer → Parser → AST (SoA) → IR → CFG → SSA → SPIR-V → Cross-compile
```

### Key Components

- **bwsl_lexer.{h,cpp}** - Tokenizer, produces TokenStream
- **bwsl_parser_soa.{h,cpp}** - Recursive descent parser, produces AST in SoA layout
- **bwsl_ast_soa.h** - Structure-of-Arrays AST representation (cache-optimized)
- **bwsl_ir_gen.h** - Intermediate representation with register-based instructions
- **bwsl_ir_lowering.h** - AST → IR lowering (massive file, ~180K)
- **bwsl_cfg.{h,cpp}** - Control Flow Graph construction
- **bwsl_ssa.{h,cpp}** - SSA form conversion with phi nodes
- **bwsl_spirv_backend.{h,cpp}** - SPIR-V binary generation

### Cross-Compilation

- **tools/spirv_cross_wrapper.cpp** - Wrapper around SPIRV-Cross library
- Compiled separately to avoid macro conflicts (`u32`, `f32`, `f64` in bwsl_defs.h)

### Runtime Integration

- **bwsl_compiler_service.h** - Metal-specific runtime integration
- **bwsl_compiler_service_core.h** - Platform-agnostic compiler service
- **middleware/** - Backend-specific middleware (Metal implementation)

## Build System Details

Uses **unity build** pattern: `tools/bwslc.cpp` includes all `.cpp` implementation files directly. SPIRV-Cross wrapper is compiled separately due to macro conflicts.

WASM builds use `tools/defs_wasm.h` for BSD type compatibility, but SPIRV-Cross must NOT include it (conflicts with member names).

## Vendored Dependencies (git submodules)

- `vendor/SPIRV-Cross/` - Shader cross-compilation
- `vendor/SPIRV-Tools/` - SPIR-V validation
- `vendor/SPIRV-Headers/` - SPIR-V definitions
- `vendor/simde/` - Portable SIMD intrinsics

## Standard Library Modules

Located in `modules/`: math.bwsl, noise.bwsl, Random.bwsl, PBR_module.bwsl, etc.

## Type Aliases

Defined in `bwsl_defs.h`: `u8`, `u16`, `u32`, `u64`, `s8`, `s16`, `s32`, `s64`, `f32`, `f64`
