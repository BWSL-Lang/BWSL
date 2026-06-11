// Part of bwsl_parser_soa.cpp. Include from that file only.
// Blocks, statements, and local/custom variable declarations.
#pragma once
#include "bwsl_parser_soa.cpp"

//==============================================================================
// Block and statement parsing
//==============================================================================

namespace BWSL {

static bool IsOutputAssignmentTarget(AST* ast, NodeRef target) {
    if (!target.IsValid()) {
        return false;
    }

    if (target.Type() == ASTNodeType::ARRAY_ACCESS) {
        return IsOutputAssignmentTarget(ast, ast->GetArrayAccess(target).array);
    }

    if (target.Type() != ASTNodeType::MEMBER_ACCESS) {
        return false;
    }

    const MemberAccessData& access = ast->GetMemberAccess(target);
    if (access.object.Type() == ASTNodeType::IDENTIFIER) {
        const IdentifierData& obj = ast->GetIdentifier(access.object);
        return obj.identifierKind == SpecialIdentifier::OUTPUT;
    }

    return IsOutputAssignmentTarget(ast, access.object);
} 

NodeRef Parser::ParseBlock() { 
    PARSER_TIME_BLOCK();
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    NodeRef block = ASTFactory::MakeBlock(ast, loc.line, loc.column);

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        NodeRef stmt = ParseStatement();
        if (stmt.IsValid()) {
            ast->GetBlock(block).statements.Push(arena, stmt);
        }
    }

    if (Consume(TokenType::RIGHT_BRACE, "Expected '}' after block")) {
        MarkNodeEndAtPreviousToken(block);
    }

    return block;
}

