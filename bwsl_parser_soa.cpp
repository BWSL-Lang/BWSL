#include <cstddef>
#include "bwsl_parser_soa.h"
#include "bwsl_eval_soa.h"
#include "bwsl_utils.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <stdexcept>
#include "bwsl_custom_type_registry.h"

namespace {
// Fuzzer-proof integer parsers: return 0 on empty / malformed / out-of-range
// input instead of throwing. The lexer's number rules are not strict enough
// to guarantee std::stoul / std::stoi success (e.g. "0x" with no digits,
// "0b2", trailing garbage); these helpers keep the parser safe against
// adversarial input.
inline u32 SafeParseU32(std::string_view s, int base = 0) {
    if (s.empty()) return 0;
    try {
        return static_cast<u32>(std::stoul(std::string(s), nullptr, base));
    } catch (const std::exception&) {
        return 0;
    }
}
inline int SafeParseInt(std::string_view s, int base = 0) {
    if (s.empty()) return 0;
    try {
        return std::stoi(std::string(s), nullptr, base);
    } catch (const std::exception&) {
        return 0;
    }
}
inline float SafeParseFloat(std::string_view s) {
    if (s.empty()) return 0.0f;
    try {
        return std::stof(std::string(s));
    } catch (const std::exception&) {
        return 0.0f;
    }
}
}

// Parser timing instrumentation - define BWSL_PARSER_TIMING to enable
#ifdef BWSL_PARSER_TIMING
#include <chrono>
#include <cstdio>

namespace parser_timing {
    struct Stats {
        double advance_time = 0;
        double expression_time = 0;
        double statement_time = 0;
        double block_time = 0;
        double function_time = 0;
        double string_alloc_time = 0;
        int advance_count = 0;
        int expression_count = 0;
        int statement_count = 0;
        int string_alloc_count = 0;

        void print() {
            fprintf(stderr, "\n=== Parser Timing ===\n");
            fprintf(stderr, "Advance():     %.3f ms (%d calls, %.3f us/call)\n",
                    advance_time, advance_count, advance_count ? advance_time*1000/advance_count : 0);
            fprintf(stderr, "Expression:    %.3f ms (%d calls)\n", expression_time, expression_count);
            fprintf(stderr, "Statement:     %.3f ms (%d calls)\n", statement_time, statement_count);
            fprintf(stderr, "Block:         %.3f ms\n", block_time);
            fprintf(stderr, "Function:      %.3f ms\n", function_time);
            fprintf(stderr, "String alloc:  %.3f ms (%d allocs, %.3f us/alloc)\n",
                    string_alloc_time, string_alloc_count, string_alloc_count ? string_alloc_time*1000/string_alloc_count : 0);
            fprintf(stderr, "=====================\n\n");
        }

        void reset() { *this = Stats{}; }
    };

    inline Stats& get() { static Stats s; return s; }

    struct Timer {
        double* acc;
        std::chrono::high_resolution_clock::time_point start;
        Timer(double* a) : acc(a), start(std::chrono::high_resolution_clock::now()) {}
        ~Timer() {
            auto end = std::chrono::high_resolution_clock::now();
            *acc += std::chrono::duration<double, std::milli>(end - start).count();
        }
    };
}

#define PARSER_TIME_ADVANCE() parser_timing::Timer _t(&parser_timing::get().advance_time); parser_timing::get().advance_count++
#define PARSER_TIME_EXPR() parser_timing::Timer _t(&parser_timing::get().expression_time); parser_timing::get().expression_count++
#define PARSER_TIME_STMT() parser_timing::Timer _t(&parser_timing::get().statement_time); parser_timing::get().statement_count++
#define PARSER_TIME_BLOCK() parser_timing::Timer _t(&parser_timing::get().block_time)
#define PARSER_TIME_FUNC() parser_timing::Timer _t(&parser_timing::get().function_time)
#define PARSER_TIME_STRING() parser_timing::Timer _t(&parser_timing::get().string_alloc_time); parser_timing::get().string_alloc_count++
#define PARSER_TIMING_PRINT() parser_timing::get().print()
#define PARSER_TIMING_RESET() parser_timing::get().reset()

#else

#define PARSER_TIME_ADVANCE()
#define PARSER_TIME_EXPR()
#define PARSER_TIME_STMT()
#define PARSER_TIME_BLOCK()
#define PARSER_TIME_FUNC()
#define PARSER_TIME_STRING()
#define PARSER_TIMING_PRINT()
#define PARSER_TIMING_RESET()

#endif

