# BWSL - Shader Language Compiler

BWSL (Brawl Shading Language) is a shader language and compiler that generates SPIR-V and cross-compiles to Metal, HLSL, GLSL, and GLSL ES (WebGL).

## Getting Started

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/apresthus/BWSL.git

# Or if already cloned without submodules:
git submodule update --init --recursive
```

## Building

### Prerequisites

**For native CLI compiler:**
- clang++ with C++20 support
- macOS, Linux, or Windows (with appropriate toolchain)

**For WebAssembly module:**
- [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) (`emcc` must be in PATH)

### Build Commands

```bash
# Build CLI compiler (default)
make bwslc

# Build CLI compiler with debug symbols
make bwslc-debug

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
├── bwslc              # Native CLI compiler
└── wasm/
    ├── bwsl.js        # WASM JavaScript wrapper
    └── bwsl.wasm      # WebAssembly binary
```

## Usage

### CLI Compiler

```bash
# Basic usage - generates SPIR-V only
./build/bwslc shader.bwsl

# Generate all output formats
./build/bwslc shader.bwsl -all

# Specific output formats
./build/bwslc shader.bwsl -metal           # SPIR-V + Metal
./build/bwslc shader.bwsl -hlsl            # SPIR-V + HLSL
./build/bwslc shader.bwsl -glsl            # SPIR-V + GLSL 450
./build/bwslc shader.bwsl -gles            # SPIR-V + GLSL ES 300 (WebGL 2.0)

# With render config
./build/bwslc shader.bwsl -config render.rcfg -all

# Output to specific directory
./build/bwslc shader.bwsl -o output_dir/ -all

# Add module search paths
./build/bwslc shader.bwsl -modules ./modules -modules ./lib

# Compile specific pass or stage
./build/bwslc shader.bwsl -pass MainPass -stage fragment
```

### CLI Options

| Option | Description |
|--------|-------------|
| `-o <dir>` | Output directory (default: current directory) |
| `-modules <dir>` | Add module search path (can be used multiple times) |
| `-config <file>` | Render config file path |
| `-pass <name>` | Compile specific pass (default: all) |
| `-stage <name>` | Compile specific stage: vertex, fragment (default: both) |
| `-metal` | Generate Metal Shading Language output |
| `-hlsl` | Generate HLSL output |
| `-glsl` | Generate GLSL output (version 450) |
| `-gles` / `-webgl` | Generate GLSL ES output (version 300 es) |
| `-all` | Generate all output formats |
| `-v` | Verbose output |
| `-timing` | Print timing information |
| `-dump-ir` | Dump BWSL IR |
| `-debug-names` | Emit debug names in SPIR-V |
| `-no-validate` | Skip SPIR-V validation |
| `-internals` | Output SPIR-V disassembly and IR to JSON |

### WebAssembly Module

```javascript
import initBWSL from './build/wasm/bwsl.js';

const bwsl = await initBWSL();

// Get version
const version = bwsl.ccall('getVersion', 'string', [], []);

// Compile shader
const result = bwsl.ccall('compile', 'string', ['string', 'string'], [
    shaderSource,  // BWSL source code
    configSource   // Render config (optional, can be empty string)
]);        

const output = JSON.parse(result);
if (output.success) {
    // output.passes contains compiled shader data
} else {
    // output.errors contains error messages
}
```

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
├── tools/
│   ├── bwslc.cpp           # CLI compiler
│   ├── bwsl_wasm.cpp       # WASM entry point
│   └── spirv_cross_wrapper.cpp
├── vendor/                  # Git submodules
│   ├── SPIRV-Cross/        # Shader cross-compilation
│   ├── SPIRV-Tools/        # SPIR-V validation
│   ├── SPIRV-Headers/      # SPIR-V definitions
│   └── simde/              # Portable SIMD intrinsics
├── modules/                 # Standard library modules
├── tests/                   # Regression tests
└── *.cpp, *.h              # Compiler source
```

## License

Copyright (c) Alexander Presthus. All rights reserved.
