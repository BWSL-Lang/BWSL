// Part of bwsl_parser_soa.cpp. Include from that file only.
// Pipeline, import, attribute, resource, pass, and shader stage declarations.
#pragma once
#include "bwsl_parser_soa_core.inl"

namespace BWSL {

NodeRef Parser::ParseDocument() {
    TokenRef documentStart = current;
    TokenRef previousStart = previous;

    // First collect file-scope modules so pipelines can import modules declared
    // later in the same BWSL document.
    while (!Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (Match(TokenType::MODULE)) {
            (void)ParseModule();
        } else if (Check(TokenType::PIPELINE)) {
            SkipBracedDeclaration(false);
        } else {
            Advance();
        }
        panicMode = false;
    }

    current = documentStart;
    previous = previousStart;
    hasLookahead = false;
    has3TokenLookahead = false;
    lookahead = INVALID_TOKEN;
    lookahead3 = INVALID_TOKEN;
    currentPipeline = NodeRef::Null();
    currentPass = NodeRef::Null();
    inShaderStage = false;

    NodeRef lastPipeline = NodeRef::Null();
    while (!Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (Check(TokenType::PIPELINE)) {
            NodeRef pipeline = ParsePipeline();
            if (pipeline.IsValid()) {
                lastPipeline = pipeline;
            }
        } else if (Check(TokenType::MODULE)) {
            SkipBracedDeclaration(false);
        } else {
            ErrorAtCurrent("Expected file-scope 'module' or 'pipeline' declaration");
            Advance();
        }
        panicMode = false;
    }

    if (lastPipeline.IsValid()) {
        context->root = lastPipeline;
    }
    return context->root;
}

void Parser::SkipBracedDeclaration(bool keywordAlreadyConsumed) {
    if (!keywordAlreadyConsumed && !Check(TokenType::EOF_TOKEN)) {
        Advance();
    }

    if (Check(TokenType::IDENTIFIER) || Check(TokenType::STRING)) {
        Advance();
    }

    if (!Match(TokenType::LEFT_BRACE)) {
        return;
    }

    u32 depth = 1;
    while (depth > 0 && !Check(TokenType::EOF_TOKEN)) {
        if (Match(TokenType::LEFT_BRACE)) {
            depth++;
        } else if (Match(TokenType::RIGHT_BRACE)) {
            depth--;
        } else {
            Advance();
        }
    }
}

NodeRef Parser::ParsePipeline() {
    SourceLocation loc = getLocation(stream->GetOffset(current));
    u32 line = loc.line;
    u32 col = loc.column;

    Consume(TokenType::PIPELINE, "Expected 'pipeline' keyword");
    Consume(TokenType::IDENTIFIER, "Expected pipeline name");

    NodeRef pipeline = ASTFactory::MakePipeline(ast, std::string(stream->GetValue(previous)), line, col);
    currentPipeline = pipeline;

    Consume(TokenType::LEFT_BRACE, "Expected '{' after pipeline name");

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (Match(TokenType::IMPORT)) {
            ParseImports(pipeline);
        } else if (Match(TokenType::USING)) {
            ParseUsing(pipeline);
        } else if (Match(TokenType::ATTRIBUTES)) {
            ParseAttributes(pipeline, true);
        } else if (Match(TokenType::RESOURCES)) {
            if (ast->GetPipeline(pipeline).resources.count > 0) {
                Error("Only one resources block is allowed per pipeline");
                continue;
            }
            ParseResources(pipeline, true);
        } else if (Match(TokenType::VARIANTS)) {
            if (ast->GetPipeline(pipeline).variantDecls.count > 0 ||
                ast->GetPipeline(pipeline).variantRules.count > 0) {
                Error("Only one variants block is allowed per pipeline");
                continue;
            }
            ParseVariants(pipeline, true);
        } else if (Match(TokenType::COMPUTE_GRAPH)) {
            if (!ast->GetPipeline(pipeline).computeGraph.IsNull()) {
                Error("Only one compute_graph block is allowed per pipeline");
                continue;
            }
            NodeRef graph = ParseComputeGraph();
            if (graph.IsValid()) {
                ast->GetPipeline(pipeline).computeGraph = graph;
            }
        } else if (Match(TokenType::CONST)) {
            if (!MatchMask(TokenMasks::CORE_TYPES)) {
                Error("Expected type after 'const'");
                Advance();
                continue;
            }

            TokenType varType = static_cast<TokenType>(stream->GetType(previous));
            Consume(TokenType::IDENTIFIER, "Expected constant name");
            std::string constName(stream->GetValue(previous));

            Consume(TokenType::ASSIGN, "const variables must be initialized");

            NodeRef value = ParseExpression();
            if (!value.IsValid()) {
                Error("Expected initializer expression for const");
                Advance();
                continue;
            }

            Consume(TokenType::SEMICOLON, "Expected ';'");

            ArenaString constNameStr = ArenaString::MakeHashOnly(constName);
            Symbol* sym = SymbolTable::AddSymbol(&symbolTable, constNameStr, SymbolKind::VARIABLE);
            if (!sym) {
                Error("Variable already declared in this scope");
                continue;
            }

            VariableData& varData = symbolTable.variables[sym->index];
            varData.typeInfo = GetTypeInfoFromToken(varType);
            varData.isConst = true;
            varData.isEval = false;
            varData.constExpr = value;

            EvalStateSoA evalState;
            CompileTimeEvaluatorSoA::Init(&evalState, this, ast, &context->evalCache, ast->arena);
            LiteralValue constValue;
            if (CompileTimeEvaluatorSoA::CanEvaluateNode(&evalState, value) &&
                CompileTimeEvaluatorSoA::EvaluateNode(&evalState, value, &constValue)) {
                bool typeOk = false;
                switch (varData.typeInfo.coreType) {
                    case CoreType::INT:
                        typeOk = (constValue.type == LiteralValue::INT);
                        break;
                    case CoreType::UINT:
                        typeOk = (constValue.type == LiteralValue::UINT);
                        break;
                    case CoreType::FLOAT:
                        if (constValue.type == LiteralValue::INT) {
                            constValue.floatValue = static_cast<float>(constValue.intValue);
                            constValue.type = LiteralValue::FLOAT;
                        }
                        typeOk = (constValue.type == LiteralValue::FLOAT);
                        break;
                    case CoreType::BOOL:
                        typeOk = (constValue.type == LiteralValue::BOOL);
                        break;
                    default:
                        break;
                }
                if (typeOk) {
                    varData.isEval = true;
                    varData.hasEvalValue = true;
                    varData.evalValue = constValue;
                }
            }
        } else if (Match(TokenType::PASS)) {
            NodeRef pass = ParsePass();
            if (pass.IsValid()) {
                ast->GetPipeline(pipeline).passes.Push(arena, pass);
            }
        } else if (Match(TokenType::EVAL)) {
            ParseEvalStatement();
        } else if (Match(TokenType::ENUM)) {
            NodeRef enumDecl = ParseEnum();
            if (enumDecl.IsValid()) {
                ast->GetPipeline(pipeline).enums.Push(arena, enumDecl);
            }
        } else if (Match(TokenType::MODULE)) {
            ErrorAtPrevious("Module declarations must be declared at file scope, outside pipeline blocks");
            SkipBracedDeclaration(true);
        } else if (Match(TokenType::STRUCT)) {
            // Top-level struct
            NodeRef structNode = ParseStruct();
            (void)structNode; // Struct is registered in symbol table
            Match(TokenType::SEMICOLON); // Optional trailing semicolon
        } else if (Check(TokenType::CONSTRAINT)) {
            // Type constraint definition
            NodeRef constraintNode = ParseConstraint();
            if (constraintNode.IsValid()) {
                ast->GetPipeline(pipeline).constraints.Push(arena, constraintNode);
            }
        } else if (IsFunctionDeclStart()) {
            NodeRef function = ParseFunction();
            if (function.IsValid()) {
                ast->GetPipeline(pipeline).functions.Push(arena, function);
            }
        } else {
            ErrorAtCurrent("Expected 'import', 'using', 'attributes', 'resources', 'variants', 'compute_graph', 'constraint', 'enum', 'eval', 'module', 'struct', or 'pass'");
            Advance();
        }
        
        // Reset panic mode after each top-level declaration to allow error recovery
        panicMode = false;
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after pipeline body");

    context->root = pipeline;
    PARSER_TIMING_PRINT();
    return pipeline;
}

//==============================================================================
// Import parsing
//==============================================================================

u32 Parser::ResolveModuleIndexByWrittenName(const ArenaString& moduleName) {
    return SymbolTable::ResolveModuleIndexByHash(&symbolTable, moduleName.nameHash);
}

std::string Parser::CanonicalizeModuleQualifiedName(const std::string& moduleName,
                                                    const std::string& memberName) {
    ArenaString written = ArenaString::MakeHashOnly(moduleName);
    u32 moduleIdx = SymbolTable::ResolveModuleIndexByHash(&symbolTable, written.nameHash);
    std::string resolvedName = moduleName;
    if (moduleIdx != INVALID_INDEX && moduleIdx < symbolTable.modules.count) {
        resolvedName = symbolTable.modules[moduleIdx].name.ToString(sourceBase());
    }
    std::string canonical = resolvedName + "::" + memberName;
    ReverseLookup::Register(Utils::HashStr(canonical.c_str()), canonical.c_str());
    return canonical;
}

std::string Parser::CanonicalizeTypeName(const std::string& typeName) {
    std::string baseName = typeName;
    std::string pointerSuffix;
    while (!baseName.empty() && baseName.back() == '^') {
        pointerSuffix.push_back('^');
        baseName.pop_back();
    }

    size_t scopePos = baseName.find("::");
    if (scopePos == std::string::npos) {
        size_t dotPos = baseName.find('.');
        if (dotPos == std::string::npos ||
            baseName.find('.', dotPos + 1) != std::string::npos) {
            return typeName;
        }

        std::string moduleName = baseName.substr(0, dotPos);
        std::string localName = baseName.substr(dotPos + 1);
        if (!IsIdentifierName(moduleName) || !IsIdentifierName(localName)) {
            return typeName;
        }

        return CanonicalizeModuleQualifiedName(moduleName, localName) + pointerSuffix;
    }

    std::string moduleName = baseName.substr(0, scopePos);
    std::string localName = baseName.substr(scopePos + 2);
    return CanonicalizeModuleQualifiedName(moduleName, localName) + pointerSuffix;
}

void Parser::ParseImports(NodeRef pipeline) {
    ParseModuleImportList(pipeline, true);
}

void Parser::ParseUsing(NodeRef pipeline) {
    ParseUsingDeclaration(pipeline, true);
}

void Parser::ParseModuleImportList(NodeRef owner, bool ownerIsPipeline) {
    static constexpr u32 MAX_IMPORTS = 32;
    u32 importedHashes[MAX_IMPORTS];
    u32 importCount = 0;

    while (Check(TokenType::IDENTIFIER)) {
        std::string moduleNameStr(stream->GetValue(current));
        Advance();

        u32 moduleHash = Utils::HashStr(moduleNameStr.c_str());
        bool hasAlias = false;
        std::string aliasNameStr;

        if (Match(TokenType::AS)) {
            if (Consume(TokenType::IDENTIFIER, "Expected module alias after 'as'")) {
                aliasNameStr = std::string(stream->GetValue(previous));
                hasAlias = true;
            }
        }

        bool alreadyImported = false;
        for (u32 i = 0; i < importCount; i++) {
            if (importedHashes[i] == moduleHash) {
                alreadyImported = true;
                break;
            }
        }

        if (alreadyImported) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Module '%s' is already imported", moduleNameStr.c_str());
            ErrorAtPrevious(msg);

            if (Check(TokenType::COMMA)) {
                Advance();
                continue;
            } else {
                break;
            }
        }

        u32 moduleIdx = SymbolTable::FindModuleByHash(&symbolTable, moduleHash);

        if (moduleIdx == INVALID_INDEX) {
            if (TryRegisterModuleFromDisk(moduleNameStr)) {
                moduleIdx = SymbolTable::FindModuleByHash(&symbolTable, moduleHash);
            }
        }

        if (moduleIdx != INVALID_INDEX) {
            ArenaString moduleName = ArenaString::MakeHashOnly(moduleNameStr);
            if (ownerIsPipeline) {
                ast->GetPipeline(owner).imports.Push(arena, moduleName);
            } else {
                ast->GetModule(owner).imports.Push(arena, moduleName);
            }

            if (importCount < MAX_IMPORTS) {
                importedHashes[importCount++] = moduleHash;
            }

            SymbolTable::AddImportedModule(&symbolTable, moduleIdx);

            if (hasAlias) {
                ArenaString aliasName = ArenaString::MakeHashOnly(aliasNameStr);
                ReverseLookup::Register(aliasName.nameHash, aliasNameStr.c_str());
                if (!SymbolTable::RegisterModuleAlias(&symbolTable, aliasName, moduleName, moduleIdx)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Module alias '%s' conflicts with an existing module or alias",
                             aliasNameStr.c_str());
                    ErrorAtPrevious(msg);
                }
            }
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Unknown module '%s'. Verify that your module exists and has been compiled",
                     moduleNameStr.c_str());
            ErrorAtPrevious(msg);
        }

        if (Check(TokenType::COMMA)) {
            Advance();
            if (!Check(TokenType::IDENTIFIER)) {
                Error("Expected module name after ','");
                break;
            }
        } else if (Check(TokenType::IDENTIFIER) &&
                   stream->GetType(PeekNext()) != TokenType::DOUBLE_COLON) {
            Error("Expected ',' between module names");
            break;
        } else {
            break;
        }
    }
}

