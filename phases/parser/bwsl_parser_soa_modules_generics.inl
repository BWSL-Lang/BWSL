// Part of bwsl_parser_soa.cpp. Include from that file only.
// Module declarations, constraints, where clauses, generic functions, and shader stage expression resolution.
#pragma once
#include "bwsl_parser_soa.cpp"

//==============================================================================
// Module parsing
//==============================================================================

namespace BWSL {

NodeRef Parser::ParseModule() {
    TokenRef declToken = previous;  // The MODULE keyword; a doc block precedes it
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;

    // Note: MODULE token already consumed by caller
    Consume(TokenType::IDENTIFIER, "Expected module name");

    std::string moduleName(stream->GetValue(previous));
    ArenaString moduleNameArena = ArenaString::MakeHashOnly(moduleName);

    if (!parsingEmbeddedModule && IsEmbeddedModuleName(moduleName)) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "Module name '%s' is reserved by the embedded standard library. "
                 "Rename this module to a non-standard-library name, for example 'My%s', and update imports to use the new name. "
                 "Use 'import %s' when you want the embedded standard-library module.",
                 moduleName.c_str(), moduleName.c_str(), moduleName.c_str());
        ErrorAtPrevious(msg);
        SkipBracedDeclaration(true);
        return NodeRef::Null();
    }

    // Register module name in reverse lookup for qualified type name resolution
    ReverseLookup::Register(moduleNameArena.nameHash, moduleName.c_str());

    // Create module in symbol table. AddModule returns INVALID_INDEX on
    // duplicate declarations; in that case we stay out of module scope so
    // subsequent struct/enum/function parsing doesn't OOB-read modules[].
    u32 moduleIndex = SymbolTable::AddModule(&symbolTable, moduleNameArena);
    if (moduleIndex == INVALID_INDEX) {
        Error("Duplicate module declaration");
    } else {
        symbolTable.currentModuleIndex = moduleIndex;
        symbolTable.inModuleScope = true;
    }

    // Create module AST node
    NodeRef module = ASTFactory::MakeModule(ast, moduleName, line, col);
    AttachDocComment(module, declToken);
    NodeRef previousModule = currentModule;
    NodeRef previousPipeline = currentPipeline;
    currentModule = module;
    currentPipeline = NodeRef::Null();

    Consume(TokenType::LEFT_BRACE, "Expected '{'");

    // Parse module contents (imports, functions, and structs)
    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (Match(TokenType::IMPORT)) {
            ParseModuleImportList(module, false);
        } else if (Match(TokenType::USING)) {
            ParseUsingDeclaration(module, false);
        } else if (Match(TokenType::ATTRIBUTES)) {
            ParseAttributes(module, false);
        } else if (Match(TokenType::RESOURCES)) {
            if (ast->GetModule(module).resources.count > 0) {
                Error("Only one resources block is allowed per module");
                continue;
            }
            ParseResources(module, false);
        } else if (Match(TokenType::VARIANTS)) {
            if (ast->GetModule(module).variantDecls.count > 0 ||
                ast->GetModule(module).variantRules.count > 0) {
                Error("Only one variants block is allowed per module");
                continue;
            }
            ParseVariants(module, false);
        } else if (IsFunctionDeclStart()) {
            // Module function (may be generic or regular)
            NodeRef func = ParseFunction();
            if (func.IsValid()) {
                const FunctionDeclData& decl = ast->GetFunction(func);

                // Check if this is a generic function (already registered in ParseFunction)
                // by looking for it in genericFunctions array
                GenericFunctionData* gfn = SymbolTable::FindGenericFunction(&symbolTable, decl.name.nameHash);
                if (gfn != nullptr) {
                    // Generic function - already registered, just add to module's function list
                    ast->GetModule(module).functions.Push(arena, func);
                    // Note: Generic functions don't get AddModuleFunction symbol entry
                    // They'll be instantiated on demand during IR lowering
                } else {
                    // Regular function
                    std::vector<OverloadTypeMask> paramMasks;
                    BuildParamMasks(decl.parameters, paramMasks);
                    u64 signatureKey = HashOverloadSignature(paramMasks.data(),
                        static_cast<u32>(paramMasks.size()));

                    if (HasDuplicateFunctionInList(ast, ast->GetModule(module).functions,
                            decl, paramMasks, signatureKey)) {
                        Error("Function overload already declared");
                        continue;
                    }

                    ast->GetModule(module).functions.Push(arena, func);

                    // Add to symbol table with module prefix
                    Symbol* sym = SymbolTable::AddModuleFunction(&symbolTable,
                        decl.name, moduleNameArena);
                    if (!sym) {
                        Error("Function already declared");
                        continue;
                    }
                    FillFunctionData(&symbolTable, arena, decl, func, paramMasks, signatureKey, sym->index);
                }
            }
        } else if (Match(TokenType::STRUCT)) {
            // Module struct
            NodeRef structNode = ParseStruct();
            if (structNode.IsValid()) {
                ast->GetModule(module).structs.Push(arena, structNode);

                SymbolTable::AddModuleStruct(&symbolTable,
                    ast->GetStructDecl(structNode).name, moduleNameArena);
            }
            Match(TokenType::SEMICOLON); // Optional trailing semicolon after struct
        } else if (Match(TokenType::ENUM)) {
            // Module enum declaration
            NodeRef enumDecl = ParseEnum();
            if (enumDecl.IsValid()) {
                ast->GetModule(module).enums.Push(arena, enumDecl);

                std::string qualifiedEnumName = moduleName + "::" +
                    ast->GetEnumDecl(enumDecl).name.ToString(sourceBase());
                ReverseLookup::Register(Utils::HashStr(qualifiedEnumName.c_str()),
                                        qualifiedEnumName.c_str());

                // Register enum type with module prefix
                Symbol* qualifiedEnumSym = SymbolTable::AddModuleEnum(&symbolTable,
                    ast->GetEnumDecl(enumDecl).name, moduleNameArena);
                Symbol* enumSym = SymbolTable::LookupByHash(&symbolTable,
                    ast->GetEnumDecl(enumDecl).name.nameHash);
                if (qualifiedEnumSym && enumSym &&
                    (enumSym->kind == SymbolKind::ENUM || enumSym->kind == SymbolKind::ENUM_SYMBOL)) {
                    symbolTable.enums[qualifiedEnumSym->index] = symbolTable.enums[enumSym->index];
                }
            }
        } else if (Match(TokenType::CONST)) {
            // Module constant declaration (e.g., const float PI = 3.14)
            if (!MatchMask(TokenMasks::CORE_TYPES)) {
                Error("Expected type after 'const'");
                continue;
            }

            Consume(TokenType::IDENTIFIER, "Expected constant name");
            std::string constName(stream->GetValue(previous));

            Consume(TokenType::ASSIGN, "const variables must be initialized");

            NodeRef value = ParseExpression();
            if (!value.IsValid()) {
                Error("Expected initializer expression for const");
                continue;
            }

            Consume(TokenType::SEMICOLON, "Expected ';'");

            // Evaluate the constant value first
            LiteralValue constValue;
            bool hasValue = false;
            if (value.Type() == ASTNodeType::LITERAL) {
                constValue = ast->GetLiteral(value).value;
                hasValue = true;
            } else {
                Error("Module constants must have literal initializers");
            }

            if (hasValue) {
                // Register with qualified name: Module::constName for external access (GLOBAL namespace)
                std::string qualifiedName = moduleName + "::" + constName;
                ArenaString qualifiedNameArena = ArenaString::MakeHashOnly(qualifiedName);
                Symbol* sym = SymbolTable::AddSymbol(&symbolTable, qualifiedNameArena, SymbolKind::EVAL_CONSTANT,
                                                      NamespaceKind::GLOBAL, INVALID_INDEX);
                if (sym) {
                    symbolTable.evalConstants[sym->index] = constValue;
                }
                
                // Also register with local name for module-internal access (MODULE namespace)
                ArenaString localNameArena = ArenaString::MakeHashOnly(constName);
                Symbol* localSym = SymbolTable::AddSymbol(&symbolTable, localNameArena, SymbolKind::EVAL_CONSTANT,
                                                          NamespaceKind::MODULE, symbolTable.currentModuleIndex);
                if (localSym) {
                    symbolTable.evalConstants[localSym->index] = constValue;
                }
            }
        } else {
            ErrorAtCurrent("Expected import, function, or struct declaration in module");
            Advance();
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}'");

    symbolTable.inModuleScope = false;
    symbolTable.currentModuleIndex = INVALID_INDEX;
    currentModule = previousModule;
    currentPipeline = previousPipeline;

    return module;
}

