// SPIRV-Cross Wrapper
// Compiles SPIRV-Cross separately to avoid macro conflicts with BWSL's defs.h
// (defs.h defines u32, f32, f64 as macros which conflict with SPIRV-Cross)

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

// Include SPIRV-Cross headers (before any BWSL headers)
#include "SPIRV-Cross/spirv_msl.hpp"
#include "SPIRV-Cross/spirv_hlsl.hpp"

// Unity build for SPIRV-Cross
#include "SPIRV-Cross/spirv_cross.cpp"
#include "SPIRV-Cross/spirv_parser.cpp"
#include "SPIRV-Cross/spirv_cross_parsed_ir.cpp"
#include "SPIRV-Cross/spirv_cfg.cpp"
#include "SPIRV-Cross/spirv_glsl.cpp"
#include "SPIRV-Cross/spirv_msl.cpp"
#include "SPIRV-Cross/spirv_hlsl.cpp"

namespace spirv_cross_wrapper {

// Helper to add MSL resource bindings that preserve SPIR-V binding indices
// Note: Metal only allows sampler indices 0-15, so we remap sampler bindings
// to stay within this range while keeping texture bindings as-is.
static void preserveBindingIndices(spirv_cross::CompilerMSL& compiler, spv::ExecutionModel stage) {
    auto resources = compiler.get_shader_resources();
    uint32_t nextSamplerSlot = 0;  // Track sampler slots separately (0-15 max)

    // Helper lambda to add binding for a resource (non-sampler)
    auto addBinding = [&](const spirv_cross::Resource& res) {
        uint32_t set = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
        uint32_t binding = compiler.get_decoration(res.id, spv::DecorationBinding);

        spirv_cross::MSLResourceBinding mslBinding;
        mslBinding.stage = stage;
        mslBinding.desc_set = set;
        mslBinding.binding = binding;
        mslBinding.msl_buffer = binding;
        mslBinding.msl_texture = binding;
        mslBinding.msl_sampler = binding;  // Not used for non-sampler resources
        compiler.add_msl_resource_binding(mslBinding);
    };

    // Helper lambda to add binding for combined image sampler
    // Texture binding stays as-is, sampler binding is remapped to 0-15 range
    auto addSampledImageBinding = [&](const spirv_cross::Resource& res) {
        uint32_t set = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
        uint32_t binding = compiler.get_decoration(res.id, spv::DecorationBinding);

        spirv_cross::MSLResourceBinding mslBinding;
        mslBinding.stage = stage;
        mslBinding.desc_set = set;
        mslBinding.binding = binding;
        mslBinding.msl_buffer = binding;
        mslBinding.msl_texture = binding;
        // Remap sampler to sequential slots within 0-15 range
        mslBinding.msl_sampler = nextSamplerSlot < 16 ? nextSamplerSlot++ : 15;
        compiler.add_msl_resource_binding(mslBinding);
    };

    // Uniform buffers
    for (const auto& res : resources.uniform_buffers) {
        addBinding(res);
    }

    // Storage buffers
    for (const auto& res : resources.storage_buffers) {
        addBinding(res);
    }

    // Separate images (textures)
    for (const auto& res : resources.separate_images) {
        addBinding(res);
    }

    // Separate samplers - these also need remapped bindings
    for (const auto& res : resources.separate_samplers) {
        uint32_t set = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
        uint32_t binding = compiler.get_decoration(res.id, spv::DecorationBinding);

        spirv_cross::MSLResourceBinding mslBinding;
        mslBinding.stage = stage;
        mslBinding.desc_set = set;
        mslBinding.binding = binding;
        mslBinding.msl_buffer = binding;
        mslBinding.msl_texture = binding;
        mslBinding.msl_sampler = nextSamplerSlot < 16 ? nextSamplerSlot++ : 15;
        compiler.add_msl_resource_binding(mslBinding);
    }

    // Combined image samplers - texture binding preserved, sampler remapped
    for (const auto& res : resources.sampled_images) {
        addSampledImageBinding(res);
    }

    // Storage images
    for (const auto& res : resources.storage_images) {
        addBinding(res);
    }
}

static bool LiteralStringEquals(const std::vector<uint32_t>& words,
                                size_t start,
                                size_t end,
                                const char* expected) {
    size_t charIndex = 0;
    for (size_t i = start; i < end; i++) {
        uint32_t word = words[i];
        for (uint32_t byte = 0; byte < 4; byte++) {
            char c = static_cast<char>((word >> (byte * 8)) & 0xffu);
            char want = expected[charIndex];
            if (c != want) {
                return false;
            }
            if (c == '\0') {
                return true;
            }
            charIndex++;
        }
    }
    return expected[charIndex] == '\0';
}

static std::string CheckGLSLESSPIRVCompatRaw(const std::vector<uint32_t>& spirv,
                                             int glslVersion,
                                             bool es) {
    if (!es || spirv.size() < 5) return {};

    std::unordered_set<uint32_t> glslStd450Ids;
    for (size_t offset = 5; offset < spirv.size();) {
        uint32_t first = spirv[offset];
        uint16_t op = static_cast<uint16_t>(first & 0xffffu);
        uint16_t wordCount = static_cast<uint16_t>(first >> 16);
        if (wordCount == 0 || offset + wordCount > spirv.size()) {
            return "error: malformed SPIR-V instruction stream";
        }

        if (op == spv::OpExtInstImport && wordCount >= 3 &&
            LiteralStringEquals(spirv, offset + 2, offset + wordCount,
                                "GLSL.std.450")) {
            glslStd450Ids.insert(spirv[offset + 1]);
        } else if (op == spv::OpExtInst && wordCount >= 5) {
            uint32_t setId = spirv[offset + 3];
            uint32_t extOp = spirv[offset + 4];
            if (glslStd450Ids.count(setId) != 0) {
                if (extOp == GLSLstd450Ldexp) {
                    return "error: GLSL ES does not support GLSL.std.450 Ldexp; use the direct GLES ldexp fallback";
                }
                if (extOp == GLSLstd450ModfStruct) {
                    return "error: GLSL ES cannot cross-compile GLSL.std.450 ModfStruct; use the direct GLES modf fallback";
                }
            }
        } else if (op == spv::OpDPdxFine || op == spv::OpDPdyFine ||
                   op == spv::OpDPdxCoarse || op == spv::OpDPdyCoarse ||
                   op == spv::OpFwidthFine || op == spv::OpFwidthCoarse) {
            return "error: GLSL ES does not support fine/coarse derivative opcodes; use the direct GLES derivative fallback";
        } else if (glslVersion < 310 && op == spv::OpImageQueryLevels) {
            return "error: GLSL ES 300 does not support textureQueryLevels";
        } else if (glslVersion < 310 && op == spv::OpImageGather &&
                   wordCount >= 7) {
            uint32_t imageOperands = spirv[offset + 6];
            if ((imageOperands & spv::ImageOperandsOffsetMask) != 0) {
                return "error: GLSL ES 300 does not support textureGatherOffset";
            }
        }

        offset += wordCount;
    }
    return {};
}

std::string CompileToMSL(const std::vector<uint32_t>& spirv) {
#ifdef SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
    // No exceptions - just compile (will abort on error)
    spirv_cross::CompilerMSL compiler(spirv);
    spirv_cross::CompilerMSL::Options mslOpts;
    mslOpts.platform = spirv_cross::CompilerMSL::Options::macOS;
    mslOpts.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 0);
    compiler.set_msl_options(mslOpts);

