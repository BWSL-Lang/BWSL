#pragma once
#include <string_view>
#include <vector>
#include "bwsl_utils.h"
#include "bwsl_lexer.h"
#include "bwsl_types.h"
#include "bwsl_ast_soa.h"
#include "bwsl_module_cache.h"
#include <cassert>
#include <cstring>

#include "bwsl_defs.h"
#include "bwsl_render_config.h"
namespace BWSL {

enum class NamespaceKind : u8 {
    GLOBAL = 0,
    MODULE = 1,      // Use moduleIndex
    RESOURCES = 2,   // bound resources from Render Config
    ATTRIBUTES = 3,  // vertex pulling attributes
};

enum class SymbolKind {
    VARIABLE,
    FUNCTION,
    ATTRIBUTE,
    RESOURCE,           // Textures, buffers, samplers etc
    SHADER_STAGE,
    PASS,
    RENDER_TARGET,   
    BUFFER_GROUP,       
    UNIFORM_BUFFER,
    EVAL_CONSTANT,
    EVAL_FUNCTION,
    ENUM_SYMBOL,        // Enum variant (e.g., LightType::Point)
    ENUM,               // Enum type itself (e.g., LightType)
    CONSTRAINT,
    GENERIC_FUNCTION,
    CUSTOM_TYPE,        // Structs and custom types      
};

struct Symbol {
    ArenaString name;
    SymbolKind kind;
    u32 moduleIndex;// Only used if namespaceKind == MODULE
    NamespaceKind namespaceKind;
    u32 scopeLevel;
    u32 index;  // Index in type-specific arrays
};

// Type-specific data stored in separate arrays
struct VariableData {
    TypeInfo typeInfo;
    bool isConst;
    bool isEval;
    LiteralValue evalValue;
    NodeRef constExpr;
};
using OverloadTypeMask = u64;
static constexpr OverloadTypeMask OVERLOAD_CUSTOM_MASK = (1ULL << 63);

inline OverloadTypeMask MakeOverloadMask(CoreType type, u32 customHash = 0) {
    if (type == CoreType::CUSTOM) {
        return OVERLOAD_CUSTOM_MASK | static_cast<OverloadTypeMask>(customHash);
    }
    if (type == CoreType::INVALID || type == CoreType::VOID) {
        return 0;
    }
    return 1ULL << static_cast<u32>(type);
}

inline OverloadTypeMask MakeOverloadMask(const TypeInfo& info) {
    return MakeOverloadMask(info.coreType, info.customTypeHash);
}

inline OverloadTypeMask MakeOverloadMaskFromTypeHash(u32 typeHash) {
    for (u32 i = 0; i < TypeHashes::HASH_TABLE_SIZE; i++) {
        if (TypeHashes::HASH_TABLE[i].hash == typeHash) {
            return MakeOverloadMask(TypeHashes::HASH_TABLE[i].info);
        }
    }
    return OVERLOAD_CUSTOM_MASK | static_cast<OverloadTypeMask>(typeHash);
}

inline bool OverloadMaskMatches(OverloadTypeMask paramMask, OverloadTypeMask argMask) {
    if ((paramMask & OVERLOAD_CUSTOM_MASK) || (argMask & OVERLOAD_CUSTOM_MASK)) {
        return paramMask == argMask;
    }
    return (paramMask & argMask) != 0;
}

inline u64 HashOverloadSignature(const OverloadTypeMask* masks, u32 count) {
    u64 hash = 1469598103934665603ULL;
    auto mix = [&hash](u64 value) {
        for (u32 i = 0; i < 8; i++) {
            hash ^= static_cast<u8>(value & 0xFF);
            hash *= 1099511628211ULL;
            value >>= 8;
        }
    };
    mix(static_cast<u64>(count));
    for (u32 i = 0; i < count; i++) {
        mix(masks[i]);
    }
    return hash;
}
struct FunctionData {
    CoreType returnType;
    ArenaArray<std::pair<ArenaString, ArenaString>> parameters;
    ArenaArray<OverloadTypeMask> paramTypeMasks;
    u64 signatureKey;
    bool isEval;           
    u32 astNodeIndex;      // For eval functions, store AST location
};

// Compression format for vertex attributes
enum class CompressionFormat : u8 {
    NONE = 0,
    PACKED_10_10_10,     // 3x10-bit components packed in uint32
    OCTAHEDRAL16,        // 2x8-bit octahedral encoding in uint16/uint32
    UNORM16_16,          // 2x16-bit unorm in uint32
    UNORM8888,           // 4x8-bit unorm in uint32
    UINT8888,            // 4x8-bit uint in uint32
};

struct AttributeData {
    TypeInfo typeInfo;          // Semantic type (e.g., float3 for position)
    CompressionFormat compression;  // Compression format
    u8 attributeIndex;
    bool isOptional;
};

// Get the raw storage type for a compression format
// All compressed formats pack data into uint (32-bit)
inline CoreType GetRawTypeForCompression(CompressionFormat fmt) {
    switch (fmt) {
        case CompressionFormat::PACKED_10_10_10:
        case CompressionFormat::OCTAHEDRAL16:
        case CompressionFormat::UNORM16_16:
        case CompressionFormat::UNORM8888:
        case CompressionFormat::UINT8888:
            return CoreType::UINT;
        case CompressionFormat::NONE:
        default:
            return CoreType::INVALID;  // No compression, use semantic type
    }
}

// Parse compression format string to enum
inline CompressionFormat ParseCompressionFormat(u32 nameHash) {
    // Pre-computed hashes for compression format names
    static const u32 hash_10_10_10 = Utils::HashStr("10_10_10");
    static const u32 hash_octahedral16 = Utils::HashStr("octahedral16");
    static const u32 hash_unorm16_16 = Utils::HashStr("unorm16_16");
    static const u32 hash_unorm8888 = Utils::HashStr("unorm8888");
    static const u32 hash_uint8888 = Utils::HashStr("uint8888");

    if (nameHash == hash_10_10_10) return CompressionFormat::PACKED_10_10_10;
    if (nameHash == hash_octahedral16) return CompressionFormat::OCTAHEDRAL16;
    if (nameHash == hash_unorm16_16) return CompressionFormat::UNORM16_16;
    if (nameHash == hash_unorm8888) return CompressionFormat::UNORM8888;
    if (nameHash == hash_uint8888) return CompressionFormat::UINT8888;

    return CompressionFormat::NONE;
}

struct ResourceData {
    ResourceBinding::Type type = ResourceBinding::Buffer;
    u32 bindingIndex = 0;
    u8 stageFlags = 0;
    u8 coreType = static_cast<u8>(CoreType::FLOAT4);  // CoreType for uniform buffers (from render config)
    u32 structTypeHash = 0;  // For struct types (storage buffers), hash of the fully qualified type name
    ArenaString renderTargetName;
    ArenaString typeName;  // Raw type name string from render config (e.g., "Globals::LightSourcesSoA")
};

struct PassContextData {
    ArenaString passName;
    PassType passType;
    ArenaArray<ResourceBinding> availableBindings;
    ArenaArray<BufferGroupLayout::GroupType> activeBufferGroups;
    ArenaString pipelineName;
};

struct StructData {
    ArenaString name;
    struct Field {
        ArenaString name;
        TypeInfo type;
        u32 arraySize;  // 0 = not an array, >0 = fixed-size array
    };
    ArenaArray<Field> fields;
    
