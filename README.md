# BWSL - Shader Language Compiler

BWSL (Brawl Shading Language) is a graphics and compute shader language with a compiler that generates SPIR-V and cross-compiles to Metal, HLSL, GLSL, and GLSL ES (WebGL).

[Public Docs Here](https://www.bwsl.dev) 

## Getting Started

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/BWSL-Lang/BWSL.git

# Or if already cloned without submodules:
git submodule update --init --recursive
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for contributor setup, test expectations,
and pull request guidelines. See [CHANGELOG.md](CHANGELOG.md) for notable
language and tooling changes.

## Building

### Prerequisites

`make build` always compiles with `clang -target <triple>`; a native build is
just the case where `TARGET_OS`/`TARGET_ARCH` equal the host (see
[Build Commands](#build-commands) below). That means the package
requirements split into two groups: what you need to build *for the host*,
and what you additionally need to build *for another OS/architecture*.

**Native build (host == target):**
- macOS or Linux: `clang++` (or another C++20 compiler), plus CMake for the linked SPIRV-Tools validator
- Windows: Visual Studio Build Tools / Developer Command Prompt (`cl.exe`) for `build.bat`, or an MSYS2 CLANG64 environment for `make` (see below), plus CMake for the linked SPIRV-Tools validator

The default `make build` and `build.bat` builds link the vendored SPIRV-Tools
library so `-validation auto` runs in-process. Use
`make build USE_LINKED_SPIRV_TOOLS=0` or set `USE_LINKED_SPIRV_TOOLS=0` before
running `build.bat` to skip that library and fall back to external
`spirv-val`/`spirv-dis` tools.

**Cross-compilation** (`make build TARGET_OS=... TARGET_ARCH=...` with a
target other than the host):
- Always clang + lld, no extra build tool beyond what native builds already need.
- The matching sysroot/toolchain for the *target* must additionally be on
  `PATH` (e.g. mingw-w64 when targeting Windows). SPIRV-Tools is
  cross-compiled too, so in-process validation still works on cross builds.
  See the per-OS package lists below for what to install.

**For WebAssembly module:**
- [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) (`emcc` must be in PATH)

#### Windows 

On windows, it's recommended to use an [MSYS2](https://www.msys2.org/) CLANG64 env. Be careful to start the actual CLANG64 env after installing (it defaults to UCRT64).

Install dependencies for a native build:

     pacman -S mingw-w64-clang-x86_64-clang make mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-python mingw-w64-clang-x86_64-emscripten mingw-w64-clang-x86_64-sccache mingw-w64-clang-x86_64-vulkan-devel

#### macOS

Native build (Xcode's bundled clang already supports C++20):

    xcode-select --install
    brew install cmake

`make bwslc-sanitize` / `make bwslc-fuzz` prefer Homebrew's LLVM when present
(for ASan/UBSan/libFuzzer runtime support):

    brew install llvm

Cross-compilation, additionally:

| Target | Package |
|--------|---------|
| `TARGET_OS=windows` | `brew install mingw-w64` (x86_64/i686 only; ARM64 Windows needs building mingw-w64 from source) |
| `TARGET_OS=linux` | `brew install messense/macos-cross-toolchains/x86_64-unknown-linux-gnu` (or the `aarch64-unknown-linux-gnu` variant for `TARGET_ARCH=arm64`) |
| `TARGET_OS=macos`, other `TARGET_ARCH` | Nothing extra -- Xcode Command Line Tools already cover both architectures |

#### Ubuntu

Native build:

    sudo apt install clang lld cmake

Cross-compilation, additionally:

| Target | Package |
|--------|---------|
| `TARGET_OS=windows` | `sudo apt install mingw-w64` (x86_64/i686 only; ARM64 Windows needs a newer mingw-w64 built from source) |
| `TARGET_OS=linux`, other `TARGET_ARCH` | `sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu libc6-dev-arm64-cross` (swap to the `x86-64-linux-gnu`/`amd64-cross` packages for the reverse direction) |
| `TARGET_OS=macos` | Not packaged -- requires [osxcross](https://github.com/tpoechtrager/osxcross) built manually against an Apple SDK extracted from Xcode |

### Build Commands

```bash
# Build CLI compiler for the host (default: release)
make build

# Build with debug symbols
make build CONFIG=debug

# Cross-compile for another OS/architecture (clang -target under the hood;
# see Prerequisites above for the toolchain each target needs)
make build TARGET_OS=windows TARGET_ARCH=x86_64
make build TARGET_OS=linux TARGET_ARCH=arm64 CONFIG=debug

# Build the native Windows CLI compiler with MSVC
# Run this from a Visual Studio Developer Command Prompt
build.bat
build.bat bwslc-debug

# Build WebAssembly module
make wasm

# Build WASM with debug info
make wasm-debug

# Clean all build artifacts
make clean
```

### Build Output

All build artifacts go into the `build/` directory:

```
build/
├── bwslc                       # Native CLI compiler on macOS/Linux
├── bwslc.exe                   # Native CLI compiler on Windows
├── bwslc-debug[.exe]           # CONFIG=debug build
├── bwslc-<os>-<arch>[.exe]     # Cross-compiled build, e.g. bwslc-windows-x86_64.exe
└── wasm/
    ├── bwsl.js        # WASM JavaScript wrapper
    └── bwsl.wasm      # WebAssembly binary
```

On Windows, the repo now also includes `build.bat` and `make.bat`. `build.bat` bootstraps the MSVC environment automatically, and `make bwslc` from `cmd.exe` or PowerShell will fall back to the local `make.bat` shim if `make.exe` is not installed.

## Usage

### CLI Compiler

```bash
# Basic usage - generates SPIR-V only
./build/bwslc shader.bwsl

# Generate all output formats
./build/bwslc shader.bwsl -all

# Specific output artifacts. Cross-compiled artifacts still use generated SPIR-V internally.
./build/bwslc shader.bwsl -metal           # Metal
./build/bwslc shader.bwsl -hlsl            # HLSL
./build/bwslc shader.bwsl -glsl            # GLSL 450
./build/bwslc shader.bwsl -gles            # GLSL ES 300 (WebGL 2.0)
./build/bwslc shader.bwsl -gles -spv       # GLSL ES + emitted SPIR-V sidecars

# Output to specific directory
./build/bwslc shader.bwsl -o output_dir/ -all

# Add module search paths
./build/bwslc shader.bwsl -modules ./modules -modules ./lib

# Compile specific pass or stage
./build/bwslc shader.bwsl -pass MainPass -stage fragment

# Compile a compute stage only
./build/bwslc shader.bwsl -pass ComputePass -stage compute

# Generate GLSL ES directly from BWSL IR
./build/bwslc shader.bwsl -gles-direct

# Check diagnostics without writing shader outputs
./build/bwslc shader.bwsl -check -errors-json

# Check unsaved editor text from stdin while reporting against a real source path
./build/bwslc -check --stdin --source-file shader.bwsl -errors-json < shader.bwsl

# Batch mode: pass multiple files, a directory (compiles every .bwsl found
# recursively, mirroring subdirectories under -o), or a manifest. Every unit
# is compiled and reported together instead of bailing on the first failure,
# module files are read once and shared across the whole batch, and
# -errors-json emits one aggregated document with a per-file breakdown.
./build/bwslc a.bwsl b.bwsl c.bwsl -check
./build/bwslc shaders/ -o build/shaders -metal
./build/bwslc -manifest shaders.txt -check -errors-json   # one path per line, # comments
```

### CLI Options

| Option | Description |
|--------|-------------|
| `-o <dir>` | Output directory (default: current directory) |
| `-modules <dir>` | Add module search path (can be used multiple times) |
| `-pass <name>` | Compile specific pass (default: all) |
| `-stage <name>` | Compile specific stage: vertex, fragment, compute (default: all) |
| `-spv` | Write generated SPIR-V files alongside requested artifacts; this is the default artifact when no format is requested |
| `-metal` | Generate Metal Shading Language output via SPIR-V |
| `-hlsl` | Generate HLSL output via SPIR-V |
| `-glsl` | Generate GLSL output via SPIR-V (version 450) |
| `-gles` / `-webgl` | Generate GLSL ES output via SPIR-V (version 300 es) |
| `-gles-direct` | Generate GLSL ES directly from BWSL IR |
| `-all` | Generate all output formats |
| `-check` | Run diagnostics without writing shader outputs |
| `--stdin` | Read BWSL source from stdin |
| `--source-file <path>` | Source path used for diagnostics and module resolution with `--stdin` |
| `-v` | Verbose output |
| `-timing` | Print timing information |
| `-dump-ir` | Dump BWSL IR |
| `-debug-names` | Emit debug names in SPIR-V |
| `-validation auto\|strict\|off` | Control SPIR-V validation (`auto` is default; linked native builds validate in-process) |
| `-no-validate` | Alias for `-validation off` |
| `-internals` | Output SPIR-V disassembly and IR to JSON |

### WebAssembly Module

```javascript
import initBWSL from './build/wasm/bwsl.js';

const bwsl = await initBWSL();

// Get version
const version = bwsl.ccall('getVersion', 'string', [], []);

// Compile shader
const result = bwsl.ccall('compile', 'string', ['string', 'string', 'string'], [
    shaderSource,               // BWSL source code
    configSource || '',         // Render config source (optional)
    '-source-file shader.bwsl -internals -modules ./modules' // Optional flags
]);

const output = JSON.parse(result);
if (output.success) {
    // output.shaders contains pass-keyed shader data
    // output.files contains downloadable shader files, e.g. shader.vert / shader.frag
    // Add -spv to include emitted shader.vert.spv / shader.frag.spv sidecars
} else {
    // output.errors contains error messages
}

// Get symbols for autocomplete / editor tooling
const symbolsJson = bwsl.ccall('getSymbols', 'string', ['string', 'string', 'string'], [
    shaderSource,
    configSource || '',
    '-modules ./modules'
]);
const symbols = JSON.parse(symbolsJson);
```

Supported WASM flags:

- `-internals`
- `-modules <path>` (repeatable)

See [docs/language.md](docs/language.md) for the language reference.

## Language Overview

BWSL is organized around `pipeline` blocks containing passes, helper declarations, and optional imports. The current language surface includes:

- Graphics and compute passes.
- Pipeline attributes plus `use attributes { ... }` pass declarations.
- Attribute decorators such as `@compressed(...)` and `@instance`.
- `input`, `output`, `attributes`, and `resources` access patterns.
- Vertex and compute built-ins including `input.vertex_id`, `input.instance_id`, `input.global_id`, `input.local_id`, `input.workgroup_id`, `input.num_workgroups`, and `input.local_index`.
- Structs, fixed-size arrays, array indexing, and module-qualified custom types.
- Module files with `module Foo { ... }`, `import Foo`, and `Foo::member` access.
- Function overloading and pass-scoped helper functions.
- Constraint-based generic functions such as `constraint FloatVectors = float2 | float3 | float4;`.
- Type-pattern dispatch in constrained functions.
- Enums with explicit underlying integer types, flag-style values, payload variants, and `eval` methods.
- Compile-time control flow and expansion via `eval if`, `eval for`, `eval loop`, and `eval { ... }` blocks.
- Pointer syntax using `^` for address-of and dereference.
- Shader-stage composition with `vertex = someStageFunc()`, compile-time ternary stage selection, pass-stage reuse via `"OtherPass".vertex`, and `fragment = null` for depth-only passes.
- Compute features including workgroup sizes, `shared` memory, `barrier()`, atomics, storage image writes, and wave/subgroup intrinsics.

Current examples for these features live in `tests/`, especially:

- `tests/compute_basic.bwsl`
- `tests/compute_workgroups.bwsl`
- `tests/atomic_operations.bwsl`
- `tests/wave_operations.bwsl`
- `tests/modules_basic.bwsl`
- `tests/generics_basic.bwsl`
- `tests/generics_type_pattern.bwsl`
- `tests/enums_sumtype_sdf.bwsl`
- `tests/pointers_basic.bwsl`

### Notes

- In `attributes { ... }`, the first declared attribute must be `position`.
- Module files are regular `.bwsl` files that begin with `module` instead of `pipeline`.
- Angle-bracket generic parameter syntax and `where` clauses are not part of the current supported surface. Constraint-based generics are the active form.
- `eval { ... }` is expanded by a post-parse comptime pass before IR lowering. It supports compile-time locals, compile-time control flow, statement emission, and hygienic runtime shadowing of visible comptime names.

## Planned Changes

BWSL's compile-time features now use a dedicated comptime pass for `eval`
execution and expansion. Longer-term work is still moving toward richer
Zig-like comptime data and specialization.

Near-term planned work:

- Broaden compile-time values beyond scalar/vector literals and existing enum/module/variant constants.
- Add compile-time parameters and const-generic style specialization.
- Continue tightening diagnostics and conformance coverage for nested eval scopes, loop expansion, and budget limits.

## Resources Overview

Resources are declared directly in BWSL source with pipeline-level
`resources { ... }` blocks and imported into passes with `use resources { ... }`.

## Example Shader

```bwsl
pipeline Demo {
    attributes {
        position: float3
        texcoord: float2
    }

    pass "Main" {
        use attributes { position, texcoord }

        vertex {
            output.position = float4(attributes.position, 1.0);
            output.uv = attributes.texcoord;
        }

        fragment {
            float3 color = float3(input.uv, 0.5);
            output.color = float4(color, 1.0);
        }
    }
}
```

## Project Structure

```
bwsl/
├── Makefile                 # Build system
├── build/                   # Build outputs
├── core/                    # Shared compiler types, arenas, AST, services
├── phases/
│   ├── lexing/              # Token definitions, token stream, lexer
│   ├── parser/              # SoA parser and parser slices
│   ├── evaluation/          # Compile-time/eval helpers
│   ├── ir_generation/       # IR, analysis, compute graph
│   ├── ir_lowering/         # AST-to-IR lowering
│   ├── control_flow/        # CFG construction
│   ├── ssa/                 # SSA conversion and verification
│   └── backends/
│       ├── spirv/           # SPIR-V backend
│       └── gles/            # Direct GLES backend
├── tools/
│   ├── bwslc.cpp            # CLI compiler
│   ├── bwsl_wasm.cpp        # WASM entry point
│   └── spirv_cross_wrapper.cpp
├── vendor/                  # Git submodules
├── modules/                 # Standard library modules
└── tests/                   # Regression tests
```

## Attribution

If you use BWSL in a game, engine, tool, article, or presentation, a short
credit such as `Uses BWSL by Alexander Presthus` is appreciated. This is a
request, not a condition of the license.

## License

BWSL is licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE)
and [NOTICE](NOTICE).

Project name usage guidance is in [NAME_USAGE.md](NAME_USAGE.md).
