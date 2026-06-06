// Part of bwsl_parser_soa.cpp. Include from that file only.
// Compile-time eval bindings, variant resolution, eval expansion, and control-flow statements.


namespace BWSL {

void Parser::PushEvalBindingScope() {
    evalBindingScopeStarts.push_back(static_cast<u32>(evalBindings.size()));
}

void Parser::PopEvalBindingScope() {
    if (evalBindingScopeStarts.empty()) return;
    evalBindings.resize(evalBindingScopeStarts.back());
    evalBindingScopeStarts.pop_back();
}

void Parser::AddEvalBinding(u32 nameHash, const LiteralValue& value) {
    EvalBinding binding{};
    binding.nameHash = nameHash;
    binding.isShadow = false;
    binding.value = value;
    evalBindings.push_back(binding);
}

void Parser::AddEvalShadow(u32 nameHash) {
    EvalBinding binding{};
    binding.nameHash = nameHash;
    binding.isShadow = true;
    evalBindings.push_back(binding);
}

bool Parser::LookupEvalBinding(u32 nameHash, LiteralValue* outValue) const {
    for (size_t i = evalBindings.size(); i > 0; --i) {
        const EvalBinding& binding = evalBindings[i - 1];
        if (binding.nameHash != nameHash) continue;
        if (binding.isShadow) return false;
        if (outValue) *outValue = binding.value;
        return true;
    }
    return false;
}

void Parser::UpdateEvalBinding(u32 nameHash, const LiteralValue& value) {
    for (size_t i = evalBindings.size(); i > 0; --i) {
        EvalBinding& binding = evalBindings[i - 1];
        if (binding.nameHash != nameHash) continue;
        if (!binding.isShadow) {
            binding.value = value;
            return;
        }
        break;
    }
    AddEvalBinding(nameHash, value);
}

void Parser::BuildVisibleEvalSubstitutions(std::vector<ParamSubstitution>& outSubs) const {
    outSubs.clear();
    outSubs.reserve(evalBindings.size());

    std::vector<u32> seen;
    seen.reserve(evalBindings.size());

    for (size_t i = evalBindings.size(); i > 0; --i) {
        const EvalBinding& binding = evalBindings[i - 1];
        bool alreadySeen = false;
        for (u32 hash : seen) {
            if (hash == binding.nameHash) {
                alreadySeen = true;
                break;
            }
        }
        if (alreadySeen) continue;

        seen.push_back(binding.nameHash);
        if (!binding.isShadow) {
            outSubs.push_back({binding.nameHash, binding.value});
        }
    }
}

bool Parser::IsOptionalAttributeFeature(NodeRef pipeline, u8 attributeIndex) const {
    if (pipeline.IsNull() || attributeIndex >= 32) return false;
    const u32 bit = (1u << attributeIndex);

    if (currentPass.IsValid() && currentPipeline == pipeline) {
        if (ast->GetPass(currentPass).optionalAttributesMask & bit) {
            return true;
        }
    }

    const PipelineData& pipelineData = ast->GetPipeline(pipeline);
    for (u32 i = 0; i < pipelineData.passes.count; i++) {
        if (ast->GetPass(pipelineData.passes[i]).optionalAttributesMask & bit) {
            return true;
        }
    }
    return false;
}

bool Parser::IsOptionalResourceFeature(NodeRef pipeline, u8 resourceIndex) const {
    if (pipeline.IsNull() || resourceIndex >= 32) return false;
    const u32 bit = (1u << resourceIndex);

    if (currentPass.IsValid() && currentPipeline == pipeline) {
        if (ast->GetPass(currentPass).optionalResourcesMask & bit) {
            return true;
        }
    }

    const PipelineData& pipelineData = ast->GetPipeline(pipeline);
    for (u32 i = 0; i < pipelineData.passes.count; i++) {
        if (ast->GetPass(pipelineData.passes[i]).optionalResourcesMask & bit) {
            return true;
        }
    }
    return false;
}

bool Parser::LookupVariantType(NodeRef pipeline, u32 nameHash, TypeInfo* outType,
                               u32* outEnumTypeHash,
                               bool* outImplicit,
                               u8* outAttributeIndex,
                               u8* outResourceIndex) const {
    if (outType) *outType = TYPE_INFO(CoreType::INVALID, 0, false);
    if (outEnumTypeHash) *outEnumTypeHash = 0;
    if (outImplicit) *outImplicit = false;
    if (outAttributeIndex) *outAttributeIndex = 0xFF;
    if (outResourceIndex) *outResourceIndex = 0xFF;

    if (pipeline.IsNull()) return false;
    const PipelineData& pipelineData = ast->GetPipeline(pipeline);

    for (u32 i = 0; i < pipelineData.variantDecls.count; i++) {
        const PipelineVariantDeclData& decl = pipelineData.variantDecls[i];
        if (decl.name.nameHash != nameHash) continue;
        if (outType) *outType = decl.typeInfo;
        if (outEnumTypeHash) *outEnumTypeHash = decl.enumTypeHash;
        return true;
    }

    static const u32 HAS_PREFIX = Utils::HashStr("has_");
    (void)HAS_PREFIX;
    const PipelineData& pipelineRef = ast->GetPipeline(pipeline);
    for (u32 i = 0; i < pipelineRef.attributes.count; i++) {
        const AttributeDeclData& attr = ast->GetAttributeDecl(pipelineRef.attributes[i]);
        if (!IsOptionalAttributeFeature(pipeline, attr.attributeIndex)) continue;
        std::string implicitName = std::string("has_") + attr.name.ToString(sourceBase());
        if (Utils::HashStr(implicitName.c_str()) != nameHash) continue;
        if (outType) *outType = TYPE_INFO(CoreType::BOOL, 1, false);
        if (outImplicit) *outImplicit = true;
        if (outAttributeIndex) *outAttributeIndex = attr.attributeIndex;
        return true;
    }

    for (u32 i = 0; i < pipelineRef.resources.count; i++) {
        const ResourceDeclData& resourceDecl = ast->GetResourceDecl(pipelineRef.resources[i]);
        if (!IsOptionalResourceFeature(pipeline, resourceDecl.resourceIndex)) continue;
        std::string implicitName = std::string("has_resource_") + resourceDecl.name.ToString(sourceBase());
        if (Utils::HashStr(implicitName.c_str()) != nameHash) continue;
        if (outType) *outType = TYPE_INFO(CoreType::BOOL, 1, false);
        if (outImplicit) *outImplicit = true;
        if (outResourceIndex) *outResourceIndex = resourceDecl.resourceIndex;
        return true;
    }

    return false;
}

bool Parser::LookupModuleVariantType(NodeRef module, u32 nameHash, TypeInfo* outType,
                                     u32* outEnumTypeHash,
                                     bool* outImplicit,
                                     u8* outAttributeIndex,
                                     u8* outResourceIndex) const {
    if (outType) *outType = TYPE_INFO(CoreType::INVALID, 0, false);
    if (outEnumTypeHash) *outEnumTypeHash = 0;
    if (outImplicit) *outImplicit = false;
    if (outAttributeIndex) *outAttributeIndex = 0xFF;
    if (outResourceIndex) *outResourceIndex = 0xFF;

    if (module.IsNull() || module.Type() != ASTNodeType::MODULE) return false;
    const ModuleNodeData& moduleData = ast->GetModule(module);

    for (u32 i = 0; i < moduleData.variantDecls.count; i++) {
        const PipelineVariantDeclData& decl = moduleData.variantDecls[i];
        if (decl.name.nameHash != nameHash) continue;
        TypeInfo typeInfo = decl.typeInfo;
        if (typeInfo.coreType == CoreType::INVALID && decl.enumTypeHash != 0) {
            const EnumData* enumData = SymbolTable::ResolveEnumDataByHash(&symbolTable, decl.enumTypeHash);
            if (enumData) {
                CoreType baseType = enumData->underlyingType;
                if (baseType == CoreType::INVALID) baseType = CoreType::INT;
                typeInfo = TYPE_INFO(baseType, 1, false);
            }
        }
        if (outType) *outType = typeInfo;
        if (outEnumTypeHash) *outEnumTypeHash = decl.enumTypeHash;
        return true;
    }

    for (u32 i = 0; i < moduleData.attributes.count; i++) {
        const AttributeDeclData& attr = ast->GetAttributeDecl(moduleData.attributes[i]);
        std::string implicitName = std::string("has_") + attr.name.ToString(sourceBase());
        if (Utils::HashStr(implicitName.c_str()) != nameHash) continue;
        if (outType) *outType = TYPE_INFO(CoreType::BOOL, 1, false);
        if (outImplicit) *outImplicit = true;
        if (outAttributeIndex) *outAttributeIndex = attr.attributeIndex;
        return true;
    }

    for (u32 i = 0; i < moduleData.resources.count; i++) {
        const ResourceDeclData& resourceDecl = ast->GetResourceDecl(moduleData.resources[i]);
        std::string implicitName = std::string("has_resource_") +
            resourceDecl.name.ToString(sourceBase());
        if (Utils::HashStr(implicitName.c_str()) != nameHash) continue;
        if (outType) *outType = TYPE_INFO(CoreType::BOOL, 1, false);
        if (outImplicit) *outImplicit = true;
        if (outResourceIndex) *outResourceIndex = resourceDecl.resourceIndex;
        return true;
    }

    return false;
}

bool Parser::LookupActiveVariantBinding(u32 nameHash, LiteralValue* outValue,
                                        TypeInfo* outType,
                                        u32* outEnumTypeHash,
                                        bool* outImplicit,
                                        u8* outAttributeIndex,
                                        u8* outResourceIndex) const {
    for (size_t i = activeVariantBindings.size(); i > 0; --i) {
        const ActiveVariantBinding& binding = activeVariantBindings[i - 1];
        if (binding.nameHash != nameHash) continue;
        if (outValue) *outValue = binding.value;
        if (outType) *outType = binding.typeInfo;
        if (outEnumTypeHash) *outEnumTypeHash = binding.enumTypeHash;
        if (outImplicit) *outImplicit = binding.isImplicit;
        if (outAttributeIndex) *outAttributeIndex = binding.attributeIndex;
        if (outResourceIndex) *outResourceIndex = binding.resourceIndex;
        return true;
    }
    return false;
}

void Parser::SetActiveVariantSelection(const VariantSelectionData& selection, bool allowBareLookup) {
    activeVariantBindings.clear();
    activeVariantBindings.reserve(selection.values.size());
    for (const auto& value : selection.values) {
        ActiveVariantBinding binding{};
        binding.nameHash = value.name.nameHash;
        binding.typeInfo = value.typeInfo;
        binding.enumTypeHash = value.enumTypeHash;
        binding.value = value.value;
        binding.isImplicit = value.isImplicit;
        binding.implicitKind = value.implicitKind;
        binding.attributeIndex = value.attributeIndex;
        binding.resourceIndex = value.resourceIndex;
        activeVariantBindings.push_back(binding);
    }
    this->allowBareVariantLookup = allowBareLookup;
}

void Parser::ClearActiveVariantSelection() {
    activeVariantBindings.clear();
    allowBareVariantLookup = false;
}

bool Parser::ResolvePipelineVariants(NodeRef pipeline, std::string* outError) {
    if (pipeline.IsNull()) return true;

    ResolvePassBlockInstances(pipeline);

    auto fail = [&](const std::string& msg) -> bool {
        if (outError) *outError = msg;
        Error(msg);
        return false;
    };

    const PipelineData& pipelineData = ast->GetPipeline(pipeline);

    activeVariantBindings.clear();
    allowBareVariantLookup = true;

    for (u32 i = 0; i < pipelineData.variantDecls.count; i++) {
        PipelineVariantDeclData& decl = ast->GetPipeline(pipeline).variantDecls[i];

        if (decl.enumTypeHash != 0) {
            const EnumData* enumData = SymbolTable::ResolveEnumDataByHash(&symbolTable, decl.enumTypeHash);
            if (!enumData) {
                ClearActiveVariantSelection();
                return fail("Variant type must be 'bool' or an enum type");
            }
            if (enumData->flags & EnumData::IS_SUM_TYPE) {
                ClearActiveVariantSelection();
                return fail("Sum-type enums are not supported as variant types");
            }
            CoreType baseType = enumData->underlyingType;
            if (baseType == CoreType::INVALID) baseType = CoreType::INT;
            decl.typeInfo = TYPE_INFO(baseType, 1, false);
        }

        LiteralValue value;
        if (!EvaluateNodeWithEvalBindings(decl.defaultExpr, &value)) {
            std::string msg = "Variant default must be a compile-time constant";
            ClearActiveVariantSelection();
            return fail(msg);
        }

        if (!CoerceLiteralToType(decl.typeInfo, &value)) {
            ClearActiveVariantSelection();
            return fail("Variant default does not match declared type");
        }

        decl.defaultValue = value;
        decl.defaultResolved = true;

        ActiveVariantBinding binding{};
        binding.nameHash = decl.name.nameHash;
        binding.typeInfo = decl.typeInfo;
        binding.enumTypeHash = decl.enumTypeHash;
        binding.value = value;
        binding.isImplicit = false;
        binding.implicitKind = ImplicitVariantKind::None;
        binding.attributeIndex = 0xFF;
        binding.resourceIndex = 0xFF;
        activeVariantBindings.push_back(binding);
    }

    for (u32 i = 0; i < pipelineData.attributes.count; i++) {
        const AttributeDeclData& attr = ast->GetAttributeDecl(pipelineData.attributes[i]);
        if (!IsOptionalAttributeFeature(pipeline, attr.attributeIndex)) continue;

        ActiveVariantBinding binding{};
        std::string implicitName = std::string("has_") + attr.name.ToString(sourceBase());
        u32 implicitHash = Utils::HashStr(implicitName.c_str());
        ReverseLookup::Register(implicitHash, implicitName.c_str());
        binding.nameHash = implicitHash;
        binding.typeInfo = TYPE_INFO(CoreType::BOOL, 1, false);
        binding.enumTypeHash = 0;
        binding.value.type = LiteralValue::BOOL;
        binding.value.boolValue = false;
        binding.isImplicit = true;
        binding.implicitKind = ImplicitVariantKind::Attribute;
        binding.attributeIndex = attr.attributeIndex;
        binding.resourceIndex = 0xFF;
        activeVariantBindings.push_back(binding);
    }

    for (u32 i = 0; i < pipelineData.resources.count; i++) {
        const ResourceDeclData& resourceDecl = ast->GetResourceDecl(pipelineData.resources[i]);
        if (!IsOptionalResourceFeature(pipeline, resourceDecl.resourceIndex)) continue;

        ActiveVariantBinding binding{};
        std::string implicitName = std::string("has_resource_") + resourceDecl.name.ToString(sourceBase());
        u32 implicitHash = Utils::HashStr(implicitName.c_str());
        ReverseLookup::Register(implicitHash, implicitName.c_str());
        binding.nameHash = implicitHash;
        binding.typeInfo = TYPE_INFO(CoreType::BOOL, 1, false);
        binding.enumTypeHash = 0;
        binding.value.type = LiteralValue::BOOL;
        binding.value.boolValue = false;
        binding.isImplicit = true;
        binding.implicitKind = ImplicitVariantKind::Resource;
        binding.attributeIndex = 0xFF;
        binding.resourceIndex = resourceDecl.resourceIndex;
        activeVariantBindings.push_back(binding);
    }

    for (u32 i = 0; i < pipelineData.variantRules.count; i++) {
        const VariantRuleData& rule = pipelineData.variantRules[i];
        LiteralValue lhsValue;
        LiteralValue rhsValue;
        if (!EvaluateNodeWithEvalBindings(rule.lhs, &lhsValue)) {
            ClearActiveVariantSelection();
            return fail("Variant rule left-hand side must be a compile-time boolean expression");
        }
        if (!EvaluateNodeWithEvalBindings(rule.rhs, &rhsValue)) {
            ClearActiveVariantSelection();
            return fail("Variant rule right-hand side must be a compile-time boolean expression");
        }
        bool lhsBool = false;
        bool rhsBool = false;
        if (!ConvertLiteralToBool(lhsValue, &lhsBool) || !ConvertLiteralToBool(rhsValue, &rhsBool)) {
            ClearActiveVariantSelection();
            return fail("Variant rules must evaluate to booleans");
        }
    }

    ClearActiveVariantSelection();
    return true;
}

std::string Parser::FormatVariantExpression(NodeRef expr) const {
    if (expr.IsNull()) return "<null>";

    switch (expr.Type()) {
        case ASTNodeType::IDENTIFIER: {
            const IdentifierData& ident = ast->GetIdentifier(expr);
            return ident.name.isHashOnly() ? ReverseLookup::GetString(ident.name.nameHash)
                                           : ident.name.ToString(sourceBase());
        }
        case ASTNodeType::LITERAL:
            return SymbolTable::FormatLiteralValue(ast->GetLiteral(expr).value, &symbolTable, sourceBase());
        case ASTNodeType::MEMBER_ACCESS: {
            const MemberAccessData& access = ast->GetMemberAccess(expr);
            std::string base = FormatVariantExpression(access.object);
            std::string member = access.member.isHashOnly()
                ? ReverseLookup::GetString(access.member.nameHash)
                : access.member.ToString(sourceBase());
            return access.isModuleQualified ? (base + "::" + member) : (base + "." + member);
        }
        case ASTNodeType::UNARY_OP: {
            const UnaryOpData& unary = ast->GetUnaryOp(expr);
            const char* op = "?";
            switch (unary.op) {
                case UnaryOpType::NOT: op = "!"; break;
                case UnaryOpType::NEGATE: op = "-"; break;
                case UnaryOpType::BITWISE_NOT: op = "~"; break;
                default: break;
            }
            return std::string(op) + FormatVariantExpression(unary.operand);
        }
        case ASTNodeType::BINARY_OP: {
            const BinaryOpData& bin = ast->GetBinaryOp(expr);
            const char* op = "?";
            switch (bin.op) {
                case BinaryOpType::AND: op = "&&"; break;
                case BinaryOpType::OR: op = "||"; break;
                case BinaryOpType::EQUALS: op = "=="; break;
                case BinaryOpType::NOT_EQUALS: op = "!="; break;
                case BinaryOpType::LESS: op = "<"; break;
                case BinaryOpType::GREATER: op = ">"; break;
                case BinaryOpType::LESS_EQUAL: op = "<="; break;
                case BinaryOpType::GREATER_EQUAL: op = ">="; break;
                case BinaryOpType::ADD: op = "+"; break;
                case BinaryOpType::SUBTRACT: op = "-"; break;
                case BinaryOpType::MULTIPLY: op = "*"; break;
                case BinaryOpType::DIVIDE: op = "/"; break;
                default: break;
            }
            return FormatVariantExpression(bin.left) + " " + op + " " + FormatVariantExpression(bin.right);
        }
        case ASTNodeType::TERNARY_EXPRESSION: {
            const TernaryExprData& ternary = ast->GetTernaryExpression(expr);
            return FormatVariantExpression(ternary.condition) + " ? " +
                   FormatVariantExpression(ternary.trueExpr) + " : " +
                   FormatVariantExpression(ternary.falseExpr);
        }
        default:
            return "<expr>";
    }
}

bool Parser::BuildVariantSelection(NodeRef pipeline, const VariantSelectionData* baseSelection,
                                   u32 attributeMask, bool hasAttributeMask,
                                   const std::vector<VariantOverride>& overrides,
                                   VariantSelectionData* outSelection,
                                   std::string* outError) {
    if (!outSelection) return false;
    outSelection->values.clear();
    outSelection->attributeMask = attributeMask;
    outSelection->hasAttributeMask = hasAttributeMask;
    outSelection->resourceMask = baseSelection ? baseSelection->resourceMask : 0;
    outSelection->hasResourceMask = baseSelection ? baseSelection->hasResourceMask : false;

    if (!ResolvePipelineVariants(pipeline, outError)) {
        return false;
    }

    auto fail = [&](const std::string& msg) -> bool {
        if (outError) *outError = msg;
        return false;
    };

    const PipelineData& pipelineData = ast->GetPipeline(pipeline);

    for (u32 i = 0; i < pipelineData.variantDecls.count; i++) {
        const PipelineVariantDeclData& decl = pipelineData.variantDecls[i];
        VariantSelectionValue value{};
        value.name = decl.name;
        value.typeInfo = decl.typeInfo;
        value.enumTypeHash = decl.enumTypeHash;
        value.value = decl.defaultValue;
        value.implicitKind = ImplicitVariantKind::None;
        outSelection->values.push_back(value);
    }

    for (const auto& overrideValue : overrides) {
        u32 nameHash = Utils::HashStr(overrideValue.name.c_str());
        VariantSelectionValue* target = nullptr;
        for (auto& value : outSelection->values) {
            if (value.name.nameHash == nameHash) {
                target = &value;
                break;
            }
        }
        if (!target) {
            if (overrideValue.name.rfind("has_", 0) == 0) {
                return fail("Implicit variant facts cannot be overridden explicitly");
            }
            return fail("Unknown variant override '" + overrideValue.name + "'");
        }

        std::string raw = overrideValue.value;
        std::string lower = raw;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        LiteralValue parsed{};
        if (target->typeInfo.coreType == CoreType::BOOL) {
            if (lower == "true" || lower == "1") {
                parsed.type = LiteralValue::BOOL;
                parsed.boolValue = true;
            } else if (lower == "false" || lower == "0") {
                parsed.type = LiteralValue::BOOL;
                parsed.boolValue = false;
            } else {
                return fail("Boolean variant override must be true/false/1/0");
            }
        } else {
            const EnumData* enumData = SymbolTable::ResolveEnumDataByHash(&symbolTable, target->enumTypeHash);
            if (!enumData) {
                return fail("Variant enum type could not be resolved");
            }

            std::string variantName = raw;
            size_t lastScope = variantName.rfind("::");
            if (lastScope != std::string::npos) {
                variantName = variantName.substr(lastScope + 2);
            }
            u32 variantHash = Utils::HashStr(variantName.c_str());
            bool found = false;
            for (u32 i = 0; i < enumData->variants.count; i++) {
                if (enumData->variants[i].name.nameHash != variantHash) continue;
                if (target->typeInfo.coreType == CoreType::UINT) {
                    parsed.type = LiteralValue::UINT;
                    parsed.uintValue = enumData->variants[i].value;
                } else {
                    parsed.type = LiteralValue::INT;
                    parsed.intValue = static_cast<int>(enumData->variants[i].value);
                }
                found = true;
                break;
            }
            if (!found) {
                return fail("Unknown enum variant override '" + raw + "'");
            }
        }
        target->value = parsed;
    }

    for (u32 i = 0; i < pipelineData.attributes.count; i++) {
        const AttributeDeclData& attr = ast->GetAttributeDecl(pipelineData.attributes[i]);
        if (!IsOptionalAttributeFeature(pipeline, attr.attributeIndex)) continue;

        VariantSelectionValue value{};
        std::string implicitName = std::string("has_") + attr.name.ToString(sourceBase());
        value.name = ArenaString::MakeHashOnly(implicitName);
        ReverseLookup::Register(value.name.nameHash, implicitName.c_str());
        value.typeInfo = TYPE_INFO(CoreType::BOOL, 1, false);
        value.enumTypeHash = 0;
        value.value.type = LiteralValue::BOOL;
        value.value.boolValue = hasAttributeMask
            ? ((attributeMask & (1u << attr.attributeIndex)) != 0)
            : true;
        value.isImplicit = true;
        value.implicitKind = ImplicitVariantKind::Attribute;
        value.attributeIndex = attr.attributeIndex;
        value.resourceIndex = 0xFF;
        outSelection->values.push_back(value);
    }

    for (u32 i = 0; i < pipelineData.resources.count; i++) {
        const ResourceDeclData& resourceDecl = ast->GetResourceDecl(pipelineData.resources[i]);
        if (!IsOptionalResourceFeature(pipeline, resourceDecl.resourceIndex)) continue;

        VariantSelectionValue value{};
        std::string implicitName = std::string("has_resource_") + resourceDecl.name.ToString(sourceBase());
        value.name = ArenaString::MakeHashOnly(implicitName);
        ReverseLookup::Register(value.name.nameHash, implicitName.c_str());
        value.typeInfo = TYPE_INFO(CoreType::BOOL, 1, false);
        value.enumTypeHash = 0;
        value.value.type = LiteralValue::BOOL;
        value.value.boolValue = outSelection->hasResourceMask
            ? ((outSelection->resourceMask & (1u << resourceDecl.resourceIndex)) != 0)
            : true;
        value.isImplicit = true;
        value.implicitKind = ImplicitVariantKind::Resource;
        value.attributeIndex = 0xFF;
        value.resourceIndex = resourceDecl.resourceIndex;
        outSelection->values.push_back(value);
    }

    SetActiveVariantSelection(*outSelection, true);
    for (u32 i = 0; i < pipelineData.variantRules.count; i++) {
        const VariantRuleData& rule = pipelineData.variantRules[i];
        LiteralValue lhsValue;
        LiteralValue rhsValue;
        if (!EvaluateNodeWithEvalBindings(rule.lhs, &lhsValue) ||
            !EvaluateNodeWithEvalBindings(rule.rhs, &rhsValue)) {
            ClearActiveVariantSelection();
            return fail("Failed to evaluate variant legality rules for the requested selection");
        }
        bool lhsBool = false;
        bool rhsBool = false;
        if (!ConvertLiteralToBool(lhsValue, &lhsBool) || !ConvertLiteralToBool(rhsValue, &rhsBool)) {
            ClearActiveVariantSelection();
            return fail("Variant legality rules must evaluate to booleans");
        }
        if (rule.type == VariantRuleType::Require) {
            if (lhsBool && !rhsBool) {
                ClearActiveVariantSelection();
                return fail("Variant selection violates rule: require " +
                            FormatVariantExpression(rule.lhs) + " -> " +
                            FormatVariantExpression(rule.rhs));
            }
        } else if (lhsBool && rhsBool) {
            ClearActiveVariantSelection();
            return fail("Variant selection violates rule: conflict " +
                        FormatVariantExpression(rule.lhs) + ", " +
                        FormatVariantExpression(rule.rhs));
        }
    }
    ClearActiveVariantSelection();

    return true;
}

bool Parser::BuildVariantReflection(NodeRef pipeline, const VariantSelectionData* selection,
                                    VariantReflectionData* outReflection,
                                    std::string* outError) {
    if (!outReflection) return false;
    outReflection->declared.clear();
    outReflection->implicit.clear();
    outReflection->selected.clear();
    outReflection->rules.clear();
    outReflection->symbolTable = &symbolTable;
    outReflection->sourceBase = sourceBase();
    outReflection->attributeMask = selection ? selection->attributeMask : 0;
    outReflection->hasAttributeMask = selection ? selection->hasAttributeMask : false;
    outReflection->resourceMask = selection ? selection->resourceMask : 0;
    outReflection->hasResourceMask = selection ? selection->hasResourceMask : false;

    if (!ResolvePipelineVariants(pipeline, outError)) {
        return false;
    }

    const PipelineData& pipelineData = ast->GetPipeline(pipeline);
    for (u32 i = 0; i < pipelineData.variantDecls.count; i++) {
        const PipelineVariantDeclData& decl = pipelineData.variantDecls[i];
        VariantDeclarationReflection reflection{};
        reflection.name = decl.name;
        reflection.typeInfo = decl.typeInfo;
        reflection.enumTypeHash = decl.enumTypeHash;
        reflection.defaultValue = decl.defaultValue;
        outReflection->declared.push_back(reflection);
    }

    for (u32 i = 0; i < pipelineData.attributes.count; i++) {
        const AttributeDeclData& attr = ast->GetAttributeDecl(pipelineData.attributes[i]);
        if (!IsOptionalAttributeFeature(pipeline, attr.attributeIndex)) continue;
        VariantDeclarationReflection reflection{};
        std::string implicitName = std::string("has_") + attr.name.ToString(sourceBase());
        reflection.name = ArenaString::MakeHashOnly(implicitName);
        ReverseLookup::Register(reflection.name.nameHash, implicitName.c_str());
        reflection.typeInfo = TYPE_INFO(CoreType::BOOL, 1, false);
        reflection.enumTypeHash = 0;
        reflection.isImplicit = true;
        reflection.implicitKind = ImplicitVariantKind::Attribute;
        reflection.attributeIndex = attr.attributeIndex;
        outReflection->implicit.push_back(reflection);
    }

    for (u32 i = 0; i < pipelineData.resources.count; i++) {
        const ResourceDeclData& resourceDecl = ast->GetResourceDecl(pipelineData.resources[i]);
        if (!IsOptionalResourceFeature(pipeline, resourceDecl.resourceIndex)) continue;
        VariantDeclarationReflection reflection{};
        std::string implicitName = std::string("has_resource_") + resourceDecl.name.ToString(sourceBase());
        reflection.name = ArenaString::MakeHashOnly(implicitName);
        ReverseLookup::Register(reflection.name.nameHash, implicitName.c_str());
        reflection.typeInfo = TYPE_INFO(CoreType::BOOL, 1, false);
        reflection.enumTypeHash = 0;
        reflection.isImplicit = true;
        reflection.implicitKind = ImplicitVariantKind::Resource;
        reflection.resourceIndex = resourceDecl.resourceIndex;
        outReflection->implicit.push_back(reflection);
    }

    for (u32 i = 0; i < pipelineData.variantRules.count; i++) {
        const VariantRuleData& rule = pipelineData.variantRules[i];
        VariantRuleReflection reflection{};
        reflection.kind = (rule.type == VariantRuleType::Require) ? "require" : "conflict";
        reflection.lhs = FormatVariantExpression(rule.lhs);
        reflection.rhs = FormatVariantExpression(rule.rhs);
        outReflection->rules.push_back(reflection);
    }

    if (selection) {
        outReflection->selected = selection->values;
    }

    return true;
}

bool Parser::ConvertLiteralToBool(const LiteralValue& value, bool* outBool) const {
    switch (value.type) {
        case LiteralValue::BOOL:
            *outBool = value.boolValue;
            return true;
        case LiteralValue::INT:
            *outBool = value.intValue != 0;
            return true;
        case LiteralValue::UINT:
            *outBool = value.uintValue != 0;
            return true;
        case LiteralValue::FLOAT:
            *outBool = value.floatValue != 0.0f;
            return true;
        default:
            return false;
    }
}

bool Parser::CoerceLiteralToType(const TypeInfo& typeInfo, LiteralValue* value) const {
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
                if (value->uintValue > static_cast<unsigned int>(INT_MAX)) {
                    return false;
                }
                value->intValue = static_cast<int>(value->uintValue);
                value->type = LiteralValue::INT;
            }
            return value->type == LiteralValue::INT;

