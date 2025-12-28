// bwsl_token_stream.h
// SoA token storage with portable SIMD via SIMDe
// This is the canonical token handling header - pure SoA design
#pragma once

// SIMDe for portable SIMD (works on x86, ARM, and falls back to scalar)
// Must be included BEFORE bwsl_defs.h due to macro conflicts (f32, u32, etc.)
#define SIMDE_ENABLE_NATIVE_ALIASES
#include "vendor/simde/simde/x86/avx2.h"
#include "vendor/simde/simde/x86/sse2.h"

// Token type definitions (minimal, no dependencies)
#include "bwsl_token_defs.h"

// BWSL headers (after SIMDe to avoid macro conflicts)
#include "bwsl_defs.h"
#include "bwsl_arena.h"

#include <cstring>
#include <string_view>

namespace BWSL {

// Forward declaration
using BWSL_Arena = Memory::BWEMemoryArena;

//==============================================================================
// TokenRef - lightweight index into TokenStream (replaces Token struct)
// Just a u32 index - all data accessed through TokenStream
//==============================================================================

using TokenRef = u32;
static constexpr TokenRef INVALID_TOKEN = 0xFFFFFFFF;

//==============================================================================
// Token length lookup table
//==============================================================================

class TokenLengthTable {
public:
    static constexpr uint8_t VARIABLE_LENGTH = 0;

    static constexpr uint8_t lengths[256] = {
        // Single character tokens
        [LEFT_BRACE] = 1,
        [RIGHT_BRACE] = 1,
        [LEFT_PAREN] = 1,
        [RIGHT_PAREN] = 1,
        [LEFT_BRACKET] = 1,
        [RIGHT_BRACKET] = 1,
        [SEMICOLON] = 1,
        [COMMA] = 1,
        [DOT] = 1,
        [QUESTION] = 1,
        [COLON] = 1,
        [PLUS] = 1,
        [MINUS] = 1,
        [MULTIPLY] = 1,
        [DIVIDE] = 1,
        [MODULO] = 1,
        [ASSIGN] = 1,
        [NOT] = 1,
        [LESS] = 1,
        [GREATER] = 1,
        [ERROR_TOKEN] = 1,
        [T] = 1,
        [U] = 1,
        [V] = 1,
        // Two character tokens
        [DOUBLE_COLON] = 2,
        [ARROW] = 2,
        [PLUS_ASSIGN] = 2,
        [MINUS_ASSIGN] = 2,
        [MULTIPLY_ASSIGN] = 2,
        [DIVIDE_ASSIGN] = 2,
        [EQUALS] = 2,
        [NOT_EQUALS] = 2,
        [LESS_EQUAL] = 2,
        [GREATER_EQUAL] = 2,
        [AND] = 2,
        [OR] = 2,
        [BITWISE_AND] = 1,
        [BITWISE_OR] = 1,
        [BITWISE_XOR] = 1,
        [BITWISE_NOT] = 1,
        [LEFT_SHIFT] = 2,
        [RIGHT_SHIFT] = 2,
        [IF] = 2,
        [IS] = 2,
        [AS] = 2,
        [IT] = 2,
        [IN] = 2,
        [DOT_DOT] = 2,
        [INCREMENT] = 2,
        [DECREMENT] = 2,
        [BY] = 2,
        // Three character keywords
        [DOT_DOT_EQUAL] = 3,
        [FOR] = 3,
        [INT] = 3,
        [USE] = 3,
        [MIX] = 3,
        [OUT] = 3,
        // Four character keywords
        [PASS] = 4,
        [TRUE] = 4,
        [BOOL] = 4,
        [MAT2] = 4,
        [MAT3] = 4,
        [MAT4] = 4,
        [INT2] = 4,
        [INT3] = 4,
        [INT4] = 4,
        [UINT] = 4,
        [ELSE] = 4,
        [NULL_TOKEN] = 4,
        [SLOT] = 4,
        [ENUM] = 4,
        [SELF] = 4,
        [LOOP] = 4,
        [NODE] = 4,
        [SKIP] = 4,
        [EVAL] = 4,
        [CASE] = 4,
        [VOID] = 4,
        // Five character keywords
        [FLOAT] = 5,
        [WHERE] = 5,
        [FALSE] = 5,
        [CONST] = 5,
        [BREAK] = 5,
        [UINT2] = 5,
        [UINT3] = 5,
        [UINT4] = 5,
        [UNTIL] = 5,
        [RANGE] = 5,
        // Six character keywords
        [VERTEX] = 6,
        [SHADER] = 6,
        [IMPORT] = 6,
        [RETURN] = 6,
        [FLOAT2] = 6,
        [FLOAT3] = 6,
        [FLOAT4] = 6,
        [MODULE] = 6,
        [STRUCT] = 6,
        [BUFFER] = 6,
        [EXTEND] = 6,
        [SWITCH] = 6,
        [INPUTS] = 6,
        // Seven character keywords
        [CBUFFER] = 7,
        [SAMPLER] = 7,
        [COMPUTE] = 7,
        [EXTENDS] = 7,
        [FOREACH] = 7,
        [DISCARD] = 7,
        [DEFAULT] = 7,
        [OUTPUTS] = 7,
        // Eight character keywords
        [PIPELINE] = 8,
        [FRAGMENT] = 8,
        [READONLY] = 8,
        // Nine character keywords
        [TEXTURE2D] = 9,
        [TEXTURE3D] = 9,
        [RESOURCES] = 9,
        [READWRITE] = 9,
        [WRITEONLY] = 9,
        // Ten character keywords
        [ATTRIBUTES] = 10,
        [PASS_BLOCK] = 10,
        [CONSTRAINT] = 10,
        // Eleven character keywords
        [TEXTURECUBE] = 11,
        // Thirteen character keywords
        [COMPUTE_GRAPH] = 13,
        // Fourteen character keywords
        [TEXTURE2DARRAY] = 14,
        // Fifteen character keywords
        [VERTEX_FUNCTION] = 15,
        // Sixteen character keywords
        [COMPUTE_FUNCTION] = 16,
        // Seventeen character keywords
        [FRAGMENT_FUNCTION] = 17,
        // Variable length tokens
        [IDENTIFIER] = VARIABLE_LENGTH,
        [NUMBER] = VARIABLE_LENGTH,
        [STRING] = VARIABLE_LENGTH,
        [AT] = VARIABLE_LENGTH,
        [EOF_TOKEN] = VARIABLE_LENGTH,
    };