void Parser::ParseUsingDeclaration(NodeRef owner, bool ownerIsPipeline) {
    if (Check(TokenType::IDENTIFIER) &&
        stream->GetType(PeekNext()) == TokenType::ASSIGN) {
        ParseUsingTypeAliasList();
        return;
    }

    ParseUsingModuleList(owner, ownerIsPipeline);
}

void Parser::ParseUsingTypeAliasList() {
    auto parseTypeAliasTarget = [&]() -> std::string {
        if (MatchMask(TokenMasks::CORE_TYPES)) {
            return std::string(stream->GetValue(previous));
        }

        Consume(TokenType::IDENTIFIER, "Expected type name after '='");
        std::string typeName(stream->GetValue(previous));
        if (Match(TokenType::DOUBLE_COLON)) {
            std::string moduleName = typeName;
            Consume(TokenType::IDENTIFIER, "Expected type name after '::'");
            typeName = CanonicalizeModuleQualifiedName(
                moduleName, std::string(stream->GetValue(previous)));
        }

        while (Match(TokenType::BITWISE_XOR)) {
            typeName += "^";
        }
        return typeName;
    };

    while (Check(TokenType::IDENTIFIER)) {
        TokenRef aliasToken = current;
        std::string aliasNameStr(stream->GetValue(current));
        Advance();
        Consume(TokenType::ASSIGN, "Expected '=' after type alias name");

        TokenRef targetToken = current;
        std::string targetNameStr = parseTypeAliasTarget();
        if (targetNameStr.empty()) {
            break;
        }

        TypeInfo targetInfo = ResolveType(targetNameStr);
        if (targetInfo.coreType == CoreType::INVALID) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Unknown type '%s' in using alias",
                     targetNameStr.c_str());
            ErrorAt(targetToken, msg);
        } else {
            ArenaString aliasName = ArenaString::MakeHashOnly(aliasNameStr);
            ArenaString targetName = ArenaString::MakeHashOnly(targetNameStr);
            ReverseLookup::Register(aliasName.nameHash, aliasNameStr.c_str());
            ReverseLookup::Register(targetName.nameHash, targetNameStr.c_str());
            if (!SymbolTable::RegisterTypeAlias(&symbolTable, aliasName, targetName)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Type alias '%s' conflicts with an existing name",
                         aliasNameStr.c_str());
                ErrorAt(aliasToken, msg);
            }
            typeCache.Clear();
        }

        if (Check(TokenType::COMMA)) {
            Advance();
            if (!Check(TokenType::IDENTIFIER)) {
                Error("Expected type alias name after ','");
                break;
            }
        } else {
            Match(TokenType::SEMICOLON);
            break;
        }
    }
}

