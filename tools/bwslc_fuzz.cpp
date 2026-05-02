// libFuzzer harness for bwslc. Feeds raw bytes as BWSL source through the
// whole lex → parse → IR lowering → CFG/SSA → SPIR-V pipeline. Cross-compile
// to text backends (Metal/HLSL/GLSL/GLES) is intentionally skipped here —
// those are exercised by the regression / equivalence suites and would add
// link-time complexity without much fuzzing signal.
//
// ASan + UBSan catch memory and UB bugs; libFuzzer's coverage instrumentation
// guides mutation toward new code paths.
//
// Build:  make bwslc-fuzz
// Run:    ./build/bwslc-fuzz -max_len=4096 -timeout=5 \
//                -dict=fuzz/bwsl.dict \
//                -artifact_prefix=fuzz/crashes/ \
//                fuzz/corpus/

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <fcntl.h>

#include "bwsl_defs.h"
#include "bwsl_arena.h"
#include "bwsl_mem_pool.h"
#include "bwsl_render_config.h"
#include "bwsl_parser_soa.h"
#include "bwsl_lexer.h"
#include "bwsl_ir_gen.h"
#include "bwsl_ir_lowering.h"
#include "bwsl_cfg.h"
#include "bwsl_ssa.h"
#include "bwsl_ir_analysis.h"
#include "bwsl_spirv_backend.h"

#include "../phases/lexing/bwsl_lexer.cpp"
#include "../phases/parser/bwsl_parser_soa.cpp"
#include "../phases/evaluation/bwsl_eval_soa.cpp"
#include "../phases/evaluation/bwsl_comptime_interpreter.cpp"
#include "../core/bwsl_module_cache.cpp"
#include "../core/bwsl_custom_type_registry.cpp"
#include "../core/bwsl_variant_system.cpp"
#include "../phases/ir_generation/bwsl_ir_gen.cpp"
#include "../phases/ir_generation/bwsl_ir_analysis.cpp"
#include "../phases/control_flow/bwsl_cfg.cpp"
#include "../phases/ssa/bwsl_ssa.cpp"
#include "../phases/backends/spirv/bwsl_spirv_backend.cpp"

using namespace BWSL;
using namespace BWSL::IR;

// Redirect stdout+stderr to /dev/null only for the duration of each compile.
// Global redirection would also hide libFuzzer's own progress / crash output.
struct ScopedSilence {
    int saved_out = -1;
    int saved_err = -1;
    int devnull = -1;
    ScopedSilence() {
        fflush(stdout); fflush(stderr);
        saved_out = dup(STDOUT_FILENO);
        saved_err = dup(STDERR_FILENO);
        devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
    }
    ~ScopedSilence() {
        fflush(stdout); fflush(stderr);
        if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
        if (saved_err >= 0) { dup2(saved_err, STDERR_FILENO); close(saved_err); }
        if (devnull >= 0) close(devnull);
    }
};

