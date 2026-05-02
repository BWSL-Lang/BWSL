// Minimal transitional RenderConfig for WASM builds
// Standalone version without engine dependencies
// Only includes structures needed by the BWSL compiler

#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include "bwsl_compiler_types.h"

// ============= Types from defs.h =============
// These are already defined as macros by defs_wasm.h (force-included)
// We just use the macros directly (u8, u16, u32, f32, etc.)
// INVALID_INDEX is also defined in defs_wasm.h

// ============= Types from platform_math.h =============
struct float2 { float x, y; };
struct float3 { float x, y, z; };
struct float4 { float x, y, z, w; };
struct int4 { int x, y, z, w; };

// ============= Types from graphics_api.h =============

// Minimal PixelFormat enum (subset needed for render config)
// Full version is in graphics_api.h but that pulls in defs.h
enum class PixelFormat : uint32_t {
    Invalid = 0,
    RGBA8Unorm = 70,
    RGBA16Float = 115,
    RGBA32Float = 125,
    RG16Float = 65,
    Depth32Float = 252,
};

// Shader stage flags (for bitmask operations)
enum class ShaderStageFlags : uint8_t {
    None = 0,
    Vertex = 1,
    Fragment = 2,
    Both = 3,
};

// Load/Store actions for attachments
enum class LoadAction { Clear, Load, DontCare };
enum class StoreAction { Store, DontCare };

// BufferGroupLayout minimal (from model_types.h)
struct BufferGroupLayout {
    enum class GroupType : uint8_t {
        OPAQUE_STATIC,
        OPAQUE_DYNAMIC,
        TRANSPARENT,
        SHADOW_CASTERS,
        PARTICLES,
        UI_ELEMENTS,
        DEBUG_GEOMETRY,
        CUSTOM
    };

    // Minimal subset
    uint32_t id = 0;
    GroupType groupType = GroupType::OPAQUE_STATIC;

    struct GroupingCriteria {};
    enum class SortMode { None, FrontToBack, BackToFront, ByMaterial };
};

// Render target descriptor
struct RenderTargetDescriptor {
    std::string name;
    PixelFormat format = PixelFormat::RGBA8Unorm;
    float widthScale = 1.0f;
    float heightScale = 1.0f;
    uint32_t absoluteWidth = 0;
    uint32_t absoluteHeight = 0;
    bool useMipMaps = false;
    uint32_t sampleCount = 1;
    uint32_t arrayLength = 1;
    bool isCubemap = false;
};

// Render target helpers
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

// Uniform buffer binding
// Note: stages is stored as a bitmask (1=vertex, 2=fragment, 3=both)
struct UniformBufferBinding {
    std::string name;
    std::string typeName;
    uint32_t bindingIndex = 0;
    uint8_t stages = 1;  // Bitmask: 1=vertex, 2=fragment, 3=both
};

// Texture binding
// Note: stages is stored as a bitmask (1=vertex, 2=fragment, 3=both)
struct TextureBinding {
    std::string name;
    uint32_t bindingIndex = 0;
    PixelFormat format = PixelFormat::RGBA8Unorm;
    bool isArray = false;
    bool isCubemap = false;
    uint8_t stages = 2;  // Bitmask: 1=vertex, 2=fragment, 3=both (default fragment)
};

// Sampler binding
// Note: stages is stored as a bitmask (1=vertex, 2=fragment, 3=both)
struct SamplerBinding {
    std::string name;
    uint32_t bindingIndex = 0;
    uint8_t stages = 2;  // Bitmask: 1=vertex, 2=fragment, 3=both (default fragment)
};

// Storage buffer binding
// Note: stages is stored as a bitmask (1=vertex, 2=fragment, 3=both)
struct StorageBufferBinding {
    std::string name;
    std::string typeName;
    uint32_t bindingIndex = 0;
    bool readOnly = true;
    uint8_t stages = 1;  // Bitmask: 1=vertex, 2=fragment, 3=both (default vertex)
};

// Buffer group config (minimal)
struct BufferGroupConfig {
    std::string name;
    BufferGroupLayout::GroupType type = BufferGroupLayout::GroupType::OPAQUE_STATIC;
    BufferGroupLayout::GroupingCriteria criteria;
    BufferGroupLayout::SortMode sortMode = BufferGroupLayout::SortMode::None;
    std::vector<std::string> usedInPasses;
    uint32_t priority = 0;
};

// Render pass attachment (color)
struct RenderPassAttachment {
    std::string targetName;
    LoadAction loadAction = LoadAction::Clear;
    StoreAction storeAction = StoreAction::Store;
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

// Depth stencil attachment
struct DepthStencilAttachment {
    std::string targetName;
    LoadAction depthLoadAction = LoadAction::Clear;
    StoreAction depthStoreAction = StoreAction::Store;
    float clearDepth = 1.0f;
    LoadAction stencilLoadAction = LoadAction::DontCare;
    StoreAction stencilStoreAction = StoreAction::DontCare;
    uint32_t clearStencil = 0;
};

// Render pass descriptor (full)
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

// Render config
struct RenderConfig {
    std::string name;

    std::vector<RenderTargetDescriptor> renderTargets;
    std::vector<BufferGroupConfig> bufferGroups;
    std::vector<UniformBufferBinding> uniformBuffers;
    std::vector<TextureBinding> textures;
    std::vector<SamplerBinding> samplers;
    std::vector<StorageBufferBinding> storageBuffers;

    struct ComputeDispatchConfig {
        uint32_t groupCountX = 1;
        uint32_t groupCountY = 1;
        uint32_t groupCountZ = 1;
    };

    struct PassData {
        std::string name;
        PassType type = PassType::Standard;
        RenderPassDescriptor descriptor;
        ComputeDispatchConfig dispatch;
    };

    std::vector<PassData> passes;
};