void Parser::ParseUsingModuleList(NodeRef owner, bool ownerIsPipeline) {
    auto ownerHasImport = [&](u32 moduleNameHash) {
        const ArenaArray<ArenaString>& imports = ownerIsPipeline
            ? ast->GetPipeline(owner).imports
            : ast->GetModule(owner).imports;
        for (u32 i = 0; i < imports.count; i++) {
            if (imports[i].nameHash == moduleNameHash) {
                return true;
            }
        }
        return false;
    };

    auto ownerHasUsing = [&](u32 moduleNameHash) {
        const ArenaArray<ArenaString>& usingImports = ownerIsPipeline
            ? ast->GetPipeline(owner).usingImports
            : ast->GetModule(owner).usingImports;
        for (u32 i = 0; i < usingImports.count; i++) {
            if (usingImports[i].nameHash == moduleNameHash) {
                return true;
            }
        }
        return false;
    };

    while (Check(TokenType::IDENTIFIER)) {
        TokenRef moduleToken = current;
        std::string moduleNameStr(stream->GetValue(current));
        Advance();

        if (Match(TokenType::AS)) {
            ErrorAtPrevious("'using' does not create aliases; import the module with 'as' first");
            if (Check(TokenType::IDENTIFIER)) {
                Advance();
            }
        }

        ArenaString writtenName = ArenaString::MakeHashOnly(moduleNameStr);
        u32 moduleIdx = ResolveModuleIndexByWrittenName(writtenName);

        if (moduleIdx == INVALID_INDEX || moduleIdx >= symbolTable.modules.count) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Module '%s' must be imported before it can be used",
                     moduleNameStr.c_str());
            ErrorAt(moduleToken, msg);
        } else {
            ArenaString moduleName = symbolTable.modules[moduleIdx].name;
            if (!ownerHasImport(moduleName.nameHash)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Module '%s' must be imported before it can be used",
                         moduleNameStr.c_str());
                ErrorAt(moduleToken, msg);
            } else if (!ownerHasUsing(moduleName.nameHash)) {
                if (ownerIsPipeline) {
                    ast->GetPipeline(owner).usingImports.Push(arena, moduleName);
                } else {
                    ast->GetModule(owner).usingImports.Push(arena, moduleName);
                }
            }
        }

        if (Check(TokenType::COMMA)) {
            Advance();
            if (!Check(TokenType::IDENTIFIER)) {
                Error("Expected module name after ','");
                break;
            }
        } else if (Check(TokenType::IDENTIFIER) &&
                   stream->GetType(PeekNext()) != TokenType::DOUBLE_COLON) {
            Error("Expected ',' between module names");
            break;
        } else {
            break;
        }
    }
}

//==============================================================================
// Attribute parsing
//==============================================================================

