// Part of bwsl_parser_soa.cpp. Include from that file only.
// Expressions, calls, postfix/member/array access, and assignment target validation.

NodeRef Parser::ParseExpression() {
    PARSER_TIME_EXPR();
    return ParseAssignment();
}

NodeRef Parser::ParseAssignment() {
    
    NodeRef expr = ParseTernary();  
    if (!expr.IsValid()) return NodeRef::Null();

    if (MatchMask(TokenMasks::ASSIGNMENT_OPERATORS)) {
        TokenType assignOp = PreviousTokenType();

        // Const checking
        if (expr.Type() == ASTNodeType::IDENTIFIER) {
            Symbol* sym = SymbolTable::LookupAny(&symbolTable, ast->GetIdentifier(expr).name);
            if (sym && sym->kind == SymbolKind::VARIABLE) {
                const VariableData& varData = symbolTable.variables[sym->index];
                if (varData.isConst && !varData.isEval) {
                    ErrorAtPrevious("Cannot assign to const variable");
                    return expr;
                }
            }
        } else if (expr.Type() == ASTNodeType::ARRAY_ACCESS) {
            NodeRef arrayBase = ast->GetArrayAccess(expr).array;
            if (arrayBase.Type() == ASTNodeType::IDENTIFIER) {
                Symbol* sym = SymbolTable::LookupAny(&symbolTable, ast->GetIdentifier(arrayBase).name);
                if (sym && sym->kind == SymbolKind::VARIABLE) {
                    const VariableData& varData = symbolTable.variables[sym->index];
                    if (varData.isConst && !varData.isEval) {
                        ErrorAtPrevious("Cannot modify const array");
                        return expr;
                    }
                }
            }
        }

        SourceLocation loc = getLocation(stream->GetOffset(previous));
        NodeRef value = ParseAssignment();

        // Desugar compound assignments: x += y  =>  x = x + y
        if (assignOp != TokenType::ASSIGN) {
            BinaryOpType binOp;
            switch (assignOp) {
                case TokenType::PLUS_ASSIGN:        binOp = BinaryOpType::ADD; break;
                case TokenType::MINUS_ASSIGN:       binOp = BinaryOpType::SUBTRACT; break;
                case TokenType::MULTIPLY_ASSIGN:    binOp = BinaryOpType::MULTIPLY; break;
                case TokenType::DIVIDE_ASSIGN:      binOp = BinaryOpType::DIVIDE; break;
                case TokenType::MODULO_ASSIGN:      binOp = BinaryOpType::MODULO; break;
                case TokenType::BITWISE_AND_ASSIGN: binOp = BinaryOpType::BITWISE_AND; break;
                case TokenType::BITWISE_OR_ASSIGN:  binOp = BinaryOpType::BITWISE_OR; break;
                case TokenType::BITWISE_XOR_ASSIGN: binOp = BinaryOpType::BITWISE_XOR; break;
                case TokenType::LEFT_SHIFT_ASSIGN:  binOp = BinaryOpType::LEFT_SHIFT; break;
                case TokenType::RIGHT_SHIFT_ASSIGN: binOp = BinaryOpType::RIGHT_SHIFT; break;
                default: binOp = BinaryOpType::ADD; break;  // Fallback
            }
            // Create binary operation: expr op value
            value = ASTFactory::MakeBinaryOp(ast, binOp, expr, value, loc.line, loc.column);
        }

        return ASTFactory::MakeAssignment(ast, expr, value, loc.line, loc.column);
    }

    return expr;
}

NodeRef Parser::ParseTernary() {
    NodeRef cond = ParseOr();
    if (Match(TokenType::QUESTION)) {
        SourceLocation loc = getLocation(stream->GetOffset(previous));
        NodeRef trueExpr = ParseExpression();
        Consume(TokenType::COLON, "Expected ':' in ternary");
        NodeRef falseExpr = ParseTernary();  // Right-associative
        return ASTFactory::MakeTernaryExpr(ast, cond, trueExpr, falseExpr, loc.line, loc.column);
    }
    return cond;
}

// Binary operator parsing using macros for common patterns
#define PARSE_BINARY_OP(name, nextLevel, ...) \
NodeRef Parser::name() { \
    NodeRef left = nextLevel(); \
    if (!left.IsValid()) return NodeRef::Null(); \
    while (MatchMask(__VA_ARGS__)) { \
        BinaryOpType op = TokenTypeToBinaryOp(PreviousTokenType()); \
        SourceLocation loc = getLocation(stream->GetOffset(previous)); \
        NodeRef right = nextLevel(); \
        if (!right.IsValid()) return NodeRef::Null(); \
        left = ASTFactory::MakeBinaryOp(ast, op, left, right, loc.line, loc.column); \
    } \
    return left; \
}

