// Part of bwsl_parser_soa.cpp. Include from that file only.
// Core helpers, token navigation, diagnostics, and primitive parser utilities.
#pragma once
#include "bwsl_parser_soa.cpp"

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

// Returns the trailing fixed-array element count of a type spelling such as
// "float4[8]", or 0 when the type has no (single-dimension) array suffix.
static u32 ExtractFixedArraySize(const std::string& typeName) {
    if (typeName.empty() || typeName.back() != ']') return 0;
    size_t left = typeName.rfind('[');
    if (left == std::string::npos || left + 2 > typeName.size()) return 0;
    u32 size = 0;
    for (size_t i = left + 1; i + 1 < typeName.size(); i++) {
        if (!std::isdigit(static_cast<unsigned char>(typeName[i]))) return 0;
        size = size * 10 + static_cast<u32>(typeName[i] - '0');
    }
    return size;
}

static bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

static bool IsIdentifierName(std::string_view value) {
    if (value.empty()) return false;
    unsigned char first = static_cast<unsigned char>(value.front());
    if (!std::isalpha(first) && value.front() != '_') return false;

    for (char ch : value.substr(1)) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '_') return false;
    }
    return true;
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
        case CoreType::INT64:  return TYPE_INFO(CoreType::INT64,  1, false);
        case CoreType::UINT64: return TYPE_INFO(CoreType::UINT64, 1, false);
        case CoreType::DOUBLE: return TYPE_INFO(CoreType::DOUBLE, 1, false);
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
        case CoreType::INT64X2:  return TYPE_INFO(CoreType::INT64X2,  2, true);
        case CoreType::INT64X3:  return TYPE_INFO(CoreType::INT64X3,  3, true);
        case CoreType::INT64X4:  return TYPE_INFO(CoreType::INT64X4,  4, true);
        case CoreType::UINT64X2: return TYPE_INFO(CoreType::UINT64X2, 2, true);
        case CoreType::UINT64X3: return TYPE_INFO(CoreType::UINT64X3, 3, true);
        case CoreType::UINT64X4: return TYPE_INFO(CoreType::UINT64X4, 4, true);
        case CoreType::DOUBLE2:  return TYPE_INFO(CoreType::DOUBLE2,  2, true);
        case CoreType::DOUBLE3:  return TYPE_INFO(CoreType::DOUBLE3,  3, true);
        case CoreType::DOUBLE4:  return TYPE_INFO(CoreType::DOUBLE4,  4, true);
        case CoreType::MAT2:   return TYPE_INFO(CoreType::MAT2,   4, true);
        case CoreType::MAT3:   return TYPE_INFO(CoreType::MAT3,   9, true);
        case CoreType::MAT4:   return TYPE_INFO(CoreType::MAT4,   16, true);
        case CoreType::DMAT2:  return TYPE_INFO(CoreType::DMAT2,  4, true);
        case CoreType::DMAT3:  return TYPE_INFO(CoreType::DMAT3,  9, true);
        case CoreType::DMAT4:  return TYPE_INFO(CoreType::DMAT4,  16, true);
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

static CoreType ResolveResourceCoreType(const TypeInfo& resolvedType,
                                        const std::string& baseType) {
    if (resolvedType.coreType != CoreType::INVALID &&
        resolvedType.coreType != CoreType::VOID) {
        return resolvedType.coreType;
    }
    return SymbolTable::ParseTypeName(baseType);
}

static u32 ResolveResourceStructHash(const TypeInfo& resolvedType,
                                     CoreType coreType,
                                     const std::string& baseType) {
    if (coreType == CoreType::CUSTOM || coreType == CoreType::ENUM) {
        if (resolvedType.customTypeHash != 0) {
            return resolvedType.customTypeHash;
        }
        return Utils::HashStr(baseType.c_str());
    }
    return 0;
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

} // namespace

