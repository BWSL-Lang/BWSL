// BWSL Render Configuration
// Transitional render config structures used internally by compiler backends.
// No external dependencies beyond standard library

#pragma once

#include "bwsl_defs.h"
#include "bwsl_compiler_types.h"
#include <string>
#include <vector>

// ============= Math Types =============
struct float2 { float x, y; };
struct float3 { float x, y, z; };
struct float4 { float x, y, z, w; };
struct int4 { int x, y, z, w; };

// ============= Graphics API Types =============

enum class PixelFormat : u32 {
    Invalid = 0,
    RGBA8Unorm = 70,
    BGRA8Unorm_sRGB = 81,
    RGBA16Float = 115,
    RGBA32Float = 125,
    RG16Float = 65,
    R32Float = 55,
    Depth32Float = 252,
};

enum class LoadAction { Clear, Load, DontCare };
enum class StoreAction { Store, DontCare };

// ============= Resource Bindings =============

struct UniformBufferBinding {
    std::string name;
    std::string typeName;
    u32 bindingIndex = 0;
    u8 stages = 1;  // Bitmask: 1=vertex, 2=fragment, 3=both
};

struct TextureBinding {
    std::string name;
    u32 bindingIndex = 0;
    PixelFormat format = PixelFormat::RGBA8Unorm;
    bool isArray = false;
    bool isCubemap = false;
    u8 stages = 2;  // Bitmask: 1=vertex, 2=fragment, 3=both
};

struct SamplerBinding {
    std::string name;
    u32 bindingIndex = 0;
    u8 stages = 2;
};

struct StorageBufferBinding {
    std::string name;
    std::string typeName;
    u32 bindingIndex = 0;
    bool readOnly = true;
    u8 stages = 1;
};

struct StorageImageBinding {
    std::string name;
    u32 bindingIndex = 0;
    PixelFormat format = PixelFormat::RGBA32Float;
    ResourceAccessMode accessMode = ResourceAccessMode::WriteOnly;
    u8 stages = 4;  // Bitmask: 1=vertex, 2=fragment, 4=compute
};

// ============= Buffer Group Config (minimal) =============

struct BufferGroupLayout {
    enum class GroupType : u8 {
        OPAQUE_STATIC,
        OPAQUE_DYNAMIC,
        TRANSPARENT,
        SHADOW_CASTERS,
        PARTICLES,
        UI_ELEMENTS,
        DEBUG_GEOMETRY,
        CUSTOM
    };
    u32 id = 0;
    GroupType groupType = GroupType::OPAQUE_STATIC;
    struct GroupingCriteria {};
    enum class SortMode { None, FrontToBack, BackToFront, ByMaterial };
};

struct BufferGroupConfig {
    std::string name;
    BufferGroupLayout::GroupType type = BufferGroupLayout::GroupType::OPAQUE_STATIC;
    BufferGroupLayout::GroupingCriteria criteria;
    BufferGroupLayout::SortMode sortMode = BufferGroupLayout::SortMode::None;
    std::vector<std::string> usedInPasses;
    u32 priority = 0;
};

// ============= Render Targets =============

struct RenderTargetDescriptor {
    std::string name;
    PixelFormat format = PixelFormat::RGBA8Unorm;
    float widthScale = 1.0f;
    float heightScale = 1.0f;
    u32 absoluteWidth = 0;
    u32 absoluteHeight = 0;
    bool useMipMaps = false;
    u32 sampleCount = 1;
    u32 arrayLength = 1;
    bool isCubemap = false;
};

namespace RenderTargetHelpers {
    inline RenderTargetDescriptor ColorTarget(const std::string& name, float scale, PixelFormat format) {
        RenderTargetDescriptor desc;
        desc.name = name;
        desc.format = format;
        desc.widthScale = scale;
        desc.heightScale = scale;
        return desc;
    }

    inline RenderTargetDescriptor DepthTarget(const std::string& name, float scale) {
        RenderTargetDescriptor desc;
        desc.name = name;
        desc.format = PixelFormat::Depth32Float;
        desc.widthScale = scale;
        desc.heightScale = scale;
        return desc;
    }
}

// ============= Render Pass Attachments =============

struct RenderPassAttachment {
    std::string targetName;
    LoadAction loadAction = LoadAction::Clear;
    StoreAction storeAction = StoreAction::Store;
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

struct DepthStencilAttachment {
    std::string targetName;
    LoadAction depthLoadAction = LoadAction::Clear;
    StoreAction depthStoreAction = StoreAction::Store;
    float clearDepth = 1.0f;
    LoadAction stencilLoadAction = LoadAction::DontCare;
    StoreAction stencilStoreAction = StoreAction::DontCare;
    u32 clearStencil = 0;
};

struct RenderPassDescriptor {
    std::string name;
    std::string debugName;
    std::string pipelineName;
    std::vector<ResourceBinding> resourceBindings;
    std::vector<BufferGroupLayout::GroupType> bufferGroupTypes;
    std::vector<RenderPassAttachment> colorAttachments;
    DepthStencilAttachment depthStencilAttachment;
    std::vector<std::string> dependencies;
};

// ============= Render Config =============

struct RenderConfig {
    std::string name;

    struct PipelineEntry {
        std::string name;
    };
    std::vector<PipelineEntry> pipelines;

    std::vector<RenderTargetDescriptor> renderTargets;
    std::vector<BufferGroupConfig> bufferGroups;
    std::vector<UniformBufferBinding> uniformBuffers;
    std::vector<TextureBinding> textures;
    std::vector<SamplerBinding> samplers;
    std::vector<StorageBufferBinding> storageBuffers;
    std::vector<StorageImageBinding> storageImages;

    struct ComputeDispatchConfig {
        u32 groupCountX = 1;
        u32 groupCountY = 1;
        u32 groupCountZ = 1;
    };

    struct PassData {
        std::string name;
        PassType type = PassType::Standard;
        RenderPassDescriptor descriptor;
        ComputeDispatchConfig dispatch;
    };
    std::vector<PassData> passes;
};