PARSE_BINARY_OP(ParseOr, ParseAnd, mask(TokenType::OR))
PARSE_BINARY_OP(ParseAnd, ParseBitwiseOr, mask(TokenType::AND))
PARSE_BINARY_OP(ParseBitwiseOr, ParseBitwiseXor, mask(TokenType::BITWISE_OR))
PARSE_BINARY_OP(ParseBitwiseXor, ParseBitwiseAnd, mask(TokenType::BITWISE_XOR))
PARSE_BINARY_OP(ParseBitwiseAnd, ParseEquality, mask(TokenType::BITWISE_AND))
PARSE_BINARY_OP(ParseEquality, ParseComparison, mask(TokenType::EQUALS) | mask(TokenType::NOT_EQUALS))
PARSE_BINARY_OP(ParseComparison, ParseBitwiseShift, TokenMasks::COMPARISON_OPERATORS)
PARSE_BINARY_OP(ParseBitwiseShift, ParseTerm, mask(TokenType::LEFT_SHIFT) | mask(TokenType::RIGHT_SHIFT))
PARSE_BINARY_OP(ParseTerm, ParseFactor, mask(TokenType::PLUS) | mask(TokenType::MINUS))
PARSE_BINARY_OP(ParseFactor, ParseUnary, mask(TokenType::MULTIPLY) | mask(TokenType::DIVIDE) | mask(TokenType::MODULO))

#undef PARSE_BINARY_OP

NodeRef Parser::ParseUnary() {
    SourceLocation loc = getLocation(stream->GetOffset(previous));

    if (Match(TokenType::NOT)) {
        NodeRef operand = ParseUnary();
        if (!operand.IsValid()) return NodeRef::Null();
        return ASTFactory::MakeUnaryOp(ast, UnaryOpType::NOT, operand, loc.line, loc.column);
    }

    if (Match(TokenType::PLUS)) {
        NodeRef operand = ParseUnary();
        if (!operand.IsValid()) return NodeRef::Null();
        return operand;  // Unary plus is a no-op
    }

    if (Match(TokenType::MINUS)) {
        NodeRef operand = ParseUnary();
        if (!operand.IsValid()) return NodeRef::Null();
        return ASTFactory::MakeUnaryOp(ast, UnaryOpType::NEGATE, operand, loc.line, loc.column);
    }

    // Prefix increment: ++x
    if (Match(TokenType::INCREMENT)) {
        NodeRef operand = ParseUnary();
        if (!operand.IsValid()) return NodeRef::Null();
        return ASTFactory::MakeUnaryOp(ast, UnaryOpType::PRE_INCREMENT, operand, loc.line, loc.column);
    }

    // Prefix decrement: --x
    if (Match(TokenType::DECREMENT)) {
        NodeRef operand = ParseUnary();
        if (!operand.IsValid()) return NodeRef::Null();
        return ASTFactory::MakeUnaryOp(ast, UnaryOpType::PRE_DECREMENT, operand, loc.line, loc.column);
    }

    // Bitwise NOT: ~x
    if (Match(TokenType::BITWISE_NOT)) {
        NodeRef operand = ParseUnary();
        if (!operand.IsValid()) return NodeRef::Null();
        return ASTFactory::MakeUnaryOp(ast, UnaryOpType::BITWISE_NOT, operand, loc.line, loc.column);
    }

    // Address-of operator: ^x
    if (Match(TokenType::BITWISE_XOR)) {
        NodeRef operand = ParseUnary();
        if (!operand.IsValid()) return NodeRef::Null();
        return ASTFactory::MakeUnaryOp(ast, UnaryOpType::ADDRESS_OF, operand, loc.line, loc.column);
    }

    return ParsePostfix();
}

