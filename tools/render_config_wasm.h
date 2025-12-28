// Minimal RenderConfig for WASM builds
// Standalone version without engine dependencies
// Only includes structures needed by the BWSL compiler

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <sstream>
#include <ctime>
#include <unordered_map>

// ============= Types from defs.h =============
// These are already defined as macros by defs_wasm.h (force-included)
// We just use the macros directly (u8, u16, u32, f32, etc.)
// INVALID_INDEX is also defined in defs_wasm.h

// ============= Types from platform_math.h =============
struct float2 { float x, y; };
struct float3 { float x, y, z; };
struct float4 { float x, y, z, w; };
struct int4 { int x, y, z, w; };

// ============= Types from model_types.h =============
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

// ShaderStage enum (from graphics_api.h)
enum class ShaderStage : uint8_t {
    Vertex = 0,
    Fragment = 1,
    Compute = 2,
};

// Shader stage flags (for bitmask operations)
enum class ShaderStageFlags : uint8_t {
    None = 0,
    Vertex = 1,
    Fragment = 2,
    Both = 3,
};

// PassType enum (from render_pass.h)
enum class PassType : uint8_t {
    Standard,
    Shadow,
    PostProcess,
    UI,
    Editor,
    Compute,
    Custom,
    Fullscreen  // Added for WASM compatibility
};

// Load/Store actions for attachments
enum class LoadAction { Clear, Load, DontCare };
enum class StoreAction { Store, DontCare };

// ResourceBinding (from render_pass.h)
// Note: stages is stored as a bitmask (1=vertex, 2=fragment, 3=both)
struct ResourceBinding {
    enum Type {
        Texture,
        Buffer,
        Sampler
    };

    Type type = Buffer;
    std::string resourceName;
    uint32_t bindingIndex = 0;
    uint8_t stages = 2;  // Bitmask: 1=vertex, 2=fragment, 3=both (default fragment)
};

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

// Uniform type enum
enum class UniformType {
    Float,
    Float2,
    Float3,
    Float4,
    Int,
    Int2,
    Int3,
    Int4,
    Mat3,
    Mat4,
};

// Uniform buffer binding
// Note: stages is stored as a bitmask (1=vertex, 2=fragment, 3=both)
// to match what bwsl_symbol_table.h expects from InitFromRenderConfig
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

// Track uniform declarations for bind lookups
struct UniformInfo {
    std::string name;
    std::string typeName;
    uint32_t bindingIndex;
    uint8_t stages;  // Bitmask: 1=vertex, 2=fragment, 3=both
    bool isSampler;
};

// Geometry configuration (instancing, etc.)
struct GeometryConfig {
    enum class Type { None, Instanced };
    Type type = Type::None;
    uint32_t instanceCount = 1;
};

// Parser result
struct ParseResult {
    bool success = false;
    std::string error;
    RenderConfig config;
    std::unordered_map<std::string, UniformInfo> uniformLookup;
    GeometryConfig geometry;
};

