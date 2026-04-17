// SPIRV-Cross Wrapper
// Compiles SPIRV-Cross separately to avoid macro conflicts with BWSL's defs.h
// (defs.h defines u32, f32, f64 as macros which conflict with SPIRV-Cross)

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

// Include SPIRV-Cross headers (before any BWSL headers)
#include "../vendor/SPIRV-Cross/spirv_msl.hpp"
#include "../vendor/SPIRV-Cross/spirv_hlsl.hpp"

// Unity build for SPIRV-Cross
#include "../vendor/SPIRV-Cross/spirv_cross.cpp"
#include "../vendor/SPIRV-Cross/spirv_parser.cpp"
#include "../vendor/SPIRV-Cross/spirv_cross_parsed_ir.cpp"
#include "../vendor/SPIRV-Cross/spirv_cfg.cpp"
#include "../vendor/SPIRV-Cross/spirv_glsl.cpp"
#include "../vendor/SPIRV-Cross/spirv_msl.cpp"
#include "../vendor/SPIRV-Cross/spirv_hlsl.cpp"

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
    };
    for (const char* kw : es310_only) {
        if (source.find(kw) != std::string::npos) {
            return std::string("error: GLSL ES ") + std::to_string(glslVersion) +
                   " does not support builtin '" + kw + ")' (requires ES 310+)";
        }
    }
    return {};
}

std::string CompileToGLSL(const std::vector<uint32_t>& spirv, int glslVersion, bool es) {
#ifdef SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
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
    if (auto err = CheckGLSLESEmittedCompat(result, glslVersion, es); !err.empty()) {
        return err;
    }
    return result;
#else
    try {
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

    return compiler.compile();
#else
    try {
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

        return compiler.compile();
    } catch (const spirv_cross::CompilerError& e) {
        return std::string("error: ") + e.what();
    } catch (const std::exception& e) {
        return std::string("error: ") + e.what();
    }
#endif
}

} // namespace spirv_cross_wrapper