NodeRef Parser::ParsePostfix() {
    NodeRef expr = ParsePrimary();
    if (!expr.IsValid()) return NodeRef::Null();

    while (true) {
        if (Match(TokenType::LEFT_PAREN)) {
            expr = ParseFunctionCall(expr);
        } else if (Match(TokenType::DOT)) {
            expr = ParseMemberAccess(expr);
        } else if (Match(TokenType::LEFT_BRACKET)) {
            expr = ParseArrayAccess(expr);
        } else if (Match(TokenType::DOUBLE_COLON)) {
            // Module-qualified access (e.g., Math::PI, ModuleName::function())
            // or chained enum access (e.g., Globals::LightType::Directional)
            SourceLocation loc = getLocation(stream->GetOffset(previous));

            // Case 1: Module::Member (expr is IDENTIFIER - the module name)
            if (expr.Type() == ASTNodeType::IDENTIFIER) {
                const ArenaString& moduleName = ast->GetIdentifier(expr).name;

                // Get the qualified member name
                Consume(TokenType::IDENTIFIER, "Expected identifier after '::'");
                std::string memberName(stream->GetValue(previous));
                ArenaString memberArena = ArenaString::MakeHashOnly(memberName);

                // Check for local enum access: EnumType::Variant
                Symbol* enumSym = SymbolTable::LookupByHash(&symbolTable, moduleName.nameHash);
                if (enumSym && (enumSym->kind == SymbolKind::ENUM || enumSym->kind == SymbolKind::ENUM_SYMBOL)) {
                    EnumData& enumData = symbolTable.enums[enumSym->index];
                    if (!(enumData.flags & EnumData::IS_SUM_TYPE)) {
                        u32 variantHash = memberArena.nameHash;
                        bool found = false;
                        u32 variantValue = 0;
                        for (u32 i = 0; i < enumData.variants.count; i++) {
                            if (enumData.variants[i].name.nameHash == variantHash) {
                                variantValue = enumData.variants[i].value;
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            Error("Unknown enum variant '" + memberName + "'");
                            return NodeRef::Null();
                        }

                        CoreType baseType = enumData.underlyingType;
                        if (baseType == CoreType::UINT) {
                            return ASTFactory::MakeLiteralUint(ast, variantValue, loc.line, loc.column);
                        }
                        return ASTFactory::MakeLiteralInt(ast, static_cast<int64_t>(variantValue), loc.line, loc.column);
                    }
                }

                // Look up in symbol table for module constants
                u32 moduleIdx = ResolveModuleIndexByWrittenName(moduleName);
                if (moduleIdx != INVALID_INDEX) {
                    // Check for module constant using Lookup with MODULE namespace
                    Symbol* sym = SymbolTable::Lookup(&symbolTable, memberArena,
                                                       NamespaceKind::MODULE, moduleIdx);

                    // Check for EVAL_CONSTANT first (module-level const declarations)
                    if (sym && sym->kind == SymbolKind::EVAL_CONSTANT) {
                        const LiteralValue& constVal = symbolTable.evalConstants[sym->index];
                        switch (constVal.type) {
                            case LiteralValue::FLOAT:
                                return ASTFactory::MakeLiteralFloat(ast, constVal.floatValue, loc.line, loc.column);
                            case LiteralValue::INT:
                                return ASTFactory::MakeLiteralInt(ast, constVal.intValue, loc.line, loc.column);
                            case LiteralValue::UINT:
                                return ASTFactory::MakeLiteralUint(ast, constVal.uintValue, loc.line, loc.column);
                            case LiteralValue::BOOL:
                                return ASTFactory::MakeLiteralBool(ast, constVal.boolValue, loc.line, loc.column);
                            default:
                                break;
                        }
                    }
                    // Also check for VARIABLE kind (for const variables declared inside functions/scopes)
                    else if (sym && sym->kind == SymbolKind::VARIABLE) {
                        const VariableData& varData = symbolTable.variables[sym->index];
                        if (varData.hasEvalValue) {
                            // Replace with literal value
                            switch (varData.evalValue.type) {
                                case LiteralValue::FLOAT:
                                    return ASTFactory::MakeLiteralFloat(ast, varData.evalValue.floatValue, loc.line, loc.column);
                                case LiteralValue::INT:
                                    return ASTFactory::MakeLiteralInt(ast, varData.evalValue.intValue, loc.line, loc.column);
                                case LiteralValue::UINT:
                                    return ASTFactory::MakeLiteralUint(ast, varData.evalValue.uintValue, loc.line, loc.column);
                                case LiteralValue::BOOL:
                                    return ASTFactory::MakeLiteralBool(ast, varData.evalValue.boolValue, loc.line, loc.column);
                                default:
                                    break;
                            }
                        }
                    }
                }

                // Create a member access node representing module::member
                // The object is the module identifier, member is the accessed name
                expr = ASTFactory::MakeMemberAccess(ast, expr, memberArena, loc.line, loc.column);
                MemberAccessData& access = ast->GetMemberAccess(expr);
                access.isModuleQualified = true;
                // Pre-compute the qualified hash since memberArena is hash-only and ToString() won't work later
                std::string qualifiedName =
                    CanonicalizeModuleQualifiedName(moduleName.ToString(sourceBase()), memberName);
                access.qualifiedNameHash = Utils::HashStr(qualifiedName.c_str());
            }
            // Case 2: Module::Enum::Variant (expr is MemberAccess with isModuleQualified=true)
            else if (expr.Type() == ASTNodeType::MEMBER_ACCESS) {
                MemberAccessData& prevAccess = ast->GetMemberAccess(expr);
                if (!prevAccess.isModuleQualified) {
                    Error("Expected module-qualified type before '::' for enum access");
                    return NodeRef::Null();
                }

                // Get the enum name from the member access
                const ArenaString& enumName = prevAccess.member;

                // Get the variant name
                Consume(TokenType::IDENTIFIER, "Expected enum variant after '::'");
                std::string variantName(stream->GetValue(previous));

                // Get the module from the object (should be an identifier)
                if (prevAccess.object.Type() != ASTNodeType::IDENTIFIER) {
                    Error("Expected module identifier for enum access");
                    return NodeRef::Null();
                }
                const ArenaString& moduleName = ast->GetIdentifier(prevAccess.object).name;
                u32 moduleIdx = ResolveModuleIndexByWrittenName(moduleName);

                if (moduleIdx == INVALID_INDEX) {
                    Error("Unknown module '" + moduleName.ToString(sourceBase()) + "'");
                    return NodeRef::Null();
                }

                u32 resolvedModuleHash = SymbolTable::ResolveModuleNameHash(&symbolTable, moduleName.nameHash);

                std::string syntheticQualifiedName;
                syntheticQualifiedName.reserve(2 + 10 + 10);
                syntheticQualifiedName.append("m").append(std::to_string(resolvedModuleHash));
                syntheticQualifiedName.append("::");
                syntheticQualifiedName.append("e").append(std::to_string(enumName.nameHash));
                Symbol* enumSym = SymbolTable::LookupByHash(&symbolTable,
                    Utils::HashStr(syntheticQualifiedName.c_str()));
                if (!enumSym || (enumSym->kind != SymbolKind::ENUM &&
                                 enumSym->kind != SymbolKind::ENUM_SYMBOL)) {
                    Error("'" + enumName.ToString(sourceBase()) + "' is not an enum type in module");
                    return NodeRef::Null();
                }
                const EnumData& enumData = symbolTable.enums[enumSym->index];

                // Find the variant in the enum
                u32 variantHash = Utils::HashStr(variantName.c_str());
                bool found = false;
                u32 variantValue = 0;

                for (u32 i = 0; i < enumData.variants.count; i++) {
                    if (enumData.variants[i].name.nameHash == variantHash) {
                        variantValue = enumData.variants[i].value;
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    Error("Unknown enum variant '" + variantName + "'");
                    return NodeRef::Null();
                }

                if (enumData.flags & EnumData::IS_SUM_TYPE) {
                    ArenaString variantArena = ArenaString::MakeHashOnly(variantName);
                    expr = ASTFactory::MakeMemberAccess(ast, expr, variantArena, loc.line, loc.column);
                    MemberAccessData& access = ast->GetMemberAccess(expr);
                    access.isModuleQualified = true;
                    std::string qualifiedName =
                        CanonicalizeModuleQualifiedName(moduleName.ToString(sourceBase()),
                                                        enumName.ToString(sourceBase()) + "::" + variantName);
                    access.qualifiedNameHash = Utils::HashStr(qualifiedName.c_str());
                    continue;
                }

                CoreType baseType = enumData.underlyingType;
                if (baseType == CoreType::UINT) {
                    return ASTFactory::MakeLiteralUint(ast, variantValue, loc.line, loc.column);
                }
                return ASTFactory::MakeLiteralInt(ast, static_cast<int64_t>(variantValue), loc.line, loc.column);
            }
            else {
                Error("Expected module name or module-qualified enum before '::'");
                return NodeRef::Null();
            }
        } else if (Match(TokenType::INCREMENT)) {
            // Postfix increment: x++
            SourceLocation loc = getLocation(stream->GetOffset(previous));
            expr = ASTFactory::MakeUnaryOp(ast, UnaryOpType::POST_INCREMENT, expr, loc.line, loc.column);
        } else if (Match(TokenType::DECREMENT)) {
            // Postfix decrement: x--
            SourceLocation loc = getLocation(stream->GetOffset(previous));
            expr = ASTFactory::MakeUnaryOp(ast, UnaryOpType::POST_DECREMENT, expr, loc.line, loc.column);
        } else if (Check(TokenType::BITWISE_XOR)) {
            // Could be postfix dereference (x^) or binary XOR (x ^ y)
            // It's a dereference only if NOT followed by something that starts an expression
            // NOTE: We exclude MINUS/PLUS from expr start because they're more commonly
            // binary operators after a postfix expression. Use parens for "a ^ (-b)".
            TokenRef nextTok = current + 1;
            if (nextTok < stream->Count()) {
                u8 nextTypeVal = stream->GetType(nextTok);
                TokenType nextType = static_cast<TokenType>(nextTypeVal);
                // If next token can start an expression (excluding ambiguous binary ops),
                // this is binary XOR, not dereference
                // Core types are 0-16, so we check if nextTypeVal <= 16
                bool nextIsExprStart = (nextType == TokenType::IDENTIFIER ||
                                        nextType == TokenType::NUMBER ||
                                        nextType == TokenType::LEFT_PAREN ||
                                        // Exclude MINUS - it's usually binary subtraction after postfix
                                        nextType == TokenType::NOT ||
                                        nextType == TokenType::BITWISE_NOT ||
                                        // Don't include BITWISE_XOR - consecutive ^ means deref-then-XOR
                                        // e.g., mPtr^ ^ nPtr^ should be (mPtr^) ^ (nPtr^)
                                        nextTypeVal <= 16); // Core types (FLOAT..VOID)
                if (nextIsExprStart) {
                    // It's binary XOR - don't match here, let ParseBitwiseXor handle it
                    break;
                }
            }
            // It's postfix dereference
            Advance(); // consume the ^
            SourceLocation loc = getLocation(stream->GetOffset(previous));
            expr = ASTFactory::MakeUnaryOp(ast, UnaryOpType::DEREFERENCE, expr, loc.line, loc.column);
        } else {
            break;
        }
        if (!expr.IsValid()) return NodeRef::Null();
    }

    return expr;
}

NodeRef Parser::ParsePrimary() {
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;

    // Type constructor check
    if (CheckMask(TokenMasks::CORE_TYPES)) {
        TokenRef next = PeekNext();

        if (stream->GetType(next) == TokenType::LEFT_BRACKET) {
            return ParseInlineArrayConstruction();
        }

        if (stream->GetType(next) == TokenType::LEFT_PAREN) {
            Advance(); // consume the type token
            std::string typeName(stream->GetValue(previous));
            NodeRef typeNode = ASTFactory::MakeIdentifier(ast, typeName, line, col);
            Advance(); // consume '('
            return ParseFunctionCall(typeNode);
        }
    }

    // Literals
    if (Match(TokenType::TRUE)) {
        return ASTFactory::MakeLiteralBool(ast, true, line, col);
    }

    if (Match(TokenType::FALSE)) {
        return ASTFactory::MakeLiteralBool(ast, false, line, col);
    }

    if (Match(TokenType::NUMBER)) {
        std::string_view numStr = stream->GetValue(previous);
        bool isHex = numStr.size() > 2 && numStr[0] == '0' &&
                     (numStr[1] == 'x' || numStr[1] == 'X');
        bool isBin = numStr.size() > 2 && numStr[0] == '0' &&
                     (numStr[1] == 'b' || numStr[1] == 'B');
        bool hasDecimal = numStr.find('.') != std::string::npos;
        bool hasFloatSuffix = (!isHex && !isBin) && !numStr.empty() &&
                              (numStr.back() == 'f' || numStr.back() == 'F');
        bool hasExponent = (!isHex && !isBin) &&
                           (numStr.find('e') != std::string::npos ||
                            numStr.find('E') != std::string::npos);
        if (hasDecimal || hasFloatSuffix || hasExponent) {
            float value = SafeParseFloat(numStr);
            return ASTFactory::MakeLiteralFloat(ast, value, line, col);
        } else {
            // Check for unsigned suffix 'u' or 'U'
            bool isUnsigned = (!numStr.empty() && (numStr.back() == 'u' || numStr.back() == 'U'));
            std::string parseStr(numStr);
            if (isUnsigned) {
                parseStr.pop_back();  // Remove the 'u' suffix for parsing
            }

            // Use SafeParseU32 with base 0 to auto-detect hex (0x), octal (0),
            // or decimal. Handles the full uint32 range for hex literals like
            // 0x9E3779B9u and returns 0 on malformed input (fuzzer-safe).
            unsigned long parsed = SafeParseU32(parseStr, 0);

            if (isUnsigned) {
                return ASTFactory::MakeLiteralUint(ast, static_cast<uint32_t>(parsed), line, col);
            } else {
                int value = static_cast<int>(static_cast<uint32_t>(parsed));
                return ASTFactory::MakeLiteralInt(ast, value, line, col);
            }
        }
    }

    if (Match(TokenType::STRING)) {
        // String literals - need to add string support to literals
        u32 index = ast->literals.count;
        LiteralData data;
        data.value.type = LiteralValue::STRING;
        data.value.stringValue = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));
        ast->literals.Push(arena, data);

        if (ast->nodeCount >= ast->nodeCapacity) {
            u32 newCapacity = ast->nodeCapacity * 2;
            u32* newPositions = (u32*)arena->Allocate(sizeof(u32) * newCapacity, 64);
            memcpy(newPositions, ast->positions, ast->nodeCount * sizeof(u32));
            ast->positions = newPositions;
            ast->nodeCapacity = newCapacity;
        }
        ast->positions[ast->nodeCount++] = AST::PackPosition(line, col);

        return NodeRef(ASTNodeType::LITERAL, index);
    }

    // Special namespace tokens
    if (Match(TokenType::RESOURCES)) {
        NodeRef node = ASTFactory::MakeIdentifier(ast, "resources", line, col);
        ast->GetIdentifier(node).identifierKind = SpecialIdentifier::RESOURCES;
        return node;
    }

    if (Match(TokenType::ATTRIBUTES)) {
        NodeRef node = ASTFactory::MakeIdentifier(ast, "attributes", line, col);
        ast->GetIdentifier(node).identifierKind = SpecialIdentifier::ATTRIBUTES;
        return node;
    }

    if (Match(TokenType::VARIANTS)) {
        NodeRef node = ASTFactory::MakeIdentifier(ast, "variants", line, col);
        ast->GetIdentifier(node).identifierKind = SpecialIdentifier::VARIANTS;
        return node;
    }

    // self keyword - used in enum methods
    if (Match(TokenType::SELF)) {
        NodeRef node = ASTFactory::MakeIdentifier(ast, "self", line, col);
        ast->GetIdentifier(node).identifierKind = SpecialIdentifier::SELF;
        return node;
    }

    // Handle texture types used as function names (e.g., textureCube(...))
    if (Match(TokenType::TEXTURECUBE) || Match(TokenType::TEXTURE2D) ||
        Match(TokenType::TEXTURE3D) || Match(TokenType::TEXTURE2DARRAY)) {
        std::string typeName(stream->GetValue(previous));
        NodeRef node = ASTFactory::MakeIdentifier(ast, typeName, line, col);
        return node;
    }

    if (Match(TokenType::IDENTIFIER)) {
        ArenaString identName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));
        Symbol* sym = SymbolTable::LookupAny(&symbolTable, identName);

        // Check for EVAL_CONSTANT first (module-level const declarations)
        if (sym && sym->kind == SymbolKind::EVAL_CONSTANT) {
            const LiteralValue& constVal = symbolTable.evalConstants[sym->index];
            switch (constVal.type) {
                case LiteralValue::FLOAT:
                    return ASTFactory::MakeLiteralFloat(ast, constVal.floatValue, line, col);
                case LiteralValue::INT:
                    return ASTFactory::MakeLiteralInt(ast, constVal.intValue, line, col);
                case LiteralValue::UINT:
                    return ASTFactory::MakeLiteralUint(ast, constVal.uintValue, line, col);
                case LiteralValue::BOOL:
                    return ASTFactory::MakeLiteralBool(ast, constVal.boolValue, line, col);
                default:
                    break;
            }
        }
        // Evaluated const variables may still be substituted while parsing; unevaluated
        // eval declarations are left as syntax for the comptime pass.
        else if (sym && sym->kind == SymbolKind::VARIABLE) {
            const VariableData& varData = symbolTable.variables[sym->index];
            if (varData.hasEvalValue) {
                switch (varData.evalValue.type) {
                    case LiteralValue::FLOAT:
                        return ASTFactory::MakeLiteralFloat(ast, varData.evalValue.floatValue, line, col);
                    case LiteralValue::INT:
                        return ASTFactory::MakeLiteralInt(ast, varData.evalValue.intValue, line, col);
                    case LiteralValue::UINT:
                        return ASTFactory::MakeLiteralUint(ast, varData.evalValue.uintValue, line, col);
                    case LiteralValue::BOOL:
                        return ASTFactory::MakeLiteralBool(ast, varData.evalValue.boolValue, line, col);
                    default:
                        break;
                }
            }
        }
        // Regular identifier - use source-backed ArenaString so we can reconstruct name later
        ArenaString identArena = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));
        NodeRef node = ASTFactory::MakeIdentifier(ast, identArena, line, col);

        // Use hash comparison instead of std::string allocation
        static constexpr u32 OUTPUT_HASH = Utils::HashStr("output");
        static constexpr u32 INPUT_HASH = Utils::HashStr("input");
        if (identArena.nameHash == OUTPUT_HASH) {
            ast->GetIdentifier(node).identifierKind = SpecialIdentifier::OUTPUT;
        } else if (identArena.nameHash == INPUT_HASH) {
            ast->GetIdentifier(node).identifierKind = SpecialIdentifier::INPUT;
        } else {
            ast->GetIdentifier(node).identifierKind = SpecialIdentifier::NONE;
        }
        return node;
    }

    if (Match(TokenType::LEFT_PAREN)) {
        NodeRef expr = ParseExpression();
        Consume(TokenType::RIGHT_PAREN, "Expected ')' after expression");
        return expr;
    }

    ErrorAtCurrent("Expected expression");
    return NodeRef::Null();
}