    // Preserve SPIR-V binding indices in Metal output
    auto entry = compiler.get_entry_points_and_stages()[0];
    preserveBindingIndices(compiler, entry.execution_model);

    return compiler.compile();
#else
    try {
        spirv_cross::CompilerMSL compiler(spirv);
        spirv_cross::CompilerMSL::Options mslOpts;
        mslOpts.platform = spirv_cross::CompilerMSL::Options::macOS;
        mslOpts.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 0);
        compiler.set_msl_options(mslOpts);

        // Preserve SPIR-V binding indices in Metal output
        auto entry = compiler.get_entry_points_and_stages()[0];
        preserveBindingIndices(compiler, entry.execution_model);

        return compiler.compile();
    } catch (const spirv_cross::CompilerError& e) {
        return std::string("error: ") + e.what();
    } catch (const std::exception& e) {
        return std::string("error: ") + e.what();
    }
#endif
}

std::string CompileToHLSL(const std::vector<uint32_t>& spirv, int shaderModel) {
#ifdef SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
    spirv_cross::CompilerHLSL compiler(spirv);
    spirv_cross::CompilerHLSL::Options hlslOpts;
    hlslOpts.shader_model = shaderModel;
    compiler.set_hlsl_options(hlslOpts);
    return compiler.compile();
#else
    try {
        spirv_cross::CompilerHLSL compiler(spirv);
        spirv_cross::CompilerHLSL::Options hlslOpts;
        hlslOpts.shader_model = shaderModel;
        compiler.set_hlsl_options(hlslOpts);
        return compiler.compile();
    } catch (const spirv_cross::CompilerError& e) {
        return std::string("error: ") + e.what();
    } catch (const std::exception& e) {
        return std::string("error: ") + e.what();
    }
#endif
}

