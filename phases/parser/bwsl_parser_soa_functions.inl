// Part of bwsl_parser_soa.cpp. Include from that file only.
// Module loading validation plus function and compute-body parsing.
#pragma once
#include "bwsl_parser_soa.cpp"
#include "bwsl_embedded_modules.generated.h"

//==============================================================================
// Helper functions
//==============================================================================

namespace BWSL {

bool Parser::ValidateAttributeInUse(const ArenaString& attrName) {
    if (!currentPass.IsValid()) return false;

    const PassData& pass = ast->GetPass(currentPass);
    for (u32 i = 0; i < pass.usedAttributes.count; i++) {
        ArenaString usedAttr = pass.usedAttributes[i];
        if (usedAttr.nameLength > 0) {
            std::string_view usedView = usedAttr.view(sourceBase());
            if (!usedView.empty() && usedView.back() == '?') {
                usedView.remove_suffix(1);
                if (Utils::HashStr(usedView.data(), static_cast<u16>(usedView.size())) == attrName.nameHash) {
                    return true;
                }
            } else if (Utils::HashStr(usedView.data(), static_cast<u16>(usedView.size())) == attrName.nameHash) {
                return true;
            }
        }
    }
    return false;
}

bool Parser::ValidateResourceInUse(const ArenaString& resourceName) {
    if (!currentPass.IsValid()) return false;

    const PassData& pass = ast->GetPass(currentPass);
    for (u32 i = 0; i < pass.usedResources.count; i++) {
        if (pass.usedResources[i].nameHash == resourceName.nameHash) {
            return true;
        }
    }
    return false;
}

bool Parser::PipelineDeclaresResources() const {
    return currentPipeline.IsValid() &&
           ast->GetPipeline(currentPipeline).resources.count > 0;
}

const ResourceDeclData* Parser::LookupPipelineResourceDecl(const ArenaString& resourceName) const {
    if (!PipelineDeclaresResources()) return nullptr;

    const PipelineData& pipeline = ast->GetPipeline(currentPipeline);
    for (u32 i = 0; i < pipeline.resources.count; i++) {
        const ResourceDeclData& resourceDecl = ast->GetResourceDecl(pipeline.resources[i]);
        if (resourceDecl.name.nameHash == resourceName.nameHash) {
            return &resourceDecl;
        }
    }
    return nullptr;
}

const AttributeDeclData* Parser::LookupPipelineAttributeDecl(const ArenaString& attrName) const {
    if (currentPipeline.IsNull()) return nullptr;

    const PipelineData& pipeline = ast->GetPipeline(currentPipeline);
    for (u32 i = 0; i < pipeline.attributes.count; i++) {
        NodeRef attrRef = pipeline.attributes[i];
        if (attrRef.Type() != ASTNodeType::ATTRIBUTE_DECL) continue;
        const AttributeDeclData& attr = ast->GetAttributeDecl(attrRef);
        if (attr.name.nameHash == attrName.nameHash) {
            return &attr;
        }
    }
    return nullptr;
}

const AttributeDeclData* Parser::LookupModuleAttributeDecl(NodeRef module, const ArenaString& attrName) const {
    if (module.IsNull() || module.Type() != ASTNodeType::MODULE) return nullptr;

    const ModuleNodeData& moduleData = ast->GetModule(module);
    for (u32 i = 0; i < moduleData.attributes.count; i++) {
        NodeRef attrRef = moduleData.attributes[i];
        if (attrRef.Type() != ASTNodeType::ATTRIBUTE_DECL) continue;
        const AttributeDeclData& attr = ast->GetAttributeDecl(attrRef);
        if (attr.name.nameHash == attrName.nameHash) {
            return &attr;
        }
    }
    return nullptr;
}

const ResourceDeclData* Parser::LookupModuleResourceDecl(NodeRef module, const ArenaString& resourceName) const {
    if (module.IsNull() || module.Type() != ASTNodeType::MODULE) return nullptr;

    const ModuleNodeData& moduleData = ast->GetModule(module);
    for (u32 i = 0; i < moduleData.resources.count; i++) {
        NodeRef resourceRef = moduleData.resources[i];
        if (resourceRef.Type() != ASTNodeType::RESOURCE_DECL) continue;
        const ResourceDeclData& resource = ast->GetResourceDecl(resourceRef);
        if (resource.name.nameHash == resourceName.nameHash) {
            return &resource;
        }
    }
    return nullptr;
}

bool Parser::ValidateAssignmentTarget(NodeRef target) {
    switch (target.Type()) {
        case ASTNodeType::IDENTIFIER: {
            Symbol* sym = SymbolTable::LookupAny(&symbolTable, ast->GetIdentifier(target).name);
            if (sym && sym->kind == SymbolKind::VARIABLE) {
                const VariableData& varData = symbolTable.variables[sym->index];
                if (varData.isConst && !varData.isEval) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Cannot assign to const variable '%s'",
                            ast->GetIdentifier(target).name.ToString(sourceBase()).c_str());
                    ErrorAt(previous, msg);
                    return false;
                }
            }
            return true;
        }