void Parser::RegisterParsedResource(const std::string& resourceName,
                                    const std::string& typeName,
                                    u32 bindingIndex) {
    ArenaString resourceArena = ArenaString::MakeHashOnly(resourceName);
    Symbol* sym = SymbolTable::AddResource(&symbolTable, resourceArena);
    if (!sym) {
        sym = SymbolTable::LookupResource(&symbolTable, resourceArena);
        if (!sym || sym->kind != SymbolKind::RESOURCE) return;
    }

    ReverseLookup::Register(resourceArena.nameHash, resourceName.c_str());

    ResourceData& data = symbolTable.resources[sym->index];
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
    if (typeName == "image2D") {
        data.type = ResourceBinding::StorageImage;
        data.coreType = static_cast<u8>(CoreType::TEXTURE2D);
        return;
    }

    std::string innerType = ParseInnerResourceType(typeName, "buffer<");
    if (!innerType.empty()) {
        data.type = ResourceBinding::StorageBuffer;
        data.typeName = ArenaString::MakeHashOnly(innerType);
        ReverseLookup::Register(data.typeName.nameHash, innerType.c_str());
        std::string baseType = StripFixedArraySuffixes(innerType);
        TypeInfo resolvedType = ResolveType(baseType);
        CoreType coreType = ResolveResourceCoreType(resolvedType, baseType);
        data.coreType = static_cast<u8>(coreType);
        data.structTypeHash = ResolveResourceStructHash(resolvedType, coreType, baseType);
        return;
    }

    innerType = ParseInnerResourceType(typeName, "cbuffer<");
    if (!innerType.empty()) {
        data.type = ResourceBinding::UniformBuffer;
        data.typeName = ArenaString::MakeHashOnly(innerType);
        ReverseLookup::Register(data.typeName.nameHash, innerType.c_str());
        std::string baseType = StripFixedArraySuffixes(innerType);
        TypeInfo resolvedType = ResolveType(baseType);
        CoreType coreType = ResolveResourceCoreType(resolvedType, baseType);
        data.coreType = static_cast<u8>(coreType);
        data.structTypeHash = ResolveResourceStructHash(resolvedType, coreType, baseType);
        return;
    }

    data.type = ResourceBinding::UniformBuffer;
    std::string baseType = StripFixedArraySuffixes(typeName);
    TypeInfo resolvedType = ResolveType(baseType);
    CoreType coreType = ResolveResourceCoreType(resolvedType, baseType);
    data.coreType = static_cast<u8>(coreType);
    data.arraySize = ExtractFixedArraySize(typeName);
    data.structTypeHash = ResolveResourceStructHash(resolvedType, coreType, baseType);
}

// Global module search paths - can be extended by external tools (e.g., bwslc)
static std::vector<std::string> g_additionalModuleSearchPaths;

void AddModuleSearchPath(const std::string& path) {
    g_additionalModuleSearchPaths.push_back(path);
}

void ClearModuleSearchPaths() {
    g_additionalModuleSearchPaths.clear();
}

// Optional batch-shared module source cache (see bwsl_parser_soa.h).
static ModuleSourceCache* g_moduleSourceCache = nullptr;

void SetModuleSourceCache(ModuleSourceCache* cache) {
    g_moduleSourceCache = cache;
}

// Optional per-job module access recorder (see bwsl_parser_soa.h).
static ModuleAccessRecorder* g_moduleAccessRecorder = nullptr;

void SetModuleAccessRecorder(ModuleAccessRecorder* recorder) {
    g_moduleAccessRecorder = recorder;
}

namespace {
    struct ArenaPath {
        const char* data;
        u32 length;
    };

    ArenaPath MakeArenaPath(BWSL_Arena* arena,
                            const char* prefix,
                            size_t prefixLength,
                            const char* suffix = nullptr) {
        size_t suffixLength = suffix ? std::strlen(suffix) : 0;
        size_t totalLength = prefixLength + suffixLength;
        char* data = static_cast<char*>(arena->Allocate(totalLength + 1, 1));
        if (prefixLength > 0) {
            std::memcpy(data, prefix, prefixLength);
        }
        if (suffixLength > 0) {
            std::memcpy(data + prefixLength, suffix, suffixLength);
        }
        data[totalLength] = '\0';
        return { data, static_cast<u32>(totalLength) };
    }

