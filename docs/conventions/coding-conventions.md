# Coding conventions

TLDR: The codebase is data-oriented: data lives in plain structs (frequently
SoA), operations live in free functions inside namespaces, and all allocation
routes through the arena. Prefer that style when adding code.

The TLDR is the direction for new code. It holds most strictly in the hot
back-end passes (CFG, SSA, SPIR-V). The parser, diagnostics, AST reference
index and CLI tools are looser: they use `std::` containers and structs with
member functions. When editing those, match the surrounding code rather than
rewriting it. Strings are the exception: new code everywhere uses
`ArenaString` (see [Types](#types)).

## Layout and build

- All compiler code lives under `src/`. Phases are in `src/phases/<phase>/`,
  shared building blocks in `src/core/`, and executables (`bwslc.cpp`,
  `bwsl_wasm.cpp`, `bwslc_fuzz.cpp`, `equiv_runner.cpp`) directly in `src/`.
- **Unity build.** `src/bwslc.cpp` and `src/bwsl_wasm.cpp` each `#include`
  every implementation `.cpp` they need. A new `.cpp` must be added to both
  lists (the WASM build deliberately leaves out some, e.g. the GLES backend).
  `src/core/bwsl_unity.cpp` is a stale leftover; don't use it as a reference.
- **`spirv_cross_wrapper.cpp` is a separate translation unit.** Never include
  SPIRV-Cross headers from unity-built code; BWSL's short type names (`u32`,
  `f32`, ...) collide with SPIRV-Cross identifiers. Talk to it through the
  `spirv_cross_wrapper` function declarations instead.
- **`.inl` shards** split large components into reviewable files: the
  parser (`bwsl_parser_soa_*.inl`), IR lowering (`bwsl_ir_lowering_*.inl`) and
  the SPIR-V backend (`bwsl_spirv_backend_*.inl`). Every shard starts with:

  ```cpp
  // Part of bwsl_spirv_backend.cpp. Include from that file only.
  // <one line saying what this shard contains>
  #pragma once
  #include "bwsl_spirv_backend.cpp"
  ```

  The trailing include of the parent is for IDE/clangd context only. Add the
  shard to the parent's include list.
- **Generated code.** `src/core/bwsl_embedded_modules.generated.h` is
  generated from `modules/` by `scripts/gen_embedded_modules.py`. Don't edit
  it by hand. `make` regenerates it when a listed module changes; `build.bat`
  regenerates it every time. Commit the regenerated header with the module
  change. A new bundled module must be added to both `MODULES` in the script
  and `EMBEDDED_MODULE_SOURCES` in the `Makefile`.

## Includes

- Use `#pragma once` in every header.
- Include project headers by their path relative to `src/`, e.g.
  `#include "phases/ir_generation/bwsl_ir_gen.h"`. Headers in the same
  directory may use the bare file name. Don't add new `../` includes.
- Vendor headers are relative to `vendor/` (also on the include path), e.g.
  `#include "SPIRV-Headers/include/spirv/unified1/spirv.hpp"`.
- Order: project headers first, then vendor headers, then standard headers.

## Naming

| Kind | Style | Example |
|------|-------|---------|
| Files | `bwsl_` prefix, snake_case | `bwsl_symbol_table.h` |
| Types (struct, class, enum) | PascalCase | `SymbolTableData`, `NodeRef` |
| Functions and methods | PascalCase | `SymbolTable::Init`, `ReportErrorAt` |
| Variables, parameters, members | camelCase | `nodeCount`, `namePosition` |
| Constants and table names | SCREAMING_SNAKE | `MAX_REGISTERS`, `IR_TO_SPV_OP_TABLE` |
| Macros | SCREAMING_SNAKE | `BWSL_LIKELY`, `PARSE_BINARY_OP` |
| Padding fields | `_pad` / `_padding` | `u8 _pad[3];` |
| Data structs paired with a namespace | `<Name>Data` | `SymbolTableData` + `namespace SymbolTable` |

- Function names start with a verb. Within a phase, use that phase's verb:
  `Parse*` in the parser, `Lower*` in IR lowering, `Emit*` in the backends.
  - Predicates start with `Is`, `Has` or `Uses`: `IsBranchOp`,
    `HasSideEffects`.
  - Factories start with `Make`: `ASTFactory::MakeIdentifier`.
  - Conversions are named `XToY`: `CoreTypeToString`, `CrossCompileToMetal`.
  - Accessors use `Get` by default. A cheap accessor with no side effects
    may be a plain noun instead: `MessageName`, `NodeTypeName`.
- Everything lives in `namespace BWSL`, with nested namespaces per area
  (`BWSL::IR`, `BWSL::SSA`, `BWSL::GLES`). Write it as `namespace BWSL {`.
- Prefer `enum class` for new enums. Value names are SCREAMING_SNAKE for
  compiler-internal kinds (`SymbolKind::FUNCTION`). Existing plain enums
  (`TokenType`, `IR::OpCode`) are deliberate, as they're used as compact
  indices; don't convert them.

## Types

- Use the aliases from `core/bwsl_defs.h`: `u8`–`u64`, `s8`–`s64`, `f32`,
  `f64`. Use `size_t` only for sizes passed to allocators and the standard
  library.
- Refer to AST nodes with `NodeRef` (a packed type + index), not pointers.
  Prefer indices over pointers in general, so data stays relocatable and
  compact.
- **Strings are `ArenaString`s.** Use them in place of `std::string` for
  identifiers, names and any other string a struct stores or a function
  takes or returns, in every part of the compiler, the CLI and tools
  included.
  - Make one with `ArenaString::Make` (text in a source buffer) or
    `MakeHashOnly` (generated text, e.g. `"ub_" + name`). Both intern the
    text, so a generated name and a source identifier with the same
    spelling compare equal.
  - Compare by the interned `nameHash` (`==` does that), never by string
    contents. An `ArenaString` has no empty state; return a `bool` and
    write it through an out-parameter instead (`bool FindX(..., ArenaString*
    out)`), or keep a bitmask of which slots of an array are set.
  - Get text only at the edges, where something has to be written out: an
    `OpName`, GLSL text, JSON, or a diagnostic message. `ToString()` without
    a source buffer looks the text up by hash and is always correct.
    `view(sourceBase)` and `ToString(sourceBase)` need the buffer the string
    came from, which isn't the main file for names declared in a module.
- `std::string` is only for those edges: building a message or output text,
  or a temporary while composing a name to intern. Existing code still uses
  `std::string` widely (`RenderConfig`, reflection, the CLI). Don't convert
  it as part of unrelated work; convert at the boundary instead
  (`ArenaString::MakeHashOnly(config.name)`).

## Data layout

- **Plain data structs, operations in free functions.** A struct holds data;
  a namespace of the same name (minus `Data`) holds functions that take a
  pointer to it, with an `Init` function instead of a constructor:

  ```cpp
  struct SymbolTableData { ... };

  namespace SymbolTable {
      inline void Init(SymbolTableData* table, BWSL_Arena* arena);
  }
  ```

  The large phase drivers (`Parser`, `IRLowering`, `SPIRVBuilder`, `Lexer`)
  are structs or classes with member functions. Extending them with more
  members is fine; don't add new class hierarchies. There are no virtual
  functions in `src/`, and new code shouldn't introduce any.
- **SoA for hot data.** The AST stores each node kind in its own pool of
  small, padded `*Data` structs, addressed by `NodeRef`. Create nodes
  through `ASTFactory::Make*`, not by pushing to the pools directly.
- **Lookup tables** are `constexpr`/`static const` arrays indexed by an
  enum, e.g. `IR_TO_SPV_OP_TABLE` (built by a `constexpr` function) and the
  intrinsic tables in `bwsl_stdlib.h`. Data-driven tables beat long `switch`
  chains for new mappings.
- Hot arrays may use `alignas(64)` (see `SPIRVBuilder`); don't add it
  speculatively.

## Memory

- **Arena first.** `BWSL_Arena` is a bump allocator: `Initialize(mem, cap)`,
  `Allocate(size, align)`, `Reset()`. It doesn't own its buffer, and on
  exhaustion it returns `nullptr`, so check allocations that can be large.
- **Growable arrays:** `ArenaArray<T>` with `Init(arena, capacity)` and
  `Push(arena, item)`. Growth abandons the old block inside the arena, so
  size the initial capacity sensibly for big arrays.
- **IR memory** comes from `IRMemoryPool` (`core/bwsl_mem_pool.h`), created
  per stage and passed into `IRLowering::Initialize`.
- **Avoid in new compiler code:** raw `new`/`delete`, `malloc` (except for
  arena backing buffers), `std::string` in place of `ArenaString` (see
  [Types](#types)), and `std::` containers in CFG/SSA/SPIR-V. `std::vector`
  is acceptable in the CLI, diagnostics, AST JSON/reference index and other
  code that isn't on the hot path.

## Errors and diagnostics

- **Report errors; don't throw.** All diagnostics go into the
  `DiagnosticStream` (`core/bwsl_diagnostics.h`). The GLES backend's
  exceptions are legacy; don't add new `throw`s. RTTI isn't used.
- **Always attach a source location.**
  - Parser: `Parser::ErrorAt(token, message)`. It also sets `panicMode`,
    and `Synchronize()` recovers at the next statement or declaration.
  - IR lowering: `ReportErrorAt(node, message)` with the offending
    `NodeRef`. Plain `ReportError` has no location; avoid it in new code.
  - An error that would otherwise surface as a SPIR-V validation failure
    ("Id is 0", "Type Id ... is not ...") should be caught earlier and
    reported at the user's source.
- **Error codes.** Each `DiagnosticMessageId` maps to a fixed code
  (`BWSL1000` parse, `BWSL1300` lowering, `BWSL1502` validation, ...). Free-
  text parse, comptime and lowering messages also get a hash of the message
  text appended (`BWSL1300-7B997123`), so rewording a message changes its
  code. A new typed diagnostic needs entries in `DiagnosticMessageId`,
  `MessageName`, `TemplateFor` and `MessageCodeBase`.
- **Write messages for shader authors:** say what's wrong in language terms
  and, where possible, what to do instead (e.g. "use `uint` and
  `flag != 0u`").
- Prefer small result enums over `bool` when a caller needs to know why
  something failed (see `AddConstraintResult` in the symbol table).

## Adding an intrinsic

Intrinsics are defined by parallel, position-indexed lists in
`core/bwsl_stdlib.h`, and nothing checks that they agree:

1. Add the value to `enum class Intrinsic`, before `COUNT`.
2. Add the matching row to `BACKEND_NAMES[]` at the same position.
3. Add an `INTRINSIC_FIXED`, `INTRINSIC_VAR` or `TEXTURE_INTRINSIC` entry to
   `INTRINSICS[]`.
4. If it needs custom lowering, add a `CUSTOM_IMPLS` entry and a
   `case Intrinsic::X` in `bwsl_ir_lowering_calls.inl`.

## Formatting

- Use 4-space indentation everywhere, with K&R braces, `T* p` and indented
  `case` labels.
- Some files still use 2 spaces, `T *p` and unindented `case`: parts of the
  SPIR-V backend and IR lowering, and `equiv_runner.cpp`. Write new code in
  them with 4 spaces too. Convert a whole file in a separate, formatting-only
  commit rather than mixing reformatting into a functional change.

## Comments

- Mark major sections with a banner:

  ```cpp
  // ============= Resource bindings =============
  ```

- Comment why, not what: invariants, limits and non-obvious choices (e.g.
  why `MAX_REGISTERS` is 8192). Short trailing comments on struct members
  are encouraged for anything whose meaning isn't obvious from the name.
- Document `#if`/`#ifdef` blocks with what enables them and why.
- No Doxygen; the code doesn't use `///` or `/** */`.

## Tests

- Every bug fix gets a small regression test; see `CONTRIBUTING.md` for the
  suites.
- An error case (`tests/error_cases/*.bwsl`) only runs once it's registered
  in `ERROR_CASE_TESTS` in `tests/run_tests.py` with a substring of the
  expected message. Unregistered files are silently skipped. Tests match
  message text, not `BWSL####` codes.
- Runtime-semantics fixes belong in `tests/equivalence/` with a JSON
  expectation, so every backend is checked.
