#pragma once

#include "bwsl_resource_reflection.h"
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace BWSL::ReflectionJson {

inline std::string EscapeJsonString(const std::string& str) {
    std::string result;
    result.reserve(str.size() * 2);
    for (char c : str) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (c >= 0 && c < 32) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    result += buf;
                } else {
                    result += c;
                }
                break;
        }
    }
    return result;
}

inline const char* ResourceTypeToString(::ResourceBinding::Type type) {
    switch (type) {
        case ::ResourceBinding::UniformBuffer: return "uniform";
        case ::ResourceBinding::StorageBuffer: return "storage_buffer";
        case ::ResourceBinding::Texture: return "texture";
        case ::ResourceBinding::Sampler: return "sampler";
        case ::ResourceBinding::StorageImage: return "storage_image";
        default: return "buffer";
    }
}

inline const char* ResourceAccessToString(BWSL::ResourceAccessMode access) {
    switch (access) {
        case BWSL::ResourceAccessMode::ReadOnly: return "readonly";
        case BWSL::ResourceAccessMode::WriteOnly: return "writeonly";
        case BWSL::ResourceAccessMode::ReadWrite: return "readwrite";
        default: return "readonly";
    }
}

inline std::string StageFlagsToJsonArray(u8 stageFlags, bool prettySpacing = false) {
    std::string json = "[";
    bool first = true;
    auto appendStage = [&](const char* stageName, ShaderStage stage) {
        if ((stageFlags & SymbolTable::ShaderStageToBit(stage)) == 0) return;
        if (!first) json += prettySpacing ? ", " : ",";
        json += "\"";
        json += stageName;
        json += "\"";
        first = false;
    };

    appendStage("vertex", ShaderStage::Vertex);
    appendStage("fragment", ShaderStage::Fragment);
    appendStage("compute", ShaderStage::Compute);

    json += "]";
    return json;
}

inline void AppendCombinedSamplerUniformsJson(std::ostringstream& json,
                                             const std::vector<CombinedSamplerUniform>& uniforms) {
    if (uniforms.empty()) return;
    json << ",\"combinedSamplerUniforms\":[";
    for (size_t i = 0; i < uniforms.size(); ++i) {
        const auto& uniform = uniforms[i];
        if (i) json << ",";
        json << "{\"name\":\"" << EscapeJsonString(uniform.name)
             << "\",\"sampler\":\"" << EscapeJsonString(uniform.samplerName)
             << "\",\"samplerSet\":" << uniform.samplerSet
             << ",\"samplerBinding\":" << uniform.samplerBinding
             << ",\"stages\":" << StageFlagsToJsonArray(uniform.stages) << "}";
    }
    json << "]";
}

inline void AppendHLSLResourcesJson(std::ostringstream& json,
                                   const std::vector<ReflectedResourceBinding>& resources) {
    bool first = true;
    for (const auto& resource : resources) {
        if (!resource.hlslOnly) continue;
        if (first) json << ",\"hlslResources\":[";
        else json << ",";
        first = false;
        json << "{\"name\":\"" << EscapeJsonString(resource.name)
             << "\",\"type\":\"uniform\",\"set\":" << resource.set
             << ",\"binding\":" << resource.binding
             << ",\"stages\":" << StageFlagsToJsonArray(resource.stages)
             << ",\"builtin\":\"" << EscapeJsonString(resource.builtin)
             << "\",\"valueType\":\"uint3\",\"byteSize\":" << resource.byteSize << "}";
    }
    if (!first) json << "]";
}

inline void AppendCompactResourceReflectionJson(
    std::ostringstream& json,
    const std::vector<ReflectedResourceBinding>& resources) {
    json << "\"resources\":[";
    bool first = true;
    for (size_t i = 0; i < resources.size(); i++) {
        const ReflectedResourceBinding& resource = resources[i];
        if (resource.hlslOnly) continue;
        if (!first) json << ",";
        first = false;
        json << "{";
        json << "\"name\":\"" << EscapeJsonString(resource.name) << "\",";
        json << "\"type\":\"" << ResourceTypeToString(resource.type) << "\",";
        json << "\"set\":" << resource.set << ",";
        json << "\"binding\":" << resource.binding << ",";
        json << "\"stages\":" << StageFlagsToJsonArray(resource.stages) << ",";
        json << "\"access\":\"" << ResourceAccessToString(resource.access) << "\"";
        if (resource.combinedSampledImage) {
            json << ",\"abi\":\"combined_sampled_image\"";
            if (!resource.combinedWith.empty()) {
                json << ",\"combinedWith\":\"" << EscapeJsonString(resource.combinedWith) << "\"";
            }
        }
        if (resource.separateImageSampler)
            json << ",\"abi\":\"" << (resource.type == ::ResourceBinding::Texture ? "separate_image" : "sampler") << "\"";
        if (!resource.defaultSamplerFor.empty())
            json << ",\"defaultSamplerFor\":\"" << EscapeJsonString(resource.defaultSamplerFor) << "\"";
        AppendCombinedSamplerUniformsJson(json, resource.combinedSamplerUniforms);
        json << "}";
    }
    json << "]";
    AppendHLSLResourcesJson(json, resources);
}

inline std::string BuildPrettyResourceBindingsJson(
    const std::string& passName,
    const std::vector<ReflectedResourceBinding>& allBindings) {
    std::vector<ReflectedResourceBinding> bindings;
    for (const auto& binding : allBindings)
        if (!binding.hlslOnly) bindings.push_back(binding);
    std::string json = "{\n";
    json += "  \"pass\": \"" + EscapeJsonString(passName) + "\",\n";
    json += "  \"resources\": [\n";

    for (size_t i = 0; i < bindings.size(); i++) {
        const ReflectedResourceBinding& binding = bindings[i];
        json += "    {\n";
        json += "      \"name\": \"" + EscapeJsonString(binding.name) + "\",\n";
        json += "      \"type\": \"" + std::string(ResourceTypeToString(binding.type)) + "\",\n";
        json += "      \"set\": " + std::to_string(binding.set) + ",\n";
        json += "      \"binding\": " + std::to_string(binding.binding) + ",\n";
        json += "      \"stages\": " + StageFlagsToJsonArray(binding.stages, true) + ",\n";
        json += "      \"access\": \"" + std::string(ResourceAccessToString(binding.access)) + "\"";
        if (binding.combinedSampledImage) {
            json += ",\n      \"abi\": \"combined_sampled_image\"";
            if (!binding.combinedWith.empty()) {
                json += ",\n      \"combinedWith\": \"" + EscapeJsonString(binding.combinedWith) + "\"";
            }
        }
        if (binding.separateImageSampler)
            json += ",\n      \"abi\": \"" + std::string(binding.type == ::ResourceBinding::Texture ? "separate_image" : "sampler") + "\"";
        if (!binding.defaultSamplerFor.empty())
            json += ",\n      \"defaultSamplerFor\": \"" + EscapeJsonString(binding.defaultSamplerFor) + "\"";
        std::ostringstream uniforms;
        AppendCombinedSamplerUniformsJson(uniforms, binding.combinedSamplerUniforms);
        json += uniforms.str();
        json += "\n";
        json += "    }";
        if (i + 1 < bindings.size()) json += ",";
        json += "\n";
    }

    json += "  ]";
    std::ostringstream hlslResources;
    AppendHLSLResourcesJson(hlslResources, allBindings);
    json += hlslResources.str();
    json += "\n";
    json += "}\n";
    return json;
}

} // namespace BWSL::ReflectionJson