    bool isIndexable;
};
struct ModuleData {
    ArenaString name;
    ArenaArray<u32> functionIndices;  
    ArenaArray<u32> structIndices;
    ArenaArray<u32> enumIndices;
    ArenaString sourcePath;
    
    // Arena-persistent source for module (ArenaStrings store offsets into this)
    const char* sourcePtr = nullptr;
    u32 sourceLength = 0;
    
    ArenaString translatedCode;           // Cached Metal/HLSL translation
    std::time_t lastModified;             // File modification time
    std::time_t lastTranslated;           // When we translated it
    ArenaArray<ArenaString> dependencies; // Other modules this depends on
    
};

struct EnumData {

    ArenaString name;
    CoreType underlyingType;  // INVALID for sum types, INT/UINT for flag enums
    
    struct Variant {
        ArenaString name;
        ArenaArray<CoreType> associatedTypes;  // Empty if no data
        u32 value;  // Computed or explicit
    };
    ArenaArray<Variant> variants;
    
    // Method indices (for eval and regular methods)
    ArenaArray<u32> methodIndices;
    
    // Packed flags: isSumType (bit 0) | isFlagEnum (bit 1)
    u8 flags;
    
    static constexpr u8 IS_SUM_TYPE = 0x01;
    static constexpr u8 IS_FLAG_ENUM = 0x02;
};

// Generics
struct ConstraintData {
    ArenaString name;
    TypeMask allowedTypes;
};

    struct TypeParameter {
        ArenaString name;
        u32 typeHash;
        u16 typeLength; // but actually mostly padding to align the struct, it is almost never needed.
    };


struct GenericFunctionData {
    ArenaString name;
    CoreType returnType;
    ArenaArray<TypeParameter> parameters;
    
    
    static constexpr u32 MAX_TYPE_PARAMS = 8;
    alignas(16) TypeMask typeConstraintMasks[MAX_TYPE_PARAMS];
    u8 typeParamCount;
    
    // Monomorphization cache
    static constexpr u32 MAX_SPECIALIZATIONS = 32;
    alignas(64) u32 specializationHashes[MAX_SPECIALIZATIONS];
    alignas(64) u32 specializationASTIndices[MAX_SPECIALIZATIONS];
    
    // Packed metadata: isEval (1 bit) | specializationCount (5 bits) | astNodeIndex (26 bits)
    // Layout: [isEval:1][specializationCount:5][astNodeIndex:26]
    u32 packedMetadata;
    
    // Bit positions and masks
    static constexpr u32 AST_INDEX_BITS = 26;
    static constexpr u32 SPECIALIZATION_BITS = 5;
    static constexpr u32 EVAL_BIT = 31;
    
    static constexpr u32 AST_INDEX_MASK = (1u << AST_INDEX_BITS) - 1;
    static constexpr u32 SPECIALIZATION_MASK = ((1u << SPECIALIZATION_BITS) - 1) << AST_INDEX_BITS;
    static constexpr u32 EVAL_MASK = 1u << EVAL_BIT;
    
    static constexpr u32 MAX_AST_INDEX = (1u << AST_INDEX_BITS) - 1;  // 67,108,863
    static constexpr u32 MAX_SPECIALIZATION_COUNT = (1u << SPECIALIZATION_BITS) - 1;  // 31
    
    inline u32 GetASTIndex() const {
        return packedMetadata & AST_INDEX_MASK;
    }
    
    inline void SetASTIndex(u32 index) {
        assert(index <= MAX_AST_INDEX && "AST index out of range");
        packedMetadata = (packedMetadata & ~AST_INDEX_MASK) | (index & AST_INDEX_MASK);
    }
    
    inline u32 GetSpecializationCount() const {
        return (packedMetadata & SPECIALIZATION_MASK) >> AST_INDEX_BITS;
    }
    
    inline void SetSpecializationCount(u32 count) {
        assert(count <= MAX_SPECIALIZATION_COUNT && "Specialization count out of range");
        packedMetadata = (packedMetadata & ~SPECIALIZATION_MASK) | 
                        ((count << AST_INDEX_BITS) & SPECIALIZATION_MASK);
    }
    
