// bwsl_token_defs.h
// Minimal token type definitions - no dependencies
#pragma once
#include <cstdint>

// Undefine macros from mach/boolean.h that conflict with our token names
#ifdef TRUE
#undef TRUE
#endif
#ifdef FALSE
#undef FALSE
#endif

namespace BWSL {

enum TokenType : uint8_t {
    // Put types that need to be in masks first (< 64)

    // Core types (0-16)
    FLOAT = 0,
    FLOAT2,
    FLOAT3,
    FLOAT4,
    INT,
    INT2,
    INT3,
    INT4,
    UINT,
    UINT2,
    UINT3,
    UINT4,
    MAT2,
    MAT3,
    MAT4,
    BOOL,
    VOID,

    // Generic Types & Constraints (17-21) - Moved here to be < 64 for masks
    T,
    U,
    V,
    ENUM,
    CONSTRAINT,

    // Variable length tokens (22-25)
    IDENTIFIER,
    NUMBER,
    STRING,
    AT,

    // Operators (26+)
    PLUS,
    MINUS,
    MULTIPLY,
    DIVIDE,
    MODULO,
    ASSIGN,
    NOT,
    LESS,
    GREATER,
    EQUALS,
    NOT_EQUALS,
    LESS_EQUAL,
    GREATER_EQUAL,
    AND,
    OR,
    BITWISE_AND,
    BITWISE_OR,
    BITWISE_XOR,
    BITWISE_NOT,
    LEFT_SHIFT,     // <<
    RIGHT_SHIFT,    // >>
    PLUS_ASSIGN,
    MINUS_ASSIGN,
    MULTIPLY_ASSIGN,
    DIVIDE_ASSIGN,
    MODULO_ASSIGN,
    BITWISE_AND_ASSIGN,
    BITWISE_OR_ASSIGN,
    BITWISE_XOR_ASSIGN,
    LEFT_SHIFT_ASSIGN,
    RIGHT_SHIFT_ASSIGN,
    DOUBLE_COLON,
    ARROW,
    AS,
    DOT_DOT,
    DOT_DOT_EQUAL,
    INCREMENT,      // ++
    DECREMENT,      // --

    // Delimiters (45-54)
    LEFT_BRACE,
    RIGHT_BRACE,
    LEFT_PAREN,
    RIGHT_PAREN,
    LEFT_BRACKET,
    RIGHT_BRACKET,
    SEMICOLON,
    COMMA,
    DOT,
    QUESTION,
    COLON,

    // Keywords (55+)
    IF,
    IS,
    IN,
    WHERE,
    SELF,
    FOR,
    FOREACH,
    USE,
    OUT,
    PASS,
    TRUE,
    FALSE,
    NULL_TOKEN,
    SLOT,
    ELSE,
    CONST,
    SHARED,
    EXTEND,
    EXTENDS,
    RETURN,
    IMPORT,
    MODULE,
    STRUCT,
    VERTEX,
    FRAGMENT,
    COMPUTE,
    COMPUTE_GRAPH,
    NODE,
    INPUTS,
    OUTPUTS,
    READONLY,
    READWRITE,
    WRITEONLY,
    SHADER,
    PIPELINE,
    BUFFER,
    CBUFFER,
    SAMPLER,
    TEXTURE2D,
    TEXTURE3D,
    TEXTURECUBE,
    TEXTURE2DARRAY,
    RESOURCES,
    ATTRIBUTES,
    PASS_BLOCK,
    VERTEX_FUNCTION,
    COMPUTE_FUNCTION,
    FRAGMENT_FUNCTION,
    LOOP,
    WHILE,
    UNTIL,
    BY,
    SKIP,
    RANGE,
    IT,
    EVAL,
    UNDERSCORE,
    SWITCH,
    CASE,
    BREAK,
    DISCARD,
    VARIANTS,
    RULES,
    REQUIRE,
    CONFLICT,

    // Special
    ERROR_TOKEN,
    EOF_TOKEN,
    DEFAULT,

    // Reserved 64-bit scalar/vector/matrix type names. These are appended
    // after the first 64 token slots so existing operator bitmasks keep their
    // layout.
    DOUBLE,
    DOUBLE2,
    DOUBLE3,
    DOUBLE4,
    INT64,
    UINT64,
    INT64X2,
    INT64X3,
    INT64X4,
    UINT64X2,
    UINT64X3,
    UINT64X4,
    DMAT2,
    DMAT3,
    DMAT4,
    USING,
    TOKEN_COUNT
};

static_assert(TOKEN_COUNT < 256, "Too many token types for uint8_t");

} // namespace BWSL
