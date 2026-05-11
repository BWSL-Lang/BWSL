#pragma once

#include "bwsl_defs.h"
#include <string>

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
using ::PassType;
using ::ResourceAccessMode;
using ::ResourceBinding;
using ::ShaderStage;
} // namespace BWSL