        case CoreType::UINT:
            if (value->type == LiteralValue::INT) {
                if (value->intValue < 0) {
                    return false;
                }
                value->uintValue = static_cast<unsigned int>(value->intValue);
                value->type = LiteralValue::UINT;
            }
            return value->type == LiteralValue::UINT;

        case CoreType::BOOL:
            return value->type == LiteralValue::BOOL;

        case CoreType::FLOAT2:
            return value->type == LiteralValue::FLOAT2;
        case CoreType::FLOAT3:
            return value->type == LiteralValue::FLOAT3;
        case CoreType::FLOAT4:
            return value->type == LiteralValue::FLOAT4;
        case CoreType::INT2:
            return value->type == LiteralValue::INT2;
        case CoreType::INT3:
            return value->type == LiteralValue::INT3;
        case CoreType::INT4:
            return value->type == LiteralValue::INT4;

        default:
            return false;
    }
}

NodeRef Parser::MakeLiteralNodeFromValue(const LiteralValue& value, u32 line, u32 col) {
    switch (value.type) {
        case LiteralValue::FLOAT:
            return ASTFactory::MakeLiteralFloat(ast, value.floatValue, line, col);
        case LiteralValue::INT:
            return ASTFactory::MakeLiteralInt(ast, value.intValue, line, col);
        case LiteralValue::UINT:
            return ASTFactory::MakeLiteralUint(ast, value.uintValue, line, col);
        case LiteralValue::BOOL:
            return ASTFactory::MakeLiteralBool(ast, value.boolValue, line, col);
        case LiteralValue::FLOAT2:
        case LiteralValue::FLOAT3:
        case LiteralValue::FLOAT4:
        case LiteralValue::INT2:
        case LiteralValue::INT3:
        case LiteralValue::INT4: {
            const char* constructorName = nullptr;
            u8 componentCount = 0;
            bool isFloat = true;
            switch (value.type) {
                case LiteralValue::FLOAT2: constructorName = "float2"; componentCount = 2; break;
                case LiteralValue::FLOAT3: constructorName = "float3"; componentCount = 3; break;
                case LiteralValue::FLOAT4: constructorName = "float4"; componentCount = 4; break;
                case LiteralValue::INT2: constructorName = "int2"; componentCount = 2; isFloat = false; break;
                case LiteralValue::INT3: constructorName = "int3"; componentCount = 3; isFloat = false; break;
                case LiteralValue::INT4: constructorName = "int4"; componentCount = 4; isFloat = false; break;
                default: break;
            }

            NodeRef vecCall = ASTFactory::MakeFunctionCall(ast, ArenaString::MakeHashOnly(constructorName), line, col);
            FunctionCallData& callData = ast->GetFunctionCall(vecCall);
            for (u8 c = 0; c < componentCount; c++) {
                NodeRef arg = isFloat
                    ? ASTFactory::MakeLiteralFloat(ast, value.floatVec[c], line, col)
                    : ASTFactory::MakeLiteralInt(ast, value.intVec[c], line, col);
                callData.arguments.Push(arena, arg);
            }
            return vecCall;
        }
        default:
            return NodeRef::Null();
    }
}