NodeRef Parser::ParseStatement() {
    PARSER_TIME_STMT();
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;

    if (Match(TokenType::AT)) {
        u32 decoHash = Utils::HashStr(stream->GetValue(previous).data(),
                                      static_cast<u16>(stream->GetLength(previous)));
        InterpolationMode interpolation = InterpolationMode::Default;
        if (decoHash == Utils::HashStr("flat")) {
            interpolation = InterpolationMode::Flat;
        } else if (decoHash == Utils::HashStr("noperspective")) {
            interpolation = InterpolationMode::NoPerspective;
        } else {
            Error("Expected @flat or @noperspective before output assignment");
            return NodeRef::Null();
        }

        NodeRef stmt = ParseStatement();
        if (!stmt.IsValid()) {
            return stmt;
        }
        if (stmt.Type() != ASTNodeType::ASSIGNMENT ||
            !IsOutputAssignmentTarget(ast, ast->GetAssignment(stmt).target)) {
            Error("@flat and @noperspective can only decorate output assignments");
            return stmt;
        }

        AssignmentData& assign = ast->GetAssignment(stmt);
        if (assign.interpolation != InterpolationMode::Default &&
            assign.interpolation != interpolation) {
            Error("Conflicting interpolation decorators on output assignment");
            return stmt;
        }
        assign.interpolation = interpolation;
        return stmt;
    }

    if (Match(TokenType::EVAL)) {
        if (Check(TokenType::LEFT_BRACE)) {
            return ParseEvalBlock();
        }
        if (Check(TokenType::FOR) || Check(TokenType::FOREACH)) {
            Advance();
            return ParseForStatement(true);
        }
        if (Check(TokenType::LOOP)) {
            Advance();
            return ParseLoopStatement(true);
        }
        if (Check(TokenType::WHILE)) {
            Advance();
            return ParseWhileStatement(true);
        }
        if (Check(TokenType::IF)) {
            Advance();
            return ParseEvalIf();
        }
        return ParseEvalStatement();
    }

    if (Match(TokenType::RETURN)) {
        if (Match(TokenType::IF)) {
            Consume(TokenType::LEFT_PAREN, "Expected '(' after 'return if'");
            NodeRef condition = ParseExpression();
            Consume(TokenType::RIGHT_PAREN, "Expected ')' after condition");

            // Check for optional return value
            NodeRef returnValue = NodeRef::Null();
            if (!Check(TokenType::SEMICOLON)) {
                returnValue = ParseExpression();
            }
            Consume(TokenType::SEMICOLON, "Expected ';' after return if");

            NodeRef ifNode = ASTFactory::MakeIfStatement(ast, line, col);
            ast->GetBlock(ifNode).statements.Push(arena, condition);

            NodeRef body = ASTFactory::MakeBlock(ast, line, col);
            ast->GetBlock(body).statements.Push(arena, ASTFactory::MakeReturn(ast, returnValue, line, col));
            ast->GetBlock(ifNode).statements.Push(arena, body);

            return ifNode;
        }

        NodeRef value = NodeRef::Null();
        if (!Check(TokenType::SEMICOLON)) {
            value = ParseExpression();

            // Check for trailing 'if (condition)' syntax: return value if (condition);
            if (Match(TokenType::IF)) {
                Consume(TokenType::LEFT_PAREN, "Expected '(' after 'if'");
                NodeRef condition = ParseExpression();
                Consume(TokenType::RIGHT_PAREN, "Expected ')' after condition");
                Consume(TokenType::SEMICOLON, "Expected ';' after return if");

                NodeRef ifNode = ASTFactory::MakeIfStatement(ast, line, col);
                ast->GetBlock(ifNode).statements.Push(arena, condition);

                NodeRef body = ASTFactory::MakeBlock(ast, line, col);
                ast->GetBlock(body).statements.Push(arena, ASTFactory::MakeReturn(ast, value, line, col));
                ast->GetBlock(ifNode).statements.Push(arena, body);

                return ifNode;
            }
        }
        Consume(TokenType::SEMICOLON, "Expected ';' after return statement");
        return ASTFactory::MakeReturn(ast, value, line, col);
    }

    if (Match(TokenType::BREAK)) {
        if (Match(TokenType::IF)) {
            Consume(TokenType::LEFT_PAREN, "Expected '(' after 'break if'");
            NodeRef condition = ParseExpression();
            Consume(TokenType::RIGHT_PAREN, "Expected ')' after condition");
            Consume(TokenType::SEMICOLON, "Expected ';' after break if");

            NodeRef ifNode = ASTFactory::MakeIfStatement(ast, line, col);
            ast->GetBlock(ifNode).statements.Push(arena, condition);

            NodeRef body = ASTFactory::MakeBlock(ast, line, col);
            ast->GetBlock(body).statements.Push(arena, NodeRef(ASTNodeType::BREAK_STATEMENT, 0));
            ast->GetBlock(ifNode).statements.Push(arena, body);

            return ifNode;
        }

        Consume(TokenType::SEMICOLON, "Expected ';' after break");
        return NodeRef(ASTNodeType::BREAK_STATEMENT, 0);  // No data needed
    }

    if (Match(TokenType::SKIP)) {
        if (Match(TokenType::IF)) {
            Consume(TokenType::LEFT_PAREN, "Expected '(' after 'skip if'");
            NodeRef condition = ParseExpression();
            Consume(TokenType::RIGHT_PAREN, "Expected ')' after condition");
            Consume(TokenType::SEMICOLON, "Expected ';' after skip if");

            NodeRef ifNode = ASTFactory::MakeIfStatement(ast, line, col);
            ast->GetBlock(ifNode).statements.Push(arena, condition);

            NodeRef body = ASTFactory::MakeBlock(ast, line, col);
            ast->GetBlock(body).statements.Push(arena, NodeRef(ASTNodeType::SKIP_STATEMENT, 0));
            ast->GetBlock(ifNode).statements.Push(arena, body);

            return ifNode;
        }

        Consume(TokenType::SEMICOLON, "Expected ';' after skip");
        return NodeRef(ASTNodeType::SKIP_STATEMENT, 0);  // No data needed
    }

    if (Match(TokenType::DISCARD)) {
        Consume(TokenType::SEMICOLON, "Expected ';' after discard");
        return NodeRef(ASTNodeType::DISCARD_STATEMENT, 0);  // No data needed
    }

    if (Match(TokenType::CONST)) {
        std::string typeStr;
        TypeInfo constType = TYPE_INFO(CoreType::INVALID, 0, false);
        if (MatchMask(TokenMasks::CORE_TYPES)) {
            TokenType varType = static_cast<TokenType>(stream->GetType(previous));
            typeStr = std::string(stream->GetValue(previous));
            constType = GetTypeInfoFromToken(varType);
        } else if (Match(TokenType::IDENTIFIER)) {
            typeStr = std::string(stream->GetValue(previous));
            if (Match(TokenType::DOUBLE_COLON)) {
                std::string moduleName = typeStr;
                Consume(TokenType::IDENTIFIER, "Expected type name after '::'");
                typeStr = CanonicalizeModuleQualifiedName(
                    moduleName, std::string(stream->GetValue(previous)));
            }
            constType = ResolveType(typeStr);
        } else {
            Error("Expected type after 'const'");
            return NodeRef::Null();
        }

        // Check for pointer type: int^ means pointer to int
        while (Match(TokenType::BITWISE_XOR)) {
            typeStr += "^";
        }

        Consume(TokenType::IDENTIFIER, "Expected variable name");
        std::string varName = std::string(stream->GetValue(previous));

        Consume(TokenType::ASSIGN, "const variables must be initialized");

        NodeRef value = ParseExpression();
        if (!value.IsValid()) {
            Error("Expected initializer expression for const variable");
            return NodeRef::Null();
        }

        Consume(TokenType::SEMICOLON, "Expected ';'");

        ArenaString varNameStr = ArenaString::MakeHashOnly(varName);
        NodeRef varDecl = ASTFactory::MakeVariableDecl(ast, varNameStr,
            ArenaString::MakeHashOnly(typeStr),
            value, true, line, col);

        Symbol* sym = SymbolTable::AddSymbol(&symbolTable, varNameStr, SymbolKind::VARIABLE);

        if (sym) {
            VariableData& varData = symbolTable.variables[sym->index];
            if (constType.coreType != CoreType::INVALID) {
                varData.typeInfo = constType;
            } else {
                varData.typeInfo.coreType = CoreType::CUSTOM;
                varData.typeInfo.componentCount = 1;
                varData.typeInfo.customTypeHash = Utils::HashStr(typeStr.c_str());
            }
            varData.isConst = true;
            varData.constExpr = value;

            LiteralValue constValue;
            if (constType.coreType != CoreType::INVALID &&
                EvaluateNodeWithEvalBindings(value, &constValue) &&
                CoerceLiteralToType(varData.typeInfo, &constValue)) {
                varData.isEval = true;
                varData.hasEvalValue = true;
                varData.evalValue = constValue;
            }
        } else {
            Error("Variable already declared in this scope");
            return NodeRef::Null();
        }

        return varDecl;
    }

    if (Match(TokenType::IF)) {
        // IF statement - statements[0]=condition, [1]=then-body, [2]=else-body (optional)
        // Supports both: if (cond) { ... } and if (cond) statement;
        NodeRef ifNode = ASTFactory::MakeIfStatement(ast, line, col);

        Consume(TokenType::LEFT_PAREN, "Expected '(' after 'if'");
        NodeRef condition = ParseExpression();
        ast->GetBlock(ifNode).statements.Push(arena, condition);
        Consume(TokenType::RIGHT_PAREN, "Expected ')' after condition");

        // Parse body - either a block or a single statement
        NodeRef body;
        if (Match(TokenType::LEFT_BRACE)) {
            body = ParseBlock();
        } else {
            // Single statement without braces - wrap in a synthetic block
            SourceLocation bodyLoc = getLocation(stream->GetOffset(current));
            body = ASTFactory::MakeBlock(ast, bodyLoc.line, bodyLoc.column);
            NodeRef stmt = ParseStatement();
            if (stmt.IsValid()) {
                ast->GetBlock(body).statements.Push(arena, stmt);
            }
        }
        ast->GetBlock(ifNode).statements.Push(arena, body);

        if (Match(TokenType::ELSE)) {
            if (Check(TokenType::IF)) {
                // else if - parse as nested if statement
                NodeRef elseIfBody = ParseStatement();
                ast->GetBlock(ifNode).statements.Push(arena, elseIfBody);
            } else if (Match(TokenType::LEFT_BRACE)) {
                // else { ... }
                NodeRef elseBody = ParseBlock();
                ast->GetBlock(ifNode).statements.Push(arena, elseBody);
            } else {
                // else statement; (single statement without braces)
                SourceLocation elseLoc = getLocation(stream->GetOffset(current));
                NodeRef elseBody = ASTFactory::MakeBlock(ast, elseLoc.line, elseLoc.column);
                NodeRef elseStmt = ParseStatement();
                if (elseStmt.IsValid()) {
                    ast->GetBlock(elseBody).statements.Push(arena, elseStmt);
                }
                ast->GetBlock(ifNode).statements.Push(arena, elseBody);
            }
        }

        return ifNode;
    }

    if (Match(TokenType::FOR) || Match(TokenType::FOREACH)) {
        return ParseForStatement(false);
    }

    if (Match(TokenType::LOOP)) {
        return ParseLoopStatement(false);
    }

    if (Match(TokenType::WHILE)) {
        return ParseWhileStatement(false);
    }

    if (Match(TokenType::SWITCH)) {
        return ParseSwitch();
    }

    // Check for identifier-starting statements
    if (Check(TokenType::IDENTIFIER)) {
        TokenRef next = PeekNext();
        if (stream->GetType(next) == TokenType::ASSIGN || stream->GetType(next) == TokenType::PLUS_ASSIGN ||
            stream->GetType(next) == TokenType::MINUS_ASSIGN || stream->GetType(next) == TokenType::MULTIPLY_ASSIGN ||
            stream->GetType(next) == TokenType::DIVIDE_ASSIGN || stream->GetType(next) == TokenType::MODULO_ASSIGN ||
            stream->GetType(next) == TokenType::BITWISE_AND_ASSIGN || stream->GetType(next) == TokenType::BITWISE_OR_ASSIGN ||
            stream->GetType(next) == TokenType::BITWISE_XOR_ASSIGN || stream->GetType(next) == TokenType::LEFT_SHIFT_ASSIGN ||
            stream->GetType(next) == TokenType::RIGHT_SHIFT_ASSIGN) {
            NodeRef expr = ParseExpression();
            if (expr.IsValid()) Consume(TokenType::SEMICOLON, "Expected ';' after assignment");
            return expr;
        } else if (stream->GetType(next) == TokenType::LEFT_PAREN) {
            Advance(); // consume the identifier
            NodeRef identifierNode = ASTFactory::MakeIdentifier(ast, std::string(stream->GetValue(previous)), line, col);
            Consume(TokenType::LEFT_PAREN, "Expected '(' after function name");
            NodeRef call = ParseFunctionCall(identifierNode);
            if (call.IsValid()) Consume(TokenType::SEMICOLON, "Expected ';' after function call");
            return call;
        } else if (stream->GetType(next) == TokenType::IDENTIFIER) {
            // Could be custom type variable declaration: TypeName varName;
            return ParseCustomTypeVarDecl();
        } else if (stream->GetType(next) == TokenType::LEFT_BRACKET) {
            // Could be custom type array declaration: TypeName[size] varName;
            TokenRef sizeTok = current + 2;
            TokenRef rightTok = current + 3;
            TokenRef nameTok = current + 4;
            if (nameTok < stream->Count() &&
                stream->GetType(sizeTok) == TokenType::NUMBER &&
                stream->GetType(rightTok) == TokenType::RIGHT_BRACKET &&
                stream->GetType(nameTok) == TokenType::IDENTIFIER) {
                return ParseCustomTypeVarDecl();
            }
        } else if (stream->GetType(next) == TokenType::DOUBLE_COLON) {
            // Could be module-qualified type: Module::Type varName;
            // ParseCustomTypeVarDecl handles the full pattern
            return ParseCustomTypeVarDecl();
        } else if (stream->GetType(next) == TokenType::BITWISE_XOR) {
            // Could be pointer-to-custom-type declaration: `TypeName^ varName`.
            // Distinguish from an XOR expression (`a ^ b`) by peeking: a var
            // decl has `^`+ followed by IDENTIFIER then `=` or `;`. Anything
            // else (e.g. literal RHS) keeps the default expression path.
            TokenRef probe = current + 1;
            while (probe < stream->Count() && stream->GetType(probe) == TokenType::BITWISE_XOR) {
                probe++;
            }
            if (probe < stream->Count() && stream->GetType(probe) == TokenType::IDENTIFIER) {
                TokenRef afterName = probe + 1;
                if (afterName < stream->Count() &&
                    (stream->GetType(afterName) == TokenType::ASSIGN ||
                     stream->GetType(afterName) == TokenType::SEMICOLON)) {
                    return ParseCustomTypeVarDecl();
                }
            }
        }
    }

    if (Match(TokenType::LEFT_BRACE)) {
        return ParseBlock();
    }

    if (Check(TokenType::SEMICOLON)) {
        Advance();
        return NodeRef::Null();
    }

    // Variable declaration
    StorageClass storageClass = StorageClass::Default;
    bool hasStorageClass = false;
    if (Match(TokenType::SHARED)) {
        storageClass = StorageClass::Shared;
        hasStorageClass = true;
    }

    if (hasStorageClass || MatchMask(TokenMasks::CORE_TYPES)) {
        if (hasStorageClass && !MatchMask(TokenMasks::CORE_TYPES)) {
            Error("Expected type after 'shared'");
            return NodeRef::Null();
        }

        TokenType varType = static_cast<TokenType>(stream->GetType(previous));
        std::string typeStr(stream->GetValue(previous));

        // Check for pointer type: int^ means pointer to int
        while (Match(TokenType::BITWISE_XOR)) {
            typeStr += "^";
        }

        if (Check(TokenType::LEFT_BRACKET)) {
            Advance();
            return ParseArrayDeclaration(TokenTypeToReturnType(varType), storageClass);
        }

        Consume(TokenType::IDENTIFIER, "Expected variable name");
        std::string varName(stream->GetValue(previous));

        bool isArray = false;
        u32 arraySize = 0;
        std::vector<u32> arrayDims;
        while (Match(TokenType::LEFT_BRACKET)) {
            u32 size = 0;
            if (!ParseArraySizeValue(&size)) {
                return NodeRef::Null();
            }
            Consume(TokenType::RIGHT_BRACKET, "Expected ']' after array size");
            arrayDims.push_back(size);
        }

        if (!arrayDims.empty()) {
            u64 total = 1;
            for (u32 dim : arrayDims) {
                if (dim == 0 || total > (MAX_ARRAY_SIZE / dim)) {
                    Error("Invalid array size. Max 256k elements");
                    return NodeRef::Null();
                }
                total *= dim;
            }
            isArray = true;
            arraySize = static_cast<u32>(total);
        }

        NodeRef initializer = NodeRef::Null();
        if (Match(TokenType::ASSIGN)) {
            // Brace-list initializer is legal for arrays in this decl
            // form (`int arr[4] = { 10, 20, 30, 40 }`). Route to the
            // dedicated array-init parser when we see `{`; otherwise
            // parse a scalar/expression initializer as before.
            if (isArray && Check(TokenType::LEFT_BRACE)) {
                initializer = ParseArrayInitializer();
            } else {
                initializer = ParseExpression();
            }
        }

        if (storageClass == StorageClass::Shared && isArray && initializer.IsValid()) {
            Error("Shared arrays cannot have initializers");
            return NodeRef::Null();
        }

        Consume(TokenType::SEMICOLON, "Expected ';'");

        ArenaString typeName = isArray ? ArenaString::MakeHashOnly("array")
                                       : ArenaString::MakeHashOnly(typeStr);
        u8 arrayDimCount = isArray ? static_cast<u8>(arrayDims.size()) : 0;
        u32 arrayLen = isArray ? arraySize : 0;
        u32 elementTypeHash = isArray ? ArenaString::MakeHashOnly(typeStr).nameHash : 0;
        NodeRef varDecl = ASTFactory::MakeVariableDecl(ast,
            ArenaString::MakeHashOnly(varName),
            typeName,
            initializer, false, line, col, storageClass, arrayDimCount, arrayLen,
            elementTypeHash);

        Symbol* sym = SymbolTable::AddSymbol(&symbolTable, ArenaString::MakeHashOnly(varName), SymbolKind::VARIABLE);
        if (sym) {
            VariableData& varData = symbolTable.variables[sym->index];
            if (isArray) {
                TypeInfo arrayInfo{};
                arrayInfo.coreType = TokenTypeToReturnType(varType);
                arrayInfo.componentCount = GetTypeInfoFromToken(varType).componentCount;
                arrayInfo.arrayDimensions = static_cast<u8>(arrayDims.size());
                arrayInfo.customTypeHash = 0;
                arrayInfo.arrayLength = arraySize;
                arrayInfo.arrayStride = static_cast<u32>(arrayInfo.componentCount) * 4u;
                varData.typeInfo = arrayInfo;
            } else {
                varData.typeInfo = GetTypeInfoFromToken(varType);
            }
            varData.storageClass = storageClass;
        }

        if (arrayDims.size() > 1) {
            multiDimArrayDims[ArenaString::MakeHashOnly(varName).nameHash] = arrayDims;
        }

        return varDecl;
    }

    // Expression statement
    NodeRef exprNode = ParseExpression();
    if (exprNode.IsValid()) {
        Consume(TokenType::SEMICOLON, "Expected ';' after expression");
    } else {
        ErrorAtCurrent("Expected expression or statement");
        if (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
            Advance();
        }
    }

    return exprNode;
}

