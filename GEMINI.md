# BWSL Shader Language Compiler

This document provides a high-level overview of the BWSL (Brawl Shading Language) compiler project, its architecture, and development conventions.

## Project Overview

BWSL is a compiler for a custom shading language, also called BWSL. It takes `.bwsl` source files and compiles them into various shader formats, including SPIR-V, Metal, HLSL, and GLSL. The compiler is written in C++ and can be built as a native command-line tool (`bwslc`) or a WebAssembly module.

The project follows a unity build pattern, where most `.cpp` files are included directly into the main entry points (`tools/bwslc.cpp` and `tools/bwsl_wasm.cpp`). This simplifies the build process and can improve compilation times.

### Core Components

*   **Lexer (`bwsl_lexer.h`, `bwsl_lexer.cpp`):** Tokenizes the BWSL source code.
*   **Parser (`bwsl_parser_soa.h`, `bwsl_parser_soa.cpp`):** Parses the token stream and builds an Abstract Syntax Tree (AST).
*   **Intermediate Representation (IR):** The project uses a custom IR (`bwsl_ir_gen.h`, `bwsl_ir_gen.cpp`) to represent the shader logic in a backend-agnostic way.
*   **SPIR-V Backend (`bwsl_spirv_backend.h`, `bwsl_spirv_backend.cpp`):** Compiles the IR into SPIR-V, which is the primary output format.
*   **Cross-Compilation:** The project uses `SPIRV-Cross` (included as a vendor submodule) to cross-compile the generated SPIR-V into other shader languages like Metal, HLSL, and GLSL.

## Building and Running

The project uses a `Makefile` for building.

### Build Commands

*   **Build CLI compiler (default):**
    ```bash
    make bwslc
    ```
*   **Build CLI compiler with debug symbols:**
    ```bash
    make bwslc-debug
    ```
*   **Build WebAssembly module:**
    ```bash
    make wasm
    ```
*   **Clean build artifacts:**
    ```bash
    make clean
    ```

### Running the Compiler

The primary executable is `build/bwslc`.

*   **Compile a shader to all supported formats:**
    ```bash
    ./build/bwslc tests/arrays_basic.bwsl -all
    ```
*   **Compile a shader to a specific format (e.g., Metal):**
    ```bash
    ./build/bwslc tests/arrays_basic.bwsl -metal
    ```

### Running Tests

The project has a `tests` directory containing numerous `.bwsl` files for regression testing. A `run_tests.sh` script is available to run these tests.

```bash
sh tests/run_tests.sh
```

## Development Conventions

*   **Code Style:** The code generally follows a consistent style with a focus on performance. This includes the use of custom memory arenas (`bwsl_arena.h`) and a data-oriented approach in some parts of the compiler (e.g., `bwsl_parser_soa.h`).
*   **Dependencies:** External dependencies are managed as Git submodules in the `vendor/` directory.
*   **Testing:** The `tests/` directory is extensive. When fixing bugs or adding features, it is expected that new test cases will be added to this directory.
