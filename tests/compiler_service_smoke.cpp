// Integration check using the same unity build as the CLI.
#define main bwsl_cli_main
#include "../src/bwslc.cpp"
#undef main
#include "core/bwsl_compiler_service_core.h"
#include <cassert>

int main(int argc, char** argv) {
    assert(argc == 2);
    std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "Good.bwsl") << "pipeline Good { pass \"Main\" { vertex { output.position=float4(1.0); } fragment { output.color=float4(2.0); } } }";
    std::ofstream(dir / "Bad.bwsl") << "pipeline Bad { pass \"Main\" { vertex { int[2] a; output.position=float4(a[1+2]); } } }";
    // Struct `t_atlas` clashes with t_atlas, the GL name of texture `atlas`.
    std::ofstream(dir / "GLNameClash.bwsl") << "pipeline GLNameClash { struct t_atlas { float x; }; resources { atlas: texture2D } pass \"Main\" { use resources { atlas } vertex { output.position=float4(1.0); } fragment { output.color=sample(resources.atlas, float2(0.5)); } } }";
    std::ofstream(dir / "Varyings.bwsl") << "pipeline Varyings { pass \"Main\" { vertex { output.position=float4(1.0); output.uv=float2(0.5); output.tint=float4(0.25); } fragment { output.color=input.tint+float4(input.uv,0.0,1.0); } } }";
    std::ofstream(dir / "VaryingNameClash.bwsl") << "pipeline VaryingNameClash { struct v_uv { float x; } pass \"Main\" { vertex { output.position=float4(1.0); output.uv=float2(0.5); } fragment { output.color=float4(input.uv,0.0,1.0); } } }";
    RenderConfig config;
    for (auto name : {"Good", "Bad", "GLNameClash", "Varyings", "VaryingNameClash",
                      "LoweringOverflow", "SSAOverflow"}) {
        RenderConfig::PassData pass;
        pass.name = "Main";
        pass.descriptor.pipelineName = name;
        config.passes.push_back(pass);
    }
    BWSL::BWSLCompilerServiceCore service;
    service.Initialize(config, 32 * 1024 * 1024, dir.string());
    auto* valid = service.GetOrCompileVariant("Good", "Main", 0);
    assert(valid && !valid->vertexSpirv.empty() && !valid->fragmentSpirv.empty());
    assert(service.GetOrCompileVariant("Good", "Main", 0) == valid);
    for (int i=0; i<2; ++i) assert(service.GetOrCompileVariant("Bad", "Main", 0) == nullptr);
    assert(service.GetOrCompileVariant("GLNameClash", "Main", 0) == nullptr);
    auto* varyings = service.GetOrCompileVariant("Varyings", "Main", 0);
    assert(varyings && !varyings->vertexSpirv.empty() && !varyings->fragmentSpirv.empty());
    auto hasName = [](const std::vector<u32>& words, const char* name) {
        for (size_t i = 5; i < words.size();) {
            u32 count = words[i] >> 16;
            if (count == 0 || i + count > words.size()) return false;
            if ((words[i] & 0xFFFF) == spv::OpName && count >= 3 &&
                strcmp(reinterpret_cast<const char*>(&words[i + 2]), name) == 0) return true;
            i += count;
        }
        return false;
    };
    for (const char* name : {"v_uv", "v_tint"}) {
        assert(hasName(varyings->vertexSpirv, name));
        assert(hasName(varyings->fragmentSpirv, name));
    }
    assert(service.GetOrCompileVariant("VaryingNameClash", "Main", 0) == nullptr);
    assert(service.GetOrCompileVariant("LoweringOverflow", "Main", 0) == nullptr);
    assert(service.GetOrCompileVariant("SSAOverflow", "Main", 0) == nullptr);
    service.HandleFileChange((dir / "Good.bwsl").string());
    assert(service.GetOrCompileVariant("Good", "Main", 0));
    std::puts("Compiler service valid/cache/reload/lowering-error/gl-name checks: PASS");
}