//==============================================================================
// Function call and member access
//==============================================================================

NodeRef Parser::ParseFunctionCall(NodeRef function) {
    if (!function.IsValid()) {
        Error("Internal error: null function in call");
        return NodeRef::Null();
    }

    SourceLocation loc = getLocation(stream->GetOffset(previous));
    NodeRef call;
    ArenaString funcName;
    u32 qualifiedNameHash = 0;
    
    if (function.Type() == ASTNodeType::IDENTIFIER) {
        // Simple function call: functionName(args)
        const IdentifierData& funcIdent = ast->GetIdentifier(function);
        funcName = funcIdent.name;
        call = ASTFactory::MakeFunctionCall(ast, funcName, loc.line, loc.column);
    } else if (function.Type() == ASTNodeType::MEMBER_ACCESS) {
        const MemberAccessData& access = ast->GetMemberAccess(function);
        funcName = access.member;

        if (access.isModuleQualified) {
            // Module-qualified function call: Module::functionName(args)
            qualifiedNameHash = access.qualifiedNameHash;

            call = ASTFactory::MakeFunctionCall(ast, funcName, loc.line, loc.column);
            ast->GetFunctionCall(call).flags |= FunctionCallFlags::IS_MODULE_FUNCTION;
            ast->GetFunctionCall(call).moduleQualifiedHash = qualifiedNameHash;
            ast->GetFunctionCall(call).moduleObject = access.object;
            if (access.object.Type() == ASTNodeType::IDENTIFIER) {
                const IdentifierData& moduleIdent = ast->GetIdentifier(access.object);
                u32 moduleIdx = ResolveModuleIndexByWrittenName(moduleIdent.name);
                if (moduleIdx != INVALID_INDEX) {
                    ast->GetFunctionCall(call).moduleIndex = moduleIdx;
                }
            }
        } else {
            // Method call on object: object.method(args)
            // e.g., self.distance(p) or shape.distance(p)
            call = ASTFactory::MakeFunctionCall(ast, funcName, loc.line, loc.column);
            ast->GetFunctionCall(call).flags |= FunctionCallFlags::IS_METHOD_CALL;
            ast->GetFunctionCall(call).moduleObject = access.object;  // Store receiver object
        }
    } else {
        Error("Can only call functions by name");
        return function;
    }

    // Check if intrinsic (skip for method calls - they use enum methods, not intrinsics)
    bool isMethodCall = (ast->GetFunctionCall(call).flags & FunctionCallFlags::IS_METHOD_CALL) != 0;

    // For method calls, clear the intrinsic flag since they're not intrinsics
    if (isMethodCall) {
        ast->GetFunctionCall(call).flags &= ~FunctionCallFlags::IS_INTRINSIC;
    }

    if (!isMethodCall && (ast->GetFunctionCall(call).flags & FunctionCallFlags::IS_INTRINSIC)) {
        const auto* intrinsic = StdLib::IntrinsicLookup::Find(ast->GetFunctionCall(call).name.nameHash);

        // Parse arguments
        if (!Check(TokenType::RIGHT_PAREN)) {
            do {
                NodeRef arg = ParseExpression();
                if (arg.IsValid()) {
                    ast->GetFunctionCall(call).arguments.Push(arena, arg);
                }
            } while (Match(TokenType::COMMA));
        }

        const auto& intrinsicData = StdLib::INTRINSICS[ast->GetFunctionCall(call).intrinsicIndex];

        if (!StdLib::IsValidForStage(&intrinsicData, currentShaderStage)) {
            ErrorAtPrevious("Intrinsic not available in this shader stage");
            Consume(TokenType::RIGHT_PAREN, "Expected ')' after arguments");
            return call;
        }

        // Validate argument count
        if (ast->GetFunctionCall(call).arguments.count < intrinsicData.minParams) {
            char msg[256];
            snprintf(msg, sizeof(msg), "'%s' requires at least %d arguments, got %d",
                    funcName.ToString(sourceBase()).c_str(), intrinsicData.minParams,
                    ast->GetFunctionCall(call).arguments.count);
            ErrorAtPrevious(msg);
        }

        if (intrinsicData.maxParams != 0xFF &&
            ast->GetFunctionCall(call).arguments.count > intrinsicData.maxParams) {
            char msg[256];
            snprintf(msg, sizeof(msg), "'%s' accepts at most %d arguments, got %d",
                    funcName.ToString(sourceBase()).c_str(), intrinsicData.maxParams,
                    ast->GetFunctionCall(call).arguments.count);
            ErrorAtPrevious(msg);
        }

        ast->GetFunctionCall(call).flags |= FunctionCallFlags::TYPE_VALIDATED;
    } else {
        // Regular function call
        if (!Check(TokenType::RIGHT_PAREN)) {
            do {
                NodeRef arg = ParseExpression();
                if (arg.IsValid()) {
                    ast->GetFunctionCall(call).arguments.Push(arena, arg);
                }
            } while (Match(TokenType::COMMA));
        }
    }

    Consume(TokenType::RIGHT_PAREN, "Expected ')' after arguments");
    return call;
}