bool Parser::EvaluateNodeWithEvalBindings(NodeRef node, LiteralValue* outValue) {
    if (node.IsNull()) return false;

    std::vector<ParamSubstitution> substitutions;
    BuildVisibleEvalSubstitutions(substitutions);
    NodeRef substituted = CloneNodeWithParams(node, substitutions.data(),
                                              static_cast<u32>(substitutions.size()));
    if (substituted.IsNull()) return false;

    EvalStateSoA evalState;
    CompileTimeEvaluatorSoA::Init(&evalState, this, ast, &context->evalCache, ast->arena);

    if (!CompileTimeEvaluatorSoA::CanEvaluateNode(&evalState, substituted)) {
        return false;
    }
    return CompileTimeEvaluatorSoA::EvaluateNode(&evalState, substituted, outValue);
}

bool Parser::BindCompileTimeVariable(NodeRef varDecl) {
    const VariableDeclData& decl = ast->GetVariableDecl(varDecl);
    Symbol* sym = SymbolTable::LookupAny(&symbolTable, decl.name);
    if (!sym || sym->kind != SymbolKind::VARIABLE) {
        Error("Failed to resolve compile-time declaration");
        return false;
    }

    VariableData& varData = symbolTable.variables[sym->index];

    if (decl.initializer.IsNull()) {
        if (!varData.isEval) {
            Error("Compile-time declarations in eval blocks must be initialized");
            return false;
        }

        varData.isConst = true;
        UpdateEvalBinding(decl.name.nameHash, varData.evalValue);
        return true;
    }

    LiteralValue value;
    if (!EvaluateNodeWithEvalBindings(decl.initializer, &value)) {
        Error("Compile-time declarations in eval blocks must have compile-time constant initializers");
        return false;
    }

    if (!CoerceLiteralToType(varData.typeInfo, &value)) {
        Error("Type mismatch in compile-time declaration");
        return false;
    }

    varData.isConst = true;
    varData.isEval = true;
    varData.hasEvalValue = true;
    varData.evalValue = value;
    UpdateEvalBinding(decl.name.nameHash, value);
    return true;
}

