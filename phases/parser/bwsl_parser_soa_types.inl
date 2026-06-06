// Part of bwsl_parser_soa.cpp. Include from that file only.
// Arrays, enum/sumtype constructs, pattern matching, and structs.
#pragma once

#include "bwsl_parser_soa_variants_eval.inl"

namespace BWSL {


//==============================================================================
// Array declaration and construction
//==============================================================================

NodeRef Parser::ParseArrayDeclaration(CoreType elementType, StorageClass storageClass) {
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;

    // Already consumed '[', now get size
    std::vector<u32> arrayDims;
    Consume(TokenType::NUMBER, "Expected array size");

    std::string_view sizeStr = PreviousValue();
    int size = SafeParseInt(sizeStr);

    if (size <= 0 || static_cast<u32>(size) > MAX_ARRAY_SIZE) {
        Error("Invalid array size. Max 256k elements");
        return NodeRef::Null();
    }

    Consume(TokenType::RIGHT_BRACKET, "Expected ']' after array size");
    arrayDims.push_back(static_cast<u32>(size));

    while (Match(TokenType::LEFT_BRACKET)) {
        Consume(TokenType::NUMBER, "Expected array size");
        std::string_view dimStr = PreviousValue();
        int dimSize = SafeParseInt(dimStr);
        if (dimSize <= 0 || static_cast<u32>(dimSize) > MAX_ARRAY_SIZE) {
            Error("Invalid array size. Max 256k elements");
            return NodeRef::Null();
        }
        Consume(TokenType::RIGHT_BRACKET, "Expected ']' after array size");
        arrayDims.push_back(static_cast<u32>(dimSize));
    }
    Consume(TokenType::IDENTIFIER, "Expected array variable name");

    std::string varName(stream->GetValue(previous));

    // Compute array type info for symbol table
    auto CalculateTypeSizeLocal = [](CoreType t, u8 comp) -> u32 {
        (void)t;
        return static_cast<u32>(comp) * 4u;
    };

    u64 totalSize = 1;
    for (u32 dim : arrayDims) {
        if (dim == 0 || totalSize > (MAX_ARRAY_SIZE / dim)) {
            Error("Invalid array size. Max 256k elements");
            return NodeRef::Null();
        }
        totalSize *= dim;
    }

    TypeInfo arrayInfo{};
    arrayInfo.coreType = elementType;
    arrayInfo.componentCount = CoreTypeComponentCount(elementType);
    arrayInfo.arrayDimensions = static_cast<u8>(arrayDims.size());
    arrayInfo.customTypeHash = 0;
    arrayInfo.arrayLength = static_cast<u32>(totalSize);
    arrayInfo.arrayStride = CalculateTypeSizeLocal(elementType, arrayInfo.componentCount);

    // Check for initializer
    NodeRef initializer = NodeRef::Null();
    if (Match(TokenType::ASSIGN)) {
        // Check for brace-enclosed array initializer: { expr, expr, ... }
        if (Check(TokenType::LEFT_BRACE)) {
            initializer = ParseArrayInitializer();
        } else {
            initializer = ParseExpression();
        }
    }

    Consume(TokenType::SEMICOLON, "Expected ';' after array declaration");

    NodeRef varDecl = ASTFactory::MakeVariableDecl(ast,
        ArenaString::MakeHashOnly(varName),
        ArenaString::MakeHashOnly("array"),
        initializer, false, line, col, storageClass, static_cast<u8>(arrayDims.size()),
        static_cast<u32>(totalSize), SymbolTable::GetCoreTypeNameHash(elementType));

    // Add to symbol table
    Symbol* sym = SymbolTable::AddSymbol(&symbolTable, ArenaString::MakeHashOnly(varName), SymbolKind::VARIABLE);

    if (sym) {
        VariableData& varData = symbolTable.variables[sym->index];
        varData.typeInfo = arrayInfo;
        varData.storageClass = storageClass;
    }

    if (arrayDims.size() > 1) {
        multiDimArrayDims[ArenaString::MakeHashOnly(varName).nameHash] = arrayDims;
    }

    return varDecl;
}

NodeRef Parser::ParseArrayInitializer() {
    // Handle brace-enclosed array initializer: { expr, expr, ... }
    SourceLocation loc = getLocation(stream->GetOffset(current));
    u32 line = loc.line;
    u32 col = loc.column;

    Consume(TokenType::LEFT_BRACE, "Expected '{' for array initializer");

    NodeRef arrayNode = ASTFactory::MakeBlock(ast, line, col);

    // Parse elements
    if (!Check(TokenType::RIGHT_BRACE)) {
        do {
            if (Check(TokenType::RIGHT_BRACE)) {
                break;  // Handle trailing comma
            }

            NodeRef element = ParseExpression();
            if (!element.IsValid()) {
                Error("Expected element expression in array initializer");
                return NodeRef::Null();
            }

            ast->GetBlock(arrayNode).statements.Push(arena, element);
        } while (Match(TokenType::COMMA));
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after array initializer");

    return arrayNode;
}

NodeRef Parser::ParseInlineArrayConstruction() {
    // Handle: float[4][1.0, 0.5, 0.25, 0.0]
    // Or partially: float[4][1.0, 0.5] -> rest default to 0
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;

    if (!MatchMask(TokenMasks::CORE_TYPES)) {
        Error("Expected type before array brackets");
        return NodeRef::Null();
    }

    CoreType elementType = TokenTypeToReturnType(static_cast<TokenType>(stream->GetType(previous)));
    (void)elementType;

    Consume(TokenType::LEFT_BRACKET, "Expected '[' for array");
    Consume(TokenType::NUMBER, "Expected array size");

    std::string_view sizeStr = PreviousValue();
    int size = 0;

#ifdef BWSL_WASM
    // WASM builds don't have exception support, use simpler parsing
    char* endPtr = nullptr;
    long parsed = std::strtol(sizeStr.data(), &endPtr, 10);
    if (endPtr == sizeStr.data() || parsed <= 0 || parsed > INT_MAX) {
        Error("Invalid or out-of-range array size");
        return NodeRef::Null();
    }
    size = static_cast<int>(parsed);
#else
    try {
        size = std::stoi(std::string(sizeStr));
    } catch (const std::out_of_range&) {
        Error("Array size is too large");
        return NodeRef::Null();
    } catch (const std::invalid_argument&) {
        Error("Invalid array size");
        return NodeRef::Null();
    }
#endif

    if (size <= 0) {
        Error("Array size must be positive");
        return NodeRef::Null();
    }

    if (static_cast<u32>(size) > MAX_ARRAY_SIZE) {
        Error("Array size too large (max 256K / 1mb floats)");
        return NodeRef::Null();
    }

    Consume(TokenType::RIGHT_BRACKET, "Expected ']' after array size");
    Consume(TokenType::LEFT_BRACKET, "Expected '[' for inline array construction");

    NodeRef arrayNode = ASTFactory::MakeBlock(ast, line, col);

    // Parse elements (can be fewer than size, in which case they get zero initialized)
    u32 elementCount = 0;
    if (!Check(TokenType::RIGHT_BRACKET)) {
        do {
            NodeRef element = ParseExpression();
            if (!element.IsValid()) {
                Error("Expected element expression in array initializer");
                return NodeRef::Null();
            }

            ast->GetBlock(arrayNode).statements.Push(arena, element);
            elementCount++;

            if (elementCount > static_cast<u32>(size)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                        "Too many elements in array initializer (expected %d, got %d)",
                        size, elementCount);
                Error(msg);
                return NodeRef::Null();
            }
        } while (Match(TokenType::COMMA));
    }

    Consume(TokenType::RIGHT_BRACKET, "Expected ']' after inline array construction");

    // Note: elementCount < size is OK - rest will be zero-initialized
    return arrayNode;
}

//==============================================================================
// Type inference
//==============================================================================

TypeInfo Parser::GetExpressionType(NodeRef expr) {
    if (expr.IsNull()) {
        return TypeInfo{CoreType::INVALID, 0, 0, 0, 0, 0, 0};
    }

    switch (expr.Type()) {
        case ASTNodeType::MEMBER_ACCESS: {
            const MemberAccessData& memberData = ast->GetMemberAccess(expr);
            if (memberData.object.Type() == ASTNodeType::IDENTIFIER) {
                const IdentifierData& objIdent = ast->GetIdentifier(memberData.object);
                switch (objIdent.identifierKind) {
                    case SpecialIdentifier::ATTRIBUTES: {
                        Symbol* sym = SymbolTable::LookupAny(&symbolTable, memberData.member);
                        if (sym && sym->kind == SymbolKind::ATTRIBUTE) {
                            AttributeData& data = symbolTable.attributes[sym->index];
                            return data.typeInfo;
                        }
                        break;
                    }

                    case SpecialIdentifier::RESOURCES: {
                        Symbol* sym = SymbolTable::LookupAny(&symbolTable, memberData.member);
                        if (sym && sym->kind == SymbolKind::RESOURCE) {
                            return MakeTypeInfoForResource(&symbolTable, symbolTable.resources[sym->index]);
                        }
                        break;
                    }

                    case SpecialIdentifier::VARIANTS: {
                        TypeInfo variantType;
                        if (LookupVariantType(currentPipeline, memberData.member.nameHash, &variantType)) {
                            return variantType;
                        }
                        break;
                    }

                    case SpecialIdentifier::NONE:
                    default: {
                        // Regular member access on user types
                        TypeInfo objType = GetExpressionType(memberData.object);
                        if (objType.coreType == CoreType::CUSTOM) {
                            StructData* structData = g_customTypes.LookupType(objType.customTypeHash);
                            if (structData) {
                                u32 memberHash = memberData.member.nameHash;
                                for (u32 i = 0; i < structData->fields.count; i++) {
                                    if (structData->fields[i].name.nameHash == memberHash) {
                                        return structData->fields[i].type;
                                    }
                                }
                            }
                        }
                        break;
                    }
                }
            }
            TypeInfo objType = GetExpressionType(memberData.object);
            if (objType.coreType == CoreType::CUSTOM) {
                u32 memberHash = memberData.member.nameHash;
                if (objType.customTypeHash == Utils::HashStr("BwslModfResult")) {
                    if (memberHash == Utils::HashStr("fraction") ||
                        memberHash == Utils::HashStr("whole")) {
                        return TypeInfo{CoreType::FLOAT, 1, 0, 0, 0, 0, 0};
                    }
                } else if (objType.customTypeHash == Utils::HashStr("BwslFrexpResult")) {
                    if (memberHash == Utils::HashStr("mantissa")) {
                        return TypeInfo{CoreType::FLOAT, 1, 0, 0, 0, 0, 0};
                    }
                    if (memberHash == Utils::HashStr("exponent")) {
                        return TypeInfo{CoreType::INT, 1, 0, 0, 0, 0, 0};
                    }
                }
            }
            break;
        }

        case ASTNodeType::IDENTIFIER: {
            const IdentifierData& ident = ast->GetIdentifier(expr);
            TypeInfo activeVariantType;
            if (allowBareVariantLookup &&
                LookupActiveVariantBinding(ident.name.nameHash, nullptr, &activeVariantType)) {
                return activeVariantType;
            }
            Symbol* sym = SymbolTable::LookupAny(&symbolTable, ident.name);
            if (sym) {
                switch (sym->kind) {
                    case SymbolKind::VARIABLE:
                        return symbolTable.variables[sym->index].typeInfo;

                    case SymbolKind::RESOURCE:
                        return MakeTypeInfoForResource(&symbolTable, symbolTable.resources[sym->index]);

                    case SymbolKind::EVAL_CONSTANT: {
                        LiteralValue& value = symbolTable.evalConstants[sym->index];
                        switch (value.type) {
                            case LiteralValue::FLOAT:
                                return TypeInfo{CoreType::FLOAT, 1, 0, 0, 0, 0, 0};
                            case LiteralValue::INT:
                                return TypeInfo{CoreType::INT, 1, 0, 0, 0, 0, 0};
                            case LiteralValue::BOOL:
                                return TypeInfo{CoreType::BOOL, 1, 0, 0, 0, 0, 0};
                            default:
                                return TypeInfo{CoreType::INVALID, 0, 0, 0, 0, 0, 0};
                        }
                    }

                    default:
                        break;
                }
            }
            break;
        }

        case ASTNodeType::ARRAY_ACCESS: {
            const ArrayAccessData& arrayData = ast->GetArrayAccess(expr);
            TypeInfo arrayType = GetExpressionType(arrayData.array);
            switch (arrayType.coreType) {
                case CoreType::FLOAT2:
                case CoreType::FLOAT3:
                case CoreType::FLOAT4:
                    return TypeInfo{CoreType::FLOAT, 1, 0, 0, 0, 0, 0};
                case CoreType::DOUBLE2:
                case CoreType::DOUBLE3:
                case CoreType::DOUBLE4:
                    return TypeInfo{CoreType::DOUBLE, 1, 0, 0, 0, 0, 0};
                case CoreType::INT2:
                case CoreType::INT3:
                case CoreType::INT4:
                    return TypeInfo{CoreType::INT, 1, 0, 0, 0, 0, 0};
                case CoreType::INT64X2:
                case CoreType::INT64X3:
                case CoreType::INT64X4:
                    return TypeInfo{CoreType::INT64, 1, 0, 0, 0, 0, 0};
                case CoreType::UINT2:
                case CoreType::UINT3:
                case CoreType::UINT4:
                    return TypeInfo{CoreType::UINT, 1, 0, 0, 0, 0, 0};
                case CoreType::UINT64X2:
                case CoreType::UINT64X3:
                case CoreType::UINT64X4:
                    return TypeInfo{CoreType::UINT64, 1, 0, 0, 0, 0, 0};
                case CoreType::MAT2:
                    return TypeInfo{CoreType::FLOAT2, 2, 0, 0, 0, 0, 0};
                case CoreType::MAT3:
                    return TypeInfo{CoreType::FLOAT3, 3, 0, 0, 0, 0, 0};
                case CoreType::MAT4:
                    return TypeInfo{CoreType::FLOAT4, 4, 0, 0, 0, 0, 0};
                case CoreType::DMAT2:
                    return TypeInfo{CoreType::DOUBLE2, 2, 0, 0, 0, 0, 0};
                case CoreType::DMAT3:
                    return TypeInfo{CoreType::DOUBLE3, 3, 0, 0, 0, 0, 0};
                case CoreType::DMAT4:
                    return TypeInfo{CoreType::DOUBLE4, 4, 0, 0, 0, 0, 0};
                default:
                    if (arrayType.arrayDimensions > 0) {
                        TypeInfo elementType = arrayType;
                        elementType.arrayDimensions--;
                        elementType.arrayLength = 0;
                        elementType.arrayStride = 0;
                        return elementType;
                    }
                    return TypeInfo{CoreType::INVALID, 0, 0, 0, 0, 0, 0};
            }
        }

        case ASTNodeType::LITERAL: {
            const LiteralData& lit = ast->GetLiteral(expr);
            switch (lit.value.type) {
                case LiteralValue::FLOAT:
                    return TypeInfo{CoreType::FLOAT, 1, 0, 0, 0, 0, 0};
                case LiteralValue::INT:
                    return TypeInfo{CoreType::INT, 1, 0, 0, 0, 0, 0};
                case LiteralValue::BOOL:
                    return TypeInfo{CoreType::BOOL, 1, 0, 0, 0, 0, 0};
                case LiteralValue::STRING:
                    return TypeInfo{CoreType::STRING, 0, 0, 0, 0, 0, 0};
                default:
                    return TypeInfo{CoreType::INVALID, 0, 0, 0, 0, 0, 0};
            }
        }

        case ASTNodeType::BINARY_OP: {
            const BinaryOpData& binOp = ast->GetBinaryOp(expr);
            TypeInfo leftType = GetExpressionType(binOp.left);

            switch (binOp.op) {
                case BinaryOpType::EQUALS:
                case BinaryOpType::NOT_EQUALS:
                case BinaryOpType::LESS:
                case BinaryOpType::GREATER:
                case BinaryOpType::LESS_EQUAL:
                case BinaryOpType::GREATER_EQUAL:
                case BinaryOpType::AND:
                case BinaryOpType::OR:
                    return TypeInfo{CoreType::BOOL, 1, 0, 0, 0, 0, 0};

                case BinaryOpType::ADD:
                case BinaryOpType::SUBTRACT:
                case BinaryOpType::MULTIPLY:
                case BinaryOpType::DIVIDE:
                case BinaryOpType::MODULO:
                    return leftType;

                default:
                    return TypeInfo{CoreType::INVALID, 0, 0, 0, 0, 0, 0};
            }
        }

        case ASTNodeType::UNARY_OP: {
            const UnaryOpData& unaryData = ast->GetUnaryOp(expr);
            return GetExpressionType(unaryData.operand);
        }

        case ASTNodeType::FUNCTION_CALL: {
            const FunctionCallData& funcCall = ast->GetFunctionCall(expr);
            if ((funcCall.flags & FunctionCallFlags::IS_METHOD_CALL) &&
                !funcCall.moduleObject.IsNull()) {
                TypeInfo receiverType = GetExpressionType(funcCall.moduleObject);
                if (receiverType.coreType == CoreType::CUSTOM &&
                    receiverType.customTypeHash != 0) {
                    StructData* structData = g_customTypes.LookupType(receiverType.customTypeHash);
                    if (!structData) {
                        Symbol* structSym = SymbolTable::LookupByHash(&symbolTable,
                            receiverType.customTypeHash);
                        if (structSym && structSym->kind == SymbolKind::CUSTOM_TYPE) {
                            structData = &symbolTable.structs[structSym->index];
                        }
                    }
                    if (structData) {
                        std::vector<OverloadTypeMask> argMasks;
                        argMasks.reserve(funcCall.arguments.count);
                        for (u32 i = 0; i < funcCall.arguments.count; i++) {
                            TypeInfo argType = GetExpressionType(funcCall.arguments[i]);
                            OverloadTypeMask mask = MakeOverloadMask(argType);
                            if (mask == 0) {
                                return TypeInfo{CoreType::INVALID, 0, 0, 0, 0, 0, 0};
                            }
                            argMasks.push_back(mask);
                        }
                        bool receiverIsConst = false;
                        if (funcCall.moduleObject.Type() == ASTNodeType::IDENTIFIER) {
                            const IdentifierData& receiverIdent =
                                ast->GetIdentifier(funcCall.moduleObject);
                            Symbol* receiverSym =
                                SymbolTable::LookupAny(&symbolTable, receiverIdent.name);
                            if (receiverSym && receiverSym->kind == SymbolKind::VARIABLE) {
                                receiverIsConst =
                                    symbolTable.variables[receiverSym->index].isConst;
                            }
                        }
                        u32 methodIdx = SymbolTable::LookupStructMethodIndex(
                            &symbolTable, structData, funcCall.name, argMasks.data(),
                            static_cast<u32>(argMasks.size()), receiverIsConst);
                        if (methodIdx != INVALID_INDEX &&
                            methodIdx < symbolTable.functions.count) {
                            const FunctionData& fn = symbolTable.functions[methodIdx];
                            return TypeInfo{fn.returnType, 1, 0, 0, fn.returnTypeHash, 0, 0};
                        }
                    }
                }
            }

            if (funcCall.flags & FunctionCallFlags::IS_INTRINSIC) {
                StdLib::Intrinsic intrinsic =
                    static_cast<StdLib::Intrinsic>(StdLib::INTRINSICS[funcCall.intrinsicIndex].enumIndex);
                switch (intrinsic) {
                    case StdLib::Intrinsic::FREXP:
                        return TypeInfo{CoreType::CUSTOM, 1, 0, 0, Utils::HashStr("BwslFrexpResult"), 0, 0};
                    case StdLib::Intrinsic::MODF_SPLIT:
                        return TypeInfo{CoreType::CUSTOM, 1, 0, 0, Utils::HashStr("BwslModfResult"), 0, 0};
                    case StdLib::Intrinsic::F32TOF16:
                        return TypeInfo{CoreType::UINT, 1, 0, 0, 0, 0, 0};
                    case StdLib::Intrinsic::F16TOF32:
                    case StdLib::Intrinsic::LDEXP:
                    case StdLib::Intrinsic::LOG10:
                        return TypeInfo{CoreType::FLOAT, 1, 0, 0, 0, 0, 0};
                    default:
                        break;
                }
                return TypeInfo{CoreType::FLOAT, 1, 0, 0, 0, 0, 0};
            } else {
                std::vector<OverloadTypeMask> argMasks;
                argMasks.reserve(funcCall.arguments.count);
                for (u32 i = 0; i < funcCall.arguments.count; i++) {
                    TypeInfo argType = GetExpressionType(funcCall.arguments[i]);
                    OverloadTypeMask mask = MakeOverloadMask(argType);
                    if (mask == 0) {
                        return TypeInfo{CoreType::INVALID, 0, 0, 0, 0, 0, 0};
                    }
                    argMasks.push_back(mask);
                }
                Symbol* sym = SymbolTable::LookupFunctionOverload(&symbolTable, funcCall.name,
                    argMasks.data(), static_cast<u32>(argMasks.size()));
                if (sym && sym->kind == SymbolKind::FUNCTION) {
                    FunctionData& funcData = symbolTable.functions[sym->index];
                    return TypeInfo{funcData.returnType, 1, 0, 0, 0, 0, 0};
                }
            }
            break;
        }

        case ASTNodeType::ASSIGNMENT: {
            const AssignmentData& assignData = ast->GetAssignment(expr);
            return GetExpressionType(assignData.value);
        }

        default:
            break;
    }

    return TypeInfo{CoreType::INVALID, 0, 0, 0, 0, 0, 0};
}

//==============================================================================
// Enum parsing
//==============================================================================

NodeRef Parser::ParseEnum() {
    // Note: ENUM token already consumed by ParsePipeline via Match(TokenType::ENUM)
    Consume(TokenType::IDENTIFIER, "Expected enum name");

    std::string enumName(stream->GetValue(previous));
    u32 enumNameOffset = stream->GetOffset(previous);
    u16 enumNameLength = stream->GetLength(previous);
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;

    // Optional underlying type (for bitflags like `enum Channels : u8`)
    CoreType underlyingType = CoreType::INVALID;
    if (Match(TokenType::COLON)) {
        if (MatchMask(TokenMasks::INT_TYPES | TokenMasks::UINT_TYPES)) {
            underlyingType = TokenTypeToReturnType(PreviousTokenType());
        } else {
            Error("Expected integer type after ':'");
        }
    }

    ArenaString enumNameStr = ArenaString::Make(sourceBase(), enumNameOffset, enumNameLength);
    NodeRef enumNode = ASTFactory::MakeEnumDecl(ast, enumNameStr, underlyingType, line, col);

    Consume(TokenType::LEFT_BRACE, "Expected '{'");

    // Parse variants and methods.
    // A progress guard keeps malformed input (e.g. `enum::` after a parse
    // error) from trapping us in an allocation-bomb infinite loop: if a full
    // iteration consumes no tokens we force an Advance and bail.
    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        TokenRef loopStart = current;
        bool isCompileTime = Match(TokenType::EVAL);

        if (IsFunctionDeclStart()) {
            // Method declaration
            NodeRef method = ParseEnumMethod();
            if (method.IsValid()) {
                if (isCompileTime) {
                    ast->GetFunction(method).isEval = true;
                }
                ast->GetEnumDecl(enumNode).methods.Push(arena, method);
            }
        } else if (isCompileTime) {
            Error("'eval' keyword must precede a method declaration");
        } else {
            // Variant declaration
            NodeRef variant = ParseEnumVariant();
            if (variant.IsValid()) {
                ast->GetEnumDecl(enumNode).variants.Push(arena, variant);
            }
            Match(TokenType::COMMA);
        }

        if (current == loopStart) {
            if (stream->GetType(current) == TokenType::EOF_TOKEN) break;
            Advance();
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}'");

    // Add to symbol table
    Symbol* sym = SymbolTable::AddSymbol(&symbolTable,
        enumNameStr, SymbolKind::ENUM_SYMBOL);

    if (sym) {
        EnumData& enumData = symbolTable.enums[sym->index];
        enumData.name = enumNameStr;
        enumData.underlyingType = underlyingType;
        enumData.variants.Init(arena, ast->GetEnumDecl(enumNode).variants.count);
        enumData.methodIndices.Init(arena, ast->GetEnumDecl(enumNode).methods.count);

        // Determine enum type flags
        enumData.flags = 0;
        bool hasData = false;
        bool hasExplicitValues = false;

        for (u32 i = 0; i < ast->GetEnumDecl(enumNode).variants.count; i++) {
            NodeRef variantRef = ast->GetEnumDecl(enumNode).variants[i];
            const EnumDeclData& variantData = ast->GetEnumDecl(variantRef);
            if (variantData.currentVariant.associatedTypes.count > 0) hasData = true;
            if (variantData.currentVariant.value != 0xFFFFFFFF) hasExplicitValues = true;
        }

        if (hasData) {
            enumData.flags |= EnumData::IS_SUM_TYPE;
        }
        if (hasExplicitValues && underlyingType != CoreType::INVALID) {
            enumData.flags |= EnumData::IS_FLAG_ENUM;
        }

        // Copy variants to symbol table
        for (u32 i = 0; i < ast->GetEnumDecl(enumNode).variants.count; i++) {
            NodeRef variantRef = ast->GetEnumDecl(enumNode).variants[i];
            const EnumDeclData& astVariant = ast->GetEnumDecl(variantRef);

            EnumData::Variant variant;
            variant.name = astVariant.currentVariant.name;
            variant.associatedTypes.Init(arena, astVariant.currentVariant.associatedTypes.count);
            variant.associatedTypeHashes.Init(arena, astVariant.currentVariant.associatedTypes.count);

            for (u32 j = 0; j < astVariant.currentVariant.associatedTypes.count; j++) {
                variant.associatedTypes.Push(arena, astVariant.currentVariant.associatedTypes[j]);
                u32 typeHash = 0;
                if (j < astVariant.currentVariant.associatedTypeHashes.count) {
                    typeHash = astVariant.currentVariant.associatedTypeHashes[j];
                }
                variant.associatedTypeHashes.Push(arena, typeHash);
            }

            // Assign values
            if (astVariant.currentVariant.value != 0xFFFFFFFF) {
                variant.value = astVariant.currentVariant.value;
            } else {
                // Auto-assign: for flag enums use powers of 2, otherwise sequential.
                // Clamp the shift count: more than 31 variants in a flag enum
                // would shift past the value width (undefined behaviour). The
                // high bit acts as a saturating sentinel; a semantic error is
                // surfaced later via enum overflow checking.
                if (enumData.flags & EnumData::IS_FLAG_ENUM) {
                    unsigned shift = (i < 31) ? (unsigned)i : 31u;
                    variant.value = (i == 0) ? 1 : (1u << shift);
                } else {
                    variant.value = i;
                }
            }

            enumData.variants.Push(arena, variant);
        }

        // Store method indices (after methods are registered in symbol table)
        for (u32 i = 0; i < ast->GetEnumDecl(enumNode).methods.count; i++) {
            NodeRef methodRef = ast->GetEnumDecl(enumNode).methods[i];
            const FunctionDeclData& method = ast->GetFunction(methodRef);
            std::vector<OverloadTypeMask> paramMasks;
            BuildParamMasks(method.parameters, paramMasks);
            Symbol* methodSym = SymbolTable::LookupFunctionOverload(&symbolTable, method.name,
                paramMasks.data(), static_cast<u32>(paramMasks.size()));
            if (methodSym && methodSym->kind == SymbolKind::FUNCTION) {
                enumData.methodIndices.Push(arena, methodSym->index);
            }
        }
    }

    return enumNode;
}

NodeRef Parser::ParseEnumVariant() {
    Consume(TokenType::IDENTIFIER, "Expected variant name");
    std::string variantName(stream->GetValue(previous));
    u32 variantOffset = stream->GetOffset(previous);
    u16 variantLength = stream->GetLength(previous);
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;

    // Start with auto-value marker (0xFFFFFFFF)
    NodeRef variant = ASTFactory::MakeVariantDecl(
        ast,
        ArenaString::Make(sourceBase(), variantOffset, variantLength),
        0xFFFFFFFF,
        line,
        col);
    ReverseLookup::Register(ast->GetEnumDecl(variant).currentVariant.name.nameHash, variantName.c_str());

    // Check for associated types (sum type variant)
    // e.g., `Constant(float4)` or `Sampled(Texture2D, float2)`
    if (Match(TokenType::LEFT_PAREN)) {
        if (!Check(TokenType::RIGHT_PAREN)) {
            do {
                // Try built-in types first (CORE_TYPES excludes IDENTIFIER)
                if (MatchMask(TokenMasks::CORE_TYPES)) {
                    CoreType type = TokenTypeToReturnType(PreviousTokenType());
                    ast->GetEnumDecl(variant).currentVariant.associatedTypes.Push(arena, type);
                    ast->GetEnumDecl(variant).currentVariant.associatedTypeHashes.Push(arena, 0);
                    // Consume optional parameter name (e.g., "float radius" -> consume "radius")
                    Match(TokenType::IDENTIFIER);
                } else if (Match(TokenType::IDENTIFIER)) {
                    // Custom type like `SDFShape` or `MyStruct`. Preserve the
                    // resolved hash so lowering can distinguish nested enum
                    // payloads from other CUSTOM payloads.
                    std::string typeName(stream->GetValue(previous));
                    u32 typeHash = Utils::HashStr(typeName.c_str());
                    TypeInfo typeInfo = ResolveType(typeName);
                    CoreType type = typeInfo.coreType;
                    u32 customHash = 0;
                    if (type == CoreType::CUSTOM) {
                        customHash = typeInfo.customTypeHash != 0
                                         ? typeInfo.customTypeHash
                                         : typeHash;
                    } else if (type == CoreType::INVALID ||
                               type == CoreType::VOID) {
                        type = CoreType::CUSTOM;
                        customHash = typeHash;
                    }
                    ast->GetEnumDecl(variant).currentVariant.associatedTypes.Push(arena, type);
                    ast->GetEnumDecl(variant).currentVariant.associatedTypeHashes.Push(arena, customHash);
                    // Consume optional parameter name
                    Match(TokenType::IDENTIFIER);
                } else {
                    Error("Expected type in variant");
                    break;
                }
            } while (Match(TokenType::COMMA));
        }

        Consume(TokenType::RIGHT_PAREN, "Expected ')' after variant types");
    }

    // Check for explicit value (for flag enums)
    // e.g., `Red = 0b0001`
    if (Match(TokenType::ASSIGN)) {
        if (Match(TokenType::NUMBER)) {
            std::string_view numStr = stream->GetValue(previous);

            u32 value = 0;
            if (numStr.length() >= 2 && numStr[0] == '0') {
                char prefix = numStr[1];
                if (prefix == 'b' || prefix == 'B') {
                    value = SafeParseU32(numStr.substr(2), 2);
                } else if (prefix == 'x' || prefix == 'X') {
                    value = SafeParseU32(numStr.substr(2), 16);
                } else {
                    value = SafeParseU32(numStr, 0);
                }
            } else {
                value = SafeParseU32(numStr, 0);
            }

            ast->GetEnumDecl(variant).currentVariant.value = value;
        } else {
            Error("Expected constant value after '='");
        }
    }

    return variant;
}

NodeRef Parser::ParseEnumMethod() {
    SourceLocation loc = getLocation(stream->GetOffset(current));
    u32 line = loc.line;
    u32 col = loc.column;

    Consume(TokenType::IDENTIFIER, "Expected method name");
    std::string methodName(stream->GetValue(previous));

    Consume(TokenType::DOUBLE_COLON, "Expected '::' after method name");

    NodeRef method = ASTFactory::MakeFunction(ast, methodName, CoreType::FLOAT, line, col);

    Consume(TokenType::LEFT_PAREN, "Expected '(' after '::'");
    ParseFunctionParameters(method);
    Consume(TokenType::RIGHT_PAREN, "Expected ')' after parameters");

    Consume(TokenType::ARROW, "Expected '->' after parameters");

    if (Check(TokenType::IDENTIFIER)) {
        Advance();
        std::string returnTypeName(stream->GetValue(previous));
        if (Match(TokenType::DOUBLE_COLON)) {
            std::string moduleName = returnTypeName;
            Consume(TokenType::IDENTIFIER, "Expected type name after '::'");
            returnTypeName = CanonicalizeModuleQualifiedName(
                moduleName, std::string(stream->GetValue(previous)));
        }
        TypeInfo resolved = ResolveType(returnTypeName);
        if (resolved.coreType != CoreType::INVALID) {
            ast->GetFunction(method).returnType = resolved.coreType;
            ast->GetFunction(method).returnTypeHash = resolved.customTypeHash;
        } else {
            ast->GetFunction(method).returnType = CoreType::CUSTOM;
            ast->GetFunction(method).returnTypeHash = Utils::HashStr(returnTypeName.c_str());
        }
    } else if (MatchMask(TokenMasks::CORE_TYPES)) {
        ast->GetFunction(method).returnType = TokenTypeToReturnType(static_cast<TokenType>(stream->GetType(previous)));
        ast->GetFunction(method).returnTypeHash = 0;
    } else if (Match(TokenType::VOID)) {
        ast->GetFunction(method).returnType = CoreType::VOID;
        ast->GetFunction(method).returnTypeHash = 0;
    } else {
        Error("Expected valid return type after '->'");
        return NodeRef::Null();
    }

    const FunctionDeclData& decl = ast->GetFunction(method);
    std::vector<OverloadTypeMask> paramMasks;
    BuildParamMasks(decl.parameters, paramMasks);
    u64 signatureKey = HashOverloadSignature(paramMasks.data(), static_cast<u32>(paramMasks.size()));

    // Add method to symbol table
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
    FillFunctionData(&symbolTable, arena, decl, method, paramMasks, signatureKey, sym->index);

    // Parse method body
    Consume(TokenType::LEFT_BRACE, "Expected '{' before method body");

    // Method body can be either:
    // 1. Pattern match arms (for matching on self)
    // 2. Regular statements with return
    bool isPatternMatchBody = false;
    if (Check(TokenType::IDENTIFIER)) {
        TokenRef next = PeekNext();
        TokenType nextType = static_cast<TokenType>(stream->GetType(next));
        if (nextType == TokenType::COLON || nextType == TokenType::LEFT_PAREN) {
            isPatternMatchBody = true;
        }
    }

    if (isPatternMatchBody) {
        // Parse as implicit pattern match on self
        NodeRef selfRef = ASTFactory::MakeIdentifier(ast, "self", line, col);
        ast->GetFunction(method).body = ParsePatternMatch(selfRef);
    } else {
        // Regular function body
        ast->GetFunction(method).body = ParseBlock();
    }

    return method;
}

NodeRef Parser::ParsePatternMatch(NodeRef scrutinee) {
    // Pattern match syntax (without explicit 'match' keyword):
    // Pattern: expression
    // Pattern(bindings): expression
    // Pattern(bindings): { statements }
    // default: expression
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;

    // Get the scrutinee name for the match node
    ArenaString scrutineeName;
    if (scrutinee.Type() == ASTNodeType::IDENTIFIER) {
        scrutineeName = ast->GetIdentifier(scrutinee).name;
    } else {
        scrutineeName = ArenaString::MakeHashOnly("_scrutinee");
    }

    NodeRef matchNode = ASTFactory::MakePatternMatch(ast, scrutineeName, line, col);

    // Parse pattern arms until we hit closing brace
    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        ArenaString variantName;
        bool isDefault = false;

        // Check for 'default' pattern
        if (Match(TokenType::DEFAULT)) {
            isDefault = true;
            variantName = ArenaString::MakeHashOnly("default");
        } else {
            Consume(TokenType::IDENTIFIER, "Expected variant name or 'default'");
            variantName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));
        }

        NodeRef arm = ASTFactory::MakePatternMatchArm(ast, variantName, isDefault, line, col);

        // Check for bindings (not for default)
        if (!isDefault && Match(TokenType::LEFT_PAREN)) {
            if (!Check(TokenType::RIGHT_PAREN)) {
                do {
                    if (Match(TokenType::IDENTIFIER)) {
                        ArenaString binding = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));
                        ast->GetPatternMatch(arm).bindings.Push(arena, std::make_pair(binding, binding));
                    } else if (Match(TokenType::UNDERSCORE)) {
                        // Wildcard binding
                        ArenaString underscore = ArenaString::MakeHashOnly("_");
                        ast->GetPatternMatch(arm).bindings.Push(arena, std::make_pair(underscore, underscore));
                    } else {
                        Error("Expected identifier or '_' in pattern binding");
                        break;
                    }
                } while (Match(TokenType::COMMA));
            }

            Consume(TokenType::RIGHT_PAREN, "Expected ')' after pattern bindings");
        }

        Consume(TokenType::COLON, "Expected ':' after pattern");

        // Parse arm body (either expression or block)
        if (Match(TokenType::LEFT_BRACE)) {
            ast->GetPatternMatch(arm).body = ParseBlock();
        } else {
            // Single expression
            ast->GetPatternMatch(arm).body = ParseExpression();
            Match(TokenType::SEMICOLON); // Optional semicolon
        }

        ast->GetPatternMatch(matchNode).arms.Push(arena, arm);

        // Check if next token starts a new pattern
        if (!Check(TokenType::IDENTIFIER) && !Check(TokenType::DEFAULT) &&
            !Check(TokenType::RIGHT_BRACE)) {
            break;
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after pattern match");

    return matchNode;
}

