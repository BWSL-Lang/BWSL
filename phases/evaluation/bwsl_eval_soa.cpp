#include "bwsl_eval_soa.h"
#include "bwsl_parser_soa.h"
#include <climits>
#include <cstring>
#include <cmath>

namespace BWSL {

// Saturating float-to-int conversion. Plain `(int)x` is undefined behaviour
// when x is NaN or outside the representable range; this clamps into a
// well-defined range so the constant folder never trips UBSan on crafted
// input.
static inline int SafeFloatToInt(float x) {
    if (std::isnan(x)) return 0;
    if (x >= (float)INT_MAX) return INT_MAX;
    if (x <= (float)INT_MIN) return INT_MIN;
    return (int)x;
}

// Pre-computed hashes for vector constructor names
static const u32 kFloat2Hash = Utils::HashStr("float2");
static const u32 kFloat3Hash = Utils::HashStr("float3");
static const u32 kFloat4Hash = Utils::HashStr("float4");
static const u32 kInt2Hash = Utils::HashStr("int2");
static const u32 kInt3Hash = Utils::HashStr("int3");
static const u32 kInt4Hash = Utils::HashStr("int4");
static const u32 kVec2Hash = Utils::HashStr("vec2");
static const u32 kVec3Hash = Utils::HashStr("vec3");
static const u32 kVec4Hash = Utils::HashStr("vec4");
static const u32 kIvec2Hash = Utils::HashStr("ivec2");
static const u32 kIvec3Hash = Utils::HashStr("ivec3");
static const u32 kIvec4Hash = Utils::HashStr("ivec4");
static const u32 kFloatHash = Utils::HashStr("float");
static const u32 kIntHash = Utils::HashStr("int");
static const u32 kUintHash = Utils::HashStr("uint");
static const u32 kBoolHash = Utils::HashStr("bool");

static bool IsScalarConstructor(u32 nameHash, LiteralValue::Type* outType = nullptr) {
    if (nameHash == kFloatHash) {
        if (outType) *outType = LiteralValue::FLOAT;
        return true;
    }
    if (nameHash == kIntHash) {
        if (outType) *outType = LiteralValue::INT;
        return true;
    }
    if (nameHash == kUintHash) {
        if (outType) *outType = LiteralValue::UINT;
        return true;
    }
    if (nameHash == kBoolHash) {
        if (outType) *outType = LiteralValue::BOOL;
        return true;
    }
    return false;
}

// Check if a function call is a vector constructor
static bool IsVectorConstructor(u32 nameHash, LiteralValue::Type* outType = nullptr) {
    if (nameHash == kFloat2Hash || nameHash == kVec2Hash) {
        if (outType) *outType = LiteralValue::FLOAT2;
        return true;
    }
    if (nameHash == kFloat3Hash || nameHash == kVec3Hash) {
        if (outType) *outType = LiteralValue::FLOAT3;
        return true;
    }
    if (nameHash == kFloat4Hash || nameHash == kVec4Hash) {
        if (outType) *outType = LiteralValue::FLOAT4;
        return true;
    }
    if (nameHash == kInt2Hash || nameHash == kIvec2Hash) {
        if (outType) *outType = LiteralValue::INT2;
        return true;
    }
    if (nameHash == kInt3Hash || nameHash == kIvec3Hash) {
        if (outType) *outType = LiteralValue::INT3;
        return true;
    }
    if (nameHash == kInt4Hash || nameHash == kIvec4Hash) {
        if (outType) *outType = LiteralValue::INT4;
        return true;
    }
    return false;
}

void CompileTimeEvaluatorSoA::Init(EvalStateSoA* state, Parser* parser, AST* ast, EvalCache* cache, BWSL_Arena* arena) {
    state->arena = arena;
    state->parser = parser;
    state->ast = ast;
    state->cache = cache;
    state->nodeStackPtr = 0;
    state->valueStackPtr = 0;
    state->hasError = false;
    state->errorMsg[0] = '\0';
    state->errorLine = 0;
    state->errorColumn = 0;
    state->iterationLimit = 10000;
    state->iterationCount = 0;
    state->comptimeUser = nullptr;
    state->lookupComptimeBinding = nullptr;
}

void CompileTimeEvaluatorSoA::SetError(EvalStateSoA* state, const char* msg) {
    state->hasError = true;
    strncpy(state->errorMsg, msg, sizeof(state->errorMsg) - 1);
    state->errorMsg[sizeof(state->errorMsg) - 1] = '\0';
}

void CompileTimeEvaluatorSoA::PushNode(EvalStateSoA* state, NodeRef node) {
    if (state->nodeStackPtr < EvalStateSoA::MAX_STACK_DEPTH) {
        state->nodeStack[state->nodeStackPtr++] = node;
    } else {
        SetError(state, "Eval stack overflow");
    }
}

NodeRef CompileTimeEvaluatorSoA::PopNode(EvalStateSoA* state) {
    if (state->nodeStackPtr > 0) {
        return state->nodeStack[--state->nodeStackPtr];
    }
    return NodeRef::Null();
}

void CompileTimeEvaluatorSoA::PushValue(EvalStateSoA* state, const LiteralValue& value) {
    if (state->valueStackPtr < EvalStateSoA::MAX_STACK_DEPTH) {
        state->valueStack[state->valueStackPtr++] = value;
    } else {
        SetError(state, "Value stack overflow");
    }
}

bool CompileTimeEvaluatorSoA::PopValue(EvalStateSoA* state, LiteralValue* value) {
    if (state->valueStackPtr > 0) {
        *value = state->valueStack[--state->valueStackPtr];
        return true;
    }
    SetError(state, "Value stack underflow");
    return false;
}

bool CompileTimeEvaluatorSoA::PerformIntOp(BinaryOpType op, int left, int right, int* result) {
    // Use unsigned arithmetic for +, -, *, << so signed overflow (which is
    // undefined behaviour in C++) cannot be triggered by hostile or malformed
    // inputs. This matches the modular wrap semantics that SPIR-V, HLSL and
    // GLSL use for integer ops at run time.
    unsigned uleft  = static_cast<unsigned>(left);
    unsigned uright = static_cast<unsigned>(right);
    switch (op) {
        case BinaryOpType::ADD:      *result = (int)(uleft + uright); return true;
        case BinaryOpType::SUBTRACT: *result = (int)(uleft - uright); return true;
        case BinaryOpType::MULTIPLY: *result = (int)(uleft * uright); return true;
        case BinaryOpType::DIVIDE:
            // INT_MIN / -1 overflows; treat as invalid fold (runtime will wrap).
            if (right == 0) return false;
            if (left == INT_MIN && right == -1) return false;
            *result = left / right;
            return true;
        case BinaryOpType::MODULO:
            if (right == 0) return false;
            if (left == INT_MIN && right == -1) { *result = 0; return true; }
            *result = left % right;
            return true;
        case BinaryOpType::EQUALS: *result = (left == right) ? 1 : 0; return true;
        case BinaryOpType::NOT_EQUALS: *result = (left != right) ? 1 : 0; return true;
        case BinaryOpType::LESS: *result = (left < right) ? 1 : 0; return true;
        case BinaryOpType::GREATER: *result = (left > right) ? 1 : 0; return true;
        case BinaryOpType::LESS_EQUAL: *result = (left <= right) ? 1 : 0; return true;
        case BinaryOpType::GREATER_EQUAL: *result = (left >= right) ? 1 : 0; return true;
        case BinaryOpType::BITWISE_AND: *result = left & right; return true;
        case BinaryOpType::BITWISE_OR: *result = left | right; return true;
        case BinaryOpType::BITWISE_XOR: *result = left ^ right; return true;
        case BinaryOpType::LEFT_SHIFT: {
            // Refuse to fold when the shift count is out of range. Runtime
            // behaviour in every target backend is "undefined" here, so the
            // safest option is to not constant-fold it.
            if (right < 0 || right >= 32) return false;
            *result = (int)(uleft << (unsigned)right);
            return true;
        }
        case BinaryOpType::RIGHT_SHIFT: {
            if (right < 0 || right >= 32) return false;
            // Arithmetic right shift: SPIR-V OpShiftRightArithmetic, HLSL
            // signed `>>` and GLSL `>>` on signed types all sign-extend.
            // Implementation-defined in C++, but GCC/Clang both sign-extend,
            // which matches target semantics.
            *result = left >> right;
            return true;
        }
        default: return false;
    }
}

bool CompileTimeEvaluatorSoA::PerformFloatOp(BinaryOpType op, float left, float right, float* result) {
    switch (op) {
        case BinaryOpType::ADD: *result = left + right; return true;
        case BinaryOpType::SUBTRACT: *result = left - right; return true;
        case BinaryOpType::MULTIPLY: *result = left * right; return true;
        case BinaryOpType::DIVIDE:
            if (right == 0.0f) return false;
            *result = left / right;
            return true;
        case BinaryOpType::MODULO:
            if (right == 0.0f) return false;
            *result = std::fmod(left, right);
            return true;
        default: return false;
    }
}

bool CompileTimeEvaluatorSoA::PerformBoolOp(BinaryOpType op, bool left, bool right, bool* result) {
    switch (op) {
        case BinaryOpType::AND: *result = left && right; return true;
        case BinaryOpType::OR: *result = left || right; return true;
        case BinaryOpType::EQUALS: *result = (left == right); return true;
        case BinaryOpType::NOT_EQUALS: *result = (left != right); return true;
        default: return false;
    }
}

bool CompileTimeEvaluatorSoA::CanEvaluateNode(EvalStateSoA* state, NodeRef node) {
    if (node.IsNull()) return false;

    switch (node.Type()) {
        case ASTNodeType::LITERAL:
            return true;

        case ASTNodeType::IDENTIFIER: {
            // Check if identifier refers to an eval constant
            const IdentifierData& ident = state->ast->GetIdentifier(node);
            if (state->lookupComptimeBinding &&
                state->lookupComptimeBinding(state->comptimeUser, ident.name.nameHash, nullptr)) {
                return true;
            }
            if (state->parser->allowBareVariantLookup &&
                state->parser->LookupActiveVariantBinding(ident.name.nameHash)) {
                return true;
            }
            Symbol* sym = SymbolTable::LookupAny(&state->parser->symbolTable, ident.name);
            if (sym) {
                if (sym->kind == SymbolKind::EVAL_CONSTANT) return true;
                if (sym->kind == SymbolKind::VARIABLE) {
                    VariableData& var = state->parser->symbolTable.variables[sym->index];
                    if (var.isEval) return true;
                }
            }
            return false;
        }

        case ASTNodeType::BINARY_OP: {
            const BinaryOpData& binOp = state->ast->GetBinaryOp(node);
            return CanEvaluateNode(state, binOp.left) && CanEvaluateNode(state, binOp.right);
        }

        case ASTNodeType::UNARY_OP: {
            const UnaryOpData& unaryOp = state->ast->GetUnaryOp(node);
            return CanEvaluateNode(state, unaryOp.operand);
        }

        case ASTNodeType::FUNCTION_CALL: {
            // Only certain intrinsics can be evaluated at compile time
            const FunctionCallData& func = state->ast->GetFunctionCall(node);

            if (IsScalarConstructor(func.name.nameHash)) {
                if (func.arguments.count != 1) return false;
                return CanEvaluateNode(state, func.arguments[0]);
            }

            // Check for vector constructors first (float2, float3, float4, etc.)
            if (IsVectorConstructor(func.name.nameHash)) {
                // All arguments must be evaluable
                for (u32 i = 0; i < func.arguments.count; i++) {
                    if (!CanEvaluateNode(state, func.arguments[i])) return false;
                }
                return true;
            }

            if (!(func.flags & FunctionCallFlags::IS_INTRINSIC)) {
                // Check for user-defined eval functions
                u32 argCount = func.arguments.count;
                if (argCount > 16) return false;
                OverloadTypeMask argMasks[16];
                for (u32 i = 0; i < argCount; i++) {
                    TypeInfo argType = state->parser->GetExpressionType(func.arguments[i]);
                    argMasks[i] = MakeOverloadMask(argType);
                }
                Symbol* sym = SymbolTable::LookupFunctionOverload(&state->parser->symbolTable,
                    func.name, argMasks, argCount);
                if (!sym || sym->kind != SymbolKind::FUNCTION) return false;
                if (!state->parser->symbolTable.functions[sym->index].isEval) return false;
            }
            // All arguments must be evaluable
            for (u32 i = 0; i < func.arguments.count; i++) {
                if (!CanEvaluateNode(state, func.arguments[i])) return false;
            }
            return true;
        }

        case ASTNodeType::MEMBER_ACCESS: {
            // Check if this is a module-qualified constant access (e.g., Math::PI)
            const MemberAccessData& access = state->ast->GetMemberAccess(node);
            if (access.object.Type() == ASTNodeType::IDENTIFIER) {
                const IdentifierData& objectIdent = state->ast->GetIdentifier(access.object);
                if (objectIdent.identifierKind == SpecialIdentifier::VARIANTS &&
                    state->parser->LookupActiveVariantBinding(access.member.nameHash)) {
                    return true;
                }
            }
            if (access.isModuleQualified) {
                // Get the module name from the object (should be an identifier)
                if (access.object.Type() != ASTNodeType::IDENTIFIER) return false;
                const IdentifierData& moduleIdent = state->ast->GetIdentifier(access.object);

                // Local enum access that remained deferred until parse completed
                Symbol* enumSym = SymbolTable::LookupByHash(&state->parser->symbolTable, moduleIdent.name.nameHash);
                if (enumSym && (enumSym->kind == SymbolKind::ENUM || enumSym->kind == SymbolKind::ENUM_SYMBOL)) {
                    return true;
                }

                // Look up as a qualified name (Module::member). The parser
                // canonicalizes aliases when it creates the member access.
                u32 qualifiedHash = access.qualifiedNameHash;
                if (qualifiedHash == 0) {
                    std::string moduleName = moduleIdent.name.ToString(state->parser->sourceBase());
                    std::string memberName = access.member.ToString(state->parser->sourceBase());
                    std::string qualifiedName = moduleName + "::" + memberName;
                    qualifiedHash = Utils::HashStr(qualifiedName.c_str());
                }

                Symbol* sym = SymbolTable::LookupByHash(&state->parser->symbolTable, qualifiedHash);
                if (sym && sym->kind == SymbolKind::EVAL_CONSTANT) return true;
                if (sym && sym->kind == SymbolKind::VARIABLE) {
                    VariableData& var = state->parser->symbolTable.variables[sym->index];
                    if (var.isEval) return true;
                }
            }
            return false;
        }

        default:
            return false;
    }
}

bool CompileTimeEvaluatorSoA::EvaluateBinaryOp(EvalStateSoA* state, NodeRef node, LiteralValue* outValue) {
    const BinaryOpData& binOp = state->ast->GetBinaryOp(node);

    LiteralValue leftVal, rightVal;
    if (!EvaluateNode(state, binOp.left, &leftVal)) return false;
    if (!EvaluateNode(state, binOp.right, &rightVal)) return false;

    // Type coercion: promote int to float if needed
    if (leftVal.type == LiteralValue::INT && rightVal.type == LiteralValue::FLOAT) {
        leftVal.floatValue = static_cast<float>(leftVal.intValue);
        leftVal.type = LiteralValue::FLOAT;
    } else if (leftVal.type == LiteralValue::FLOAT && rightVal.type == LiteralValue::INT) {
        rightVal.floatValue = static_cast<float>(rightVal.intValue);
        rightVal.type = LiteralValue::FLOAT;
    }

    // Perform operation based on types
    if (leftVal.type == LiteralValue::INT && rightVal.type == LiteralValue::INT) {
        int result;
        if (!PerformIntOp(binOp.op, leftVal.intValue, rightVal.intValue, &result)) {
            SetError(state, "Integer operation failed (division by zero?)");
            return false;
        }

        // Comparison operators return bool
        if (binOp.op == BinaryOpType::EQUALS || binOp.op == BinaryOpType::NOT_EQUALS ||
            binOp.op == BinaryOpType::LESS || binOp.op == BinaryOpType::GREATER ||
            binOp.op == BinaryOpType::LESS_EQUAL || binOp.op == BinaryOpType::GREATER_EQUAL) {
            outValue->type = LiteralValue::BOOL;
            outValue->boolValue = (result != 0);
        } else {
            outValue->type = LiteralValue::INT;
            outValue->intValue = result;
        }
        return true;

    } else if (leftVal.type == LiteralValue::FLOAT && rightVal.type == LiteralValue::FLOAT) {
        // Handle comparison operators separately
        if (binOp.op == BinaryOpType::EQUALS || binOp.op == BinaryOpType::NOT_EQUALS ||
            binOp.op == BinaryOpType::LESS || binOp.op == BinaryOpType::GREATER ||
            binOp.op == BinaryOpType::LESS_EQUAL || binOp.op == BinaryOpType::GREATER_EQUAL) {
            bool result = false;
            switch (binOp.op) {
                case BinaryOpType::EQUALS: result = (leftVal.floatValue == rightVal.floatValue); break;
                case BinaryOpType::NOT_EQUALS: result = (leftVal.floatValue != rightVal.floatValue); break;
                case BinaryOpType::LESS: result = (leftVal.floatValue < rightVal.floatValue); break;
                case BinaryOpType::GREATER: result = (leftVal.floatValue > rightVal.floatValue); break;
                case BinaryOpType::LESS_EQUAL: result = (leftVal.floatValue <= rightVal.floatValue); break;
                case BinaryOpType::GREATER_EQUAL: result = (leftVal.floatValue >= rightVal.floatValue); break;
                default: break;
            }
            outValue->type = LiteralValue::BOOL;
            outValue->boolValue = result;
            return true;
        }

        float result;
        if (!PerformFloatOp(binOp.op, leftVal.floatValue, rightVal.floatValue, &result)) {
            SetError(state, "Float operation failed");
            return false;
        }
        outValue->type = LiteralValue::FLOAT;
        outValue->floatValue = result;
        return true;

    } else if (leftVal.type == LiteralValue::BOOL && rightVal.type == LiteralValue::BOOL) {
        bool result;
        if (!PerformBoolOp(binOp.op, leftVal.boolValue, rightVal.boolValue, &result)) {
            SetError(state, "Boolean operation failed");
            return false;
        }
        outValue->type = LiteralValue::BOOL;
        outValue->boolValue = result;
        return true;
    }

    SetError(state, "Type mismatch in binary operation");
    return false;
}

bool CompileTimeEvaluatorSoA::EvaluateUnaryOp(EvalStateSoA* state, NodeRef node, LiteralValue* outValue) {
    const UnaryOpData& unaryOp = state->ast->GetUnaryOp(node);

    LiteralValue operandVal;
    if (!EvaluateNode(state, unaryOp.operand, &operandVal)) return false;

    switch (unaryOp.op) {
        case UnaryOpType::NEGATE:
            if (operandVal.type == LiteralValue::INT) {
                outValue->type = LiteralValue::INT;
                outValue->intValue = -operandVal.intValue;
                return true;
            } else if (operandVal.type == LiteralValue::FLOAT) {
                outValue->type = LiteralValue::FLOAT;
                outValue->floatValue = -operandVal.floatValue;
                return true;
            }
            break;

        case UnaryOpType::NOT:
            if (operandVal.type == LiteralValue::BOOL) {
                outValue->type = LiteralValue::BOOL;
                outValue->boolValue = !operandVal.boolValue;
                return true;
            }
            // Also support !int (0 is false, non-zero is true)
            if (operandVal.type == LiteralValue::INT) {
                outValue->type = LiteralValue::BOOL;
                outValue->boolValue = (operandVal.intValue == 0);
                return true;
            }
            break;

        case UnaryOpType::BITWISE_NOT:
            if (operandVal.type == LiteralValue::INT) {
                outValue->type = LiteralValue::INT;
                outValue->intValue = ~operandVal.intValue;
                return true;
            }
            break;

        case UnaryOpType::PRE_INCREMENT:
        case UnaryOpType::POST_INCREMENT:
            if (operandVal.type == LiteralValue::INT) {
                outValue->type = LiteralValue::INT;
                outValue->intValue = operandVal.intValue + 1;
                return true;
            } else if (operandVal.type == LiteralValue::FLOAT) {
                outValue->type = LiteralValue::FLOAT;
                outValue->floatValue = operandVal.floatValue + 1.0f;
                return true;
            }
            break;

        case UnaryOpType::PRE_DECREMENT:
        case UnaryOpType::POST_DECREMENT:
            if (operandVal.type == LiteralValue::INT) {
                outValue->type = LiteralValue::INT;
                outValue->intValue = operandVal.intValue - 1;
                return true;
            } else if (operandVal.type == LiteralValue::FLOAT) {
                outValue->type = LiteralValue::FLOAT;
                outValue->floatValue = operandVal.floatValue - 1.0f;
                return true;
            }
            break;

        case UnaryOpType::ADDRESS_OF:
        case UnaryOpType::DEREFERENCE:
            break;
    }

    SetError(state, "Invalid unary operation");
    return false;
}

bool CompileTimeEvaluatorSoA::EvaluateFunctionCall(EvalStateSoA* state, NodeRef node, LiteralValue* outValue) {
    const FunctionCallData& func = state->ast->GetFunctionCall(node);

    // Evaluate arguments first
    LiteralValue args[16];
    u32 argCount = func.arguments.count;
    if (argCount > 16) {
        SetError(state, "Too many arguments for eval function");
        return false;
    }

    for (u32 i = 0; i < argCount; i++) {
        if (!EvaluateNode(state, func.arguments[i], &args[i])) {
            return false;
        }
    }

    // Handle common intrinsics
    u32 nameHash = func.name.nameHash;

    LiteralValue::Type scalarType;
    if (IsScalarConstructor(nameHash, &scalarType)) {
        if (argCount != 1) {
            SetError(state, "Scalar constructor requires exactly one argument");
            return false;
        }

        outValue->type = scalarType;
        switch (scalarType) {
            case LiteralValue::FLOAT:
                if (args[0].type == LiteralValue::FLOAT) outValue->floatValue = args[0].floatValue;
                else if (args[0].type == LiteralValue::INT) outValue->floatValue = (float)args[0].intValue;
                else if (args[0].type == LiteralValue::UINT) outValue->floatValue = (float)args[0].uintValue;
                else if (args[0].type == LiteralValue::BOOL) outValue->floatValue = args[0].boolValue ? 1.0f : 0.0f;
                else return false;
                return true;
            case LiteralValue::INT:
                if (args[0].type == LiteralValue::INT) outValue->intValue = args[0].intValue;
                else if (args[0].type == LiteralValue::UINT) outValue->intValue = (int)args[0].uintValue;
                else if (args[0].type == LiteralValue::FLOAT) outValue->intValue = SafeFloatToInt(args[0].floatValue);
                else if (args[0].type == LiteralValue::BOOL) outValue->intValue = args[0].boolValue ? 1 : 0;
                else return false;
                return true;
            case LiteralValue::UINT:
                if (args[0].type == LiteralValue::UINT) outValue->uintValue = args[0].uintValue;
                else if (args[0].type == LiteralValue::INT) outValue->uintValue = args[0].intValue < 0 ? 0u : (u32)args[0].intValue;
                else if (args[0].type == LiteralValue::FLOAT) outValue->uintValue = (u32)SafeFloatToInt(args[0].floatValue);
                else if (args[0].type == LiteralValue::BOOL) outValue->uintValue = args[0].boolValue ? 1u : 0u;
                else return false;
                return true;
            case LiteralValue::BOOL:
                if (args[0].type == LiteralValue::BOOL) outValue->boolValue = args[0].boolValue;
                else if (args[0].type == LiteralValue::INT) outValue->boolValue = args[0].intValue != 0;
                else if (args[0].type == LiteralValue::UINT) outValue->boolValue = args[0].uintValue != 0;
                else if (args[0].type == LiteralValue::FLOAT) outValue->boolValue = args[0].floatValue != 0.0f;
                else return false;
                return true;
            default:
                return false;
        }
    }

    // Handle vector constructors (float2, float3, float4, int2, int3, int4)
    LiteralValue::Type vecType;
    if (IsVectorConstructor(nameHash, &vecType)) {
        outValue->type = vecType;
        u8 expectedComponents = 0;
        bool isFloat = (vecType == LiteralValue::FLOAT2 || vecType == LiteralValue::FLOAT3 || vecType == LiteralValue::FLOAT4);

        switch (vecType) {
            case LiteralValue::FLOAT2: case LiteralValue::INT2: expectedComponents = 2; break;
            case LiteralValue::FLOAT3: case LiteralValue::INT3: expectedComponents = 3; break;
            case LiteralValue::FLOAT4: case LiteralValue::INT4: expectedComponents = 4; break;
            default: break;
        }

        // Initialize to zero
        for (u8 i = 0; i < 4; i++) {
            if (isFloat) outValue->floatVec[i] = 0.0f;
            else outValue->intVec[i] = 0;
        }

        // Handle different constructor patterns:
        // 1. float3(x, y, z) - individual components
        // 2. float3(scalar) - broadcast scalar to all components
        // 3. float4(vec3, w) - combine smaller vector with scalar(s)
        u8 componentIndex = 0;
        for (u32 i = 0; i < argCount && componentIndex < expectedComponents; i++) {
            if (args[i].IsVector()) {
                // Copy components from vector argument
                u8 srcSize = args[i].VectorSize();
                bool srcIsFloat = (args[i].type >= LiteralValue::FLOAT2 && args[i].type <= LiteralValue::FLOAT4);
                for (u8 j = 0; j < srcSize && componentIndex < expectedComponents; j++) {
                    if (isFloat) {
                        outValue->floatVec[componentIndex++] = srcIsFloat ? args[i].floatVec[j] : (float)args[i].intVec[j];
                    } else {
                        outValue->intVec[componentIndex++] = srcIsFloat ? SafeFloatToInt(args[i].floatVec[j]) : args[i].intVec[j];
                    }
                }
            } else {
                // Scalar argument
                float fval = 0.0f;
                int ival = 0;
                switch (args[i].type) {
                    case LiteralValue::FLOAT: fval = args[i].floatValue; ival = SafeFloatToInt(fval); break;
                    case LiteralValue::INT: ival = args[i].intValue; fval = (float)ival; break;
                    case LiteralValue::UINT: ival = (int)args[i].uintValue; fval = (float)args[i].uintValue; break;
                    default: break;
                }

                if (argCount == 1) {
                    // Broadcast single scalar to all components
                    for (u8 j = 0; j < expectedComponents; j++) {
                        if (isFloat) outValue->floatVec[j] = fval;
                        else outValue->intVec[j] = ival;
                    }
                    componentIndex = expectedComponents;
                } else {
                    // Single component
                    if (isFloat) outValue->floatVec[componentIndex++] = fval;
                    else outValue->intVec[componentIndex++] = ival;
                }
            }
        }
        return true;
    }

    // min(a, b)
    static const u32 minHash = Utils::HashStr("min");
    if (nameHash == minHash && argCount == 2) {
        if (args[0].type == LiteralValue::FLOAT && args[1].type == LiteralValue::FLOAT) {
            outValue->type = LiteralValue::FLOAT;
            outValue->floatValue = (args[0].floatValue < args[1].floatValue) ? args[0].floatValue : args[1].floatValue;
            return true;
        }
        if (args[0].type == LiteralValue::INT && args[1].type == LiteralValue::INT) {
            outValue->type = LiteralValue::INT;
            outValue->intValue = (args[0].intValue < args[1].intValue) ? args[0].intValue : args[1].intValue;
            return true;
        }
    }

    // max(a, b)
    static const u32 maxHash = Utils::HashStr("max");
    if (nameHash == maxHash && argCount == 2) {
        if (args[0].type == LiteralValue::FLOAT && args[1].type == LiteralValue::FLOAT) {
            outValue->type = LiteralValue::FLOAT;
            outValue->floatValue = (args[0].floatValue > args[1].floatValue) ? args[0].floatValue : args[1].floatValue;
            return true;
        }
        if (args[0].type == LiteralValue::INT && args[1].type == LiteralValue::INT) {
            outValue->type = LiteralValue::INT;
            outValue->intValue = (args[0].intValue > args[1].intValue) ? args[0].intValue : args[1].intValue;
            return true;
        }
    }

    // abs(a)
    static const u32 absHash = Utils::HashStr("abs");
    if (nameHash == absHash && argCount == 1) {
        if (args[0].type == LiteralValue::FLOAT) {
            outValue->type = LiteralValue::FLOAT;
            outValue->floatValue = std::fabs(args[0].floatValue);
            return true;
        }
        if (args[0].type == LiteralValue::INT) {
            outValue->type = LiteralValue::INT;
            outValue->intValue = (args[0].intValue < 0) ? -args[0].intValue : args[0].intValue;
            return true;
        }
    }

    // sqrt(a)
    static const u32 sqrtHash = Utils::HashStr("sqrt");
    if (nameHash == sqrtHash && argCount == 1) {
        if (args[0].type == LiteralValue::FLOAT) {
            outValue->type = LiteralValue::FLOAT;
            outValue->floatValue = std::sqrt(args[0].floatValue);
            return true;
        }
        if (args[0].type == LiteralValue::INT) {
            outValue->type = LiteralValue::FLOAT;
            outValue->floatValue = std::sqrt(static_cast<float>(args[0].intValue));
            return true;
        }
    }

    // pow(a, b)
    static const u32 powHash = Utils::HashStr("pow");
    if (nameHash == powHash && argCount == 2) {
        float base = (args[0].type == LiteralValue::FLOAT) ? args[0].floatValue : static_cast<float>(args[0].intValue);
        float exp = (args[1].type == LiteralValue::FLOAT) ? args[1].floatValue : static_cast<float>(args[1].intValue);
        outValue->type = LiteralValue::FLOAT;
        outValue->floatValue = std::pow(base, exp);
        return true;
    }

    // floor(a)
    static const u32 floorHash = Utils::HashStr("floor");
    if (nameHash == floorHash && argCount == 1) {
        if (args[0].type == LiteralValue::FLOAT) {
            outValue->type = LiteralValue::FLOAT;
            outValue->floatValue = std::floor(args[0].floatValue);
            return true;
        }
        if (args[0].type == LiteralValue::INT) {
            *outValue = args[0]; // int floor is identity
            return true;
        }
    }

    // ceil(a)
    static const u32 ceilHash = Utils::HashStr("ceil");
    if (nameHash == ceilHash && argCount == 1) {
        if (args[0].type == LiteralValue::FLOAT) {
            outValue->type = LiteralValue::FLOAT;
            outValue->floatValue = std::ceil(args[0].floatValue);
            return true;
        }
        if (args[0].type == LiteralValue::INT) {
            *outValue = args[0]; // int ceil is identity
            return true;
        }
    }

    // clamp(x, min, max)
    static const u32 clampHash = Utils::HashStr("clamp");
    if (nameHash == clampHash && argCount == 3) {
        if (args[0].type == LiteralValue::FLOAT) {
            float minVal = (args[1].type == LiteralValue::FLOAT) ? args[1].floatValue : static_cast<float>(args[1].intValue);
            float maxVal = (args[2].type == LiteralValue::FLOAT) ? args[2].floatValue : static_cast<float>(args[2].intValue);
            outValue->type = LiteralValue::FLOAT;
            float val = args[0].floatValue;
            outValue->floatValue = (val < minVal) ? minVal : ((val > maxVal) ? maxVal : val);
            return true;
        }
        if (args[0].type == LiteralValue::INT && args[1].type == LiteralValue::INT && args[2].type == LiteralValue::INT) {
            outValue->type = LiteralValue::INT;
            int val = args[0].intValue;
            int minVal = args[1].intValue;
            int maxVal = args[2].intValue;
            outValue->intValue = (val < minVal) ? minVal : ((val > maxVal) ? maxVal : val);
            return true;
        }
    }

    SetError(state, "Cannot evaluate function at compile time");
    return false;
}

bool CompileTimeEvaluatorSoA::EvaluateNode(EvalStateSoA* state, NodeRef node, LiteralValue* outValue) {
    if (node.IsNull()) {
        SetError(state, "Null node in evaluation");
        return false;
    }

    if (state->hasError) return false;

    // Check iteration limit
    if (++state->iterationCount > state->iterationLimit) {
        SetError(state, "Evaluation iteration limit exceeded");
        return false;
    }

    switch (node.Type()) {
        case ASTNodeType::LITERAL: {
            const LiteralData& lit = state->ast->GetLiteral(node);
            *outValue = lit.value;
            return true;
        }

        case ASTNodeType::IDENTIFIER: {
            const IdentifierData& ident = state->ast->GetIdentifier(node);
            if (state->lookupComptimeBinding &&
                state->lookupComptimeBinding(state->comptimeUser, ident.name.nameHash, outValue)) {
                return true;
            }
            if (state->parser->allowBareVariantLookup &&
                state->parser->LookupActiveVariantBinding(ident.name.nameHash, outValue)) {
                return true;
            }
            Symbol* sym = SymbolTable::LookupAny(&state->parser->symbolTable, ident.name);
            if (!sym) {
                SetError(state, "Unknown identifier in eval");
                return false;
            }

            if (sym->kind == SymbolKind::EVAL_CONSTANT) {
                *outValue = state->parser->symbolTable.evalConstants[sym->index];
                return true;
            }

            if (sym->kind == SymbolKind::VARIABLE) {
                VariableData& var = state->parser->symbolTable.variables[sym->index];
                if (var.isEval) {
                    *outValue = var.evalValue;
                    return true;
                }
            }

            SetError(state, "Identifier is not a compile-time constant");
            return false;
        }

        case ASTNodeType::BINARY_OP:
            return EvaluateBinaryOp(state, node, outValue);

        case ASTNodeType::UNARY_OP:
            return EvaluateUnaryOp(state, node, outValue);

        case ASTNodeType::FUNCTION_CALL:
            return EvaluateFunctionCall(state, node, outValue);

        case ASTNodeType::MEMBER_ACCESS: {
            const MemberAccessData& access = state->ast->GetMemberAccess(node);
            if (access.object.Type() == ASTNodeType::IDENTIFIER) {
                const IdentifierData& objectIdent = state->ast->GetIdentifier(access.object);
                if (objectIdent.identifierKind == SpecialIdentifier::VARIANTS &&
                    state->parser->LookupActiveVariantBinding(access.member.nameHash, outValue)) {
                    return true;
                }
            }
            if (access.isModuleQualified) {
                // Get the module name from the object (should be an identifier)
                if (access.object.Type() != ASTNodeType::IDENTIFIER) {
                    SetError(state, "Expected module identifier before ::");
                    return false;
                }
                const IdentifierData& moduleIdent = state->ast->GetIdentifier(access.object);

                Symbol* enumSym = SymbolTable::LookupByHash(&state->parser->symbolTable, moduleIdent.name.nameHash);
                if (enumSym && (enumSym->kind == SymbolKind::ENUM || enumSym->kind == SymbolKind::ENUM_SYMBOL)) {
                    const EnumData& enumData = state->parser->symbolTable.enums[enumSym->index];
                    if (!(enumData.flags & EnumData::IS_SUM_TYPE)) {
                        for (u32 i = 0; i < enumData.variants.count; i++) {
                            if (enumData.variants[i].name.nameHash == access.member.nameHash) {
                                CoreType baseType = enumData.underlyingType;
                                if (baseType == CoreType::UINT) {
                                    outValue->type = LiteralValue::UINT;
                                    outValue->uintValue = enumData.variants[i].value;
                                } else {
                                    outValue->type = LiteralValue::INT;
                                    outValue->intValue = static_cast<int>(enumData.variants[i].value);
                                }
                                return true;
                            }
                        }
                    }
                }

                // Look up as a qualified name (Module::member)
                // Use the pre-computed hash since member may be hash-only ArenaString
                u32 qualifiedHash = access.qualifiedNameHash;

                #ifdef DEBUG_EVAL
                fprintf(stderr, "DEBUG: Looking up module-qualified name (hash=%u)\n", qualifiedHash);
                // Also print all EVAL_CONSTANT symbols
                for (u32 i = 0; i < state->parser->symbolTable.symbols.count; i++) {
                    const Symbol& s = state->parser->symbolTable.symbols[i];
                    if (s.kind == SymbolKind::EVAL_CONSTANT) {
                        fprintf(stderr, "  EVAL_CONSTANT: hash=%u\n", s.name.nameHash);
                    }
                }
                #endif

                Symbol* sym = SymbolTable::LookupByHash(&state->parser->symbolTable, qualifiedHash);
                if (!sym) {
                    SetError(state, "Unknown module-qualified identifier in eval");
                    return false;
                }

                if (sym->kind == SymbolKind::EVAL_CONSTANT) {
                    *outValue = state->parser->symbolTable.evalConstants[sym->index];
                    return true;
                }

                if (sym->kind == SymbolKind::VARIABLE) {
                    VariableData& var = state->parser->symbolTable.variables[sym->index];
                    if (var.isEval) {
                        *outValue = var.evalValue;
                        return true;
                    }
                }

                SetError(state, "Module member is not a compile-time constant");
                return false;
            }
            SetError(state, "Non-module member access cannot be evaluated at compile time");
            return false;
        }

        default:
            SetError(state, "Node type cannot be evaluated at compile time");
            return false;
    }
}

} // namespace BWSL