namespace BWSL {

namespace {

static std::string StripFixedArraySuffixes(const std::string& typeName) {
    std::string base = typeName;
    while (!base.empty() && base.back() == ']') {
        size_t left = base.rfind('[');
        if (left == std::string::npos) break;
        bool digitsOnly = true;
        for (size_t i = left + 1; i + 1 < base.size(); i++) {
            if (!std::isdigit(static_cast<unsigned char>(base[i]))) {
                digitsOnly = false;
                break;
            }
        }
        if (!digitsOnly) break;
        base.erase(left);
    }
    return base;
}

static bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

static std::string ParseInnerResourceType(std::string_view typeName, std::string_view prefix) {
    if (!StartsWith(typeName, prefix) || typeName.size() <= prefix.size() + 1 || typeName.back() != '>') {
        return {};
    }
    return std::string(typeName.substr(prefix.size(), typeName.size() - prefix.size() - 1));
}

static TypeInfo MakeTypeInfoForCoreType(CoreType coreType) {
    switch (coreType) {
        case CoreType::BOOL:   return TYPE_INFO(CoreType::BOOL,   1, false);
        case CoreType::INT:    return TYPE_INFO(CoreType::INT,    1, false);
        case CoreType::UINT:   return TYPE_INFO(CoreType::UINT,   1, false);
        case CoreType::FLOAT:  return TYPE_INFO(CoreType::FLOAT,  1, false);
        case CoreType::BOOL2:  return TYPE_INFO(CoreType::BOOL2,  2, true);
        case CoreType::BOOL3:  return TYPE_INFO(CoreType::BOOL3,  3, true);
        case CoreType::BOOL4:  return TYPE_INFO(CoreType::BOOL4,  4, true);
        case CoreType::INT2:   return TYPE_INFO(CoreType::INT2,   2, true);
        case CoreType::INT3:   return TYPE_INFO(CoreType::INT3,   3, true);
        case CoreType::INT4:   return TYPE_INFO(CoreType::INT4,   4, true);
        case CoreType::UINT2:  return TYPE_INFO(CoreType::UINT2,  2, true);
        case CoreType::UINT3:  return TYPE_INFO(CoreType::UINT3,  3, true);
        case CoreType::UINT4:  return TYPE_INFO(CoreType::UINT4,  4, true);
        case CoreType::FLOAT2: return TYPE_INFO(CoreType::FLOAT2, 2, true);
        case CoreType::FLOAT3: return TYPE_INFO(CoreType::FLOAT3, 3, true);
        case CoreType::FLOAT4: return TYPE_INFO(CoreType::FLOAT4, 4, true);
        case CoreType::MAT2:   return TYPE_INFO(CoreType::MAT2,   4, true);
        case CoreType::MAT3:   return TYPE_INFO(CoreType::MAT3,   9, true);
        case CoreType::MAT4:   return TYPE_INFO(CoreType::MAT4,   16, true);
        case CoreType::TEXTURE2D:      return TYPE_INFO(CoreType::TEXTURE2D,      0, false);
        case CoreType::TEXTURE3D:      return TYPE_INFO(CoreType::TEXTURE3D,      0, false);
        case CoreType::TEXTURECUBE:    return TYPE_INFO(CoreType::TEXTURECUBE,    0, false);
        case CoreType::TEXTURE2DARRAY: return TYPE_INFO(CoreType::TEXTURE2DARRAY, 0, false);
        case CoreType::SAMPLER:        return TYPE_INFO(CoreType::SAMPLER,        0, false);
        case CoreType::CBUFFER:        return TYPE_INFO(CoreType::CBUFFER,        0, false);
        case CoreType::BUFFER:         return TYPE_INFO(CoreType::BUFFER,         0, false);
        default:
            return TYPE_INFO(CoreType::INVALID, 0, false);
    }
}

static TypeInfo MakeCustomTypeInfo(SymbolTableData* table, u32 typeHash) {
    if (typeHash == 0) {
        return TYPE_INFO(CoreType::INVALID, 0, false);
    }

    Symbol* sym = SymbolTable::LookupByHash(table, typeHash);
    if (sym && sym->kind == SymbolKind::CUSTOM_TYPE) {
        const StructData& structData = table->structs[sym->index];
        return TypeInfo{
            CoreType::CUSTOM,
            static_cast<u8>(structData.fields.count),
            structData.isIndexable,
            0,
            structData.name.nameHash,
            0,
            0
        };
    }

    return TypeInfo{CoreType::CUSTOM, 1, 0, 0, typeHash, 0, 0};
}

static TypeInfo MakeTypeInfoForResource(SymbolTableData* table, const ResourceData& data) {
    CoreType coreType = static_cast<CoreType>(data.coreType);

    switch (data.type) {
        case ResourceBinding::Texture:
            if (coreType == CoreType::INVALID) {
                coreType = data.isArrayTexture ? CoreType::TEXTURE2DARRAY
                         : data.isCubemapTexture ? CoreType::TEXTURECUBE
                         : CoreType::TEXTURE2D;
            }
            return MakeTypeInfoForCoreType(coreType);

        case ResourceBinding::Sampler:
            return MakeTypeInfoForCoreType(CoreType::SAMPLER);

        case ResourceBinding::UniformBuffer: {
            if (data.structTypeHash != 0 || coreType == CoreType::CUSTOM) {
                u32 typeHash = data.structTypeHash != 0 ? data.structTypeHash : data.typeName.nameHash;
                return MakeCustomTypeInfo(table, typeHash);
            }
            return MakeTypeInfoForCoreType(coreType);
        }

        case ResourceBinding::StorageBuffer: {
            TypeInfo elementType = TYPE_INFO(CoreType::INVALID, 0, false);
            if (data.structTypeHash != 0 || coreType == CoreType::CUSTOM) {
                u32 typeHash = data.structTypeHash != 0 ? data.structTypeHash : data.typeName.nameHash;
                elementType = MakeCustomTypeInfo(table, typeHash);
            } else {
                elementType = MakeTypeInfoForCoreType(coreType);
            }

            if (elementType.coreType == CoreType::INVALID) {
                return elementType;
            }

            elementType.arrayDimensions = elementType.arrayDimensions > 0 ? elementType.arrayDimensions : 1;
            if (elementType.arrayStride == 0) {
                elementType.arrayStride = static_cast<u32>(elementType.componentCount ? elementType.componentCount : 1) * 4u;
            }
            return elementType;
        }

        case ResourceBinding::StorageImage:
            return MakeTypeInfoForCoreType(CoreType::TEXTURE2D);

        default:
            return TYPE_INFO(CoreType::INVALID, 0, false);
    }
}

static void RegisterParsedResource(SymbolTableData* table,
                                   const std::string& resourceName,
                                   const std::string& typeName,
                                   u32 bindingIndex) {
    ArenaString resourceArena = ArenaString::MakeHashOnly(resourceName);
    Symbol* sym = SymbolTable::AddResource(table, resourceArena);
    if (!sym) {
        sym = SymbolTable::LookupResource(table, resourceArena);
        if (!sym || sym->kind != SymbolKind::RESOURCE) return;
    }

    ReverseLookup::Register(resourceArena.nameHash, resourceName.c_str());

    ResourceData& data = table->resources[sym->index];
    data = ResourceData{};
    data.bindingIndex = bindingIndex;
    data.stageFlags = SymbolTable::ShaderStageToBit(ShaderStage::Vertex) |
                      SymbolTable::ShaderStageToBit(ShaderStage::Fragment) |
                      SymbolTable::ShaderStageToBit(ShaderStage::Compute);
    data.typeName = ArenaString::MakeHashOnly(typeName);
    ReverseLookup::Register(data.typeName.nameHash, typeName.c_str());

    if (typeName == "sampler") {
        data.type = ResourceBinding::Sampler;
        data.coreType = static_cast<u8>(CoreType::SAMPLER);
        return;
    }
    if (typeName == "texture2D") {
        data.type = ResourceBinding::Texture;
        data.coreType = static_cast<u8>(CoreType::TEXTURE2D);
        return;
    }
    if (typeName == "texture3D") {
        data.type = ResourceBinding::Texture;
        data.coreType = static_cast<u8>(CoreType::TEXTURE3D);
        return;
    }
    if (typeName == "textureCube") {
        data.type = ResourceBinding::Texture;
        data.coreType = static_cast<u8>(CoreType::TEXTURECUBE);
        data.isCubemapTexture = true;
        return;
    }
    if (typeName == "texture2DArray") {
        data.type = ResourceBinding::Texture;
        data.coreType = static_cast<u8>(CoreType::TEXTURE2DARRAY);
        data.isArrayTexture = true;
        return;
    }

    std::string innerType = ParseInnerResourceType(typeName, "buffer<");
    if (!innerType.empty()) {
        data.type = ResourceBinding::StorageBuffer;
        data.typeName = ArenaString::MakeHashOnly(innerType);
        ReverseLookup::Register(data.typeName.nameHash, innerType.c_str());
        std::string baseType = StripFixedArraySuffixes(innerType);
        CoreType coreType = SymbolTable::ParseTypeName(baseType);
        data.coreType = static_cast<u8>(coreType);
        if (coreType == CoreType::CUSTOM) {
            data.structTypeHash = Utils::HashStr(baseType.c_str());
        }
        return;
    }

    innerType = ParseInnerResourceType(typeName, "cbuffer<");
    if (!innerType.empty()) {
        data.type = ResourceBinding::UniformBuffer;
        data.typeName = ArenaString::MakeHashOnly(innerType);
        ReverseLookup::Register(data.typeName.nameHash, innerType.c_str());
        std::string baseType = StripFixedArraySuffixes(innerType);
        CoreType coreType = SymbolTable::ParseTypeName(baseType);
        data.coreType = static_cast<u8>(coreType);
        if (coreType == CoreType::CUSTOM) {
            data.structTypeHash = Utils::HashStr(baseType.c_str());
        }
        return;
    }

    data.type = ResourceBinding::UniformBuffer;
    std::string baseType = StripFixedArraySuffixes(typeName);
    CoreType coreType = SymbolTable::ParseTypeName(baseType);
    data.coreType = static_cast<u8>(coreType);
    if (coreType == CoreType::CUSTOM) {
        data.structTypeHash = Utils::HashStr(baseType.c_str());
    }
}

} // namespace

// Global module search paths - can be extended by external tools (e.g., bwslc)
static std::vector<std::string> g_additionalModuleSearchPaths;

void AddModuleSearchPath(const std::string& path) {
    g_additionalModuleSearchPaths.push_back(path);
}

void ClearModuleSearchPaths() {
    g_additionalModuleSearchPaths.clear();
}

namespace {
    std::string ResolveModulePath(const std::string& moduleName) {
        // Build search roots: additional paths first (highest priority), then built-in paths
        std::vector<std::string> searchRoots;

        // Additional paths added by external tools (e.g., input file directory)
        for (const auto& path : g_additionalModuleSearchPaths) {
            searchRoots.push_back(path);
            searchRoots.push_back(path + "/modules");
            searchRoots.push_back(path + "/../modules");
            searchRoots.push_back(path + "/../bwsl/modules");
        }

        // Built-in paths based on source file location (works in engine builds)
        static const std::vector<std::string> builtInRoots = [] {
            std::vector<std::string> roots;
            std::string file = __FILE__;
            auto pos = file.find_last_of("/\\");
            std::string sourceDir = (pos == std::string::npos) ? std::string(".") : file.substr(0, pos);
            roots.push_back(sourceDir + "/modules");
            roots.push_back(sourceDir + "/../modules");
            roots.push_back("bwsl/modules");
            roots.push_back("modules");
            roots.push_back("../modules");
            roots.push_back("../../modules");
            return roots;
        }();

        for (const auto& root : builtInRoots) {
            searchRoots.push_back(root);
        }

        // Generate lowercase version of module name for case-insensitive lookup
        std::string lowerName = moduleName;
        for (char& c : lowerName) {
            if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        }

        std::vector<std::string> candidateNames = {
            moduleName + "_module.bwsl",
            moduleName + ".bwsl",
            lowerName + "_module.bwsl",
            lowerName + ".bwsl"
        };

        for (const auto& root : searchRoots) {
            for (const auto& fileName : candidateNames) {
                std::string candidate = root + "/" + fileName;
                std::ifstream file(candidate);
                if (file.good()) {
                    return candidate;
                }
            }
        }
        return {};
    }