NodeRef Parser::ParseMemberAccess(NodeRef object) {
    if (!object.IsValid()) {
        Error("Internal error: null object in member access");
        return NodeRef::Null();
    }

    SourceLocation loc = getLocation(stream->GetOffset(previous));
    Consume(TokenType::IDENTIFIER, "Expected member name after '.'");
    ArenaString memberName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));

    // Validate resource/attribute access
    if (object.Type() == ASTNodeType::IDENTIFIER) {
        const IdentifierData& ident = ast->GetIdentifier(object);

        switch (ident.identifierKind) {
            case SpecialIdentifier::RESOURCES:
                if (inShaderStage) {
                    if (PipelineDeclaresResources() && !LookupPipelineResourceDecl(memberName)) {
                        ErrorAtPrevious("Resource not declared in pipeline resources block");
                        break;
                    }
                    bool requireUseResources = currentPass.IsValid() &&
                        ((currentPipeline.IsValid() && ast->GetPipeline(currentPipeline).resources.count > 0) ||
                         ast->GetPass(currentPass).usedResources.count > 0);
                    if (requireUseResources && !ValidateResourceInUse(memberName)) {
                        ErrorAtPrevious("Resource not declared in 'use resources' for this pass");
                    } else if (!SymbolTable::ValidateResourceAccess(&symbolTable, memberName, currentShaderStage, sourceBase())) {
                        ErrorAtPrevious("Resource not available in this shader stage");
                    }
                }
                break;

            case SpecialIdentifier::ATTRIBUTES:
                if (inShaderStage && !ValidateAttributeInUse(memberName)) {
                    ErrorAtPrevious("Attribute not declared in 'use attributes' for this pass");
                }
                break;

            case SpecialIdentifier::VARIANTS: {
                TypeInfo variantType;
                if (!LookupVariantType(currentPipeline, memberName.nameHash, &variantType)) {
                    ErrorAtPrevious("Unknown variant or implicit variant feature");
                }
                break;
            }

            default:
                break;
        }
    }

    return ASTFactory::MakeMemberAccess(ast, object, memberName, loc.line, loc.column);
}