    inline void IncrementSpecializationCount() {
        u32 count = GetSpecializationCount();
        assert(count < MAX_SPECIALIZATION_COUNT && "Specialization count overflow");
        SetSpecializationCount(count + 1);
    }
    
    inline bool IsEval() const {
        return (packedMetadata & EVAL_MASK) != 0;
    }
    
    inline void SetEval(bool eval) {
        if (eval) {
            packedMetadata |= EVAL_MASK;
        } else {
            packedMetadata &= ~EVAL_MASK;
        }
    }
    
    inline void SetMetadata(u32 astIndex, u32 specCount, bool eval) {
        assert(astIndex <= MAX_AST_INDEX);
        assert(specCount <= MAX_SPECIALIZATION_COUNT);
        packedMetadata = (astIndex & AST_INDEX_MASK) |
                        ((specCount << AST_INDEX_BITS) & SPECIALIZATION_MASK) |
                        (eval ? EVAL_MASK : 0);
    }
};
struct SymbolTableData {

    BWSL_Arena* arena;

    ArenaArray<Symbol> symbols;

    //------------ Type-specific data arrays (parallel to symbols via index) ---------------//
    
    ArenaArray<VariableData>                            variables;
    ArenaArray<FunctionData>                            functions;
    ArenaArray<GenericFunctionData>                     genericFunctions;
    ArenaArray<AttributeData>                           attributes;
    ArenaArray<ResourceData>                            resources;
    ArenaArray<StructData>                              structs;
    ArenaArray<ModuleData>                              modules;
    ArenaArray<EnumData>                                enums;
    ArenaArray<ConstraintData>                          constraints;
    
    //---------------------------- Scope management ----------------------------------//
    
    u32 currentScope;
    ArenaArray<u32> scopeStartIndices;  // Where each scope starts in symbols array
    
    //---------------------------- Render context ----------------------------------//
    
    const RenderConfig* renderConfig;
    PassContextData currentPass;
    
    //------------ Resource lookup tables (built from RenderConfig) ---------------//
    
    ArenaArray<std::pair<ArenaString, ResourceData>> renderTargets;
    ArenaArray<std::pair<ArenaString, BufferGroupLayout::GroupType>> bufferGroups;

    //---------------------------- Module data -----------------------------------//
    
    static constexpr u32 MODULE_HASH_TABLE_SIZE = 64; // Power of 2
    struct ModuleHashEntry {
        u32 nameHash;
        u32 moduleIndex;
        u32 nextIndex;  // For collision chaining
    };
    
    ModuleHashEntry moduleHashTable[MODULE_HASH_TABLE_SIZE];
    u32 moduleHashChain[ModuleCache::MAX_MODULES * 2];  // Collision overflow
    u32 moduleHashChainUsed;
    
    // module data is indexed by hash
    ArenaArray<u32> moduleNameHashes;  // Parallel array for fast validation
    ArenaArray<u32> importedModules;
    // state tracking
    u32 currentModuleIndex;
    bool inModuleScope;

    // --------------------------- Eval -------------------------------------//
    
    ArenaArray<LiteralValue> evalConstants;
    ArenaArray<u32> evalFunctionIndices;  // AST node indices for eval functions
   
};


namespace SymbolTable {
    // Helper constructors for ArenaString from non-source strings (hash-only views)
    inline ArenaString MakeFromLiteral(const char* literal) {
        return ArenaString::MakeHashOnly(literal);
    }

    inline void InitModules(SymbolTableData* table, BWSL_Arena* arena) {
        table->modules.Init(arena, 16);
        table->moduleNameHashes.Init(arena, 16);
        
        // Initialize hash table to empty
        for (u32 i = 0; i < SymbolTableData::MODULE_HASH_TABLE_SIZE; i++) {
            table->moduleHashTable[i].moduleIndex = 0xFFFFFFFF;
            table->moduleHashTable[i].nextIndex = 0xFFFFFFFF;
        }
        table->moduleHashChainUsed = 0;
        
        table->currentModuleIndex = INVALID_INDEX;
        table->inModuleScope = false;
    }
    inline void Init(SymbolTableData* table, BWSL_Arena* arena) {
        table->arena = arena;
        table->symbols.Init(arena, 64);
        table->variables.Init(arena, 32);
        table->functions.Init(arena, 32);
        table->attributes.Init(arena, 16);
        table->resources.Init(arena, 32);
        table->structs.Init(arena, 16);
        table->enums.Init(arena, 32);
        table->constraints.Init(arena, 16);
        table->scopeStartIndices.Init(arena, 16);
        table->renderTargets.Init(arena, 32);
        table->bufferGroups.Init(arena, 16);
        table->evalConstants.Init(arena, 16);      // Initialize eval constants array
        table->evalFunctionIndices.Init(arena, 8); // Initialize eval function indices
        table->currentScope = 0;
        table->renderConfig = nullptr;
        table->importedModules.Init(arena, 8);
        table->currentModuleIndex = INVALID_INDEX;
        table->inModuleScope = false;
        InitModules(table, arena);

        // Global scope starts at index 0
        table->scopeStartIndices.Push(arena, 0);
    }

    
    // Lookup by pre-computed hash @redundant??
    inline Symbol* LookupByHash(SymbolTableData* table, u32 hash) {
        // Search from current scope outward
        for (int i = table->symbols.count - 1; i >= 0; i--) {
            if (table->symbols[i].name.nameHash == hash) {
                return &table->symbols[i];
            }
        }
        return nullptr;
    }

    inline Symbol* LookupResource(SymbolTableData* table, const ArenaString& name) {
        for (int i = table->symbols.count - 1; i >= 0; i--) {
            if (table->symbols[i].namespaceKind == NamespaceKind::RESOURCES &&
                table->symbols[i].name.nameHash == name.nameHash) {
                return &table->symbols[i];
            }
        }
        return nullptr;
    }

