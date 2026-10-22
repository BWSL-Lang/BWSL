// Part of bwsl_parser_soa.cpp. Include from that file only.
// Pipeline, import, attribute, resource, pass, and shader stage declarations.
#pragma once
#include "bwsl_parser_soa.cpp"

//==============================================================================
// Main parsing function
//==============================================================================

namespace BWSL {

NodeRef Parser::ParseDocument() {
    TokenRef documentStart = current;
    TokenRef previousStart = previous;

    // First collect file-scope modules so pipelines and submodules can refer to
    // modules declared later in the same BWSL document.
    while (!Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (Match(TokenType::MODULE)) {
            NodeRef module = ParseModule();
            if (module.IsValid()) ast->documentRoots.Push(arena, module);
        } else if (Check(TokenType::SUBMODULE) || Check(TokenType::PIPELINE)) {
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

    // Then fold file-scope submodules into their parent modules before
    // pipelines resolve imports and qualified references.
    while (!Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (Match(TokenType::SUBMODULE)) {
            (void)ParseSubmodule();
        } else if (Check(TokenType::MODULE) || Check(TokenType::PIPELINE)) {
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
                ast->documentRoots.Push(arena, pipeline);
            }
        } else if (Check(TokenType::MODULE) || Check(TokenType::SUBMODULE)) {
            SkipBracedDeclaration(false);
        } else {
            ErrorAtCurrent("Expected file-scope 'module', 'submodule', or 'pipeline' declaration");
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

    while (!Check(TokenType::LEFT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        if (Check(TokenType::SEMICOLON) || Check(TokenType::RIGHT_BRACE)) {
            return;
        }
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
    const u32 firstMemberAccess = ast->memberAccesses.count;
    const u32 firstFunctionCall = ast->functionCalls.count;
    TokenRef declToken = current;  // The PIPELINE keyword; a doc block precedes it
    SourceLocation loc = getLocation(stream->GetOffset(current));
    u32 line = loc.line;
    u32 col = loc.column;

    Consume(TokenType::PIPELINE, "Expected 'pipeline' keyword");
    Consume(TokenType::IDENTIFIER, "Expected pipeline name");

    NodeRef pipeline = ASTFactory::MakePipeline(ast, std::string(stream->GetValue(previous)), line, col);
    AttachDocComment(pipeline, declToken);
    {
        SourceLocation nameLoc = getLocation(stream->GetOffset(previous));
        ast->SetNamePosition(pipeline, nameLoc.line, nameLoc.column);
    }
    RecordDeclarationSource(pipeline);
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
            SourceLocation constLoc = getLocation(stream->GetOffset(previous));
            if (!MatchMask(TokenMasks::CORE_TYPES)) {
                Error("Expected type after 'const'");
                Advance();
                continue;
            }

            TokenType varType = static_cast<TokenType>(stream->GetType(previous));
            std::string typeStr(stream->GetValue(previous));
            SourceLocation typeLoc = getLocation(stream->GetOffset(previous));
            if (Match(TokenType::LEFT_BRACKET)) {
                NodeRef arrayDecl = ParseArrayDeclaration(TokenTypeToReturnType(varType),
                                                           StorageClass::Default, true);
                if (arrayDecl.IsValid()) {
                    ast->IndexPosition(arrayDecl, constLoc.line, constLoc.column);
                    ast->GetVariableDecl(arrayDecl).typePosition =
                        AST::PackPosition(typeLoc.line, typeLoc.column);
                    ast->GetPipeline(pipeline).consts.Push(arena, arrayDecl);
                }
                continue;
            }
            Consume(TokenType::IDENTIFIER, "Expected constant name");
            SourceLocation nameLoc = getLocation(stream->GetOffset(previous));
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
            NodeRef constDecl = ASTFactory::MakeVariableDecl(ast, constNameStr,
                ArenaString::MakeHashOnly(typeStr), value, true, constLoc.line, constLoc.column);
            ast->GetVariableDecl(constDecl).typePosition = AST::PackPosition(typeLoc.line, typeLoc.column);
            ast->GetVariableDecl(constDecl).namePosition = AST::PackPosition(nameLoc.line, nameLoc.column);
            ast->GetPipeline(pipeline).consts.Push(arena, constDecl);
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
                // Duplicate pass names would emit to the same output file
                // names, silently clobbering each other on disk.
                const ArenaString &passName = ast->GetPass(pass).name;
                for (u32 i = 0; i < ast->GetPipeline(pipeline).passes.count; i++) {
                    NodeRef existing = ast->GetPipeline(pipeline).passes[i];
                    if (ast->GetPass(existing).name.nameHash == passName.nameHash) {
                        Error("Duplicate pass name in pipeline");
                        break;
                    }
                }
                ast->GetPipeline(pipeline).passes.Push(arena, pass);
            }
        } else if (Match(TokenType::EVAL)) {
            NodeRef declaration = ParseEvalStatement();
            if (declaration.Type() == ASTNodeType::FUNCTION)
                ast->GetPipeline(pipeline).functions.Push(arena, declaration);
        } else if (Match(TokenType::ENUM)) {
            NodeRef enumDecl = ParseEnum();
            if (enumDecl.IsValid()) {
                ast->GetPipeline(pipeline).enums.Push(arena, enumDecl);
            }
        } else if (Match(TokenType::MODULE)) {
            ErrorAtPrevious("Module declarations must be declared at file scope, outside pipeline blocks");
            SkipBracedDeclaration(true);
        } else if (Match(TokenType::SUBMODULE)) {
            ErrorAtPrevious("Submodule declarations must be declared at file scope, outside pipeline blocks");
            SkipBracedDeclaration(true);
        } else if (Match(TokenType::STRUCT)) {
            // Top-level struct. Registered in the symbol table for type
            // resolution; also kept on the pipeline node so AST consumers
            // (e.g. -ast-json for IDE integration) can reach it.
            NodeRef structNode = ParseStruct();
            if (structNode.IsValid()) {
                ast->GetPipeline(pipeline).structs.Push(arena, structNode);
            }
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

    if (Consume(TokenType::RIGHT_BRACE, "Expected '}' after pipeline body")) {
        MarkNodeEndAtPreviousToken(pipeline);
    }
    RecordMemberSources(pipeline);
    ValidateResourceTypes(pipeline, true);
    FinalizeParsedResourceBindings(pipeline, firstMemberAccess, firstFunctionCall);

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
        TokenRef moduleToken = current;
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
        bool reportedConflict = false;

        if (moduleIdx == INVALID_INDEX) {
            std::string conflictingPath;
            bool hasEmbeddedDiskConflict =
                FindConflictingDiskModule(moduleNameStr, &conflictingPath);
            bool registered = false;
            if (hasEmbeddedDiskConflict) {
                char msg[512];
                snprintf(msg, sizeof(msg),
                         "Module name '%s' is reserved by the embedded standard library. "
                         "A different module file was found at '%s', but reserved standard-library names cannot be overridden or imported with an alias. "
                         "Rename your module and file to a non-standard-library name, for example 'My%s', then import that name. "
                         "Use 'import %s' for the embedded standard-library module.",
                         moduleNameStr.c_str(), conflictingPath.c_str(),
                         moduleNameStr.c_str(), moduleNameStr.c_str());
                ErrorAt(moduleToken, msg);
                reportedConflict = true;
            } else {
                registered = TryRegisterModule(moduleNameStr);
            }

            if (registered) {
                moduleIdx = SymbolTable::FindModuleByHash(&symbolTable, moduleHash);
            }
        }

        if (moduleIdx != INVALID_INDEX) {
            ArenaString moduleName = ArenaString::MakeHashOnly(moduleNameStr);
            SourceLocation moduleLoc = getLocation(stream->GetOffset(moduleToken));
            u32 modulePosition = AST::PackPosition(moduleLoc.line, moduleLoc.column);
            if (ownerIsPipeline) {
                ast->GetPipeline(owner).imports.Push(arena, moduleName);
                ast->GetPipeline(owner).importPositions.Push(arena, modulePosition);
            } else {
                ast->GetModule(owner).imports.Push(arena, moduleName);
                ast->GetModule(owner).importPositions.Push(arena, modulePosition);
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
        } else if (!reportedConflict) {
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
        ParseUsingTypeAliasList(owner);
        return;
    }

    ParseUsingModuleList(owner, ownerIsPipeline);
}

void Parser::ParseUsingTypeAliasList(NodeRef owner) {
    auto parseTypeAliasTarget = [&]() -> std::string {
        if (MatchMask(TokenMasks::CORE_TYPES)) {
            return std::string(stream->GetValue(previous));
        }

        Consume(TokenType::IDENTIFIER, "Expected type name after '='");
        std::string typeName(stream->GetValue(previous));
        if (Match(TokenType::DOUBLE_COLON)) {
            std::string moduleName = typeName;
            Consume(TokenType::IDENTIFIER, "Expected type name after '::'");
            RecordTypeQualifierBeforeTypeName();
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
            } else {
                const SourceLocation loc = getLocation(stream->GetOffset(targetToken));
                ast->typeAliases.Push(arena, {owner, aliasName, targetName,
                    AST::PackPosition(loc.line, loc.column),
                    ast->InternSourceFile(currentSourceName.c_str())});
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
                SourceLocation moduleLoc = getLocation(stream->GetOffset(moduleToken));
                ModuleNameSite site{};
                site.position = AST::PackPosition(moduleLoc.line, moduleLoc.column);
                site.name = ArenaString::Make(sourceBase(), stream->GetOffset(moduleToken),
                                              stream->GetLength(moduleToken));
                site.moduleNameHash = moduleName.nameHash;
                if (ownerIsPipeline) {
                    ast->GetPipeline(owner).usingImports.Push(arena, moduleName);
                    ast->GetPipeline(owner).usingSites.Push(arena, site);
                } else {
                    ast->GetModule(owner).usingImports.Push(arena, moduleName);
                    ast->GetModule(owner).usingSites.Push(arena, site);
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
                                       resourceIndex, decl.bounds);
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
    TokenRef declToken = current;  // First token of the declaration; a doc block precedes it
    BoundsSpec bounds;
    while (IsBoundsDecorator(current)) {
        Advance();
        if (!ParseBoundsDecorator(&bounds)) return NodeRef::Null();
    }
    TokenRef nameToken = current;
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
    TypeInfo attributeType = GetTypeInfoFromToken(PreviousTokenType());
    NodeRef attr = ASTFactory::MakeAttributeDecl(ast, name, std::string(stream->GetValue(previous)), loc.line, loc.column);
    AttachDocComment(attr, declToken);
    {
        SourceLocation nameLoc = getLocation(stream->GetOffset(nameToken));
        ast->SetNamePosition(attr, nameLoc.line, nameLoc.column);
    }

    // Parse decorators. @bounds on a later line is the next attribute's.
    const u32 typeLine = loc.line;
    while (Check(TokenType::AT) &&
           !(IsBoundsDecorator(current) && getLocation(stream->GetOffset(current)).line != typeLine)) {
        Advance();
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
        } else if (decoHash == Utils::HashStr("bounds")) {
            if (!ParseBoundsDecorator(&bounds)) break;
        } else {
            Error("Unknown decorator");
        }

        if (panicMode) break;
    }
    ast->GetAttributeDecl(attr).bounds = ResolveBounds(bounds, attributeType);

    // Note: attributeIndex is assigned by ParseAttributes() based on declaration order

    return attr;
}

NodeRef Parser::ParseResourceDecl() {
    TokenRef declToken = current;  // First token of the declaration; a doc block precedes it
    BoundsSpec bounds;
    while (IsBoundsDecorator(current)) {
        Advance();
        if (!ParseBoundsDecorator(&bounds)) return NodeRef::Null();
    }
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

    const SourceLocation typeLoc = getLocation(stream->GetOffset(current));
    std::string wrapper;
    if (Match(TokenType::BUFFER) || Match(TokenType::CBUFFER)) {
        wrapper = std::string(stream->GetValue(previous));
        if (!Consume(TokenType::LESS, "Expected '<' after buffer resource type")) {
            return NodeRef::Null();
        }
    }

    const TokenRef payloadToken = current;
    const SourceLocation payloadLoc = getLocation(stream->GetOffset(current));
    const TokenType payloadTokenType = CurrentTokenType();
    const bool customType = payloadTokenType == TokenType::IDENTIFIER ||
        payloadTokenType == TokenType::T || payloadTokenType == TokenType::U || payloadTokenType == TokenType::V;
    const bool builtinType = payloadTokenType <= TokenType::VOID ||
        (payloadTokenType >= TokenType::SAMPLER && payloadTokenType <= TokenType::TEXTURE2DARRAY) ||
        (payloadTokenType >= TokenType::DOUBLE && payloadTokenType <= TokenType::DMAT4);
    if (!customType && !builtinType) {
        ErrorAtCurrent("Expected resource type after ':'");
        return NodeRef::Null();
    }
    Advance();
    std::string payloadType(stream->GetValue(previous));
    u32 qualifierIndex = INVALID_INDEX;
    if (customType && (Match(TokenType::DOUBLE_COLON) || Match(TokenType::DOT))) {
        std::string moduleName = payloadType;
        if (!Consume(TokenType::IDENTIFIER, "Expected resource type name after module qualifier")) {
            return NodeRef::Null();
        }
        RecordTypeQualifierBeforeTypeName();
        qualifierIndex = ast->typeQualifiers.count - 1;
        payloadType = CanonicalizeModuleQualifiedName(moduleName, std::string(stream->GetValue(previous)));
    }

    const bool textureHandle = payloadTokenType >= TokenType::TEXTURE2D &&
        payloadTokenType <= TokenType::TEXTURE2DARRAY;
    const bool otherHandle = payloadTokenType == TokenType::SAMPLER || payloadType == "image2D";
    u32 dimensions = 0;
    while (Match(TokenType::LEFT_BRACKET)) {
        const TokenRef bracket = previous;
        u32 size = 0;
        if (!ParseArraySizeValue(&size) ||
            !Consume(TokenType::RIGHT_BRACKET, "Expected ']' after resource array size")) {
            return NodeRef::Null();
        }
        // Each texture array element takes its own binding slot (see
        // FinalizeParsedResourceBindings), so arrays share the 32-slot limit.
        // A rejected suffix is dropped, so uses of the resource still resolve.
        if (wrapper.empty() && otherHandle) {
            ErrorAt(bracket, "Arrays of " + std::string(stream->GetValue(payloadToken)) +
                    " resources are not supported; declare one resource per element");
            continue;
        }
        if (wrapper.empty() && textureHandle && dimensions > 0) {
            ErrorAt(bracket, "Texture arrays must have one dimension, e.g. 'textures: texture2D[4]'");
            continue;
        }
        if (wrapper.empty() && textureHandle && size > 32) {
            ErrorAt(bracket, "Texture array '" + name + "' has " + std::to_string(size) +
                    " elements; a pipeline has 32 resource slots and each element uses one");
            continue;
        }
        payloadType += "[" + std::to_string(size) + "]";
        ++dimensions;
    }
    if (!wrapper.empty() && !Consume(TokenType::GREATER, "Expected '>' after buffer element type")) {
        return NodeRef::Null();
    }
    std::string typeName = wrapper.empty() ? payloadType : wrapper + "<" + payloadType + ">";
    // @bounds after the type, on its line; a later line's is the next resource's.
    const u32 typeLine = getLocation(stream->GetOffset(previous)).line;
    while (IsBoundsDecorator(current) && getLocation(stream->GetOffset(current)).line == typeLine) {
        Advance();
        if (!ParseBoundsDecorator(&bounds)) return NodeRef::Null();
    }
    Match(TokenType::SEMICOLON);

    ReverseLookup::Register(Utils::HashStr(name.c_str()), name.c_str());
    ReverseLookup::Register(Utils::HashStr(typeName.c_str()), typeName.c_str());
    NodeRef resource = ASTFactory::MakeResourceDecl(ast, name, typeName, loc.line, loc.column);
    ast->GetResourceDecl(resource).typePosition = AST::PackPosition(typeLoc.line, typeLoc.column);
    ast->GetResourceDecl(resource).payloadTypePosition = AST::PackPosition(payloadLoc.line, payloadLoc.column);
    resourceTypeSites[resource.packed] = {payloadToken, qualifierIndex};
    AttachDocComment(resource, declToken);
    if (bounds.present) {
        // Only a plain scalar or vector has one value to bound; a struct's
        // fields take their own.
        TypeInfo payload = TYPE_INFO(CoreType::INVALID, 0, false);
        if (builtinType && wrapper.empty() && dimensions == 0) payload = GetTypeInfoFromToken(payloadTokenType);
        if (payload.coreType == CoreType::INVALID) {
            ErrorAt(bounds.decorator, "@bounds on a resource takes a float, int or uint scalar or vector; "
                                      "put them on the fields of a struct or buffer element instead");
        } else {
            ast->GetResourceDecl(resource).bounds = ResolveBounds(bounds, payload);
        }
    }
    return resource;
}

//==============================================================================
// @bounds(...)
//==============================================================================

bool Parser::IsBoundsDecorator(TokenRef token) const {
    static const u32 HASH_BOUNDS = Utils::HashStr("bounds");
    return stream->GetType(token) == TokenType::AT &&
           Utils::HashStr(stream->GetValue(token).data(), static_cast<u16>(stream->GetLength(token))) == HASH_BOUNDS;
}

// Reads the clauses of `@bounds(...)`, with the decorator already consumed:
//   >0        every component is greater than 0 (>=, <, <= likewise)
//   y<=1      just component y
//   -10<y<0   component y on both sides (< or <=)
// A clause without a component applies to every component, except on a
// side a component clause sets, or where it contradicts one; see
// ResolveBounds. A declaration has one @bounds: a second is an error.
bool Parser::ParseBoundsDecorator(BoundsSpec* spec) {
    if (spec->present) {
        ErrorAtPrevious("A declaration takes one @bounds: put all its clauses in it, as in @bounds(>=0, <=1)");
        // Its clauses are read so the declaration parses on, into a spec of
        // their own: the first @bounds keeps its clauses and its position,
        // and the two can't add up past the clause limit.
        BoundsSpec ignored;
        return ParseBoundsDecorator(&ignored);
    }
    spec->present = true;
    spec->decorator = previous;
    if (!Consume(TokenType::LEFT_PAREN, "Expected '(' after @bounds")) return false;

    auto parseNumber = [&](f64* value) -> bool {
        bool negative = Match(TokenType::MINUS);
        if (!Consume(TokenType::NUMBER, "Expected a number in @bounds")) return false;
        std::string text(stream->GetValue(previous));
        char* end = nullptr;
        f64 parsed = std::strtod(text.c_str(), &end);
        // Integer suffixes and digit separators aren't part of a bound.
        if (end == text.c_str() || (*end != '\0' && *end != 'f' && *end != 'u')) {
            ErrorAtPrevious("Expected a number in @bounds");
            return false;
        }
        *value = negative ? -parsed : parsed;
        return true;
    };
    auto parseComponent = [&](s8* component) -> bool {
        std::string_view name = stream->GetValue(previous);
        const char* letters = "xyzwrgba";
        for (u32 i = 0; i < 8 && name.size() == 1; i++) {
            if (name[0] == letters[i]) {
                *component = static_cast<s8>(i % 4);
                return true;
            }
        }
        ErrorAtPrevious("Expected a component x, y, z or w (or r, g, b, a) in @bounds");
        return false;
    };
    auto isRelation = [&]() {
        return Check(TokenType::LESS) || Check(TokenType::LESS_EQUAL) ||
               Check(TokenType::GREATER) || Check(TokenType::GREATER_EQUAL);
    };
    // Applies `value <relation> bound` to clause.bounds.
    auto applyRelation = [&](BoundsClause* clause, TokenType relation, f64 bound) {
        ComponentBounds& b = clause->bounds;
        bool exclusive = relation == TokenType::LESS || relation == TokenType::GREATER;
        if (relation == TokenType::GREATER || relation == TokenType::GREATER_EQUAL) {
            b.low = bound;
            b.flags |= ComponentBounds::HAS_LOW | (exclusive ? ComponentBounds::LOW_EXCLUSIVE : 0);
        } else {
            b.high = bound;
            b.flags |= ComponentBounds::HAS_HIGH | (exclusive ? ComponentBounds::HIGH_EXCLUSIVE : 0);
        }
    };

    while (!Check(TokenType::RIGHT_PAREN) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (spec->count == 16) {
            ErrorAtCurrent("Too many clauses in @bounds");
            return false;
        }
        BoundsClause clause = {-1, {0.0, 0.0, 0}, current};
        if (isRelation()) {
            TokenType relation = CurrentTokenType();
            Advance();
            f64 bound;
            if (!parseNumber(&bound)) return false;
            applyRelation(&clause, relation, bound);
        } else if (Match(TokenType::IDENTIFIER)) {
            if (!parseComponent(&clause.component)) return false;
            if (!isRelation()) {
                ErrorAtCurrent("Expected <, <=, > or >= after the component in @bounds");
                return false;
            }
            TokenType relation = CurrentTokenType();
            Advance();
            f64 bound;
            if (!parseNumber(&bound)) return false;
            applyRelation(&clause, relation, bound);
        } else if (Check(TokenType::NUMBER) || Check(TokenType::MINUS)) {
            // low < component < high
            f64 low, high;
            if (!parseNumber(&low)) return false;
            if (!Check(TokenType::LESS) && !Check(TokenType::LESS_EQUAL)) {
                ErrorAtCurrent("Expected < or <= in a two-sided bound like '-1<y<1'");
                return false;
            }
            bool lowExclusive = CurrentTokenType() == TokenType::LESS;
            Advance();
            if (!Consume(TokenType::IDENTIFIER, "Expected a component in a two-sided bound like '-1<y<1'") ||
                !parseComponent(&clause.component)) {
                return false;
            }
            if (!Check(TokenType::LESS) && !Check(TokenType::LESS_EQUAL)) {
                ErrorAtCurrent("Expected < or <= in a two-sided bound like '-1<y<1'");
                return false;
            }
            TokenType relation = CurrentTokenType();
            Advance();
            if (!parseNumber(&high)) return false;
            applyRelation(&clause, lowExclusive ? TokenType::GREATER : TokenType::GREATER_EQUAL, low);
            applyRelation(&clause, relation, high);
        } else {
            ErrorAtCurrent("Expected a bound like '>0', 'x<=1' or '-1<y<1' in @bounds");
            return false;
        }
        spec->clauses[spec->count++] = clause;
        if (!Match(TokenType::COMMA)) break;
    }
    if (!Consume(TokenType::RIGHT_PAREN, "Expected ')' after @bounds")) return false;
    spec->close = previous;
    return true;
}

// Per component: the clauses naming it, then, on a side they leave open,
// the clauses for every component when they agree with it. So
// `@bounds(>0, y<10)` gives y both bounds, while `@bounds(>0, y<0)` keeps
// y below 0 and the others above it.
const DeclaredBounds* Parser::ResolveBounds(const BoundsSpec& spec, const TypeInfo& type) {
    if (!spec.present) return nullptr;
    CoreType scalar = CoreTypeScalarType(type.coreType);
    u32 count = CoreTypeScalarComponentCount(type.coreType);
    bool numeric = scalar == CoreType::FLOAT || scalar == CoreType::INT || scalar == CoreType::UINT;
    // Vector types set arrayDimensions too, so arrays are ruled out by the
    // callers, which pass INVALID for them.
    if (!numeric || CoreTypeComponentCount(type.coreType) != count) {
        ErrorAt(spec.decorator, "@bounds applies to float, int and uint scalars and vectors");
        return nullptr;
    }

    // Two bounds on one side keep the tighter one.
    auto intersect = [](ComponentBounds* into, const ComponentBounds& with) {
        if (with.flags & ComponentBounds::HAS_LOW) {
            bool tighter = !(into->flags & ComponentBounds::HAS_LOW) || with.low > into->low ||
                           (with.low == into->low && (with.flags & ComponentBounds::LOW_EXCLUSIVE));
            if (tighter) {
                into->low = with.low;
                into->flags = static_cast<u8>((into->flags & ~ComponentBounds::LOW_EXCLUSIVE) |
                                              ComponentBounds::HAS_LOW |
                                              (with.flags & ComponentBounds::LOW_EXCLUSIVE));
            }
        }
        if (with.flags & ComponentBounds::HAS_HIGH) {
            bool tighter = !(into->flags & ComponentBounds::HAS_HIGH) || with.high < into->high ||
                           (with.high == into->high && (with.flags & ComponentBounds::HIGH_EXCLUSIVE));
            if (tighter) {
                into->high = with.high;
                into->flags = static_cast<u8>((into->flags & ~ComponentBounds::HIGH_EXCLUSIVE) |
                                              ComponentBounds::HAS_HIGH |
                                              (with.flags & ComponentBounds::HIGH_EXCLUSIVE));
            }
        }
    };
    auto isEmpty = [](const ComponentBounds& b) {
        if (!(b.flags & ComponentBounds::HAS_LOW) || !(b.flags & ComponentBounds::HAS_HIGH)) return false;
        bool exclusive = b.flags & (ComponentBounds::LOW_EXCLUSIVE | ComponentBounds::HIGH_EXCLUSIVE);
        return b.low > b.high || (b.low == b.high && exclusive);
    };

    ComponentBounds general = {0.0, 0.0, 0};
    for (u32 k = 0; k < spec.count; k++) {
        const BoundsClause& clause = spec.clauses[k];
        if (clause.component < 0) {
            intersect(&general, clause.bounds);
        } else if (count == 1 || static_cast<u32>(clause.component) >= count) {
            ErrorAt(clause.token, count == 1 ? std::string("@bounds on a scalar takes no component")
                                             : "The type has no component " +
                                                   std::string(1, "xyzw"[clause.component]));
            return nullptr;
        }
    }
    if (isEmpty(general)) {
        ErrorAt(spec.decorator, "@bounds leave no value possible");
        return nullptr;
    }

    DeclaredBounds* result = (DeclaredBounds*)arena->Allocate(sizeof(DeclaredBounds), alignof(DeclaredBounds));
    *result = {};
    result->count = static_cast<u8>(count);
    SourceLocation start = getLocation(stream->GetOffset(spec.decorator));
    SourceLocation end = getLocation(stream->GetOffset(spec.close));
    // The decorator token is the name after `@`, which comes right before it.
    result->position = AST::PackPosition(start.line, start.column > 1 ? start.column - 1 : start.column);
    result->endPosition = AST::PackPosition(end.line, end.column + stream->GetLength(spec.close));
    for (u32 c = 0; c < count; c++) {
        ComponentBounds own = {0.0, 0.0, 0};
        for (u32 k = 0; k < spec.count; k++) {
            if (spec.clauses[k].component == static_cast<s8>(c)) intersect(&own, spec.clauses[k].bounds);
        }
        if (isEmpty(own)) {
            ErrorAt(spec.decorator, std::string("@bounds leave component ") + "xyzw"[c] + " no value");
            return nullptr;
        }
        ComponentBounds combined = own;
        ComponentBounds side = general;
        side.flags &= ~(ComponentBounds::HAS_HIGH | ComponentBounds::HIGH_EXCLUSIVE);
        if (!(own.flags & ComponentBounds::HAS_LOW) && (side.flags & ComponentBounds::HAS_LOW)) {
            ComponentBounds candidate = combined;
            intersect(&candidate, side);
            if (!isEmpty(candidate)) combined = candidate;
        }
        side = general;
        side.flags &= ~(ComponentBounds::HAS_LOW | ComponentBounds::LOW_EXCLUSIVE);
        if (!(own.flags & ComponentBounds::HAS_HIGH) && (side.flags & ComponentBounds::HAS_HIGH)) {
            ComponentBounds candidate = combined;
            intersect(&candidate, side);
            if (!isEmpty(candidate)) combined = candidate;
        }
        result->components[c] = combined;
    }
    return result;
}

void Parser::ValidateResourceTypes(NodeRef owner, bool ownerIsPipeline, u32 firstResource) {
    // Resource payloads may refer to structs or imports declared later in the
    // owner's body. Check them once that body is complete, in its own scope.
    const auto& declarations = ownerIsPipeline ? ast->GetPipeline(owner).resources : ast->GetModule(owner).resources;
    typeCache.Clear();
    for (u32 i = firstResource; i < declarations.count; i++) {
        NodeRef ref = declarations[i];
        auto site = resourceTypeSites.find(ref.packed);
        if (site == resourceTypeSites.end()) continue;
        ResourceDeclData& declaration = ast->GetResourceDecl(ref);
        std::string typeName = declaration.typeName.ToString(sourceBase());
        std::string wrapper;
        std::string payload = ParseInnerResourceType(typeName, "buffer<");
        if (!payload.empty()) wrapper = "buffer";
        else {
            payload = ParseInnerResourceType(typeName, "cbuffer<");
            if (!payload.empty()) wrapper = "cbuffer";
            else payload = typeName;
        }
        std::string base = StripFixedArraySuffixes(payload);
        std::string canonicalBase = CanonicalizeTypeName(base);
        payload = canonicalBase + payload.substr(base.size());
        typeName = wrapper.empty() ? payload : wrapper + "<" + payload + ">";
        ReverseLookup::Register(Utils::HashStr(typeName.c_str()), typeName.c_str());
        declaration.typeName = ArenaString::MakeHashOnly(typeName);

        if (site->second.qualifierIndex != INVALID_INDEX) {
            ModuleNameSite& qualifier = ast->typeQualifiers[site->second.qualifierIndex];
            u32 moduleIdx = ResolveModuleIndexByWrittenName(qualifier.name);
            if (moduleIdx != INVALID_INDEX && moduleIdx < symbolTable.modules.count) {
                qualifier.moduleNameHash = symbolTable.modules[moduleIdx].name.nameHash;
            }
        }

        // Resource handles include spellings outside ResolveType's value-type table.
        const bool resourceHandle = canonicalBase == "sampler" || canonicalBase == "texture2D" ||
            canonicalBase == "texture3D" || canonicalBase == "textureCube" ||
            canonicalBase == "texture2DArray" || canonicalBase == "image2D";
        TypeInfo info = ResolveType(canonicalBase);
        if (!ownerIsPipeline && !resourceHandle && canonicalBase.find("::") == std::string::npos) {
            TypeInfo local = ResolveType(ast->GetModule(owner).name.ToString(sourceBase()) + "::" + canonicalBase);
            if (local.coreType != CoreType::INVALID) info = local;
        }
        if (!resourceHandle && (info.coreType == CoreType::INVALID || info.coreType == CoreType::VOID ||
            info.coreType == CoreType::GENERIC_T || info.coreType == CoreType::GENERIC_U || info.coreType == CoreType::GENERIC_V)) {
            panicMode = false;
            ErrorAt(site->second.token, "Unknown resource type '" + canonicalBase + "'");
        } else if (ownerIsPipeline) {
            RegisterParsedResource(declaration.name.ToString(sourceBase()), typeName, declaration.resourceIndex,
                                   declaration.bounds);
        }
    }
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
    TokenRef declToken = previous;  // The PASS keyword; a doc block precedes it
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    Consume(TokenType::STRING, "Expected pass name in quotes");
    ArenaString passName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));

    NodeRef pass = ASTFactory::MakePass(ast, passName.ToString(sourceBase()), loc.line, loc.column);
    AttachDocComment(pass, declToken);
    {
        // STRING token offsets start at the contents, past the opening quote.
        SourceLocation nameLoc = getLocation(stream->GetOffset(previous));
        ast->SetNamePosition(pass, nameLoc.line, nameLoc.column);
    }

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
            if (Consume(TokenType::RIGHT_BRACE, "Expected '}' after pass_block mappings")) {
                MarkNodeEndAtPreviousToken(pass);
            }
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
    if (Consume(TokenType::RIGHT_BRACE, "Expected '}' after pass body")) {
        MarkNodeEndAtPreviousToken(pass);
    }

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
            SourceLocation typeLoc = getLocation(stream->GetOffset(previous));
            if (Match(TokenType::LEFT_BRACKET)) {
                NodeRef arrayDecl = ParseArrayDeclaration(TokenTypeToReturnType(varType),
                                                           StorageClass::Default, true);
                if (arrayDecl.IsValid()) {
                    ast->IndexPosition(arrayDecl, loc.line, loc.column);
                    ast->GetVariableDecl(arrayDecl).typePosition =
                        AST::PackPosition(typeLoc.line, typeLoc.column);
                    ast->GetPass(pass).consts.Push(arena, arrayDecl);
                }
                continue;
            }
            Consume(TokenType::IDENTIFIER, "Expected constant name");
            SourceLocation nameLoc = getLocation(stream->GetOffset(previous));
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
            ast->GetVariableDecl(varDecl).typePosition = AST::PackPosition(typeLoc.line, typeLoc.column);
            ast->GetVariableDecl(varDecl).namePosition = AST::PackPosition(nameLoc.line, nameLoc.column);
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
        SourceLocation outputNameLoc = getLocation(stream->GetOffset(previous));

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
        SourceLocation outputTypeLoc = getLocation(stream->GetOffset(previous));
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
                if (!TryParseU32(stream->GetValue(previous), &location))
                    Error("Fragment output location must be a valid 32-bit integer literal");
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
            decl.namePosition = AST::PackPosition(outputNameLoc.line, outputNameLoc.column);
            decl.typePosition = AST::PackPosition(outputTypeLoc.line, outputTypeLoc.column);
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
        SourceLocation attrLoc = getLocation(stream->GetOffset(previous));
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

        for (u32 i = 0; i < ast->GetPass(pass).usedAttributes.count; i++) {
            if (ast->GetPass(pass).usedAttributes[i].nameHash ==
                attrName.nameHash) {
                Error("Duplicate attribute in 'use attributes'");
                break;
            }
        }

        ast->GetPass(pass).usedAttributes.Push(arena, attrName);
        ast->GetPass(pass).usedAttributePositions.Push(arena,
            AST::PackPosition(attrLoc.line, attrLoc.column));

        // Set the optional bit for this attribute
        if (isOptional && idx < 32) {
            ast->GetPass(pass).optionalAttributesMask |= (1u << idx);
        }

        if (!Match(TokenType::COMMA) || Check(TokenType::RIGHT_BRACE)) break;
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

        if (!Match(TokenType::COMMA) || Check(TokenType::RIGHT_BRACE)) break;
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

    if (Consume(TokenType::RIGHT_BRACE, "Expected '}' after compute_graph")) {
        MarkNodeEndAtPreviousToken(graph);
    }
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
        u32 value = 0;
        if (!TryParseU32(num, &value)) {
            Error("Workgroup size must be an integer literal");
            return 1;
        }
        return value;
    };

    u32 sizeX = parseSize("Expected workgroup size X");
    Consume(TokenType::COMMA, "Expected ',' after workgroup size X");
    u32 sizeY = parseSize("Expected workgroup size Y");
    Consume(TokenType::COMMA, "Expected ',' after workgroup size Y");
    u32 sizeZ = parseSize("Expected workgroup size Z");
    Match(TokenType::COMMA);
    Consume(TokenType::RIGHT_BRACKET, "Expected ']' after workgroup size");

    if (sizeX == 0 || sizeY == 0 || sizeZ == 0) {
        Error("Workgroup size components must be greater than 0");
    }
    if (sizeX > 1024u || sizeY > 1024u || sizeZ > 1024u ||
        static_cast<u64>(sizeX) * sizeY * sizeZ > 1024u) {
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

    SymbolTable::EnterScope(&symbolTable);
    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        NodeRef stmt = ParseStatement();
        if (stmt.IsValid()) {
            ast->GetBlock(body).statements.Push(arena, stmt);
        }
    }

    bool hasEnd = Consume(TokenType::RIGHT_BRACE, "Expected '}' after shader stage body");
    SymbolTable::ExitScope(&symbolTable);
    if (hasEnd) {
        MarkNodeEndAtPreviousToken(body);
    }

    NodeRef stage = ASTFactory::MakeShaderStage(ast, stageType, body, loc.line, loc.column);
    if (hasEnd) {
        MarkNodeEndAtPreviousToken(stage);
    }

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


} // namespace BWSL