//==============================================================================
// Type pattern matching for generics
//==============================================================================

NodeRef Parser::ParseTypePatternMatch() {
    // Type pattern match body for generic functions:
    //   float2: v * 2.0
    //   float3: cross(v, float3(0.0, 1.0, 0.0))
    //   float4: v.wzyx
    //   default: v  // optional

    SourceLocation loc = getLocation(stream->GetOffset(current));
    u32 line = loc.line;
    u32 col = loc.column;

    NodeRef matchNode = ASTFactory::MakeTypePatternMatch(ast, line, col);

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        bool isDefault = false;
        CoreType armType = CoreType::VOID;

        if (Match(TokenType::DEFAULT)) {
            isDefault = true;
        } else if (MatchMask(TokenMasks::CORE_TYPES)) {
            TokenType typeToken = PreviousTokenType();
            TypeInfo typeInfo = GetTypeInfoFromToken(typeToken);
            armType = typeInfo.coreType;
        } else {
            // Not a valid arm start - we're done
            break;
        }

        Consume(TokenType::COLON, "Expected ':' after type in type pattern");

        NodeRef body;
        if (Match(TokenType::LEFT_BRACE)) {
            body = ParseBlock();
        } else {
            body = ParseExpression();
        }

        NodeRef arm = ASTFactory::MakeTypePatternArm(ast, armType, isDefault, body, line, col);

        if (isDefault) {
            ast->GetTypePatternMatch(matchNode).defaultArm = arm;
        }
        ast->GetTypePatternMatch(matchNode).arms.Push(arena, arm);
    }

    return matchNode;
}