    static uint8_t get(uint8_t type) {
        return lengths[type];
    }
};

//==============================================================================
// TokenTypeName - for debugging
//==============================================================================

inline const char* TokenTypeName(TokenType type) {
    switch(type) {
        case LEFT_BRACE: return "LEFT_BRACE";
        case RIGHT_BRACE: return "RIGHT_BRACE";
        case LEFT_PAREN: return "LEFT_PAREN";
        case RIGHT_PAREN: return "RIGHT_PAREN";
        case LEFT_BRACKET: return "LEFT_BRACKET";
        case RIGHT_BRACKET: return "RIGHT_BRACKET";
        case SEMICOLON: return "SEMICOLON";
        case COMMA: return "COMMA";
        case DOT: return "DOT";
        case QUESTION: return "QUESTION";
        case COLON: return "COLON";
        case PLUS: return "PLUS";
        case MINUS: return "MINUS";
        case MULTIPLY: return "MULTIPLY";
        case DIVIDE: return "DIVIDE";
        case MODULO: return "MODULO";
        case ASSIGN: return "ASSIGN";
        case NOT: return "NOT";
        case LESS: return "LESS";
        case GREATER: return "GREATER";
        case DOUBLE_COLON: return "DOUBLE_COLON";
        case ARROW: return "ARROW";
        case PLUS_ASSIGN: return "PLUS_ASSIGN";
        case MINUS_ASSIGN: return "MINUS_ASSIGN";
        case MULTIPLY_ASSIGN: return "MULTIPLY_ASSIGN";
        case DIVIDE_ASSIGN: return "DIVIDE_ASSIGN";
        case EQUALS: return "EQUALS";
        case NOT_EQUALS: return "NOT_EQUALS";
        case LESS_EQUAL: return "LESS_EQUAL";
        case GREATER_EQUAL: return "GREATER_EQUAL";
        case AND: return "AND";
        case OR: return "OR";
        case BITWISE_AND: return "BITWISE_AND";
        case BITWISE_OR: return "BITWISE_OR";
        case BITWISE_XOR: return "BITWISE_XOR";
        case BITWISE_NOT: return "BITWISE_NOT";
        case LEFT_SHIFT: return "LEFT_SHIFT";
        case RIGHT_SHIFT: return "RIGHT_SHIFT";
        case IF: return "IF";
        case AS: return "AS";
        case FOR: return "FOR";
        case INT: return "INT";
        case USE: return "USE";
        case MIX: return "MIX";
        case OUT: return "OUT";
        case PASS: return "PASS";
        case TRUE: return "TRUE";
        case FALSE: return "FALSE";
        case BOOL: return "BOOL";
        case MAT2: return "MAT2";
        case MAT3: return "MAT3";
        case MAT4: return "MAT4";
        case INT2: return "INT2";
        case INT3: return "INT3";
        case INT4: return "INT4";
        case UINT: return "UINT";
        case UINT2: return "UINT2";
        case UINT3: return "UINT3";
        case UINT4: return "UINT4";
        case ELSE: return "ELSE";
        case NULL_TOKEN: return "NULL";
        case SLOT: return "SLOT";
        case FLOAT: return "FLOAT";
        case FLOAT2: return "FLOAT2";
        case FLOAT3: return "FLOAT3";
        case FLOAT4: return "FLOAT4";
        case CONST: return "CONST";
        case VERTEX: return "VERTEX";
        case SHADER: return "SHADER";
        case IMPORT: return "IMPORT";
        case RETURN: return "RETURN";
        case MODULE: return "MODULE";
        case STRUCT: return "STRUCT";
        case BUFFER: return "BUFFER";
        case CBUFFER: return "CBUFFER";
        case SAMPLER: return "SAMPLER";
        case COMPUTE: return "COMPUTE";
        case COMPUTE_GRAPH: return "COMPUTE_GRAPH";
        case NODE: return "NODE";
        case INPUTS: return "INPUTS";
        case OUTPUTS: return "OUTPUTS";
        case READONLY: return "READONLY";
        case READWRITE: return "READWRITE";
        case WRITEONLY: return "WRITEONLY";
        case PIPELINE: return "PIPELINE";
        case FRAGMENT: return "FRAGMENT";
        case TEXTURE2D: return "TEXTURE2D";
        case TEXTURE3D: return "TEXTURE3D";
        case TEXTURECUBE: return "TEXTURECUBE";
        case TEXTURE2DARRAY: return "TEXTURE2DARRAY";
        case RESOURCES: return "RESOURCES";
        case ATTRIBUTES: return "ATTRIBUTES";
        case PASS_BLOCK: return "PASS_BLOCK";
        case VERTEX_FUNCTION: return "VERTEX_FUNCTION";
        case COMPUTE_FUNCTION: return "COMPUTE_FUNCTION";
        case FRAGMENT_FUNCTION: return "FRAGMENT_FUNCTION";
        case IDENTIFIER: return "IDENTIFIER";
        case NUMBER: return "NUMBER";
        case STRING: return "STRING";
        case AT: return "AT";
        case ERROR_TOKEN: return "ERROR_TOKEN";
        case EOF_TOKEN: return "EOF";
        case SELF: return "self";
        case ENUM: return "enum";
        case CONSTRAINT: return "constraint";
        case T: return "T";
        case U: return "U";
        case V: return "V";
        case LOOP: return "LOOP";
        case UNTIL: return "UNTIL";
        case BY: return "BY";
        case SKIP: return "SKIP";
        case RANGE: return "RANGE";
        case IT: return "IT";
        case IN: return "IN";
        case FOREACH: return "FOREACH";
        case EVAL: return "EVAL";
        case DOT_DOT: return "DOT_DOT";
        case DOT_DOT_EQUAL: return "DOT_DOT_EQUAL";
        case INCREMENT: return "INCREMENT";
        case DECREMENT: return "DECREMENT";
        case BREAK: return "BREAK";
        case DISCARD: return "DISCARD";
        case CASE: return "CASE";
        case SWITCH: return "SWITCH";
        case DEFAULT: return "DEFAULT";
        case VOID: return "VOID";
        default: return "UNKNOWN";
    }
}

//==============================================================================
// Likely/unlikely hints for branch prediction
//==============================================================================

#if defined(__GNUC__) || defined(__clang__)
#define BWSL_LIKELY(x)   __builtin_expect(!!(x), 1)
#define BWSL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define BWSL_LIKELY(x)   (x)
#define BWSL_UNLIKELY(x) (x)
#endif

//==============================================================================
// Identifier continuation character table
//==============================================================================

static constexpr bool IsIdentCont[256] = {
    ['0' ... '9'] = true,
    ['A' ... 'Z'] = true,
    ['a' ... 'z'] = true,
    ['_'] = true,
};

//==============================================================================
// TokenStream - SoA token storage with SIMD search
// All token data is accessed via index (TokenRef)
//==============================================================================

class alignas(64) TokenStream {
    static constexpr size_t CACHE_LINE = 64;
    static constexpr size_t PREFETCH_DISTANCE = 4;