    void BuildParamMasks(const ArenaArray<std::pair<ArenaString, ArenaString>>& params,
        std::vector<OverloadTypeMask>& outMasks) {
        outMasks.clear();
        outMasks.reserve(params.count);
        for (u32 i = 0; i < params.count; i++) {
            const auto& param = params[i];
            outMasks.push_back(MakeOverloadMaskFromTypeHash(param.second.nameHash));
        }
    }

    bool HasDuplicateFunctionSignature(SymbolTableData* table, const ArenaString& name,
        const std::vector<OverloadTypeMask>& paramMasks, NamespaceKind ns, u32 moduleIndex,
        u64 signatureKey) {
        u32 scopeStart = table->scopeStartIndices[table->currentScope];
        for (u32 i = scopeStart; i < table->symbols.count; i++) {
            Symbol& existing = table->symbols[i];
            if (existing.kind != SymbolKind::FUNCTION) continue;
            if (existing.name.nameHash != name.nameHash) continue;
            if (existing.namespaceKind != ns) continue;
            if (ns == NamespaceKind::MODULE && existing.moduleIndex != moduleIndex) continue;

            const FunctionData& funcData = table->functions[existing.index];
            if (funcData.signatureKey != signatureKey) continue;
            if (funcData.paramTypeMasks.count != paramMasks.size()) continue;

            bool matches = true;
            for (u32 j = 0; j < funcData.paramTypeMasks.count; j++) {
                if (funcData.paramTypeMasks[j] != paramMasks[j]) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                return true;
            }
        }
        return false;
    }

    bool HasDuplicateFunctionInList(AST* ast, const ArenaArray<NodeRef>& functions,
        const FunctionDeclData& decl, const std::vector<OverloadTypeMask>& paramMasks,
        u64 signatureKey) {
        for (u32 i = 0; i < functions.count; i++) {
            NodeRef fnRef = functions[i];
            if (fnRef.Type() != ASTNodeType::FUNCTION) continue;
            const FunctionDeclData& existing = ast->GetFunction(fnRef);
            if (existing.name.nameHash != decl.name.nameHash) continue;

            std::vector<OverloadTypeMask> existingMasks;
            BuildParamMasks(existing.parameters, existingMasks);
            u64 existingKey = HashOverloadSignature(existingMasks.data(),
                static_cast<u32>(existingMasks.size()));
            if (existingKey != signatureKey) continue;
            if (existingMasks.size() != paramMasks.size()) continue;

            bool matches = true;
            for (size_t j = 0; j < existingMasks.size(); j++) {
                if (existingMasks[j] != paramMasks[j]) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                return true;
            }
        }
        return false;
    }

    void FillFunctionData(SymbolTableData* table, BWSL_Arena* arena, const FunctionDeclData& decl,
        NodeRef functionRef, const std::vector<OverloadTypeMask>& paramMasks, u64 signatureKey,
        u32 symbolIndex) {
        FunctionData& funcData = table->functions[symbolIndex];
        funcData.returnType = decl.returnType;
        funcData.parameters.Init(arena, decl.parameters.count);
        funcData.paramTypeMasks.Init(arena, static_cast<u32>(paramMasks.size()));
        for (u32 i = 0; i < decl.parameters.count; i++) {
            funcData.parameters.Push(arena, decl.parameters[i]);
            funcData.paramTypeMasks.Push(arena, paramMasks[i]);
        }
        funcData.signatureKey = signatureKey;
        funcData.astNodeIndex = functionRef.packed;
    }

    // Check if any parameter type is a constraint (making this a generic function)
    // Returns true if at least one parameter uses a constraint type
    // Also fills outConstraintInfo with constraint masks for each parameter
    bool CheckForConstrainedParams(
        SymbolTableData* table,
        const ArenaArray<std::pair<ArenaString, ArenaString>>& params,
        std::vector<TypeMask>& outConstraintMasks,
        std::vector<bool>& outIsConstrained
    ) {
        bool hasAnyConstrained = false;
        outConstraintMasks.clear();
        outIsConstrained.clear();
        outConstraintMasks.reserve(params.count);
        outIsConstrained.reserve(params.count);

        for (u32 i = 0; i < params.count; i++) {
            const ArenaString& typeName = params[i].second;
            TypeMask mask = SymbolTable::LookupConstraint(table, typeName);

            outConstraintMasks.push_back(mask);
            outIsConstrained.push_back(mask != 0);

            if (mask != 0) {
                hasAnyConstrained = true;
            }
        }

        return hasAnyConstrained;
    }

    // Fill GenericFunctionData from parsed function declaration
    void FillGenericFunctionData(
        SymbolTableData* table,
        BWSL_Arena* arena,
        const FunctionDeclData& decl,
        NodeRef functionRef,
        const std::vector<TypeMask>& constraintMasks,
        const std::vector<bool>& isConstrained,
        const ArenaString& returnTypeName,
        TypeMask returnConstraint,
        s8 returnMatchesParam
    ) {
        GenericFunctionData gfn;
        gfn.name = decl.name;
        gfn.nameHash = decl.name.nameHash;
        gfn.astNodeIndex = functionRef.packed;
        gfn.isEval = false;

        // Initialize parameters array
        gfn.parameters.Init(arena, decl.parameters.count);
        for (u32 i = 0; i < decl.parameters.count; i++) {
            GenericParamInfo info;
            info.name = decl.parameters[i].first;
            info.typeName = decl.parameters[i].second;
            info.constraintMask = constraintMasks[i];
            info.isConstrained = isConstrained[i];
            gfn.parameters.Push(arena, info);
        }

        // Set return type info
        gfn.returnTypeName = returnTypeName;
        gfn.returnConstraint = returnConstraint;
        gfn.returnMatchesParam = returnMatchesParam;

        SymbolTable::AddGenericFunction(table, gfn);
    }