// Full render config parser (inline implementation)
namespace RenderConfigParser {

inline std::string Trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

inline std::vector<std::string> Split(const std::string& str) {
    std::vector<std::string> tokens;
    std::istringstream iss(str);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

inline bool IsIndented(const std::string& line) {
    return !line.empty() && (line[0] == ' ' || line[0] == '\t');
}

inline bool IsFloat(const std::string& s) {
    if (s.empty()) return false;
    char* end;
    std::strtof(s.c_str(), &end);
    return end != s.c_str() && *end == '\0';
}

inline LoadAction ParseLoadAction(const std::string& s) {
    if (s == "clear") return LoadAction::Clear;
    if (s == "load") return LoadAction::Load;
    if (s == "dontcare") return LoadAction::DontCare;
    return LoadAction::Clear;
}

inline StoreAction ParseStoreAction(const std::string& s) {
    if (s == "store") return StoreAction::Store;
    if (s == "dontcare") return StoreAction::DontCare;
    return StoreAction::Store;
}

inline uint8_t ParseStage(const std::string& s) {
    if (s == "vertex") return 1;
    if (s == "fragment") return 2;
    if (s == "both") return 3;
    return 0;
}

inline PixelFormat ParsePixelFormat(const std::string& fmt) {
    if (fmt == "RGBA16Float") return PixelFormat::RGBA16Float;
    if (fmt == "RG16Float") return PixelFormat::RG16Float;
    if (fmt == "Depth32Float") return PixelFormat::Depth32Float;
    if (fmt == "RGBA8Unorm") return PixelFormat::RGBA8Unorm;
    if (fmt == "RGBA32Float") return PixelFormat::RGBA32Float;
    return PixelFormat::Invalid;
}

inline ParseResult Parse(const std::string& content) {
    ParseResult result;
    result.success = true;

    std::istringstream stream(content);
    std::string line;
    int lineNum = 0;

    RenderConfig::PassData* currentPass = nullptr;

    while (std::getline(stream, line)) {
        lineNum++;

        // Skip empty lines and comments
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        std::vector<std::string> tokens = Split(trimmed);
        if (tokens.empty()) continue;

        bool indented = IsIndented(line);

        if (!indented) {
            // Top-level declaration
            if (tokens[0] == "pipeline") {
                if (tokens.size() < 2) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": pipeline requires a name";
                    return result;
                }
                result.config.name = tokens[1];
            }
            else if (tokens[0] == "pass") {
                if (tokens.size() < 2) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": pass requires a name";
                    return result;
                }

                RenderConfig::PassData pass;
                pass.name = tokens[1];
                pass.type = PassType::Standard;
                pass.descriptor.name = tokens[1];
                pass.descriptor.debugName = tokens[1];

                result.config.passes.push_back(pass);
                currentPass = &result.config.passes.back();
            }
            else if (tokens[0] == "target") {
                if (tokens.size() < 4) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": target requires name, format, and size";
                    return result;
                }

                RenderTargetDescriptor desc;
                desc.name = tokens[1];
                desc.widthScale = 1.0f;
                desc.heightScale = 1.0f;
                desc.sampleCount = 1;
                desc.arrayLength = 1;
                desc.absoluteWidth = 0;
                desc.absoluteHeight = 0;

                desc.format = ParsePixelFormat(tokens[2]);

                std::string sizeStr = tokens[3];
                if (sizeStr == "viewport") {
                    desc.widthScale = 1.0f;
                    desc.heightScale = 1.0f;
                } else {
                    size_t xPos = sizeStr.find('x');
                    if (xPos != std::string::npos) {
                        desc.absoluteWidth = std::stoi(sizeStr.substr(0, xPos));
                        desc.absoluteHeight = std::stoi(sizeStr.substr(xPos + 1));
                        desc.widthScale = 0.0f;
                        desc.heightScale = 0.0f;
                    }
                }

                result.config.renderTargets.push_back(desc);
            }
            else if (tokens[0] == "uniform") {
                if (tokens.size() < 5) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": uniform requires name, type, slot, and stage";
                    return result;
                }

                std::string name = tokens[1];
                std::string typeName = tokens[2];
                uint32_t binding = std::stoi(tokens[3]);
                uint8_t stages = ParseStage(tokens[4]);
                if (stages == 0) stages = 1;

                bool isSampler = (typeName.find("sampler") != std::string::npos);

                // Store in lookup for bind resolution
                UniformInfo info;
                info.name = name;
                info.typeName = typeName;
                info.bindingIndex = binding;
                info.stages = stages;
                info.isSampler = isSampler;
                result.uniformLookup[name] = info;

                if (isSampler) {
                    TextureBinding texDesc;
                    texDesc.name = name;
                    texDesc.bindingIndex = binding;
                    texDesc.stages = stages;
                    texDesc.isArray = false;
                    texDesc.isCubemap = (typeName.find("Cube") != std::string::npos);
                    result.config.textures.push_back(texDesc);
                } else {
                    UniformBufferBinding desc;
                    desc.name = name;
                    desc.typeName = typeName;
                    desc.bindingIndex = binding;
                    desc.stages = stages;
                    result.config.uniformBuffers.push_back(desc);
                }
            }
            else if (tokens[0] == "texture") {
                if (tokens.size() < 3) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": texture requires name and binding";
                    return result;
                }