        case ASTNodeType::MEMBER_ACCESS:
            return true;

        case ASTNodeType::ARRAY_ACCESS:
            return ValidateAssignmentTarget(ast->GetArrayAccess(target).array);

        default:
            Error("Invalid assignment target");
            return false;
    }
}

bool Parser::TryRegisterModule(const std::string& moduleName) {
    if (moduleName.empty()) {
        return false;
    }

    u32 moduleHash = Utils::HashStr(moduleName.c_str());
    u32 existingIdx = SymbolTable::FindModuleByHash(&symbolTable, moduleHash);
    if (existingIdx != INVALID_INDEX) {
        return true;
    }

    if (const EmbeddedModules::ModuleSource* embedded =
            EmbeddedModules::FindByHash(moduleHash)) {
        return RegisterModuleFromSource(moduleName, embedded->source,
                                        embedded->sourceLength,
                                        embedded->virtualPath);
    }

    return TryRegisterModuleFromDisk(moduleName);
}

bool Parser::IsEmbeddedModuleName(const std::string& moduleName) const {
    if (moduleName.empty()) {
        return false;
    }
    return EmbeddedModules::FindByHash(Utils::HashStr(moduleName.c_str())) != nullptr;
}

bool Parser::TryRegisterModuleFromDisk(const std::string& moduleName) {
    if (moduleName.empty()) {
        return false;
    }

    u32 moduleHash = Utils::HashStr(moduleName.c_str());
    u32 existingIdx = SymbolTable::FindModuleByHash(&symbolTable, moduleHash);
    if (existingIdx != INVALID_INDEX) {
        return true;
    }

    std::string modulePath = ResolveModulePath(moduleName);
    if (modulePath.empty()) {
        return false;
    }

    std::ifstream moduleFile(modulePath);
    if (!moduleFile.is_open()) {
        return false;
    }

    // Read module file contents
    std::string moduleSource((std::istreambuf_iterator<char>(moduleFile)),
                              std::istreambuf_iterator<char>());
    moduleFile.close();

    return RegisterModuleFromSource(moduleName, moduleSource.data(),
                                    moduleSource.size(), modulePath.c_str());
}

bool Parser::FindConflictingDiskModule(const std::string& moduleName,
                                       std::string* outModulePath) {
    if (moduleName.empty()) {
        return false;
    }

    const EmbeddedModules::ModuleSource* embedded =
        EmbeddedModules::FindByHash(Utils::HashStr(moduleName.c_str()));
    if (!embedded) {
        return false;
    }

    std::string modulePath = ResolveModulePath(moduleName);
    if (modulePath.empty()) {
        return false;
    }

    std::ifstream moduleFile(modulePath, std::ios::binary);
    if (!moduleFile.is_open()) {
        return false;
    }

    std::string moduleSource((std::istreambuf_iterator<char>(moduleFile)),
                              std::istreambuf_iterator<char>());
    moduleFile.close();

    bool differs = moduleSource.size() != embedded->sourceLength ||
                   memcmp(moduleSource.data(), embedded->source,
                          embedded->sourceLength) != 0;
    if (differs && outModulePath) {
        *outModulePath = modulePath;
    }
    return differs;
}