    // Find which parameter index a return type constraint matches (for -> T style returns)
    s8 FindMatchingParamForReturn(
        const ArenaArray<std::pair<ArenaString, ArenaString>>& params,
        const ArenaString& returnTypeName,
        const std::vector<bool>& isConstrained
    ) {
        for (u32 i = 0; i < params.count; i++) {
            if (isConstrained[i] && params[i].second.nameHash == returnTypeName.nameHash) {
                return static_cast<s8>(i);
            }
        }
        return -1;  // Not matching any parameter
    }
}

// Token management
void Parser::Advance() {
    previous = current;
    current++;

    // Skip error tokens
    while (current < stream->Count() && stream->GetType(current) == TokenType::ERROR_TOKEN) {
        if (!panicMode) {
            ErrorAtCurrent(std::string(stream->GetValue(current)));
        }
        current++;
    }

    // Clamp to EOF
    if (current >= stream->Count()) {
        current = stream->Count() - 1;
    }
}

bool Parser::Match(TokenType type) {
    if (!Check(type)) return false;
    Advance();
    return true;
}

bool Parser::Consume(TokenType type, const char* message) {
    if (stream->GetType(current) == type) {
        Advance();
        return true;
    }

    ErrorAtCurrent(message);
    return false;
}

void Parser::Error(const char* message) {
    ErrorAt(previous, message);
}

void Parser::ErrorAt(TokenRef token, const char* message) {
    if (panicMode || !lexer || !message) return;

    SourceLocation loc = getLocation(stream->GetOffset(token));
    panicMode = true;
    hadError = true;

    ParseError error;
    size_t msgLen = strlen(message);
    char* msgCopy = (char*)arena->Allocate(msgLen + 1, 1);
    if (!msgCopy) return;
    memcpy(msgCopy, message, msgLen);
    msgCopy[msgLen] = '\0';
    error.message = msgCopy;
    error.line = loc.line;
    error.column = loc.column;
    error.token = token;
    errors.Push(arena, error);
}

void Parser::Synchronize() {
    panicMode = false;

    while (stream->GetType(current) != TokenType::EOF_TOKEN) {
        if (stream->GetType(previous) == TokenType::SEMICOLON) return;

        switch (stream->GetType(current)) {
            case TokenType::PASS:
            case TokenType::ATTRIBUTES:
            case TokenType::RESOURCES:
            case TokenType::IF:
            case TokenType::FOR:
            case TokenType::FOREACH:
            case TokenType::LOOP:
            case TokenType::SWITCH:
            case TokenType::CASE:
            case TokenType::RETURN:
                return;
            default:
                ;
        }

        Advance();
    }
}

TokenRef Parser::PeekNext() {
    // With pre-tokenized stream, just look at next index
    TokenRef next = current + 1;
    if (next >= stream->Count()) {
        return stream->Count() - 1;  // Return EOF token
    }
    return next;
}

TokenRef Parser::Peek3() {
    // With pre-tokenized stream, look 2 tokens ahead
    TokenRef ahead = current + 2;
    if (ahead >= stream->Count()) {
        return stream->Count() - 1;  // Return EOF token
    }
    return ahead;
}

bool Parser::IsFunctionDeclStart() {
    // Function declaration pattern: IDENTIFIER :: (
    // Module-qualified pattern: IDENTIFIER :: IDENTIFIER
    if (!Check(TokenType::IDENTIFIER)) return false;
    if (stream->GetType(PeekNext()) != TokenType::DOUBLE_COLON) return false;
    return stream->GetType(Peek3()) == TokenType::LEFT_PAREN;
}

bool Parser::CheckMask(TokenMask mask) {
    size_t type = static_cast<size_t>(stream->GetType(current));
    if (type >= 64) {
        return false;
    }
    return (mask & (1ULL << type)) != 0;
}

bool Parser::MatchMask(TokenMask mask) {
    if (!CheckMask(mask)) return false;
    Advance();
    return true;
}

CoreType Parser::TokenTypeToReturnType(TokenType type) {
    switch (type) {
        case TokenType::INT:    return CoreType::INT;
        case TokenType::INT2:   return CoreType::INT2;
        case TokenType::INT3:   return CoreType::INT3;
        case TokenType::INT4:   return CoreType::INT4;
        case TokenType::UINT:   return CoreType::UINT;
        case TokenType::UINT2:  return CoreType::UINT2;
        case TokenType::UINT3:  return CoreType::UINT3;
        case TokenType::UINT4:  return CoreType::UINT4;
        case TokenType::FLOAT:  return CoreType::FLOAT;
        case TokenType::FLOAT2: return CoreType::FLOAT2;
        case TokenType::FLOAT3: return CoreType::FLOAT3;
        case TokenType::FLOAT4: return CoreType::FLOAT4;
        case TokenType::BOOL:   return CoreType::BOOL;
        case TokenType::MAT2:   return CoreType::MAT2;
        case TokenType::MAT3:   return CoreType::MAT3;
        case TokenType::MAT4:   return CoreType::MAT4;
        case TokenType::VERTEX_FUNCTION:   return CoreType::VERTEX_FUNCTION;
        case TokenType::FRAGMENT_FUNCTION: return CoreType::FRAGMENT_FUNCTION;
        case TokenType::COMPUTE_FUNCTION:  return CoreType::COMPUTE_FUNCTION;
        case TokenType::PASS_BLOCK:        return CoreType::PASS_BLOCK;
        default:
            Error("Unknown return type");
            return CoreType::INVALID;
    }
}

BinaryOpType Parser::TokenTypeToBinaryOp(TokenType type) {
    switch (type) {
        case TokenType::PLUS:          return BinaryOpType::ADD;
        case TokenType::MINUS:         return BinaryOpType::SUBTRACT;
        case TokenType::MULTIPLY:      return BinaryOpType::MULTIPLY;
        case TokenType::DIVIDE:        return BinaryOpType::DIVIDE;
        case TokenType::EQUALS:        return BinaryOpType::EQUALS;
        case TokenType::NOT_EQUALS:    return BinaryOpType::NOT_EQUALS;
        case TokenType::LESS:          return BinaryOpType::LESS;
        case TokenType::GREATER:       return BinaryOpType::GREATER;
        case TokenType::LESS_EQUAL:    return BinaryOpType::LESS_EQUAL;
        case TokenType::GREATER_EQUAL: return BinaryOpType::GREATER_EQUAL;
        case TokenType::AND:           return BinaryOpType::AND;
        case TokenType::OR:            return BinaryOpType::OR;
        case TokenType::MODULO:        return BinaryOpType::MODULO;
        case TokenType::BITWISE_AND:   return BinaryOpType::BITWISE_AND;
        case TokenType::BITWISE_OR:    return BinaryOpType::BITWISE_OR;
        case TokenType::BITWISE_XOR:   return BinaryOpType::BITWISE_XOR;
        case TokenType::LEFT_SHIFT:    return BinaryOpType::LEFT_SHIFT;
        case TokenType::RIGHT_SHIFT:   return BinaryOpType::RIGHT_SHIFT;
        default:
            return BinaryOpType::ADD; // Should never happen
    }
}

//==============================================================================
// Main parsing function
//==============================================================================

NodeRef Parser::ParsePipeline() {
    SourceLocation loc = getLocation(stream->GetOffset(previous));
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
        } else if (Match(TokenType::ATTRIBUTES)) {
            ParseAttributes(pipeline);
        } else if (Match(TokenType::RESOURCES)) {
            if (ast->GetPipeline(pipeline).resources.count > 0) {
                Error("Only one resources block is allowed per pipeline");
                continue;
            }
            ParseResources(pipeline);
        } else if (Match(TokenType::VARIANTS)) {
            if (ast->GetPipeline(pipeline).variantDecls.count > 0 ||
                ast->GetPipeline(pipeline).variantRules.count > 0) {
                Error("Only one variants block is allowed per pipeline");
                continue;
            }
            ParseVariants(pipeline);
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
            // Inline module definition (for testing)
            NodeRef module = ParseModule();
            if (module.IsValid()) {
                // Store module in AST's module pool (already done by MakeModule)
                // The module is accessible via symbol table for imports
            }
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
        } else if (Check(TokenType::IDENTIFIER) && stream->GetType(PeekNext()) == TokenType::DOUBLE_COLON) {
            NodeRef function = ParseFunction();
            if (function.IsValid()) {
                ast->GetPipeline(pipeline).functions.Push(arena, function);
            }
        } else {
            ErrorAtCurrent("Expected 'import', 'attributes', 'resources', 'variants', 'compute_graph', 'constraint', 'enum', 'eval', 'module', 'struct', or 'pass'");
            Advance();
        }
        