void Parser::ParseAttributes(NodeRef owner, bool ownerIsPipeline) {
    Consume(TokenType::LEFT_BRACE, "Expected '{' after 'attributes'");
    u8 attrIndex = 0;  // Assign indices by declaration order

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        NodeRef attr = ParseAttributeDecl();
        if (attr.IsValid()) {
            ast->GetAttributeDecl(attr).attributeIndex = attrIndex++;
            if (ownerIsPipeline) {
                ast->GetPipeline(owner).attributes.Push(arena, attr);
            } else {
                ast->GetModule(owner).attributes.Push(arena, attr);
            }
        } else {
            if (panicMode) {
                Synchronize();
                panicMode = false;
            } else {
                if (stream->GetType(current) != TokenType::RIGHT_BRACE && stream->GetType(current) != TokenType::EOF_TOKEN) {
                    Advance();
                } else {
                    break;
                }
            }
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after attributes");

    // Validate first attribute is "position"
    static const u32 POSITION_HASH = Utils::HashStr("position");
    if (ownerIsPipeline) {
        PipelineData& pipelineData = ast->GetPipeline(owner);
        if (pipelineData.attributes.count > 0) {
            NodeRef firstAttr = pipelineData.attributes[0];
            const AttributeDeclData& first = ast->GetAttributeDecl(firstAttr);
            if (first.name.nameHash != POSITION_HASH) {
                Error("First attribute must be 'position'");
            }
        }
    }
}

void Parser::ParseResources(NodeRef owner, bool ownerIsPipeline) {
    Consume(TokenType::LEFT_BRACE, "Expected '{' after 'resources'");
    u8 resourceIndex = 0;

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        NodeRef resource = ParseResourceDecl();
        if (resource.IsValid()) {
            ast->GetResourceDecl(resource).resourceIndex = resourceIndex;
            if (ownerIsPipeline) {
                ast->GetPipeline(owner).resources.Push(arena, resource);

                const ResourceDeclData& decl = ast->GetResourceDecl(resource);
                RegisterParsedResource(decl.name.ToString(sourceBase()),
                                       decl.typeName.ToString(sourceBase()),
                                       resourceIndex);
            } else {
                ast->GetModule(owner).resources.Push(arena, resource);
            }
            resourceIndex++;
        } else {
            if (panicMode) {
                Synchronize();
                panicMode = false;
            } else if (stream->GetType(current) != TokenType::RIGHT_BRACE &&
                       stream->GetType(current) != TokenType::EOF_TOKEN) {
                Advance();
            } else {
                break;
            }
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after resources");
}

void Parser::ParseVariants(NodeRef owner, bool ownerIsPipeline) {
    Consume(TokenType::LEFT_BRACE, "Expected '{' after 'variants'");

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (Match(TokenType::RULES)) {
            ParseVariantRules(owner, ownerIsPipeline);
            continue;
        }

        Consume(TokenType::IDENTIFIER, "Expected variant name");
        ArenaString variantName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));

        bool duplicate = false;
        if (ownerIsPipeline) {
            for (u32 i = 0; i < ast->GetPipeline(owner).variantDecls.count; i++) {
                if (ast->GetPipeline(owner).variantDecls[i].name.nameHash == variantName.nameHash) {
                    duplicate = true;
                    break;
                }
            }
        } else {
            for (u32 i = 0; i < ast->GetModule(owner).variantDecls.count; i++) {
                if (ast->GetModule(owner).variantDecls[i].name.nameHash == variantName.nameHash) {
                    duplicate = true;
                    break;
                }
            }
        }
        if (duplicate) {
            Error(ownerIsPipeline ? "Variant already declared in this pipeline"
                                  : "Variant already declared in this module");
        }

        Consume(TokenType::COLON, "Expected ':' after variant name");

        ArenaString typeName;
        TypeInfo typeInfo = TYPE_INFO(CoreType::INVALID, 0, false);
        u32 enumTypeHash = 0;

        if (Match(TokenType::BOOL)) {
            typeName = ArenaString::MakeHashOnly("bool");
            typeInfo = TYPE_INFO(CoreType::BOOL, 1, false);
        } else {
            Consume(TokenType::IDENTIFIER, "Expected 'bool' or enum type name");
            u32 typeOffset = stream->GetOffset(previous);
            u16 typeLength = stream->GetLength(previous);
            std::string typeNameStr(stream->GetValue(previous));
            bool isQualified = false;
            if (Match(TokenType::DOUBLE_COLON)) {
                Consume(TokenType::IDENTIFIER, "Expected enum type after '::'");
                typeNameStr += "::";
                typeNameStr += std::string(stream->GetValue(previous));
                isQualified = true;
            }
            if (isQualified) {
                typeName = ArenaString::MakeHashOnly(typeNameStr);
                ReverseLookup::Register(typeName.nameHash, typeNameStr.c_str());
            } else {
                typeName = ArenaString::Make(sourceBase(), typeOffset, typeLength);
            }
            enumTypeHash = typeName.nameHash;
        }

        Consume(TokenType::ASSIGN, "Expected '=' after variant type");
        NodeRef defaultExpr = ParseExpression();
        if (defaultExpr.IsNull()) {
            Error("Expected compile-time default value for variant");
        }
        Consume(TokenType::SEMICOLON, "Expected ';' after variant declaration");

        PipelineVariantDeclData decl{};
        decl.name = variantName;
        decl.typeName = typeName;
        decl.typeInfo = typeInfo;
        decl.enumTypeHash = enumTypeHash;
        decl.defaultExpr = defaultExpr;
        decl.defaultResolved = false;
        if (!duplicate) {
            if (ownerIsPipeline) {
                ast->GetPipeline(owner).variantDecls.Push(arena, decl);
            } else {
                ast->GetModule(owner).variantDecls.Push(arena, decl);
            }
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after variants block");
}

void Parser::ParseVariantRules(NodeRef owner, bool ownerIsPipeline) {
    Consume(TokenType::LEFT_BRACE, "Expected '{' after 'rules'");

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        VariantRuleData rule{};

        if (Match(TokenType::REQUIRE)) {
            rule.type = VariantRuleType::Require;
            rule.lhs = ParseExpression();
            Consume(TokenType::ARROW, "Expected '->' in require rule");
            rule.rhs = ParseExpression();
            Consume(TokenType::SEMICOLON, "Expected ';' after require rule");
        } else if (Match(TokenType::CONFLICT)) {
            rule.type = VariantRuleType::Conflict;
            rule.lhs = ParseExpression();
            Consume(TokenType::COMMA, "Expected ',' in conflict rule");
            rule.rhs = ParseExpression();
            Consume(TokenType::SEMICOLON, "Expected ';' after conflict rule");
        } else {
            ErrorAtCurrent("Expected 'require' or 'conflict' inside variant rules");
            Advance();
            continue;
        }

        if (ownerIsPipeline) {
            ast->GetPipeline(owner).variantRules.Push(arena, rule);
        } else {
            ast->GetModule(owner).variantRules.Push(arena, rule);
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after rules block");
}

NodeRef Parser::ParseAttributeDecl() {
    if (!Consume(TokenType::IDENTIFIER, "Expected attribute name")) {
        if (stream->GetType(current) != TokenType::RIGHT_BRACE && stream->GetType(current) != TokenType::EOF_TOKEN) {
            Advance();
        }
        return NodeRef::Null();
    }
    std::string name(stream->GetValue(previous));

    if (!Consume(TokenType::COLON, "Expected ':' after attribute name")) {
        if (stream->GetType(current) != TokenType::RIGHT_BRACE && stream->GetType(current) != TokenType::EOF_TOKEN) {
            Advance();
        }
        return NodeRef::Null();
    }

    if (!MatchMask(TokenMasks::ALL)) {
        Error("Expected type after ':'");
        if (stream->GetType(current) != TokenType::RIGHT_BRACE && stream->GetType(current) != TokenType::EOF_TOKEN) {
            Advance();
        }
        return NodeRef::Null();
    }

    SourceLocation loc = getLocation(stream->GetOffset(previous));
    NodeRef attr = ASTFactory::MakeAttributeDecl(ast, name, std::string(stream->GetValue(previous)), loc.line, loc.column);

    // Parse decorators
    while (Match(TokenType::AT)) {
        u32 decoHash = Utils::HashStr(stream->GetValue(previous).data(), static_cast<u16>(stream->GetLength(previous)));

        if (decoHash == Utils::HashStr("compressed")) {
            if (!Consume(TokenType::LEFT_PAREN, "Expected '(' after @compressed")) {
                break;
            }

            std::string compressionValue;
            while (!Check(TokenType::RIGHT_PAREN) && !Check(TokenType::EOF_TOKEN)) {
                ProgressGuard _pg_(this);
                std::string_view segment = stream->GetValue(current);
                compressionValue.append(segment.data(), segment.size());
                Advance();
            }

            if (compressionValue.empty()) {
                Error("Expected compression type");
                break;
            }

            ast->GetAttributeDecl(attr).compression = ArenaString::MakeHashOnly(compressionValue.c_str());

            if (!Consume(TokenType::RIGHT_PAREN, "Expected ')' after compression type")) {
                break;
            }
        } else if (decoHash == Utils::HashStr("instance")) {
            ast->GetAttributeDecl(attr).isInstance = true;
        } else {
            Error("Unknown decorator");
        }

        if (panicMode) break;
    }

    // Note: attributeIndex is assigned by ParseAttributes() based on declaration order

    return attr;
}

NodeRef Parser::ParseResourceDecl() {
    if (!Consume(TokenType::IDENTIFIER, "Expected resource name")) {
        if (stream->GetType(current) != TokenType::RIGHT_BRACE && stream->GetType(current) != TokenType::EOF_TOKEN) {
            Advance();
        }
        return NodeRef::Null();
    }

    std::string name(stream->GetValue(previous));
    SourceLocation loc = getLocation(stream->GetOffset(previous));

    if (!Consume(TokenType::COLON, "Expected ':' after resource name")) {
        if (stream->GetType(current) != TokenType::RIGHT_BRACE && stream->GetType(current) != TokenType::EOF_TOKEN) {
            Advance();
        }
        return NodeRef::Null();
    }

    std::string typeName;
    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        if (Check(TokenType::SEMICOLON)) {
            Advance();
            break;
        }
        if (!typeName.empty() && Check(TokenType::IDENTIFIER) && stream->GetType(PeekNext()) == TokenType::COLON) {
            break;
        }
        std::string_view segment = stream->GetValue(current);
        typeName.append(segment.data(), segment.size());
        Advance();
    }

    if (typeName.empty()) {
        Error("Expected resource type after ':'");
        return NodeRef::Null();
    }

    ReverseLookup::Register(Utils::HashStr(name.c_str()), name.c_str());
    ReverseLookup::Register(Utils::HashStr(typeName.c_str()), typeName.c_str());
    return ASTFactory::MakeResourceDecl(ast, name, typeName, loc.line, loc.column);
}

//==============================================================================
// Pass parsing
//==============================================================================

static bool IsValidFragmentOutputCoreType(CoreType type) {
    switch (type) {
        case CoreType::FLOAT:
        case CoreType::FLOAT2:
        case CoreType::FLOAT3:
        case CoreType::FLOAT4:
        case CoreType::INT:
        case CoreType::INT2:
        case CoreType::INT3:
        case CoreType::INT4:
        case CoreType::UINT:
        case CoreType::UINT2:
        case CoreType::UINT3:
        case CoreType::UINT4:
            return true;
        default:
            return false;
    }
}

NodeRef Parser::ParsePass() {
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    Consume(TokenType::STRING, "Expected pass name in quotes");
    ArenaString passName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));

    NodeRef pass = ASTFactory::MakePass(ast, passName.ToString(sourceBase()), loc.line, loc.column);

    if (Match(TokenType::ASSIGN)) {
        NodeRef expr = ParseExpression();
        if (expr.IsNull()) {
            Error("Expected pass_block function call after '='");
        } else if (expr.Type() != ASTNodeType::FUNCTION_CALL) {
            Error("pass_block instantiation must use a direct function call");
        } else {
            ast->GetPass(pass).isPassBlockInstance = true;
            ast->GetPass(pass).passBlockCall = expr;
        }

        if (Match(TokenType::LEFT_BRACE)) {
            ParsePassBlockInstantiationBody(pass);
            Consume(TokenType::RIGHT_BRACE, "Expected '}' after pass_block mappings");
            Match(TokenType::SEMICOLON);
        } else {
            Consume(TokenType::SEMICOLON, "Expected ';' after pass_block instantiation");
        }

        return pass;
    }

    currentPass = pass;

    // Enter a pass scope for pass-local symbols
    SymbolTable::EnterScope(&symbolTable);

    Consume(TokenType::LEFT_BRACE, "Expected '{' after pass name");
    ParsePassBody(pass);
    Consume(TokenType::RIGHT_BRACE, "Expected '}' after pass body");

    SymbolTable::ExitScope(&symbolTable);
    currentPass = NodeRef::Null();

    return pass;
}