    // Hot data - frequently accessed together (fits in 1 cache line)
    struct alignas(64) HotData {
        u32 count;
        u32 capacity;
        const char* source_base;
        u32* offsets;    // Offset into source for each token
        u8* types;       // TokenType for each token
        u16* lengths;    // Pre-computed length for each token
        u8 padding[16];
    } hot;

    // Cold data - rarely accessed
    BWSL_Arena* arena;
    u32* line_starts;
    u32 line_count;

    struct Stats {
        u32 identifier_count;
        u32 keyword_count;
        u32 operator_count;
        u32 total_bytes_scanned;
    } stats;

public:
    void Init(BWSL_Arena* arena_, const char* source, size_t source_len) {
        arena = arena_;

        size_t estimated_tokens = (source_len / 6) + 128;
        estimated_tokens = ((estimated_tokens + 15) / 16) * 16;

        hot.capacity = static_cast<u32>(estimated_tokens);
        hot.count = 0;
        hot.source_base = source;

        size_t offsets_size = AlignUp(sizeof(u32) * hot.capacity, CACHE_LINE);
        size_t types_size = AlignUp(sizeof(u8) * hot.capacity, CACHE_LINE);
        size_t lengths_size = AlignUp(sizeof(u16) * hot.capacity, CACHE_LINE);
        size_t total_size = offsets_size + types_size + lengths_size;

        void* memory = arena->Allocate(total_size, CACHE_LINE);

        char* mem_base = static_cast<char*>(memory);
        hot.offsets = static_cast<u32*>(memory);
        hot.types = reinterpret_cast<u8*>(mem_base + offsets_size);
        hot.lengths = reinterpret_cast<u16*>(mem_base + offsets_size + types_size);

        memset(hot.types, 0, types_size);
        memset(&stats, 0, sizeof(stats));
        line_starts = nullptr;
        line_count = 0;
    }