        // Reset panic mode after each top-level declaration to allow error recovery
        panicMode = false;
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after pipeline body");

    ResolvePipelineVariants(pipeline);
    // Resolve deferred shader stage expressions before returning
    ResolveShaderStageExpressions(pipeline);

    context->root = pipeline;
    PARSER_TIMING_PRINT();
    return pipeline;
}

//==============================================================================
// Import parsing
//==============================================================================

void Parser::ParseImports(NodeRef pipeline) {
    static constexpr u32 MAX_IMPORTS = 32;
    u32 importedHashes[MAX_IMPORTS];
    u32 importCount = 0;

    while (Check(TokenType::IDENTIFIER)) {
        std::string moduleNameStr(stream->GetValue(current));
        Advance();

        u32 moduleHash = Utils::HashStr(moduleNameStr.c_str());

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
            ast->GetPipeline(pipeline).imports.Push(arena, moduleName);

            if (importCount < MAX_IMPORTS) {
                importedHashes[importCount++] = moduleHash;
            }

            symbolTable.importedModules.Push(arena, moduleIdx);
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
        } else if (Check(TokenType::IDENTIFIER)) {
            Error("Expected ',' between module names");
        } else {
            break;
        }
    }
}

//==============================================================================
// Attribute parsing
//==============================================================================

void Parser::ParseAttributes(NodeRef pipeline) {
    Consume(TokenType::LEFT_BRACE, "Expected '{' after 'attributes'");
    u8 attrIndex = 0;  // Assign indices by declaration order

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        NodeRef attr = ParseAttributeDecl();
        if (attr.IsValid()) {
            ast->GetAttributeDecl(attr).attributeIndex = attrIndex++;
            ast->GetPipeline(pipeline).attributes.Push(arena, attr);
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
    PipelineData& pipelineData = ast->GetPipeline(pipeline);
    if (pipelineData.attributes.count > 0) {
        NodeRef firstAttr = pipelineData.attributes[0];
        const AttributeDeclData& first = ast->GetAttributeDecl(firstAttr);
        if (first.name.nameHash != POSITION_HASH) {
            Error("First attribute must be 'position'");
        }
    }
}

void Parser::ParseResources(NodeRef pipeline) {
    Consume(TokenType::LEFT_BRACE, "Expected '{' after 'resources'");
    u8 resourceIndex = 0;

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        NodeRef resource = ParseResourceDecl();
        if (resource.IsValid()) {
            ast->GetResourceDecl(resource).resourceIndex = resourceIndex;
            ast->GetPipeline(pipeline).resources.Push(arena, resource);

            const ResourceDeclData& decl = ast->GetResourceDecl(resource);
            RegisterParsedResource(&symbolTable,
                                   decl.name.ToString(sourceBase()),
                                   decl.typeName.ToString(sourceBase()),
                                   resourceIndex);
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

void Parser::ParseVariants(NodeRef pipeline) {
    Consume(TokenType::LEFT_BRACE, "Expected '{' after 'variants'");

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (Match(TokenType::RULES)) {
            ParseVariantRules(pipeline);
            continue;
        }

        Consume(TokenType::IDENTIFIER, "Expected variant name");
        ArenaString variantName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));

        bool duplicate = false;
        for (u32 i = 0; i < ast->GetPipeline(pipeline).variantDecls.count; i++) {
            if (ast->GetPipeline(pipeline).variantDecls[i].name.nameHash == variantName.nameHash) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            Error("Variant already declared in this pipeline");
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
            ast->GetPipeline(pipeline).variantDecls.Push(arena, decl);
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after variants block");
}

void Parser::ParseVariantRules(NodeRef pipeline) {
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

        ast->GetPipeline(pipeline).variantRules.Push(arena, rule);
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

NodeRef Parser::ParsePass() {
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    Consume(TokenType::STRING, "Expected pass name in quotes");
    ArenaString passName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));

    NodeRef pass = ASTFactory::MakePass(ast, passName.ToString(sourceBase()), loc.line, loc.column);

    SymbolTable::SetCurrentPass(&symbolTable, passName);
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
            ErrorAtCurrent("Expected 'use', 'vertex', 'fragment', 'compute', or function declaration in pass body");
            Advance();
        }
    }
}

