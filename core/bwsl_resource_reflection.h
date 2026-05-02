#pragma once

#include "bwsl_ast_soa.h"
#include "bwsl_ir_analysis.h"
#include "bwsl_ir_gen.h"
#include "bwsl_symbol_table.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <string>
#include <vector>

namespace BWSL {

struct ResourceReflectionConfig {
    enum class VertexPullingMode : u8 {
        Disabled,
        SeparateBuffers,
        UnifiedWithOffsets
    };

    VertexPullingMode vertexPullingMode = VertexPullingMode::Disabled;
    u8 attributeMask = 0;
    u32 baseBufferBinding = 0;
    u32 descriptorSet = 0;
};

struct ReflectedResourceBinding {
    std::string name;
    ::ResourceBinding::Type type = ::ResourceBinding::Buffer;
    u32 set = 0;
    u32 binding = 0;
    u8 stages = 0;
    u8 resourceIndex = 0xFF;
    u8 bindingSlot = 0xFF;
    ResourceAccessMode access = ResourceAccessMode::ReadOnly;
    bool combinedSampledImage = false;
    std::string combinedWith;
};

constexpr u32 TEX_METADATA_EXPLICIT_SAMPLER_FLAG = 0x80000000u;

inline bool TextureOpHasExplicitSampler(u32 metadata) {
    return (metadata & TEX_METADATA_EXPLICIT_SAMPLER_FLAG) != 0;
}

inline u16 GetTextureOpExplicitSamplerBinding(u32 metadata) {
    return static_cast<u16>(metadata & 0xFFFFu);
}

inline void SetTextureOpExplicitSamplerMetadata(IR::IRProgram& program,
                                                u32 instructionIndex,
                                                u16 samplerBinding) {
    if (instructionIndex < program.instructionCount) {
        program.metadata[instructionIndex] =
            TEX_METADATA_EXPLICIT_SAMPLER_FLAG | static_cast<u32>(samplerBinding);
    }
}

struct ExplicitSamplerUse {
    u16 textureBinding = 0;
    u16 samplerBinding = 0;
    u8 stageFlags = 0;
};

inline bool ReflectionMaskContains(u32 mask, u32 bindingIndex) {
    return bindingIndex < 32 && (mask & (1u << bindingIndex)) != 0;
}

inline bool ResourceUsedByStage(const ResourceData& resource, const IRAnalysis& analysis) {
    switch (resource.type) {
        case ::ResourceBinding::UniformBuffer:
            return ReflectionMaskContains(analysis.usedUniformMask, resource.bindingIndex);
        case ::ResourceBinding::Texture:
            return ReflectionMaskContains(analysis.usedTextureMask, resource.bindingIndex);
        case ::ResourceBinding::Sampler:
            return ReflectionMaskContains(analysis.usedSamplerMask, resource.bindingIndex);
        case ::ResourceBinding::StorageBuffer:
            return ReflectionMaskContains(analysis.usedStorageBufferMask, resource.bindingIndex);
        case ::ResourceBinding::StorageImage:
            return ReflectionMaskContains(analysis.usedStorageImageMask, resource.bindingIndex);
        default:
            return false;
    }
}

inline ResourceAccessMode CombineAccessModes(ResourceAccessMode lhs,
                                             ResourceAccessMode rhs) {
    if (lhs == rhs) return lhs;
    return ResourceAccessMode::ReadWrite;
}

inline ResourceAccessMode DefaultResourceAccess(const ResourceData& resource) {
    switch (resource.type) {
        case ::ResourceBinding::StorageBuffer:
        case ::ResourceBinding::StorageImage:
            return ResourceAccessMode::ReadWrite;
        case ::ResourceBinding::UniformBuffer:
        case ::ResourceBinding::Texture:
        case ::ResourceBinding::Sampler:
        default:
            return ResourceAccessMode::ReadOnly;
    }
}

inline ResourceAccessMode ResourceAccessForStage(const ResourceData& resource,
                                                 const IRAnalysis& analysis) {
    switch (resource.type) {
        case ::ResourceBinding::UniformBuffer:
        case ::ResourceBinding::Texture:
        case ::ResourceBinding::Sampler:
            return ResourceUsedByStage(resource, analysis)
                ? ResourceAccessMode::ReadOnly
                : DefaultResourceAccess(resource);

        case ::ResourceBinding::StorageBuffer: {
            const bool read =
                ReflectionMaskContains(analysis.readStorageBufferMask, resource.bindingIndex);
            const bool write =
                ReflectionMaskContains(analysis.writeStorageBufferMask, resource.bindingIndex);
            if (read && write) return ResourceAccessMode::ReadWrite;
            if (write) return ResourceAccessMode::WriteOnly;
            if (read) return ResourceAccessMode::ReadOnly;
            return ReflectionMaskContains(analysis.usedStorageBufferMask, resource.bindingIndex)
                ? ResourceAccessMode::ReadWrite
                : DefaultResourceAccess(resource);
        }

        case ::ResourceBinding::StorageImage: {
            const bool read =
                ReflectionMaskContains(analysis.readStorageImageMask, resource.bindingIndex);
            const bool write =
                ReflectionMaskContains(analysis.writeStorageImageMask, resource.bindingIndex);
            if (read && write) return ResourceAccessMode::ReadWrite;
            if (write) return ResourceAccessMode::WriteOnly;
            if (read) return ResourceAccessMode::ReadOnly;
            return ReflectionMaskContains(analysis.usedStorageImageMask, resource.bindingIndex)
                ? ResourceAccessMode::ReadWrite
                : DefaultResourceAccess(resource);
        }

        default:
            return DefaultResourceAccess(resource);
    }
}

inline u8 CollectResourceStageFlags(const ResourceData& resource,
                                    const IRAnalysis* vertexAnalysis,
                                    const IRAnalysis* fragmentAnalysis,
                                    const IRAnalysis* computeAnalysis) {
    u8 stageFlags = 0;

    if (vertexAnalysis && ResourceUsedByStage(resource, *vertexAnalysis)) {
        stageFlags |= SymbolTable::ShaderStageToBit(ShaderStage::Vertex);
    }
    if (fragmentAnalysis && ResourceUsedByStage(resource, *fragmentAnalysis)) {
        stageFlags |= SymbolTable::ShaderStageToBit(ShaderStage::Fragment);
    }
    if (computeAnalysis && ResourceUsedByStage(resource, *computeAnalysis)) {
        stageFlags |= SymbolTable::ShaderStageToBit(ShaderStage::Compute);
    }

    if (stageFlags == 0 && !vertexAnalysis && !fragmentAnalysis && !computeAnalysis) {
        stageFlags = resource.stageFlags;
    }

    return stageFlags;
}

inline ResourceAccessMode CollectResourceAccess(const ResourceData& resource,
                                                const IRAnalysis* vertexAnalysis,
                                                const IRAnalysis* fragmentAnalysis,
                                                const IRAnalysis* computeAnalysis) {
    bool found = false;
    ResourceAccessMode access = DefaultResourceAccess(resource);

    auto mergeStageAccess = [&](const IRAnalysis* analysis) {
        if (!analysis || !ResourceUsedByStage(resource, *analysis)) return;
        const ResourceAccessMode stageAccess = ResourceAccessForStage(resource, *analysis);
        if (!found) {
            access = stageAccess;
            found = true;
        } else {
            access = CombineAccessModes(access, stageAccess);
        }
    };

    mergeStageAccess(vertexAnalysis);
    mergeStageAccess(fragmentAnalysis);
    mergeStageAccess(computeAnalysis);

    return found ? access : DefaultResourceAccess(resource);
}

inline bool PassAllowsResource(const PassData& pass, u32 resourceNameHash) {
    if (pass.usedResources.count == 0) return true;

    for (u32 i = 0; i < pass.usedResources.count; i++) {
        if (pass.usedResources[i].nameHash == resourceNameHash) {
            return true;
        }
    }
    return false;
}

inline std::string GetResourceNameByIndex(const SymbolTableData& symbols, u32 resourceIndex, const char* sourceBase = nullptr) {
    std::string fallback;

    for (s32 i = static_cast<s32>(symbols.symbols.count) - 1; i >= 0; --i) {
        const Symbol& sym = symbols.symbols[i];
        if (sym.kind != SymbolKind::RESOURCE ||
            sym.namespaceKind != NamespaceKind::RESOURCES ||
            sym.index != resourceIndex) {
            continue;
        }

        std::string name = sym.name.ToString(sourceBase);
        if (name.rfind("resources.", 0) == 0) {
            if (fallback.empty()) {
                fallback = name.substr(std::strlen("resources."));
            }
            continue;
        }
        return name;
    }

    return fallback;
}

inline u32 GetVertexPullingBindingCount(const ResourceReflectionConfig& config) {
    switch (config.vertexPullingMode) {
        case ResourceReflectionConfig::VertexPullingMode::SeparateBuffers:
            return static_cast<u32>(std::popcount(config.attributeMask));
        case ResourceReflectionConfig::VertexPullingMode::UnifiedWithOffsets:
            return 2;
        case ResourceReflectionConfig::VertexPullingMode::Disabled:
        default:
            return 0;
    }
}

inline u32 ResolveResourceSet(const ResourceData& resource) {
    return resource.type == ::ResourceBinding::StorageBuffer ? 1u : 0u;
}

inline u32 ResolveResourceBindingIndex(const ResourceData& resource,
                                       const ResourceReflectionConfig& config) {
    if (ResolveResourceSet(resource) != config.descriptorSet) {
        return resource.bindingIndex;
    }

    const u32 occupiedCount = GetVertexPullingBindingCount(config);
    if (occupiedCount == 0) {
        return resource.bindingIndex;
    }

    const u32 occupiedStart = config.baseBufferBinding;
    if (resource.bindingIndex >= occupiedStart) {
        return resource.bindingIndex + occupiedCount;
    }

    return resource.bindingIndex;
}

inline std::vector<ExplicitSamplerUse> CollectExplicitSamplerUses(const IR::IRProgram& ir,
                                                                  ShaderStage stage) {
    std::vector<ExplicitSamplerUse> uses;
    const u8 stageFlags = SymbolTable::ShaderStageToBit(stage);

    auto appendUse = [&](u16 textureBinding, u16 samplerBinding) {
        for (ExplicitSamplerUse& use : uses) {
            if (use.textureBinding == textureBinding &&
                use.samplerBinding == samplerBinding) {
                use.stageFlags |= stageFlags;
                return;
            }
        }

        ExplicitSamplerUse use;
        use.textureBinding = textureBinding;
        use.samplerBinding = samplerBinding;
        use.stageFlags = stageFlags;
        uses.push_back(use);
    };

    for (u32 i = 0; i < ir.instructionCount; i++) {
        switch (ir.opcodes[i]) {
            case IR::OP_TEX_SAMPLE:
            case IR::OP_TEX_SAMPLE_LOD:
            case IR::OP_TEX_SAMPLE_BIAS:
            case IR::OP_TEX_SAMPLE_GRAD:
            case IR::OP_TEX_SAMPLE_CMP: {
                const u32 metadata = ir.metadata[i];
                if (!TextureOpHasExplicitSampler(metadata)) {
                    break;
                }

                const u16 texReg = ir.GetOperand(i, 0);
                if ((texReg & 0xF000) != 0x2000) {
                    break;
                }

                appendUse(static_cast<u16>(texReg & 0x0FFFu),
                          GetTextureOpExplicitSamplerBinding(metadata));
                break;
            }
            default:
                break;
        }
    }

    return uses;
}

inline ReflectedResourceBinding* FindReflectedResourceBinding(
    std::vector<ReflectedResourceBinding>& bindings,
    ::ResourceBinding::Type type,
    u16 bindingSlot,
    u8 stageFlags) {
    for (ReflectedResourceBinding& binding : bindings) {
        if (binding.type != type || binding.bindingSlot != bindingSlot) {
            continue;
        }
        if (stageFlags != 0 && (binding.stages & stageFlags) == 0) {
            continue;
        }
        return &binding;
    }
    return nullptr;
}

inline std::vector<ReflectedResourceBinding> BuildResolvedResourceReflection(
    const AST* ast,
    const PipelineData* pipeline,
    const PassData* pass,
    const SymbolTableData& symbols,
    const char* sourceBase,
    const IRAnalysis* vertexAnalysis,
    const IRAnalysis* fragmentAnalysis,
    const IRAnalysis* computeAnalysis,
    const ResourceReflectionConfig& config,
    const std::vector<ExplicitSamplerUse>* explicitSamplerUses = nullptr) {

    std::vector<ReflectedResourceBinding> bindings;

    auto appendResource = [&](const ArenaString& resourceName, u8 declaredIndex) {
        if (pass && !PassAllowsResource(*pass, resourceName.nameHash)) {
            return;
        }

        Symbol* sym = SymbolTable::LookupResource(const_cast<SymbolTableData*>(&symbols), resourceName);
        if (!sym || sym->index >= symbols.resources.count) {
            return;
        }

        const ResourceData& resource = symbols.resources[sym->index];
        const u8 stages = CollectResourceStageFlags(resource, vertexAnalysis, fragmentAnalysis, computeAnalysis);
        if (stages == 0) {
            return;
        }

        std::string name = resourceName.ToString(sourceBase);
        if (name.empty()) {
            name = GetResourceNameByIndex(symbols, sym->index, sourceBase);
        }
        if (name.empty()) {
            return;
        }

        ReflectedResourceBinding binding;
        binding.name = std::move(name);
        binding.type = resource.type;
        binding.set = ResolveResourceSet(resource);
        binding.binding = ResolveResourceBindingIndex(resource, config);
        binding.stages = stages;
        binding.resourceIndex = declaredIndex;
        binding.bindingSlot = static_cast<u8>(resource.bindingIndex);
        binding.access = CollectResourceAccess(resource, vertexAnalysis, fragmentAnalysis, computeAnalysis);
        bindings.push_back(std::move(binding));
    };

    if (ast && pipeline && pipeline->resources.count > 0) {
        for (u32 i = 0; i < pipeline->resources.count; i++) {
            const ResourceDeclData& decl = ast->GetResourceDecl(pipeline->resources[i]);
            appendResource(decl.name, decl.resourceIndex);
        }
    } else {
        for (u32 resourceIndex = 0; resourceIndex < symbols.resources.count; resourceIndex++) {
            const ResourceData& resource = symbols.resources[resourceIndex];
            const u8 stages = CollectResourceStageFlags(resource, vertexAnalysis, fragmentAnalysis, computeAnalysis);
            if (stages == 0) {
                continue;
            }

            std::string name = GetResourceNameByIndex(symbols, resourceIndex, sourceBase);
            if (name.empty()) {
                continue;
            }

            ReflectedResourceBinding binding;
            binding.name = std::move(name);
            binding.type = resource.type;
            binding.set = ResolveResourceSet(resource);
            binding.binding = ResolveResourceBindingIndex(resource, config);
            binding.stages = stages;
            binding.bindingSlot = static_cast<u8>(resource.bindingIndex);
            binding.access = CollectResourceAccess(resource, vertexAnalysis, fragmentAnalysis, computeAnalysis);
            bindings.push_back(std::move(binding));
        }
    }

    std::sort(bindings.begin(), bindings.end(),
              [](const ReflectedResourceBinding& lhs, const ReflectedResourceBinding& rhs) {
                  if (lhs.set != rhs.set) return lhs.set < rhs.set;
                  if (lhs.binding != rhs.binding) return lhs.binding < rhs.binding;
                  return lhs.name < rhs.name;
              });

    bindings.erase(std::unique(bindings.begin(), bindings.end(),
                               [](const ReflectedResourceBinding& lhs, const ReflectedResourceBinding& rhs) {
                                   return lhs.name == rhs.name &&
                                          lhs.type == rhs.type &&
                                          lhs.set == rhs.set &&
                                          lhs.binding == rhs.binding &&
                                          lhs.stages == rhs.stages;
                               }),
                   bindings.end());

    if (explicitSamplerUses) {
        for (const ExplicitSamplerUse& use : *explicitSamplerUses) {
            ReflectedResourceBinding* textureBinding =
                FindReflectedResourceBinding(bindings, ::ResourceBinding::Texture,
                                             use.textureBinding, use.stageFlags);
            ReflectedResourceBinding* samplerBinding =
                FindReflectedResourceBinding(bindings, ::ResourceBinding::Sampler,
                                             use.samplerBinding, use.stageFlags);
            if (!textureBinding || !samplerBinding) {
                continue;
            }

            if (textureBinding->combinedWith.empty() ||
                textureBinding->combinedWith == samplerBinding->name) {
                textureBinding->combinedSampledImage = true;
                textureBinding->combinedWith = samplerBinding->name;
            }

            if (samplerBinding->combinedWith.empty() ||
                samplerBinding->combinedWith == textureBinding->name) {
                samplerBinding->combinedSampledImage = true;
                samplerBinding->combinedWith = textureBinding->name;
                samplerBinding->set = textureBinding->set;
                samplerBinding->binding = textureBinding->binding;
            }
        }
    }

    return bindings;
}

} // namespace BWSL