bool Parser::ExecuteCompileTimeAssignment(NodeRef assignment) {
    const AssignmentData& assign = ast->GetAssignment(assignment);
    if (assign.target.Type() != ASTNodeType::IDENTIFIER) {
        return false;
    }

    const IdentifierData& ident = ast->GetIdentifier(assign.target);
    LiteralValue currentValue;
    if (!LookupEvalBinding(ident.name.nameHash, &currentValue)) {
        return false;
    }

    LiteralValue newValue;
    if (!EvaluateNodeWithEvalBindings(assign.value, &newValue)) {
        Error("Compile-time assignments in eval blocks must use compile-time values");
        return false;
    }

    Symbol* sym = SymbolTable::LookupAny(&symbolTable, ident.name);
    if (sym && sym->kind == SymbolKind::VARIABLE) {
        VariableData& varData = symbolTable.variables[sym->index];
        if (!CoerceLiteralToType(varData.typeInfo, &newValue)) {
            Error("Type mismatch in compile-time assignment");
            return false;
        }
        varData.isEval = true;
        varData.hasEvalValue = true;
        varData.evalValue = newValue;
    }

    UpdateEvalBinding(ident.name.nameHash, newValue);
    return true;
}

bool Parser::ExpandEvalStatementsFromBlock(NodeRef blockNode, BlockData& outBlock) {
    if (blockNode.IsNull()) return true;

    const BlockData& block = ast->GetBlock(blockNode);
    PushEvalBindingScope();
    for (u32 i = 0; i < block.statements.count; i++) {
        if (!ExpandEvalStatement(block.statements[i], outBlock)) {
            PopEvalBindingScope();
            return false;
        }
    }
    PopEvalBindingScope();
    return true;
}

