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

std::string CompileToMSL(const std::vector<uint32_t>& spirv) {
#ifdef SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
    // No exceptions - just compile (will abort on error)
    spirv_cross::CompilerMSL compiler(spirv);
    spirv_cross::CompilerMSL::Options mslOpts;
    mslOpts.platform = spirv_cross::CompilerMSL::Options::macOS;
    mslOpts.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 0);
    compiler.set_msl_options(mslOpts);
    return compiler.compile();
#else
    try {
        spirv_cross::CompilerMSL compiler(spirv);
        spirv_cross::CompilerMSL::Options mslOpts;
        mslOpts.platform = spirv_cross::CompilerMSL::Options::macOS;
        mslOpts.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 0);
        compiler.set_msl_options(mslOpts);
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

std::string CompileToGLSL(const std::vector<uint32_t>& spirv, int glslVersion, bool es) {
#ifdef SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
    spirv_cross::CompilerGLSL compiler(spirv);
    spirv_cross::CompilerGLSL::Options glslOpts;
    glslOpts.version = glslVersion;
    glslOpts.es = es;
    glslOpts.vulkan_semantics = false;
    glslOpts.separate_shader_objects = true;
    compiler.set_common_options(glslOpts);
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
        return compiler.compile();
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
