#pragma once

#include "bwsl_defs.h"
#include "bwsl_arena.h"
#include <string>
#include <unordered_map>
#include <vector>
#include "bwsl_token_defs.h"
#include "bwsl_utils.h"
#include <cassert>

namespace BWSL {
    using BWSL_Arena = Memory::BWEMemoryArena;

// Forward declaration for TokenStream-based ArenaString::Make
class TokenStream;
using TokenRef = u32;

struct ArenaString {
    u32 nameHash;
    u32 sourceOffset;  // Only valid if nameLength > 0
    u16 nameLength;    // 0 = "hash-only", > 0 = "source-backed"
    u8 _padding[2];

    // For strings FROM source buffer
    static ArenaString Make(const char* sourceBase, u32 offset, u16 length) {
        return {
            Utils::HashStr(sourceBase + offset, length),
            offset,
            length,
            {0, 0}
        };
    }

    // For strings via TokenStream (declared here, implemented after TokenStream is included)
    static inline ArenaString Make(const TokenStream* stream, TokenRef ref);
    
    // For synthetic/built-in strings NOT in source
    static ArenaString MakeHashOnly(const char* literal) {
        return { Utils::HashStr(literal), 0u, 0u, {0, 0} };
    }
    
    static ArenaString MakeHashOnly(const std::string& str) {
        return { Utils::HashStr(str.c_str()), 0u, 0u, {0, 0} };
    }
    
    static ArenaString MakeHashOnly(u32 precomputedHash) {
        return { precomputedHash, 0u, 0u, {0, 0} };
    }
    
    // Check if this string came from source
    bool isHashOnly() const { return nameLength == 0; }
    
    // Get view (only valid for source-backed strings)
    std::string_view view(const char* sourceBase) const {
        assert(!isHashOnly() && "Cannot get view of hash-only string");
        return std::string_view(sourceBase + sourceOffset, nameLength);
    }
    
    // For debugging/display - requires reverse lookup for hash-only
    std::string ToString(const char* sourceBase = nullptr) const {
        if (nameLength > 0 && sourceBase) {
            return std::string(view(sourceBase));
        }
        // Hash-only: use reverse lookup table (see below)
        return ReverseLookup::GetString(nameHash);
    }
    
    bool operator==(const ArenaString& other) const {
        return nameHash == other.nameHash;
    }
};

enum class CoreType : u8 {
    
    // Invalid/Not a type
    INVALID = 0,  // Explicitly 0 so uninitialized data is invalid

    // Scalars
    BOOL,
    INT,
    UINT,
    FLOAT,
    
    // Vectors
    BOOL2,
    BOOL3,
    BOOL4,
    INT2,
    INT3,
    INT4,
    UINT2,
    UINT3,
    UINT4,
    FLOAT2,
    FLOAT3,
    FLOAT4,
    
    // Matrices
    MAT2,
    MAT3,
    MAT4,
    
    // Special
    VOID,
    STRING,
    CUSTOM,  // For user-defined structs
    ENUM,
    GENERIC_T,
    GENERIC_U,
    GENERIC_V,
    CONSTRAINT,
    // Resources
    CBUFFER,
    BUFFER,
    TEXTURE2D,
    TEXTURE3D,
    TEXTURECUBE,
    TEXTURE2DARRAY,
    SAMPLER,

    // Shader specific
    VERTEX_FUNCTION,
    FRAGMENT_FUNCTION,
    COMPUTE_FUNCTION,
    PASS_BLOCK,

    // Reserved 64-bit types
    INT64,
    UINT64,
    DOUBLE,
    INT64X2,
    INT64X3,
    INT64X4,
    UINT64X2,
    UINT64X3,
    UINT64X4,
    DOUBLE2,
    DOUBLE3,
    DOUBLE4,
    DMAT2,
    DMAT3,
    DMAT4,
    COUNT // THIS HAS TO BE LAST, OR ELSE SHIT BREAKS!!!!!!!!
};

enum class SpecialIdentifier : u8 {
    NONE = 0,
    ATTRIBUTES = 1,
    RESOURCES = 2,
    OUTPUT = 3,
    INPUT = 4,      // For fragment shader reading interpolated varyings
    SELF = 5,       // For enum methods referencing self
    VARIANTS = 6    // Compiler-defined variant namespace
};

enum class BackendType {
    Metal,
    HLSL,      // DirectX
    GLSL,      // OpenGL/Vulkan
    SPIRV,     // Vulkan bytecode
    WGSL       // WebGPU
};
constexpr u32 MAX_ARRAY_SIZE = 262144;  // 256K elements (1MB of float4s)
struct TypeInfo {
    CoreType coreType;
    u8 componentCount;    // 1 for scalar, 2-4 for vectors, etc.
    u8 arrayDimensions;   // 0 = not array, 1 = 1D, 2 = 2D, etc.
    u8 _padding;
    u32 customTypeHash;   // Only used if coreType == CUSTOM, otherwise 0
    u32 arrayLength;      
    u32 arrayStride;     
};

#define TYPE_INFO(CORE_TYPE, COMP_COUNT, IS_ARRAY) {CORE_TYPE, COMP_COUNT, IS_ARRAY, 0, 0, 0, 0}

inline bool IsArray(const TypeInfo& info) {
    return info.arrayDimensions > 0;
}

inline u32 CoreTypeScalarComponentCount(CoreType type) {
    switch (type) {
        case CoreType::FLOAT2:
        case CoreType::INT2:
        case CoreType::UINT2:
        case CoreType::BOOL2:
            return 2;
        case CoreType::FLOAT3:
        case CoreType::INT3:
        case CoreType::UINT3:
        case CoreType::BOOL3:
            return 3;
        case CoreType::FLOAT4:
        case CoreType::INT4:
        case CoreType::UINT4:
        case CoreType::BOOL4:
            return 4;
        default:
            return 1;
    }
}

inline u8 CoreTypeComponentCount(CoreType type) {
    switch (type) {
        case CoreType::INT:
        case CoreType::UINT:
        case CoreType::FLOAT:
        case CoreType::INT64:
        case CoreType::UINT64:
        case CoreType::DOUBLE:
        case CoreType::BOOL:
            return 1;
        case CoreType::INT2:
        case CoreType::UINT2:
        case CoreType::FLOAT2:
        case CoreType::INT64X2:
        case CoreType::UINT64X2:
        case CoreType::DOUBLE2:
            return 2;
        case CoreType::INT3:
        case CoreType::UINT3:
        case CoreType::FLOAT3:
        case CoreType::INT64X3:
        case CoreType::UINT64X3:
        case CoreType::DOUBLE3:
            return 3;
        case CoreType::INT4:
        case CoreType::UINT4:
        case CoreType::FLOAT4:
        case CoreType::INT64X4:
        case CoreType::UINT64X4:
        case CoreType::DOUBLE4:
            return 4;
        case CoreType::MAT2:
        case CoreType::DMAT2:
            return 4;
        case CoreType::MAT3:
        case CoreType::DMAT3:
            return 9;
        case CoreType::MAT4:
        case CoreType::DMAT4:
            return 16;
        default:
            return 1;
    }
}

inline u32 CoreTypeStorageSize(CoreType type) {
    switch (type) {
        case CoreType::FLOAT:
        case CoreType::INT:
        case CoreType::UINT:
        case CoreType::BOOL:
            return 4;
        case CoreType::FLOAT2:
        case CoreType::INT2:
        case CoreType::UINT2:
            return 8;
        case CoreType::FLOAT3:
        case CoreType::INT3:
        case CoreType::UINT3:
            return 12;
        case CoreType::FLOAT4:
        case CoreType::INT4:
        case CoreType::UINT4:
            return 16;
        case CoreType::MAT2:
            return 32;
        case CoreType::MAT3:
            return 48;
        case CoreType::MAT4:
            return 64;
        default:
            return 4;
    }
}

inline u32 CoreTypeStd140Alignment(CoreType type) {
    switch (type) {
        case CoreType::FLOAT:
        case CoreType::INT:
        case CoreType::UINT:
        case CoreType::BOOL:
            return 4;
        case CoreType::FLOAT2:
        case CoreType::INT2:
        case CoreType::UINT2:
            return 8;
        case CoreType::FLOAT3:
        case CoreType::FLOAT4:
        case CoreType::INT3:
        case CoreType::INT4:
        case CoreType::UINT3:
        case CoreType::UINT4:
        case CoreType::MAT2:
        case CoreType::MAT3:
        case CoreType::MAT4:
            return 16;
        default:
            return 4;
    }
}

inline u32 CoreTypeStd140Size(CoreType type) {
    switch (type) {
        case CoreType::FLOAT3:
        case CoreType::INT3:
        case CoreType::UINT3:
            return 16;
        default:
            return CoreTypeStorageSize(type);
    }
}