void Parser::ParsePassBlockInstantiationBody(NodeRef pass) {
    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (Match(TokenType::USE)) {
            if (Match(TokenType::ATTRIBUTES)) {
                ParsePassBlockBindingList(ast->GetPass(pass).attributeBindings, "attributes");
            } else if (Match(TokenType::RESOURCES)) {
                ParsePassBlockBindingList(ast->GetPass(pass).resourceBindings, "resources");
            } else {
                Error("Expected 'attributes' or 'resources' after 'use'");
                Advance();
            }
        } else if (Match(TokenType::VARIANTS)) {
            ParsePassBlockBindingList(ast->GetPass(pass).variantBindings, "variants");
        } else {
            ErrorAtCurrent("Expected 'use attributes', 'use resources', or 'variants' in pass_block mapping");
            Advance();
        }
    }
}

void Parser::ParsePassBlockBindingList(ArenaArray<PassBlockBindingData>& bindings,
                                       const char* groupName) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Expected '{' after %s mapping", groupName);
    Consume(TokenType::LEFT_BRACE, msg);

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        Consume(TokenType::IDENTIFIER, "Expected local interface name");
        ArenaString localName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));
        ArenaString targetName = localName;

        if (Match(TokenType::ASSIGN)) {
            Consume(TokenType::IDENTIFIER, "Expected target interface name after '='");
            targetName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));
        }

        PassBlockBindingData binding{};
        binding.localName = localName;
        binding.targetName = targetName;
        bindings.Push(arena, binding);

        while (Match(TokenType::COMMA) || Match(TokenType::SEMICOLON)) {
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after pass_block mapping list");
}