bool Parser::ExpandEvalStatement(NodeRef stmt, BlockData& outBlock) {
    if (stmt.IsNull()) return true;

    if (evalExpansionBudget == 0) {
        Error("Eval expansion exceeded total budget "
              "(100000 statements). Check for combinatorially nested "
              "eval loops.");
        return false;
    }
    evalExpansionBudget--;

    auto cloneWithBindings = [&](NodeRef node) -> NodeRef {
        std::vector<ParamSubstitution> substitutions;
        BuildVisibleEvalSubstitutions(substitutions);
        return CloneNodeWithParams(node, substitutions.data(),
                                   static_cast<u32>(substitutions.size()));
    };

    auto literalToInt = [&](const LiteralValue& value, s32* outInt) -> bool {
        switch (value.type) {
            case LiteralValue::INT:
                *outInt = static_cast<s32>(value.intValue);
                return true;
            case LiteralValue::UINT:
                if (value.uintValue > static_cast<unsigned int>(INT_MAX)) {
                    return false;
                }
                *outInt = static_cast<s32>(value.uintValue);
                return true;
            default:
                return false;
        }
    };

    switch (stmt.Type()) {
        case ASTNodeType::BLOCK:
            return ExpandEvalStatementsFromBlock(stmt, outBlock);

        case ASTNodeType::VARIABLE_DECL: {
            const VariableDeclData& decl = ast->GetVariableDecl(stmt);
            if (decl.isConst) {
                return BindCompileTimeVariable(stmt);
            }

            if (LookupEvalBinding(decl.name.nameHash, nullptr)) {
                Error("Runtime declarations in eval blocks cannot shadow compile-time bindings");
                return false;
            }

            NodeRef cloned = cloneWithBindings(stmt);
            if (cloned.IsValid()) {
                outBlock.statements.Push(arena, cloned);
            }
            AddEvalShadow(decl.name.nameHash);
            return true;
        }

        case ASTNodeType::ASSIGNMENT:
            if (ExecuteCompileTimeAssignment(stmt)) {
                return true;
            }
            break;

        case ASTNodeType::IF_STATEMENT: {
            const BlockData& ifData = ast->GetBlock(stmt);
            if (ifData.statements.count == 0) return true;

            LiteralValue condValue;
            if (!EvaluateNodeWithEvalBindings(ifData.statements[0], &condValue)) {
                Error("Conditions in eval blocks must be compile-time constants");
                return false;
            }

            bool conditionTrue = false;
            if (!ConvertLiteralToBool(condValue, &conditionTrue)) {
                Error("Eval block conditions must resolve to bool, int, uint, or float");
                return false;
            }

            if (conditionTrue && ifData.statements.count >= 2) {
                return ExpandEvalStatement(ifData.statements[1], outBlock);
            }
            if (!conditionTrue && ifData.statements.count >= 3) {
                return ExpandEvalStatement(ifData.statements[2], outBlock);
            }
            return true;
        }

        case ASTNodeType::FOR_RANGE: {
            const ForRangeData& loop = ast->GetForRange(stmt);
            LiteralValue startValue, endValue, stepValue;
            if (!EvaluateNodeWithEvalBindings(loop.rangeStart, &startValue) ||
                !EvaluateNodeWithEvalBindings(loop.rangeEnd, &endValue)) {
                Error("Eval for ranges must be compile-time constants");
                return false;
            }

            s32 start = 0;
            s32 end = 0;
            s32 step = 1;
            if (!literalToInt(startValue, &start) || !literalToInt(endValue, &end)) {
                Error("Eval for ranges must resolve to integers");
                return false;
            }

            if (!loop.step.IsNull()) {
                if (!EvaluateNodeWithEvalBindings(loop.step, &stepValue) ||
                    !literalToInt(stepValue, &step)) {
                    Error("Eval for steps must resolve to integers");
                    return false;
                }
            }

            if (step == 0) {
                Error("Eval for step must not be zero");
                return false;
            }

            if (loop.iteratorVar.Type() != ASTNodeType::IDENTIFIER) {
                Error("Eval for iterator must be an identifier");
                return false;
            }

            const IdentifierData& iteratorIdent = ast->GetIdentifier(loop.iteratorVar);
            u32 iterationCount = 0;
            constexpr u32 MAX_EVAL_ITERATIONS = 10000;

            auto continueLoop = [&](s32 value) -> bool {
                if (step > 0) {
                    return loop.inclusive ? value <= end : value < end;
                }
                return loop.inclusive ? value >= end : value > end;
            };

            for (s32 i = start; continueLoop(i); i += step) {
                if (++iterationCount > MAX_EVAL_ITERATIONS) {
                    Error("Eval for exceeded iteration limit");
                    return false;
                }

                LiteralValue iteratorValue{};
                iteratorValue.type = LiteralValue::INT;
                iteratorValue.intValue = static_cast<int>(i);

                PushEvalBindingScope();
                AddEvalBinding(iteratorIdent.name.nameHash, iteratorValue);
                if (!ExpandEvalStatement(loop.body, outBlock)) {
                    PopEvalBindingScope();
                    return false;
                }
                PopEvalBindingScope();
            }
            return true;
        }

        case ASTNodeType::FOR_COLLECTION: {
            const ForCollectionData& loop = ast->GetForCollection(stmt);
            if (loop.iteratorVar.Type() != ASTNodeType::IDENTIFIER) {
                Error("Eval collection iterator must be an identifier");
                return false;
            }

            const IdentifierData& iteratorIdent = ast->GetIdentifier(loop.iteratorVar);
            for (u32 i = 0; i < loop.length; i++) {
                LiteralValue iteratorValue{};
                iteratorValue.type = LiteralValue::INT;
                iteratorValue.intValue = static_cast<int>(i);

                PushEvalBindingScope();
                AddEvalBinding(iteratorIdent.name.nameHash, iteratorValue);
                if (!ExpandEvalStatement(loop.body, outBlock)) {
                    PopEvalBindingScope();
                    return false;
                }
                PopEvalBindingScope();
            }
            return true;
        }

        case ASTNodeType::FOR_CSTYLE:
        {
            const ForCStyleData& loop = ast->GetForCStyle(stmt);
            if (!loop.isWhile) {
                Error("C-style for loops are not yet supported inside eval blocks");
                return false;
            }

            constexpr u32 MAX_EVAL_ITERATIONS = 10000;
            u32 iterationCount = 0;
            while (true) {
                LiteralValue conditionValue;
                if (!EvaluateNodeWithEvalBindings(loop.condition, &conditionValue)) {
                    Error("Eval while conditions must be compile-time constants");
                    return false;
                }

                bool conditionTrue = false;
                if (!ConvertLiteralToBool(conditionValue, &conditionTrue)) {
                    Error("Eval while conditions must resolve to bool, int, uint, or float");
                    return false;
                }
                if (!conditionTrue) return true;

                if (++iterationCount > MAX_EVAL_ITERATIONS) {
                    Error("Eval while exceeded iteration limit");
                    return false;
                }
                if (!ExpandEvalStatement(loop.body, outBlock)) {
                    return false;
                }
            }
        }

        case ASTNodeType::LOOP: {
            const LoopData& loop = ast->GetLoop(stmt);
            constexpr u32 MAX_EVAL_ITERATIONS = 10000;
            u32 iterationCount = 0;

            auto checkUntilCondition = [&](bool* outDone) -> bool {
                *outDone = false;
                if (loop.untilCondition.IsNull()) return true;
                LiteralValue untilValue;
                if (!EvaluateNodeWithEvalBindings(loop.untilCondition, &untilValue)) {
                    Error("Eval loop until conditions must be compile-time constants");
                    return false;
                }

                if (!ConvertLiteralToBool(untilValue, outDone)) {
                    Error("Eval loop until conditions must resolve to bool, int, uint, or float");
                    return false;
                }
                return true;
            };

            if (!loop.count.IsNull()) {
                LiteralValue countValue;
                s32 count = 0;
                if (!EvaluateNodeWithEvalBindings(loop.count, &countValue) ||
                    !literalToInt(countValue, &count)) {
                    Error("Eval loop counts must resolve to integers");
                    return false;
                }
                if (count < 0) {
                    Error("Eval loop count must not be negative");
                    return false;
                }

                for (s32 i = 0; i < count; i++) {
                    if (++iterationCount > MAX_EVAL_ITERATIONS) {
                        Error("Eval loop exceeded iteration limit");
                        return false;
                    }
                    if (!ExpandEvalStatement(loop.body, outBlock)) {
                        return false;
                    }
                    bool done = false;
                    if (!loop.untilCondition.IsNull()) {
                        if (!checkUntilCondition(&done)) {
                            return false;
                        }
                        if (done) break;
                    }
                }
                return true;
            }

            if (loop.untilCondition.IsNull()) {
                Error("Infinite eval loops require an until condition");
                return false;
            }

            while (true) {
                if (++iterationCount > MAX_EVAL_ITERATIONS) {
                    Error("Eval loop exceeded iteration limit");
                    return false;
                }
                if (!ExpandEvalStatement(loop.body, outBlock)) {
                    return false;
                }
                bool done = false;
                if (!checkUntilCondition(&done)) {
                    return false;
                }
                if (done) {
                    return true;
                }
            }
        }

        default:
            break;
    }

    NodeRef cloned = cloneWithBindings(stmt);
    if (cloned.IsValid()) {
        outBlock.statements.Push(arena, cloned);
    }
    return true;
}

