#pragma once

#include "bwsl_defs.h"
#include <string>

// Shared compiler-facing pipeline, shader-stage, attribute, and resource types.

enum class VertexAttributeType : u8 {
    POSITION = 0,
    NORMAL = 1,
    TEXCOORD = 2,
    COLOR = 3,
    TANGENT = 4,
    BITANGENT = 5,
    BONE_INDICES = 6,
    BONE_WEIGHTS = 7,
    COUNT
};

constexpr u8 AttributeMask(VertexAttributeType type) {
    return 1 << static_cast<u8>(type);
}

inline const char* GetAttributeTypeName(VertexAttributeType type) {
    switch (type) {
        case VertexAttributeType::POSITION:     return "position";
        case VertexAttributeType::NORMAL:       return "normal";
        case VertexAttributeType::TEXCOORD:     return "texcoord";
        case VertexAttributeType::COLOR:        return "color";
        case VertexAttributeType::TANGENT:      return "tangent";
        case VertexAttributeType::BITANGENT:    return "bitangent";
        case VertexAttributeType::BONE_INDICES: return "boneIndices";
        case VertexAttributeType::BONE_WEIGHTS: return "boneWeights";
        default: return "unknown";
    }
}

enum class ShaderStage : u8 {
    Vertex = 0,
    Fragment = 1,
    Compute = 2,
};

enum class PassType : u8 {
    Standard,
    Shadow,
    PostProcess,
    UI,
    Editor,
    Compute,
    Custom,
    Fullscreen
};

struct ResourceBinding {
    enum Type { Texture, Buffer, Sampler, UniformBuffer, StorageBuffer, StorageImage };
    Type type = Buffer;
    std::string resourceName;
    u32 bindingIndex = 0;
    u8 stages = 2;  // Bitmask: 1=vertex, 2=fragment, 3=both
};

enum class ResourceAccessMode : u8 {
    ReadOnly = 0,
    WriteOnly = 1,
    ReadWrite = 2
};

namespace BWSL {
using ::AttributeMask;
using ::GetAttributeTypeName;
using ::PassType;
using ::ResourceAccessMode;
using ::ResourceBinding;
using ::ShaderStage;
using ::VertexAttributeType;
} // namespace BWSL
