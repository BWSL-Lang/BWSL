#pragma once

#include "bwsl_defs.h"
#include "bwsl_arena.h"
#include "bwsl_types.h"

namespace BWSL {

using BWSL_Arena = Memory::BWEMemoryArena;

// Arena-allocated dynamic array - used by both AST implementations
template<typename T>
struct ArenaArray {
    T* data;
    u32 count;
    u32 capacity;

    ArenaArray() : data(nullptr), count(0), capacity(0) {}

    void Init(BWSL_Arena* arena, u32 initialCapacity = 8) {
        capacity = initialCapacity;
        count = 0;
        data = (T*)arena->Allocate(sizeof(T) * capacity, alignof(T));
    }

    void Push(BWSL_Arena* arena, const T& item) {
        if (count >= capacity) {
            u32 newCapacity = capacity * 2;
            T* newData = (T*)arena->Allocate(sizeof(T) * newCapacity, alignof(T));
            for (u32 i = 0; i < count; ++i) {
                newData[i] = data[i];
            }
            data = newData;
            capacity = newCapacity;
        }
        data[count++] = item;
    }

    T& operator[](u32 index) { return data[index]; }
    const T& operator[](u32 index) const { return data[index]; }
};

struct LiteralValue {
    enum Type { FLOAT, INT, UINT, BOOL, STRING };
    Type type;
    union {
        float floatValue;
        int intValue;
        unsigned int uintValue;
        bool boolValue;
        ArenaString stringValue;
    };
};

// Common AST node types
enum class ASTNodeType : u8 {
    PIPELINE,
    MODULE,
    PASS,
    FUNCTION,
    ATTRIBUTE_DECL,
    RESOURCE_DECL,
    VARIABLE_DECL,
    ASSIGNMENT,
    BINARY_OP,
    UNARY_OP,
    FUNCTION_CALL,
    MEMBER_ACCESS,
    ARRAY_ACCESS,
    LITERAL,
    IDENTIFIER,
    VARIANT_DECL,
    ENUM_DECL,
    BLOCK,
    IF_STATEMENT,
    FOR_CSTYLE,
    FOR_RANGE,
    FOR_COLLECTION,
    LOOP,
    RETURN,
    USE_ATTRIBUTES,
    VERTEX_STAGE,
    FRAGMENT_STAGE,
    COMPUTE_STAGE,
    STRUCT_DECL,
    MODULE_FUNCTION,
    CONSTRAINT_DECL,
    PATTERN_MATCH_ARM,
    PATTERN_MATCH,
    SWITCH_CASE,
    SWITCH,
    TERNARY_EXPRESSION,
    BREAK_STATEMENT,
    SKIP_STATEMENT,
    DISCARD_STATEMENT,
    COMPUTE_GRAPH,
    TYPE_PATTERN_MATCH,
    TYPE_PATTERN_ARM,
    INVALID = 0xFF
};

enum class ResourceType : u8 {
    CBUFFER,
    BUFFER,
    TEXTURE2D,
    TEXTURE3D,
    TEXTURECUBE,
    TEXTURE2DARRAY,
    SAMPLER
};

enum class StorageClass : u8 {
    Default = 0,
    Shared = 1,
};

enum class BinaryOpType : u8 {
    ADD,
    SUBTRACT,
    MULTIPLY,
    DIVIDE,
    EQUALS,
    NOT_EQUALS,
    LESS,
    GREATER,
    AND,
    OR,
    MODULO,
    LESS_EQUAL,
    GREATER_EQUAL,
    BITWISE_AND,
    BITWISE_OR,
    BITWISE_XOR,
    LEFT_SHIFT,
    RIGHT_SHIFT,
};

enum class UnaryOpType : u8 {
    NEGATE,
    NOT,
    BITWISE_NOT,
    PRE_INCREMENT,   // ++x
    PRE_DECREMENT,   // --x
    POST_INCREMENT,  // x++
    POST_DECREMENT,  // x--
    ADDRESS_OF,      // ^x (get address)
    DEREFERENCE,     // x^ (dereference pointer)
};

namespace VariantDecl {
    constexpr u32 AUTO_VALUE = 0xFFFFFFFF;
}

// Function call flags - used by both AST implementations
namespace FunctionCallFlags {
    constexpr u8 IS_INTRINSIC         = 0x01;
    constexpr u8 IS_MODULE_FUNCTION   = 0x02;
    constexpr u8 TYPE_VALIDATED       = 0x04;
    constexpr u8 NEEDS_CUSTOM_IMPL    = 0x08;
    constexpr u8 IS_TEXTURE_OP        = 0x10;
    constexpr u8 IS_WAVE_OP           = 0x20;
    constexpr u8 IS_COMPILE_TIME_EVAL = 0x40;
    constexpr u8 IS_METHOD_CALL       = 0x80;  // Method call on object (e.g., shape.distance(p))
}

} // namespace BWSL