bool Parser::RegisterModuleFromSource(const std::string& moduleName,
                                      const char* source,
                                      size_t sourceLength,
                                      const char* sourceName) {
    if (moduleName.empty() || source == nullptr) {
        return false;
    }

    u32 moduleHash = Utils::HashStr(moduleName.c_str());
    u32 existingIdx = SymbolTable::FindModuleByHash(&symbolTable, moduleHash);
    if (existingIdx != INVALID_INDEX) {
        return true;
    }

    // Copy source into arena so it lives as long as AST
    // ArenaStrings store sourceOffset values into the lexer's source buffer,
    // so we need the source to persist after this function returns
    char* persistentSource = (char*)arena->Allocate(sourceLength + 1, 1);
    if (!persistentSource) {
        return false;
    }
    memcpy(persistentSource, source, sourceLength);
    persistentSource[sourceLength] = '\0';

    // Save current parser state
    TokenRef savedCurrent = current;
    TokenRef savedPrevious = previous;
    Lexer* savedLexer = lexer;
    TokenStream* savedStream = stream;
    bool savedHasLookahead = hasLookahead;
    TokenRef savedLookahead = lookahead;
    bool savedHas3TokenLookahead = has3TokenLookahead;
    TokenRef savedLookahead3 = lookahead3;
    bool savedInModuleScope = symbolTable.inModuleScope;
    u32 savedCurrentModuleIdx = symbolTable.currentModuleIndex;
    NodeRef savedRoot = context->root;
    NodeRef savedCurrentPipeline = currentPipeline;
    NodeRef savedCurrentPass = currentPass;
    bool savedInShaderStage = inShaderStage;
    ShaderStage savedCurrentShaderStage = currentShaderStage;
    bool savedParsingEmbeddedModule = parsingEmbeddedModule;

    // Create new TokenStream and lexer for arena-persistent module source
    TokenStream moduleStream;
    moduleStream.Init(arena, persistentSource, sourceLength);
    Lexer moduleLexer(std::string(persistentSource, sourceLength), moduleStream);
    moduleLexer.Tokenize();  // Must tokenize before parsing!
    lexer = &moduleLexer;
    stream = &moduleStream;
    hasLookahead = false;
    has3TokenLookahead = false;
    lookahead = INVALID_TOKEN;
    lookahead3 = INVALID_TOKEN;
    parsingEmbeddedModule =
        sourceName && strncmp(sourceName, "stdlib://", 9) == 0;

    bool success = false;
    current = 0;
    previous = 0;

    while (!Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (Match(TokenType::MODULE)) {
            NodeRef moduleNode = ParseModule();
            if (moduleNode.IsValid()) {
                const ModuleNodeData& moduleData = ast->GetModule(moduleNode);
                u32 parsedModuleIdx = SymbolTable::FindModuleByHash(&symbolTable,
                    moduleData.name.nameHash);
                if (parsedModuleIdx != INVALID_INDEX && parsedModuleIdx < symbolTable.modules.count) {
                    symbolTable.modules[parsedModuleIdx].sourcePtr = persistentSource;
                    symbolTable.modules[parsedModuleIdx].sourceLength = static_cast<u32>(sourceLength);
                }
                if (moduleData.name.nameHash == moduleHash) {
                    success = true;
                }
            }
        } else if (Check(TokenType::PIPELINE)) {
            SkipBracedDeclaration(false);
        } else {
            Advance();
        }
        panicMode = false;
    }

    // Restore parser state
    current = savedCurrent;
    previous = savedPrevious;
    lexer = savedLexer;
    stream = savedStream;
    hasLookahead = savedHasLookahead;
    lookahead = savedLookahead;
    has3TokenLookahead = savedHas3TokenLookahead;
    lookahead3 = savedLookahead3;
    symbolTable.inModuleScope = savedInModuleScope;
    symbolTable.currentModuleIndex = savedCurrentModuleIdx;
    context->root = savedRoot;
    currentPipeline = savedCurrentPipeline;
    currentPass = savedCurrentPass;
    inShaderStage = savedInShaderStage;
    currentShaderStage = savedCurrentShaderStage;
    parsingEmbeddedModule = savedParsingEmbeddedModule;

    return success;
}