    using TokenMask = uint64_t;
    using TypeMask  = uint64_t;

    static_assert(static_cast<size_t>(CoreType::COUNT) <= 64,
        "TypeMask must be wide enough to represent every CoreType");

     static constexpr TokenMask mask(TokenType type) {
        return 1ULL << static_cast<size_t>(type);
    }

    static constexpr TypeMask mask(CoreType type) {
        // Bounds-check: fuzz inputs can end up with a CoreType cast from
        // garbage storage (e.g. uninitialized registerTypes[reg]) which makes
        // the shift count >= 64 -> undefined behavior (caught by UBSan).
        size_t idx = static_cast<size_t>(type);
        if (idx >= 64) {
            return 0;
        }
        return static_cast<TypeMask>(1ULL << idx);
    }

     // Generics
    struct TypeConstraint {
    ArenaString name;
    u32 nameHash;
    TypeMask allowedTypes;  
    bool isInline;
    };


    constexpr TypeInfo TOKEN_TO_TYPE_INFO[static_cast<size_t>(TokenType::TOKEN_COUNT)] = {
        // --- Core types (0–16) ---
        TYPE_INFO(CoreType::FLOAT,  1,  false),  // FLOAT
        TYPE_INFO(CoreType::FLOAT2, 2,  true ),  // FLOAT2
        TYPE_INFO(CoreType::FLOAT3, 3,  true ),  // FLOAT3
        TYPE_INFO(CoreType::FLOAT4, 4,  true ),  // FLOAT4
        TYPE_INFO(CoreType::INT,    1,  false),  // INT
        TYPE_INFO(CoreType::INT2,   2,  true ),  // INT2
        TYPE_INFO(CoreType::INT3,   3,  true ),  // INT3
        TYPE_INFO(CoreType::INT4,   4,  true ),  // INT4
        TYPE_INFO(CoreType::UINT,   1,  false),  // UINT
        TYPE_INFO(CoreType::UINT2,  2,  true ),  // UINT2
        TYPE_INFO(CoreType::UINT3,  3,  true ),  // UINT3
        TYPE_INFO(CoreType::UINT4,  4,  true ),  // UINT4
        TYPE_INFO(CoreType::MAT2,   4,  true ),  // MAT2
        TYPE_INFO(CoreType::MAT3,   9,  true ),  // MAT3
        TYPE_INFO(CoreType::MAT4,   16, true ),  // MAT4
        TYPE_INFO(CoreType::BOOL,   1,  false),  // BOOL
        TYPE_INFO(CoreType::VOID,   0,  false),  // VOID

        // --- Generic Types & Constraints (17-21) ---
        TYPE_INFO(CoreType::GENERIC_T, 1,  true),  // T
        TYPE_INFO(CoreType::GENERIC_U, 1,  true),  // U
        TYPE_INFO(CoreType::GENERIC_V, 1,  true),  // V
        TYPE_INFO(CoreType::ENUM,   4,  false),    // ENUM
        TYPE_INFO(CoreType::CONSTRAINT, 0, false), // CONSTRAINT

        // --- Variable length tokens (22–25) (not types) ---
        TYPE_INFO(CoreType::INVALID, 0, false),  // IDENTIFIER
        TYPE_INFO(CoreType::INVALID, 0, false),  // NUMBER
        TYPE_INFO(CoreType::INVALID, 0, false),  // STRING (literal)
        TYPE_INFO(CoreType::INVALID, 0, false),  // AT

        // --- Operators (26+) (not types) ---
        TYPE_INFO(CoreType::INVALID, 0, false),  // PLUS
        TYPE_INFO(CoreType::INVALID, 0, false),  // MINUS
        TYPE_INFO(CoreType::INVALID, 0, false),  // MULTIPLY
        TYPE_INFO(CoreType::INVALID, 0, false),  // DIVIDE
        TYPE_INFO(CoreType::INVALID, 0, false),  // MODULO
        TYPE_INFO(CoreType::INVALID, 0, false),  // ASSIGN
        TYPE_INFO(CoreType::INVALID, 0, false),  // NOT
        TYPE_INFO(CoreType::INVALID, 0, false),  // LESS
        TYPE_INFO(CoreType::INVALID, 0, false),  // GREATER
        TYPE_INFO(CoreType::INVALID, 0, false),  // EQUALS
        TYPE_INFO(CoreType::INVALID, 0, false),  // NOT_EQUALS
        TYPE_INFO(CoreType::INVALID, 0, false),  // LESS_EQUAL
        TYPE_INFO(CoreType::INVALID, 0, false),  // GREATER_EQUAL
        TYPE_INFO(CoreType::INVALID, 0, false),  // AND
        TYPE_INFO(CoreType::INVALID, 0, false),  // OR
        TYPE_INFO(CoreType::INVALID, 0, false),  // PLUS_ASSIGN
        TYPE_INFO(CoreType::INVALID, 0, false),  // MINUS_ASSIGN
        TYPE_INFO(CoreType::INVALID, 0, false),  // MULTIPLY_ASSIGN
        TYPE_INFO(CoreType::INVALID, 0, false),  // DIVIDE_ASSIGN
        TYPE_INFO(CoreType::INVALID, 0, false),  // DOUBLE_COLON
        TYPE_INFO(CoreType::INVALID, 0, false),  // ARROW
        TYPE_INFO(CoreType::INVALID, 0, false),  // AS
        TYPE_INFO(CoreType::INVALID, 0, false),  // DOT_DOT
        TYPE_INFO(CoreType::INVALID, 0, false),  // DOT_DOT_EQUAL

        // --- Delimiters (50+) (not types) ---
        TYPE_INFO(CoreType::INVALID, 0, false),  // LEFT_BRACE
        TYPE_INFO(CoreType::INVALID, 0, false),  // RIGHT_BRACE
        TYPE_INFO(CoreType::INVALID, 0, false),  // LEFT_PAREN
        TYPE_INFO(CoreType::INVALID, 0, false),  // RIGHT_PAREN
        TYPE_INFO(CoreType::INVALID, 0, false),  // LEFT_BRACKET
        TYPE_INFO(CoreType::INVALID, 0, false),  // RIGHT_BRACKET
        TYPE_INFO(CoreType::INVALID, 0, false),  // SEMICOLON
        TYPE_INFO(CoreType::INVALID, 0, false),  // COMMA
        TYPE_INFO(CoreType::INVALID, 0, false),  // DOT
        TYPE_INFO(CoreType::INVALID, 0, false),  // QUESTION
        TYPE_INFO(CoreType::INVALID, 0, false),  // COLON

        // --- Keywords (61+) ---
        TYPE_INFO(CoreType::INVALID, 0, false),  // IF
        TYPE_INFO(CoreType::INVALID, 0, false),  // IS
        TYPE_INFO(CoreType::INVALID, 0, false),  // IN
        TYPE_INFO(CoreType::INVALID, 0, false),  // WHERE
        TYPE_INFO(CoreType::INVALID, 0, false),  // SELF
        TYPE_INFO(CoreType::INVALID, 0, false),  // FOR
        TYPE_INFO(CoreType::INVALID, 0, false),  // FOREACH
        TYPE_INFO(CoreType::INVALID, 0, false),  // USE
        TYPE_INFO(CoreType::INVALID, 0, false),  // MIX
        TYPE_INFO(CoreType::INVALID, 0, false),  // OUT
        TYPE_INFO(CoreType::INVALID, 0, false),  // PASS
        TYPE_INFO(CoreType::INVALID, 0, false),  // TRUE
        TYPE_INFO(CoreType::INVALID, 0, false),  // FALSE
        TYPE_INFO(CoreType::INVALID, 0, false),  // NULL_TOKEN
        TYPE_INFO(CoreType::INVALID, 0, false),  // SLOT
        TYPE_INFO(CoreType::INVALID, 0, false),  // ELSE
        TYPE_INFO(CoreType::INVALID, 0, false),  // CONST
        TYPE_INFO(CoreType::INVALID, 0, false),  // SHARED
        TYPE_INFO(CoreType::INVALID, 0, false),  // EXTEND
        TYPE_INFO(CoreType::INVALID, 0, false),  // EXTENDS
        TYPE_INFO(CoreType::INVALID, 0, false),  // RETURN
        TYPE_INFO(CoreType::INVALID, 0, false),  // IMPORT
        TYPE_INFO(CoreType::INVALID, 0, false),  // MODULE
        TYPE_INFO(CoreType::CUSTOM,  0, false),  // STRUCT (custom/user-defined type)
        TYPE_INFO(CoreType::INVALID, 0, false),  // VERTEX
        TYPE_INFO(CoreType::INVALID, 0, false),  // FRAGMENT
        TYPE_INFO(CoreType::INVALID, 0, false),  // COMPUTE
        TYPE_INFO(CoreType::INVALID, 0, false),  // SHADER
        TYPE_INFO(CoreType::INVALID, 0, false),  // PIPELINE
        TYPE_INFO(CoreType::BUFFER,       0, false),  // BUFFER
        TYPE_INFO(CoreType::CBUFFER,      0, false),  // CBUFFER
        TYPE_INFO(CoreType::SAMPLER,      0, false),  // SAMPLER
        TYPE_INFO(CoreType::TEXTURE2D,    0, false),  // TEXTURE2D
        TYPE_INFO(CoreType::TEXTURE3D,    0, false),  // TEXTURE3D
        TYPE_INFO(CoreType::TEXTURECUBE,  0, false),  // TEXTURECUBE
        TYPE_INFO(CoreType::TEXTURE2DARRAY, 0, false),// TEXTURE2DARRAY
        TYPE_INFO(CoreType::INVALID, 0, false),  // RESOURCES
        TYPE_INFO(CoreType::INVALID, 0, false),  // ATTRIBUTES
        TYPE_INFO(CoreType::PASS_BLOCK,        0, false),  // PASS_BLOCK
        TYPE_INFO(CoreType::VERTEX_FUNCTION,   0, false),  // VERTEX_FUNCTION
        TYPE_INFO(CoreType::COMPUTE_FUNCTION,  0, false),  // COMPUTE_FUNCTION
        TYPE_INFO(CoreType::FRAGMENT_FUNCTION, 0, false),  // FRAGMENT_FUNCTION
        TYPE_INFO(CoreType::INVALID, 0, false),  // LOOP
        TYPE_INFO(CoreType::INVALID, 0, false),  // WHILE
        TYPE_INFO(CoreType::INVALID, 0, false),  // UNTIL
        TYPE_INFO(CoreType::INVALID, 0, false),  // STEP
        TYPE_INFO(CoreType::INVALID, 0, false),  // SKIP
        TYPE_INFO(CoreType::INVALID, 0, false),  // RANGE
        TYPE_INFO(CoreType::INVALID, 0, false),  // IT
        TYPE_INFO(CoreType::INVALID, 0, false),  // EVAL
        TYPE_INFO(CoreType::INVALID, 0, false),  // UNDERSCORE
        TYPE_INFO(CoreType::INVALID, 0, false),  // SWITCH
        TYPE_INFO(CoreType::INVALID, 0, false),  // CASE
        TYPE_INFO(CoreType::INVALID, 0, false),  // BREAK
        TYPE_INFO(CoreType::INVALID, 0, false),  // DISCARD
        TYPE_INFO(CoreType::INVALID, 0, false),  // VARIANTS
        TYPE_INFO(CoreType::INVALID, 0, false),  // RULES
        TYPE_INFO(CoreType::INVALID, 0, false),  // REQUIRE
        TYPE_INFO(CoreType::INVALID, 0, false),  // CONFLICT
        
        // Special
        TYPE_INFO(CoreType::INVALID, 0, false),  // ERROR_TOKEN
        TYPE_INFO(CoreType::INVALID, 0, false),  // EOF_TOKEN
        TYPE_INFO(CoreType::INVALID, 0, false),  // DEFAULT

        // --- Reserved 64-bit types ---
        TYPE_INFO(CoreType::DOUBLE,   1,  false),  // DOUBLE
        TYPE_INFO(CoreType::DOUBLE2,  2,  true ),  // DOUBLE2
        TYPE_INFO(CoreType::DOUBLE3,  3,  true ),  // DOUBLE3
        TYPE_INFO(CoreType::DOUBLE4,  4,  true ),  // DOUBLE4
        TYPE_INFO(CoreType::INT64,    1,  false),  // INT64
        TYPE_INFO(CoreType::UINT64,   1,  false),  // UINT64
        TYPE_INFO(CoreType::INT64X2,  2,  true ),  // INT64X2
        TYPE_INFO(CoreType::INT64X3,  3,  true ),  // INT64X3
        TYPE_INFO(CoreType::INT64X4,  4,  true ),  // INT64X4
        TYPE_INFO(CoreType::UINT64X2, 2,  true ),  // UINT64X2
        TYPE_INFO(CoreType::UINT64X3, 3,  true ),  // UINT64X3
        TYPE_INFO(CoreType::UINT64X4, 4,  true ),  // UINT64X4
        TYPE_INFO(CoreType::DMAT2,    4,  true ),  // DMAT2
        TYPE_INFO(CoreType::DMAT3,    9,  true ),  // DMAT3
        TYPE_INFO(CoreType::DMAT4,    16, true ),  // DMAT4
    };