// Returns non-empty error string if the SPIR-V module uses features that
// require GLSL ES 3.10+ when the target is below 310.
static std::string CheckGLSLESCompat(spirv_cross::CompilerGLSL& compiler,
                                     int glslVersion, bool es) {
    if (!es || glslVersion >= 310) return {};
    auto resources = compiler.get_shader_resources();
    if (!resources.storage_buffers.empty()) {
        return "error: GLSL ES " + std::to_string(glslVersion) +
               " does not support storage buffers (requires ES 310+)";
    }
    if (!resources.storage_images.empty()) {
        return "error: GLSL ES " + std::to_string(glslVersion) +
               " does not support storage images (requires ES 310+)";
    }
    for (const auto& ep : compiler.get_entry_points_and_stages()) {
        if (ep.execution_model == spv::ExecutionModelGLCompute) {
            return "error: GLSL ES " + std::to_string(glslVersion) +
                   " does not support compute shaders (requires ES 310+)";
        }
    }
    return {};
}

// Inspects emitted GLSL ES source for builtins that require ES 310+.
static std::string CheckGLSLESEmittedCompat(const std::string& source,
                                            int glslVersion, bool es) {
    if (!es || glslVersion >= 310) return {};
    static const char* es310_only[] = {
        "findMSB(", "findLSB(", "bitCount(", "bitfieldReverse(",
        "bitfieldExtract(", "bitfieldInsert(",
        "uaddCarry(", "usubBorrow(", "umulExtended(", "imulExtended(",
        "ldexp(",
    };
    for (const char* kw : es310_only) {
        if (source.find(kw) != std::string::npos) {
            return std::string("error: GLSL ES ") + std::to_string(glslVersion) +
                   " does not support builtin '" + kw + ")' (requires ES 310+)";
        }
    }
    return {};
}

