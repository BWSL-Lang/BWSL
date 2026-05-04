# BWSL Coding Standards

This document describes the coding standards, paradigms, and patterns used throughout the BWSL compiler codebase.

## Table of Contents

1. [Guiding Philosophy: Data-Oriented Design](#guiding-philosophy-data-oriented-design)
2. [Architecture](#architecture)
3. [Naming Conventions](#naming-conventions)
4. [Code Organization](#code-organization)
5. [Data Structures - Structure of Arrays (SoA)](#data-structures---structure-of-arrays-soa)
6. [Memory Management](#memory-management)
7. [Error Handling](#error-handling)
8. [Type System](#type-system)
9. [Commenting Style](#commenting-style)
10. [Include Patterns](#include-patterns)
11. [Macro Usage](#macro-usage)
12. [Performance Optimizations](#performance-optimizations)
13. [Common Code Patterns](#common-code-patterns)

---

## Guiding Philosophy: Data-Oriented Design

BWSL is written in a **data-oriented** style. The compiler processes large
homogeneous batches of data — millions of tokens, AST nodes, IR instructions,
SPIR-V words — and the codebase is shaped around that fact rather than around
classical OO encapsulation.

The five rules that follow from this philosophy show up everywhere in the
code, and new code is expected to respect them:

1. **Data is plain.** Components are `struct`s containing only data members.
   No virtual methods, no constructors with side effects, no hidden lifetime
   contracts. A struct is an inert layout description; reading the struct
   tells you exactly what it owns.

2. **Operations are free functions in namespaces.** Logic lives in
   `namespace Component { void DoThing(ComponentData* d, ...); }`, not in
   methods on `ComponentData`. This separates *what the data is* from
   *what gets done with it*, makes the call site explicit about which data
   is being touched, and keeps types small and copyable.

3. **Prefer Structure-of-Arrays over Array-of-Structs.** Hot data — AST
   nodes, IR instructions, SPIR-V IDs — is stored in parallel arrays so a
   pass that only touches one field walks contiguous memory. The
   `_soa` filename suffix marks the major SoA datastores
   (`bwsl_ast_soa.{h,cpp}`, `bwsl_eval_soa.{h,cpp}`, etc.).

4. **All allocation goes through the arena.** Long-lived state lives in a
   `BWSL_Arena` per compilation. There is no `new`/`delete` and no
   per-component lifetime to track — when the compilation ends, the arena
   is reset and everything goes away at once. Per-type pools and
   `ArenaArray<T>` are layered on top.

5. **Identifiers, not pointers, into pools.** Cross-references between
   components are 4-byte indices into SoA pools (`NodeRef`, `TokenRef`,
   IR register IDs, SPIR-V IDs), not raw pointers. This keeps data dense,
   serializable, and stable across pool growth.

Concrete patterns enforcing each of these rules are documented in the
sections below. When a new component doesn't fit one of these patterns,
that's worth a comment explaining *why* — most exceptions in the current
codebase are at FFI boundaries (SPIRV-Cross, libFuzzer, the Metal
middleware) where the data-oriented layout meets a third-party API.

---

## Architecture

The compiler is organized as a pipeline of passes, with one directory per
phase under `phases/` and shared building blocks under `core/`. Each phase
has its own data type (`TokenStream`, `AST`, `IRProgram`, `CFG`, etc.) that
the next phase consumes.

```
Source (.bwsl)
  → phases/lexing          → TokenStream  (SoA)
  → phases/parser          → AST          (SoA)
  → phases/evaluation        comptime / eval-block expansion over the AST
  → phases/ir_generation   → IRProgram    (SoA, register-based)
  → phases/ir_lowering       AST → IR (split across *.inl shards)
  → phases/control_flow    → CFG
  → phases/ssa             → SSA-form IR
  → phases/backends/spirv  → SPIR-V words
  → phases/backends/gles     direct GLSL ES emission for WebGL targets
  → tools/spirv_cross_wrapper.cpp → Metal / HLSL / GLSL
```

`core/` contains the shared infrastructure every phase depends on:

- `bwsl_arena.h`, `bwsl_mem_pool.h` — arena allocator and per-type pools
- `bwsl_defs.h`, `bwsl_types.h`, `bwsl_ast_common.h` — shared scalar and
  AST primitives (`CoreType`, `ArenaString`, `NodeRef`)
- `bwsl_symbol_table.h` — scope/overload/module-aware symbol resolution
- `bwsl_module_cache.{h,cpp}`, `bwsl_module_integration.h` — module loading
- `bwsl_custom_type_registry.{h,cpp}`, `bwsl_variant_system.{h,cpp}`,
  `bwsl_cast_registry.{h,cpp}` — user-type and variant machinery
- `bwsl_stdlib.h` — built-in intrinsic table
- `bwsl_reflection_json.h`, `bwsl_resource_reflection.h`,
  `bwsl_render_config.h` — reflection and pipeline metadata emitted
  alongside generated shaders
- `bwsl_compiler_service.h`, `bwsl_compiler_service_core.h`,
  `middleware/` — embeddable runtime compiler service

`tools/` holds the binaries: `bwslc.cpp` (CLI, also the unity-build
aggregator), `bwslc_fuzz.cpp`, `equiv_runner.cpp`, `bwsl_wasm.cpp`, and
`spirv_cross_wrapper.cpp`.

---

## Naming Conventions

### Variables

| Context | Convention | Examples |
|---------|------------|----------|
| Local variables | camelCase or snake_case | `current`, `previous`, `varName`, `type_id` |
| Struct/class members | camelCase | `nameHash`, `sourceOffset`, `componentCount`, `currentScope` |
| Loop counters | Short names | `i`, `j`, `b`, `s` |
| Padding fields | `_pad` suffix | `u8 _pad[3];` |

```cpp
// From bwsl_symbol_table.h
struct Symbol {
    ArenaString name;
    SymbolKind kind;
    u32 moduleIndex;      // camelCase for members
    NamespaceKind namespaceKind;
    u32 scopeLevel;
    u32 index;
};
```

### Functions

| Context | Convention | Examples |
|---------|------------|----------|
| Public/namespace functions | PascalCase | `Init()`, `AddSymbol()`, `LookupByHash()` |
| Accessors | Get/Set prefix | `GetTypeId()`, `GetSpirvId()`, `SetStage()` |
| Parser methods | PascalCase | `Match()`, `Consume()`, `Advance()`, `ParseExpression()` |
| Private helpers | PascalCase or lambda | See pattern section |

### Classes and Structs

| Context | Convention | Examples |
|---------|------------|----------|
| Main types | PascalCase | `Parser`, `SPIRVBuilder`, `CFG` |
| Data containers | PascalCase + Data suffix | `SymbolTableData`, `VariableData`, `FunctionData` |
| Node data | PascalCase + Data suffix | `IdentifierData`, `BinaryOpData`, `BlockData` |

### Enums

Use `enum class` with ALL_CAPS or PascalCase values depending on context:

```cpp
// Category/flag enums use ALL_CAPS
enum class TokenType : u8 {
    LEFT_BRACE,
    RIGHT_BRACE,
    IDENTIFIER,
    EOF_TOKEN,
};

// Type enums use ALL_CAPS
enum class CoreType : u8 {
    INVALID = 0,
    BOOL, INT, UINT, FLOAT,
    FLOAT2, FLOAT3, FLOAT4,
    // ...
};

// Semantic enums use PascalCase
enum class SymbolKind {
    VARIABLE,
    FUNCTION,
    ATTRIBUTE,
    RESOURCE,
    CUSTOM_TYPE,
};
```

### Constants

| Context | Convention | Examples |
|---------|------------|----------|
| Compile-time constants | SCREAMING_SNAKE_CASE | `MAX_FIELDS_PER_STRUCT`, `NO_EXT_INST` |
| Sentinel values | SCREAMING_SNAKE_CASE | `INVALID_INDEX`, `NO_BLOCK = 0xFFFFFFFF` |
| Static constexpr in struct | SCREAMING_SNAKE_CASE | `static constexpr u32 MODULE_HASH_TABLE_SIZE = 64;` |

```cpp
// From bwsl_spirv_backend.h
static constexpr u32 MAX_FIELDS_PER_STRUCT = 32;
constexpr u32 SpvVersion_1_2 = 0x00010200;
constexpr u32 NO_EXT_INST = 0xFFFFFFFF;
```

### Files

| Type | Convention | Examples |
|------|------------|----------|
| Phase components | `phases/<phase>/bwsl_<component>.{h,cpp}` | `phases/lexing/bwsl_lexer.h`, `phases/parser/bwsl_parser_soa.h` |
| Core components | `core/bwsl_<component>.{h,cpp}` | `core/bwsl_arena.h`, `core/bwsl_symbol_table.h` |
| SoA implementations | `_soa` suffix | `bwsl_ast_soa.h`, `bwsl_eval_soa.h` |
| IR-lowering shards | `_<topic>.inl` included from header | `bwsl_ir_lowering_calls.inl`, `bwsl_ir_lowering_expr.inl` |
| Tools | `tools/<name>.cpp` | `tools/bwslc.cpp`, `tools/spirv_cross_wrapper.cpp` |

---

## Code Organization

### Free Functions in Namespaces

Rather than member methods, the codebase frequently uses free functions in namespaces that take a data struct pointer:

```cpp
// From bwsl_symbol_table.h
namespace SymbolTable {
    inline void Init(SymbolTableData* table, BWSL_Arena* arena) {
        table->arena = arena;
        table->symbols.Init(arena, 64);
        // ...
    }

    inline Symbol* AddSymbol(SymbolTableData* table, const ArenaString& name,
        SymbolKind kind, NamespaceKind ns = NamespaceKind::GLOBAL) {
        // ...
    }

    inline Symbol* Lookup(SymbolTableData* table, const ArenaString& name) {
        return LookupByHash(table, name.nameHash);
    }
}

// Usage:
SymbolTableData table;
SymbolTable::Init(&table, arena);
Symbol* sym = SymbolTable::AddSymbol(&table, name, SymbolKind::VARIABLE);
```

This pattern separates data (structs) from operations (namespace functions).

### Factory Namespaces

AST node creation uses a factory namespace pattern:

```cpp
// From bwsl_ast_soa.h
namespace ASTFactory {
    inline NodeRef MakeIdentifier(AST* ast, const ArenaString& name, u32 line = 0, u32 col = 0) {
        u32 index = ast->identifiers.count;
        IdentifierData data;
        data.name = name;
        data.identifierKind = SpecialIdentifier::NONE;
        ast->identifiers.Push(ast->arena, data);
        // ... position tracking ...
        return NodeRef(ASTNodeType::IDENTIFIER, index);
    }

    inline NodeRef MakeBinaryOp(AST* ast, BinaryOpType op, NodeRef left, NodeRef right,
                                 u32 line = 0, u32 col = 0) {
        // ...
    }
}
```

### Unity Build Pattern

The codebase uses a unity build where `tools/bwslc.cpp` includes every implementation `.cpp` directly, in pipeline order:

```cpp
// tools/bwslc.cpp (excerpt)
#include "../phases/lexing/bwsl_lexer.cpp"
#include "../phases/parser/bwsl_parser_soa.cpp"
#include "../phases/evaluation/bwsl_eval_soa.cpp"
#include "../phases/evaluation/bwsl_comptime_interpreter.cpp"
#include "../core/bwsl_module_cache.cpp"
#include "../phases/ir_generation/bwsl_ir_gen.cpp"
#include "../phases/ir_generation/bwsl_ir_analysis.cpp"
#include "../phases/control_flow/bwsl_cfg.cpp"
#include "../phases/ssa/bwsl_ssa.cpp"
#include "../phases/backends/spirv/bwsl_spirv_backend.cpp"
#include "../phases/backends/gles/bwsl_gles_backend.cpp"
#include "../phases/ir_generation/bwsl_compute_graph.cpp"
#include "../core/bwsl_custom_type_registry.cpp"
#include "../core/bwsl_variant_system.cpp"
```

**Exception:** `tools/spirv_cross_wrapper.cpp` is compiled as a separate translation unit because BWSL's type aliases (`u32`, `f32`, `f64` from `core/bwsl_defs.h`) collide with member names in SPIRV-Cross.

The IR-lowering phase splits its body across `.inl` shards (`bwsl_ir_lowering_*.inl`) included from `bwsl_ir_lowering.h` so each file stays reviewable while still benefiting from the unity build.

### Header Organization

Most logic lives in headers with inline functions:

```cpp
// bwsl_component.h
#pragma once

#include <cstdint>
#include "bwsl_defs.h"
#include "bwsl_arena.h"

namespace BWSL {

struct ComponentData {
    // Data members
};

namespace Component {
    inline void Init(ComponentData* data, BWSL_Arena* arena) {
        // Implementation directly in header
    }
}

} // namespace BWSL
```

---

## Data Structures - Structure of Arrays (SoA)

### NodeRef - Compact AST References

```cpp
// From bwsl_ast_soa.h - 4 bytes total
struct NodeRef {
    u32 packed;  // High 8 bits = type, low 24 bits = index

    NodeRef() : packed(0xFFFFFFFF) {}
    NodeRef(ASTNodeType type, u32 index)
        : packed((static_cast<u32>(type) << 24) | (index & 0x00FFFFFF)) {}

    ASTNodeType Type() const { return static_cast<ASTNodeType>(packed >> 24); }
    u32 Index() const { return packed & 0x00FFFFFF; }
    bool IsValid() const { return packed != 0xFFFFFFFF; }
    bool IsNull() const { return packed == 0xFFFFFFFF; }
    static NodeRef Null() { return NodeRef(); }
};
```

### Type-Specific Data Pools

Each AST node type has its own data pool with minimal per-node overhead:

```cpp
// From bwsl_ast_soa.h
// 16 bytes - very common
struct IdentifierData {
    ArenaString name;
    SpecialIdentifier identifierKind;
    u8 _pad[3];  // Explicit padding for alignment
};

// 12 bytes - very common
struct BinaryOpData {
    BinaryOpType op;
    u8 _pad[3];
    NodeRef left;
    NodeRef right;
};

// 24 bytes
struct VariableDeclData {
    ArenaString name;
    ArenaString type;
    NodeRef initializer;
    bool isConst;
    StorageClass storageClass;
    u8 arrayDimensions;
    u8 _pad;
    u32 arrayLength;
    u32 arrayElementTypeHash;
};
```

### AST Structure with Parallel Arrays

```cpp
// From bwsl_ast_soa.h
struct AST {
    BWSL_Arena* arena;

    // Core node metadata - hot path for traversal
    alignas(64) u32* positions;  // Packed line/column
    u32 nodeCount;
    u32 nodeCapacity;

    // Type-specific pools (cold, only touched when you need that node type)
    ArenaArray<IdentifierData> identifiers;
    ArenaArray<LiteralData> literals;
    ArenaArray<BinaryOpData> binaryOps;
    ArenaArray<UnaryOpData> unaryOps;
    ArenaArray<MemberAccessData> memberAccesses;
    ArenaArray<FunctionCallData> functionCalls;
    // ... many more pools
};
```

### SPIR-V Builder SoA Layout

```cpp
// From bwsl_spirv_backend.h
struct SPIRVBuilder {
    // ============= Core ID Management (Hot Data) =============
    alignas(64) u32* spirvIds;           // Maps IR register -> SPIR-V ID
    alignas(64) u16* idTypes;            // CoreType for each SPIR-V ID
    alignas(64) u32* idDecorations;      // Packed decoration flags per ID
    alignas(64) bool* hasPreAllocatedId;
    alignas(64) u32* localVarIds;
    u32 nextId;
    u32 idCapacity;

    // ============= Type Deduplication =============
    alignas(64) u32 typeIds[static_cast<u32>(CoreType::COUNT)];
    alignas(64) u32* compositeTypeIds;
    alignas(64) u32* compositeTypeHashes;
    u32 compositeTypeCount;

    // ============= Constant Pools (parallel to IR) =============
    alignas(64) u32* floatConstantIds;
    alignas(64) u32* intConstantIds;
    alignas(64) u32* uintConstantIds;
    // ...
};
```

### Static Lookup Tables with Designated Initializers

```cpp
// From bwsl_spirv_backend.cpp
static constexpr spv::Op IR_TO_SPV_OP_TABLE[256] = {
    // ========== Control Flow ==========
    [IR::OP_NOP]           = spv::OpNop,
    [IR::OP_JUMP]          = spv::OpBranch,
    [IR::OP_BRANCH]        = spv::OpBranchConditional,
    [IR::OP_CALL]          = spv::OpFunctionCall,

    // ========== Arithmetic (Float) ==========
    [IR::OP_FADD]          = spv::OpFAdd,
    [IR::OP_FSUB]          = spv::OpFSub,
    [IR::OP_FMUL]          = spv::OpFMul,
    [IR::OP_FDIV]          = spv::OpFDiv,

    // ========== Math Functions ==========
    [IR::OP_SQRT]          = spv::OpExtInst,
    [IR::OP_SIN]           = spv::OpExtInst,
    [IR::OP_COS]           = spv::OpExtInst,
    // ...
};
```

---

## Memory Management

### Arena Allocator with Init/Push Pattern

```cpp
// ArenaArray from bwsl_arena.h
template<typename T>
struct ArenaArray {
    T* data;
    u32 count;
    u32 capacity;

    void Init(BWSL_Arena* arena, u32 initial_capacity);
    void Push(BWSL_Arena* arena, const T& item);
    T& operator[](u32 index) { return data[index]; }
};

// Usage:
ArenaArray<Symbol> symbols;
symbols.Init(arena, 64);
symbols.Push(arena, newSymbol);
```

### Initialization Patterns

```cpp
// From bwsl_symbol_table.h - Init function pattern
inline void Init(SymbolTableData* table, BWSL_Arena* arena) {
    table->arena = arena;
    table->symbols.Init(arena, 64);
    table->variables.Init(arena, 32);
    table->functions.Init(arena, 32);
    table->genericFunctions.Init(arena, 16);
    table->attributes.Init(arena, 16);
    table->resources.Init(arena, 32);
    table->structs.Init(arena, 16);
    table->enums.Init(arena, 32);
    table->constraints.Init(arena, 16);
    table->scopeStartIndices.Init(arena, 16);
    // ...
    table->currentScope = 0;
}
```

### Aligned Allocation for Hot Data

```cpp
// From bwsl_spirv_backend.cpp
void SPIRVBuilder::Initialize(BWSL_Arena* arena, IR::IRProgram* ir, ...) {
    // Initialize ID management with cache-line alignment
    nextId = 1;
    idCapacity = ir->registerCount + 256;
    spirvIds = (u32*)arena->Allocate(idCapacity * sizeof(u32), 64);
    idTypes = (u16*)arena->Allocate(idCapacity * sizeof(u16), 64);
    idDecorations = (u32*)arena->Allocate(idCapacity * sizeof(u32), 64);

    // Zero-initialize
    memset(spirvIds, 0, idCapacity * sizeof(u32));
    memset(idTypes, 0, idCapacity * sizeof(u16));
    memset(idDecorations, 0, idCapacity * sizeof(u32));
}
```

### Section Pattern for Binary Building

```cpp
// From bwsl_spirv_backend.h
struct Section {
    alignas(64) u32* words;
    u32 count;
    u32 capacity;
};

// Initialize sections
auto initSection = [arena](Section* s, u32 initial_capacity) {
    s->words = (u32*)arena->Allocate(initial_capacity * sizeof(u32), 64);
    s->count = 0;
    s->capacity = initial_capacity;
};

initSection(&capabilities, 32);
initSection(&extensions, 32);
initSection(&typesConstants, 512);
initSection(&functions, 2048);
```

---

## Error Handling

### Error Collection Pattern

```cpp
// From bwsl_parser_soa.h
struct ParseError {
    const char* message;
    u32 line;
    u32 column;
    TokenRef token;
};

struct Parser {
    ArenaArray<ParseError> errors;
    bool hadError;
    bool panicMode;  // For panic-mode recovery
    // ...
};
```

### Error Reporting

```cpp
// From bwsl_parser_soa.cpp
void Parser::ErrorAt(TokenRef token, const char* message) {
    if (panicMode || !lexer || !message) return;

    SourceLocation loc = getLocation(stream->GetOffset(token));
    panicMode = true;
    hadError = true;

    ParseError error;
    size_t msgLen = strlen(message);
    char* msgCopy = (char*)arena->Allocate(msgLen + 1, 1);
    if (!msgCopy) return;
    memcpy(msgCopy, message, msgLen);
    msgCopy[msgLen] = '\0';
    error.message = msgCopy;
    error.line = loc.line;
    error.column = loc.column;
    error.token = token;
    errors.Push(arena, error);
}
```

### Result Enums

```cpp
// From bwsl_symbol_table.h
enum class AddConstraintResult {
    SUCCESS,
    DUPLICATE_IN_SCOPE,
    DUPLICATE_IN_MODULE,
    DUPLICATE_FROM_IMPORT
};

inline AddConstraintResult AddConstraint(SymbolTableData* table, ArenaString name,
    TypeMask allowedTypes, ArenaString* outConflictingModule = nullptr) {
    // Check for collision
    if (LookupByHash(table, finalHash)) {
        if (table->inModuleScope) {
            if (outConflictingModule) { *outConflictingModule = table->modules[table->currentModuleIndex].name; }
            return AddConstraintResult::DUPLICATE_IN_MODULE;
        } else {
            return AddConstraintResult::DUPLICATE_IN_SCOPE;
        }
    }
    // ...
    return AddConstraintResult::SUCCESS;
}
```

### Panic Mode Recovery

```cpp
// From bwsl_parser_soa.cpp
void Parser::Synchronize() {
    panicMode = false;

    while (stream->GetType(current) != TokenType::EOF_TOKEN) {
        if (stream->GetType(previous) == TokenType::SEMICOLON) return;

        switch (stream->GetType(current)) {
            case TokenType::PASS:
            case TokenType::ATTRIBUTES:
            case TokenType::RESOURCES:
            case TokenType::IF:
            case TokenType::FOR:
            case TokenType::RETURN:
                return;
            default:
                ;
        }
        Advance();
    }
}
```

---

## Type System

### Type Aliases (core/bwsl_defs.h)

```cpp
using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8  = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using f32 = float;
using f64 = double;
```

### CoreType Enumeration

```cpp
// From bwsl_types.h
enum class CoreType : u8 {
    INVALID = 0,
    BOOL, INT, UINT, FLOAT,
    BOOL2, BOOL3, BOOL4,
    INT2, INT3, INT4,
    UINT2, UINT3, UINT4,
    FLOAT2, FLOAT3, FLOAT4,
    MAT2, MAT3, MAT4,
    VOID, STRING, CUSTOM, ENUM,
    GENERIC_T, GENERIC_U, GENERIC_V,
    CONSTRAINT,
    CBUFFER, BUFFER, TEXTURE2D, TEXTURE3D, TEXTURECUBE, TEXTURE2DARRAY,
    SAMPLER,
    VERTEX_FUNCTION, FRAGMENT_FUNCTION, COMPUTE_FUNCTION, PASS_BLOCK,
    COUNT  // Must be last
};
```

### Type Masking for Overloads

```cpp
// From bwsl_symbol_table.h
using OverloadTypeMask = u64;
static constexpr OverloadTypeMask OVERLOAD_CUSTOM_MASK = (1ULL << 63);

inline OverloadTypeMask MakeOverloadMask(CoreType type, u32 customHash = 0) {
    if (type == CoreType::CUSTOM) {
        return OVERLOAD_CUSTOM_MASK | static_cast<OverloadTypeMask>(customHash);
    }
    if (type == CoreType::INVALID || type == CoreType::VOID) {
        return 0;
    }
    return 1ULL << static_cast<u32>(type);
}

inline bool OverloadMaskMatches(OverloadTypeMask paramMask, OverloadTypeMask argMask) {
    if ((paramMask & OVERLOAD_CUSTOM_MASK) || (argMask & OVERLOAD_CUSTOM_MASK)) {
        return paramMask == argMask;
    }
    return (paramMask & argMask) != 0;
}
```

### ArenaString for Hash-Based Strings

```cpp
// Strings are stored as hash + optional source reference
struct ArenaString {
    u32 nameHash;        // Pre-computed hash for fast comparison
    u32 sourceOffset;    // Offset into source for reconstruction
    u16 length;          // Length of original string

    static ArenaString MakeHashOnly(const char* str);
    static ArenaString MakeHashOnly(const std::string& str);
    static ArenaString Make(const char* sourceBase, u32 offset, u16 len);

    bool operator==(const ArenaString& other) const {
        return nameHash == other.nameHash;
    }
};
```

---

## Commenting Style

### Section Headers

Use boxed comments with equals signs for major sections:

```cpp
// ============= Core ID Management (Hot Data) =============

// ============= Type Deduplication =============

// ============= Binary Sections =============

// ============= Inline Implementations (Hot Path) =============
```

### Category Comments in Tables

```cpp
static constexpr spv::Op IR_TO_SPV_OP_TABLE[256] = {
    // ========== Control Flow ==========
    [IR::OP_NOP]           = spv::OpNop,
    [IR::OP_JUMP]          = spv::OpBranch,

    // ========== Arithmetic (Float) ==========
    [IR::OP_FADD]          = spv::OpFAdd,
    [IR::OP_FSUB]          = spv::OpFSub,

    // ========== Math Functions ==========
    [IR::OP_SQRT]          = spv::OpExtInst,
};
```

### Inline Member Comments

```cpp
struct SPIRVBuilder {
    alignas(64) u32* spirvIds;           // Maps IR register -> SPIR-V ID
    alignas(64) u16* idTypes;            // CoreType for each SPIR-V ID
    alignas(64) u32* idDecorations;      // Packed decoration flags per ID
    u32 nextId;
    u32 idCapacity;
    u32 spvVersion = SpvVersion_1_2;
};
```

### Conditional Compilation Documentation

```cpp
// Parser timing instrumentation - define BWSL_PARSER_TIMING to enable
#ifdef BWSL_PARSER_TIMING
#include <chrono>
// ...
#define PARSER_TIME_ADVANCE() parser_timing::Timer _t(&parser_timing::get().advance_time)
#else
#define PARSER_TIME_ADVANCE()
#endif
```

---

## Include Patterns

### Header Guards

Use `#pragma once` exclusively:

```cpp
#pragma once

#include <cstdint>
#include "bwsl_defs.h"
// ...
```

### Include Order

1. Standard library headers
2. Core BWSL headers (defs, arena)
3. Component-specific BWSL headers
4. Vendor headers last

```cpp
// From bwsl_spirv_backend.h
#pragma once
#include "bwsl_ir_gen.h"
#include "bwsl_ir_analysis.h"
#include "bwsl_cfg.h"
#include "bwsl_ast_common.h"
#include "bwsl_symbol_table.h"
#include "bwsl_ast_soa.h"
#include "bwsl_defs.h"
#include "bwsl_render_config.h"
#include "bwsl_arena.h"
#include "vendor/SPIRV-Headers/include/spirv/unified1/spirv.hpp"
#include <vector>
```

### Forward Declarations

```cpp
// Forward declaration for SoA AST
struct AST;
struct CFG;
class TokenStream;
```

---

## Macro Usage

### Conditional Compilation for Instrumentation

```cpp
// Parser timing instrumentation
#ifdef BWSL_PARSER_TIMING
#define PARSER_TIME_ADVANCE() parser_timing::Timer _t(&parser_timing::get().advance_time)
#define PARSER_TIME_EXPR() parser_timing::Timer _t(&parser_timing::get().expression_time)
#define PARSER_TIMING_PRINT() parser_timing::get().print()
#else
#define PARSER_TIME_ADVANCE()
#define PARSER_TIME_EXPR()
#define PARSER_TIMING_PRINT()
#endif
```

### Binary Operator Parsing Macro

```cpp
// From bwsl_parser_soa.cpp - reduces repetitive code
#define PARSE_BINARY_OP(name, nextLevel, ...) \
NodeRef Parser::name() { \
    NodeRef left = nextLevel(); \
    if (!left.IsValid()) return NodeRef::Null(); \
    while (MatchMask(__VA_ARGS__)) { \
        BinaryOpType op = TokenTypeToBinaryOp(PreviousTokenType()); \
        SourceLocation loc = getLocation(stream->GetOffset(previous)); \
        NodeRef right = nextLevel(); \
        if (!right.IsValid()) return NodeRef::Null(); \
        left = ASTFactory::MakeBinaryOp(ast, op, left, right, loc.line, loc.column); \
    } \
    return left; \
}

PARSE_BINARY_OP(ParseOr, ParseAnd, mask(TokenType::OR))
PARSE_BINARY_OP(ParseAnd, ParseBitwiseOr, mask(TokenType::AND))
PARSE_BINARY_OP(ParseEquality, ParseComparison, mask(TokenType::EQUALS) | mask(TokenType::NOT_EQUALS))
// ...

#undef PARSE_BINARY_OP
```

### Token Mask Helpers

```cpp
// Bit mask for token type matching
constexpr TokenMask mask(TokenType t) {
    return 1ULL << static_cast<u64>(t);
}

namespace TokenMasks {
    constexpr TokenMask CORE_TYPES = mask(TokenType::INT) | mask(TokenType::FLOAT) | ...;
    constexpr TokenMask COMPARISON_OPERATORS = mask(TokenType::LESS) | mask(TokenType::GREATER) | ...;
    constexpr TokenMask ASSIGNMENT_OPERATORS = mask(TokenType::ASSIGN) | mask(TokenType::PLUS_ASSIGN) | ...;
}
```

---

## Performance Optimizations

### Cache-Line Alignment

```cpp
// From bwsl_spirv_backend.h
alignas(64) u32* spirvIds;
alignas(64) u16* idTypes;
alignas(64) u32 typeIds[static_cast<u32>(CoreType::COUNT)];
```

### Hash-Based Lookups

```cpp
// FNV-1a based signature hashing
inline u64 HashOverloadSignature(const OverloadTypeMask* masks, u32 count) {
    u64 hash = 1469598103934665603ULL;
    auto mix = [&hash](u64 value) {
        for (u32 i = 0; i < 8; i++) {
            hash ^= static_cast<u8>(value & 0xFF);
            hash *= 1099511628211ULL;
            value >>= 8;
        }
    };
    mix(static_cast<u64>(count));
    for (u32 i = 0; i < count; i++) {
        mix(masks[i]);
    }
    return hash;
}
```

### Template Emission for Hot Paths

```cpp
// From bwsl_spirv_backend.h - variadic template for instruction emission
template<typename... Args>
void Emit(spv::Op op, Args... args) {
    constexpr u32 word_count = sizeof...(args) + 1;
    if (currentFunctionSize + word_count > currentFunctionCapacity) {
        GrowCurrentFunction();
    }
    currentFunction[currentFunctionSize++] = (word_count << 16) | op;
    ((currentFunction[currentFunctionSize++] = args), ...);
}
```

### Lambda Helpers for Local Operations

```cpp
// From bwsl_spirv_backend.cpp
auto initSection = [arena](Section* s, u32 initial_capacity) {
    s->words = (u32*)arena->Allocate(initial_capacity * sizeof(u32), 64);
    s->count = 0;
    s->capacity = initial_capacity;
};

// From bwsl_parser_soa.cpp
auto parseSize = [&](const char* message) -> u32 {
    Consume(TokenType::NUMBER, message);
    std::string_view num = stream->GetValue(previous);
    // ...
    return static_cast<u32>(std::stoul(std::string(num), nullptr, 0));
};
```

---

## Common Code Patterns

### NodeRef Usage

```cpp
// Creating nodes via factory
NodeRef binOp = ASTFactory::MakeBinaryOp(ast, BinaryOpType::ADD, left, right, line, col);

// Checking validity
if (binOp.IsValid()) {
    BinaryOpData& data = ast->GetBinaryOp(binOp);
    // Process data
}

// Null checks
if (node.IsNull()) return;
```

### Symbol Table Lookup Pattern

```cpp
// Lookup with fallback to imported modules
inline Symbol* LookupAny(SymbolTableData* table, const ArenaString& name) {
    NamespaceKind currentNs = table->inModuleScope ? NamespaceKind::MODULE : NamespaceKind::GLOBAL;
    u32 currentModule = table->inModuleScope ? table->currentModuleIndex : INVALID_INDEX;

    Symbol* sym = Lookup(table, name, currentNs, currentModule);
    if (sym) return sym;

    if (currentNs != NamespaceKind::MODULE) {
        for (u32 i = 0; i < table->importedModules.count; i++) {
            u32 modIdx = table->importedModules[i];
            sym = Lookup(table, name, NamespaceKind::MODULE, modIdx);
            if (sym) return sym;
        }
    }
    return nullptr;
}
```

### SPIR-V ID Management

```cpp
// Lazy allocation on first use
inline u32 SPIRVBuilder::GetSpirvId(u16 ir_register) {
    // Handle special encoding for constants
    if ((ir_register & 0xC000) == 0xC000) {
        u32 idx = ir_register & 0x3FFF;
        return GetBoolConstantId(ir->boolConstants[idx] != 0);
    }
    if (ir_register & 0x8000) {
        u32 idx = ir_register & 0x7FFF;
        return GetFloatConstantId(ir->floatConstants[idx]);
    }
    // ...

    // Regular register - allocate on first use
    u32 id = spirvIds[ir_register];
    if (id == 0) {
        id = AllocateId();
        spirvIds[ir_register] = id;
    }
    return id;
}
```

### Type Deduplication Pattern

```cpp
inline u32 SPIRVBuilder::GetTypeId(CoreType type) {
    u32 idx = static_cast<u32>(type);
    if (idx >= static_cast<u32>(CoreType::COUNT)) return 0;

    u32 id = typeIds[idx];
    if (id == 0) {
        // Lazy creation
        id = AllocateId();
        typeIds[idx] = id;

        // Emit the OpType* instruction
        switch (type) {
            case CoreType::VOID:   EmitToSection(&typesConstants, spv::OpTypeVoid, &id, 1); break;
            case CoreType::BOOL:   EmitToSection(&typesConstants, spv::OpTypeBool, &id, 1); break;
            case CoreType::FLOAT:  { u32 ops[] = {id, 32}; EmitToSection(&typesConstants, spv::OpTypeFloat, ops, 2); break; }
            // ...
        }
    }
    return id;
}
```

---

## Quick Reference

| Aspect | Convention |
|--------|------------|
| Variables | camelCase for members, camelCase or snake_case for locals |
| Functions | PascalCase |
| Classes/Structs | PascalCase, Data suffix for data containers |
| Enums | `enum class`, ALL_CAPS or PascalCase values |
| Constants | SCREAMING_SNAKE_CASE, `static constexpr` |
| Files | `bwsl_component.h/cpp`, `_soa` suffix for SoA |
| Namespaces | PascalCase, free functions take data struct pointer |
| Memory | Arena allocation, Init/Push pattern |
| Hot Data | `alignas(64)` for cache-line alignment |
| Errors | Collection pattern, panic-mode recovery |
| Guards | `#pragma once` |
| Comments | `// ===` section headers, inline for members |