NodeRef Parser::ParseEvalBlock() {
    SourceLocation loc = getLocation(stream->GetOffset(current));
    u32 line = loc.line;
    u32 col = loc.column;

    Consume(TokenType::LEFT_BRACE, "Expected '{' after 'eval'");

    NodeRef block = ASTFactory::MakeEvalBlock(ast, line, col);
    BlockData& outBlock = ast->GetBlock(block);

    SymbolTable::EnterScope(&symbolTable);

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        NodeRef stmt = ParseStatement();
        if (stmt.IsValid()) {
            outBlock.statements.Push(arena, stmt);
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after eval block");

    SymbolTable::ExitScope(&symbolTable);
    return block;
}

NodeRef Parser::ParseEvalStatement() {
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;
    // 'eval' already consumed

    // Check for function: eval funcName :: (params) -> type { ... }
    if (IsFunctionDeclStart()) {
        // It's an eval function
        NodeRef func = ParseFunction();
        if (func.IsValid()) {
            const FunctionDeclData& decl = ast->GetFunction(func);
            std::vector<OverloadTypeMask> paramMasks;
            BuildParamMasks(decl.parameters, paramMasks);
            // Mark as eval in symbol table
            Symbol* sym = SymbolTable::LookupFunctionOverload(&symbolTable, decl.name,
                paramMasks.data(), static_cast<u32>(paramMasks.size()));
            if (sym && sym->kind == SymbolKind::FUNCTION) {
                symbolTable.functions[sym->index].isEval = true;

                // Cache for compile-time calls
                u32 funcHash = ast->GetFunction(func).name.nameHash;
                if (context->evalCache.functionCount < 64) {
                    context->evalCache.functionHashes[context->evalCache.functionCount] = funcHash;
                    context->evalCache.functionIndices[context->evalCache.functionCount] =
                        static_cast<uintptr_t>(func.packed);
                    context->evalCache.functionCount++;
                }
            }
        }
        return func;
    }

    // Must be eval variable - REQUIRE explicit type
    if (!MatchMask(TokenMasks::CORE_TYPES)) {
        Error("Expected type after 'eval'");
        return NodeRef::Null();
    }

    // Get type info directly from token
    TokenType typeToken = static_cast<TokenType>(stream->GetType(previous));
    std::string typeName(stream->GetValue(previous));
    TypeInfo typeInfo = GetTypeInfoFromToken(typeToken);

    // Check if it's actually a type token
    if (typeInfo.coreType == CoreType::INVALID) {
        Error("Invalid type specified for eval");
        return NodeRef::Null();
    }

    Consume(TokenType::IDENTIFIER, "Expected identifier after type");
    std::string varName(stream->GetValue(previous));

    Consume(TokenType::ASSIGN, "eval declarations must be initialized");

    // Parse expression
    NodeRef expr = ParseExpression();
    if (!expr.IsValid()) {
        Error("Expected expression after '='");
        return NodeRef::Null();
    }

    Consume(TokenType::SEMICOLON, "Expected ';'");

    // Create VARIABLE_DECL node
    NodeRef varDecl = ASTFactory::MakeVariableDecl(ast,
        ArenaString::MakeHashOnly(varName),
        ArenaString::MakeHashOnly(typeName),
        expr, true, line, col);
    ast->GetVariableDecl(varDecl).isEval = true;

    // Add to symbol table
    Symbol* sym = SymbolTable::AddSymbol(&symbolTable,
        ArenaString::MakeHashOnly(varName), SymbolKind::VARIABLE);

    if (sym) {
        VariableData& varData = symbolTable.variables[sym->index];
        varData.typeInfo = typeInfo;
        varData.isConst = true;
        varData.isEval = true;
        varData.constExpr = expr;
        varData.hasEvalValue = false;
    } else {
        Error("Variable already declared in this scope");
        return NodeRef::Null();
    }

    return varDecl;
}

//==============================================================================
// Eval if parsing - compile-time conditional
//==============================================================================

NodeRef Parser::ParseEvalIf() {
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;
    // 'eval' and 'if' already consumed

    // Parse condition in parentheses
    Consume(TokenType::LEFT_PAREN, "Expected '(' after 'eval if'");
    NodeRef condition = ParseExpression();
    if (!condition.IsValid()) {
        Error("Expected condition expression in eval if");
        return NodeRef::Null();
    }
    Consume(TokenType::RIGHT_PAREN, "Expected ')' after condition");

    // Parse the body (either a block or single statement)
    NodeRef body = NodeRef::Null();
    if (Check(TokenType::LEFT_BRACE)) {
        Consume(TokenType::LEFT_BRACE, "Expected '{'");
        body = ParseBlock();
    } else {
        // Single statement
        body = ParseStatement();
    }

    NodeRef ifStmt = ASTFactory::MakeEvalIfStatement(ast, line, col);
    ast->GetBlock(ifStmt).statements.Push(arena, condition);
    ast->GetBlock(ifStmt).statements.Push(arena, body);

    if (Match(TokenType::ELSE)) {
        if (Check(TokenType::IF)) {
            NodeRef elseIfBody = ParseStatement();
            ast->GetBlock(ifStmt).statements.Push(arena, elseIfBody);
        } else if (Match(TokenType::LEFT_BRACE)) {
            NodeRef elseBody = ParseBlock();
            ast->GetBlock(ifStmt).statements.Push(arena, elseBody);
        } else {
            SourceLocation elseLoc = getLocation(stream->GetOffset(current));
            NodeRef elseBody = ASTFactory::MakeBlock(ast, elseLoc.line, elseLoc.column);
            NodeRef elseStmt = ParseStatement();
            if (elseStmt.IsValid()) {
                ast->GetBlock(elseBody).statements.Push(arena, elseStmt);
            }
            ast->GetBlock(ifStmt).statements.Push(arena, elseBody);
        }
    }

    return ifStmt;
}

//==============================================================================
// For loop parsing
//==============================================================================

NodeRef Parser::ParseForStatement(bool isEval) {
    TokenType loopType = PreviousTokenType();
    SymbolTable::EnterScope(&symbolTable);
    u32 scopesToExit = 1;

    Consume(TokenType::LEFT_PAREN, "Expected '(' after 'for'");

    if (loopType == TokenType::FOREACH) {
        NodeRef rootLoop = NodeRef::Null();
        NodeRef* lastBodyPtr = nullptr;

        do {
            SymbolTable::EnterScope(&symbolTable);
            scopesToExit++;

            Consume(TokenType::IDENTIFIER, "Expected iterator variable in foreach");
            std::string varName(stream->GetValue(previous));
            SourceLocation loc = getLocation(stream->GetOffset(previous));

            Symbol* sym = SymbolTable::AddSymbol(&symbolTable, ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous)), SymbolKind::VARIABLE);
            if (sym) symbolTable.variables[sym->index].typeInfo = TYPE_INFO(CoreType::INT, 1, false);

            NodeRef iteratorVar = ASTFactory::MakeIdentifier(ast, varName, loc.line, loc.column);

            Consume(TokenType::IN, "Expected 'in' in foreach");
            NodeRef rangeStart = ParseExpression();

            NodeRef rangeEnd = NodeRef::Null();
            NodeRef step = NodeRef::Null();
            bool inclusive = false;

            if (Match(TokenType::DOT_DOT) || Match(TokenType::DOT_DOT_EQUAL)) {
                inclusive = (stream->GetType(previous) == TokenType::DOT_DOT_EQUAL);
                rangeEnd = ParseExpression();
            } else {
                Error("Expected range in foreach");
                for (u32 i = 0; i < scopesToExit; ++i) SymbolTable::ExitScope(&symbolTable);
                return NodeRef::Null();
            }

            if (Match(TokenType::BY)) {
                step = ParseExpression();
            }

            // Create the FOR_RANGE node (body will be set later)
            NodeRef loopNode = ASTFactory::MakeForRange(ast, iteratorVar, rangeStart, rangeEnd,
                                                         step, NodeRef::Null(), inclusive, isEval,
                                                         loc.line, loc.column);

            if (rootLoop.IsNull()) {
                rootLoop = loopNode;
            } else if (lastBodyPtr) {
                *lastBodyPtr = loopNode;
            }

            lastBodyPtr = &ast->GetForRange(loopNode).body;

        } while (Match(TokenType::COMMA));

        Consume(TokenType::RIGHT_PAREN, "Expected ')' after foreach clauses");

        Consume(TokenType::LEFT_BRACE, "Expected '{' after foreach");
        NodeRef body = ParseBlock();

        // Set the body on the innermost loop
        if (lastBodyPtr) {
            *lastBodyPtr = body;
        }

        for (u32 i = 0; i < scopesToExit; ++i) SymbolTable::ExitScope(&symbolTable);
        return rootLoop;
    }

    // Regular 'for' loop
    NodeRef firstPart = NodeRef::Null();
    if (!Check(TokenType::SEMICOLON)) {
        SourceLocation loc = getLocation(stream->GetOffset(current));
        u32 line = loc.line;
        u32 col = loc.column;

        // Check for variable declaration: type identifier [= expr]
        if (CheckMask(TokenMasks::CORE_TYPES)) {
            Advance(); // consume type
            TokenType varType = static_cast<TokenType>(stream->GetType(previous));
            std::string typeStr(stream->GetValue(previous));

            // Check for pointer type: int^ means pointer to int
            while (Match(TokenType::BITWISE_XOR)) {
                typeStr += "^";
            }

            Consume(TokenType::IDENTIFIER, "Expected variable name in for loop init");
            std::string varName(stream->GetValue(previous));

            NodeRef initializer = NodeRef::Null();
            if (Match(TokenType::ASSIGN)) {
                initializer = ParseExpression();
            }

            firstPart = ASTFactory::MakeVariableDecl(ast,
                ArenaString::MakeHashOnly(varName),
                ArenaString::MakeHashOnly(typeStr),
                initializer, false, line, col);

            // Add to symbol table
            Symbol* sym = SymbolTable::AddSymbol(&symbolTable, ArenaString::MakeHashOnly(varName), SymbolKind::VARIABLE);
            if (sym) {
                symbolTable.variables[sym->index].typeInfo = GetTypeInfoFromToken(varType);
            }
        } else {
            // Expression (like an assignment)
            firstPart = ParseExpression();
        }
    }

    if (Check(TokenType::IN)) {
        Consume(TokenType::IN, "Expected 'in'");

        NodeRef rangeStart = ParseExpression();
        SourceLocation loc = getLocation(stream->GetOffset(previous));

        if (Match(TokenType::DOT_DOT) || Match(TokenType::DOT_DOT_EQUAL)) {
            bool inclusive = (stream->GetType(previous) == TokenType::DOT_DOT_EQUAL);
            NodeRef rangeEnd = ParseExpression();

            NodeRef step = NodeRef::Null();
            if (Match(TokenType::BY)) {
                step = ParseExpression();
            }

            Consume(TokenType::RIGHT_PAREN, "Expected ')' after for-in expression");
            Consume(TokenType::LEFT_BRACE, "Expected '{' after for");
            NodeRef body = ParseBlock();

            SymbolTable::ExitScope(&symbolTable);
            return ASTFactory::MakeForRange(ast, firstPart, rangeStart, rangeEnd, step, body, inclusive, isEval, loc.line, loc.column);

        } else {
// Collection iteration
Consume(TokenType::RIGHT_PAREN, "Expected ')' after for-in expression");
Consume(TokenType::LEFT_BRACE, "Expected '{' after for");
NodeRef body = ParseBlock();

// Resolve collection length from type info
u32 length = 0;
if (rangeStart.Type() == ASTNodeType::IDENTIFIER) {
    const IdentifierData& ident = ast->GetIdentifier(rangeStart);
    Symbol* sym = SymbolTable::LookupByHash(&symbolTable, ident.name.nameHash);
    if (sym && sym->kind == SymbolKind::VARIABLE) {

        const VariableData& varData = symbolTable.variables[sym->index];
        TypeInfo typeInfo = varData.typeInfo;
        if (IsArray(typeInfo)) {
            length = typeInfo.arrayLength;
        }
        else {
            Error("Expected array type for for-in loop");
        }
       
    }
}

SymbolTable::ExitScope(&symbolTable);
return ASTFactory::MakeForCollection(ast, firstPart, rangeStart, body, isEval, length, loc.line, loc.column);
        }

    } else if (Check(TokenType::SEMICOLON)) {
        SourceLocation loc = getLocation(stream->GetOffset(previous));
        Consume(TokenType::SEMICOLON, "Expected ';'");

        NodeRef condition = NodeRef::Null();
        if (!Check(TokenType::SEMICOLON)) {
            condition = ParseExpression();
        }
        Consume(TokenType::SEMICOLON, "Expected ';'");

        NodeRef increment = NodeRef::Null();
        if (!Check(TokenType::RIGHT_PAREN)) {
            increment = ParseExpression();
        }

        Consume(TokenType::RIGHT_PAREN, "Expected ')' after for clauses");
        Consume(TokenType::LEFT_BRACE, "Expected '{' after for");
        NodeRef body = ParseBlock();

        SymbolTable::ExitScope(&symbolTable);
        return ASTFactory::MakeForCStyle(ast, firstPart, condition, increment, body, isEval, loc.line, loc.column);

    } else if (Check(TokenType::RIGHT_PAREN)) {
        // Collection iteration with implicit 'it' variable
        SourceLocation loc = getLocation(stream->GetOffset(previous));
        std::string varName = "it";
        NodeRef iteratorVar = ASTFactory::MakeIdentifier(ast, varName, loc.line, loc.column);
        Symbol* sym = SymbolTable::AddSymbol(&symbolTable, ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous)), SymbolKind::VARIABLE);
        (void)sym;

        Consume(TokenType::RIGHT_PAREN, "Expected ')'");
        Consume(TokenType::LEFT_BRACE, "Expected '{' after for");
        NodeRef body = ParseBlock();

        // Resolve collection length from type info
        u32 length = 0;
        if (firstPart.Type() == ASTNodeType::IDENTIFIER) {
            const IdentifierData& ident = ast->GetIdentifier(firstPart);
            Symbol* collSym = SymbolTable::LookupByHash(&symbolTable, ident.name.nameHash);
            if (collSym && collSym->kind == SymbolKind::VARIABLE) {
                const VariableData& varData = symbolTable.variables[collSym->index];
                if (IsArray(varData.typeInfo)) {
                    length = varData.typeInfo.arrayLength;
                }
            }
        }

        SymbolTable::ExitScope(&symbolTable);
        return ASTFactory::MakeForCollection(ast, iteratorVar, firstPart, body, isEval, length, loc.line, loc.column);
    }

    ErrorAtCurrent("Invalid for loop structure");
    SymbolTable::ExitScope(&symbolTable);
    return NodeRef::Null();
}