static bool IsGLSLIdentChar(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

static bool ReplaceFunctionCalls(std::string& source, const char* from, const char* to) {
    bool replaced = false;
    size_t pos = 0;
    const size_t fromLen = std::char_traits<char>::length(from);
    const size_t toLen = std::char_traits<char>::length(to);
    while ((pos = source.find(from, pos)) != std::string::npos) {
        if (pos > 0 && IsGLSLIdentChar(source[pos - 1])) {
            pos += fromLen;
            continue;
        }
        source.replace(pos, fromLen, to);
        pos += toLen;
        replaced = true;
    }
    return replaced;
}

static size_t GLSLESPolyfillInsertionPoint(const std::string& source) {
    size_t pos = 0;
    if (source.rfind("#version", 0) == 0) {
        size_t end = source.find('\n', pos);
        pos = (end == std::string::npos) ? source.size() : end + 1;
    }

    while (pos < source.size()) {
        size_t end = source.find('\n', pos);
        size_t lineEnd = (end == std::string::npos) ? source.size() : end;
        std::string_view line(source.data() + pos, lineEnd - pos);
        if (line.empty() ||
            line.rfind("#", 0) == 0 ||
            line.rfind("precision ", 0) == 0) {
            pos = (end == std::string::npos) ? source.size() : end + 1;
            continue;
        }
        break;
    }
    return pos;
}

static void PatchGLSLES300Packing4x8Builtins(std::string& source,
                                             int glslVersion,
                                             bool es) {
    if (!es || glslVersion >= 310) return;

    bool needsPolyfill = false;
    needsPolyfill |= ReplaceFunctionCalls(source, "unpackUnorm4x8(", "bwsl_unpackUnorm4x8(");
    needsPolyfill |= ReplaceFunctionCalls(source, "unpackSnorm4x8(", "bwsl_unpackSnorm4x8(");
    needsPolyfill |= ReplaceFunctionCalls(source, "packUnorm4x8(", "bwsl_packUnorm4x8(");
    needsPolyfill |= ReplaceFunctionCalls(source, "packSnorm4x8(", "bwsl_packSnorm4x8(");
    if (!needsPolyfill) return;

    static const char* polyfill =
        "uint bwsl_packUnorm4x8(vec4 v) {\n"
        "    uvec4 u = uvec4(round(clamp(v, vec4(0.0), vec4(1.0)) * 255.0));\n"
        "    return (u.x & 255u) | ((u.y & 255u) << 8) | ((u.z & 255u) << 16) | ((u.w & 255u) << 24);\n"
        "}\n\n"
        "vec4 bwsl_unpackUnorm4x8(uint p) {\n"
        "    uvec4 u = uvec4(p & 255u, (p >> 8) & 255u, (p >> 16) & 255u, (p >> 24) & 255u);\n"
        "    return vec4(u) / 255.0;\n"
        "}\n\n"
        "uint bwsl_packSnorm4x8(vec4 v) {\n"
        "    ivec4 i = ivec4(round(clamp(v, vec4(-1.0), vec4(1.0)) * 127.0));\n"
        "    uvec4 u = uvec4((i + ivec4(256)) & ivec4(255));\n"
        "    return (u.x & 255u) | ((u.y & 255u) << 8) | ((u.z & 255u) << 16) | ((u.w & 255u) << 24);\n"
        "}\n\n"
        "int bwsl_unpackSnorm8(uint v) {\n"
        "    int i = int(v & 255u);\n"
        "    return (i > 127) ? (i - 256) : i;\n"
        "}\n\n"
        "vec4 bwsl_unpackSnorm4x8(uint p) {\n"
        "    ivec4 i = ivec4(bwsl_unpackSnorm8(p), bwsl_unpackSnorm8(p >> 8), bwsl_unpackSnorm8(p >> 16), bwsl_unpackSnorm8(p >> 24));\n"
        "    return max(vec4(i) / 127.0, vec4(-1.0));\n"
        "}\n\n";
    source.insert(GLSLESPolyfillInsertionPoint(source), polyfill);
}

std::string CompileToGLSL(const std::vector<uint32_t>& spirv, int glslVersion, bool es) {
#ifdef SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
    if (auto err = CheckGLSLESSPIRVCompatRaw(spirv, glslVersion, es); !err.empty()) {
        return err;
    }
    spirv_cross::CompilerGLSL compiler(spirv);
    if (auto err = CheckGLSLESCompat(compiler, glslVersion, es); !err.empty()) {
        return err;
    }
    spirv_cross::CompilerGLSL::Options glslOpts;
    glslOpts.version = glslVersion;
    glslOpts.es = es;
    glslOpts.vulkan_semantics = false;
    glslOpts.separate_shader_objects = true;
    compiler.set_common_options(glslOpts);
    std::string result = compiler.compile();
    PatchGLSLES300Packing4x8Builtins(result, glslVersion, es);
    if (auto err = CheckGLSLESEmittedCompat(result, glslVersion, es); !err.empty()) {
        return err;
    }
    return result;
#else
    try {
        if (auto err = CheckGLSLESSPIRVCompatRaw(spirv, glslVersion, es); !err.empty()) {
            return err;
        }
        spirv_cross::CompilerGLSL compiler(spirv);
        if (auto err = CheckGLSLESCompat(compiler, glslVersion, es); !err.empty()) {
            return err;
        }
        spirv_cross::CompilerGLSL::Options glslOpts;
        glslOpts.version = glslVersion;
        glslOpts.es = es;
        glslOpts.vulkan_semantics = false;
        glslOpts.separate_shader_objects = true;
        compiler.set_common_options(glslOpts);
        std::string result = compiler.compile();
        PatchGLSLES300Packing4x8Builtins(result, glslVersion, es);
        if (auto err = CheckGLSLESEmittedCompat(result, glslVersion, es); !err.empty()) {
            return err;
        }
        return result;
    } catch (const spirv_cross::CompilerError& e) {
        return std::string("error: ") + e.what();
    } catch (const std::exception& e) {
        return std::string("error: ") + e.what();
    }
#endif
}

// Compile to GLSL with varying name remapping
// varyingNames maps location -> desired name (e.g., {0: "v_position", 1: "v_normal"})
// isVertex: true for vertex shader (rename outputs), false for fragment (rename inputs)
std::string CompileToGLSLWithVaryings(
    const std::vector<uint32_t>& spirv,
    int glslVersion,
    bool es,
    const std::unordered_map<uint32_t, std::string>& varyingNames,
    bool isVertex)
{
#ifdef SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
    if (auto err = CheckGLSLESSPIRVCompatRaw(spirv, glslVersion, es); !err.empty()) {
        return err;
    }
    spirv_cross::CompilerGLSL compiler(spirv);
    spirv_cross::CompilerGLSL::Options glslOpts;
    glslOpts.version = glslVersion;
    glslOpts.es = es;
    glslOpts.vulkan_semantics = false;
    glslOpts.separate_shader_objects = true;
    compiler.set_common_options(glslOpts);

    // Rename stage inputs/outputs based on location
    auto resources = compiler.get_shader_resources();
    auto& interfaceVars = isVertex ? resources.stage_outputs : resources.stage_inputs;

    for (const auto& var : interfaceVars) {
        uint32_t location = compiler.get_decoration(var.id, spv::DecorationLocation);
        auto it = varyingNames.find(location);
        if (it != varyingNames.end()) {
            compiler.set_name(var.id, it->second);
        }
    }

    std::string result = compiler.compile();
    if (auto err = CheckGLSLESEmittedCompat(result, glslVersion, es); !err.empty()) {
        return err;
    }
    return result;
#else
    try {
        if (auto err = CheckGLSLESSPIRVCompatRaw(spirv, glslVersion, es); !err.empty()) {
            return err;
        }
        spirv_cross::CompilerGLSL compiler(spirv);
        spirv_cross::CompilerGLSL::Options glslOpts;
        glslOpts.version = glslVersion;
        glslOpts.es = es;
        glslOpts.vulkan_semantics = false;
        glslOpts.separate_shader_objects = true;
        compiler.set_common_options(glslOpts);

        // Rename stage inputs/outputs based on location
        auto resources = compiler.get_shader_resources();
        auto& interfaceVars = isVertex ? resources.stage_outputs : resources.stage_inputs;

        for (const auto& var : interfaceVars) {
            uint32_t location = compiler.get_decoration(var.id, spv::DecorationLocation);
            auto it = varyingNames.find(location);
            if (it != varyingNames.end()) {
                compiler.set_name(var.id, it->second);
            }
        }

        std::string result = compiler.compile();
        if (auto err = CheckGLSLESEmittedCompat(result, glslVersion, es); !err.empty()) {
            return err;
        }
        return result;
    } catch (const spirv_cross::CompilerError& e) {
        return std::string("error: ") + e.what();
    } catch (const std::exception& e) {
        return std::string("error: ") + e.what();
    }
#endif
}

} // namespace spirv_cross_wrapper