    static_assert(
        sizeof(TOKEN_TO_TYPE_INFO) / sizeof(TOKEN_TO_TYPE_INFO[0]) ==
        static_cast<size_t>(TokenType::TOKEN_COUNT),
        "TOKEN_TO_TYPE_INFO must map every TokenType in order."
    );

    inline TypeInfo GetTypeInfoFromToken(TokenType token) {
        auto idx = static_cast<size_t>(token);
        if (idx < sizeof(TOKEN_TO_TYPE_INFO)/sizeof(TypeInfo)) {
            return TOKEN_TO_TYPE_INFO[idx];
        }
        return TYPE_INFO(CoreType::INVALID, 0, false);

    }

    inline bool IsValidType(const TypeInfo& info) {
        return info.coreType != CoreType::INVALID;
    }

namespace TokenMasks {
    
    // Core type masks
    static constexpr TokenMask CORE_TYPES = 
        // Float types
        mask(TokenType::FLOAT)  | mask(TokenType::FLOAT2) | 
        mask(TokenType::FLOAT3) | mask(TokenType::FLOAT4) |
        // Integer types  
        mask(TokenType::INT)    | mask(TokenType::INT2)   | 
        mask(TokenType::INT3)   | mask(TokenType::INT4)   |
        // Unsigned integer types
        mask(TokenType::UINT)   | mask(TokenType::UINT2)  | 
        mask(TokenType::UINT3)  | mask(TokenType::UINT4)  |
        // Matrix types
        mask(TokenType::MAT2)   | mask(TokenType::MAT3)   | 
        mask(TokenType::MAT4)   |
        // Boolean type         // Enum
        mask(TokenType::BOOL) | mask(TokenType::ENUM);
        