//==============================================================================
// Function parsing
//==============================================================================

NodeRef Parser::ParseFunction() {
    TokenRef declToken = current;  // Function name token; a doc block precedes it

    // Get function name
    if (Check(TokenType::IDENTIFIER) || CheckMask(TokenMasks::CORE_TYPES)) {
        Advance();
    } else {
        Error("Expected function name");
        return NodeRef::Null();
    }
    std::string functionName(stream->GetValue(previous));
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;

    Consume(TokenType::DOUBLE_COLON, "Expected '::' after function name");

    // Create function node with temporary return type
    NodeRef function = ASTFactory::MakeFunction(ast, functionName, CoreType::FLOAT, line, col);
    AttachDocComment(function, declToken);

    // Parse parameters
    Consume(TokenType::LEFT_PAREN, "Expected '(' after '::'");
    ParseFunctionParameters(function);
    Consume(TokenType::RIGHT_PAREN, "Expected ')' after parameters");

    // Parse return type
    Consume(TokenType::ARROW, "Expected '->' after parameters");

    // Check all possible return types
    std::string customReturnTypeName;
    if (Check(TokenType::IDENTIFIER)) {
        // Custom type return (struct name)
        Advance();
        std::string returnTypeName(stream->GetValue(previous));

        // Check for module-qualified type: Module::Type
        if (Match(TokenType::DOUBLE_COLON)) {
            Consume(TokenType::IDENTIFIER, "Expected type name after '::'");
            customReturnTypeName = CanonicalizeModuleQualifiedName(
                returnTypeName, std::string(stream->GetValue(previous)));
        } else {
            customReturnTypeName = returnTypeName;
        }
        TypeInfo resolved = ResolveType(customReturnTypeName);
        if (resolved.coreType != CoreType::INVALID) {
            ast->GetFunction(function).returnType = resolved.coreType;
            ast->GetFunction(function).returnTypeHash = resolved.customTypeHash;
        } else {
            ast->GetFunction(function).returnType = CoreType::CUSTOM;
            ast->GetFunction(function).returnTypeHash =
                Utils::HashStr(customReturnTypeName.c_str());
        }
    } else if (MatchMask(TokenMasks::CORE_TYPES)) {
        ast->GetFunction(function).returnType = TokenTypeToReturnType(static_cast<TokenType>(stream->GetType(previous)));
        ast->GetFunction(function).returnTypeHash = 0;
    } else if (Match(TokenType::VOID)) {
        ast->GetFunction(function).returnType = CoreType::VOID;
        ast->GetFunction(function).returnTypeHash = 0;
    } else if (Match(TokenType::VERTEX_FUNCTION)) {
        ast->GetFunction(function).returnType = CoreType::VERTEX_FUNCTION;
        ast->GetFunction(function).returnTypeHash = 0;
    } else if (Match(TokenType::FRAGMENT_FUNCTION)) {
        ast->GetFunction(function).returnType = CoreType::FRAGMENT_FUNCTION;
        ast->GetFunction(function).returnTypeHash = 0;
    } else if (Match(TokenType::COMPUTE_FUNCTION)) {
        ast->GetFunction(function).returnType = CoreType::COMPUTE_FUNCTION;
        ast->GetFunction(function).returnTypeHash = 0;
    } else if (Match(TokenType::PASS_BLOCK)) {
        ast->GetFunction(function).returnType = CoreType::PASS_BLOCK;
        ast->GetFunction(function).returnTypeHash = 0;
    } else {
        Error("Expected valid return type after '->'");
        return NodeRef::Null();
    }

    const FunctionDeclData& decl = ast->GetFunction(function);

    // Check if any parameter uses a constraint type (making this a generic function)
    std::vector<TypeMask> constraintMasks;
    std::vector<bool> isConstrained;
    bool isGenericFunction = CheckForConstrainedParams(&symbolTable, decl.parameters,
                                                        constraintMasks, isConstrained);

    // Check if return type is also a constraint
    ArenaString returnTypeName = customReturnTypeName.empty()
        ? ArenaString::MakeHashOnly("")
        : ArenaString::MakeHashOnly(customReturnTypeName);
    TypeMask returnConstraint = 0;
    s8 returnMatchesParam = -1;

    if (!customReturnTypeName.empty()) {
        returnConstraint = SymbolTable::LookupConstraint(&symbolTable, returnTypeName);
        if (returnConstraint != 0) {
            // Return type is a constraint - check if it matches a parameter's constraint
            returnMatchesParam = FindMatchingParamForReturn(decl.parameters, returnTypeName, isConstrained);
            isGenericFunction = true;
        }
    }

    if (isGenericFunction) {
        // Register as a generic function template
        // Generic functions are not added as regular symbols - they're templates
        FillGenericFunctionData(&symbolTable, arena, decl, function,
                                constraintMasks, isConstrained,
                                returnTypeName, returnConstraint, returnMatchesParam);
    } else {
        // Regular function - add to symbol table as before
        std::vector<OverloadTypeMask> paramMasks;
        BuildParamMasks(decl.parameters, paramMasks);
        u64 signatureKey = HashOverloadSignature(paramMasks.data(), static_cast<u32>(paramMasks.size()));

        // Only add unqualified function symbol when NOT in module scope
        // Module functions get their qualified name added by AddModuleFunction() after parsing
        if (!symbolTable.inModuleScope) {
            if (HasDuplicateFunctionSignature(&symbolTable, decl.name, paramMasks, NamespaceKind::GLOBAL,
                    INVALID_INDEX, signatureKey)) {
                Error("Function overload already declared");
                return NodeRef::Null();
            }

            Symbol* sym = SymbolTable::AddSymbol(&symbolTable, decl.name, SymbolKind::FUNCTION);
            if (!sym) {
                Error("Function already declared");
                return NodeRef::Null();
            }
            FillFunctionData(&symbolTable, arena, decl, function, paramMasks, signatureKey, sym->index);
        }
    }

    SymbolTable::EnterScope(&symbolTable);

    // Parse function body
    Consume(TokenType::LEFT_BRACE, "Expected '{' before function body");

    // Check if this is a shader block function
    if (ast->GetFunction(function).returnType == CoreType::VERTEX_FUNCTION ||
        ast->GetFunction(function).returnType == CoreType::FRAGMENT_FUNCTION ||
        ast->GetFunction(function).returnType == CoreType::COMPUTE_FUNCTION ||
        ast->GetFunction(function).returnType == CoreType::PASS_BLOCK) {

        // For shader block functions, we expect a shader stage inside
        if (ast->GetFunction(function).returnType == CoreType::VERTEX_FUNCTION) {
            if (!Match(TokenType::VERTEX)) {
                Error("Expected 'vertex' block in vertex_function");
                SymbolTable::ExitScope(&symbolTable);
                return NodeRef::Null();
            }
            ast->GetFunction(function).body = ParseShaderStage(ASTNodeType::VERTEX_STAGE);
        } else if (ast->GetFunction(function).returnType == CoreType::FRAGMENT_FUNCTION) {
            if (!Match(TokenType::FRAGMENT)) {
                Error("Expected 'fragment' block in fragment_function");
                SymbolTable::ExitScope(&symbolTable);
                return NodeRef::Null();
            }
            ast->GetFunction(function).body = ParseShaderStage(ASTNodeType::FRAGMENT_STAGE);
        } else if (ast->GetFunction(function).returnType == CoreType::PASS_BLOCK) {
            if (!Match(TokenType::PASS)) {
                Error("Expected 'pass' block in pass_block function");
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
                SymbolTable::ExitScope(&symbolTable);
                return NodeRef::Null();
            }
            SourceLocation ploc = getLocation(stream->GetOffset(previous));
            ast->GetFunction(function).body = ASTFactory::MakePass(ast, "", ploc.line, ploc.column);
            NodeRef oldPass = currentPass;
            currentPass = ast->GetFunction(function).body;
            SymbolTable::EnterScope(&symbolTable);
            Consume(TokenType::LEFT_BRACE, "Expected '{' after pass in pass_block function");
            ParsePassBody(ast->GetFunction(function).body);
            Consume(TokenType::RIGHT_BRACE, "Expected '}' after pass block");
            SymbolTable::ExitScope(&symbolTable);
            currentPass = oldPass;
        } else if (ast->GetFunction(function).returnType == CoreType::COMPUTE_FUNCTION) {
            // For compute functions, parse the compute body
            SourceLocation cloc = getLocation(stream->GetOffset(current));
            ast->GetFunction(function).body = ASTFactory::MakeShaderStage(ast, ASTNodeType::COMPUTE_STAGE, NodeRef::Null(), cloc.line, cloc.column);
            ParseComputeBody(ast->GetFunction(function).body);
        }

        Consume(TokenType::RIGHT_BRACE, "Expected '}' after function body");
    } else {
        // Check if this is a type pattern match body (for generic functions)
        // Syntax: type: expression  (e.g., float2: v * 2.0)
        bool isTypePatternBody = false;
        if (isGenericFunction) {
            // Look ahead: if we see CORE_TYPE followed by COLON, it's a type pattern
            if (CheckMask(TokenMasks::CORE_TYPES)) {
                TokenRef nextTok = PeekNext();
                if (stream->GetType(nextTok) == static_cast<u8>(TokenType::COLON)) {
                    isTypePatternBody = true;
                }
            } else if (Check(TokenType::DEFAULT)) {
                TokenRef nextTok = PeekNext();
                if (stream->GetType(nextTok) == static_cast<u8>(TokenType::COLON)) {
                    isTypePatternBody = true;
                }
            }
        }

        if (isTypePatternBody) {
            ast->GetFunction(function).body = ParseTypePatternMatch();
            Consume(TokenType::RIGHT_BRACE, "Expected '}' after type pattern match");
        } else {
            // Regular function with statements
            ast->GetFunction(function).body = ParseBlock();
        }
    }

    SymbolTable::ExitScope(&symbolTable);

    return function;
}