//==============================================================================
// Struct parsing
//==============================================================================

NodeRef Parser::ParseStructMethod(u32 ownerStructTypeHash, bool isCompileTime) {
    SourceLocation loc = getLocation(stream->GetOffset(current));
    u32 line = loc.line;
    u32 col = loc.column;

    Consume(TokenType::IDENTIFIER, "Expected method name");
    std::string methodName(stream->GetValue(previous));

    Consume(TokenType::DOUBLE_COLON, "Expected '::' after method name");

    NodeRef method = ASTFactory::MakeFunction(ast, methodName, CoreType::FLOAT, line, col);
    FunctionDeclData& methodDecl = ast->GetFunction(method);
    methodDecl.isEval = isCompileTime;
    methodDecl.isStructMethod = true;
    methodDecl.isConstMethod = false;
    methodDecl.ownerStructTypeHash = ownerStructTypeHash;

    Consume(TokenType::LEFT_PAREN, "Expected '(' after '::'");
    ParseFunctionParameters(method);
    Consume(TokenType::RIGHT_PAREN, "Expected ')' after parameters");

    if (Match(TokenType::CONST)) {
        ast->GetFunction(method).isConstMethod = true;
    }

    Consume(TokenType::ARROW, "Expected '->' after parameters");

    if (Check(TokenType::IDENTIFIER)) {
        Advance();
        std::string returnTypeName(stream->GetValue(previous));
        if (Match(TokenType::DOUBLE_COLON)) {
            std::string moduleName = returnTypeName;
            Consume(TokenType::IDENTIFIER, "Expected type name after '::'");
            returnTypeName = CanonicalizeModuleQualifiedName(
                moduleName, std::string(stream->GetValue(previous)));
        }
        TypeInfo resolved = ResolveType(returnTypeName);
        if (resolved.coreType != CoreType::INVALID) {
            ast->GetFunction(method).returnType = resolved.coreType;
            ast->GetFunction(method).returnTypeHash = resolved.customTypeHash;
        } else {
            ast->GetFunction(method).returnType = CoreType::CUSTOM;
            ast->GetFunction(method).returnTypeHash = Utils::HashStr(returnTypeName.c_str());
        }
    } else if (MatchMask(TokenMasks::CORE_TYPES)) {
        ast->GetFunction(method).returnType = TokenTypeToReturnType(PreviousTokenType());
        ast->GetFunction(method).returnTypeHash = 0;
    } else if (Match(TokenType::VOID)) {
        ast->GetFunction(method).returnType = CoreType::VOID;
        ast->GetFunction(method).returnTypeHash = 0;
    } else {
        Error("Expected valid return type after '->'");
        return NodeRef::Null();
    }

    Consume(TokenType::LEFT_BRACE, "Expected '{' before method body");
    ast->GetFunction(method).body = ParseBlock();
    return method;
}