//==============================================================================
// Constraint and generics parsing
//==============================================================================

NodeRef Parser::ParseConstraint() {
    Consume(TokenType::CONSTRAINT, "Expected 'constraint'");
    Consume(TokenType::IDENTIFIER, "Expected constraint name");

    // Extract name from token
    ArenaString name = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));

    SourceLocation loc = getLocation(stream->GetOffset(previous));

    Consume(TokenType::ASSIGN, "Expected '=' after constraint name");

    TypeMask allowedTypes = ParseConstraintTypeExpression();
    if (allowedTypes == 0) {
        return NodeRef::Null();
    }

    // Try to add to symbol table
    ArenaString conflictingModule;
    SymbolTable::AddConstraintResult result =
        SymbolTable::AddConstraint(&symbolTable, name, allowedTypes, &conflictingModule);

    switch (result) {
        case SymbolTable::AddConstraintResult::SUCCESS:
            break;

        case SymbolTable::AddConstraintResult::DUPLICATE_IN_MODULE:
            Error("Constraint '" + name.ToString(sourceBase()) + "' is already defined in this module");
            return NodeRef::Null();

        case SymbolTable::AddConstraintResult::DUPLICATE_FROM_IMPORT:
            Error("Constraint '" + name.ToString(sourceBase()) + "' conflicts with imported constraint from module '" +
                  conflictingModule.ToString(sourceBase()) + "'");
            return NodeRef::Null();

        case SymbolTable::AddConstraintResult::DUPLICATE_IN_SCOPE:
            Error("Constraint '" + name.ToString(sourceBase()) + "' is already defined in the current scope");
            return NodeRef::Null();
    }

    Consume(TokenType::SEMICOLON, "Expected ';' after constraint definition");

    return ASTFactory::MakeConstraint(ast, name, allowedTypes, loc.line, loc.column);
}