    inline u32 FindModuleByHash(SymbolTableData* table, u32 hash) {
        u32 slot = hash & (SymbolTableData::MODULE_HASH_TABLE_SIZE - 1);
        
        SymbolTableData::ModuleHashEntry* entry = &table->moduleHashTable[slot];
        
        // Check main slot
        if (entry->nameHash == hash && entry->moduleIndex != 0xFFFFFFFF) {
            return entry->moduleIndex;
        }
        
        // Check collision chain
        u32 chainIndex = entry->nextIndex;
        while (chainIndex != 0xFFFFFFFF && chainIndex < table->moduleHashChainUsed) {
            if (table->moduleHashChain[chainIndex] < table->modules.count) {
                u32 moduleIdx = table->moduleHashChain[chainIndex];
                if (table->moduleNameHashes[moduleIdx] == hash) {
                    return moduleIdx;
                }
            }
            // Move to next in chain
            chainIndex++;  
        }
        
        return INVALID_INDEX;
    }
    
    // We keep the string version for convenience but have it use the hash version
    inline u32 FindModule(SymbolTableData* table, const ArenaString& moduleName) {
        return FindModuleByHash(table,moduleName.nameHash);
    }

    inline bool IsModuleImported(SymbolTableData* table, u32 moduleIndex) {
        for (u32 i = 0; i < table->importedModules.count; i++) {
            if (table->importedModules[i] == moduleIndex) {
                return true;
            }
        }
        return false;
    }

    inline u8 ShaderStageToBit(ShaderStage stage) {
        return 1 << static_cast<u8>(stage);
    }

    inline bool HasShaderStage(u8 stageFlags, ShaderStage stage) {
        return (stageFlags & ShaderStageToBit(stage)) != 0;
    }

    // Parse type name string to CoreType
    inline CoreType ParseTypeName(const std::string& typeName) {
        // Strip array suffix if present (e.g., "mat4[]" -> "mat4")
        std::string baseType = typeName;
        if (baseType.size() >= 2 && baseType.substr(baseType.size() - 2) == "[]") {
            baseType = baseType.substr(0, baseType.size() - 2);
        }

        if (baseType == "int") return CoreType::INT;
        if (baseType == "uint") return CoreType::UINT;
        if (baseType == "float") return CoreType::FLOAT;
        if (baseType == "float2") return CoreType::FLOAT2;
        if (baseType == "float3") return CoreType::FLOAT3;
        if (baseType == "float4") return CoreType::FLOAT4;
        if (baseType == "int2") return CoreType::INT2;
        if (baseType == "int3") return CoreType::INT3;
        if (baseType == "int4") return CoreType::INT4;
        if (baseType == "uint2") return CoreType::UINT2;
        if (baseType == "uint3") return CoreType::UINT3;
        if (baseType == "uint4") return CoreType::UINT4;
        if (baseType == "mat2" || baseType == "float2x2") return CoreType::MAT2;
        if (baseType == "mat3" || baseType == "float3x3") return CoreType::MAT3;
        if (baseType == "mat4" || baseType == "float4x4") return CoreType::MAT4;
        if (baseType == "bool") return CoreType::BOOL;
        return CoreType::CUSTOM;  // Unknown type - treat as custom struct
    }

    inline void InitFromRenderConfig(SymbolTableData* table, const RenderConfig& config) {
        table->renderConfig = &config;

        // Pre-populate render targets
        for (const auto& rt : config.renderTargets) {
            ResourceData data;
            data.type = ResourceBinding::Texture;
            data.bindingIndex = 0;  // Will be set per-pass
            data.stageFlags = 0;
            data.renderTargetName = ArenaString::MakeHashOnly(rt.name);
            table->renderTargets.Push(table->arena,
                std::make_pair(ArenaString::MakeHashOnly(rt.name), data));
        }

        // Pre-populate buffer groups
        for (const auto& bg : config.bufferGroups) {
            table->bufferGroups.Push(table->arena,
                std::make_pair(ArenaString::MakeHashOnly(bg.name), bg.type));
        }

        // Register uniform buffers as resources
        // Use just the short name - RESOURCES namespace disambiguates
        for (const auto& ub : config.uniformBuffers) {
            ArenaString name = ArenaString::MakeHashOnly(ub.name);

            // Manually add resource symbol (AddSymbol is defined later)
            Symbol sym;
            sym.name = name;
            sym.moduleIndex = INVALID_INDEX;
            sym.namespaceKind = NamespaceKind::RESOURCES;
            sym.kind = SymbolKind::RESOURCE;
            sym.scopeLevel = 0;
            sym.index = table->resources.count;

            ResourceData resData;
            resData.type = ResourceBinding::Buffer;
            resData.bindingIndex = ub.bindingIndex;
            // Parser stores stage flags as bitmask directly (1=vertex, 2=fragment, 3=both)
            resData.stageFlags = static_cast<u8>(ub.stages);
            // Store the explicit type from render config
            resData.coreType = static_cast<u8>(ParseTypeName(ub.typeName));
            table->resources.Push(table->arena, resData);
            table->symbols.Push(table->arena, sym);
        }

        // Register textures as resources
        // Use just the short name - RESOURCES namespace disambiguates
        for (const auto& tex : config.textures) {
            ArenaString name = ArenaString::MakeHashOnly(tex.name);

            Symbol sym;
            sym.name = name;
            sym.moduleIndex = INVALID_INDEX;
            sym.namespaceKind = NamespaceKind::RESOURCES;
            sym.kind = SymbolKind::RESOURCE;
            sym.scopeLevel = 0;
            sym.index = table->resources.count;

            ResourceData resData;
            resData.type = ResourceBinding::Texture;
            resData.bindingIndex = tex.bindingIndex;
            resData.stageFlags = static_cast<u8>(tex.stages);
            table->resources.Push(table->arena, resData);
            table->symbols.Push(table->arena, sym);
        }

        // Register samplers as resources
        // Use just the short name - RESOURCES namespace disambiguates
        for (const auto& samp : config.samplers) {
            ArenaString name = ArenaString::MakeHashOnly(samp.name);

            Symbol sym;
            sym.name = name;
            sym.moduleIndex = INVALID_INDEX;
            sym.namespaceKind = NamespaceKind::RESOURCES;
            sym.kind = SymbolKind::RESOURCE;
            sym.scopeLevel = 0;
            sym.index = table->resources.count;

            ResourceData resData;
            resData.type = ResourceBinding::Sampler;
            resData.bindingIndex = samp.bindingIndex;
            resData.stageFlags = static_cast<u8>(samp.stages);
            table->resources.Push(table->arena, resData);
            table->symbols.Push(table->arena, sym);
        }

        // Register storage buffers as resources
        // Use just the short name - RESOURCES namespace disambiguates
        for (const auto& buf : config.storageBuffers) {
            ArenaString name = ArenaString::MakeHashOnly(buf.name);

            Symbol sym;
            sym.name = name;
            sym.moduleIndex = INVALID_INDEX;
            sym.namespaceKind = NamespaceKind::RESOURCES;
            sym.kind = SymbolKind::RESOURCE;
            sym.scopeLevel = 0;
            sym.index = table->resources.count;

            ResourceData resData;
            resData.type = ResourceBinding::Buffer;
            resData.bindingIndex = buf.bindingIndex;
            resData.stageFlags = static_cast<u8>(buf.stages);
            // Store the type name for struct lookup during IR lowering
            resData.typeName = ArenaString::MakeHashOnly(buf.typeName);

            // Parse the type - primitive types (mat4, float4, etc.) vs custom structs
            CoreType coreType = ParseTypeName(buf.typeName);
            resData.coreType = static_cast<u8>(coreType);

            // Only set structTypeHash for custom struct types, not primitives
            // Module structs are registered with their qualified name (e.g., "Globals::LightSourcesSoA")
            if (coreType == CoreType::CUSTOM) {
                resData.structTypeHash = Utils::HashStr(buf.typeName.c_str());
            } else {
                resData.structTypeHash = 0;  // Primitive type - no struct lookup needed
            }

            table->resources.Push(table->arena, resData);
            table->symbols.Push(table->arena, sym);
        }
    }
    