NodeRef Parser::ParseStruct() {
    // Note: STRUCT token already consumed by caller
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;

    Consume(TokenType::IDENTIFIER, "Expected struct name");
    std::string structName(stream->GetValue(previous));

    NodeRef structNode = ASTFactory::MakeStructDecl(ast, structName, line, col);

    Consume(TokenType::LEFT_BRACE, "Expected '{'");

    StructData structData;
    structData.name = ArenaString::MakeHashOnly(structName);
    structData.fields.Init(arena, 8);
    structData.methodIndices.Init(arena, 4);
    structData.isIndexable = false;

    // Progress guard against malformed struct bodies that could otherwise
    // trap the parser in an allocation-bomb infinite loop (e.g. a field
    // with an embedded `.` like `float3 a.b = ...;`). Same pattern as the
    // variant loop in ParseEnum.
    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        TokenRef loopStart = current;
        bool isCompileTime = Match(TokenType::EVAL);

        if (IsFunctionDeclStart()) {
            NodeRef method = ParseStructMethod(structData.name.nameHash, isCompileTime);
            if (method.IsValid()) {
                const FunctionDeclData& decl = ast->GetFunction(method);
                std::vector<OverloadTypeMask> paramMasks;
                BuildParamMasks(decl.parameters, paramMasks);
                u64 signatureKey = HashOverloadSignature(paramMasks.data(),
                    static_cast<u32>(paramMasks.size()));

                if (HasDuplicateMethodInList(ast, ast->GetStructDecl(structNode).methods,
                        decl, paramMasks, signatureKey)) {
                    Error("Struct method overload already declared");
                } else {
                    u32 methodIndex = symbolTable.functions.count;
                    symbolTable.functions.Push(arena, FunctionData{});
                    FillFunctionData(&symbolTable, arena, decl, method, paramMasks,
                                     signatureKey, methodIndex);
                    structData.methodIndices.Push(arena, methodIndex);
                    ast->GetStructDecl(structNode).methods.Push(arena, method);
                }
            }
            if (current == loopStart) {
                if (stream->GetType(current) == TokenType::EOF_TOKEN) break;
                Advance();
            }
            continue;
        }

        if (isCompileTime) {
            Error("'eval' keyword must precede a method declaration");
            if (current == loopStart) {
                if (stream->GetType(current) == TokenType::EOF_TOKEN) break;
                Advance();
            }
            continue;
        }

        // Parse field type (core or custom/module-qualified)
        TypeInfo fieldType = TYPE_INFO(CoreType::INVALID, 0, false);
        std::string fieldTypeName;
        if (MatchMask(TokenMasks::CORE_TYPES)) {
            fieldType = GetTypeInfoFromToken(PreviousTokenType());
            fieldTypeName = std::string(stream->GetValue(previous));
        } else if (Match(TokenType::IDENTIFIER)) {
            fieldTypeName = std::string(stream->GetValue(previous));
            if (Match(TokenType::DOUBLE_COLON)) {
                std::string moduleName = fieldTypeName;
                Consume(TokenType::IDENTIFIER, "Expected type name after '::'");
                fieldTypeName = CanonicalizeModuleQualifiedName(
                    moduleName, std::string(stream->GetValue(previous)));
            }
            fieldType = ResolveType(fieldTypeName);
        } else {
            Error("Expected field type");
            Synchronize();
            if (current == loopStart) {
                if (stream->GetType(current) == TokenType::EOF_TOKEN) break;
                Advance();
            }
            continue;
        }

        if (fieldType.coreType == CoreType::INVALID) {
            Error("Unknown field type '" + fieldTypeName + "'");
            Synchronize();
            if (current == loopStart) {
                if (stream->GetType(current) == TokenType::EOF_TOKEN) break;
                Advance();
            }
            continue;
        }

        auto ParseFieldArraySize = [&]() -> u32 {
            u32 sizeValue = 0;
            if (Match(TokenType::NUMBER)) {
                // Literal number size
                std::string_view numStr = stream->GetValue(previous);
                sizeValue = SafeParseU32(numStr, 0);
            } else if (Match(TokenType::IDENTIFIER)) {
                // Constant name (e.g., MAX_LIGHTS)
                std::string constName(stream->GetValue(previous));
                ArenaString constArena = ArenaString::MakeHashOnly(constName);

                bool found = false;

                // Look up in symbol table for eval constants
                Symbol* sym = SymbolTable::LookupAny(&symbolTable, constArena);
                if (sym) {
                    if (sym->kind == SymbolKind::VARIABLE) {
                        const VariableData& varData = symbolTable.variables[sym->index];
                        if (varData.isEval && varData.evalValue.type == LiteralValue::INT) {
                            sizeValue = static_cast<u32>(varData.evalValue.intValue);
                            found = true;
                        }
                    } else if (sym->kind == SymbolKind::EVAL_CONSTANT) {
                        const LiteralValue& constVal = symbolTable.evalConstants[sym->index];
                        if (constVal.type == LiteralValue::INT) {
                            sizeValue = static_cast<u32>(constVal.intValue);
                            found = true;
                        }
                    }
                }

                // Try module-qualified lookup if in module scope
                if (!found && symbolTable.inModuleScope && symbolTable.currentModuleIndex != INVALID_INDEX) {
                    const ModuleData& mod = symbolTable.modules[symbolTable.currentModuleIndex];
                    std::string qualifiedName = mod.name.ToString(sourceBase()) + "::" + constName;
                    ArenaString qualifiedArena = ArenaString::MakeHashOnly(qualifiedName);
                    sym = SymbolTable::LookupAny(&symbolTable, qualifiedArena);
                    if (sym && sym->kind == SymbolKind::EVAL_CONSTANT) {
                        const LiteralValue& constVal = symbolTable.evalConstants[sym->index];
                        if (constVal.type == LiteralValue::INT) {
                            sizeValue = static_cast<u32>(constVal.intValue);
                            found = true;
                        }
                    }
                }

                if (!found) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Unknown constant '%s' for array size", constName.c_str());
                    Error(msg);
                }
            } else {
                Error("Expected array size (number or constant)");
            }
            Consume(TokenType::RIGHT_BRACKET, "Expected ']' after array size");
            structData.isIndexable = true;
            return sizeValue;
        };

        u32 arraySize = 0;
        bool arrayBeforeName = false;
        if (Match(TokenType::LEFT_BRACKET)) {
            arraySize = ParseFieldArraySize();
            arrayBeforeName = true;
        }

        Consume(TokenType::IDENTIFIER, "Expected field name");
        std::string fieldNameStr(stream->GetValue(previous));

        // Create field with pre-computed hash
        StructData::Field field;
        field.name = ArenaString::MakeHashOnly(fieldNameStr);
        // Register field name for debug symbol lookup
        ReverseLookup::Register(field.name.nameHash, fieldNameStr.c_str());
        field.type = fieldType;
        field.arraySize = arraySize;

        // Check for fixed-size array [size] after name
        if (Match(TokenType::LEFT_BRACKET)) {
            if (arrayBeforeName) {
                Error("Multiple array size declarations for struct field");
            }
            field.arraySize = ParseFieldArraySize();
        }

        // Add to AST node (for code generation)
        StructFieldData astField;
        astField.name = field.name;
        astField.type = field.type;
        astField.arraySize = field.arraySize;
        ast->GetStructDecl(structNode).fields.Push(arena, astField);

        // Add to struct data (for symbol table)
        structData.fields.Push(arena, field);

        Match(TokenType::SEMICOLON); // Optional semicolon

        if (current == loopStart) {
            if (stream->GetType(current) == TokenType::EOF_TOKEN) break;
            Advance();
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}'");

    // NOTE: Do NOT register with g_customTypes here - we need to use the pointer
    // to the stored copy in symbolTable.structs, not a local variable pointer.
    // Registration happens below after storing in symbol table.

    // Register in symbol table
    if (symbolTable.inModuleScope &&
        symbolTable.currentModuleIndex != INVALID_INDEX &&
        symbolTable.currentModuleIndex < symbolTable.modules.count) {
        // Module struct - create human-readable qualified name (e.g., "Globals::LightSourcesSoA")
        const ModuleData& currentModule = symbolTable.modules[symbolTable.currentModuleIndex];
        // Use ReverseLookup to get the actual module name string
        std::string moduleNameStr = ReverseLookup::GetString(currentModule.name.nameHash);
        std::string humanQualifiedName = moduleNameStr + "::" + structName;
        ArenaString humanQualifiedArena = ArenaString::MakeHashOnly(humanQualifiedName);

        // Register qualified name in reverse lookup for later resolution
        ReverseLookup::Register(humanQualifiedArena.nameHash, humanQualifiedName.c_str());

        // Add to symbol table with human-readable qualified name
        Symbol* sym = SymbolTable::AddSymbol(&symbolTable, humanQualifiedArena,
                                            SymbolKind::CUSTOM_TYPE);
        if (sym) {
            symbolTable.structs[sym->index] = structData;

            // Register with human-readable qualified name in global registry
            // Use pointer to the stored copy in symbolTable.structs
            g_customTypes.RegisterType(humanQualifiedArena, &symbolTable.structs[sym->index]);

            // Also register with unqualified name for function signature matching
            // This allows PBR::PBRMaterial variables to match PBRMaterial parameters
            g_customTypes.RegisterType(structData.name, &symbolTable.structs[sym->index]);

            // Add to module's struct list
            symbolTable.modules[symbolTable.currentModuleIndex].structIndices.Push(
                arena, sym->index);
        }
    } else {
        // Global struct - register name for debug symbol lookup
        ReverseLookup::Register(structData.name.nameHash, structName.c_str());

        Symbol* sym = SymbolTable::AddSymbol(&symbolTable, structData.name,
                                            SymbolKind::CUSTOM_TYPE);
        if (sym) {
            symbolTable.structs[sym->index] = structData;

            // Register with global registry using pointer to stored copy
            g_customTypes.RegisterType(structData.name, &symbolTable.structs[sym->index]);
        }
    }

    return structNode;
}

//==============================================================================
// Module parsing
//==============================================================================


} // namespace BWSL
