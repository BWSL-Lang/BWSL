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
    RenderConfig config;
    for (auto name : {"Good", "Bad"}) {
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
    service.HandleFileChange((dir / "Good.bwsl").string());
    assert(service.GetOrCompileVariant("Good", "Main", 0));
    std::puts("Compiler service valid/cache/reload/lowering-error checks: PASS");
}