    //==========================================================================
    // Push operations - returns TokenRef (index) of pushed token
    //==========================================================================

    // Push token (computes length automatically)
    inline TokenRef Push(u32 offset, uint8_t type) {
        if ((hot.count & 15) == 12) {
            _mm_prefetch(reinterpret_cast<const char*>(&hot.offsets[hot.count + 16]), _MM_HINT_T0);
            _mm_prefetch(reinterpret_cast<const char*>(&hot.types[hot.count + 16]), _MM_HINT_T0);
        }

        TokenRef idx = hot.count;
        hot.offsets[idx] = offset;
        hot.types[idx] = type;

        if (type == TokenType::IDENTIFIER || type == TokenType::STRING ||
            type == TokenType::NUMBER || type == TokenType::AT) {
            hot.lengths[idx] = ComputeTokenLength(offset, type);
        } else {
            hot.lengths[idx] = TokenLengthTable::get(type);
        }

        hot.count++;

        if (BWSL_UNLIKELY(hot.count >= hot.capacity)) {
            Grow();
        }

        return idx;
    }

    // Push with pre-computed length
    inline TokenRef PushWithLength(u32 offset, uint8_t type, u16 length) {
        if ((hot.count & 15) == 12) {
            _mm_prefetch(reinterpret_cast<const char*>(&hot.offsets[hot.count + 16]), _MM_HINT_T0);
            _mm_prefetch(reinterpret_cast<const char*>(&hot.types[hot.count + 16]), _MM_HINT_T0);
        }

        TokenRef idx = hot.count;
        hot.offsets[idx] = offset;
        hot.types[idx] = type;
        hot.lengths[idx] = length;
        hot.count++;

        if (BWSL_UNLIKELY(hot.count >= hot.capacity)) {
            Grow();
        }

        return idx;
    }