//==============================================================================
// Custom type variable declaration parsing
//==============================================================================

static bool IsRemovedMatrixAlias(const std::string& typeName) {
    return typeName == "float2x2" || typeName == "float3x3" || typeName == "float4x4";
}

NodeRef Parser::ParseCustomTypeVarDecl() {
    SourceLocation loc = getLocation(stream->GetOffset(current));
    u32 line = loc.line;
    u32 col = loc.column;

    std::string typeName;
    bool isModuleQualified = false;

    // Check for module-qualified type: Module::Type
    if (Check(TokenType::IDENTIFIER) && stream->GetType(PeekNext()) == TokenType::DOUBLE_COLON) {
        Advance(); // consume module name
        std::string moduleName(stream->GetValue(previous));
        Advance(); // consume ::
        Consume(TokenType::IDENTIFIER, "Expected type name after '::'");
        std::string localTypeName(stream->GetValue(previous));
        typeName = CanonicalizeModuleQualifiedName(moduleName, localTypeName);
        isModuleQualified = true;
    } else {
        // Simple type name
        Consume(TokenType::IDENTIFIER, "Expected type name");
        typeName = std::string(stream->GetValue(previous));
    }

    if (IsRemovedMatrixAlias(typeName)) {
        Error("Matrix aliases float2x2/float3x3/float4x4 are not supported; use mat2, mat3, or mat4");
        Synchronize();
        return NodeRef::Null();
    }

    // Check for pointer type: `CustomType^ name` (matches core-type path in
    // function parameters). Without this, the statement-level dispatch parses
    // `Inner^ p = ...` as an XOR expression and silently drops the var decl,
    // leaving later `p^` dereferences to fall through to the zero-fallback in
    // LowerIdentifier and produce invalid SPIR-V.
    while (Match(TokenType::BITWISE_XOR)) {
        typeName += "^";
    }

    auto ParseArrayDims = [&](std::vector<u32>& dims) -> bool {
        do {
            u32 size = 0;
            if (!ParseArraySizeValue(&size)) {
                return false;
            }
            Consume(TokenType::RIGHT_BRACKET, "Expected ']' after array size");
            dims.push_back(size);
        } while (Match(TokenType::LEFT_BRACKET));
        return true;
    };

    std::vector<u32> arrayDims;
    if (Match(TokenType::LEFT_BRACKET)) {
        if (!ParseArrayDims(arrayDims)) {
            return NodeRef::Null();
        }
    }

    // Now expect variable name
    Consume(TokenType::IDENTIFIER, "Expected variable name");
    std::string varName(stream->GetValue(previous));

    if (arrayDims.empty() && Match(TokenType::LEFT_BRACKET)) {
        if (!ParseArrayDims(arrayDims)) {
            return NodeRef::Null();
        }
    }

    // Optional initializer
    NodeRef initializer = NodeRef::Null();
    if (Match(TokenType::ASSIGN)) {
        // Brace-list initializer is legal for arrays in this same
        // decl form (`int arr[4] = { 10, 20, 30, 40 }`). Route to the
        // dedicated array-init parser when we see `{`; otherwise parse
        // a scalar/expression initializer as before.
        if (!arrayDims.empty() && Check(TokenType::LEFT_BRACE)) {
            initializer = ParseArrayInitializer();
        } else {
            initializer = ParseExpression();
        }
    }

    Consume(TokenType::SEMICOLON, "Expected ';' after variable declaration");

    // Look up the type in symbol table to get struct info
    ArenaString typeArena = ArenaString::MakeHashOnly(typeName);
    Symbol* typeSym = nullptr;

    // For module-qualified types, track the unqualified name hash for signature matching
    u32 unqualifiedTypeHash = 0;

    if (isModuleQualified) {
        // For Module::Type, need to look up using internal naming scheme
        size_t colonPos = typeName.find("::");
        std::string moduleName = typeName.substr(0, colonPos);
        std::string localTypeName = typeName.substr(colonPos + 2);

        ArenaString moduleArena = ArenaString::MakeHashOnly(moduleName);
        ArenaString localTypeArena = ArenaString::MakeHashOnly(localTypeName);
        u32 resolvedModuleHash = SymbolTable::ResolveModuleNameHash(&symbolTable, moduleArena.nameHash);

        // Store the unqualified type hash for later use
        unqualifiedTypeHash = localTypeArena.nameHash;

        // Build internal qualified names. Structs and enums live in separate
        // synthetic namespaces (`::s` vs `::e`), but source uses the same
        // Module::Type spelling for both.
        std::string internalName = "m" + std::to_string(resolvedModuleHash) +
                                   "::s" + std::to_string(localTypeArena.nameHash);
        ArenaString internalArena = ArenaString::MakeHashOnly(internalName);
        typeSym = SymbolTable::LookupAny(&symbolTable, internalArena);
        if (!typeSym) {
            internalName = "m" + std::to_string(resolvedModuleHash) +
                           "::e" + std::to_string(localTypeArena.nameHash);
            internalArena = ArenaString::MakeHashOnly(internalName);
            typeSym = SymbolTable::LookupAny(&symbolTable, internalArena);
        }
    } else {
        typeSym = SymbolTable::LookupAny(&symbolTable, typeArena);
    }
    TypeInfo resolvedDeclType = ResolveType(typeName);

    u32 arrayElementTypeHash = 0;
    if (!arrayDims.empty()) {
        // For module-qualified types, use unqualified hash for consistent signature matching
        if (unqualifiedTypeHash != 0) {
            arrayElementTypeHash = unqualifiedTypeHash;
        } else if (typeSym) {
            arrayElementTypeHash = typeSym->name.nameHash;
        } else {
            arrayElementTypeHash = typeArena.nameHash;
        }
    }

    u32 totalSize = 0;
    if (!arrayDims.empty()) {
        u64 total = 1;
        for (u32 dim : arrayDims) {
            if (dim == 0 || total > (MAX_ARRAY_SIZE / dim)) {
                Error("Invalid array size. Max 256k elements");
                return NodeRef::Null();
            }
            total *= dim;
        }
        totalSize = static_cast<u32>(total);
    }

    // Create the variable declaration node
    NodeRef varDecl = NodeRef::Null();
    if (!arrayDims.empty()) {
        varDecl = ASTFactory::MakeVariableDecl(ast,
            ArenaString::MakeHashOnly(varName),
            ArenaString::MakeHashOnly("array"),
            initializer, false, line, col, StorageClass::Default,
            static_cast<u8>(arrayDims.size()), totalSize, arrayElementTypeHash);
    } else {
        varDecl = ASTFactory::MakeVariableDecl(ast,
            ArenaString::MakeHashOnly(varName),
            ArenaString::MakeHashOnly(typeName),
            initializer, false, line, col);
    }

    // Register variable in symbol table
    Symbol* varSym = SymbolTable::AddSymbol(&symbolTable,
        ArenaString::MakeHashOnly(varName), SymbolKind::VARIABLE);
    if (varSym) {
        if (resolvedDeclType.coreType != CoreType::INVALID) {
            symbolTable.variables[varSym->index].typeInfo = resolvedDeclType;
        } else if (typeSym && (typeSym->kind == SymbolKind::ENUM || typeSym->kind == SymbolKind::ENUM_SYMBOL)) {
            EnumData& enumData = symbolTable.enums[typeSym->index];
            if (enumData.flags & EnumData::IS_SUM_TYPE) {
                symbolTable.variables[varSym->index].typeInfo.coreType = CoreType::CUSTOM;
                symbolTable.variables[varSym->index].typeInfo.customTypeHash = enumData.name.nameHash;
            } else {
                CoreType baseType = enumData.underlyingType;
                if (baseType == CoreType::INVALID) {
                    baseType = CoreType::INT;
                }
                symbolTable.variables[varSym->index].typeInfo.coreType = baseType;
                symbolTable.variables[varSym->index].typeInfo.customTypeHash = 0;
            }
        } else {
            symbolTable.variables[varSym->index].typeInfo.coreType = CoreType::CUSTOM;
            if (typeSym && typeSym->kind == SymbolKind::CUSTOM_TYPE) {
                // For module-qualified types, use the unqualified hash for signature matching
                // This allows PBR::PBRMaterial variables to match PBRMaterial parameters
                if (unqualifiedTypeHash != 0) {
                    symbolTable.variables[varSym->index].typeInfo.customTypeHash = unqualifiedTypeHash;
                } else {
                    symbolTable.variables[varSym->index].typeInfo.customTypeHash = typeSym->name.nameHash;
                }
            }
        }
        if (!arrayDims.empty()) {
            VariableData& varData = symbolTable.variables[varSym->index];
            varData.typeInfo.arrayDimensions = static_cast<u8>(arrayDims.size());
            varData.typeInfo.arrayLength = totalSize;
            if (varData.typeInfo.coreType != CoreType::CUSTOM) {
                varData.typeInfo.arrayStride = static_cast<u32>(varData.typeInfo.componentCount) * 4u;
            } else {
                varData.typeInfo.arrayStride = 0;
            }
        }
    }

    if (arrayDims.size() > 1) {
        multiDimArrayDims[ArenaString::MakeHashOnly(varName).nameHash] = arrayDims;
    }

    return varDecl;
}

} // namespace BWSL