NodeRef Parser::ParseArrayAccess(NodeRef array) {
    if (!array.IsValid()) {
        Error("Internal error: null array in array access");
        return NodeRef::Null();
    }

    SourceLocation loc = getLocation(stream->GetOffset(previous));
    NodeRef index = ParseExpression();
    if (!index.IsValid()) {
        Error("Expected index expression");
        return array;
    }

    Consume(TokenType::RIGHT_BRACKET, "Expected ']' after array index");

    NodeRef access = ASTFactory::MakeArrayAccess(ast, array, index, loc.line, loc.column);
    return FlattenMultiDimArrayAccess(access);
}

NodeRef Parser::FlattenMultiDimArrayAccess(NodeRef access) {
    if (access.Type() != ASTNodeType::ARRAY_ACCESS) {
        return access;
    }

    std::vector<NodeRef> indices;
    NodeRef base = access;

    while (base.Type() == ASTNodeType::ARRAY_ACCESS) {
        const ArrayAccessData& data = ast->GetArrayAccess(base);
        indices.push_back(data.index);
        base = data.array;
    }

    if (base.Type() != ASTNodeType::IDENTIFIER) {
        return access;
    }

    u32 baseHash = ast->GetIdentifier(base).name.nameHash;
    auto it = multiDimArrayDims.find(baseHash);
    if (it == multiDimArrayDims.end()) {
        return access;
    }

    const std::vector<u32>& dims = it->second;
    if (dims.size() <= 1 || indices.size() != dims.size()) {
        return access;
    }

    std::reverse(indices.begin(), indices.end());

    NodeRef flatIndex = indices[0];
    for (size_t i = 1; i < indices.size(); i++) {
        NodeRef dimLiteral = ASTFactory::MakeLiteralUint(ast, dims[i], 0, 0);
        NodeRef mul = ASTFactory::MakeBinaryOp(ast, BinaryOpType::MULTIPLY, flatIndex, dimLiteral, 0, 0);
        flatIndex = ASTFactory::MakeBinaryOp(ast, BinaryOpType::ADD, mul, indices[i], 0, 0);
    }

    return ASTFactory::MakeArrayAccess(ast, base, flatIndex, 0, 0);
}

//==============================================================================
// Helper functions
//==============================================================================