    static constexpr TokenMask ALL = 
        CORE_TYPES | mask(TokenType::IDENTIFIER) |
        mask(TokenType::T) | mask(TokenType::U) | mask(TokenType::V) | mask(TokenType::CONSTRAINT);
        
    static constexpr TokenMask BUFFER_TYPES = 
        // Float types
        mask(TokenType::FLOAT)  | mask(TokenType::FLOAT2) | 
        mask(TokenType::FLOAT3) | mask(TokenType::FLOAT4) |
        // Matrix types
        mask(TokenType::MAT2)   | mask(TokenType::MAT3)   | 
        mask(TokenType::MAT4)   |
        // Other
        mask(TokenType::BOOL)   | mask(TokenType::IDENTIFIER);
        
    static constexpr TokenMask INDEXABLE_TYPES = 
        // Vector types
        mask(TokenType::FLOAT2) | mask(TokenType::FLOAT3) | 
        mask(TokenType::FLOAT4) |
        mask(TokenType::INT2)   | mask(TokenType::INT3)   | 
        mask(TokenType::INT4)   |
        mask(TokenType::UINT2)  | mask(TokenType::UINT3)  | 
        mask(TokenType::UINT4)  |
        // Matrix types
        mask(TokenType::MAT2)   | mask(TokenType::MAT3)   | 
        mask(TokenType::MAT4);
        
    // Operator masks
    static constexpr TokenMask ASSIGNMENT_OPERATORS =
        mask(TokenType::ASSIGN)             |
        mask(TokenType::PLUS_ASSIGN)        |
        mask(TokenType::MINUS_ASSIGN)       |
        mask(TokenType::MULTIPLY_ASSIGN)    |
        mask(TokenType::DIVIDE_ASSIGN)      |
        mask(TokenType::MODULO_ASSIGN)      |
        mask(TokenType::BITWISE_AND_ASSIGN) |
        mask(TokenType::BITWISE_OR_ASSIGN)  |
        mask(TokenType::BITWISE_XOR_ASSIGN) |
        mask(TokenType::LEFT_SHIFT_ASSIGN)  |
        mask(TokenType::RIGHT_SHIFT_ASSIGN);
        