    inline void EnterScope(SymbolTableData* table) {
        table->currentScope++;
        table->scopeStartIndices.Push(table->arena, table->symbols.count);
    }
    
    inline void ExitScope(SymbolTableData* table) {
        if (table->currentScope == 0) return;
        
        // Remove symbols from current scope
        u32 scopeStart = table->scopeStartIndices[table->currentScope];
        table->symbols.count = scopeStart;
        table->currentScope--;
    }
    
    inline Symbol* AddSymbol(SymbolTableData* table, const ArenaString& name, 
        SymbolKind kind, NamespaceKind ns = NamespaceKind::GLOBAL, u32 moduleIndex = INVALID_INDEX) {
            // Check for duplicates in same namespace/module
            u32 scopeStart = table->scopeStartIndices[table->currentScope];
            for (u32 i = scopeStart; i < table->symbols.count; i++) {
            Symbol& existing = table->symbols[i];
            if (existing.name.nameHash == name.nameHash &&
            existing.namespaceKind == ns &&
            existing.moduleIndex == moduleIndex) {
            if (existing.kind == SymbolKind::FUNCTION && kind == SymbolKind::FUNCTION) {
                continue;
            }
            return nullptr;  // Already exists
            }
            }

            Symbol sym;
            sym.name = name;
            sym.moduleIndex = moduleIndex;
            sym.namespaceKind = ns;
            sym.kind = kind;
            sym.scopeLevel = table->currentScope;
            sym.index = 0;

            // Assign type-specific index
            switch (kind) {

            case SymbolKind::VARIABLE:
            sym.index = table->variables.count;
            table->variables.Push(table->arena, VariableData{});
            break;

            case SymbolKind::FUNCTION:
            sym.index = table->functions.count;
            table->functions.Push(table->arena, FunctionData{});
            break;

            case SymbolKind::ATTRIBUTE:
            sym.index = table->attributes.count;
            table->attributes.Push(table->arena, AttributeData{});
            break;

            case SymbolKind::RESOURCE:
            sym.index = table->resources.count;
            table->resources.Push(table->arena, ResourceData{});
            break;

            case SymbolKind::ENUM_SYMBOL:
            case SymbolKind::ENUM:
            sym.index = table->enums.count;
            table->enums.Push(table->arena, EnumData{});
            break;

            case SymbolKind::CUSTOM_TYPE:
            // Allocate index in structs array - caller will fill in the data
            sym.index = table->structs.count;
            table->structs.Push(table->arena, StructData{});
            break;

            case SymbolKind::EVAL_CONSTANT:
            sym.index = table->evalConstants.count;
            table->evalConstants.Push(table->arena, LiteralValue{});
            break;

            case SymbolKind::PASS:
            case SymbolKind::RENDER_TARGET:
            case SymbolKind::BUFFER_GROUP:
            case SymbolKind::UNIFORM_BUFFER:
            case SymbolKind::SHADER_STAGE:
            case SymbolKind::EVAL_FUNCTION:
            case SymbolKind::GENERIC_FUNCTION:
            case SymbolKind::CONSTRAINT:
            break;
            }

            table->symbols.Push(table->arena, sym);
            return &table->symbols[table->symbols.count - 1];
    }
    
    inline Symbol* Lookup(SymbolTableData* table, const ArenaString& name) {
        return LookupByHash(table, name.nameHash);
    }