    //==========================================================================
    // Access operations - all via TokenRef (index)
    //==========================================================================

    TokenType GetType(TokenRef idx) const {
        return static_cast<TokenType>(hot.types[idx]);
    }

    u32 GetOffset(TokenRef idx) const {
        return hot.offsets[idx];
    }

    u16 GetLength(TokenRef idx) const {
        return hot.lengths[idx];
    }

    const char* GetStart(TokenRef idx) const {
        return hot.source_base + hot.offsets[idx];
    }

    std::string_view GetValue(TokenRef idx) const {
        return std::string_view(hot.source_base + hot.offsets[idx], hot.lengths[idx]);
    }

    //==========================================================================
    // Type checking
    //==========================================================================

    inline bool IsType(TokenRef idx, TokenType type) const {
        _mm_prefetch(reinterpret_cast<const char*>(&hot.types[idx + PREFETCH_DISTANCE]), _MM_HINT_T0);
        return hot.types[idx] == static_cast<u8>(type);
    }

    inline bool IsEOF(TokenRef idx) const {
        return idx >= hot.count || hot.types[idx] == static_cast<u8>(TokenType::EOF_TOKEN);
    }

    bool HasSequence(TokenRef start_idx, const TokenType* types, u32 count) const {
        if (start_idx + count > hot.count) return false;
        for (u32 i = 0; i < count; i++) {
            if (hot.types[start_idx + i] != static_cast<u8>(types[i])) {
                return false;
            }
        }
        return true;
    }

    //==========================================================================
    // SIMD search operations
    //==========================================================================

    TokenRef FindNext(TokenRef start_idx, TokenType type) const {
        u8 target = static_cast<u8>(type);
        u32 remaining = hot.count - start_idx;

        if (remaining >= 32) {
            __m256i target_vec = _mm256_set1_epi8(static_cast<char>(target));
            u32 i = start_idx;
            u32 aligned_end = start_idx + ((remaining / 32) * 32);

            for (; i < aligned_end; i += 32) {
                __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&hot.types[i]));
                __m256i cmp = _mm256_cmpeq_epi8(chunk, target_vec);
                int mask = _mm256_movemask_epi8(cmp);

                if (mask != 0) {
#if defined(__GNUC__) || defined(__clang__)
                    return i + static_cast<u32>(__builtin_ctz(static_cast<unsigned>(mask)));
#elif defined(_MSC_VER)
                    unsigned long index;
                    _BitScanForward(&index, static_cast<unsigned long>(mask));
                    return i + static_cast<u32>(index);
#else
                    for (int j = 0; j < 32; j++) {
                        if (mask & (1 << j)) return i + j;
                    }
#endif
                }
            }
            start_idx = i;
        }

        for (u32 i = start_idx; i < hot.count; i++) {
            if (hot.types[i] == target) return i;
        }
        return hot.count;
    }

    TokenRef FindNextAny(TokenRef start_idx, u64 type_mask) const {
        for (u32 i = start_idx; i < hot.count; i++) {
            if ((1ULL << hot.types[i]) & type_mask) {
                return i;
            }
        }
        return hot.count;
    }

    //==========================================================================
    // Stream metadata
    //==========================================================================

    const char* GetSourceBase() const { return hot.source_base; }
    u32 Count() const { return hot.count; }
    const Stats& GetStats() const { return stats; }

private:
    static size_t AlignUp(size_t size, size_t alignment) {
        return (size + alignment - 1) & ~(alignment - 1);
    }

#if defined(__GNUC__) || defined(__clang__)
    [[gnu::cold]]
#endif
    void Grow() { GrowTo(hot.capacity * 2); }

#if defined(__GNUC__) || defined(__clang__)
    [[gnu::cold]]