    static constexpr TokenMask UNARY_OPERATORS = 
        mask(TokenType::NOT)       | 
        mask(TokenType::MINUS)     | 
        mask(TokenType::PLUS)      |
        mask(TokenType::INCREMENT) |
        mask(TokenType::DECREMENT);
        
    static constexpr TokenMask COMPARISON_OPERATORS = 
        mask(TokenType::LESS)          | 
        mask(TokenType::GREATER)       | 
        mask(TokenType::LESS_EQUAL)    |
        mask(TokenType::GREATER_EQUAL) | 
        mask(TokenType::EQUALS)        | 
        mask(TokenType::NOT_EQUALS);
        
    static constexpr TokenMask LOGICAL_OPERATORS = 
        mask(TokenType::AND) | 
        mask(TokenType::OR);
        
    static constexpr TokenMask ARITHMETIC_OPERATORS = 
        mask(TokenType::PLUS)     | 
        mask(TokenType::MINUS)    | 
        mask(TokenType::MULTIPLY) |
        mask(TokenType::DIVIDE)   | 
        mask(TokenType::MODULO);
        
    // Additional useful masks
    static constexpr TokenMask BINARY_OPERATORS = 
        ARITHMETIC_OPERATORS | 
        COMPARISON_OPERATORS | 
        LOGICAL_OPERATORS;
        
    static constexpr TokenMask ALL_OPERATORS = 
        ASSIGNMENT_OPERATORS | 
        UNARY_OPERATORS | 
        BINARY_OPERATORS;
        
    static constexpr TokenMask VECTOR_TYPES = 
        mask(TokenType::FLOAT2) | mask(TokenType::FLOAT3) | 
        mask(TokenType::FLOAT4) |
        mask(TokenType::INT2)   | mask(TokenType::INT3)   | 
        mask(TokenType::INT4)   |
        mask(TokenType::UINT2)  | mask(TokenType::UINT3)  | 
        mask(TokenType::UINT4);
        
    static constexpr TokenMask MATRIX_TYPES = 
        mask(TokenType::MAT2) | 
        mask(TokenType::MAT3) | 
        mask(TokenType::MAT4);
        
    static constexpr TokenMask SCALAR_TYPES = 
        mask(TokenType::FLOAT) | 
        mask(TokenType::INT)   | 
        mask(TokenType::UINT)  | 
        mask(TokenType::BOOL);
    
    static constexpr TokenMask INT_TYPES = 
        mask(TokenType::INT) | mask(TokenType::INT2) | 
        mask(TokenType::INT3) | mask(TokenType::INT4);
    
    static constexpr TokenMask UINT_TYPES = 
        mask(TokenType::UINT) | mask(TokenType::UINT2) | 
        mask(TokenType::UINT3) | mask(TokenType::UINT4);
        
    static constexpr TokenMask GENERIC_TYPES = 
        mask(TokenType::T) | mask(TokenType::U) | mask(TokenType::V);

    // Helper functions
    static constexpr bool hasToken(TokenMask mask, TokenType type) {
        u8 idx = static_cast<u8>(type);
        return idx < 64 && (mask & (1ULL << idx)) != 0;
    }

    static constexpr bool isReservedScalar64Type(TokenType type) {
        return type == TokenType::DOUBLE ||
               type == TokenType::INT64 ||
               type == TokenType::UINT64;
    }

    static constexpr bool isReservedVector64Type(TokenType type) {
        return type == TokenType::DOUBLE2 ||
               type == TokenType::DOUBLE3 ||
               type == TokenType::DOUBLE4 ||
               type == TokenType::INT64X2 ||
               type == TokenType::INT64X3 ||
               type == TokenType::INT64X4 ||
               type == TokenType::UINT64X2 ||
               type == TokenType::UINT64X3 ||
               type == TokenType::UINT64X4;
    }

    static constexpr bool isReservedMatrix64Type(TokenType type) {
        return type == TokenType::DMAT2 ||
               type == TokenType::DMAT3 ||
               type == TokenType::DMAT4;
    }

    static constexpr bool isReservedInt64Type(TokenType type) {
        return type == TokenType::INT64 ||
               type == TokenType::INT64X2 ||
               type == TokenType::INT64X3 ||
               type == TokenType::INT64X4;
    }

    static constexpr bool isReservedUint64Type(TokenType type) {
        return type == TokenType::UINT64 ||
               type == TokenType::UINT64X2 ||
               type == TokenType::UINT64X3 ||
               type == TokenType::UINT64X4;
    }

    static constexpr bool isReservedFloat64Type(TokenType type) {
        return type == TokenType::DOUBLE ||
               type == TokenType::DOUBLE2 ||
               type == TokenType::DOUBLE3 ||
               type == TokenType::DOUBLE4 ||
               type == TokenType::DMAT2 ||
               type == TokenType::DMAT3 ||
               type == TokenType::DMAT4;
    }

    static constexpr bool isReserved64Type(TokenType type) {
        return isReservedScalar64Type(type) ||
               isReservedVector64Type(type) ||
               isReservedMatrix64Type(type);
    }

    static constexpr bool matches(TokenMask tokenMask, TokenType type) {
        if (hasToken(tokenMask, type)) return true;
        if ((tokenMask & CORE_TYPES) == CORE_TYPES && isReserved64Type(type)) return true;
        if ((tokenMask & SCALAR_TYPES) == SCALAR_TYPES && isReservedScalar64Type(type)) return true;
        if ((tokenMask & VECTOR_TYPES) == VECTOR_TYPES && isReservedVector64Type(type)) return true;
        if ((tokenMask & MATRIX_TYPES) == MATRIX_TYPES && isReservedMatrix64Type(type)) return true;
        if ((tokenMask & INT_TYPES) == INT_TYPES && isReservedInt64Type(type)) return true;
        if ((tokenMask & UINT_TYPES) == UINT_TYPES && isReservedUint64Type(type)) return true;
        if ((tokenMask & BUFFER_TYPES) == BUFFER_TYPES && isReservedFloat64Type(type)) return true;
        if ((tokenMask & INDEXABLE_TYPES) == INDEXABLE_TYPES &&
            (isReservedVector64Type(type) || isReservedMatrix64Type(type))) {
            return true;
        }
        return false;
    }
    