                TextureBinding desc;
                desc.name = tokens[1];
                desc.bindingIndex = std::stoi(tokens[2]);
                desc.stages = 0;
                desc.isArray = false;
                desc.isCubemap = false;

                for (size_t i = 3; i < tokens.size(); i++) {
                    if (tokens[i] == "array") desc.isArray = true;
                    else if (tokens[i] == "cubemap") desc.isCubemap = true;
                    else if (tokens[i] == "vertex") desc.stages |= 1;
                    else if (tokens[i] == "fragment") desc.stages |= 2;
                    else if (tokens[i] == "both") desc.stages = 3;
                }
                if (desc.stages == 0) desc.stages = 2;

                result.config.textures.push_back(desc);
            }
            else if (tokens[0] == "sampler") {
                if (tokens.size() < 3) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": sampler requires name and binding";
                    return result;
                }

                SamplerBinding desc;
                desc.name = tokens[1];
                desc.bindingIndex = std::stoi(tokens[2]);
                desc.stages = 0;

                for (size_t i = 3; i < tokens.size(); i++) {
                    if (tokens[i] == "vertex") desc.stages |= 1;
                    else if (tokens[i] == "fragment") desc.stages |= 2;
                    else if (tokens[i] == "both") desc.stages = 3;
                }
                if (desc.stages == 0) desc.stages = 2;

                result.config.samplers.push_back(desc);
            }
            else if (tokens[0] == "buffer") {
                if (tokens.size() < 4) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": buffer requires name, type, and binding";
                    return result;
                }

                StorageBufferBinding desc;
                desc.name = tokens[1];
                desc.typeName = tokens[2];
                desc.bindingIndex = std::stoi(tokens[3]);
                desc.readOnly = true;
                desc.stages = 0;

                for (size_t i = 4; i < tokens.size(); i++) {
                    if (tokens[i] == "readwrite") desc.readOnly = false;
                    else if (tokens[i] == "readonly") desc.readOnly = true;
                    else if (tokens[i] == "vertex") desc.stages |= 1;
                    else if (tokens[i] == "fragment") desc.stages |= 2;
                    else if (tokens[i] == "both") desc.stages = 3;
                }
                if (desc.stages == 0) desc.stages = 1;

                result.config.storageBuffers.push_back(desc);
            }
            else if (tokens[0] == "instanced") {
                // instanced <count>
                // Configures geometry for instanced rendering
                if (tokens.size() < 2) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": instanced requires instance count";
                    return result;
                }
                result.geometry.type = GeometryConfig::Type::Instanced;
                result.geometry.instanceCount = std::stoi(tokens[1]);
            }
        }
        else {
            // Indented = pass property
            if (!currentPass) {
                result.success = false;
                result.error = "Line " + std::to_string(lineNum) + ": property outside of pass";
                return result;
            }

            if (tokens[0] == "depends") {
                if (tokens.size() < 2) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": depends requires pass name";
                    return result;
                }
                if (tokens[1] != "none") {
                    currentPass->descriptor.dependencies.push_back(tokens[1]);
                }
            }
            else if (tokens[0] == "color") {
                if (tokens.size() < 4) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": color requires target, load_action, and store_action";
                    return result;
                }

                RenderPassAttachment attachment;
                attachment.targetName = tokens[1];

                // Handle @screen as special swapchain target
                if (attachment.targetName == "@screen") {
                    attachment.targetName = "";
                }

                attachment.loadAction = ParseLoadAction(tokens[2]);

                size_t storeIdx = 3;
                if (attachment.loadAction == LoadAction::Clear && tokens.size() >= 8) {
                    if (IsFloat(tokens[3]) && IsFloat(tokens[4]) &&
                        IsFloat(tokens[5]) && IsFloat(tokens[6])) {
                        attachment.clearColor[0] = std::stof(tokens[3]);
                        attachment.clearColor[1] = std::stof(tokens[4]);
                        attachment.clearColor[2] = std::stof(tokens[5]);
                        attachment.clearColor[3] = std::stof(tokens[6]);
                        storeIdx = 7;
                    }
                }

                if (storeIdx < tokens.size()) {
                    attachment.storeAction = ParseStoreAction(tokens[storeIdx]);
                } else {
                    attachment.storeAction = StoreAction::Store;
                }

                currentPass->descriptor.colorAttachments.push_back(attachment);
            }
            else if (tokens[0] == "depth") {
                if (tokens.size() < 4) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": depth requires target, load_action, and store_action";
                    return result;
                }

                currentPass->descriptor.depthStencilAttachment.targetName = tokens[1];
                currentPass->descriptor.depthStencilAttachment.depthLoadAction = ParseLoadAction(tokens[2]);

                size_t storeIdx = 3;
                if (currentPass->descriptor.depthStencilAttachment.depthLoadAction == LoadAction::Clear &&
                    tokens.size() >= 5 && IsFloat(tokens[3])) {
                    currentPass->descriptor.depthStencilAttachment.clearDepth = std::stof(tokens[3]);
                    storeIdx = 4;
                } else {
                    currentPass->descriptor.depthStencilAttachment.clearDepth = 1.0f;
                }

                if (storeIdx < tokens.size()) {
                    currentPass->descriptor.depthStencilAttachment.depthStoreAction = ParseStoreAction(tokens[storeIdx]);
                } else {
                    currentPass->descriptor.depthStencilAttachment.depthStoreAction = StoreAction::Store;
                }

                currentPass->descriptor.depthStencilAttachment.stencilLoadAction = LoadAction::DontCare;
                currentPass->descriptor.depthStencilAttachment.stencilStoreAction = StoreAction::DontCare;
                currentPass->descriptor.depthStencilAttachment.clearStencil = 0;
            }
            else if (tokens[0] == "bind") {
                if (tokens.size() < 3) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": bind requires uniform_name and target_name";
                    return result;
                }

                std::string uniformName = tokens[1];
                std::string targetName = tokens[2];

                auto it = result.uniformLookup.find(uniformName);
                if (it == result.uniformLookup.end()) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": unknown uniform '" + uniformName + "'";
                    return result;
                }

                ResourceBinding binding;
                binding.type = ResourceBinding::Texture;
                binding.resourceName = targetName;
                binding.bindingIndex = it->second.bindingIndex;
                binding.stages = it->second.stages;
                currentPass->descriptor.resourceBindings.push_back(binding);
            }
            else if (tokens[0] == "dispatch") {
                if (tokens.size() != 4) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": dispatch requires 3 arguments: dispatch X Y Z";
                    return result;
                }

                currentPass->dispatch.groupCountX = std::stoi(tokens[1]);
                currentPass->dispatch.groupCountY = std::stoi(tokens[2]);
                currentPass->dispatch.groupCountZ = std::stoi(tokens[3]);
            }
            else if (tokens[0] == "type") {
                if (tokens.size() < 2) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": type requires value";
                    return result;
                }

                if (tokens[1] == "shadow") currentPass->type = PassType::Shadow;
                else if (tokens[1] == "geometry") currentPass->type = PassType::Standard;
                else if (tokens[1] == "standard") currentPass->type = PassType::Standard;
                else if (tokens[1] == "fullscreen") currentPass->type = PassType::Fullscreen;
                else if (tokens[1] == "postprocess") currentPass->type = PassType::PostProcess;
                else if (tokens[1] == "compute") currentPass->type = PassType::Compute;
                else if (tokens[1] == "ui") currentPass->type = PassType::UI;
                else if (tokens[1] == "editor") currentPass->type = PassType::Editor;
                else if (tokens[1] == "custom") currentPass->type = PassType::Custom;
            }
            else if (tokens[0] == "pipeline") {
                if (tokens.size() < 2) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": pipeline requires name";
                    return result;
                }
                currentPass->descriptor.pipelineName = tokens[1];
            }
        }
    }

    // Default pass if none specified
    if (result.config.passes.empty()) {
        RenderConfig::PassData defaultPass;
        defaultPass.name = "Main";
        defaultPass.descriptor.name = "Main";
        defaultPass.descriptor.pipelineName = result.config.name;
        result.config.passes.push_back(defaultPass);
    }

    return result;
}

} // namespace RenderConfigParser