void Parser::ParsePassBody(NodeRef pass) {
    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (Match(TokenType::USE)) {
            if (Check(TokenType::ATTRIBUTES)) {
                if (!ast->GetPass(pass).computeShader.IsNull()) {
                    Error("Compute passes cannot use attributes");
                }
                ParseUseAttributes(pass);
            } else if (Check(TokenType::RESOURCES)) {
                ParseUseResources(pass);
            } else {
                Error("Expected 'attributes' or 'resources' after 'use'");
            }
        } else if (Match(TokenType::OUTPUTS)) {
            if (ast->GetPass(pass).hasFragmentOutputs) {
                Error("Only one outputs block is allowed per pass");
                continue;
            }
            ParsePassOutputs(pass);
        } else if (Match(TokenType::CONST)) {
            SourceLocation loc = getLocation(stream->GetOffset(previous));
            if (!MatchMask(TokenMasks::CORE_TYPES)) {
                Error("Expected type after 'const'");
                Advance();
                continue;
            }

            TokenType varType = static_cast<TokenType>(stream->GetType(previous));
            std::string typeStr(stream->GetValue(previous));
            Consume(TokenType::IDENTIFIER, "Expected constant name");
            std::string constName(stream->GetValue(previous));

            Consume(TokenType::ASSIGN, "const variables must be initialized");

            NodeRef value = ParseExpression();
            if (!value.IsValid()) {
                Error("Expected initializer expression for const");
                Advance();
                continue;
            }

            Consume(TokenType::SEMICOLON, "Expected ';'");

            ArenaString constNameStr = ArenaString::MakeHashOnly(constName);
            NodeRef varDecl = ASTFactory::MakeVariableDecl(ast, constNameStr,
                ArenaString::MakeHashOnly(typeStr),
                value, true, loc.line, loc.column);
            ast->GetPass(pass).consts.Push(arena, varDecl);
            Symbol* sym = SymbolTable::AddSymbol(&symbolTable, constNameStr, SymbolKind::VARIABLE);
            if (!sym) {
                Error("Variable already declared in this scope");
                continue;
            }

            VariableData& varData = symbolTable.variables[sym->index];
            varData.typeInfo = GetTypeInfoFromToken(varType);
            varData.isConst = true;
            varData.isEval = false;
            varData.constExpr = value;

            EvalStateSoA evalState;
            CompileTimeEvaluatorSoA::Init(&evalState, this, ast, &context->evalCache, ast->arena);
            LiteralValue constValue;
            if (CompileTimeEvaluatorSoA::CanEvaluateNode(&evalState, value) &&
                CompileTimeEvaluatorSoA::EvaluateNode(&evalState, value, &constValue)) {
                bool typeOk = false;
                switch (varData.typeInfo.coreType) {
                    case CoreType::INT:
                        typeOk = (constValue.type == LiteralValue::INT);
                        break;
                    case CoreType::UINT:
                        typeOk = (constValue.type == LiteralValue::UINT);
                        break;
                    case CoreType::FLOAT:
                        if (constValue.type == LiteralValue::INT) {
                            constValue.floatValue = static_cast<float>(constValue.intValue);
                            constValue.type = LiteralValue::FLOAT;
                        }
                        typeOk = (constValue.type == LiteralValue::FLOAT);
                        break;
                    case CoreType::BOOL:
                        typeOk = (constValue.type == LiteralValue::BOOL);
                        break;
                    default:
                        break;
                }
                if (typeOk) {
                    varData.isEval = true;
                    varData.hasEvalValue = true;
                    varData.evalValue = constValue;
                }
            }
        } else if (Match(TokenType::VERTEX)) {
            if (!ast->GetPass(pass).computeShader.IsNull()) {
                Error("Compute passes cannot include vertex/fragment stages");
                Advance();
                continue;
            }
            if (Match(TokenType::ASSIGN)) {
                if (Check(TokenType::STRING)) {
                    // Pass inheritance: vertex = "PassName".vertex
                    NodeRef inheritedStage = ParseShaderStageInheritance(ASTNodeType::VERTEX_STAGE);
                    ast->GetPass(pass).vertexShader = inheritedStage;
                } else {
                    // Expression assignment: vertex = funcCall() or vertex = cond ? f1() : f2()
                    NodeRef exprStage = ParseShaderStageExpression(ASTNodeType::VERTEX_STAGE);
                    ast->GetPass(pass).vertexShader = exprStage;
                }
            } else {
                ast->GetPass(pass).vertexShader = ParseShaderStage(ASTNodeType::VERTEX_STAGE);
            }
        } else if (Match(TokenType::FRAGMENT)) {
            if (!ast->GetPass(pass).computeShader.IsNull()) {
                Error("Compute passes cannot include vertex/fragment stages");
                Advance();
                continue;
            }
            if (Match(TokenType::ASSIGN)) {
                if (Match(TokenType::NULL_TOKEN)) {
                    ast->GetPass(pass).fragmentShader = NodeRef::Null();
                } else if (Check(TokenType::STRING)) {
                    // Pass inheritance: fragment = "PassName".fragment
                    NodeRef inheritedStage = ParseShaderStageInheritance(ASTNodeType::FRAGMENT_STAGE);
                    ast->GetPass(pass).fragmentShader = inheritedStage;
                } else {
                    // Expression assignment: fragment = funcCall() or fragment = cond ? f1() : f2()
                    NodeRef exprStage = ParseShaderStageExpression(ASTNodeType::FRAGMENT_STAGE);
                    ast->GetPass(pass).fragmentShader = exprStage;
                }
            } else {
                ast->GetPass(pass).fragmentShader = ParseShaderStage(ASTNodeType::FRAGMENT_STAGE);
            }
        } else if (Match(TokenType::COMPUTE)) {
            if (!ast->GetPass(pass).vertexShader.IsNull() || !ast->GetPass(pass).fragmentShader.IsNull()) {
                Error("Compute passes cannot include vertex/fragment stages");
                Advance();
                continue;
            }
            if (ast->GetPass(pass).usedAttributes.count > 0) {
                Error("Compute passes cannot use attributes");
            }
            if (!ast->GetPass(pass).computeShader.IsNull()) {
                Error("Only one compute block is allowed per pass");
            }
            NodeRef computeStage = ParseComputeStage();
            if (computeStage.IsValid()) {
                ast->GetPass(pass).computeShader = computeStage;
            }
        } else if (IsFunctionDeclStart()) {
            // Pass-scoped function declaration: name :: (...) -> type { }
            NodeRef function = ParseFunction();
            if (function.IsValid()) {
                ast->GetPass(pass).functions.Push(arena, function);
            }
        } else {
            ErrorAtCurrent("Expected 'use', 'outputs', 'vertex', 'fragment', 'compute', or function declaration in pass body");
            Advance();
        }
    }
}