    inline Symbol* Lookup(SymbolTableData* table, const ArenaString& name,
        NamespaceKind ns = NamespaceKind::GLOBAL, u32 moduleIndex = INVALID_INDEX) {
            // Search from current scope outward
            for (int i = table->symbols.count - 1; i >= 0; i--) {
                Symbol& sym = table->symbols[i];
                bool isMatch = sym.name == name && sym.namespaceKind == ns && (ns != NamespaceKind::MODULE || sym.moduleIndex == moduleIndex);
               
                if (isMatch) return &sym;
            }
            return nullptr;

    }

    // Generic lookup that checks all imported modules
    inline Symbol* LookupAny(SymbolTableData* table, const ArenaString& name) {
        NamespaceKind currentNs = table->inModuleScope ?  NamespaceKind::MODULE : NamespaceKind::GLOBAL;
            u32 currentModule   = table->inModuleScope ? table->currentModuleIndex : INVALID_INDEX;

            Symbol* sym = Lookup(table, name, currentNs, currentModule);
            if (sym) return sym;

            if (currentNs != NamespaceKind::MODULE) {
                for (u32 i = 0; i < table->importedModules.count; i++) {
                    u32 modIdx = table->importedModules[i];
                    sym = Lookup(table, name, NamespaceKind::MODULE, modIdx);
                    if (sym) return sym;
                }
            }

            return nullptr;

    }

    inline Symbol* LookupFunctionOverloadInNamespace(SymbolTableData* table, const ArenaString& name,
        const OverloadTypeMask* argMasks, u32 argCount, NamespaceKind ns, u32 moduleIndex) {
            for (int i = table->symbols.count - 1; i >= 0; i--) {
                Symbol& sym = table->symbols[i];
                if (sym.kind != SymbolKind::FUNCTION) continue;
                if (sym.name.nameHash != name.nameHash) continue;
                if (sym.namespaceKind != ns) continue;
                if (ns == NamespaceKind::MODULE && sym.moduleIndex != moduleIndex) continue;

                const FunctionData& funcData = table->functions[sym.index];
                if (funcData.paramTypeMasks.count != argCount) continue;
                bool matches = true;
                for (u32 j = 0; j < argCount; j++) {
                    if (!OverloadMaskMatches(funcData.paramTypeMasks[j], argMasks[j])) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    return &sym;
                }
            }
            return nullptr;
    }

    inline Symbol* LookupFunctionOverload(SymbolTableData* table, const ArenaString& name,
        const OverloadTypeMask* argMasks, u32 argCount) {
            NamespaceKind currentNs = table->inModuleScope ?  NamespaceKind::MODULE : NamespaceKind::GLOBAL;
            u32 currentModule   = table->inModuleScope ? table->currentModuleIndex : INVALID_INDEX;

            Symbol* sym = LookupFunctionOverloadInNamespace(table, name, argMasks, argCount,
                currentNs, currentModule);
            if (sym) return sym;

            if (currentNs != NamespaceKind::MODULE) {
                for (u32 i = 0; i < table->importedModules.count; i++) {
                    u32 modIdx = table->importedModules[i];
                    sym = LookupFunctionOverloadInNamespace(table, name, argMasks, argCount,
                        NamespaceKind::MODULE, modIdx);
                    if (sym) return sym;
                }
            }

            return nullptr;
    }
    
    inline void SetCurrentPass(SymbolTableData* table, const ArenaString& passName) {
        if (!table->renderConfig) return;
        
        // Find pass in config
        for (const auto& pass : table->renderConfig->passes) {
            if (Utils::HashStr(pass.name.c_str()) == passName.nameHash) {
                table->currentPass.passName = passName;
                table->currentPass.passType = pass.type;
                table->currentPass.pipelineName = ArenaString::MakeHashOnly(pass.descriptor.pipelineName);
                
                // Copy bindings
                table->currentPass.availableBindings.Init(table->arena, pass.descriptor.resourceBindings.size());
                for (const auto& binding : pass.descriptor.resourceBindings) {
                    table->currentPass.availableBindings.Push(table->arena, binding);
                }
                
                // Copy buffer groups
                table->currentPass.activeBufferGroups.Init(table->arena, pass.descriptor.bufferGroupTypes.size());
                for (auto groupType : pass.descriptor.bufferGroupTypes) {
                    table->currentPass.activeBufferGroups.Push(table->arena, groupType);
                }
                
                // Add pass resources to symbol table
                EnterScope(table);
                for (const auto& binding : pass.descriptor.resourceBindings) {
                    std::string fullNameStr = std::string("resources.") + binding.resourceName;
                    ArenaString fullName = ArenaString::MakeHashOnly(fullNameStr);
                    // Register for debug symbol lookup (both full path and short name)
                    ReverseLookup::Register(fullName.nameHash, fullNameStr.c_str());
                    ArenaString shortName = ArenaString::MakeHashOnly(binding.resourceName);
                    ReverseLookup::Register(shortName.nameHash, binding.resourceName.c_str());

                    // Use AddResource to ensure proper namespace
                    Symbol* sym = AddSymbol(table, fullName, SymbolKind::RESOURCE,
                                           NamespaceKind::RESOURCES, INVALID_INDEX);
                    if (sym) {
                        // New resource - initialize all fields
                        ResourceData& data = table->resources[sym->index];
                        data.type = binding.type;
                        data.bindingIndex = binding.bindingIndex;
                        // stages is already a bitmask (1=vertex, 2=fragment, 3=both)
                        data.stageFlags = binding.stages;
                        data.renderTargetName = ArenaString::MakeHashOnly(binding.resourceName);
                    } else {
                        // Resource already exists - update stage flags to support both stages
                        Symbol* existing = LookupResource(table, fullName);
                        if (existing) {
                            ResourceData& data = table->resources[existing->index];
                            data.stageFlags |= binding.stages;
                        }
                    }
                }
                
                break;
            }
        }
    }
    
