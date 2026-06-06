// Part of bwsl_parser_soa.cpp. Include from that file only.
// Shader lookup, AST cloning, variant specialization, and final stage resolution.

namespace BWSL {

static std::string ArenaStringToStdString(const ArenaString& str, const char* sourceBase) {
    return str.isHashOnly() ? ReverseLookup::GetString(str.nameHash)
                            : str.ToString(sourceBase);
}

static ArenaString MakeRegisteredArenaString(const std::string& str) {
    u32 hash = Utils::HashStr(str.c_str());
    ReverseLookup::Register(hash, str.c_str());
    return ArenaString::MakeHashOnly(hash);
}

static bool SameTypeInfoForVariantBinding(const TypeInfo& lhs, const TypeInfo& rhs,
                                          u32 lhsEnumHash, u32 rhsEnumHash) {
    if (lhsEnumHash != rhsEnumHash) return false;
    return lhs.coreType == rhs.coreType &&
           lhs.componentCount == rhs.componentCount &&
           lhs.arrayDimensions == rhs.arrayDimensions &&
           lhs.customTypeHash == rhs.customTypeHash;
}

NodeRef Parser::LookupShaderFunction(u32 nameHash, const PassData& pass, CoreType expectedReturnType) {
    // Search in pass-scoped functions first
    for (u32 i = 0; i < pass.functions.count; i++) {
        const FunctionDeclData& func = ast->GetFunction(pass.functions[i]);
        if (func.name.nameHash == nameHash && func.returnType == expectedReturnType) {
            return pass.functions[i];
        }
    }

    // Search in pipeline-scoped functions
    if (currentPipeline.IsValid()) {
        const PipelineData& pipeline = ast->GetPipeline(currentPipeline);
        for (u32 i = 0; i < pipeline.functions.count; i++) {
            const FunctionDeclData& func = ast->GetFunction(pipeline.functions[i]);
            if (func.name.nameHash == nameHash && func.returnType == expectedReturnType) {
                return pipeline.functions[i];
            }
        }
    }

    // Search in global functions
    for (u32 i = 0; i < ast->functions.count; i++) {
        const FunctionDeclData& func = ast->GetFunction(NodeRef(ASTNodeType::FUNCTION, i));
        if (func.name.nameHash == nameHash && func.returnType == expectedReturnType) {
            return NodeRef(ASTNodeType::FUNCTION, i);
        }
    }

    return NodeRef::Null();
}

NodeRef Parser::ResolveShaderStageExpr(NodeRef stageNode, const PassData& pass, ASTNodeType expectedType) {
    if (stageNode.IsNull()) return stageNode;

    ShaderStageData& stage = ast->GetShaderStage(stageNode);
    if (!stage.isDeferred) return stageNode;  // Already resolved

    NodeRef expr = stage.deferredExpr;
    if (expr.IsNull()) {
        Error("Deferred shader stage has no expression");
        return NodeRef::Null();
    }

    ASTNodeType exprType = expr.Type();

    if (exprType == ASTNodeType::TERNARY_EXPRESSION) {
        // Evaluate condition at compile-time
        const TernaryExprData& ternary = ast->GetTernaryExpression(expr);

        EvalStateSoA evalState;
        CompileTimeEvaluatorSoA::Init(&evalState, this, ast, &context->evalCache, ast->arena);

        LiteralValue condValue;
        if (!CompileTimeEvaluatorSoA::CanEvaluateNode(&evalState, ternary.condition)) {
            return stageNode;
        }

        if (!CompileTimeEvaluatorSoA::EvaluateNode(&evalState, ternary.condition, &condValue)) {
            return stageNode;
        }

        if (condValue.type != LiteralValue::BOOL) {
            return stageNode;
        }

        // Select the branch and recursively resolve
        NodeRef selectedBranch = condValue.boolValue ? ternary.trueExpr : ternary.falseExpr;

        // Create a new deferred stage node for the selected branch and resolve it
        NodeRef newStageNode = ASTFactory::MakeShaderStage(ast, expectedType, NodeRef::Null(),
            ast->GetLine(stageNode), ast->GetColumn(stageNode));
        ast->GetShaderStage(newStageNode).isDeferred = true;
        ast->GetShaderStage(newStageNode).deferredExpr = selectedBranch;

        return ResolveShaderStageExpr(newStageNode, pass, expectedType);
    }
    else if (exprType == ASTNodeType::FUNCTION_CALL) {
        const FunctionCallData& call = ast->GetFunctionCall(expr);

        // Determine expected return type
        CoreType expectedReturnType = (expectedType == ASTNodeType::VERTEX_STAGE) ? CoreType::VERTEX_FUNCTION :
                                      (expectedType == ASTNodeType::FRAGMENT_STAGE) ? CoreType::FRAGMENT_FUNCTION :
                                      CoreType::COMPUTE_FUNCTION;

        // Look up the function
        NodeRef funcRef = LookupShaderFunction(call.name.nameHash, pass, expectedReturnType);
        if (funcRef.IsNull()) {
            Error("Cannot find shader function with expected return type");
            return NodeRef::Null();
        }

        const FunctionDeclData& func = ast->GetFunction(funcRef);

        // The function body should be the shader stage
        NodeRef shaderBody = func.body;
        if (shaderBody.IsNull() || shaderBody.Type() != expectedType) {
            Error("Shader function does not contain expected shader stage");
            return NodeRef::Null();
        }

        // Handle parameters if any (substitute with evaluated argument values)
        if (func.parameters.count > 0) {
            if (call.arguments.count < func.parameters.count) {
                Error("Not enough arguments for shader function call");
                return NodeRef::Null();
            }

            // Evaluate all arguments at compile-time
            ParamSubstitution paramSubs[16];
            u32 paramSubCount = 0;

            EvalStateSoA evalState;
            CompileTimeEvaluatorSoA::Init(&evalState, this, ast, &context->evalCache, ast->arena);

            for (u32 i = 0; i < func.parameters.count && i < 16; i++) {
                LiteralValue argValue;
                if (!CompileTimeEvaluatorSoA::CanEvaluateNode(&evalState, call.arguments[i])) {
                    return stageNode;
                }
                if (!CompileTimeEvaluatorSoA::EvaluateNode(&evalState, call.arguments[i], &argValue)) {
                    return stageNode;
                }
                paramSubs[paramSubCount].nameHash = func.parameters[i].first.nameHash;
                paramSubs[paramSubCount].value = argValue;
                paramSubCount++;
            }

            // Clone the shader stage body with parameter substitution
            shaderBody = CloneShaderStageWithParams(shaderBody, paramSubs, paramSubCount);
        }

        // Return the resolved shader stage
        return shaderBody;
    }
    else {
        Error("Invalid expression type for shader stage assignment (expected function call or ternary)");
        return NodeRef::Null();
    }
}

//==============================================================================
// Parameter Substitution Cloning
// Clones AST nodes while replacing parameter identifiers with literal values
//==============================================================================

NodeRef Parser::CloneNodeWithParams(NodeRef node, const ParamSubstitution* subs, u32 subCount) {
    if (node.IsNull()) return NodeRef::Null();

    if (cloneDepth >= MAX_CLONE_DEPTH) {
        Error("Expression nesting too deep to clone (exceeded 2048 levels)");
        return NodeRef::Null();
    }

    // Share the eval-expansion budget with node-level cloning. A single
    // CloneNodeWithParams call can build a huge tree (deep ternary
    // expressions, wide function calls), and without a node-count cap a
    // clone inside an eval loop can exhaust the arena even when the outer
    // expansion budget isn't yet depleted.
    if (evalExpansionBudget == 0) {
        return NodeRef::Null();
    }
    evalExpansionBudget--;

    struct DepthGuard {
        u32& d;
        DepthGuard(u32& d_) : d(d_) { ++d; }
        ~DepthGuard() { --d; }
    } _depth_guard_(cloneDepth);

    u32 line = ast->GetLine(node);
    u32 col = ast->GetColumn(node);

    switch (node.Type()) {
        case ASTNodeType::IDENTIFIER: {
            const IdentifierData& src = ast->GetIdentifier(node);
            // Check if this identifier should be substituted
            for (u32 i = 0; i < subCount; i++) {
                if (subs[i].nameHash == src.name.nameHash) {
                    return MakeLiteralNodeFromValue(subs[i].value, line, col);
                }
            }
            if (passBlockRemapActive && remapBareVariants) {
                for (const PassBlockNameRemap& remap : passBlockVariantRemaps) {
                    if (remap.localHash == src.name.nameHash) {
                        NodeRef cloned = ASTFactory::MakeIdentifier(ast, remap.targetName, line, col);
                        ast->GetIdentifier(cloned).identifierKind = src.identifierKind;
                        return cloned;
                    }
                }
            }
            LiteralValue variantValue;
            if (allowBareVariantLookup &&
                LookupActiveVariantBinding(src.name.nameHash, &variantValue)) {
                return MakeLiteralNodeFromValue(variantValue, line, col);
            }
            // Not a parameter - clone as-is. Preserve identifierKind so
            // downstream passes (e.g. IR lowering's variants.X handler) can
            // still recognize special identifiers after cloning.
            NodeRef cloned = ASTFactory::MakeIdentifier(ast, src.name, line, col);
            ast->GetIdentifier(cloned).identifierKind = src.identifierKind;
            return cloned;
        }

        case ASTNodeType::LITERAL: {
            const LiteralData& src = ast->GetLiteral(node);
            switch (src.value.type) {
                case LiteralValue::FLOAT:
                    return ASTFactory::MakeLiteralFloat(ast, src.value.floatValue, line, col);
                case LiteralValue::INT:
                    return ASTFactory::MakeLiteralInt(ast, src.value.intValue, line, col);
                case LiteralValue::UINT:
                    return ASTFactory::MakeLiteralUint(ast, src.value.uintValue, line, col);
                case LiteralValue::BOOL:
                    return ASTFactory::MakeLiteralBool(ast, src.value.boolValue, line, col);
                default:
                    return NodeRef::Null();
            }
        }

        case ASTNodeType::BINARY_OP: {
            const BinaryOpData& src = ast->GetBinaryOp(node);
            NodeRef left = CloneNodeWithParams(src.left, subs, subCount);
            NodeRef right = CloneNodeWithParams(src.right, subs, subCount);
            return ASTFactory::MakeBinaryOp(ast, src.op, left, right, line, col);
        }

        case ASTNodeType::UNARY_OP: {
            const UnaryOpData& src = ast->GetUnaryOp(node);
            NodeRef operand = CloneNodeWithParams(src.operand, subs, subCount);
            return ASTFactory::MakeUnaryOp(ast, src.op, operand, line, col);
        }

        case ASTNodeType::MEMBER_ACCESS: {
            const MemberAccessData& src = ast->GetMemberAccess(node);
            ArenaString memberName = src.member;
            if (src.object.Type() == ASTNodeType::IDENTIFIER) {
                const IdentifierData& objectIdent = ast->GetIdentifier(src.object);
                if (passBlockRemapActive) {
                    const std::vector<PassBlockNameRemap>* remaps = nullptr;
                    if (objectIdent.identifierKind == SpecialIdentifier::ATTRIBUTES) {
                        remaps = &passBlockAttributeRemaps;
                    } else if (objectIdent.identifierKind == SpecialIdentifier::RESOURCES) {
                        remaps = &passBlockResourceRemaps;
                    } else if (objectIdent.identifierKind == SpecialIdentifier::VARIANTS) {
                        remaps = &passBlockVariantRemaps;
                    }
                    if (remaps) {
                        bool foundRemap = false;
                        for (const PassBlockNameRemap& remap : *remaps) {
                            if (remap.localHash == src.member.nameHash) {
                                memberName = remap.targetName;
                                foundRemap = true;
                                break;
                            }
                        }
                        if (!foundRemap) {
                            Error("Missing pass_block interface mapping");
                        }
                    }
                }
                if (objectIdent.identifierKind == SpecialIdentifier::VARIANTS) {
                    LiteralValue variantValue;
                    if (LookupActiveVariantBinding(memberName.nameHash, &variantValue)) {
                        return MakeLiteralNodeFromValue(variantValue, line, col);
                    }
                }
            }
            NodeRef object = CloneNodeWithParams(src.object, subs, subCount);
            NodeRef memberNode = ASTFactory::MakeMemberAccess(ast, object, memberName, line, col);
            MemberAccessData& memberData = ast->GetMemberAccess(memberNode);
            memberData.isModuleQualified = src.isModuleQualified;
            memberData.qualifiedNameHash = src.qualifiedNameHash;
            return memberNode;
        }

        case ASTNodeType::ARRAY_ACCESS: {
            const ArrayAccessData& src = ast->GetArrayAccess(node);
            NodeRef array = CloneNodeWithParams(src.array, subs, subCount);
            NodeRef index = CloneNodeWithParams(src.index, subs, subCount);
            return ASTFactory::MakeArrayAccess(ast, array, index, line, col);
        }

        case ASTNodeType::ASSIGNMENT: {
            const AssignmentData& src = ast->GetAssignment(node);
            NodeRef target = CloneNodeWithParams(src.target, subs, subCount);
            NodeRef value = CloneNodeWithParams(src.value, subs, subCount);
            return ASTFactory::MakeAssignment(ast, target, value, line, col, src.interpolation);
        }

        case ASTNodeType::BLOCK:
        case ASTNodeType::EVAL_BLOCK: {
            const BlockData& src = ast->GetBlock(node);
            std::vector<NodeRef> statements;
            statements.reserve(src.statements.count);
            for (u32 i = 0; i < src.statements.count; i++) {
                statements.push_back(src.statements[i]);
            }
            NodeRef newBlock = node.Type() == ASTNodeType::EVAL_BLOCK
                ? ASTFactory::MakeEvalBlock(ast, line, col)
                : ASTFactory::MakeBlock(ast, line, col);
            for (NodeRef stmt : statements) {
                NodeRef cloned = CloneNodeWithParams(stmt, subs, subCount);
                if (cloned.IsValid()) {
                    ast->GetBlock(newBlock).statements.Push(arena, cloned);
                }
            }
            return newBlock;
        }

        case ASTNodeType::VARIABLE_DECL: {
            const VariableDeclData& src = ast->GetVariableDecl(node);
            NodeRef initializer = CloneNodeWithParams(src.initializer, subs, subCount);
            NodeRef cloned = ASTFactory::MakeVariableDecl(ast, src.name, src.type, initializer, src.isConst,
                                                          line, col, src.storageClass, src.arrayDimensions,
                                                          src.arrayLength, src.arrayElementTypeHash);
            ast->GetVariableDecl(cloned).isEval = src.isEval;
            return cloned;
        }

        case ASTNodeType::FUNCTION_CALL: {
            const FunctionCallData& src = ast->GetFunctionCall(node);
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
                ? CloneNodeWithParams(moduleObject, subs, subCount)
                : NodeRef::Null();

            NodeRef newCall = ASTFactory::MakeFunctionCall(ast, name, line, col);
            FunctionCallData& dst = ast->GetFunctionCall(newCall);
            dst.intrinsicIndex = intrinsicIndex;
            dst.flags = flags;
            dst.moduleIndex = moduleIndex;
            dst.moduleQualifiedHash = moduleQualifiedHash;
            dst.moduleObject = clonedModuleObject;
            for (NodeRef arg : args) {
                NodeRef clonedArg = CloneNodeWithParams(arg, subs, subCount);
                ast->GetFunctionCall(newCall).arguments.Push(arena, clonedArg);
            }
            return newCall;
        }

        case ASTNodeType::TERNARY_EXPRESSION: {
            const TernaryExprData& src = ast->GetTernaryExpression(node);
            NodeRef condition = CloneNodeWithParams(src.condition, subs, subCount);
            if (!activeVariantBindings.empty()) {
                EvalStateSoA evalState;
                CompileTimeEvaluatorSoA::Init(&evalState, this, ast, &context->evalCache, ast->arena);
                LiteralValue condValue;
                if (CompileTimeEvaluatorSoA::CanEvaluateNode(&evalState, condition) &&
                    CompileTimeEvaluatorSoA::EvaluateNode(&evalState, condition, &condValue)) {
                    bool conditionTrue = false;
                    if (ConvertLiteralToBool(condValue, &conditionTrue)) {
                        return CloneNodeWithParams(conditionTrue ? src.trueExpr : src.falseExpr, subs, subCount);
                    }
                }
            }
            NodeRef trueExpr = CloneNodeWithParams(src.trueExpr, subs, subCount);
            NodeRef falseExpr = CloneNodeWithParams(src.falseExpr, subs, subCount);
            return ASTFactory::MakeTernaryExpr(ast, condition, trueExpr, falseExpr, line, col);
        }

        case ASTNodeType::IF_STATEMENT:
        case ASTNodeType::EVAL_IF: {
            // IF_STATEMENT uses BlockData: [condition, thenBranch, elseBranch?]
            const BlockData& src = ast->GetBlock(node);
            std::vector<NodeRef> statements;
            statements.reserve(src.statements.count);
            for (u32 i = 0; i < src.statements.count; i++) {
                statements.push_back(src.statements[i]);
            }
            if (statements.empty()) {
                return ASTFactory::MakeBlock(ast, line, col);
            }
            NodeRef condition = CloneNodeWithParams(statements[0], subs, subCount);
            if (!activeVariantBindings.empty()) {
                EvalStateSoA evalState;
                CompileTimeEvaluatorSoA::Init(&evalState, this, ast, &context->evalCache, ast->arena);
                LiteralValue condValue;
                if (CompileTimeEvaluatorSoA::CanEvaluateNode(&evalState, condition) &&
                    CompileTimeEvaluatorSoA::EvaluateNode(&evalState, condition, &condValue)) {
                    bool conditionTrue = false;
                    if (ConvertLiteralToBool(condValue, &conditionTrue)) {
                        if (conditionTrue && statements.size() >= 2) {
                            return CloneNodeWithParams(statements[1], subs, subCount);
                        }
                        if (!conditionTrue && statements.size() >= 3) {
                            return CloneNodeWithParams(statements[2], subs, subCount);
                        }
                        return ASTFactory::MakeBlock(ast, line, col);
                    }
                }
            }
            NodeRef newIf = node.Type() == ASTNodeType::EVAL_IF
                ? ASTFactory::MakeEvalIfStatement(ast, line, col)
                : ASTFactory::MakeIfStatement(ast, line, col);
            ast->GetBlock(newIf).statements.Push(arena, condition);
            for (u32 i = 1; i < statements.size(); i++) {
                NodeRef cloned = CloneNodeWithParams(statements[i], subs, subCount);
                if (cloned.IsValid()) {
                    ast->GetBlock(newIf).statements.Push(arena, cloned);
                }
            }
            return newIf;
        }

        case ASTNodeType::FOR_CSTYLE: {
            const ForCStyleData& src = ast->GetForCStyle(node);
            NodeRef init = CloneNodeWithParams(src.init, subs, subCount);
            NodeRef condition = CloneNodeWithParams(src.condition, subs, subCount);
            NodeRef increment = CloneNodeWithParams(src.increment, subs, subCount);
            NodeRef body = CloneNodeWithParams(src.body, subs, subCount);
            NodeRef newFor = ASTFactory::MakeForCStyle(ast, init, condition, increment, body,
                                                       src.isEval, line, col, src.isWhile);
            return newFor;
        }

        case ASTNodeType::FOR_RANGE: {
            const ForRangeData& src = ast->GetForRange(node);
            NodeRef iteratorVar = CloneNodeWithParams(src.iteratorVar, subs, subCount);
            NodeRef rangeStart = CloneNodeWithParams(src.rangeStart, subs, subCount);
            NodeRef rangeEnd = CloneNodeWithParams(src.rangeEnd, subs, subCount);
            NodeRef step = CloneNodeWithParams(src.step, subs, subCount);
            NodeRef body = CloneNodeWithParams(src.body, subs, subCount);
            return ASTFactory::MakeForRange(ast, iteratorVar, rangeStart, rangeEnd, step,
                                            body, src.inclusive, src.isEval, line, col);
        }

        case ASTNodeType::FOR_COLLECTION: {
            const ForCollectionData& src = ast->GetForCollection(node);
            NodeRef iteratorVar = CloneNodeWithParams(src.iteratorVar, subs, subCount);
            NodeRef collection = CloneNodeWithParams(src.collection, subs, subCount);
            NodeRef body = CloneNodeWithParams(src.body, subs, subCount);
            return ASTFactory::MakeForCollection(ast, iteratorVar, collection, body,
                                                 src.isEval, src.length, line, col);
        }

        case ASTNodeType::LOOP: {
            const LoopData& src = ast->GetLoop(node);
            NodeRef count = CloneNodeWithParams(src.count, subs, subCount);
            NodeRef body = CloneNodeWithParams(src.body, subs, subCount);
            NodeRef untilCondition = CloneNodeWithParams(src.untilCondition, subs, subCount);
            return ASTFactory::MakeLoop(ast, count, body, untilCondition, src.isEval, line, col);
        }

        case ASTNodeType::RETURN: {
            // RETURN uses AssignmentData
            const AssignmentData& src = ast->GetAssignment(node);
            NodeRef value = src.value.IsValid() ? CloneNodeWithParams(src.value, subs, subCount) : NodeRef::Null();
            return ASTFactory::MakeReturn(ast, value, line, col);
        }

        default:
            // For other node types, just return a clone without recursion
            // This might need expansion for more complex cases
            return node;
    }
}

NodeRef Parser::CloneShaderStageWithParams(NodeRef stageNode, const ParamSubstitution* subs, u32 subCount) {
    if (stageNode.IsNull()) return stageNode;

    const ShaderStageData& src = ast->GetShaderStage(stageNode);
    u32 line = ast->GetLine(stageNode);
    u32 col = ast->GetColumn(stageNode);

    // Clone the body with parameter substitution
    NodeRef newBody = CloneNodeWithParams(src.body, subs, subCount);

    // Create a new shader stage with the cloned body
    NodeRef newStage = ASTFactory::MakeShaderStage(ast, stageNode.Type(), newBody, line, col);
    ShaderStageData& dst = ast->GetShaderStage(newStage);

    // Copy other fields
    dst.workgroupSizeX = src.workgroupSizeX;
    dst.workgroupSizeY = src.workgroupSizeY;
    dst.workgroupSizeZ = src.workgroupSizeZ;
    dst.name = src.name;
    // Don't copy inheritsFrom/isInherited/isDeferred - this is a fresh resolved stage

    return newStage;
}

NodeRef Parser::ClonePassWithParamsAndRemap(NodeRef passRef, const ParamSubstitution* subs, u32 subCount,
                                            const std::string& passName) {
    const PassData& srcPass = ast->GetPass(passRef);
    NodeRef newPass = ASTFactory::MakePass(ast, passName, ast->GetLine(passRef), ast->GetColumn(passRef));
    PassData& dstPass = ast->GetPass(newPass);

    auto findRemap = [](const std::vector<PassBlockNameRemap>& remaps,
                        const ArenaString& localName) -> const PassBlockNameRemap* {
        for (const PassBlockNameRemap& remap : remaps) {
            if (remap.localHash == localName.nameHash) {
                return &remap;
            }
        }
        return nullptr;
    };

    for (u32 i = 0; i < srcPass.usedAttributes.count; i++) {
        ArenaString name = srcPass.usedAttributes[i];
        const PassBlockNameRemap* remap = passBlockRemapActive
            ? findRemap(passBlockAttributeRemaps, name)
            : nullptr;
        if (passBlockRemapActive && !remap) {
            Error("Missing pass_block attribute mapping");
            continue;
        }
        ArenaString targetName = remap ? remap->targetName : name;
        dstPass.usedAttributes.Push(arena, targetName);
        if (remap && remap->localIndex < 32 && remap->targetIndex < 32 &&
            (srcPass.optionalAttributesMask & (1u << remap->localIndex))) {
            dstPass.optionalAttributesMask |= (1u << remap->targetIndex);
        }
    }

    for (u32 i = 0; i < srcPass.usedResources.count; i++) {
        ArenaString name = srcPass.usedResources[i];
        const PassBlockNameRemap* remap = passBlockRemapActive
            ? findRemap(passBlockResourceRemaps, name)
            : nullptr;
        if (passBlockRemapActive && !remap) {
            Error("Missing pass_block resource mapping");
            continue;
        }
        ArenaString targetName = remap ? remap->targetName : name;
        dstPass.usedResources.Push(arena, targetName);
        if (remap && remap->localIndex < 32 && remap->targetIndex < 32 &&
            (srcPass.optionalResourcesMask & (1u << remap->localIndex))) {
            dstPass.optionalResourcesMask |= (1u << remap->targetIndex);
        }
    }

    dstPass.hasFragmentOutputs = srcPass.hasFragmentOutputs;
    for (u32 i = 0; i < srcPass.fragmentOutputs.count; i++) {
        dstPass.fragmentOutputs.Push(arena, srcPass.fragmentOutputs[i]);
    }

    for (u32 i = 0; i < srcPass.consts.count; i++) {
        dstPass.consts.Push(arena, CloneNodeWithParams(srcPass.consts[i], subs, subCount));
    }

    for (u32 i = 0; i < srcPass.functions.count; i++) {
        NodeRef fnRef = srcPass.functions[i];
        const FunctionDeclData& srcFn = ast->GetFunction(fnRef);
        std::string fnName = srcFn.name.isHashOnly()
            ? ReverseLookup::GetString(srcFn.name.nameHash)
            : srcFn.name.ToString(sourceBase());
        NodeRef newFn = ASTFactory::MakeFunction(ast, fnName, srcFn.returnType,
                                                ast->GetLine(fnRef), ast->GetColumn(fnRef));
        FunctionDeclData& dstFn = ast->GetFunction(newFn);
        for (u32 j = 0; j < srcFn.parameters.count; j++) {
            dstFn.parameters.Push(arena, srcFn.parameters[j]);
        }
        dstFn.isEval = srcFn.isEval;
        if (srcFn.body.IsValid() &&
            (srcFn.body.Type() == ASTNodeType::VERTEX_STAGE ||
             srcFn.body.Type() == ASTNodeType::FRAGMENT_STAGE ||
             srcFn.body.Type() == ASTNodeType::COMPUTE_STAGE)) {
            dstFn.body = CloneShaderStageWithParams(srcFn.body, subs, subCount);
        } else {
            dstFn.body = CloneNodeWithParams(srcFn.body, subs, subCount);
        }
        dstPass.functions.Push(arena, newFn);
    }

    auto cloneStage = [&](NodeRef stageRef) -> NodeRef {
        if (stageRef.IsNull()) return stageRef;
        const ShaderStageData& srcStage = ast->GetShaderStage(stageRef);
        if (srcStage.isDeferred) {
            NodeRef stage = ASTFactory::MakeShaderStage(ast, stageRef.Type(), NodeRef::Null(),
                                                        ast->GetLine(stageRef), ast->GetColumn(stageRef));
            ShaderStageData& dstStage = ast->GetShaderStage(stage);
            dstStage.isDeferred = true;
            dstStage.deferredExpr = CloneNodeWithParams(srcStage.deferredExpr, subs, subCount);
            dstStage.isInherited = srcStage.isInherited;
            dstStage.inheritsFrom = srcStage.inheritsFrom;
            dstStage.name = srcStage.name;
            dstStage.workgroupSizeX = srcStage.workgroupSizeX;
            dstStage.workgroupSizeY = srcStage.workgroupSizeY;
            dstStage.workgroupSizeZ = srcStage.workgroupSizeZ;
            return stage;
        }
        NodeRef stage = CloneShaderStageWithParams(stageRef, subs, subCount);
        ShaderStageData& dstStage = ast->GetShaderStage(stage);
        dstStage.isInherited = srcStage.isInherited;
        dstStage.inheritsFrom = srcStage.inheritsFrom;
        return stage;
    };

    dstPass.vertexShader = cloneStage(srcPass.vertexShader);
    dstPass.fragmentShader = cloneStage(srcPass.fragmentShader);
    dstPass.computeShader = cloneStage(srcPass.computeShader);

    return newPass;
}

NodeRef Parser::LookupPassBlockFunction(const FunctionCallData& call, NodeRef pipeline) {
    auto findInFunctions = [&](const ArenaArray<NodeRef>& functions) -> NodeRef {
        for (u32 i = 0; i < functions.count; i++) {
            const FunctionDeclData& func = ast->GetFunction(functions[i]);
            if (func.name.nameHash == call.name.nameHash &&
                func.returnType == CoreType::PASS_BLOCK &&
                func.parameters.count == call.arguments.count) {
                return functions[i];
            }
        }
        return NodeRef::Null();
    };

    auto moduleNodeForSymbolIndex = [&](u32 moduleIndex) -> NodeRef {
        if (moduleIndex == INVALID_INDEX || moduleIndex >= symbolTable.modules.count) {
            return NodeRef::Null();
        }

        u32 moduleHash = symbolTable.modules[moduleIndex].name.nameHash;
        for (u32 i = 0; i < ast->modules.count; i++) {
            if (ast->modules[i].name.nameHash == moduleHash) {
                return NodeRef(ASTNodeType::MODULE, i);
            }
        }
        return NodeRef::Null();
    };

    if (call.flags & FunctionCallFlags::IS_MODULE_FUNCTION) {
        NodeRef module = moduleNodeForSymbolIndex(call.moduleIndex);
        if (module.IsNull() && call.moduleObject.Type() == ASTNodeType::IDENTIFIER) {
            const IdentifierData& moduleIdent = ast->GetIdentifier(call.moduleObject);
            module = moduleNodeForSymbolIndex(ResolveModuleIndexByWrittenName(moduleIdent.name));
        }
        if (module.IsValid()) {
            return findInFunctions(ast->GetModule(module).functions);
        }
        return NodeRef::Null();
    }

    if (pipeline.IsValid()) {
        const PipelineData& pipelineData = ast->GetPipeline(pipeline);
        NodeRef local = findInFunctions(pipelineData.functions);
        if (local.IsValid()) return local;

        for (u32 i = 0; i < pipelineData.usingImports.count; i++) {
            u32 moduleIndex = ResolveModuleIndexByWrittenName(pipelineData.usingImports[i]);
            NodeRef module = moduleNodeForSymbolIndex(moduleIndex);
            if (module.IsNull()) continue;
            NodeRef imported = findInFunctions(ast->GetModule(module).functions);
            if (imported.IsValid()) return imported;
        }
    }

    for (u32 i = 0; i < ast->functions.count; i++) {
        NodeRef fnRef(ASTNodeType::FUNCTION, i);
        const FunctionDeclData& func = ast->GetFunction(fnRef);
        if (func.name.nameHash == call.name.nameHash &&
            func.returnType == CoreType::PASS_BLOCK &&
            func.parameters.count == call.arguments.count) {
            return fnRef;
        }
    }

    return NodeRef::Null();
}

NodeRef Parser::ExpandPassBlockInstance(NodeRef passRef, NodeRef pipeline) {
    if (passRef.IsNull() || pipeline.IsNull()) return NodeRef::Null();

    const PassData& instancePass = ast->GetPass(passRef);
    if (!instancePass.isPassBlockInstance || instancePass.passBlockCall.IsNull()) {
        return passRef;
    }
    if (instancePass.passBlockCall.Type() != ASTNodeType::FUNCTION_CALL) {
        Error("pass_block instantiation must use a direct function call");
        return NodeRef::Null();
    }

    const FunctionCallData& call = ast->GetFunctionCall(instancePass.passBlockCall);
    NodeRef funcRef = LookupPassBlockFunction(call, pipeline);
    if (funcRef.IsNull()) {
        Error("Cannot find pass_block function with matching argument count");
        return NodeRef::Null();
    }

    const FunctionDeclData& func = ast->GetFunction(funcRef);
    if (func.returnType != CoreType::PASS_BLOCK ||
        func.body.IsNull() ||
        func.body.Type() != ASTNodeType::PASS) {
        Error("pass_block function does not return a pass block");
        return NodeRef::Null();
    }

    if (func.parameters.count != call.arguments.count) {
        Error("pass_block function call argument count mismatch");
        return NodeRef::Null();
    }

    std::vector<ParamSubstitution> paramSubs;
    paramSubs.reserve(func.parameters.count);
    if (func.parameters.count > 0) {
        EvalStateSoA evalState;
        CompileTimeEvaluatorSoA::Init(&evalState, this, ast, &context->evalCache, ast->arena);
        for (u32 i = 0; i < func.parameters.count; i++) {
            LiteralValue argValue;
            if (!CompileTimeEvaluatorSoA::CanEvaluateNode(&evalState, call.arguments[i]) ||
                !CompileTimeEvaluatorSoA::EvaluateNode(&evalState, call.arguments[i], &argValue)) {
                Error("pass_block function arguments must be compile-time constants");
                return NodeRef::Null();
            }
            paramSubs.push_back({func.parameters[i].first.nameHash, argValue});
        }
    }

    NodeRef sourceModule = NodeRef::Null();
    for (u32 i = 0; i < ast->modules.count && sourceModule.IsNull(); i++) {
        const ModuleNodeData& moduleData = ast->modules[i];
        for (u32 j = 0; j < moduleData.functions.count; j++) {
            if (moduleData.functions[j] == funcRef) {
                sourceModule = NodeRef(ASTNodeType::MODULE, i);
                break;
            }
        }
    }

    const PassData& sourcePass = ast->GetPass(func.body);
    const PipelineData& pipelineData = ast->GetPipeline(pipeline);
    const ModuleNodeData* sourceModuleData = sourceModule.IsValid()
        ? &ast->GetModule(sourceModule)
        : nullptr;

    const ArenaArray<NodeRef>& sourceAttributes = sourceModuleData
        ? sourceModuleData->attributes
        : pipelineData.attributes;
    const ArenaArray<NodeRef>& sourceResources = sourceModuleData
        ? sourceModuleData->resources
        : pipelineData.resources;
    const ArenaArray<PipelineVariantDeclData>& sourceVariants = sourceModuleData
        ? sourceModuleData->variantDecls
        : pipelineData.variantDecls;

    auto findAttribute = [&](const ArenaArray<NodeRef>& attrs,
                             const ArenaString& name) -> const AttributeDeclData* {
        for (u32 i = 0; i < attrs.count; i++) {
            const AttributeDeclData& attr = ast->GetAttributeDecl(attrs[i]);
            if (attr.name.nameHash == name.nameHash) return &attr;
        }
        return nullptr;
    };

    auto findResource = [&](const ArenaArray<NodeRef>& resources,
                            const ArenaString& name) -> const ResourceDeclData* {
        for (u32 i = 0; i < resources.count; i++) {
            const ResourceDeclData& resource = ast->GetResourceDecl(resources[i]);
            if (resource.name.nameHash == name.nameHash) return &resource;
        }
        return nullptr;
    };

    auto findVariant = [&](const ArenaArray<PipelineVariantDeclData>& variants,
                           const ArenaString& name) -> const PipelineVariantDeclData* {
        for (u32 i = 0; i < variants.count; i++) {
            if (variants[i].name.nameHash == name.nameHash) return &variants[i];
        }
        return nullptr;
    };

    auto resolveVariantType = [&](const PipelineVariantDeclData& decl,
                                  TypeInfo* outType,
                                  u32* outEnumHash) {
        TypeInfo typeInfo = decl.typeInfo;
        u32 enumHash = decl.enumTypeHash;
        if (typeInfo.coreType == CoreType::INVALID && enumHash != 0) {
            const EnumData* enumData = SymbolTable::ResolveEnumDataByHash(&symbolTable, enumHash);
            if (enumData) {
                CoreType baseType = enumData->underlyingType;
                if (baseType == CoreType::INVALID) baseType = CoreType::INT;
                typeInfo = TYPE_INFO(baseType, 1, false);
            }
        }
        *outType = typeInfo;
        *outEnumHash = enumHash;
    };

    struct RemapStateGuard {
        Parser* parser;
        std::vector<PassBlockNameRemap> savedAttributes;
        std::vector<PassBlockNameRemap> savedResources;
        std::vector<PassBlockNameRemap> savedVariants;
        bool savedActive;
        bool savedBare;

        RemapStateGuard(Parser* p)
            : parser(p),
              savedAttributes(p->passBlockAttributeRemaps),
              savedResources(p->passBlockResourceRemaps),
              savedVariants(p->passBlockVariantRemaps),
              savedActive(p->passBlockRemapActive),
              savedBare(p->remapBareVariants) {
            parser->passBlockAttributeRemaps.clear();
            parser->passBlockResourceRemaps.clear();
            parser->passBlockVariantRemaps.clear();
            parser->passBlockRemapActive = false;
            parser->remapBareVariants = false;
        }

        ~RemapStateGuard() {
            parser->passBlockAttributeRemaps = savedAttributes;
            parser->passBlockResourceRemaps = savedResources;
            parser->passBlockVariantRemaps = savedVariants;
            parser->passBlockRemapActive = savedActive;
            parser->remapBareVariants = savedBare;
        }
    } remapGuard(this);

    auto hasRemap = [](const std::vector<PassBlockNameRemap>& remaps, u32 localHash) -> bool {
        for (const PassBlockNameRemap& remap : remaps) {
            if (remap.localHash == localHash) return true;
        }
        return false;
    };

    auto addRemap = [&](std::vector<PassBlockNameRemap>& remaps,
                        const ArenaString& localName,
                        const ArenaString& targetName,
                        u8 localIndex,
                        u8 targetIndex,
                        const char* duplicateMessage) -> bool {
        if (hasRemap(remaps, localName.nameHash)) {
            Error(duplicateMessage);
            return false;
        }
        PassBlockNameRemap remap{};
        remap.localHash = localName.nameHash;
        remap.targetName = targetName;
        remap.localIndex = localIndex;
        remap.targetIndex = targetIndex;
        remaps.push_back(remap);
        return true;
    };

    auto addImplicitVariantRemap = [&](const std::string& prefix,
                                       const ArenaString& localName,
                                       const ArenaString& targetName,
                                       u8 localIndex,
                                       u8 targetIndex) {
        std::string local = prefix + ArenaStringToStdString(localName, sourceBase());
        std::string target = prefix + ArenaStringToStdString(targetName, sourceBase());
        if (hasRemap(passBlockVariantRemaps, Utils::HashStr(local.c_str()))) {
            return;
        }
        PassBlockNameRemap remap{};
        remap.localHash = Utils::HashStr(local.c_str());
        remap.targetName = MakeRegisteredArenaString(target);
        remap.localIndex = localIndex;
        remap.targetIndex = targetIndex;
        passBlockVariantRemaps.push_back(remap);
    };

    bool mappingsOk = true;

    if (sourceModule.IsValid() &&
        sourcePass.usedAttributes.count > 0 &&
        instancePass.attributeBindings.count == 0) {
        Error("pass_block attribute mappings are required for module pass attributes");
        return NodeRef::Null();
    }

    for (u32 i = 0; i < instancePass.attributeBindings.count; i++) {
        const PassBlockBindingData& binding = instancePass.attributeBindings[i];
        const AttributeDeclData* local = findAttribute(sourceAttributes, binding.localName);
        const AttributeDeclData* target = findAttribute(pipelineData.attributes, binding.targetName);
        if (!local) {
            Error("Unknown pass_block attribute mapping source");
            mappingsOk = false;
            continue;
        }
        if (!target) {
            Error("Unknown pass_block attribute mapping target");
            mappingsOk = false;
            continue;
        }
        if (local->dataType.nameHash != target->dataType.nameHash ||
            local->compression.nameHash != target->compression.nameHash ||
            local->isInstance != target->isInstance) {
            Error("pass_block attribute mapping type mismatch");
            mappingsOk = false;
            continue;
        }
        if (addRemap(passBlockAttributeRemaps, binding.localName, binding.targetName,
                     local->attributeIndex, target->attributeIndex,
                     "Duplicate pass_block attribute mapping")) {
            addImplicitVariantRemap("has_", binding.localName, binding.targetName,
                                    local->attributeIndex, target->attributeIndex);
        } else {
            mappingsOk = false;
        }
    }

    for (u32 i = 0; i < sourcePass.usedAttributes.count; i++) {
        const ArenaString& localName = sourcePass.usedAttributes[i];
        if (hasRemap(passBlockAttributeRemaps, localName.nameHash)) continue;
        if (sourceModule.IsValid()) {
            Error("Missing pass_block attribute mapping");
            mappingsOk = false;
            continue;
        }
        const AttributeDeclData* attr = findAttribute(sourceAttributes, localName);
        if (!attr) {
            Error("Unknown pass_block attribute");
            mappingsOk = false;
            continue;
        }
        if (addRemap(passBlockAttributeRemaps, localName, localName,
                     attr->attributeIndex, attr->attributeIndex,
                     "Duplicate pass_block attribute mapping")) {
            addImplicitVariantRemap("has_", localName, localName,
                                    attr->attributeIndex, attr->attributeIndex);
        } else {
            mappingsOk = false;
        }
    }

    if (sourceModule.IsValid() &&
        sourcePass.usedResources.count > 0 &&
        instancePass.resourceBindings.count == 0) {
        Error("pass_block resource mappings are required for module pass resources");
        return NodeRef::Null();
    }

    for (u32 i = 0; i < instancePass.resourceBindings.count; i++) {
        const PassBlockBindingData& binding = instancePass.resourceBindings[i];
        const ResourceDeclData* local = findResource(sourceResources, binding.localName);
        const ResourceDeclData* target = findResource(pipelineData.resources, binding.targetName);
        if (!local) {
            Error("Unknown pass_block resource mapping source");
            mappingsOk = false;
            continue;
        }
        if (!target) {
            Error("Unknown pass_block resource mapping target");
            mappingsOk = false;
            continue;
        }
        if (local->typeName.nameHash != target->typeName.nameHash) {
            Error("pass_block resource mapping type mismatch");
            mappingsOk = false;
            continue;
        }
        if (addRemap(passBlockResourceRemaps, binding.localName, binding.targetName,
                     local->resourceIndex, target->resourceIndex,
                     "Duplicate pass_block resource mapping")) {
            addImplicitVariantRemap("has_resource_", binding.localName, binding.targetName,
                                    local->resourceIndex, target->resourceIndex);
        } else {
            mappingsOk = false;
        }
    }

    for (u32 i = 0; i < sourcePass.usedResources.count; i++) {
        const ArenaString& localName = sourcePass.usedResources[i];
        if (hasRemap(passBlockResourceRemaps, localName.nameHash)) continue;
        if (sourceModule.IsValid()) {
            Error("Missing pass_block resource mapping");
            mappingsOk = false;
            continue;
        }
        const ResourceDeclData* resource = findResource(sourceResources, localName);
        if (!resource) {
            Error("Unknown pass_block resource");
            mappingsOk = false;
            continue;
        }
        if (addRemap(passBlockResourceRemaps, localName, localName,
                     resource->resourceIndex, resource->resourceIndex,
                     "Duplicate pass_block resource mapping")) {
            addImplicitVariantRemap("has_resource_", localName, localName,
                                    resource->resourceIndex, resource->resourceIndex);
        } else {
            mappingsOk = false;
        }
    }

    for (u32 i = 0; i < instancePass.variantBindings.count; i++) {
        const PassBlockBindingData& binding = instancePass.variantBindings[i];
        const PipelineVariantDeclData* local = findVariant(sourceVariants, binding.localName);
        const PipelineVariantDeclData* target = findVariant(pipelineData.variantDecls, binding.targetName);
        if (!local) {
            Error("Unknown pass_block variant mapping source");
            mappingsOk = false;
            continue;
        }
        if (!target) {
            Error("pass_block variant mapping target must be a declared pipeline variant");
            mappingsOk = false;
            continue;
        }

        TypeInfo localType;
        TypeInfo targetType;
        u32 localEnumHash = 0;
        u32 targetEnumHash = 0;
        resolveVariantType(*local, &localType, &localEnumHash);
        resolveVariantType(*target, &targetType, &targetEnumHash);
        if (!SameTypeInfoForVariantBinding(localType, targetType, localEnumHash, targetEnumHash)) {
            Error("pass_block variant mapping type mismatch");
            mappingsOk = false;
            continue;
        }

        if (!addRemap(passBlockVariantRemaps, binding.localName, binding.targetName,
                      0xFF, 0xFF, "Duplicate pass_block variant mapping")) {
            mappingsOk = false;
        }
    }

    if (!sourceModule.IsValid()) {
        for (u32 i = 0; i < sourceVariants.count; i++) {
            const PipelineVariantDeclData& variant = sourceVariants[i];
            if (!hasRemap(passBlockVariantRemaps, variant.name.nameHash)) {
                if (!addRemap(passBlockVariantRemaps, variant.name, variant.name,
                              0xFF, 0xFF, "Duplicate pass_block variant mapping")) {
                    mappingsOk = false;
                }
            }
        }
    }

    if (!mappingsOk) {
        return NodeRef::Null();
    }

    std::string passName = ArenaStringToStdString(instancePass.name, sourceBase());
    passBlockRemapActive = true;
    remapBareVariants = false;
    NodeRef clonedPass = ClonePassWithParamsAndRemap(
        func.body,
        paramSubs.empty() ? nullptr : paramSubs.data(),
        static_cast<u32>(paramSubs.size()),
        passName);

    if (sourceModuleData) {
        bool savedBare = remapBareVariants;
        remapBareVariants = true;
        PipelineData& mutablePipeline = ast->GetPipeline(pipeline);
        for (u32 i = 0; i < sourceModuleData->variantRules.count; i++) {
            const VariantRuleData& srcRule = sourceModuleData->variantRules[i];
            VariantRuleData dstRule{};
            dstRule.type = srcRule.type;
            dstRule.lhs = CloneNodeWithParams(srcRule.lhs, nullptr, 0);
            dstRule.rhs = CloneNodeWithParams(srcRule.rhs, nullptr, 0);
            mutablePipeline.variantRules.Push(arena, dstRule);
        }
        remapBareVariants = savedBare;
    }

    return clonedPass;
}

void Parser::ResolvePassBlockInstances(NodeRef pipeline) {
    if (pipeline.IsNull()) return;

    NodeRef savedPipeline = currentPipeline;
    currentPipeline = pipeline;

    PipelineData& pipelineData = ast->GetPipeline(pipeline);
    for (u32 i = 0; i < pipelineData.passes.count; i++) {
        NodeRef passRef = pipelineData.passes[i];
        if (passRef.IsNull()) continue;
        const PassData& pass = ast->GetPass(passRef);
        if (!pass.isPassBlockInstance) continue;

        NodeRef expanded = ExpandPassBlockInstance(passRef, pipeline);
        if (expanded.IsValid()) {
            pipelineData.passes[i] = expanded;
        }
    }

    currentPipeline = savedPipeline;
}

NodeRef Parser::ClonePassWithActiveVariants(NodeRef passRef) {
    const PassData& srcPass = ast->GetPass(passRef);
    std::string passName = srcPass.name.isHashOnly()
        ? ReverseLookup::GetString(srcPass.name.nameHash)
        : srcPass.name.ToString(sourceBase());
    NodeRef newPass = ASTFactory::MakePass(ast, passName, ast->GetLine(passRef), ast->GetColumn(passRef));
    PassData& dstPass = ast->GetPass(newPass);

    for (u32 i = 0; i < srcPass.usedAttributes.count; i++) {
        dstPass.usedAttributes.Push(arena, srcPass.usedAttributes[i]);
    }
    for (u32 i = 0; i < srcPass.usedResources.count; i++) {
        dstPass.usedResources.Push(arena, srcPass.usedResources[i]);
    }
    dstPass.hasFragmentOutputs = srcPass.hasFragmentOutputs;
    for (u32 i = 0; i < srcPass.fragmentOutputs.count; i++) {
        dstPass.fragmentOutputs.Push(arena, srcPass.fragmentOutputs[i]);
    }
    dstPass.optionalAttributesMask = srcPass.optionalAttributesMask;
    dstPass.optionalResourcesMask = srcPass.optionalResourcesMask;

    for (u32 i = 0; i < srcPass.consts.count; i++) {
        dstPass.consts.Push(arena, CloneNodeWithParams(srcPass.consts[i], nullptr, 0));
    }

    for (u32 i = 0; i < srcPass.functions.count; i++) {
        NodeRef fnRef = srcPass.functions[i];
        const FunctionDeclData& srcFn = ast->GetFunction(fnRef);
        std::string fnName = srcFn.name.isHashOnly()
            ? ReverseLookup::GetString(srcFn.name.nameHash)
            : srcFn.name.ToString(sourceBase());
        NodeRef newFn = ASTFactory::MakeFunction(ast, fnName, srcFn.returnType,
                                                ast->GetLine(fnRef), ast->GetColumn(fnRef));
        FunctionDeclData& dstFn = ast->GetFunction(newFn);
        for (u32 j = 0; j < srcFn.parameters.count; j++) {
            dstFn.parameters.Push(arena, srcFn.parameters[j]);
        }
        dstFn.isEval = srcFn.isEval;
        dstFn.body = CloneNodeWithParams(srcFn.body, nullptr, 0);
        dstPass.functions.Push(arena, newFn);
    }

    auto cloneStage = [&](NodeRef stageRef) -> NodeRef {
        if (stageRef.IsNull()) return stageRef;
        const ShaderStageData& srcStage = ast->GetShaderStage(stageRef);
        if (srcStage.isDeferred) {
            NodeRef stage = ASTFactory::MakeShaderStage(ast, stageRef.Type(), NodeRef::Null(),
                                                        ast->GetLine(stageRef), ast->GetColumn(stageRef));
            ShaderStageData& dstStage = ast->GetShaderStage(stage);
            dstStage.isDeferred = true;
            dstStage.deferredExpr = CloneNodeWithParams(srcStage.deferredExpr, nullptr, 0);
            dstStage.isInherited = srcStage.isInherited;
            dstStage.inheritsFrom = srcStage.inheritsFrom;
            dstStage.name = srcStage.name;
            dstStage.workgroupSizeX = srcStage.workgroupSizeX;
            dstStage.workgroupSizeY = srcStage.workgroupSizeY;
            dstStage.workgroupSizeZ = srcStage.workgroupSizeZ;
            return stage;
        }
        NodeRef stage = CloneShaderStageWithParams(stageRef, nullptr, 0);
        ShaderStageData& dstStage = ast->GetShaderStage(stage);
        dstStage.isInherited = srcStage.isInherited;
        dstStage.inheritsFrom = srcStage.inheritsFrom;
        return stage;
    };

    dstPass.vertexShader = cloneStage(srcPass.vertexShader);
    dstPass.fragmentShader = cloneStage(srcPass.fragmentShader);
    dstPass.computeShader = cloneStage(srcPass.computeShader);

    return newPass;
}

NodeRef Parser::SpecializePipelineForVariants(NodeRef pipeline,
                                              const VariantSelectionData& selection,
                                              std::string* outError) {
    if (pipeline.IsNull()) return NodeRef::Null();

    ResolvePassBlockInstances(pipeline);

    const PipelineData& srcPipeline = ast->GetPipeline(pipeline);
    if (srcPipeline.variantDecls.count == 0 && srcPipeline.variantRules.count == 0) {
        (void)selection;
        ResolveShaderStageExpressions(pipeline);
        return pipeline;
    }

    std::string pipelineName = srcPipeline.name.isHashOnly()
        ? ReverseLookup::GetString(srcPipeline.name.nameHash)
        : srcPipeline.name.ToString(sourceBase());

    SetActiveVariantSelection(selection, false);

    NodeRef newPipeline = ASTFactory::MakePipeline(ast, pipelineName, ast->GetLine(pipeline), ast->GetColumn(pipeline));
    PipelineData& dstPipeline = ast->GetPipeline(newPipeline);
    for (u32 i = 0; i < srcPipeline.imports.count; i++) {
        dstPipeline.imports.Push(arena, srcPipeline.imports[i]);
    }
    for (u32 i = 0; i < srcPipeline.usingImports.count; i++) {
        dstPipeline.usingImports.Push(arena, srcPipeline.usingImports[i]);
    }
    for (u32 i = 0; i < srcPipeline.attributes.count; i++) {
        dstPipeline.attributes.Push(arena, srcPipeline.attributes[i]);
    }
    for (u32 i = 0; i < srcPipeline.resources.count; i++) {
        dstPipeline.resources.Push(arena, srcPipeline.resources[i]);
    }
    for (u32 i = 0; i < srcPipeline.variantDecls.count; i++) {
        dstPipeline.variantDecls.Push(arena, srcPipeline.variantDecls[i]);
    }
    for (u32 i = 0; i < srcPipeline.variantRules.count; i++) {
        dstPipeline.variantRules.Push(arena, srcPipeline.variantRules[i]);
    }
    for (u32 i = 0; i < srcPipeline.enums.count; i++) {
        dstPipeline.enums.Push(arena, srcPipeline.enums[i]);
    }
    for (u32 i = 0; i < srcPipeline.constraints.count; i++) {
        dstPipeline.constraints.Push(arena, srcPipeline.constraints[i]);
    }
    dstPipeline.computeGraph = srcPipeline.computeGraph;

    for (u32 i = 0; i < srcPipeline.functions.count; i++) {
        NodeRef fnRef = srcPipeline.functions[i];
        const FunctionDeclData& srcFn = ast->GetFunction(fnRef);
        std::string fnName = srcFn.name.isHashOnly()
            ? ReverseLookup::GetString(srcFn.name.nameHash)
            : srcFn.name.ToString(sourceBase());
        NodeRef newFn = ASTFactory::MakeFunction(ast, fnName, srcFn.returnType,
                                                ast->GetLine(fnRef), ast->GetColumn(fnRef));
        FunctionDeclData& dstFn = ast->GetFunction(newFn);
        for (u32 j = 0; j < srcFn.parameters.count; j++) {
            dstFn.parameters.Push(arena, srcFn.parameters[j]);
        }
        dstFn.isEval = srcFn.isEval;
        dstFn.body = CloneNodeWithParams(srcFn.body, nullptr, 0);
        dstPipeline.functions.Push(arena, newFn);
    }

    for (u32 i = 0; i < srcPipeline.passes.count; i++) {
        dstPipeline.passes.Push(arena, ClonePassWithActiveVariants(srcPipeline.passes[i]));
    }

    NodeRef savedPipeline = currentPipeline;
    currentPipeline = newPipeline;
    ResolveShaderStageExpressions(newPipeline);
    currentPipeline = savedPipeline;
    ClearActiveVariantSelection();

    for (u32 i = 0; i < dstPipeline.passes.count; i++) {
        const PassData& pass = ast->GetPass(dstPipeline.passes[i]);
        auto unresolved = [&](NodeRef stageRef) -> bool {
            return stageRef.IsValid() && ast->GetShaderStage(stageRef).isDeferred;
        };
        if (unresolved(pass.vertexShader) || unresolved(pass.fragmentShader) || unresolved(pass.computeShader)) {
            if (outError) {
                *outError = "Shader stage selection did not resolve for the requested variant selection";
            }
            return NodeRef::Null();
        }
    }

    return newPipeline;
}

void Parser::ResolveShaderStageExpressions(NodeRef pipeline) {
    if (!pipeline.IsValid()) return;

    ResolvePassBlockInstances(pipeline);

    NodeRef savedPipeline = currentPipeline;
    currentPipeline = pipeline;

    const PipelineData& pipelineData = ast->GetPipeline(pipeline);

    // Iterate through all passes
    for (u32 i = 0; i < pipelineData.passes.count; i++) {
        NodeRef passRef = pipelineData.passes[i];
        PassData& pass = ast->GetPass(passRef);

        // Resolve vertex shader if deferred
        if (!pass.vertexShader.IsNull()) {
            const ShaderStageData& vertexStage = ast->GetShaderStage(pass.vertexShader);
            if (vertexStage.isDeferred) {
                NodeRef resolved = ResolveShaderStageExpr(pass.vertexShader, pass, ASTNodeType::VERTEX_STAGE);
                if (resolved.IsValid()) {
                    pass.vertexShader = resolved;
                }
            }
        }

        // Resolve fragment shader if deferred
        if (!pass.fragmentShader.IsNull()) {
            const ShaderStageData& fragmentStage = ast->GetShaderStage(pass.fragmentShader);
            if (fragmentStage.isDeferred) {
                NodeRef resolved = ResolveShaderStageExpr(pass.fragmentShader, pass, ASTNodeType::FRAGMENT_STAGE);
                if (resolved.IsValid()) {
                    pass.fragmentShader = resolved;
                }
            }
        }

        // Resolve compute shader if deferred
        if (!pass.computeShader.IsNull()) {
            const ShaderStageData& computeStage = ast->GetShaderStage(pass.computeShader);
            if (computeStage.isDeferred) {
                NodeRef resolved = ResolveShaderStageExpr(pass.computeShader, pass, ASTNodeType::COMPUTE_STAGE);
                if (resolved.IsValid()) {
                    pass.computeShader = resolved;
                }
            }
        }
    }

    currentPipeline = savedPipeline;
}


} // namespace BWSL
