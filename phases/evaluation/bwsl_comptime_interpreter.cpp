#include "bwsl_comptime_interpreter.h"
#include "bwsl_parser_soa.h"
#include "bwsl_eval_soa.h"
#include <climits>
#include <cstring>

namespace BWSL::Comptime {

namespace {

constexpr u32 INVALID_U32 = 0xFFFFFFFFu;
constexpr u8 BINDING_HAS_VALUE = 0x01;
constexpr u8 BINDING_SHADOW = 0x02;

struct ComptimeState {
    CompilationContext* context;
    Parser* parser;
    AST* ast;
    SymbolTableData* symbols;
    BWSL_Arena* arena;
    const char* sourceBase;

    ArenaArray<ComptimeValue> values;
    ArenaArray<ComptimeBinding> bindings;
    ArenaArray<ComptimeScope> scopes;
    ArenaArray<ComptimeFrame> frames;
    ArenaArray<NodeRef> emittedStatements;
    ArenaArray<ComptimeEmitList> emitLists;
    ArenaArray<ComptimeDiagnostic> diagnostics;

    ComptimeBudget budget;
    bool hadError;
};

static void InitBudget(ComptimeBudget* budget) {
    budget->executedStatements = 0;
    budget->emittedStatements = 0;
    budget->loopIterations = 0;
    budget->cloneNodes = 0;
    budget->cloneDepth = 0;
    budget->maxExecutedStatements = 100000;
    budget->maxEmittedStatements = 100000;
    budget->maxLoopIterations = 10000;
    budget->maxCloneNodes = 100000;
    budget->maxCloneDepth = 2048;
}

static void Report(ComptimeState* state, NodeRef node, const char* message, u32 code = 1) {
    if (state->hadError) return;
    state->hadError = true;

    ComptimeDiagnostic diag{};
    diag.node = node;
    diag.line = state->ast->GetLine(node);
    diag.column = state->ast->GetColumn(node);
    diag.code = code;
    diag.message = message;
    state->diagnostics.Push(state->arena, diag);

    state->parser->hadError = true;
    ParseError err{};
    err.message = message;
    err.line = diag.line;
    err.column = diag.column;
    err.token = INVALID_TOKEN;
    state->parser->errors.Push(state->parser->arena, err);
}

static u32 PushScope(ComptimeState* state) {
    ComptimeScope scope{};
    scope.firstBinding = state->bindings.count;
    scope.bindingCount = 0;
    scope.parentScope = state->scopes.count ? state->scopes.count - 1 : INVALID_U32;
    state->scopes.Push(state->arena, scope);
    return state->scopes.count - 1;
}

static void PopScope(ComptimeState* state) {
    if (state->scopes.count == 0) return;
    ComptimeScope& scope = state->scopes[state->scopes.count - 1];
    state->bindings.count = scope.firstBinding;
    state->scopes.count--;
}

static bool LookupBindingIndex(const ComptimeState* state, u32 nameHash, u32* outIndex) {
    for (u32 i = state->bindings.count; i > 0; --i) {
        const ComptimeBinding& binding = state->bindings[i - 1];
        if (binding.nameHash != nameHash) continue;
        if (outIndex) *outIndex = i - 1;
        return true;
    }
    return false;
}

static bool LookupBindingValue(const ComptimeState* state, u32 nameHash, LiteralValue* outValue) {
    u32 index = INVALID_U32;
    if (!LookupBindingIndex(state, nameHash, &index)) return false;
    const ComptimeBinding& binding = state->bindings[index];
    if (binding.flags & BINDING_SHADOW) return false;
    if (!(binding.flags & BINDING_HAS_VALUE)) return false;
    if (binding.valueSlot >= state->values.count) return false;
    if (outValue) *outValue = state->values[binding.valueSlot].value;
    return true;
}

static bool EvalLookupHook(void* user, u32 nameHash, LiteralValue* outValue) {
    return LookupBindingValue(static_cast<const ComptimeState*>(user), nameHash, outValue);
}

static u32 AddValue(ComptimeState* state, const LiteralValue& value) {
    ComptimeValue slot{};
    slot.value = value;
    state->values.Push(state->arena, slot);
    return state->values.count - 1;
}

static void AddBinding(ComptimeState* state, u32 nameHash, const TypeInfo& typeInfo, const LiteralValue& value) {
    ComptimeBinding binding{};
    binding.nameHash = nameHash;
    binding.typeInfo = typeInfo;
    binding.valueSlot = AddValue(state, value);
    binding.scopeDepth = state->scopes.count;
    binding.flags = BINDING_HAS_VALUE;
    state->bindings.Push(state->arena, binding);
    if (state->scopes.count) {
        state->scopes[state->scopes.count - 1].bindingCount++;
    }
}

static void AddShadow(ComptimeState* state, u32 nameHash) {
    u32 ignored = INVALID_U32;
    if (!LookupBindingIndex(state, nameHash, &ignored)) return;
    ComptimeBinding binding{};
    binding.nameHash = nameHash;
    binding.typeInfo = TYPE_INFO(CoreType::INVALID, 0, false);
    binding.valueSlot = INVALID_U32;
    binding.scopeDepth = state->scopes.count;
    binding.flags = BINDING_SHADOW;
    state->bindings.Push(state->arena, binding);
    if (state->scopes.count) {
        state->scopes[state->scopes.count - 1].bindingCount++;
    }
}

static bool UpdateBinding(ComptimeState* state, u32 nameHash, const LiteralValue& value) {
    for (u32 i = state->bindings.count; i > 0; --i) {
        ComptimeBinding& binding = state->bindings[i - 1];
        if (binding.nameHash != nameHash) continue;
        if (binding.flags & BINDING_SHADOW) return false;
        binding.valueSlot = AddValue(state, value);
        binding.flags |= BINDING_HAS_VALUE;
        return true;
    }
    return false;
}

static TypeInfo TypeInfoFromTypeName(ArenaString typeName) {
    u32 hash = typeName.nameHash;
    for (const auto& entry : TypeHashes::HASH_TABLE) {
        if (entry.hash == hash) return entry.info;
    }
    return TYPE_INFO(CoreType::INVALID, 0, false);
}

static bool CoerceLiteralToType(const TypeInfo& typeInfo, LiteralValue* value) {
    switch (typeInfo.coreType) {
        case CoreType::FLOAT:
            if (value->type == LiteralValue::INT) {
                value->floatValue = static_cast<float>(value->intValue);
                value->type = LiteralValue::FLOAT;
            } else if (value->type == LiteralValue::UINT) {
                value->floatValue = static_cast<float>(value->uintValue);
                value->type = LiteralValue::FLOAT;
            }
            return value->type == LiteralValue::FLOAT;
        case CoreType::INT:
            if (value->type == LiteralValue::UINT) {
                if (value->uintValue > static_cast<unsigned int>(INT_MAX)) return false;
                value->intValue = static_cast<int>(value->uintValue);
                value->type = LiteralValue::INT;
            }
            return value->type == LiteralValue::INT;
        case CoreType::UINT:
            if (value->type == LiteralValue::INT) {
                if (value->intValue < 0) return false;
                value->uintValue = static_cast<unsigned int>(value->intValue);
                value->type = LiteralValue::UINT;
            }
            return value->type == LiteralValue::UINT;
        case CoreType::BOOL:
            return value->type == LiteralValue::BOOL;
        case CoreType::FLOAT2: return value->type == LiteralValue::FLOAT2;
        case CoreType::FLOAT3: return value->type == LiteralValue::FLOAT3;
        case CoreType::FLOAT4: return value->type == LiteralValue::FLOAT4;
        case CoreType::INT2: return value->type == LiteralValue::INT2;
        case CoreType::INT3: return value->type == LiteralValue::INT3;
        case CoreType::INT4: return value->type == LiteralValue::INT4;
        default:
            return typeInfo.coreType == CoreType::INVALID;
    }
}

static bool LiteralToBool(const LiteralValue& value, bool* outBool) {
    switch (value.type) {
        case LiteralValue::BOOL: *outBool = value.boolValue; return true;
        case LiteralValue::INT: *outBool = value.intValue != 0; return true;
        case LiteralValue::UINT: *outBool = value.uintValue != 0; return true;
        case LiteralValue::FLOAT: *outBool = value.floatValue != 0.0f; return true;
        default: return false;
    }
}

static bool LiteralToInt(const LiteralValue& value, s32* outInt) {
    switch (value.type) {
        case LiteralValue::INT:
            *outInt = static_cast<s32>(value.intValue);
            return true;
        case LiteralValue::UINT:
            if (value.uintValue > static_cast<unsigned int>(INT_MAX)) return false;
            *outInt = static_cast<s32>(value.uintValue);
            return true;
        default:
            return false;
    }
}

static NodeRef MakeLiteralNode(AST* ast, const LiteralValue& value, u32 line, u32 col) {
    switch (value.type) {
        case LiteralValue::FLOAT:
            return ASTFactory::MakeLiteralFloat(ast, value.floatValue, line, col);
        case LiteralValue::INT:
            return ASTFactory::MakeLiteralInt(ast, value.intValue, line, col);
        case LiteralValue::UINT:
            return ASTFactory::MakeLiteralUint(ast, value.uintValue, line, col);
        case LiteralValue::BOOL:
            return ASTFactory::MakeLiteralBool(ast, value.boolValue, line, col);
        default:
            return NodeRef::Null();
    }
}

static bool EvalExpression(ComptimeState* state, NodeRef expr, LiteralValue* outValue) {
    EvalStateSoA evalState;
    CompileTimeEvaluatorSoA::Init(&evalState, state->parser, state->ast,
                                  &state->context->evalCache, state->arena);
    evalState.comptimeUser = state;
    evalState.lookupComptimeBinding = EvalLookupHook;
    return CompileTimeEvaluatorSoA::CanEvaluateNode(&evalState, expr) &&
           CompileTimeEvaluatorSoA::EvaluateNode(&evalState, expr, outValue);
}

static bool CheckExecutedBudget(ComptimeState* state, NodeRef node) {
    if (++state->budget.executedStatements > state->budget.maxExecutedStatements) {
        Report(state, node, "Comptime execution exceeded statement budget");
        return false;
    }
    return true;
}

static bool CheckEmitBudget(ComptimeState* state, NodeRef node) {
    if (++state->budget.emittedStatements > state->budget.maxEmittedStatements) {
        Report(state, node, "Comptime expansion exceeded emitted statement budget");
        return false;
    }
    return true;
}

static void AppendStatement(ComptimeState* state, BlockData& outBlock, NodeRef stmt) {
    if (stmt.IsNull()) return;
    outBlock.statements.Push(state->arena, stmt);
    state->emittedStatements.Push(state->arena, stmt);
}

static NodeRef CloneNode(ComptimeState* state, NodeRef node);

static NodeRef CloneBlockLike(ComptimeState* state, NodeRef node, bool evalBlock) {
    const BlockData& src = state->ast->GetBlock(node);
    std::vector<NodeRef> statements;
    statements.reserve(src.statements.count);
    for (u32 i = 0; i < src.statements.count; i++) {
        statements.push_back(src.statements[i]);
    }
    NodeRef cloned = evalBlock
        ? ASTFactory::MakeEvalBlock(state->ast, state->ast->GetLine(node), state->ast->GetColumn(node))
        : ASTFactory::MakeBlock(state->ast, state->ast->GetLine(node), state->ast->GetColumn(node));
    for (NodeRef stmt : statements) {
        NodeRef child = CloneNode(state, stmt);
        if (child.IsValid()) {
            state->ast->GetBlock(cloned).statements.Push(state->arena, child);
        }
    }
    return cloned;
}

static NodeRef CloneNode(ComptimeState* state, NodeRef node) {
    if (node.IsNull()) return NodeRef::Null();
    if (state->budget.cloneDepth >= state->budget.maxCloneDepth) {
        Report(state, node, "Comptime clone depth exceeded");
        return NodeRef::Null();
    }
    if (++state->budget.cloneNodes > state->budget.maxCloneNodes) {
        Report(state, node, "Comptime clone node budget exceeded");
        return NodeRef::Null();
    }

    struct DepthGuard {
        ComptimeBudget* budget;
        explicit DepthGuard(ComptimeBudget* b) : budget(b) { budget->cloneDepth++; }
        ~DepthGuard() { budget->cloneDepth--; }
    } guard(&state->budget);

    u32 line = state->ast->GetLine(node);
    u32 col = state->ast->GetColumn(node);

    switch (node.Type()) {
        case ASTNodeType::IDENTIFIER: {
            const IdentifierData& src = state->ast->GetIdentifier(node);
            LiteralValue value;
            if (LookupBindingValue(state, src.name.nameHash, &value)) {
                NodeRef lit = MakeLiteralNode(state->ast, value, line, col);
                if (lit.IsValid()) return lit;
            }
            NodeRef cloned = ASTFactory::MakeIdentifier(state->ast, src.name, line, col);
            state->ast->GetIdentifier(cloned).identifierKind = src.identifierKind;
            return cloned;
        }
        case ASTNodeType::LITERAL: {
            const LiteralData& src = state->ast->GetLiteral(node);
            NodeRef lit = MakeLiteralNode(state->ast, src.value, line, col);
            return lit.IsValid() ? lit : node;
        }
        case ASTNodeType::BINARY_OP: {
            const BinaryOpData& src = state->ast->GetBinaryOp(node);
            return ASTFactory::MakeBinaryOp(state->ast, src.op,
                                            CloneNode(state, src.left),
                                            CloneNode(state, src.right),
                                            line, col);
        }
        case ASTNodeType::UNARY_OP: {
            const UnaryOpData& src = state->ast->GetUnaryOp(node);
            return ASTFactory::MakeUnaryOp(state->ast, src.op,
                                           CloneNode(state, src.operand), line, col);
        }
        case ASTNodeType::TERNARY_EXPRESSION: {
            const TernaryExprData& src = state->ast->GetTernaryExpression(node);
            return ASTFactory::MakeTernaryExpr(state->ast,
                                               CloneNode(state, src.condition),
                                               CloneNode(state, src.trueExpr),
                                               CloneNode(state, src.falseExpr),
                                               line, col);
        }
        case ASTNodeType::MEMBER_ACCESS: {
            const MemberAccessData& src = state->ast->GetMemberAccess(node);
            NodeRef object = CloneNode(state, src.object);
            NodeRef cloned = ASTFactory::MakeMemberAccess(state->ast, object, src.member, line, col);
            MemberAccessData& dst = state->ast->GetMemberAccess(cloned);
            dst.isModuleQualified = src.isModuleQualified;
            dst.qualifiedNameHash = src.qualifiedNameHash;
            return cloned;
        }
        case ASTNodeType::ARRAY_ACCESS: {
            const ArrayAccessData& src = state->ast->GetArrayAccess(node);
            return ASTFactory::MakeArrayAccess(state->ast, CloneNode(state, src.array),
                                               CloneNode(state, src.index), line, col);
        }
        case ASTNodeType::ASSIGNMENT: {
            const AssignmentData& src = state->ast->GetAssignment(node);
            return ASTFactory::MakeAssignment(state->ast, CloneNode(state, src.target),
                                              CloneNode(state, src.value), line, col);
        }
        case ASTNodeType::BLOCK:
            return CloneBlockLike(state, node, false);
        case ASTNodeType::EVAL_BLOCK:
            return CloneBlockLike(state, node, true);
        case ASTNodeType::VARIABLE_DECL: {
            const VariableDeclData& src = state->ast->GetVariableDecl(node);
            NodeRef cloned = ASTFactory::MakeVariableDecl(state->ast, src.name, src.type,
                                                          CloneNode(state, src.initializer),
                                                          src.isConst, line, col, src.storageClass,
                                                          src.arrayDimensions, src.arrayLength,
                                                          src.arrayElementTypeHash);
            state->ast->GetVariableDecl(cloned).isEval = src.isEval;
            return cloned;
        }
        case ASTNodeType::FUNCTION_CALL: {
            const FunctionCallData& src = state->ast->GetFunctionCall(node);
            if (src.name.nameHash == TypeHashes::FLOAT && src.arguments.count == 1) {
                LiteralValue argValue;
                if (EvalExpression(state, src.arguments[0], &argValue)) {
                    LiteralValue castValue{};
                    castValue.type = LiteralValue::FLOAT;
                    if (argValue.type == LiteralValue::FLOAT) {
                        castValue.floatValue = argValue.floatValue;
                        return ASTFactory::MakeLiteralFloat(state->ast, castValue.floatValue, line, col);
                    }
                    if (argValue.type == LiteralValue::INT) {
                        castValue.floatValue = static_cast<float>(argValue.intValue);
                        return ASTFactory::MakeLiteralFloat(state->ast, castValue.floatValue, line, col);
                    }
                    if (argValue.type == LiteralValue::UINT) {
                        castValue.floatValue = static_cast<float>(argValue.uintValue);
                        return ASTFactory::MakeLiteralFloat(state->ast, castValue.floatValue, line, col);
                    }
                }
            }
            ArenaString name = src.name;
            u16 intrinsicIndex = src.intrinsicIndex;
            u8 flags = src.flags;
            u32 moduleIndex = src.moduleIndex;
            u32 moduleQualifiedHash = src.moduleQualifiedHash;
            NodeRef moduleObject = src.moduleObject;
            std::vector<NodeRef> args;
            args.reserve(src.arguments.count);
            for (u32 i = 0; i < src.arguments.count; i++) {
                args.push_back(src.arguments[i]);
            }
            NodeRef clonedModuleObject = moduleObject.IsValid()
                ? CloneNode(state, moduleObject)
                : NodeRef::Null();

            NodeRef cloned = ASTFactory::MakeFunctionCall(state->ast, name, line, col);
            FunctionCallData& dst = state->ast->GetFunctionCall(cloned);
            dst.intrinsicIndex = intrinsicIndex;
            dst.flags = flags;
            dst.moduleIndex = moduleIndex;
            dst.moduleQualifiedHash = moduleQualifiedHash;
            dst.moduleObject = clonedModuleObject;
            for (NodeRef arg : args) {
                NodeRef clonedArg = CloneNode(state, arg);
                state->ast->GetFunctionCall(cloned).arguments.Push(state->arena, clonedArg);
            }
            return cloned;
        }
        case ASTNodeType::IF_STATEMENT:
        case ASTNodeType::EVAL_IF: {
            const BlockData& src = state->ast->GetBlock(node);
            std::vector<NodeRef> statements;
            statements.reserve(src.statements.count);
            for (u32 i = 0; i < src.statements.count; i++) {
                statements.push_back(src.statements[i]);
            }
            NodeRef cloned = node.Type() == ASTNodeType::EVAL_IF
                ? ASTFactory::MakeEvalIfStatement(state->ast, line, col)
                : ASTFactory::MakeIfStatement(state->ast, line, col);
            for (NodeRef stmt : statements) {
                NodeRef child = CloneNode(state, stmt);
                if (child.IsValid()) {
                    state->ast->GetBlock(cloned).statements.Push(state->arena, child);
                }
            }
            return cloned;
        }
        case ASTNodeType::FOR_CSTYLE: {
            const ForCStyleData& src = state->ast->GetForCStyle(node);
            return ASTFactory::MakeForCStyle(state->ast, CloneNode(state, src.init),
                                             CloneNode(state, src.condition),
                                             CloneNode(state, src.increment),
                                             CloneNode(state, src.body),
                                             src.isEval, line, col, src.isWhile);
        }
        case ASTNodeType::FOR_RANGE: {
            const ForRangeData& src = state->ast->GetForRange(node);
            return ASTFactory::MakeForRange(state->ast, CloneNode(state, src.iteratorVar),
                                            CloneNode(state, src.rangeStart),
                                            CloneNode(state, src.rangeEnd),
                                            CloneNode(state, src.step),
                                            CloneNode(state, src.body),
                                            src.inclusive, src.isEval, line, col);
        }
        case ASTNodeType::FOR_COLLECTION: {
            const ForCollectionData& src = state->ast->GetForCollection(node);
            return ASTFactory::MakeForCollection(state->ast, CloneNode(state, src.iteratorVar),
                                                 CloneNode(state, src.collection),
                                                 CloneNode(state, src.body),
                                                 src.isEval, src.length, line, col);
        }
        case ASTNodeType::LOOP: {
            const LoopData& src = state->ast->GetLoop(node);
            return ASTFactory::MakeLoop(state->ast, CloneNode(state, src.count),
                                        CloneNode(state, src.body),
                                        CloneNode(state, src.untilCondition),
                                        src.isEval, line, col);
        }
        case ASTNodeType::RETURN: {
            const AssignmentData& src = state->ast->GetAssignment(node);
            return ASTFactory::MakeReturn(state->ast, CloneNode(state, src.value), line, col);
        }
        default:
            return node;
    }
}

static bool ProcessBlock(ComptimeState* state, NodeRef blockRef);
static bool ExecuteStatement(ComptimeState* state, NodeRef stmt, BlockData& outBlock, bool evalContext);

static bool ProcessRuntimeChildren(ComptimeState* state, NodeRef stmt) {
    if (stmt.IsNull()) return true;
    switch (stmt.Type()) {
        case ASTNodeType::BLOCK:
            return ProcessBlock(state, stmt);
        case ASTNodeType::IF_STATEMENT: {
            BlockData& block = state->ast->GetBlock(stmt);
            for (u32 i = 1; i < block.statements.count; i++) {
                if (block.statements[i].Type() == ASTNodeType::BLOCK) {
                    if (!ProcessBlock(state, block.statements[i])) return false;
                } else if (!ProcessRuntimeChildren(state, block.statements[i])) {
                    return false;
                }
            }
            return true;
        }
        case ASTNodeType::FOR_CSTYLE: {
            ForCStyleData& loop = state->ast->GetForCStyle(stmt);
            return loop.body.IsNull() || ProcessBlock(state, loop.body);
        }
        case ASTNodeType::FOR_RANGE: {
            ForRangeData& loop = state->ast->GetForRange(stmt);
            return loop.body.IsNull() || ProcessBlock(state, loop.body);
        }
        case ASTNodeType::FOR_COLLECTION: {
            ForCollectionData& loop = state->ast->GetForCollection(stmt);
            return loop.body.IsNull() || ProcessBlock(state, loop.body);
        }
        case ASTNodeType::LOOP: {
            LoopData& loop = state->ast->GetLoop(stmt);
            return loop.body.IsNull() || ProcessBlock(state, loop.body);
        }
        default:
            return true;
    }
}

static bool BindCompileTimeDecl(ComptimeState* state, NodeRef stmt, bool requireEvalContext) {
    const VariableDeclData& decl = state->ast->GetVariableDecl(stmt);
    if (!decl.isEval && !(requireEvalContext && decl.isConst)) return false;
    if (decl.initializer.IsNull()) {
        Report(state, stmt, "Compile-time declarations must be initialized");
        return true;
    }

    LiteralValue value;
    if (!EvalExpression(state, decl.initializer, &value)) {
        Report(state, decl.initializer, "Compile-time declaration initializer is not a compile-time value");
        return true;
    }

    TypeInfo typeInfo = TypeInfoFromTypeName(decl.type);
    if (!CoerceLiteralToType(typeInfo, &value)) {
        Report(state, stmt, "Type mismatch in compile-time declaration");
        return true;
    }

    AddBinding(state, decl.name.nameHash, typeInfo, value);

    if (Symbol* sym = SymbolTable::LookupByHash(state->symbols, decl.name.nameHash)) {
        if (sym->kind == SymbolKind::VARIABLE) {
            VariableData& var = state->symbols->variables[sym->index];
            var.typeInfo = typeInfo;
            var.isConst = true;
            var.isEval = true;
            var.hasEvalValue = true;
            var.evalValue = value;
        }
    }

    return true;
}

static bool ExecuteAssignmentIfComptime(ComptimeState* state, NodeRef stmt) {
    const AssignmentData& assign = state->ast->GetAssignment(stmt);
    if (assign.target.Type() != ASTNodeType::IDENTIFIER) return false;
    const IdentifierData& ident = state->ast->GetIdentifier(assign.target);
    u32 index = INVALID_U32;
    if (!LookupBindingIndex(state, ident.name.nameHash, &index)) return false;
    if (state->bindings[index].flags & BINDING_SHADOW) return false;

    LiteralValue value;
    if (!EvalExpression(state, assign.value, &value)) {
        Report(state, assign.value, "Compile-time assignment value is not a compile-time value");
        return true;
    }
    UpdateBinding(state, ident.name.nameHash, value);
    return true;
}

static bool ExecuteBlockAsEval(ComptimeState* state, NodeRef blockRef, BlockData& outBlock) {
    const BlockData& block = state->ast->GetBlock(blockRef);
    PushScope(state);
    for (u32 i = 0; i < block.statements.count && !state->hadError; i++) {
        ExecuteStatement(state, block.statements[i], outBlock, true);
    }
    PopScope(state);
    return !state->hadError;
}

static bool ExecuteEvalIf(ComptimeState* state, NodeRef stmt, BlockData& outBlock, bool evalContext) {
    const BlockData& ifData = state->ast->GetBlock(stmt);
    if (ifData.statements.count == 0) return true;

    LiteralValue condValue;
    if (!EvalExpression(state, ifData.statements[0], &condValue)) {
        Report(state, ifData.statements[0], "Eval if condition must be a compile-time value");
        return false;
    }
    bool conditionTrue = false;
    if (!LiteralToBool(condValue, &conditionTrue)) {
        Report(state, ifData.statements[0], "Eval if condition must resolve to bool, int, uint, or float");
        return false;
    }

    NodeRef selected = NodeRef::Null();
    if (conditionTrue && ifData.statements.count >= 2) selected = ifData.statements[1];
    if (!conditionTrue && ifData.statements.count >= 3) selected = ifData.statements[2];
    if (selected.IsNull()) return true;

    if (evalContext) {
        return ExecuteStatement(state, selected, outBlock, true);
    }

    if (!ProcessRuntimeChildren(state, selected)) return false;
    if (!CheckEmitBudget(state, selected)) return false;
    AppendStatement(state, outBlock, CloneNode(state, selected));
    return !state->hadError;
}

static bool ExecuteEvalForRange(ComptimeState* state, NodeRef stmt, BlockData& outBlock) {
    const ForRangeData& loop = state->ast->GetForRange(stmt);
    if (loop.iteratorVar.Type() != ASTNodeType::IDENTIFIER) {
        Report(state, stmt, "Eval for iterator must be an identifier");
        return false;
    }

    LiteralValue startValue, endValue, stepValue;
    if (!EvalExpression(state, loop.rangeStart, &startValue) ||
        !EvalExpression(state, loop.rangeEnd, &endValue)) {
        Report(state, stmt, "Eval for range bounds must be compile-time values");
        return false;
    }
    s32 start = 0, end = 0, step = 1;
    if (!LiteralToInt(startValue, &start) || !LiteralToInt(endValue, &end)) {
        Report(state, stmt, "Eval for range bounds must resolve to integers");
        return false;
    }
    if (!loop.step.IsNull()) {
        if (!EvalExpression(state, loop.step, &stepValue) || !LiteralToInt(stepValue, &step)) {
            Report(state, stmt, "Eval for step must resolve to an integer");
            return false;
        }
    }
    if (step == 0) {
        Report(state, stmt, "Eval for step must not be zero");
        return false;
    }

    const IdentifierData& iterator = state->ast->GetIdentifier(loop.iteratorVar);
    auto shouldContinue = [&](s32 value) {
        if (step > 0) return loop.inclusive ? value <= end : value < end;
        return loop.inclusive ? value >= end : value > end;
    };

    for (s32 i = start; shouldContinue(i); i += step) {
        if (++state->budget.loopIterations > state->budget.maxLoopIterations) {
            Report(state, stmt, "Eval for exceeded iteration limit");
            return false;
        }
        LiteralValue value{};
        value.type = LiteralValue::INT;
        value.intValue = static_cast<int>(i);
        PushScope(state);
        AddBinding(state, iterator.name.nameHash, TYPE_INFO(CoreType::INT, 1, false), value);
        ExecuteStatement(state, loop.body, outBlock, true);
        PopScope(state);
        if (state->hadError) return false;
    }
    return true;
}

static bool ExecuteEvalLoop(ComptimeState* state, NodeRef stmt, BlockData& outBlock) {
    const LoopData& loop = state->ast->GetLoop(stmt);

    auto checkUntil = [&](bool* outDone) -> bool {
        *outDone = false;
        if (loop.untilCondition.IsNull()) return true;
        LiteralValue untilValue;
        if (!EvalExpression(state, loop.untilCondition, &untilValue)) {
            Report(state, loop.untilCondition, "Eval loop until condition must be a compile-time value");
            return false;
        }
        if (!LiteralToBool(untilValue, outDone)) {
            Report(state, loop.untilCondition, "Eval loop until condition must resolve to bool, int, uint, or float");
            return false;
        }
        return true;
    };

    if (!loop.count.IsNull()) {
        LiteralValue countValue;
        s32 count = 0;
        if (!EvalExpression(state, loop.count, &countValue) || !LiteralToInt(countValue, &count)) {
            Report(state, loop.count, "Eval loop count must resolve to an integer");
            return false;
        }
        if (count < 0) {
            Report(state, loop.count, "Eval loop count must not be negative");
            return false;
        }
        for (s32 i = 0; i < count; i++) {
            if (++state->budget.loopIterations > state->budget.maxLoopIterations) {
                Report(state, stmt, "Eval loop exceeded iteration limit");
                return false;
            }
            ExecuteStatement(state, loop.body, outBlock, true);
            if (state->hadError) return false;
            bool done = false;
            if (!checkUntil(&done)) return false;
            if (done) break;
        }
        return true;
    }

    if (loop.untilCondition.IsNull()) {
        Report(state, stmt, "Infinite eval loops require an until condition");
        return false;
    }
    while (true) {
        if (++state->budget.loopIterations > state->budget.maxLoopIterations) {
            Report(state, stmt, "Eval loop exceeded iteration limit");
            return false;
        }
        ExecuteStatement(state, loop.body, outBlock, true);
        if (state->hadError) return false;
        bool done = false;
        if (!checkUntil(&done)) return false;
        if (done) return true;
    }
}

static bool ExecuteEvalWhile(ComptimeState* state, NodeRef stmt, BlockData& outBlock) {
    const ForCStyleData& loop = state->ast->GetForCStyle(stmt);

    while (true) {
        LiteralValue conditionValue;
        if (!EvalExpression(state, loop.condition, &conditionValue)) {
            Report(state, loop.condition, "Eval while condition must be a compile-time value");
            return false;
        }

        bool conditionTrue = false;
        if (!LiteralToBool(conditionValue, &conditionTrue)) {
            Report(state, loop.condition, "Eval while condition must resolve to bool, int, uint, or float");
            return false;
        }
        if (!conditionTrue) return true;

        if (++state->budget.loopIterations > state->budget.maxLoopIterations) {
            Report(state, stmt, "Eval while exceeded iteration limit");
            return false;
        }
        ExecuteStatement(state, loop.body, outBlock, true);
        if (state->hadError) return false;
    }
}

static bool ExecuteStatement(ComptimeState* state, NodeRef stmt, BlockData& outBlock, bool evalContext) {
    if (stmt.IsNull()) return true;
    if (!CheckExecutedBudget(state, stmt)) return false;

    switch (stmt.Type()) {
        case ASTNodeType::EVAL_BLOCK:
            return ExecuteBlockAsEval(state, stmt, outBlock);
        case ASTNodeType::EVAL_IF:
            return ExecuteEvalIf(state, stmt, outBlock, evalContext);
        case ASTNodeType::BLOCK:
            if (evalContext) return ExecuteBlockAsEval(state, stmt, outBlock);
            break;
        case ASTNodeType::VARIABLE_DECL: {
            const VariableDeclData& decl = state->ast->GetVariableDecl(stmt);
            if (decl.isEval || (evalContext && decl.isConst)) {
                BindCompileTimeDecl(state, stmt, evalContext);
                return !state->hadError;
            }
            break;
        }
        case ASTNodeType::ASSIGNMENT:
            if (ExecuteAssignmentIfComptime(state, stmt)) return !state->hadError;
            break;
        case ASTNodeType::IF_STATEMENT:
            if (evalContext) return ExecuteEvalIf(state, stmt, outBlock, true);
            break;
        case ASTNodeType::FOR_RANGE: {
            const ForRangeData& loop = state->ast->GetForRange(stmt);
            if (evalContext || loop.isEval) return ExecuteEvalForRange(state, stmt, outBlock);
            break;
        }
        case ASTNodeType::FOR_COLLECTION: {
            const ForCollectionData& loop = state->ast->GetForCollection(stmt);
            if (evalContext || loop.isEval) {
                if (loop.iteratorVar.Type() != ASTNodeType::IDENTIFIER) {
                    Report(state, stmt, "Eval collection iterator must be an identifier");
                    return false;
                }
                const IdentifierData& iterator = state->ast->GetIdentifier(loop.iteratorVar);
                for (u32 i = 0; i < loop.length; i++) {
                    if (++state->budget.loopIterations > state->budget.maxLoopIterations) {
                        Report(state, stmt, "Eval collection loop exceeded iteration limit");
                        return false;
                    }
                    LiteralValue value{};
                    value.type = LiteralValue::INT;
                    value.intValue = static_cast<int>(i);
                    PushScope(state);
                    AddBinding(state, iterator.name.nameHash, TYPE_INFO(CoreType::INT, 1, false), value);
                    ExecuteStatement(state, loop.body, outBlock, true);
                    PopScope(state);
                    if (state->hadError) return false;
                }
                return true;
            }
            break;
        }
        case ASTNodeType::FOR_CSTYLE: {
            const ForCStyleData& loop = state->ast->GetForCStyle(stmt);
            if (evalContext || loop.isEval) {
                if (loop.isWhile) return ExecuteEvalWhile(state, stmt, outBlock);
                Report(state, stmt, "C-style for loops are not supported in eval contexts");
                return false;
            }
            break;
        }
        case ASTNodeType::LOOP: {
            const LoopData& loop = state->ast->GetLoop(stmt);
            if (evalContext || loop.isEval) return ExecuteEvalLoop(state, stmt, outBlock);
            break;
        }
        default:
            break;
    }

    if (!ProcessRuntimeChildren(state, stmt)) return false;
    NodeRef cloned = CloneNode(state, stmt);
    if (!cloned.IsValid()) return !state->hadError;
    if (!CheckEmitBudget(state, stmt)) return false;
    AppendStatement(state, outBlock, cloned);

    if (stmt.Type() == ASTNodeType::VARIABLE_DECL) {
        const VariableDeclData& decl = state->ast->GetVariableDecl(stmt);
        AddShadow(state, decl.name.nameHash);
    }
    return !state->hadError;
}

static bool ProcessBlock(ComptimeState* state, NodeRef blockRef) {
    if (blockRef.IsNull() || blockRef.Type() != ASTNodeType::BLOCK) return true;
    ArenaArray<NodeRef> inputStatements = state->ast->GetBlock(blockRef).statements;
    u32 inputCount = inputStatements.count;
    BlockData rewritten{};
    rewritten.statements.Init(state->arena, inputCount > 0 ? inputCount : 1);

    u32 listStart = state->emittedStatements.count;
    PushScope(state);
    for (u32 i = 0; i < inputCount && !state->hadError; i++) {
        ExecuteStatement(state, inputStatements[i], rewritten, false);
    }
    PopScope(state);

    ComptimeEmitList emitList{};
    emitList.firstStatement = listStart;
    emitList.statementCount = state->emittedStatements.count - listStart;
    state->emitLists.Push(state->arena, emitList);

    state->ast->GetBlock(blockRef).statements = rewritten.statements;
    return !state->hadError;
}

static bool ProcessFunction(ComptimeState* state, NodeRef functionRef) {
    if (functionRef.IsNull()) return true;
    FunctionDeclData& fn = state->ast->GetFunction(functionRef);
    if (fn.isEval) return true;
    if (fn.body.IsNull()) return true;
    if (fn.body.Type() == ASTNodeType::BLOCK) return ProcessBlock(state, fn.body);
    if (fn.body.Type() == ASTNodeType::VERTEX_STAGE ||
        fn.body.Type() == ASTNodeType::FRAGMENT_STAGE ||
        fn.body.Type() == ASTNodeType::COMPUTE_STAGE) {
        ShaderStageData& stage = state->ast->GetShaderStage(fn.body);
        return stage.body.IsNull() || ProcessBlock(state, stage.body);
    }
    return true;
}

static bool ProcessPass(ComptimeState* state, NodeRef passRef) {
    PassData& pass = state->ast->GetPass(passRef);

    auto processStage = [&](NodeRef stageRef) -> bool {
        if (stageRef.IsNull()) return true;
        ShaderStageData& stage = state->ast->GetShaderStage(stageRef);
        return stage.body.IsNull() || ProcessBlock(state, stage.body);
    };

    if (pass.consts.count > 0) {
        NodeRef wrapper = ASTFactory::MakeBlock(state->ast, state->ast->GetLine(pass.consts[0]),
                                                state->ast->GetColumn(pass.consts[0]));
        for (u32 i = 0; i < pass.consts.count; i++) {
            state->ast->GetBlock(wrapper).statements.Push(state->arena, pass.consts[i]);
        }
        if (!ProcessBlock(state, wrapper)) return false;
        const BlockData& processed = state->ast->GetBlock(wrapper);
        pass.consts = processed.statements;
    }

    for (u32 i = 0; i < pass.functions.count; i++) {
        if (!ProcessFunction(state, pass.functions[i])) return false;
    }

    return processStage(pass.vertexShader) &&
           processStage(pass.fragmentShader) &&
           processStage(pass.computeShader);
}

static bool ProcessEnum(ComptimeState* state, NodeRef enumRef) {
    const EnumDeclData& enumDecl = state->ast->GetEnumDecl(enumRef);
    for (u32 i = 0; i < enumDecl.methods.count; i++) {
        if (!ProcessFunction(state, enumDecl.methods[i])) return false;
    }
    return true;
}

static bool ProcessPipeline(ComptimeState* state, NodeRef pipelineRef) {
    PipelineData& pipeline = state->ast->GetPipeline(pipelineRef);
    for (u32 i = 0; i < pipeline.functions.count; i++) {
        if (!ProcessFunction(state, pipeline.functions[i])) return false;
    }
    for (u32 i = 0; i < pipeline.enums.count; i++) {
        if (!ProcessEnum(state, pipeline.enums[i])) return false;
    }
    for (u32 i = 0; i < pipeline.passes.count; i++) {
        if (!ProcessPass(state, pipeline.passes[i])) return false;
    }
    return true;
}

static bool ProcessModule(ComptimeState* state, NodeRef moduleRef) {
    ModuleNodeData& module = state->ast->GetModule(moduleRef);
    for (u32 i = 0; i < module.functions.count; i++) {
        if (!ProcessFunction(state, module.functions[i])) return false;
    }
    for (u32 i = 0; i < module.enums.count; i++) {
        if (!ProcessEnum(state, module.enums[i])) return false;
    }
    return true;
}

static bool ValidateNoEvalNodes(ComptimeState* state, NodeRef node) {
    if (node.IsNull() || state->hadError) return !state->hadError;
    switch (node.Type()) {
        case ASTNodeType::EVAL_BLOCK:
        case ASTNodeType::EVAL_IF:
            Report(state, node, "Comptime pass left an eval node before IR lowering");
            return false;
        case ASTNodeType::BLOCK: {
            const BlockData& block = state->ast->GetBlock(node);
            for (u32 i = 0; i < block.statements.count; i++) {
                if (!ValidateNoEvalNodes(state, block.statements[i])) return false;
            }
            return true;
        }
        case ASTNodeType::IF_STATEMENT: {
            const BlockData& block = state->ast->GetBlock(node);
            for (u32 i = 0; i < block.statements.count; i++) {
                if (!ValidateNoEvalNodes(state, block.statements[i])) return false;
            }
            return true;
        }
        case ASTNodeType::FOR_CSTYLE: {
            const ForCStyleData& loop = state->ast->GetForCStyle(node);
            return ValidateNoEvalNodes(state, loop.init) &&
                   ValidateNoEvalNodes(state, loop.condition) &&
                   ValidateNoEvalNodes(state, loop.increment) &&
                   ValidateNoEvalNodes(state, loop.body);
        }
        case ASTNodeType::FOR_RANGE: {
            const ForRangeData& loop = state->ast->GetForRange(node);
            if (loop.isEval) {
                Report(state, node, "Comptime pass left an eval for before IR lowering");
                return false;
            }
            return ValidateNoEvalNodes(state, loop.body);
        }
        case ASTNodeType::FOR_COLLECTION: {
            const ForCollectionData& loop = state->ast->GetForCollection(node);
            if (loop.isEval) {
                Report(state, node, "Comptime pass left an eval collection loop before IR lowering");
                return false;
            }
            return ValidateNoEvalNodes(state, loop.body);
        }
        case ASTNodeType::LOOP: {
            const LoopData& loop = state->ast->GetLoop(node);
            if (loop.isEval) {
                Report(state, node, "Comptime pass left an eval loop before IR lowering");
                return false;
            }
            return ValidateNoEvalNodes(state, loop.body);
        }
        default:
            return true;
    }
}

} // namespace

bool RunComptimeInterpreter(CompilationContext* context, Parser* parser,
                            NodeRef root, std::string* outError) {
    if (!context || !parser || root.IsNull()) return true;

    ComptimeState state{};
    state.context = context;
    state.parser = parser;
    state.ast = &context->ast;
    state.symbols = &parser->symbolTable;
    state.arena = &context->arena;
    state.sourceBase = parser->sourceBase();
    state.hadError = false;
    state.values.Init(state.arena, 64);
    state.bindings.Init(state.arena, 64);
    state.scopes.Init(state.arena, 32);
    state.frames.Init(state.arena, 32);
    state.emittedStatements.Init(state.arena, 128);
    state.emitLists.Init(state.arena, 32);
    state.diagnostics.Init(state.arena, 8);
    InitBudget(&state.budget);

    bool ok = true;
    if (root.Type() == ASTNodeType::PIPELINE) {
        ok = ProcessPipeline(&state, root);
        if (ok) {
            const PipelineData& pipeline = state.ast->GetPipeline(root);
            for (u32 i = 0; i < pipeline.passes.count && ok; i++) {
                const PassData& pass = state.ast->GetPass(pipeline.passes[i]);
                if (pass.vertexShader.IsValid()) ok = ValidateNoEvalNodes(&state, state.ast->GetShaderStage(pass.vertexShader).body);
                if (ok && pass.fragmentShader.IsValid()) ok = ValidateNoEvalNodes(&state, state.ast->GetShaderStage(pass.fragmentShader).body);
                if (ok && pass.computeShader.IsValid()) ok = ValidateNoEvalNodes(&state, state.ast->GetShaderStage(pass.computeShader).body);
            }
        }
    } else if (root.Type() == ASTNodeType::MODULE) {
        ok = ProcessModule(&state, root);
    }

    if (!ok || state.hadError) {
        if (outError) {
            if (state.diagnostics.count > 0 && state.diagnostics[0].message) {
                *outError = state.diagnostics[0].message;
            } else {
                *outError = "Comptime interpretation failed";
            }
        }
        return false;
    }
    return true;
}

} // namespace BWSL::Comptime