#endif
    void GrowTo(u32 new_capacity) {
        new_capacity = ((new_capacity + 15) / 16) * 16;

        size_t offsets_size = AlignUp(sizeof(u32) * new_capacity, CACHE_LINE);
        size_t types_size = AlignUp(sizeof(u8) * new_capacity, CACHE_LINE);
        size_t lengths_size = AlignUp(sizeof(u16) * new_capacity, CACHE_LINE);

        void* new_memory = arena->Allocate(offsets_size + types_size + lengths_size, CACHE_LINE);

        char* mem_base = static_cast<char*>(new_memory);
        u32* new_offsets = static_cast<u32*>(new_memory);
        u8* new_types = reinterpret_cast<u8*>(mem_base + offsets_size);
        u16* new_lengths = reinterpret_cast<u16*>(mem_base + offsets_size + types_size);

        memcpy(new_offsets, hot.offsets, sizeof(u32) * hot.count);
        memcpy(new_types, hot.types, sizeof(u8) * hot.count);
        memcpy(new_lengths, hot.lengths, sizeof(u16) * hot.count);

        hot.offsets = new_offsets;
        hot.types = new_types;
        hot.lengths = new_lengths;
        hot.capacity = new_capacity;
    }

    u16 ComputeTokenLength(u32 offset, u8 type) const {
        const char* start = hot.source_base + offset;
        const char* end = start;
        const char* max_end = start + 65536;

        switch (static_cast<TokenType>(type)) {
            case TokenType::IDENTIFIER: {
                while (end < max_end && *end && IsIdentCont[static_cast<unsigned char>(*end)]) {
                    end++;
                }
                break;
            }

            case TokenType::NUMBER: {
                auto is_digit = [](unsigned c) { return unsigned(c - '0') < 10; };
                auto is_hex_digit = [](unsigned c) {
                    if (unsigned(c - '0') < 10) return true;
                    unsigned lo = c | 0x20;
                    return lo >= 'a' && lo <= 'f';
                };

                if (end + 1 < max_end && end[0] == '0' && (end[1] == 'x' || end[1] == 'X')) {
                    end += 2;
                    while (end < max_end && *end && is_hex_digit(static_cast<unsigned>(*end))) ++end;
                    if (end < max_end && (*end == 'u' || *end == 'U')) ++end;
                    break;
                }

                if (end + 1 < max_end && end[0] == '0' && (end[1] == 'b' || end[1] == 'B')) {
                    end += 2;
                    while (end < max_end && *end && (*end == '0' || *end == '1')) ++end;
                    if (end < max_end && (*end == 'u' || *end == 'U')) ++end;
                    break;
                }

                while (end < max_end && *end && is_digit(static_cast<unsigned>(*end))) ++end;

                if (end + 1 < max_end && *end == '.' && is_digit(static_cast<unsigned>(*(end + 1)))) {
                    ++end;
                    while (end < max_end && *end && is_digit(static_cast<unsigned>(*end))) ++end;
                }

                if (end + 2 < max_end && (*end == 'e' || *end == 'E')) {
                    char c1 = *(end + 1);
                    char c2 = *(end + 2);
                    bool validExp = is_digit(static_cast<unsigned>(c1)) ||
                                    ((c1 == '+' || c1 == '-') && is_digit(static_cast<unsigned>(c2)));
                    if (validExp) {
                        ++end;
                        if (end < max_end && (*end == '+' || *end == '-')) ++end;
                        while (end < max_end && *end && is_digit(static_cast<unsigned>(*end))) ++end;
                    }
                }

                if (end < max_end && (*end == 'f' || *end == 'F' || *end == 'u' || *end == 'U')) {
                    ++end;
                }
                break;
            }

            case TokenType::STRING: {
                while (end < max_end && *end && *end != '"') {
                    if (*end == '\\' && end + 1 < max_end && *(end + 1)) {
                        end += 2;
                    } else {
                        end++;
                    }
                }
                break;
            }

            case TokenType::AT: {
                while (end < max_end && *end && IsIdentCont[static_cast<unsigned char>(*end)]) {
                    end++;
                }
                break;
            }

            default:
                return TokenLengthTable::get(type);
        }

        size_t result = static_cast<size_t>(end - start);
        return static_cast<u16>(result > 65535 ? 0 : result);
    }
};

} // namespace BWSL