void Parser::ParsePassOutputs(NodeRef pass) {
    PassData& passData = ast->GetPass(pass);
    passData.hasFragmentOutputs = true;

    Consume(TokenType::LEFT_BRACE, "Expected '{' after outputs");

    u32 nextLocation = 0;
    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);

        if (!Consume(TokenType::IDENTIFIER, "Expected fragment output name")) {
            if (stream->GetType(current) != TokenType::RIGHT_BRACE &&
                stream->GetType(current) != TokenType::EOF_TOKEN) {
                Advance();
            }
            continue;
        }
        ArenaString name = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));

        if (!Consume(TokenType::COLON, "Expected ':' after fragment output name")) {
            if (stream->GetType(current) != TokenType::RIGHT_BRACE &&
                stream->GetType(current) != TokenType::EOF_TOKEN) {
                Advance();
            }
            continue;
        }

        if (!MatchMask(TokenMasks::CORE_TYPES)) {
            Error("Expected scalar or vector numeric type after ':'");
            if (stream->GetType(current) != TokenType::RIGHT_BRACE &&
                stream->GetType(current) != TokenType::EOF_TOKEN) {
                Advance();
            }
            continue;
        }

        TokenType typeToken = static_cast<TokenType>(stream->GetType(previous));
        ArenaString typeName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));
        TypeInfo typeInfo = GetTypeInfoFromToken(typeToken);

        if (!IsValidFragmentOutputCoreType(typeInfo.coreType)) {
            Error("Fragment outputs must use float/int/uint scalar or vector types");
        }

        u32 location = nextLocation;

        while (Match(TokenType::AT)) {
            std::string decorator(stream->GetValue(previous));
            if (decorator == "location") {
                Consume(TokenType::LEFT_PAREN, "Expected '(' after @location");
                Consume(TokenType::NUMBER, "Expected numeric location");
                location = SafeParseU32(stream->GetValue(previous), 0);
                Consume(TokenType::RIGHT_PAREN, "Expected ')' after @location");
            } else {
                Error("Unknown fragment output decorator");
            }
        }

        if (location >= FragmentOutput::MAX_COLOR_ATTACHMENTS) {
            Error("Fragment output location exceeds supported color attachment count");
        }

        static const u32 HASH_DEPTH = Utils::HashStr("depth");
        if (name.nameHash == HASH_DEPTH) {
            Error("output.depth is a builtin depth output and is not declared in outputs");
        }

        bool duplicateName = false;
        bool duplicateLocation = false;
        for (u32 i = 0; i < passData.fragmentOutputs.count; i++) {
            if (passData.fragmentOutputs[i].name.nameHash == name.nameHash) {
                duplicateName = true;
            }
            if (passData.fragmentOutputs[i].location == location) {
                duplicateLocation = true;
            }
        }
        if (duplicateName) {
            Error("Duplicate fragment output name");
        }
        if (duplicateLocation) {
            Error("Duplicate fragment output location");
        }

        if (!duplicateName && !duplicateLocation &&
            location < FragmentOutput::MAX_COLOR_ATTACHMENTS &&
            name.nameHash != HASH_DEPTH &&
            IsValidFragmentOutputCoreType(typeInfo.coreType)) {
            FragmentOutputDeclData decl{};
            decl.name = name;
            decl.typeName = typeName;
            decl.typeInfo = typeInfo;
            decl.location = static_cast<u8>(location);
            passData.fragmentOutputs.Push(arena, decl);
        }

        if (location >= nextLocation) {
            nextLocation = location + 1;
        }
        while (Match(TokenType::COMMA) || Match(TokenType::SEMICOLON)) {
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after outputs");
}

void Parser::ParseUseAttributes(NodeRef pass) {
    Consume(TokenType::ATTRIBUTES, "Expected 'attributes'");
    Consume(TokenType::LEFT_BRACE, "Expected '{'");

    while (!Check(TokenType::RIGHT_BRACE)) {
        ProgressGuard _pg_(this);
        Consume(TokenType::IDENTIFIER, "Expected attribute name");
        ArenaString attrName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));
        bool isOptional = Match(TokenType::QUESTION);

        u8 idx = 0xFF;
        const AttributeDeclData* decl = nullptr;
        if (!currentPipeline.IsNull()) {
            decl = LookupPipelineAttributeDecl(attrName);
        } else if (!currentModule.IsNull()) {
            decl = LookupModuleAttributeDecl(currentModule, attrName);
        }
        if (decl) {
            idx = decl->attributeIndex;
        }
        if (idx == 0xFF) { Error("Unknown attribute in 'use attributes'"); break; }

        ast->GetPass(pass).usedAttributes.Push(arena, attrName);

        // Set the optional bit for this attribute
        if (isOptional && idx < 32) {
            ast->GetPass(pass).optionalAttributesMask |= (1u << idx);
        }

        if (!Match(TokenType::COMMA)) break;
    }
    Consume(TokenType::RIGHT_BRACE, "Expected '}'");
}

void Parser::ParseUseResources(NodeRef pass) {
    Consume(TokenType::RESOURCES, "Expected 'resources'");
    Consume(TokenType::LEFT_BRACE, "Expected '{'");

    while (!Check(TokenType::RIGHT_BRACE)) {
        ProgressGuard _pg_(this);
        Consume(TokenType::IDENTIFIER, "Expected resource name");
        ArenaString resourceName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));
        bool isOptional = Match(TokenType::QUESTION);

        const ResourceDeclData* pipelineDecl = LookupPipelineResourceDecl(resourceName);
        const ResourceDeclData* moduleDecl = currentPipeline.IsNull() && currentModule.IsValid()
            ? LookupModuleResourceDecl(currentModule, resourceName)
            : nullptr;
        const bool pipelineOwnsResources = PipelineDeclaresResources();
        if (pipelineOwnsResources) {
            if (!pipelineDecl) {
                Error("Unknown resource in 'use resources'");
            } else {
                ast->GetPass(pass).usedResources.Push(arena, resourceName);

                if (isOptional && pipelineDecl->resourceIndex < 32) {
                    ast->GetPass(pass).optionalResourcesMask |= (1u << pipelineDecl->resourceIndex);
                }
            }
        } else if (moduleDecl) {
            ast->GetPass(pass).usedResources.Push(arena, resourceName);

            if (isOptional && moduleDecl->resourceIndex < 32) {
                ast->GetPass(pass).optionalResourcesMask |= (1u << moduleDecl->resourceIndex);
            }
        } else {
            Symbol* sym = SymbolTable::LookupResource(&symbolTable, resourceName);
            if (!sym || sym->kind != SymbolKind::RESOURCE) {
                Error("Unknown resource in 'use resources'");
            } else {
                ast->GetPass(pass).usedResources.Push(arena, resourceName);
            }
        }

        if (!Match(TokenType::COMMA)) break;
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}'");
}

//==============================================================================
// Compute graph parsing
//==============================================================================

NodeRef Parser::ParseComputeGraph() {
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    NodeRef graph = ASTFactory::MakeComputeGraph(ast, loc.line, loc.column);

    Consume(TokenType::LEFT_BRACE, "Expected '{' after compute_graph");

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (Match(TokenType::NODE)) {
            ComputeGraphNode node = ParseComputeGraphNode();
            ast->GetComputeGraph(graph).nodes.Push(arena, node);
        } else {
            ErrorAtCurrent("Expected 'node' in compute_graph");
            Advance();
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after compute_graph");
    return graph;
}

ComputeGraphNode Parser::ParseComputeGraphNode() {
    Consume(TokenType::STRING, "Expected node name in quotes");
    ArenaString passName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));

    ComputeGraphNode node;
    node.passName = passName;
    node.inputs.Init(arena, 4);
    node.outputs.Init(arena, 4);

    Consume(TokenType::LEFT_BRACE, "Expected '{' after node name");

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (Match(TokenType::INPUTS)) {
            ParseComputeGraphInputs(node);
        } else if (Match(TokenType::OUTPUTS)) {
            ParseComputeGraphOutputs(node);
        } else {
            ErrorAtCurrent("Expected 'inputs' or 'outputs' in node");
            Advance();
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after node body");
    return node;
}