    static constexpr bool isType(TokenType type) {
        return matches(CORE_TYPES, type);
    }
    
    static constexpr bool isOperator(TokenType type) {
        return hasToken(ALL_OPERATORS, type);
    }
    
    static constexpr bool isVectorType(TokenType type) {
        return matches(VECTOR_TYPES, type);
    }
    
    static constexpr bool isMatrixType(TokenType type) {
        return matches(MATRIX_TYPES, type);
    }
    
    static constexpr bool isScalarType(TokenType type) {
        return matches(SCALAR_TYPES, type);
    }

    static constexpr bool isGenericType(TokenType type) {
        return hasToken(GENERIC_TYPES, type);
    }
}
    namespace TypeMasks{

        static constexpr TypeMask CORE_TYPES = 
        mask(CoreType::FLOAT) | mask(CoreType::FLOAT2) | mask(CoreType::FLOAT3) | mask(CoreType::FLOAT4) |
        mask(CoreType::DOUBLE) | mask(CoreType::DOUBLE2) | mask(CoreType::DOUBLE3) | mask(CoreType::DOUBLE4) |
        mask(CoreType::INT)   | mask(CoreType::INT2)   | mask(CoreType::INT3)   | mask(CoreType::INT4)   |
        mask(CoreType::INT64) | mask(CoreType::INT64X2) | mask(CoreType::INT64X3) | mask(CoreType::INT64X4) |
        mask(CoreType::UINT)  | mask(CoreType::UINT2)  | mask(CoreType::UINT3)  | mask(CoreType::UINT4)  |
        mask(CoreType::UINT64) | mask(CoreType::UINT64X2) | mask(CoreType::UINT64X3) | mask(CoreType::UINT64X4) |
        mask(CoreType::MAT2)  | mask(CoreType::MAT3)   | mask(CoreType::MAT4)   |
        mask(CoreType::DMAT2) | mask(CoreType::DMAT3)  | mask(CoreType::DMAT4)  | mask(CoreType::BOOL)
        | mask(CoreType::ENUM);

        static constexpr TypeMask ALL = 
        mask(CoreType::FLOAT) | mask(CoreType::FLOAT2) | mask(CoreType::FLOAT3) | mask(CoreType::FLOAT4) |
        mask(CoreType::DOUBLE) | mask(CoreType::DOUBLE2) | mask(CoreType::DOUBLE3) | mask(CoreType::DOUBLE4) |
        mask(CoreType::INT)   | mask(CoreType::INT2)   | mask(CoreType::INT3)   | mask(CoreType::INT4)   |
        mask(CoreType::INT64) | mask(CoreType::INT64X2) | mask(CoreType::INT64X3) | mask(CoreType::INT64X4) |
        mask(CoreType::UINT)  | mask(CoreType::UINT2)  | mask(CoreType::UINT3)  | mask(CoreType::UINT4)  |
        mask(CoreType::UINT64) | mask(CoreType::UINT64X2) | mask(CoreType::UINT64X3) | mask(CoreType::UINT64X4) |
        mask(CoreType::MAT2)  | mask(CoreType::MAT3)   | mask(CoreType::MAT4)   |
        mask(CoreType::DMAT2) | mask(CoreType::DMAT3)  | mask(CoreType::DMAT4)  | mask(CoreType::BOOL)
        | mask(CoreType::ENUM) | mask(CoreType::GENERIC_T) | mask(CoreType::GENERIC_U) | mask(CoreType::GENERIC_V) | mask(CoreType::CONSTRAINT);

        static constexpr TypeMask BUFFER_TYPES = 
        mask(CoreType::FLOAT) | mask(CoreType::FLOAT2) | mask(CoreType::FLOAT3) | mask(CoreType::FLOAT4) |
        mask(CoreType::DOUBLE) | mask(CoreType::DOUBLE2) | mask(CoreType::DOUBLE3) | mask(CoreType::DOUBLE4) |
        mask(CoreType::MAT2)  | mask(CoreType::MAT3)   | mask(CoreType::MAT4)   |
        mask(CoreType::DMAT2) | mask(CoreType::DMAT3)  | mask(CoreType::DMAT4)  | mask(CoreType::BOOL);

        static constexpr TypeMask INDEXABLE_TYPES = 
        mask(CoreType::FLOAT2) | mask(CoreType::FLOAT3) | mask(CoreType::FLOAT4) |
        mask(CoreType::DOUBLE2) | mask(CoreType::DOUBLE3) | mask(CoreType::DOUBLE4) |
        mask(CoreType::INT2)   | mask(CoreType::INT3)   | mask(CoreType::INT4)   |
        mask(CoreType::INT64X2) | mask(CoreType::INT64X3) | mask(CoreType::INT64X4) |
        mask(CoreType::UINT2)  | mask(CoreType::UINT3)  | mask(CoreType::UINT4)  |
        mask(CoreType::UINT64X2) | mask(CoreType::UINT64X3) | mask(CoreType::UINT64X4) |
        mask(CoreType::MAT2)   | mask(CoreType::MAT3)   | mask(CoreType::MAT4) |
        mask(CoreType::DMAT2)  | mask(CoreType::DMAT3)  | mask(CoreType::DMAT4);

        
        static constexpr TypeMask NUMERIC_TYPES = 
        mask(CoreType::FLOAT2) | mask(CoreType::FLOAT3) | mask(CoreType::FLOAT4) |
        mask(CoreType::DOUBLE2) | mask(CoreType::DOUBLE3) | mask(CoreType::DOUBLE4) |
        mask(CoreType::INT2)   | mask(CoreType::INT3)   | mask(CoreType::INT4)   |
        mask(CoreType::INT64X2) | mask(CoreType::INT64X3) | mask(CoreType::INT64X4) |
        mask(CoreType::UINT2)  | mask(CoreType::UINT3)  | mask(CoreType::UINT4)  |
        mask(CoreType::UINT64X2) | mask(CoreType::UINT64X3) | mask(CoreType::UINT64X4) |
        mask(CoreType::MAT2)   | mask(CoreType::MAT3)   | mask(CoreType::MAT4) |
        mask(CoreType::DMAT2)  | mask(CoreType::DMAT3)  | mask(CoreType::DMAT4);