//==============================================================================
// Loop statement parsing
//==============================================================================

NodeRef Parser::ParseWhileStatement(bool isEval) {
    SourceLocation loc = getLocation(stream->GetOffset(previous));

    Consume(TokenType::LEFT_PAREN, "Expected '(' after 'while'");
    NodeRef condition = ParseExpression();
    Consume(TokenType::RIGHT_PAREN, "Expected ')' after while condition");

    SymbolTable::EnterScope(&symbolTable);
    Consume(TokenType::LEFT_BRACE, "Expected '{' after while");
    NodeRef body = ParseBlock();
    SymbolTable::ExitScope(&symbolTable);

    return ASTFactory::MakeForCStyle(ast, NodeRef::Null(), condition, NodeRef::Null(), body,
                                     isEval, loc.line, loc.column, true);
}

NodeRef Parser::ParseLoopStatement(bool isEval) {
    SourceLocation loc = getLocation(stream->GetOffset(previous));

    NodeRef count = NodeRef::Null();
    if (Match(TokenType::LEFT_PAREN)) {
        count = ParseExpression();
        Consume(TokenType::RIGHT_PAREN, "Expected ')' after loop count");
    }

    Consume(TokenType::LEFT_BRACE, "Expected '{' after loop");
    NodeRef body = ParseBlock();

    NodeRef untilCondition = NodeRef::Null();
    if (Match(TokenType::UNTIL)) {
        Consume(TokenType::LEFT_PAREN, "Expected '(' after 'until'");
        untilCondition = ParseExpression();
        Consume(TokenType::RIGHT_PAREN, "Expected ')' after until condition");
    }

    return ASTFactory::MakeLoop(ast, count, body, untilCondition, isEval, loc.line, loc.column);
}