void Parser::ParseUseAttributes(NodeRef pass) {
    Consume(TokenType::ATTRIBUTES, "Expected 'attributes'");
    Consume(TokenType::LEFT_BRACE, "Expected '{'");

    while (!Check(TokenType::RIGHT_BRACE)) {
        ProgressGuard _pg_(this);
        Consume(TokenType::IDENTIFIER, "Expected attribute name");
        ArenaString attrName = ArenaString::Make(sourceBase(), stream->GetOffset(previous), stream->GetLength(previous));
        bool isOptional = Match(TokenType::QUESTION);

        // Look up attribute in current pipeline's attribute list
        u8 idx = 0xFF;
        if (!currentPipeline.IsNull()) {
            const PipelineData& pipeline = ast->GetPipeline(currentPipeline);
            for (u32 i = 0; i < pipeline.attributes.count; i++) {
                NodeRef attrRef = pipeline.attributes[i];
                if (attrRef.Type() == ASTNodeType::ATTRIBUTE_DECL) {
                    const AttributeDeclData& attr = ast->GetAttributeDecl(attrRef);
                    if (attr.name.nameHash == attrName.nameHash) {
                        idx = attr.attributeIndex;
                        break;
                    }
                }
            }
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

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after block");

    return block;
}

NodeRef Parser::ParseStatement() {
    PARSER_TIME_STMT();
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;

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
        if (!MatchMask(TokenMasks::CORE_TYPES)) {
            Error("Expected type after 'const'");
            return NodeRef::Null();
        }

        TokenType varType = static_cast<TokenType>(stream->GetType(previous));
        std::string typeStr(stream->GetValue(previous));

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
            varData.typeInfo = GetTypeInfoFromToken(varType);
            varData.isConst = true;
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
            Consume(TokenType::NUMBER, "Expected array size");
            std::string_view sizeStr = PreviousValue();
            int size = 0;

#ifdef BWSL_WASM
            char* endPtr = nullptr;
            long parsed = std::strtol(sizeStr.data(), &endPtr, 10);
            if (endPtr == sizeStr.data() || parsed <= 0 || parsed > INT_MAX) {
                Error("Invalid or out-of-range array size");
                return NodeRef::Null();
            }
            size = static_cast<int>(parsed);
#else
            size = SafeParseInt(sizeStr);
#endif

            if (size <= 0 || static_cast<u32>(size) > MAX_ARRAY_SIZE) {
                Error("Invalid array size. Max 256k elements");
                return NodeRef::Null();
            }
            Consume(TokenType::RIGHT_BRACKET, "Expected ']' after array size");
            arrayDims.push_back(static_cast<u32>(size));
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
        typeName = moduleName + "::" + localTypeName;
        isModuleQualified = true;
    } else {
        // Simple type name
        Consume(TokenType::IDENTIFIER, "Expected type name");
        typeName = std::string(stream->GetValue(previous));
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
            Consume(TokenType::NUMBER, "Expected array size");
            std::string_view sizeStr = PreviousValue();
            int size = SafeParseInt(sizeStr);
            if (size <= 0 || static_cast<u32>(size) > MAX_ARRAY_SIZE) {
                Error("Invalid array size. Max 256k elements");
                return false;
            }
            Consume(TokenType::RIGHT_BRACKET, "Expected ']' after array size");
            dims.push_back(static_cast<u32>(size));
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

        // Store the unqualified type hash for later use
        unqualifiedTypeHash = localTypeArena.nameHash;

        // Build internal qualified name: m<moduleHash>::s<typeHash>
        std::string internalName = "m" + std::to_string(moduleArena.nameHash) +
                                   "::s" + std::to_string(localTypeArena.nameHash);
        ArenaString internalArena = ArenaString::MakeHashOnly(internalName);
        typeSym = SymbolTable::LookupAny(&symbolTable, internalArena);
    } else {
        typeSym = SymbolTable::LookupAny(&symbolTable, typeArena);
    }

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
        if (typeSym && (typeSym->kind == SymbolKind::ENUM || typeSym->kind == SymbolKind::ENUM_SYMBOL)) {
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

//==============================================================================
// Expression parsing
//==============================================================================

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
                if (varData.isConst) {
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
                    if (varData.isConst) {
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
                u32 moduleHash = moduleName.nameHash;
                u32 moduleIdx = SymbolTable::FindModuleByHash(&symbolTable, moduleHash);
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
                        if (varData.isEval || varData.isConst) {
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
                std::string qualifiedName = moduleName.ToString(sourceBase()) + "::" + memberName;
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
                u32 moduleIdx = SymbolTable::FindModuleByHash(&symbolTable, moduleName.nameHash);

                if (moduleIdx == INVALID_INDEX) {
                    Error("Unknown module '" + moduleName.ToString(sourceBase()) + "'");
                    return NodeRef::Null();
                }

                std::string syntheticQualifiedName;
                syntheticQualifiedName.reserve(2 + 10 + 10);
                syntheticQualifiedName.append("m").append(std::to_string(moduleName.nameHash));
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
        // Also check for VARIABLE kind eval constants
        else if (sym && sym->kind == SymbolKind::VARIABLE) {
            const VariableData& varData = symbolTable.variables[sym->index];
            if (varData.isEval) {
                // Replace with literal value
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
    bool isModuleQualified = false;
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
            isModuleQualified = true;
            qualifiedNameHash = access.qualifiedNameHash;

            call = ASTFactory::MakeFunctionCall(ast, funcName, loc.line, loc.column);
            ast->GetFunctionCall(call).flags |= FunctionCallFlags::IS_MODULE_FUNCTION;
            ast->GetFunctionCall(call).moduleQualifiedHash = qualifiedNameHash;
            ast->GetFunctionCall(call).moduleObject = access.object;
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

bool Parser::ValidateAssignmentTarget(NodeRef target) {
    switch (target.Type()) {
        case ASTNodeType::IDENTIFIER: {
            Symbol* sym = SymbolTable::LookupAny(&symbolTable, ast->GetIdentifier(target).name);
            if (sym && sym->kind == SymbolKind::VARIABLE) {
                const VariableData& varData = symbolTable.variables[sym->index];
                if (varData.isConst) {
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

bool Parser::TryRegisterModuleFromDisk(const std::string& moduleName) {
    if (moduleName.empty()) {
        return false;
    }

    // Check if module already registered in symbol table
    u32 moduleHash = Utils::HashStr(moduleName.c_str());
    u32 existingIdx = SymbolTable::FindModuleByHash(&symbolTable, moduleHash);
    
    if (existingIdx != INVALID_INDEX) {
        // Module already parsed and registered
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

    // Copy source into arena so it lives as long as AST
    // ArenaStrings store sourceOffset values into the lexer's source buffer,
    // so we need the source to persist after this function returns
    char* persistentSource = (char*)arena->Allocate(moduleSource.size() + 1, 1);
    if (!persistentSource) {
        return false;
    }
    memcpy(persistentSource, moduleSource.data(), moduleSource.size());
    persistentSource[moduleSource.size()] = '\0';

    // Save current parser state
    TokenRef savedCurrent = current;
    TokenRef savedPrevious = previous;
    Lexer* savedLexer = lexer;
    TokenStream* savedStream = stream;
    bool savedHasLookahead = hasLookahead;
    TokenRef savedLookahead = lookahead;
    bool savedInModuleScope = symbolTable.inModuleScope;
    u32 savedCurrentModuleIdx = symbolTable.currentModuleIndex;

    // Create new TokenStream and lexer for arena-persistent module source
    TokenStream moduleStream;
    moduleStream.Init(arena, persistentSource, moduleSource.size());
    Lexer moduleLexer(std::string(persistentSource, moduleSource.size()), moduleStream);
    moduleLexer.Tokenize();  // Must tokenize before parsing!
    lexer = &moduleLexer;
    stream = &moduleStream;
    hasLookahead = false;

    // Reset token position and advance to first token
    current = static_cast<TokenRef>(-1);  // Will become 0 after Advance()
    previous = static_cast<TokenRef>(0);
    Advance();

    // Parse the module - look for 'module' keyword
    bool success = false;
    if (Match(TokenType::MODULE)) {
        NodeRef moduleNode = ParseModule();
        success = moduleNode.IsValid();

        // Store source pointer in module data for lifetime tracking
        if (success) {
            u32 moduleIdx = SymbolTable::FindModuleByHash(&symbolTable, Utils::HashStr(moduleName.c_str()));
            if (moduleIdx != INVALID_INDEX && moduleIdx < symbolTable.modules.count) {
                symbolTable.modules[moduleIdx].sourcePtr = persistentSource;
                symbolTable.modules[moduleIdx].sourceLength = static_cast<u32>(moduleSource.size());
            }
        }
    }

    // Restore parser state
    current = savedCurrent;
    previous = savedPrevious;
    lexer = savedLexer;
    stream = savedStream;
    hasLookahead = savedHasLookahead;
    lookahead = savedLookahead;
    symbolTable.inModuleScope = savedInModuleScope;
    symbolTable.currentModuleIndex = savedCurrentModuleIdx;

    return success;
}

//==============================================================================
// Function parsing
//==============================================================================

NodeRef Parser::ParseFunction() {
    // Get function name
    Consume(TokenType::IDENTIFIER, "Expected function name");
    std::string functionName(stream->GetValue(previous));
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;

    Consume(TokenType::DOUBLE_COLON, "Expected '::' after function name");

    // Create function node with temporary return type
    NodeRef function = ASTFactory::MakeFunction(ast, functionName, CoreType::FLOAT, line, col);

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
            customReturnTypeName = returnTypeName + "::" + std::string(stream->GetValue(previous));
        } else {
            customReturnTypeName = returnTypeName;
        }
        TypeInfo resolved = ResolveType(customReturnTypeName);
        if (resolved.coreType != CoreType::INVALID) {
            ast->GetFunction(function).returnType = resolved.coreType;
        } else {
            ast->GetFunction(function).returnType = CoreType::CUSTOM;
        }
    } else if (MatchMask(TokenMasks::CORE_TYPES)) {
        ast->GetFunction(function).returnType = TokenTypeToReturnType(static_cast<TokenType>(stream->GetType(previous)));
    } else if (Match(TokenType::VERTEX_FUNCTION)) {
        ast->GetFunction(function).returnType = CoreType::VERTEX_FUNCTION;
    } else if (Match(TokenType::FRAGMENT_FUNCTION)) {
        ast->GetFunction(function).returnType = CoreType::FRAGMENT_FUNCTION;
    } else if (Match(TokenType::COMPUTE_FUNCTION)) {
        ast->GetFunction(function).returnType = CoreType::COMPUTE_FUNCTION;
    } else if (Match(TokenType::PASS_BLOCK)) {
        ast->GetFunction(function).returnType = CoreType::PASS_BLOCK;
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
            // For pass blocks, parse the entire pass body
            SourceLocation ploc = getLocation(stream->GetOffset(current));
            ast->GetFunction(function).body = ASTFactory::MakePass(ast, "", ploc.line, ploc.column);
            ParsePassBody(ast->GetFunction(function).body);
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
                paramType = moduleName + "::" + typeName;

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
                        paramType = typeIdent + "::" + std::string(stream->GetValue(previous));
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
        if (Check(TokenType::IDENTIFIER) && stream->GetType(PeekNext()) == TokenType::DOUBLE_COLON) {
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

//==============================================================================
// Eval statement parsing
//==============================================================================

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
            Error("C-style for loops are not yet supported inside eval blocks");
            return false;

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

    NodeRef block = ASTFactory::MakeBlock(ast, line, col);
    BlockData& outBlock = ast->GetBlock(block);

    SymbolTable::EnterScope(&symbolTable);
    PushEvalBindingScope();

    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        NodeRef stmt = ParseStatement();
        if (stmt.IsValid() && !ExpandEvalStatement(stmt, outBlock)) {
            while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
                ProgressGuard _pg_(this);
                Advance();
            }
            break;
        }
    }

    Consume(TokenType::RIGHT_BRACE, "Expected '}' after eval block");

    PopEvalBindingScope();
    SymbolTable::ExitScope(&symbolTable);
    return block;
}

NodeRef Parser::ParseEvalStatement() {
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;
    // 'eval' already consumed

    // Check for function: eval funcName :: (params) -> type { ... }
    if (Check(TokenType::IDENTIFIER) && stream->GetType(PeekNext()) == TokenType::DOUBLE_COLON) {
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

    // Compile-time evaluation using SoA evaluator
    EvalStateSoA evalState;
    CompileTimeEvaluatorSoA::Init(&evalState, this, ast, &context->evalCache, ast->arena);

    LiteralValue value;
    if (!CompileTimeEvaluatorSoA::EvaluateNode(&evalState, expr, &value)) {
        if (evalState.hasError) {
            Error(evalState.errorMsg);
        } else {
            Error("Eval expressions must be compile-time constants");
        }
        return NodeRef::Null();
    }

    // Type check the result
    bool typeMatch = false;
    switch (typeInfo.coreType) {
        case CoreType::INT:
        case CoreType::UINT:
            typeMatch = (value.type == LiteralValue::INT);
            break;
        case CoreType::FLOAT:
            typeMatch = (value.type == LiteralValue::FLOAT);
            break;
        case CoreType::BOOL:
            typeMatch = (value.type == LiteralValue::BOOL);
            break;
        default:
            Error("Invalid type for eval constant");
            return NodeRef::Null();
    }

    if (!typeMatch) {
        Error("Type mismatch: eval expression doesn't match declared type");
        return NodeRef::Null();
    }

    Consume(TokenType::SEMICOLON, "Expected ';'");

    // Create VARIABLE_DECL node
    NodeRef varDecl = ASTFactory::MakeVariableDecl(ast,
        ArenaString::MakeHashOnly(varName),
        ArenaString::MakeHashOnly(0u),
        NodeRef::Null(), true, line, col);

    // Add to symbol table
    Symbol* sym = SymbolTable::AddSymbol(&symbolTable,
        ArenaString::MakeHashOnly(varName), SymbolKind::VARIABLE);

    if (sym) {
        VariableData& varData = symbolTable.variables[sym->index];
        varData.typeInfo = typeInfo;
        varData.isConst = true;
        varData.isEval = true;
        varData.evalValue = value;
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

    // Try to evaluate condition at compile time
    EvalStateSoA evalState;
    CompileTimeEvaluatorSoA::Init(&evalState, this, ast, &context->evalCache, arena);
    
    LiteralValue condValue;
    bool canEval = CompileTimeEvaluatorSoA::CanEvaluateNode(&evalState, condition);
    
    // Parse the body (either a block or single statement)
    NodeRef body = NodeRef::Null();
    if (Check(TokenType::LEFT_BRACE)) {
        Consume(TokenType::LEFT_BRACE, "Expected '{'");
        body = ParseBlock();
    } else {
        // Single statement
        body = ParseStatement();
    }

    if (canEval) {
        // Can evaluate at compile time - include/exclude body
        if (!CompileTimeEvaluatorSoA::EvaluateNode(&evalState, condition, &condValue)) {
            // Evaluation failed - treat as runtime if
            NodeRef ifNode = ASTFactory::MakeBlock(ast, line, col);
            ast->GetBlock(ifNode).statements.Push(arena, condition);
            ast->GetBlock(ifNode).statements.Push(arena, body);
            return ifNode;
        }

        // Convert result to bool
        bool conditionTrue = false;
        if (condValue.type == LiteralValue::BOOL) {
            conditionTrue = condValue.boolValue;
        } else if (condValue.type == LiteralValue::INT) {
            conditionTrue = (condValue.intValue != 0);
        } else if (condValue.type == LiteralValue::FLOAT) {
            conditionTrue = (condValue.floatValue != 0.0f);
        }

        // If condition is true, return the body statements
        // If false, return an empty block (no-op)
        if (conditionTrue) {
            return body;
        } else {
            // Return an empty block - the statement is compiled out
            return ASTFactory::MakeBlock(ast, line, col);
        }
    } else {
        // Cannot evaluate at compile time - generate as regular if statement
        // This allows eval if with runtime values like 'self' in enum methods
        // The eval if becomes a normal if that may be optimized by the backend
        
        // Create an if statement node
        // IF_STATEMENT uses BlockData: statements[0]=condition, [1]=then-body, [2]=else-body
        NodeRef ifStmt = ASTFactory::MakeIfStatement(ast, line, col);
        ast->GetBlock(ifStmt).statements.Push(arena, condition);
        ast->GetBlock(ifStmt).statements.Push(arena, body);
        // No else body for eval if
        
        return ifStmt;
    }
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
    auto GetComponentCountLocal = [](CoreType t) -> u8 {
        switch (t) {
            case CoreType::INT: case CoreType::UINT: case CoreType::FLOAT: case CoreType::BOOL: return 1;
            case CoreType::INT2: case CoreType::UINT2: case CoreType::FLOAT2: return 2;
            case CoreType::INT3: case CoreType::UINT3: case CoreType::FLOAT3: return 3;
            case CoreType::INT4: case CoreType::UINT4: case CoreType::FLOAT4: return 4;
            case CoreType::MAT2: return 4;
            case CoreType::MAT3: return 9;
            case CoreType::MAT4: return 16;
            default: return 1;
        }
    };
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
    arrayInfo.componentCount = GetComponentCountLocal(elementType);
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
                case CoreType::INT2:
                case CoreType::INT3:
                case CoreType::INT4:
                    return TypeInfo{CoreType::INT, 1, 0, 0, 0, 0, 0};
                case CoreType::UINT2:
                case CoreType::UINT3:
                case CoreType::UINT4:
                    return TypeInfo{CoreType::UINT, 1, 0, 0, 0, 0, 0};
                case CoreType::MAT2:
                    return TypeInfo{CoreType::FLOAT2, 2, 0, 0, 0, 0, 0};
                case CoreType::MAT3:
                    return TypeInfo{CoreType::FLOAT3, 3, 0, 0, 0, 0, 0};
                case CoreType::MAT4:
                    return TypeInfo{CoreType::FLOAT4, 4, 0, 0, 0, 0, 0};
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
            if (funcCall.flags & FunctionCallFlags::IS_INTRINSIC) {
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

        if (Check(TokenType::IDENTIFIER) && stream->GetType(PeekNext()) == TokenType::DOUBLE_COLON) {
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

    if (MatchMask(TokenMasks::ALL)) {
        ast->GetFunction(method).returnType = TokenTypeToReturnType(static_cast<TokenType>(stream->GetType(previous)));
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
    structData.isIndexable = false;

    // Progress guard against malformed struct bodies that could otherwise
    // trap the parser in an allocation-bomb infinite loop (e.g. a field
    // with an embedded `.` like `float3 a.b = ...;`). Same pattern as the
    // variant loop in ParseEnum.
    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        TokenRef loopStart = current;

        // Parse field type (core or custom/module-qualified)
        TypeInfo fieldType = TYPE_INFO(CoreType::INVALID, 0, false);
        std::string fieldTypeName;
        if (MatchMask(TokenMasks::CORE_TYPES)) {
            fieldType = GetTypeInfoFromToken(PreviousTokenType());
            fieldTypeName = std::string(stream->GetValue(previous));
        } else if (Match(TokenType::IDENTIFIER)) {
            fieldTypeName = std::string(stream->GetValue(previous));
            if (Match(TokenType::DOUBLE_COLON)) {
                Consume(TokenType::IDENTIFIER, "Expected type name after '::'");
                fieldTypeName += "::" + std::string(stream->GetValue(previous));
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

NodeRef Parser::ParseModule() {
    SourceLocation loc = getLocation(stream->GetOffset(previous));
    u32 line = loc.line;
    u32 col = loc.column;

    // Note: MODULE token already consumed by caller
    Consume(TokenType::IDENTIFIER, "Expected module name");

    std::string moduleName(stream->GetValue(previous));
    ArenaString moduleNameArena = ArenaString::MakeHashOnly(moduleName);

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

    Consume(TokenType::LEFT_BRACE, "Expected '{'");

    // Parse module contents (imports, functions, and structs)
    while (!Check(TokenType::RIGHT_BRACE) && !Check(TokenType::EOF_TOKEN)) {
        ProgressGuard _pg_(this);
        if (Match(TokenType::IMPORT)) {
            // Parse module imports
            static constexpr u32 MAX_IMPORTS = 32;
            u32 importedHashes[MAX_IMPORTS];
            u32 importCount = 0;

            while (Check(TokenType::IDENTIFIER)) {
                std::string depModuleName(stream->GetValue(current));
                Advance();

                u32 depHash = Utils::HashStr(depModuleName.c_str());

                // Check for duplicate imports
                bool alreadyImported = false;
                for (u32 i = 0; i < importCount; i++) {
                    if (importedHashes[i] == depHash) {
                        alreadyImported = true;
                        break;
                    }
                }

                if (!alreadyImported) {
                    // Try to find or register the module
                    u32 depModuleIdx = SymbolTable::FindModuleByHash(&symbolTable, depHash);
                    if (depModuleIdx == INVALID_INDEX) {
                        if (TryRegisterModuleFromDisk(depModuleName)) {
                            depModuleIdx = SymbolTable::FindModuleByHash(&symbolTable, depHash);
                        }
                    }

                    if (depModuleIdx != INVALID_INDEX) {
                        ArenaString depName = ArenaString::MakeHashOnly(depModuleName);
                        ast->GetModule(module).imports.Push(arena, depName);

                        if (importCount < MAX_IMPORTS) {
                            importedHashes[importCount++] = depHash;
                        }

                        symbolTable.importedModules.Push(arena, depModuleIdx);
                    } else {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "Unknown module '%s'. Module not found in search paths",
                                 depModuleName.c_str());
                        ErrorAtPrevious(msg);
                    }
                }

                if (Check(TokenType::COMMA)) {
                    Advance();
                } else {
                    break;
                }
            }
        } else if (Check(TokenType::IDENTIFIER) && stream->GetType(PeekNext()) == TokenType::DOUBLE_COLON) {
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

            TokenType varType = static_cast<TokenType>(stream->GetType(previous));
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
        // First check symbol table for imported types
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
    }

    // Type not found
    return TYPE_INFO(CoreType::INVALID, 0, false);
}

//==============================================================================
// Shader Stage Expression Resolution
// Resolves deferred shader expressions (function calls/ternaries) at compile-time
//==============================================================================

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
            if (src.object.Type() == ASTNodeType::IDENTIFIER) {
                const IdentifierData& objectIdent = ast->GetIdentifier(src.object);
                if (objectIdent.identifierKind == SpecialIdentifier::VARIANTS) {
                    LiteralValue variantValue;
                    if (LookupActiveVariantBinding(src.member.nameHash, &variantValue)) {
                        return MakeLiteralNodeFromValue(variantValue, line, col);
                    }
                }
            }
            NodeRef object = CloneNodeWithParams(src.object, subs, subCount);
            NodeRef memberNode = ASTFactory::MakeMemberAccess(ast, object, src.member, line, col);
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
            return ASTFactory::MakeAssignment(ast, target, value, line, col);
        }

        case ASTNodeType::BLOCK: {
            const BlockData& src = ast->GetBlock(node);
            NodeRef newBlock = ASTFactory::MakeBlock(ast, line, col);
            BlockData& dst = ast->GetBlock(newBlock);
            for (u32 i = 0; i < src.statements.count; i++) {
                NodeRef cloned = CloneNodeWithParams(src.statements[i], subs, subCount);
                if (cloned.IsValid()) {
                    dst.statements.Push(arena, cloned);
                }
            }
            return newBlock;
        }

        case ASTNodeType::VARIABLE_DECL: {
            const VariableDeclData& src = ast->GetVariableDecl(node);
            NodeRef initializer = CloneNodeWithParams(src.initializer, subs, subCount);
            return ASTFactory::MakeVariableDecl(ast, src.name, src.type, initializer, src.isConst,
                                                 line, col, src.storageClass, src.arrayDimensions,
                                                 src.arrayLength, src.arrayElementTypeHash);
        }

        case ASTNodeType::FUNCTION_CALL: {
            const FunctionCallData& src = ast->GetFunctionCall(node);
            NodeRef newCall = ASTFactory::MakeFunctionCall(ast, src.name, line, col);
            FunctionCallData& dst = ast->GetFunctionCall(newCall);
            dst.intrinsicIndex = src.intrinsicIndex;
            dst.flags = src.flags;
            dst.moduleIndex = src.moduleIndex;
            dst.moduleQualifiedHash = src.moduleQualifiedHash;
            dst.moduleObject = src.moduleObject.IsValid() ? CloneNodeWithParams(src.moduleObject, subs, subCount) : NodeRef::Null();
            for (u32 i = 0; i < src.arguments.count; i++) {
                NodeRef clonedArg = CloneNodeWithParams(src.arguments[i], subs, subCount);
                dst.arguments.Push(arena, clonedArg);
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

        case ASTNodeType::IF_STATEMENT: {
            // IF_STATEMENT uses BlockData: [condition, thenBranch, elseBranch?]
            const BlockData& src = ast->GetBlock(node);
            if (!src.statements.count) {
                return ASTFactory::MakeBlock(ast, line, col);
            }
            NodeRef condition = CloneNodeWithParams(src.statements[0], subs, subCount);
            if (!activeVariantBindings.empty()) {
                EvalStateSoA evalState;
                CompileTimeEvaluatorSoA::Init(&evalState, this, ast, &context->evalCache, ast->arena);
                LiteralValue condValue;
                if (CompileTimeEvaluatorSoA::CanEvaluateNode(&evalState, condition) &&
                    CompileTimeEvaluatorSoA::EvaluateNode(&evalState, condition, &condValue)) {
                    bool conditionTrue = false;
                    if (ConvertLiteralToBool(condValue, &conditionTrue)) {
                        if (conditionTrue && src.statements.count >= 2) {
                            return CloneNodeWithParams(src.statements[1], subs, subCount);
                        }
                        if (!conditionTrue && src.statements.count >= 3) {
                            return CloneNodeWithParams(src.statements[2], subs, subCount);
                        }
                        return ASTFactory::MakeBlock(ast, line, col);
                    }
                }
            }
            NodeRef newIf = ASTFactory::MakeIfStatement(ast, line, col);
            BlockData& dst = ast->GetBlock(newIf);
            dst.statements.Push(arena, condition);
            for (u32 i = 1; i < src.statements.count; i++) {
                NodeRef cloned = CloneNodeWithParams(src.statements[i], subs, subCount);
                if (cloned.IsValid()) {
                    dst.statements.Push(arena, cloned);
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
                                                       src.isEval, line, col);
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

    const PipelineData& srcPipeline = ast->GetPipeline(pipeline);
    if (srcPipeline.variantDecls.count == 0 && srcPipeline.variantRules.count == 0) {
        (void)selection;
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
}

} // namespace BWSL