        static constexpr TypeMask SCALAR_TYPES = 
        mask(CoreType::FLOAT)  | mask(CoreType::DOUBLE) | mask(CoreType::INT) |
        mask(CoreType::INT64) | mask(CoreType::UINT) | mask(CoreType::UINT64) | mask(CoreType::BOOL);
    
        static constexpr TypeMask VECTOR_TYPES = 
        mask(CoreType::FLOAT2) | mask(CoreType::FLOAT3) | mask(CoreType::FLOAT4) |
        mask(CoreType::DOUBLE2) | mask(CoreType::DOUBLE3) | mask(CoreType::DOUBLE4) |
        mask(CoreType::INT2)   | mask(CoreType::INT3)   | mask(CoreType::INT4) |
        mask(CoreType::INT64X2) | mask(CoreType::INT64X3) | mask(CoreType::INT64X4) |
        mask(CoreType::UINT2)  | mask(CoreType::UINT3)  | mask(CoreType::UINT4) |
        mask(CoreType::UINT64X2) | mask(CoreType::UINT64X3) | mask(CoreType::UINT64X4);
    
        static constexpr TypeMask MATRIX_TYPES = 
        mask(CoreType::MAT2) | mask(CoreType::MAT3) | mask(CoreType::MAT4) |
        mask(CoreType::DMAT2) | mask(CoreType::DMAT3) | mask(CoreType::DMAT4);
    
        static constexpr TypeMask FLOAT_TYPES = 
        mask(CoreType::FLOAT) | mask(CoreType::FLOAT2) | mask(CoreType::FLOAT3) | mask(CoreType::FLOAT4) |
        mask(CoreType::DOUBLE) | mask(CoreType::DOUBLE2) | mask(CoreType::DOUBLE3) | mask(CoreType::DOUBLE4) |
        mask(CoreType::DMAT2) | mask(CoreType::DMAT3) | mask(CoreType::DMAT4);

        static constexpr TypeMask INT_TYPES = 
        mask(CoreType::INT) | mask(CoreType::INT2) | mask(CoreType::INT3) | mask(CoreType::INT4) |
        mask(CoreType::INT64) | mask(CoreType::INT64X2) | mask(CoreType::INT64X3) | mask(CoreType::INT64X4);

        static constexpr TypeMask UINT_TYPES = 
        mask(CoreType::UINT) | mask(CoreType::UINT2) | mask(CoreType::UINT3) | mask(CoreType::UINT4) |
        mask(CoreType::UINT64) | mask(CoreType::UINT64X2) | mask(CoreType::UINT64X3) | mask(CoreType::UINT64X4);

        static constexpr TypeMask FLOAT_VECTORS = 
        mask(CoreType::FLOAT2) | 
        mask(CoreType::FLOAT3) | mask(CoreType::FLOAT4) |
        mask(CoreType::DOUBLE2) | mask(CoreType::DOUBLE3) | mask(CoreType::DOUBLE4);
    
        static constexpr TypeMask INT_VECTORS = 
        mask(CoreType::INT2) | 
        mask(CoreType::INT3) | mask(CoreType::INT4) |
        mask(CoreType::INT64X2) | mask(CoreType::INT64X3) | mask(CoreType::INT64X4);
    
        static constexpr TypeMask UINT_VECTORS =
        mask(CoreType::UINT2) |
        mask(CoreType::UINT3) | mask(CoreType::UINT4) |
        mask(CoreType::UINT64X2) | mask(CoreType::UINT64X3) | mask(CoreType::UINT64X4);

        static constexpr TypeMask BOOL_VECTORS =
        mask(CoreType::BOOL2) | mask(CoreType::BOOL3) | mask(CoreType::BOOL4);
    
        static constexpr TypeMask GENERIC_TYPES = 
        mask(CoreType::GENERIC_T) | mask(CoreType::GENERIC_U) | mask(CoreType::GENERIC_V);
    
        static constexpr TypeMask ALL_VECTORS = 
        FLOAT_VECTORS | INT_VECTORS | UINT_VECTORS;
    
        static constexpr TypeMask ANY_NUMERIC = SCALAR_TYPES | VECTOR_TYPES | MATRIX_TYPES;

        static constexpr TypeMask TEXTURE_TYPES = 
        mask(CoreType::TEXTURE2D) | mask(CoreType::TEXTURE3D) | 
        mask(CoreType::TEXTURECUBE) | mask(CoreType::TEXTURE2DARRAY);

        static constexpr bool TypeSatisfiesMask(CoreType type, TypeMask mask) {
        return (mask & (1ULL << static_cast<u32>(type))) != 0;
        }
    }



namespace TypeHashes {
    
    // Pre-computed hashes for all core types
    constexpr u32 BOOL        = Utils::HashStr("bool");
    constexpr u32 BOOL2       = Utils::HashStr("bool2");
    constexpr u32 BOOL3       = Utils::HashStr("bool3");
    constexpr u32 BOOL4       = Utils::HashStr("bool4");
    constexpr u32 INT         = Utils::HashStr("int");
    constexpr u32 UINT        = Utils::HashStr("uint");
    constexpr u32 FLOAT       = Utils::HashStr("float");
    constexpr u32 INT64       = Utils::HashStr("int64");
    constexpr u32 UINT64      = Utils::HashStr("uint64");
    constexpr u32 DOUBLE      = Utils::HashStr("double");
    constexpr u32 INT2        = Utils::HashStr("int2");
    constexpr u32 INT3        = Utils::HashStr("int3");
    constexpr u32 INT4        = Utils::HashStr("int4");
    constexpr u32 UINT2       = Utils::HashStr("uint2");
    constexpr u32 UINT3       = Utils::HashStr("uint3");
    constexpr u32 UINT4       = Utils::HashStr("uint4");
    constexpr u32 FLOAT2      = Utils::HashStr("float2");
    constexpr u32 FLOAT3      = Utils::HashStr("float3");
    constexpr u32 FLOAT4      = Utils::HashStr("float4");
    constexpr u32 INT64X2     = Utils::HashStr("int64x2");
    constexpr u32 INT64X3     = Utils::HashStr("int64x3");
    constexpr u32 INT64X4     = Utils::HashStr("int64x4");
    constexpr u32 UINT64X2    = Utils::HashStr("uint64x2");
    constexpr u32 UINT64X3    = Utils::HashStr("uint64x3");
    constexpr u32 UINT64X4    = Utils::HashStr("uint64x4");
    constexpr u32 DOUBLE2     = Utils::HashStr("double2");
    constexpr u32 DOUBLE3     = Utils::HashStr("double3");
    constexpr u32 DOUBLE4     = Utils::HashStr("double4");
    constexpr u32 MAT2        = Utils::HashStr("mat2");
    constexpr u32 MAT3        = Utils::HashStr("mat3");
    constexpr u32 MAT4        = Utils::HashStr("mat4");
    constexpr u32 DMAT2       = Utils::HashStr("dmat2");
    constexpr u32 DMAT3       = Utils::HashStr("dmat3");
    constexpr u32 DMAT4       = Utils::HashStr("dmat4");
    constexpr u32 VOID        = Utils::HashStr("void");
    constexpr u32 ENUM        = Utils::HashStr("enum");
    constexpr u32 CONSTRAINT  = Utils::HashStr("constraint");
    constexpr u32 T           = Utils::HashStr("T");
    constexpr u32 U           = Utils::HashStr("U");
    constexpr u32 V           = Utils::HashStr("V");
    // Texture/resource type hashes
    constexpr u32 TEXTURE2D   = Utils::HashStr("texture2D");
    constexpr u32 TEXTURE3D   = Utils::HashStr("texture3D");
    constexpr u32 TEXTURECUBE = Utils::HashStr("textureCube");
    constexpr u32 SAMPLER     = Utils::HashStr("sampler");
    constexpr u32 CBUFFER     = Utils::HashStr("cbuffer");
    constexpr u32 BUFFER      = Utils::HashStr("buffer");
    