void Parser::ParseComputeGraphInputs(ComputeGraphNode& node) {
    Consume(TokenType::LEFT_BRACE, "Expected '{' after inputs");

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        Consume(TokenType::IDENTIFIER, "Expected resource name in inputs");
        ArenaString name = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));

        GraphResourceRef ref;
        ref.name = name;
        ref.access = ResourceAccessMode::ReadOnly;

        if (Match(TokenType::READONLY)) {
            ref.access = ResourceAccessMode::ReadOnly;
        } else if (Match(TokenType::READWRITE)) {
            ref.access = ResourceAccessMode::ReadWrite;
        } else if (Match(TokenType::WRITEONLY)) {
            ref.access = ResourceAccessMode::WriteOnly;
        }

        node.inputs.Push(arena, ref);

        if (Match(TokenType::COMMA)) {
            continue;
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after inputs");
}

void Parser::ParseComputeGraphOutputs(ComputeGraphNode& node) {
    Consume(TokenType::LEFT_BRACE, "Expected '{' after outputs");

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        Consume(TokenType::IDENTIFIER, "Expected resource name in outputs");
        ArenaString name = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));
        node.outputs.Push(arena, name);

        if (Match(TokenType::COMMA)) {
            continue;
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after outputs");
}

//==============================================================================
// Shader stage parsing
//==============================================================================

NodeRef Parser::ParseComputeStage() {
    Consume(TokenType::STRING, "Expected compute block name in quotes");
    ArenaString computeName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));

    Consume(TokenType::LEFT_BRACKET, "Expected '[' after compute block name");

    auto parseSize = [&](const char* message) -> u32 {
        Consume(TokenType::NUMBER, message);
        std::string_view num = stream->GetValue(previous);
        if (num.find('.') != std::string_view::npos || num.find('e') != std::string_view::npos ||
            num.find('E') != std::string_view::npos) {
            Error("Workgroup size must be an integer literal");
            return 1;
        }
        return SafeParseU32(num, 0);
    };

    u32 sizeX = parseSize("Expected workgroup size X");
    Consume(TokenType::COMMA, "Expected ',' after workgroup size X");
    u32 sizeY = parseSize("Expected workgroup size Y");
    Consume(TokenType::COMMA, "Expected ',' after workgroup size Y");
    u32 sizeZ = parseSize("Expected workgroup size Z");
    Consume(TokenType::RIGHT_BRACKET, "Expected ']' after workgroup size");

    if (sizeX == 0 || sizeY == 0 || sizeZ == 0) {
        Error("Workgroup size components must be greater than 0");
    }
    if (sizeX * sizeY * sizeZ > 1024u) {
        Error("Workgroup size exceeds maximum (1024 invocations)");
    }

    NodeRef stage = ParseShaderStage(ASTNodeType::COMPUTE_STAGE);
    if (stage.IsValid()) {
        ShaderStageData& stageData = ast->GetShaderStage(stage);
        stageData.name = computeName;
        stageData.workgroupSizeX = sizeX;
        stageData.workgroupSizeY = sizeY;
        stageData.workgroupSizeZ = sizeZ;
    }
    return stage;
}

NodeRef Parser::ParseShaderStage(ASTNodeType stageType) {
    ShaderStage oldStage = currentShaderStage;
    bool wasInShader = inShaderStage;

    inShaderStage = true;
    currentShaderStage = (stageType == ASTNodeType::VERTEX_STAGE) ? ShaderStage::Vertex :
                         (stageType == ASTNodeType::FRAGMENT_STAGE) ? ShaderStage::Fragment :
                         ShaderStage::Compute;

    Consume(TokenType::LEFT_BRACE, "Expected '{' after shader stage");

    SourceLocation loc = getLocation(stream->GetOffset(previous));
    NodeRef body = ASTFactory::MakeBlock(ast, loc.line, loc.column);

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        NodeRef stmt = ParseStatement();
        if (stmt.IsValid()) {
            ast->GetBlock(body).statements.Push(arena, stmt);
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after shader stage body");

    NodeRef stage = ASTFactory::MakeShaderStage(ast, stageType, body, loc.line, loc.column);

    currentShaderStage = oldStage;
    inShaderStage = wasInShader;
    return stage;
}

NodeRef Parser::ParseShaderStageInheritance(ASTNodeType stageType) {
    // Parse: "PassName".vertex or "PassName".fragment
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    
    Consume(TokenType::STRING, "Expected pass name in quotes for shader inheritance");
    std::string inheritFromPass(stream->GetValue(previous));
    // Remove quotes from string literal
    if (inheritFromPass.size() >= 2 && inheritFromPass.front() == '"' && inheritFromPass.back() == '"') {
        inheritFromPass = inheritFromPass.substr(1, inheritFromPass.size() - 2);
    }
    
    Consume(TokenType::DOT, "Expected '.' after pass name");
    
    // Expect 'vertex' or 'fragment'
    TokenType expectedStage = (stageType == ASTNodeType::VERTEX_STAGE) ? TokenType::VERTEX : TokenType::FRAGMENT;
    const char* stageName = (stageType == ASTNodeType::VERTEX_STAGE) ? "vertex" : "fragment";
    
    if (!Match(expectedStage)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Expected '%s' after '.' for shader inheritance", stageName);
        Error(msg);
        return NodeRef::Null();
    }
    
    // Create a shader stage node that references another pass
    // For now, create an empty block with a special marker
    NodeRef body = ASTFactory::MakeBlock(ast, loc.line, loc.column);
    NodeRef stage = ASTFactory::MakeShaderStage(ast, stageType, body, loc.line, loc.column);
    
    // Store the inheritance info
    ast->GetShaderStage(stage).inheritsFrom = ArenaString::MakeHashOnly(inheritFromPass);
    ast->GetShaderStage(stage).isInherited = true;

    return stage;
}

NodeRef Parser::ParseShaderStageExpression(ASTNodeType stageType) {
    // Parse: funcCall() or cond ? funcA() : funcB()
    // The expression must resolve to a shader stage at compile-time
    SourceLocation loc = getLocation(stream->GetOffset(current));

    // Parse the expression (function call or ternary)
    NodeRef expr = ParseExpression();

    if (!expr.IsValid()) {
        Error("Expected expression for shader stage assignment");
        return NodeRef::Null();
    }

    // Create a deferred shader stage node
    NodeRef stage = ASTFactory::MakeShaderStage(ast, stageType, NodeRef::Null(), loc.line, loc.column);

    // Mark as deferred and store the expression for later resolution
    ast->GetShaderStage(stage).isDeferred = true;
    ast->GetShaderStage(stage).deferredExpr = expr;

    return stage;
}

//==============================================================================
// Block and statement parsing
//==============================================================================


} // namespace BWSL
