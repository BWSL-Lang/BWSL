#include "bwsl_eval_soa.h"
#include "bwsl_parser_soa.h"
#include <cstring>
#include <cmath>

namespace BWSL {

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
    switch (op) {
        case BinaryOpType::ADD: *result = left + right; return true;
        case BinaryOpType::SUBTRACT: *result = left - right; return true;
        case BinaryOpType::MULTIPLY: *result = left * right; return true;
        case BinaryOpType::DIVIDE:
            if (right == 0) return false;
            *result = left / right;
            return true;
        case BinaryOpType::MODULO:
            if (right == 0) return false;
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
        case BinaryOpType::LEFT_SHIFT: *result = left << right; return true;
        case BinaryOpType::RIGHT_SHIFT: *result = left >> right; return true;
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
            if (access.isModuleQualified) {
                // Get the module name from the object (should be an identifier)
                if (access.object.Type() != ASTNodeType::IDENTIFIER) return false;
                const IdentifierData& moduleIdent = state->ast->GetIdentifier(access.object);

                // Look up as a qualified name (Module::member)
                std::string moduleName = moduleIdent.name.ToString(state->parser->sourceBase());
                std::string memberName = access.member.ToString(state->parser->sourceBase());
                std::string qualifiedName = moduleName + "::" + memberName;
                u32 qualifiedHash = Utils::HashStr(qualifiedName.c_str());

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
            if (access.isModuleQualified) {
                // Get the module name from the object (should be an identifier)
                if (access.object.Type() != ASTNodeType::IDENTIFIER) {
                    SetError(state, "Expected module identifier before ::");
                    return false;
                }
                const IdentifierData& moduleIdent = state->ast->GetIdentifier(access.object);

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