//==============================================================================
// Switch statement parsing
//==============================================================================

NodeRef Parser::ParseSwitch() {
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;

    // Parse switch expression
    Consume(TokenType::LEFT_PAREN, "Expected '(' after 'switch'");
    NodeRef expression = ParseExpression();
    Consume(TokenType::RIGHT_PAREN, "Expected ')' after switch expression");

    // Create switch node
    NodeRef switchNode = ASTFactory::MakeSwitch(ast, expression, line, col);
    SwitchData& switchData = ast->GetSwitch(switchNode);

    // Parse switch body
    Consume(TokenType::LEFT_BRACE, "Expected '{' after switch expression");

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (Match(TokenType::CASE)) {
            // Parse comma-separated case values
            ArenaArray<NodeRef> caseValues;
            caseValues.Init(arena, 4);
            
            do {
                NodeRef caseValue = ParseExpression();
                caseValues.Push(arena, caseValue);
            } while (Match(TokenType::COMMA));
            
            Consume(TokenType::COLON, "Expected ':' after case value(s)");

            // Parse case body (statements until next case/default/closing brace)
            NodeRef caseBody = ASTFactory::MakeBlock(ast, loc.line, loc.column);
            BlockData& bodyBlock = ast->GetBlock(caseBody);

            while (!Check(TokenType::CASE) && !Check(TokenType::DEFAULT) &&
                   !Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
                NodeRef stmt = ParseStatement();
                if (stmt.IsValid()) {
                    bodyBlock.statements.Push(arena, stmt);
                }
            }

            // Create case node and add all values
            NodeRef caseNode = ASTFactory::MakeSwitchCase(ast, caseBody, false, line, col);
            SwitchCaseData& caseData = ast->GetSwitchCase(caseNode);
            for (u32 v = 0; v < caseValues.count; v++) {
                caseData.values.Push(arena, caseValues[v]);
            }
            switchData.cases.Push(arena, caseNode);

        } else if (Match(TokenType::DEFAULT)) {
            Consume(TokenType::COLON, "Expected ':' after 'default'");

            // Parse default body
            NodeRef defaultBody = ASTFactory::MakeBlock(ast, loc.line, loc.column);
            BlockData& bodyBlock = ast->GetBlock(defaultBody);

            while (!Check(TokenType::CASE) && !Check(TokenType::RIGHT_BRACE) &&
                   !Check(TokenType::EOF_TOKEN)) {
                NodeRef stmt = ParseStatement();
                if (stmt.IsValid()) {
                    bodyBlock.statements.Push(arena, stmt);
                }
            }

            // Create default case node and store in switch
            NodeRef defaultNode = ASTFactory::MakeSwitchCase(ast, defaultBody, true, line, col);
            switchData.defaultCase = defaultNode;

        } else {
            Error("Expected 'case' or 'default' in switch statement");
            Advance();
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after switch body");

    return switchNode;
}

} // namespace BWSL