    inline bool ValidateResourceAccess(SymbolTableData* table, const ArenaString& resourceName, ShaderStage stage, [[maybe_unused]] const char* sourceBase = nullptr) {
        // Resources are registered with their short name in the RESOURCES namespace
        // The namespace disambiguates them from other symbols
        for (int i = table->symbols.count - 1; i >= 0; i--) {
            Symbol& sym = table->symbols[i];
            if (sym.namespaceKind == NamespaceKind::RESOURCES &&
                sym.name.nameHash == resourceName.nameHash &&
                sym.kind == SymbolKind::RESOURCE) {

                const ResourceData& data = table->resources[sym.index];
                return HasShaderStage(data.stageFlags, stage);
            }
        }
        return false;
    }  

    inline Symbol* AddStruct(SymbolTableData* table, const ArenaString& name, 
        const StructData& data) {
        // Determine namespace based on current parsing context
        NamespaceKind ns = table->inModuleScope ? 
        NamespaceKind::MODULE : NamespaceKind::GLOBAL;

        u32 moduleIdx = table->inModuleScope ? 
        table->currentModuleIndex : INVALID_INDEX;

        Symbol* sym = AddSymbol(table, name, SymbolKind::CUSTOM_TYPE, ns, moduleIdx);
        if (sym) {
        sym->index = table->structs.count;
        table->structs.Push(table->arena, data);
        }
        return sym;
     }

    inline Symbol* AddResource(SymbolTableData* table, const ArenaString& name) {
        return AddSymbol(table, name, SymbolKind::RESOURCE, 
                        NamespaceKind::RESOURCES, INVALID_INDEX);
    }

    inline Symbol* AddAttribute(SymbolTableData* table, const ArenaString& name) {
        return AddSymbol(table, name, SymbolKind::ATTRIBUTE,
                        NamespaceKind::ATTRIBUTES, INVALID_INDEX);
    }

    // Get available attributes based on buffer groups
    inline u32 GetAvailableAttributes(SymbolTableData* table, ArenaString* outAttributes, u32 maxCount) {
        u32 count = 0;
        
        // Check each active buffer group
        for (u32 i = 0; i < table->currentPass.activeBufferGroups.count; i++) {
            auto groupType = table->currentPass.activeBufferGroups[i];
            
            // Add attributes based on group type
            switch (groupType) {
                case BufferGroupLayout::GroupType::OPAQUE_STATIC:
                case BufferGroupLayout::GroupType::OPAQUE_DYNAMIC:
                    if (count < maxCount) outAttributes[count++] = ArenaString::MakeHashOnly("position");
                    if (count < maxCount) outAttributes[count++] = ArenaString::MakeHashOnly("normal");
                    if (count < maxCount) outAttributes[count++] = ArenaString::MakeHashOnly("texcoord");
                    if (count < maxCount) outAttributes[count++] = ArenaString::MakeHashOnly("tangent");
                    break;
                    
                case BufferGroupLayout::GroupType::SHADOW_CASTERS:
                    if (count < maxCount) outAttributes[count++] = ArenaString::MakeHashOnly("position");
                    break;
                    
                default:
                    break;
            }
        }
        
        return count;
    }



    inline u32 AddModule(SymbolTableData* table, const ArenaString& name) {
        u32 hash = name.nameHash;
        
        // Check if already exists
        if (FindModule(table, name) != INVALID_INDEX) {
            return INVALID_INDEX;
        }
        
        u32 moduleIndex = table->modules.count;
        
        // Add module data
        ModuleData module;
        module.name = name;
        module.functionIndices.Init(table->arena, 16);
        module.structIndices.Init(table->arena, 8);
        table->modules.Push(table->arena, module);
        table->moduleNameHashes.Push(table->arena, hash);
        
        // Insert into hash table
        u32 slot = hash & (SymbolTableData::MODULE_HASH_TABLE_SIZE - 1);
        
        if (table->moduleHashTable[slot].moduleIndex == 0xFFFFFFFF) {
            // Empty slot
            table->moduleHashTable[slot].nameHash = hash;
            table->moduleHashTable[slot].moduleIndex = moduleIndex;
            table->moduleHashTable[slot].nextIndex = 0xFFFFFFFF;
        } else {
            // Collision - add to chain
            u32 chainIndex = table->moduleHashChainUsed++;
            table->moduleHashChain[chainIndex] = moduleIndex;
            
            // Find end of chain
            while (table->moduleHashTable[slot].nextIndex != 0xFFFFFFFF) {
                slot = table->moduleHashTable[slot].nextIndex;
            }
            table->moduleHashTable[slot].nextIndex = chainIndex;
        }
        
        return moduleIndex;
    }
    
    inline Symbol* AddModuleFunction(SymbolTableData* table, const ArenaString& name, 
                                    const ArenaString& moduleName) {
        // Create qualified name: module::function (hash-only)
        std::string qualifiedName = std::string("<mod>::<fn>"); // placeholder for length
        qualifiedName = std::string("::"); // reset
        // We don't have source strings here; build a synthetic key using hashes to ensure uniqueness
        // Format: hash(module) + '::' + hash(name)
        qualifiedName.reserve(2 + 10 + 10);
        qualifiedName.append("m").append(std::to_string(moduleName.nameHash));
        qualifiedName.append("::");
        qualifiedName.append("f").append(std::to_string(name.nameHash));
        ArenaString fullName = ArenaString::MakeHashOnly(qualifiedName);
       
        Symbol* sym = AddSymbol(table, fullName, SymbolKind::FUNCTION);
        if (sym && table->currentModuleIndex != INVALID_INDEX) {
            table->modules[table->currentModuleIndex].functionIndices.Push(
                table->arena, sym->index);
        }
        return sym;
    }