TypeMask Parser::ParseConstraintTypeExpression() {
    TypeMask result = ParseConstraintType();

    if (result == 0) {
        Error("Constraint must include at least one type. "
              "Example: 'constraint MyConstraint = float2 | float3;'");
        return 0;
    }

    while (Match(TokenType::BITWISE_OR)) {
        TypeMask nextType = ParseConstraintType();
        if (nextType == 0) {
            Error("Expected type after '|' in constraint expression");
            break;
        }
        result |= nextType;
    }

    return result;
}

TypeMask Parser::ParseConstraintType() {
    // Check if it's a core type token
    if (MatchMask(TokenMasks::CORE_TYPES)) {
        TokenType typeToken = PreviousTokenType();
        TypeInfo typeInfo = GetTypeInfoFromToken(typeToken);
        return mask(typeInfo.coreType);
    }

    // Check if it's a constraint reference (identifier)
    if (Match(TokenType::IDENTIFIER)) {
        ArenaString constraintName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));

        // Look up the constraint and get its mask
        TypeMask constraintMask = SymbolTable::LookupConstraint(&symbolTable, constraintName);

        if (constraintMask == 0) {
            ErrorAtPrevious(std::string("Unknown constraint: ") + std::string(constraintName.view(sourceBase())));
            return 0;
        }

        return constraintMask;
    }

    ErrorAtPrevious("Expected type or constraint reference");
    return 0;
}

NodeRef Parser::ParseWhereClause() {
    /*
      scale :: (T v, float s) -> T where T is FloatVectors {
        return v * s;
    }
    */

    Consume(TokenType::WHERE, "Expected 'where' keyword");
    Consume(TokenType::T, "Expected type T in where clause");
    Consume(TokenType::IS, "Expected 'is' keyword");
    Consume(TokenType::IDENTIFIER, "Expected type parameter constraint");
    std::string_view typeParamConstraint = stream->GetValue(previous);
    (void)typeParamConstraint; // Reserved for future use
    Consume(TokenType::LEFT_BRACE, "Expected '{' after type parameter constraint");
    // Not implemented yet - generics with where clauses are future work
    return NodeRef::Null();
}

NodeRef Parser::ParseGenericParams() {
    // Generic parameters parsing: <T, U>
    // Not yet implemented - reserved for future generics support
    Error("Generic parameters are not yet supported");
    return NodeRef::Null();
}

NodeRef Parser::ParseGenericFunction() {
    // Generic function parsing: func<T> :: (T arg) -> T { ... }
    // Not yet implemented - reserved for future generics support
    Error("Generic functions are not yet supported");
    return NodeRef::Null();
}

//==============================================================================
// Type resolution helpers
//==============================================================================