    void PushArenaPath(BWSL_Arena* arena,
                       ArenaArray<ArenaPath>* paths,
                       const char* prefix,
                       size_t prefixLength,
                       const char* suffix = nullptr) {
        paths->Push(arena, MakeArenaPath(arena, prefix, prefixLength, suffix));
    }

    ArenaPath MakeLowercaseArenaPath(BWSL_Arena* arena, const std::string& value) {
        char* data = static_cast<char*>(arena->Allocate(value.size() + 1, 1));
        for (size_t i = 0; i < value.size(); i++) {
            char c = value[i];
            data[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
        data[value.size()] = '\0';
        return { data, static_cast<u32>(value.size()) };
    }

    ArenaPath MakeModuleCandidatePath(BWSL_Arena* arena,
                                      const ArenaPath& root,
                                      const ArenaPath& moduleName,
                                      const char* suffix) {
        size_t suffixLength = std::strlen(suffix);
        size_t totalLength = static_cast<size_t>(root.length) + 1 +
                             moduleName.length + suffixLength;
        char* data = static_cast<char*>(arena->Allocate(totalLength + 1, 1));
        size_t at = 0;
        if (root.length > 0) {
            std::memcpy(data + at, root.data, root.length);
            at += root.length;
        }
        data[at++] = '/';
        if (moduleName.length > 0) {
            std::memcpy(data + at, moduleName.data, moduleName.length);
            at += moduleName.length;
        }
        if (suffixLength > 0) {
            std::memcpy(data + at, suffix, suffixLength);
            at += suffixLength;
        }
        data[at] = '\0';
        return { data, static_cast<u32>(at) };
    }

    ArenaArray<ArenaPath> BuildModuleSearchRoots(BWSL_Arena* arena) {
        ArenaArray<ArenaPath> searchRoots;
        u32 initialCapacity =
            static_cast<u32>(g_additionalModuleSearchPaths.size() * 4 + 6);
        searchRoots.Init(arena, initialCapacity);

        // Additional paths added by external tools (e.g., input file directory)
        for (const auto& path : g_additionalModuleSearchPaths) {
            PushArenaPath(arena, &searchRoots, path.data(), path.size());
            PushArenaPath(arena, &searchRoots, path.data(), path.size(), "/modules");
            PushArenaPath(arena, &searchRoots, path.data(), path.size(), "/../modules");
            PushArenaPath(arena, &searchRoots, path.data(), path.size(), "/../bwsl/modules");
        }

        // Built-in paths based on source file location (works in engine builds)
        const char* file = __FILE__;
        const char* slash = std::strrchr(file, '/');
        const char* backslash = std::strrchr(file, '\\');
        const char* separator = slash;
        if (backslash && (!separator || backslash > separator)) {
            separator = backslash;
        }
        const char* sourceDir = separator ? file : ".";
        size_t sourceDirLength = separator
            ? static_cast<size_t>(separator - file)
            : 1;

        PushArenaPath(arena, &searchRoots, sourceDir, sourceDirLength, "/modules");
        PushArenaPath(arena, &searchRoots, sourceDir, sourceDirLength, "/../modules");
        PushArenaPath(arena, &searchRoots, "bwsl/modules", std::strlen("bwsl/modules"));
        PushArenaPath(arena, &searchRoots, "modules", std::strlen("modules"));
        PushArenaPath(arena, &searchRoots, "../modules", std::strlen("../modules"));
        PushArenaPath(arena, &searchRoots, "../../modules", std::strlen("../../modules"));

        return searchRoots;
    }

    std::string NormalizeModuleFilePath(const char* path) {
        if (path == nullptr || path[0] == '\0') {
            return {};
        }

        std::error_code ec;
        std::filesystem::path fsPath(path);
        std::filesystem::path normalized = std::filesystem::weakly_canonical(fsPath, ec);
        if (ec) {
            ec.clear();
            normalized = std::filesystem::absolute(fsPath, ec);
        }
        if (ec) {
            return path;
        }
        return normalized.lexically_normal().string();
    }

    std::string NormalizeModuleFilePath(const std::string& path) {
        return NormalizeModuleFilePath(path.c_str());
    }

    std::string ResolveModulePath(BWSL_Arena* arena, const std::string& moduleName) {
        ArenaPath originalName = {
            moduleName.c_str(),
            static_cast<u32>(moduleName.size())
        };
        ArenaPath lowerName = MakeLowercaseArenaPath(arena, moduleName);
        ArenaArray<ArenaPath> searchRoots = BuildModuleSearchRoots(arena);

        for (u32 rootIndex = 0; rootIndex < searchRoots.count; rootIndex++) {
            const ArenaPath& root = searchRoots[rootIndex];
            for (u32 candidateIndex = 0; candidateIndex < 4; candidateIndex++) {
                const ArenaPath& name = (candidateIndex < 2) ? originalName : lowerName;
                const char* suffix = (candidateIndex % 2 == 0)
                    ? "_module.bwsl"
                    : ".bwsl";
                ArenaPath candidate = MakeModuleCandidatePath(arena, root, name, suffix);
                std::ifstream file(candidate.data);
                if (file.good()) {
                    return NormalizeModuleFilePath(candidate.data);
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

    bool HasDuplicateMethodInList(AST* ast, const ArenaArray<NodeRef>& functions,
        const FunctionDeclData& decl, const std::vector<OverloadTypeMask>& paramMasks,
        u64 signatureKey) {
        for (u32 i = 0; i < functions.count; i++) {
            NodeRef fnRef = functions[i];
            if (fnRef.Type() != ASTNodeType::FUNCTION) continue;
            const FunctionDeclData& existing = ast->GetFunction(fnRef);
            if (existing.name.nameHash != decl.name.nameHash) continue;
            if (existing.isConstMethod != decl.isConstMethod) continue;

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
        funcData.name = decl.name;
        funcData.returnType = decl.returnType;
        funcData.returnTypeHash = decl.returnTypeHash;
        funcData.parameters.Init(arena, decl.parameters.count);
        funcData.paramTypeMasks.Init(arena, static_cast<u32>(paramMasks.size()));
        for (u32 i = 0; i < decl.parameters.count; i++) {
            funcData.parameters.Push(arena, decl.parameters[i]);
            funcData.paramTypeMasks.Push(arena, paramMasks[i]);
        }
        funcData.signatureKey = signatureKey;
        funcData.isEval = decl.isEval;
        funcData.isStructMethod = decl.isStructMethod;
        funcData.isConstMethod = decl.isConstMethod;
        funcData.ownerStructTypeHash = decl.ownerStructTypeHash;
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

void Parser::MarkNodeEndAtToken(NodeRef node, TokenRef token) {
    if (!node.IsValid() || token == INVALID_TOKEN) return;
    SourceLocation loc = getLocation(stream->GetOffset(token));
    ast->SetEndPosition(node, loc.line, loc.column + stream->GetLength(token));
}

void Parser::MarkNodeEndAtPreviousToken(NodeRef node) {
    MarkNodeEndAtToken(node, previous);
}

// Attach the doc comment block (`///` lines or `/** ... */`) the lexer
// recorded for `firstToken` to `node`. Walks back over `eval` and decorator
// tokens so a doc block above a modifier still reaches the declaration.
// This is the only place doc text is materialized: the raw source spans
// are cleaned once, directly into the AST arena, so module lexers
// (stack-local in RegisterModuleFromSource) can die afterwards.
void Parser::AttachDocComment(NodeRef node, TokenRef firstToken) {
    if (node.IsNull() || lexer == nullptr) return;

    while (firstToken > 0) {
        TokenType prevType = stream->GetType(firstToken - 1);
        if (prevType != TokenType::EVAL && prevType != TokenType::AT) break;
        firstToken--;
    }

    const DocCommentStream& docs = lexer->GetDocComments();
    s32 firstDoc = docs.FindFirst(firstToken);
    if (firstDoc < 0) return;

    // Cleaned text never exceeds the raw span; size the buffer from the
    // raw lengths plus one joining '\n' per extra block.
    u32 endDoc = (u32)firstDoc;
    u32 rawTotal = 0;
    while (endDoc < docs.Count() && docs.tokenIndices[endDoc] == firstToken) {
        rawTotal += docs.rawEnds[endDoc] - docs.rawStarts[endDoc] + 1;
        endDoc++;
    }

    char* buf = (char*)arena->Allocate(rawTotal + 1, 1);
    if (!buf) return;

    const char* src = sourceBase();
    u32 length = 0;
    for (u32 i = (u32)firstDoc; i < endDoc; i++) {
        u32 cleaned = CleanDocCommentText(src + docs.rawStarts[i],
                                          src + docs.rawEnds[i],
                                          buf + length + (length ? 1 : 0));
        if (cleaned == 0) continue;
        if (length) buf[length++] = '\n';
        length += cleaned;
    }
    buf[length] = '\0';

    if (length) ast->AttachDocs(node, buf, length);
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

    if (context) {
        DiagnosticSpan span;
        span.line = loc.line;
        span.column = loc.column;
        span.token = token;
        span.SetLocation();
        if (token != INVALID_TOKEN) {
            span.offset = stream->GetOffset(token);
            span.length = stream->GetLength(token);
            span.SetOffset();
            if (span.length > 0) {
                span.endLine = loc.line;
                span.endColumn = loc.column + span.length;
                span.SetEndLocation();
            }
        }
        context->Diag().AddRaw(DiagnosticSeverity::Error,
                               diagnosticPhase,
                               message,
                               span,
                               currentSourceName,
                               "",
                               "",
                               DiagnosticMessageId::ParseError);
    }
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
    // Function declaration pattern: name :: (. Names are normally identifiers,
    // but overloaded helpers may intentionally use core type words such as
    // `double`.
    if (!Check(TokenType::IDENTIFIER) && !CheckMask(TokenMasks::CORE_TYPES)) return false;
    if (stream->GetType(PeekNext()) != TokenType::DOUBLE_COLON) return false;
    return stream->GetType(Peek3()) == TokenType::LEFT_PAREN;
}

bool Parser::CheckMask(TokenMask mask) {
    return TokenMasks::matches(mask, stream->GetType(current));
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
        case TokenType::INT64:    return CoreType::INT64;
        case TokenType::UINT64:   return CoreType::UINT64;
        case TokenType::DOUBLE:   return CoreType::DOUBLE;
        case TokenType::INT64X2:  return CoreType::INT64X2;
        case TokenType::INT64X3:  return CoreType::INT64X3;
        case TokenType::INT64X4:  return CoreType::INT64X4;
        case TokenType::UINT64X2: return CoreType::UINT64X2;
        case TokenType::UINT64X3: return CoreType::UINT64X3;
        case TokenType::UINT64X4: return CoreType::UINT64X4;
        case TokenType::DOUBLE2:  return CoreType::DOUBLE2;
        case TokenType::DOUBLE3:  return CoreType::DOUBLE3;
        case TokenType::DOUBLE4:  return CoreType::DOUBLE4;
        case TokenType::BOOL:   return CoreType::BOOL;
        case TokenType::MAT2:   return CoreType::MAT2;
        case TokenType::MAT3:   return CoreType::MAT3;
        case TokenType::MAT4:   return CoreType::MAT4;
        case TokenType::DMAT2:   return CoreType::DMAT2;
        case TokenType::DMAT3:   return CoreType::DMAT3;
        case TokenType::DMAT4:   return CoreType::DMAT4;
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

} // namespace BWSL