    // Hash to TypeInfo lookup table
    struct HashEntry {
        u32 hash;
        TypeInfo info;
    };
    
    // Sorted by hash for potential binary search if we ever have enough to matter.
    constexpr HashEntry HASH_TABLE[] = {
        {BOOL,   TYPE_INFO(CoreType::BOOL,   1,  false)},
        {BOOL2,  TYPE_INFO(CoreType::BOOL2,  2,  true)},
        {BOOL3,  TYPE_INFO(CoreType::BOOL3,  3,  true)},
        {BOOL4,  TYPE_INFO(CoreType::BOOL4,  4,  true)},
        {INT,    TYPE_INFO(CoreType::INT,    1,  false)},
        {UINT,   TYPE_INFO(CoreType::UINT,   1,  false)},
        {FLOAT,  TYPE_INFO(CoreType::FLOAT,  1,  false)},
        {INT64,  TYPE_INFO(CoreType::INT64,  1,  false)},
        {UINT64, TYPE_INFO(CoreType::UINT64, 1,  false)},
        {DOUBLE, TYPE_INFO(CoreType::DOUBLE, 1,  false)},
        {INT2,   TYPE_INFO(CoreType::INT2,   2,  true)},
        {INT3,   TYPE_INFO(CoreType::INT3,   3,  true)},
        {INT4,   TYPE_INFO(CoreType::INT4,   4,  true)},
        {UINT2,  TYPE_INFO(CoreType::UINT2,  2,  true)},
        {UINT3,  TYPE_INFO(CoreType::UINT3,  3,  true)},
        {UINT4,  TYPE_INFO(CoreType::UINT4,  4,  true)},
        {FLOAT2, TYPE_INFO(CoreType::FLOAT2, 2,  true)},
        {FLOAT3, TYPE_INFO(CoreType::FLOAT3, 3,  true)},
        {FLOAT4, TYPE_INFO(CoreType::FLOAT4, 4,  true)},
        {INT64X2,  TYPE_INFO(CoreType::INT64X2,  2, true)},
        {INT64X3,  TYPE_INFO(CoreType::INT64X3,  3, true)},
        {INT64X4,  TYPE_INFO(CoreType::INT64X4,  4, true)},
        {UINT64X2, TYPE_INFO(CoreType::UINT64X2, 2, true)},
        {UINT64X3, TYPE_INFO(CoreType::UINT64X3, 3, true)},
        {UINT64X4, TYPE_INFO(CoreType::UINT64X4, 4, true)},
        {DOUBLE2, TYPE_INFO(CoreType::DOUBLE2, 2, true)},
        {DOUBLE3, TYPE_INFO(CoreType::DOUBLE3, 3, true)},
        {DOUBLE4, TYPE_INFO(CoreType::DOUBLE4, 4, true)},
        {MAT2,   TYPE_INFO(CoreType::MAT2,   4,  true)},
        {MAT3,   TYPE_INFO(CoreType::MAT3,   9,  true)},
        {MAT4,   TYPE_INFO(CoreType::MAT4,   16, true)},
        {DMAT2,  TYPE_INFO(CoreType::DMAT2,  4,  true)},
        {DMAT3,  TYPE_INFO(CoreType::DMAT3,  9,  true)},
        {DMAT4,  TYPE_INFO(CoreType::DMAT4,  16, true)},
        {T, TYPE_INFO(CoreType::GENERIC_T, 1,  true)},
        {U, TYPE_INFO(CoreType::GENERIC_U, 1,  true)},
        {V, TYPE_INFO(CoreType::GENERIC_V, 1,  true)},
        {CONSTRAINT, TYPE_INFO(CoreType::VOID,   0,  false)},
        {VOID,   TYPE_INFO(CoreType::VOID,   0,  false)},
        {ENUM,   TYPE_INFO(CoreType::ENUM,   1,  false)},
        {TEXTURE2D,   TYPE_INFO(CoreType::TEXTURE2D,     0, false)},
        {TEXTURE3D,   TYPE_INFO(CoreType::TEXTURE3D,     0, false)},
        {TEXTURECUBE, TYPE_INFO(CoreType::TEXTURECUBE,   0, false)},
        {SAMPLER,     TYPE_INFO(CoreType::SAMPLER,       0, false)},
        {CBUFFER,     TYPE_INFO(CoreType::CBUFFER,       0, false)},
        {BUFFER,      TYPE_INFO(CoreType::BUFFER,        0, false)},
    };
    
    constexpr u32 HASH_TABLE_SIZE = sizeof(HASH_TABLE) / sizeof(HashEntry);
}


inline std::string HandleModuleType(const std::string& typeName) {
    // Strip module prefix for code generation
    size_t colonPos = typeName.find("::");
    if (colonPos != std::string::npos) {
        // e.g., "Module_StructName" instead of "Module::StructName"
        std::string moduleName = typeName.substr(0, colonPos);
        std::string structName = typeName.substr(colonPos + 2);
        
        // Generate: "ModuleName_StructName" for the actual shader code
        return moduleName + "_" + structName;
    }
    return typeName;
}





}