TypeInfo Parser::GetTypeInfoFromSymbol(Symbol* sym) {
    if (!sym) {
        return TYPE_INFO(CoreType::INVALID, 0, false);
    }

    switch (sym->kind) {
        case SymbolKind::CUSTOM_TYPE: {
            const StructData& structData = symbolTable.structs[sym->index];
            return TypeInfo{
                CoreType::CUSTOM,
                static_cast<u8>(structData.fields.count),
                structData.isIndexable,
                0, // _padding
                structData.name.nameHash, // customTypeHash
                0, // arrayLength
                0  // arrayStride
            };
        }

        case SymbolKind::VARIABLE: {
            const VariableData& varData = symbolTable.variables[sym->index];
            return varData.typeInfo;
        }

        case SymbolKind::ATTRIBUTE: {
            const AttributeData& attrData = symbolTable.attributes[sym->index];
            return attrData.typeInfo;
        }

        case SymbolKind::RESOURCE: {
            return MakeTypeInfoForResource(&symbolTable, symbolTable.resources[sym->index]);
        }

        case SymbolKind::EVAL_CONSTANT: {
            const LiteralValue& value = symbolTable.evalConstants[sym->index];
            switch (value.type) {
                case LiteralValue::FLOAT: return TYPE_INFO(CoreType::FLOAT, 1, false);
                case LiteralValue::INT:   return TYPE_INFO(CoreType::INT, 1, false);
                case LiteralValue::BOOL:  return TYPE_INFO(CoreType::BOOL, 1, false);
                default: return TYPE_INFO(CoreType::INVALID, 0, false);
            }
        }

        case SymbolKind::FUNCTION: {
            const FunctionData& funcData = symbolTable.functions[sym->index];
            return TYPE_INFO(funcData.returnType, 1, false);
        }

        default:
            return TYPE_INFO(CoreType::INVALID, 0, false);
    }
}

