#pragma once
#include "bwsl_ast_soa.h"
#include "bwsl_types.h"
#include "bwsl_utils.h"
#include <cstring>

namespace BWSL {

// Forward declaration
struct Parser;

// Compile-time evaluation cache (moved from deprecated bwsl_eval.h)
struct EvalCache {
    static constexpr u32 CACHE_SIZE = 256;  // Power of 2
    static constexpr u32 INVALID_HASH = 0xFFFFFFFF;
    
    // Parallel arrays for evaluated constants (hot data)
    alignas(64) u32 hashes[CACHE_SIZE];
    alignas(64) LiteralValue values[CACHE_SIZE];
    alignas(64) u8 valueTypes[CACHE_SIZE];  // LiteralValue::Type
    alignas(64) u8 valid[CACHE_SIZE];
    
    // Eval function cache
    alignas(64) u32 functionHashes[64];
    alignas(64) u32 functionIndices[64];  // Index into AST nodes
    u32 functionCount;
    
    void Init() {
        memset(hashes, 0xFF, sizeof(hashes));
        memset(valid, 0, sizeof(valid));
        functionCount = 0;
    }
    
    inline LiteralValue* Lookup(u32 hash) {
        u32 slot = hash & (CACHE_SIZE - 1);
        if (valid[slot] && hashes[slot] == hash) {
            return &values[slot];
        }
        return nullptr;
    }
    
    inline void Insert(u32 hash, const LiteralValue& value) {
        u32 slot = hash & (CACHE_SIZE - 1);
        hashes[slot] = hash;
        values[slot] = value;
        valueTypes[slot] = (u8)value.type;
        valid[slot] = 1;
    }
};

// SoA-specific eval state
struct EvalStateSoA {
    BWSL_Arena* arena;
    Parser* parser;
    AST* ast;
    EvalCache* cache;

    // Stack-based evaluation (no recursion)
    static constexpr u32 MAX_STACK_DEPTH = 128;
    alignas(64) NodeRef nodeStack[MAX_STACK_DEPTH];
    alignas(64) LiteralValue valueStack[MAX_STACK_DEPTH];
    u32 nodeStackPtr;
    u32 valueStackPtr;

    // Error tracking
    bool hasError;
    char errorMsg[256];
    u32 errorLine;
    u32 errorColumn;

    // Limits for compile-time evaluation
    u32 iterationLimit;
    u32 iterationCount;

    // Optional data-oriented comptime binding lookup. The parser/symbol-table
    // path remains the default; the comptime interpreter installs this hook
    // while executing expanded scopes.
    void* comptimeUser;
    bool (*lookupComptimeBinding)(void* user, u32 nameHash, LiteralValue* outValue);
};

// Compile-time evaluator for SoA AST
class CompileTimeEvaluatorSoA {
public:
    // Initialize eval state
    static void Init(EvalStateSoA* state, Parser* parser, AST* ast, EvalCache* cache, BWSL_Arena* arena);

    // Main evaluation entry point
    static bool EvaluateNode(EvalStateSoA* state, NodeRef node, LiteralValue* outValue);

    // Check if expression can be evaluated at compile time
    static bool CanEvaluateNode(EvalStateSoA* state, NodeRef node);

    // Specialized evaluators for common cases
    static bool EvaluateBinaryOp(EvalStateSoA* state, NodeRef node, LiteralValue* outValue);
    static bool EvaluateUnaryOp(EvalStateSoA* state, NodeRef node, LiteralValue* outValue);
    static bool EvaluateFunctionCall(EvalStateSoA* state, NodeRef node, LiteralValue* outValue);

private:
    // Stack manipulation
    static void PushNode(EvalStateSoA* state, NodeRef node);
    static NodeRef PopNode(EvalStateSoA* state);
    static void PushValue(EvalStateSoA* state, const LiteralValue& value);
    static bool PopValue(EvalStateSoA* state, LiteralValue* value);

    // Arithmetic operations
    static bool PerformIntOp(BinaryOpType op, int left, int right, int* result);
    static bool PerformFloatOp(BinaryOpType op, float left, float right, float* result);
    static bool PerformBoolOp(BinaryOpType op, bool left, bool right, bool* result);

    // Error helpers
    static void SetError(EvalStateSoA* state, const char* msg);
};

} // namespace BWSL