    inline Symbol* AddModuleStruct(SymbolTableData* table, const ArenaString& name, const ArenaString& moduleName) {
        // Create qualified name: module::struct (hash-only)
        std::string qualifiedName;
        qualifiedName.reserve(2 + 10 + 10);
        qualifiedName.append("m").append(std::to_string(moduleName.nameHash));
        qualifiedName.append("::");
        qualifiedName.append("s").append(std::to_string(name.nameHash));
        ArenaString fullName = ArenaString::MakeHashOnly(qualifiedName);
        
        Symbol* sym = AddSymbol(table, fullName, SymbolKind::CUSTOM_TYPE);
        if (sym && table->currentModuleIndex != INVALID_INDEX) {
            table->modules[table->currentModuleIndex].structIndices.Push(
                table->arena, sym->index);
        }
        return sym;
    }
    
    inline Symbol* AddModuleEnum(SymbolTableData* table, const ArenaString& name, const ArenaString& moduleName) {
        // Create qualified name: module::enum (hash-only)
        std::string qualifiedName;
        qualifiedName.reserve(2 + 10 + 10);
        qualifiedName.append("m").append(std::to_string(moduleName.nameHash));
        qualifiedName.append("::");
        qualifiedName.append("e").append(std::to_string(name.nameHash));
        ArenaString fullName = ArenaString::MakeHashOnly(qualifiedName);
        
        Symbol* sym = AddSymbol(table, fullName, SymbolKind::ENUM);
        if (sym && table->currentModuleIndex != INVALID_INDEX) {
            table->modules[table->currentModuleIndex].enumIndices.Push(
                table->arena, sym->index);
        }
        return sym;
    }

    inline bool ValidateResourceInPass(SymbolTableData* table, const ArenaString& resourceName, ShaderStage stage) {
        // Check if resource is in current pass's available bindings
        for (u32 i = 0; i < table->currentPass.availableBindings.count; i++) {
            const auto& binding = table->currentPass.availableBindings[i];
            if (Utils::HashStr(binding.resourceName.c_str()) == resourceName.nameHash) {
                // Check shader stage compatibility (stages is a bitmask)
                return HasShaderStage(binding.stages, stage);
            }
        }
        return false;
    }

    // Get resources available in current pass
    inline void GetPassResources(SymbolTableData* table, ShaderStage stage, ArenaArray<ArenaString>& outResources) {
        for (u32 i = 0; i < table->currentPass.availableBindings.count; i++) {
            const auto& binding = table->currentPass.availableBindings[i];
            // stages is a bitmask
            if (HasShaderStage(binding.stages, stage)) {
                outResources.Push(table->arena,
                    ArenaString::MakeHashOnly(binding.resourceName));
            }
        }
    }
    enum class AddConstraintResult {
        SUCCESS,
        DUPLICATE_IN_SCOPE,
        DUPLICATE_IN_MODULE,
        DUPLICATE_FROM_IMPORT
    };

    inline AddConstraintResult AddConstraint(SymbolTableData* table, ArenaString name, TypeMask allowedTypes, ArenaString* outConflictingModule = nullptr) {
        ArenaString finalName = name;
        u32 finalHash = name.nameHash;
        
        // If we're in a module scope, qualify the name
        if (table->inModuleScope && table->currentModuleIndex != INVALID_INDEX) {
            const ModuleData& currentModule = table->modules[table->currentModuleIndex];
            
            // Build qualified name: "ModuleName::ConstraintName"
            // We don't have access to source strings here; synthesize a stable combined key
            std::string qualifiedName;
            qualifiedName.reserve(2 + 10 + 10);
            qualifiedName.append("m").append(std::to_string(currentModule.name.nameHash));
            qualifiedName.append("::");
            qualifiedName.append("c").append(std::to_string(name.nameHash));
            finalName = ArenaString::MakeHashOnly(qualifiedName);
            
            finalHash = finalName.nameHash;
        }
        
        // Check for collision using the (possibly qualified) hash
        if (LookupByHash(table, finalHash)) {
            if (table->inModuleScope) {
                if (outConflictingModule) { *outConflictingModule = table->modules[table->currentModuleIndex].name; }
                return AddConstraintResult::DUPLICATE_IN_MODULE;
            } else {

                if (outConflictingModule) { *outConflictingModule = MakeFromLiteral("<global>"); }
                return AddConstraintResult::DUPLICATE_IN_SCOPE;
            }
        }
        
        ConstraintData constraintData;
        constraintData.name = finalName; 
        constraintData.allowedTypes = allowedTypes;
        
        table->constraints.Push(table->arena, constraintData);

        return AddConstraintResult::SUCCESS; 
    }

    inline TypeMask LookupConstraint(SymbolTableData* table, const ArenaString& name) {
        u32 nameHash = name.nameHash;
        
        // Try direct lookup first (works for both qualified and local names)
        Symbol* sym = LookupByHash(table, nameHash);
        if (sym && sym->kind == SymbolKind::CONSTRAINT) {
            return table->constraints[sym->index].allowedTypes;
        }
        
        // If not found and we're NOT using qualified syntax,
        // try looking in imported modules
        // Without access to source strings here, skip qualified lookup by text
       
            for (u32 i = 0; i < table->importedModules.count; i++) {
                u32 moduleIdx = table->importedModules[i];
                // Compose a synthetic qualified hash using the same scheme as AddConstraint
                std::string qualifiedName;
                qualifiedName.reserve(2 + 10 + 10);
                qualifiedName.append("m").append(std::to_string(table->moduleNameHashes[moduleIdx]));
                qualifiedName.append("::");
                qualifiedName.append("c").append(std::to_string(nameHash));
                u32 qualifiedHash = Utils::HashStr(qualifiedName.c_str());
                sym = LookupByHash(table, qualifiedHash);
                if (sym && sym->kind == SymbolKind::CONSTRAINT) {
                    return table->constraints[sym->index].allowedTypes;
                }
            }
        
        return 0;  // Not found
    }
}


}