TypeInfo Parser::ResolveType(const std::string& typeName) {
    // Fast path 1: If we just parsed a type token, use direct lookup
    if (stream->GetType(previous) >= TokenType::FLOAT && static_cast<u8>(stream->GetType(previous)) <= static_cast<u8>(TokenType::VOID)) {
        TypeInfo info = GetTypeInfoFromToken(static_cast<TokenType>(stream->GetType(previous)));
        if (info.coreType != CoreType::INVALID) {
            return info;  // O(1), no hashing, no caching needed
        }
    }

    // Fast path 2: Hash once and check cache
    u32 typeHash = Utils::HashStr(typeName.c_str());

    if (TypeInfo* cached = typeCache.Find(typeHash)) {
        return *cached;  // Cache hit
    }

    std::string canonicalTypeName = CanonicalizeTypeName(typeName);
    if (canonicalTypeName != typeName) {
        TypeInfo resolved = ResolveType(canonicalTypeName);
        typeCache.Insert(typeHash, resolved);
        return resolved;
    }

    u32 resolvedAliasHash = SymbolTable::ResolveTypeAliasHash(&symbolTable, typeHash);
    if (resolvedAliasHash != typeHash) {
        std::string resolvedName = ReverseLookup::GetString(resolvedAliasHash);
        TypeInfo resolved = ResolveType(resolvedName);
        typeCache.Insert(typeHash, resolved);
        return resolved;
    }

    // Fast path 3: Check pre-computed core type hashes. GENERIC_T/U/V are
    // the short names `T`, `U`, `V` — but a user may have defined a
    // struct or enum with exactly that name (real-world shader code uses
    // `V` for a vertex struct, etc.). A user-defined symbol takes
    // precedence over the generic placeholder, which is only meaningful
    // inside generic-function / constraint contexts.
    for (u32 i = 0; i < TypeHashes::HASH_TABLE_SIZE; i++) {
        if (TypeHashes::HASH_TABLE[i].hash == typeHash) {
            const TypeInfo& info = TypeHashes::HASH_TABLE[i].info;
            if (info.coreType == CoreType::GENERIC_T ||
                info.coreType == CoreType::GENERIC_U ||
                info.coreType == CoreType::GENERIC_V) {
                Symbol* userSym = SymbolTable::LookupByHash(&symbolTable, typeHash);
                if (userSym && (userSym->kind == SymbolKind::CUSTOM_TYPE ||
                                userSym->kind == SymbolKind::ENUM ||
                                userSym->kind == SymbolKind::ENUM_SYMBOL)) {
                    break; // fall through to the custom-type path below
                }
            }
            typeCache.Insert(typeHash, info);
            return info;
        }
    }

    // Slow path: Custom types
    TypeInfo result;

    // Check for :: to determine if module-qualified
    const char* colonColon = strstr(typeName.c_str(), "::");

    if (!colonColon) {
        // Simple custom type lookup
        Symbol* sym = SymbolTable::LookupByHash(&symbolTable, typeHash);
        if (sym && sym->kind == SymbolKind::CUSTOM_TYPE) {
            result = GetTypeInfoFromSymbol(sym);
            typeCache.Insert(typeHash, result);
            return result;
        }
        if (sym && (sym->kind == SymbolKind::ENUM || sym->kind == SymbolKind::ENUM_SYMBOL)) {
            EnumData& enumData = symbolTable.enums[sym->index];
            if (enumData.flags & EnumData::IS_SUM_TYPE) {
                result = TypeInfo{CoreType::CUSTOM, 1, 0, 0, enumData.name.nameHash, 0, 0};
            } else {
                CoreType baseType = enumData.underlyingType;
                if (baseType == CoreType::INVALID) baseType = CoreType::INT;
                result = TYPE_INFO(baseType, 1, false);
            }
            typeCache.Insert(typeHash, result);
            return result;
        }

        // Try implicit module qualification if in module scope
        if (symbolTable.inModuleScope && symbolTable.currentModuleIndex != INVALID_INDEX) {
            const ModuleData& currentModule = symbolTable.modules[symbolTable.currentModuleIndex];

            // Build qualified hash efficiently
            u32 qualifiedHash = Utils::HashStr(currentModule.name.ToString(sourceBase()).c_str());
            qualifiedHash ^= Utils::HashStr("::");
            qualifiedHash ^= typeHash;

            sym = SymbolTable::LookupByHash(&symbolTable, qualifiedHash);
            if (sym && sym->kind == SymbolKind::CUSTOM_TYPE) {
                result = GetTypeInfoFromSymbol(sym);
                typeCache.Insert(typeHash, result);
                return result;
            }
        }
    } else {
        // Module-qualified type: Module::Type
        std::string moduleName(typeName.c_str(), colonColon - typeName.c_str());
        std::string localTypeName(colonColon + 2);
        ArenaString moduleArena = ArenaString::MakeHashOnly(moduleName);
        ArenaString localTypeArena = ArenaString::MakeHashOnly(localTypeName);
        u32 resolvedModuleHash = SymbolTable::ResolveModuleNameHash(&symbolTable, moduleArena.nameHash);

        std::string internalName = "m" + std::to_string(resolvedModuleHash) +
                                   "::s" + std::to_string(localTypeArena.nameHash);
        Symbol* sym = SymbolTable::LookupByHash(&symbolTable,
            Utils::HashStr(internalName.c_str()));
        if (!sym) {
            internalName = "m" + std::to_string(resolvedModuleHash) +
                           "::e" + std::to_string(localTypeArena.nameHash);
            sym = SymbolTable::LookupByHash(&symbolTable,
                Utils::HashStr(internalName.c_str()));
        }
        if (sym && sym->kind == SymbolKind::CUSTOM_TYPE) {
            result = GetTypeInfoFromSymbol(sym);
            result.customTypeHash = localTypeArena.nameHash;
            typeCache.Insert(typeHash, result);
            return result;
        }
        if (sym && (sym->kind == SymbolKind::ENUM || sym->kind == SymbolKind::ENUM_SYMBOL)) {
            EnumData& enumData = symbolTable.enums[sym->index];
            if (enumData.flags & EnumData::IS_SUM_TYPE) {
                result = TypeInfo{CoreType::CUSTOM, 1, 0, 0, enumData.name.nameHash, 0, 0};
            } else {
                CoreType baseType = enumData.underlyingType;
                if (baseType == CoreType::INVALID) baseType = CoreType::INT;
                result = TYPE_INFO(baseType, 1, false);
            }
            typeCache.Insert(typeHash, result);
            return result;
        }

        // First check symbol table for imported types
        sym = SymbolTable::LookupByHash(&symbolTable, typeHash);
        if (sym && sym->kind == SymbolKind::CUSTOM_TYPE) {
            result = GetTypeInfoFromSymbol(sym);
            typeCache.Insert(typeHash, result);
            return result;
        }
        if (sym && (sym->kind == SymbolKind::ENUM || sym->kind == SymbolKind::ENUM_SYMBOL)) {
            EnumData& enumData = symbolTable.enums[sym->index];
            if (enumData.flags & EnumData::IS_SUM_TYPE) {
                result = TypeInfo{CoreType::CUSTOM, 1, 0, 0, enumData.name.nameHash, 0, 0};
            } else {
                CoreType baseType = enumData.underlyingType;
                if (baseType == CoreType::INVALID) baseType = CoreType::INT;
                result = TYPE_INFO(baseType, 1, false);
            }
            typeCache.Insert(typeHash, result);
            return result;
        }
    }

    // Type not found
    return TYPE_INFO(CoreType::INVALID, 0, false);
}

} // namespace BWSL
