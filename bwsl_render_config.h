// BWSL Render Configuration
// Standalone render config structures and parser for BWSL compiler
// No external dependencies beyond standard library

#pragma once

#include "bwsl_defs.h"
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <unordered_map>

// ============= Math Types =============
struct float2 { float x, y; };
struct float3 { float x, y, z; };
struct float4 { float x, y, z, w; };
struct int4 { int x, y, z, w; };

// ============= Vertex Attribute Types =============
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

enum class LoadAction { Clear, Load, DontCare };
enum class StoreAction { Store, DontCare };

// ============= Resource Bindings =============

struct ResourceBinding {
    enum Type { Texture, Buffer, Sampler, UniformBuffer, StorageBuffer, StorageImage };
    Type type = Buffer;
    std::string resourceName;
    u32 bindingIndex = 0;
    u8 stages = 2;  // Bitmask: 1=vertex, 2=fragment, 3=both
};

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

enum class ResourceAccessMode : u8 {
    ReadOnly = 0,
    WriteOnly = 1,
    ReadWrite = 2
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

// ============= Render Config Parser =============

namespace RenderConfigParser {

struct UniformInfo {
    std::string name;
    std::string typeName;
    u32 bindingIndex;
    u8 stages;
    bool isSampler;
};

struct GeometryConfig {
    enum class Type { None, Instanced };
    Type type = Type::None;
    u32 instanceCount = 1;
};

struct ParseResult {
    bool success = false;
    std::string error;
    RenderConfig config;
    std::unordered_map<std::string, UniformInfo> uniformLookup;
    GeometryConfig geometry;
};

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

inline u8 ParseStage(const std::string& s) {
    if (s == "vertex") return 1;
    if (s == "fragment") return 2;
    if (s == "both") return 3;
    if (s == "compute") return 4;
    return 0;
}

inline PixelFormat ParsePixelFormat(const std::string& fmt) {
    if (fmt == "RGBA16Float") return PixelFormat::RGBA16Float;
    if (fmt == "RG16Float") return PixelFormat::RG16Float;
    if (fmt == "Depth32Float") return PixelFormat::Depth32Float;
    if (fmt == "BGRA8Unorm_sRGB") return PixelFormat::BGRA8Unorm_sRGB;
    if (fmt == "RGBA8Unorm") return PixelFormat::RGBA8Unorm;
    if (fmt == "R32Float") return PixelFormat::R32Float;
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

        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        std::vector<std::string> tokens = Split(trimmed);
        if (tokens.empty()) continue;

        bool indented = IsIndented(line);

        if (!indented) {
            if (tokens[0] == "pipeline") {
                if (tokens.size() < 2) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": pipeline requires a name";
                    return result;
                }
                result.config.name = tokens[1];
                RenderConfig::PipelineEntry entry;
                entry.name = tokens[1];
                result.config.pipelines.push_back(entry);
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

                for (size_t i = 4; i + 1 < tokens.size(); i++) {
                    if (tokens[i] == "array" && i + 1 < tokens.size()) {
                        desc.arrayLength = std::stoi(tokens[i + 1]);
                        i++;
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
                u32 binding = std::stoi(tokens[3]);
                u8 stages = ParseStage(tokens[4]);
                if (stages == 0) stages = 1;

                bool isSampler = (typeName.find("sampler") != std::string::npos);

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
                    texDesc.isArray = (typeName.find("Array") != std::string::npos);
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
                    else if (tokens[i] == "compute") desc.stages |= 4;
                }
                if (desc.stages == 0) desc.stages = 1;

                result.config.storageBuffers.push_back(desc);
            }
            else if (tokens[0] == "image") {
                // Storage image declaration: image name binding [readonly|writeonly|readwrite] [compute|fragment|vertex]
                if (tokens.size() < 3) {
                    result.success = false;
                    result.error = "Line " + std::to_string(lineNum) + ": image requires name and binding";
                    return result;
                }

                StorageImageBinding desc;
                desc.name = tokens[1];
                desc.bindingIndex = std::stoi(tokens[2]);
                desc.accessMode = ResourceAccessMode::WriteOnly;  // Default to write-only
                desc.stages = 0;

                for (size_t i = 3; i < tokens.size(); i++) {
                    if (tokens[i] == "readonly") desc.accessMode = ResourceAccessMode::ReadOnly;
                    else if (tokens[i] == "writeonly") desc.accessMode = ResourceAccessMode::WriteOnly;
                    else if (tokens[i] == "readwrite") desc.accessMode = ResourceAccessMode::ReadWrite;
                    else if (tokens[i] == "compute") desc.stages |= 4;
                    else if (tokens[i] == "vertex") desc.stages |= 1;
                    else if (tokens[i] == "fragment") desc.stages |= 2;
                }
                if (desc.stages == 0) desc.stages = 4;  // Default to compute

                result.config.storageImages.push_back(desc);
            }
            else if (tokens[0] == "instanced") {
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

inline ParseResult ParseFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        ParseResult result;
        result.success = false;
        result.error = "Could not open file: " + path;
        return result;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return Parse(buffer.str());
}

} // namespace RenderConfigParser