void Parser::ParseFunctionParameters(NodeRef function) {
    // Handle empty parameter list
    if (Check(TokenType::RIGHT_PAREN)) {
        return;
    }

    do {
        std::string paramName;
        std::string paramType;

        // Check for type-first syntax: "type name" (C-style)
        if (CheckMask(TokenMasks::CORE_TYPES)) {
            Advance();
            paramType = std::string(stream->GetValue(previous));

            // Check for pointer type: int^ means pointer to int
            while (Match(TokenType::BITWISE_XOR)) {
                paramType += "^";
            }

            // Check for array type suffix: type[size]
            if (Match(TokenType::LEFT_BRACKET)) {
                if (!Match(TokenType::NUMBER)) {
                    Error("Expected array size");
                }
                Consume(TokenType::RIGHT_BRACKET, "Expected ']' after array size");
            }

            // Expect parameter name after type
            if (Check(TokenType::IDENTIFIER)) {
                Advance();
                paramName = std::string(stream->GetValue(previous));
            } else {
                // Anonymous parameter (just type)
                paramName = "";
            }
        }
        // Check for identifier - could be:
        // - "name: type" (Rust/Swift style)
        // - "CustomType name" (C-style with custom type)
        // - "CustomType" (anonymous parameter with custom type)
        // - "Module::Type name" (module-qualified custom type)
        else if (Check(TokenType::IDENTIFIER)) {
            Advance();
            std::string identifierStr(stream->GetValue(previous));

            // Check for module-qualified type: Module::Type
            if (Match(TokenType::DOUBLE_COLON)) {
                // Module::Type pattern
                std::string moduleName = identifierStr;
                Consume(TokenType::IDENTIFIER, "Expected type name after '::'");
                std::string typeName(stream->GetValue(previous));
                paramType = CanonicalizeModuleQualifiedName(moduleName, typeName);

                // Now expect parameter name
                if (Check(TokenType::IDENTIFIER)) {
                    Advance();
                    paramName = std::string(stream->GetValue(previous));
                } else {
                    // Anonymous parameter with module-qualified type
                    paramName = "";
                }
            } else if (Match(TokenType::COLON)) {
                // We have name: type
                paramName = identifierStr;

                // Parse type (could be core type, custom type, or module-qualified type)
                if (MatchMask(TokenMasks::CORE_TYPES)) {
                    paramType = std::string(stream->GetValue(previous));
                    // Check for pointer type: int^ means pointer to int
                    while (Match(TokenType::BITWISE_XOR)) {
                        paramType += "^";
                    }
                } else if (Match(TokenType::IDENTIFIER)) {
                    std::string typeIdent(stream->GetValue(previous));
                    // Check for module-qualified type after colon
                    if (Match(TokenType::DOUBLE_COLON)) {
                        Consume(TokenType::IDENTIFIER, "Expected type name after '::'");
                        paramType = CanonicalizeModuleQualifiedName(
                            typeIdent, std::string(stream->GetValue(previous)));
                    } else {
                        paramType = typeIdent;
                    }
                    // Check for pointer type on custom types
                    while (Match(TokenType::BITWISE_XOR)) {
                        paramType += "^";
                    }
                } else {
                    Error("Expected parameter type after ':'");
                    return;
                }
            } else if (Check(TokenType::LEFT_BRACKET)) {
                // CustomType[size] name - array of custom type
                paramType = identifierStr;
                Match(TokenType::LEFT_BRACKET);
                if (!Match(TokenType::NUMBER)) {
                    Error("Expected array size");
                }
                Consume(TokenType::RIGHT_BRACKET, "Expected ']' after array size");

                // Now expect parameter name
                if (Check(TokenType::IDENTIFIER)) {
                    Advance();
                    paramName = std::string(stream->GetValue(previous));
                } else {
                    paramName = "";
                }
            } else if (Check(TokenType::IDENTIFIER)) {
                // CustomType name - identifier followed by another identifier
                paramType = identifierStr;
                Advance();
                paramName = std::string(stream->GetValue(previous));
            } else {
                // Just identifier, treat as custom type with anonymous parameter
                paramType = identifierStr;
                paramName = "";
            }
        } else {
            Error("Expected parameter type or name");
            return;
        }

        // Add parameter to function
        ast->GetFunction(function).parameters.Push(arena,
            std::make_pair(ArenaString::MakeHashOnly(paramName),
                          ArenaString::MakeHashOnly(paramType)));

    } while (Match(TokenType::COMMA));
}

void Parser::ParseComputeBody(NodeRef compute) {
    // Parse the compute shader body - for now just parse statements into a block
    SourceLocation loc = getLocation(stream->GetOffset(current));
    NodeRef body = ASTFactory::MakeBlock(ast, loc.line, loc.column);

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        NodeRef stmt = ParseStatement();
        if (stmt.IsValid()) {
            ast->GetBlock(body).statements.Push(arena, stmt);
        }
    }

    ast->GetShaderStage(compute).body = body;
}

void Parser::ParseFunctionsBlockBody(NodeRef block) {
    // Parse a block of function definitions
    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (IsFunctionDeclStart()) {
            NodeRef func = ParseFunction();
            if (func.IsValid()) {
                ast->GetBlock(block).statements.Push(arena, func);
            }
        } else {
            ErrorAtCurrent("Expected function definition");
            Advance();
        }
    }
}

} // namespace BWSL