// Run the full compile pipeline on one shader stage. Returns silently on any
// expected failure (missing stage, empty body); crashes / ASan / UBSan
// violations propagate to libFuzzer naturally.
static void CompileOneStage(CompilationContext& context, Parser& parser,
                            NodeRef pipelineRef, NodeRef passRef,
                            const PassData& pass, ShaderStage stage,
                            NodeRef shaderRef) {
    if (shaderRef.IsNull()) return;
    const ShaderStageData& stageData = context.ast.GetShaderStage(shaderRef);
    if (stageData.body.IsNull()) return;

    IRMemoryPool irPool;
    IRLowering lowering;
    lowering.Initialize(&irPool,
                        const_cast<SymbolTableData*>(&parser.symbolTable),
                        &context.ast, parser.sourceBase());
    lowering.currentStage = stage;
    lowering.currentPipeline = pipelineRef;
    lowering.currentPass = passRef;

    for (u32 i = 0; i < pass.consts.count; i++) {
        lowering.LowerStatement(pass.consts[i]);
    }

    const BlockData& block = context.ast.GetBlock(stageData.body);
    for (u32 i = 0; i < block.statements.count; i++) {
        lowering.LowerStatement(block.statements[i]);
    }

    // CFG + SSA (only if control flow is present — matches bwslc's behavior).
    // Static scratch buffers reused across iterations; each iteration calls
    // Initialize() which resets the arena bookkeeping.
    static char cfgMem[1 * 1024 * 1024];
    Memory::BWEMemoryArena cfgArena;
    cfgArena.Initialize(cfgMem, sizeof(cfgMem));
    CFGBuilder cfgBuilder;
    CFG* cfgPtr = nullptr;

    bool hasControlFlow = false;
    for (u32 i = 0; i < lowering.program.instructionCount; i++) {
        u16 op = lowering.program.opcodes[i];
        if (op == OP_BRANCH || op == OP_JUMP || op == OP_SWITCH) {
            hasControlFlow = true;
            break;
        }
    }
    if (hasControlFlow) {
        cfgBuilder.Init(&lowering.program, &cfgArena);
        cfgBuilder.Build();
        cfgPtr = &cfgBuilder.cfg;
        if (cfgBuilder.cfg.blockCount > 1) {
            SSA::ConvertToSSA(&lowering.program, &cfgBuilder.cfg, &cfgBuilder,
                              &cfgArena);
        }
    }

    // SPIR-V emit. 16 MB covers pathological fuzz inputs that emit tens of
    // thousands of IR instructions; real shaders use well under 1 MB. The
    // arena is reused across iterations via reset-by-reinitialize.
    static char spirvMem[16 * 1024 * 1024];
    Memory::BWEMemoryArena spirvArena;
    spirvArena.Initialize(spirvMem, sizeof(spirvMem));

    SPIRVBuilder builder;
    builder.Initialize(&spirvArena, &lowering.program, stage,
                       const_cast<SymbolTableData*>(&parser.symbolTable),
                       cfgPtr);
    if (stage == ShaderStage::Compute) {
        builder.SetComputeWorkgroupSize(stageData.workgroupSizeX,
                                        stageData.workgroupSizeY,
                                        stageData.workgroupSizeZ);
    }
    builder.EmitFunction();
    (void) builder.Finalize();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Skip empty and pathologically large inputs. 32 KiB is well above any
    // real shader and keeps each iteration fast.
    if (size == 0 || size > 32 * 1024) return 0;

    ScopedSilence silence;

    std::string source(reinterpret_cast<const char*>(data), size);

    CompilationContext context;
    TokenStream stream;
    stream.Init(&context.arena, source.c_str(), source.length());

    Lexer lexer(source, stream);
    lexer.Tokenize();

    Parser parser;
    parser.Init(&lexer, &stream, &context);

    bool isModule = (parser.CurrentTokenType() == TokenType::MODULE);
    if (isModule) {
        (void) parser.ParseModuleFile();
        return 0;
    }
    (void) parser.ParsePipeline();

    if (parser.hadError) return 0;
    if (context.ast.pipelines.count == 0) return 0;

    // Only the first pipeline is compiled — the point is to cover backend
    // paths, not reproduce the full CLI driver. All three shader stages
    // (vertex, fragment, compute) that exist get lowered and emitted.
    NodeRef pipelineRef = context.root;
    std::string variantResolveError;
    if (!parser.ResolveVariants(pipelineRef, &variantResolveError)) return 0;
    std::string comptimeError;
    if (!BWSL::Comptime::RunComptimeInterpreter(&context, &parser, pipelineRef, &comptimeError)) return 0;
    parser.ResolveShaderStages(pipelineRef);

    const PipelineData& pipeline = context.ast.GetPipeline(pipelineRef);
    for (u32 p = 0; p < pipeline.passes.count; p++) {
        NodeRef passRef = pipeline.passes[p];
        const PassData& pass = context.ast.GetPass(passRef);
        CompileOneStage(context, parser, pipelineRef, passRef, pass,
                        ShaderStage::Vertex, pass.vertexShader);
        CompileOneStage(context, parser, pipelineRef, passRef, pass,
                        ShaderStage::Fragment, pass.fragmentShader);
        CompileOneStage(context, parser, pipelineRef, passRef, pass,
                        ShaderStage::Compute, pass.computeShader);
    }

    return 0;
}
