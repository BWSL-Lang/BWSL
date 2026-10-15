// Reuse the production frontend and stage compiler, without CLI invocation,
// source reads, output files, validation subprocesses or a persistent AST cache.
#define main bwslc_cli_main
#include "../../src/bwslc.cpp"
#undef main

#include <iomanip>

namespace {

using BenchClock = std::chrono::steady_clock;

double ElapsedUs(BenchClock::time_point start) {
    return std::chrono::duration<double, std::micro>(BenchClock::now() - start).count();
}

struct Observation {
    double setupUs = 0;
    double lexerUs = 0;
    double parserInitUs = 0;
    double parseUs = 0;
    double resolveUs = 0;
    double variantsUs = 0;
    double comptimeUs = 0;
    double specializeUs = 0;
    double stagesUs = 0;
    double loweringUs = 0;
    double ssaUs = 0;
    double emitUs = 0;
    double totalUs = 0;
    size_t sourceBytes = 0;
    size_t arenaBytes = 0;
    std::vector<std::pair<std::string, std::string>> outputs;
};

Observation CompileSource(const std::string& source, const std::string& name, bool reflection) {
    Observation observation;
    const BenchClock::time_point start = BenchClock::now();
    {
        BenchClock::time_point phase = BenchClock::now();
        const u32 estimatedNodes = static_cast<u32>(std::clamp<size_t>(source.size() / 8, 256, 2048));
        CompilationContext context(estimatedNodes);
        TokenStream stream;
        stream.Init(&context.arena, source.c_str(), source.size());
        observation.setupUs = ElapsedUs(phase);
        phase = BenchClock::now();
        Lexer lexer(source, stream);
        lexer.Tokenize();
        observation.lexerUs = ElapsedUs(phase);
        phase = BenchClock::now();
        Parser parser;
        parser.Init(&lexer, &stream, &context);
        parser.currentSourceName = name;
        observation.parserInitUs = ElapsedUs(phase);
        phase = BenchClock::now();
        parser.ParseDocument();
        observation.parseUs = ElapsedUs(phase);
        if (parser.hadError || !context.root.IsValid()) {
            throw std::runtime_error("Frontend failed: " + name);
        }
        phase = BenchClock::now();
        std::string error;
        if (!parser.ResolveVariants(context.root, &error)) {
            throw std::runtime_error(error);
        }
        observation.variantsUs = ElapsedUs(phase);
        const BenchClock::time_point comptimeStart = BenchClock::now();
        if (!Comptime::RunComptimeInterpreter(&context, &parser, context.root, &error)) {
            throw std::runtime_error(error);
        }
        observation.comptimeUs = ElapsedUs(comptimeStart);
        parser.ResolveShaderStages(context.root);
        VariantSelectionData selection;
        const std::vector<VariantOverride> overrides;
        if (!parser.BuildVariantSelection(context.root, nullptr, 0, false, overrides, &selection, &error)) {
            throw std::runtime_error(error);
        }
        const BenchClock::time_point specializeStart = BenchClock::now();
        const NodeRef pipelineRef = parser.SpecializePipelineForVariants(context.root, selection, &error);
        observation.specializeUs = ElapsedUs(specializeStart);
        if (parser.hadError || pipelineRef.IsNull()) {
            throw std::runtime_error(error);
        }
        observation.resolveUs = ElapsedUs(phase);
        phase = BenchClock::now();
        const PipelineData& pipeline = context.ast.GetPipeline(pipelineRef);
        for (u32 passIndex = 0; passIndex < pipeline.passes.count; ++passIndex) {
            const NodeRef passRef = pipeline.passes[passIndex];
            const PassData& pass = context.ast.GetPass(passRef);
            PassVaryingContext varyings;
            for (const ShaderStage stage : {ShaderStage::Vertex, ShaderStage::Fragment, ShaderStage::Compute}) {
                const NodeRef shader = stage == ShaderStage::Vertex ? pass.vertexShader :
                                       stage == ShaderStage::Fragment ? pass.fragmentShader : pass.computeShader;
                if (shader.IsNull()) {
                    continue;
                }
                CompileResult result = CompileShaderStage(
                    context, parser, pass, stage, false, false, false, true, false,
                    pipelineRef, passRef, &varyings, false, false, false, nullptr,
                    nullptr, "", true, true, false, reflection);
                if (!result.success) {
                    throw std::runtime_error(result.error);
                }
                observation.loweringUs += result.timing.irLoweringMs * 1000;
                observation.ssaUs += result.timing.cfgSsaMs * 1000;
                observation.emitUs += result.timing.wgslEmitMs * 1000;
                observation.sourceBytes += result.wgslSource.size();
                observation.outputs.emplace_back(std::move(result.wgslSource), std::move(result.wgslBindingsJson));
            }
        }
        observation.stagesUs = ElapsedUs(phase);
        observation.arenaBytes = context.arena.allocated;
    }
    // Include destruction of all per-compilation compiler arenas and state.
    observation.totalUs = ElapsedUs(start);
    return observation;
}

int ServeSource(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot read worker source");
    }
    const std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::cout << "{\"ready\":true}\n" << std::flush;
    std::string request;
    while (std::getline(std::cin, request)) {
        if (request == "quit") {
            break;
        }
        if (request != "compile") {
            throw std::runtime_error("Unknown worker request");
        }
        const Observation result = CompileSource(source, path, true);
        std::cout << std::setprecision(12) << "{\"native_us\":" << result.totalUs
                  << ",\"phases_us\":{\"setup\":" << result.setupUs << ",\"lexer\":" << result.lexerUs
                  << ",\"parse\":" << result.parseUs << ",\"resolve\":" << result.resolveUs
                  << ",\"lowering\":" << result.loweringUs << ",\"ssa\":" << result.ssaUs
                  << ",\"emit\":" << result.emitUs << "},\"outputs\":[";
        for (size_t index = 0; index < result.outputs.size(); ++index) {
            std::cout << (index ? "," : "") << "{\"source\":\"" << EscapeJsonString(result.outputs[index].first)
                      << "\",\"bindings\":\"" << EscapeJsonString(result.outputs[index].second) << "\"}";
        }
        std::cout << "]}\n" << std::flush;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--serve") {
            return ServeSource(argv[2]);
        }
        if (argc < 4 || argc > 6) {
            throw std::runtime_error("Usage: wgsl-inprocess SOURCE RUNS WARMUP [OUTPUT_DIR] [no-reflection]");
        }
        const unsigned long runs = std::stoul(argv[2]);
        const unsigned long warmup = std::stoul(argv[3]);
        if (runs == 0 || runs > 100000 || warmup > 100000) {
            throw std::runtime_error("Invalid sample count");
        }
        std::ifstream file(argv[1], std::ios::binary);
        if (!file) {
            throw std::runtime_error("Cannot read source");
        }
        const std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const bool reflection = argc < 6 || std::string_view(argv[5]) != "no-reflection";
        std::vector<Observation> observations;
        observations.reserve(runs);
        Observation reference = CompileSource(source, argv[1], reflection);
        for (unsigned long iteration = 0; iteration < runs + warmup; ++iteration) {
            Observation current = CompileSource(source, argv[1], reflection);
            if (current.outputs != reference.outputs) {
                throw std::runtime_error("Non-deterministic source or reflection");
            }
            current.outputs.clear();
            if (iteration >= warmup) {
                observations.push_back(std::move(current));
            }
        }
        if (argc >= 5 && std::string_view(argv[4]) != "-") {
            const std::filesystem::path directory(argv[4]);
            std::filesystem::create_directories(directory);
            for (size_t index = 0; index < reference.outputs.size(); ++index) {
                std::ofstream(directory / (std::to_string(index) + ".wgsl")) << reference.outputs[index].first;
                std::ofstream(directory / (std::to_string(index) + ".json")) << reference.outputs[index].second;
            }
        }
        std::cout << std::setprecision(12)
                  << "{\"schema_version\":1,\"mode\":\"fresh-source-in-process\",\"reflection\":"
                  << (reflection ? "true" : "false") << ",\"source_bytes\":" << source.size()
                  << ",\"wgsl_bytes\":" << reference.sourceBytes << ",\"arena_bytes\":" << reference.arenaBytes
                  << ",\"samples\":[";
        for (size_t index = 0; index < observations.size(); ++index) {
            const Observation& sample = observations[index];
            std::cout << (index ? "," : "") << "{\"setup_us\":" << sample.setupUs
                      << ",\"lexer_us\":" << sample.lexerUs << ",\"parser_init_us\":" << sample.parserInitUs
                      << ",\"parse_us\":" << sample.parseUs << ",\"resolve_us\":" << sample.resolveUs
                      << ",\"variants_us\":" << sample.variantsUs << ",\"comptime_us\":" << sample.comptimeUs
                      << ",\"specialize_us\":" << sample.specializeUs
                      << ",\"stages_us\":" << sample.stagesUs << ",\"lowering_us\":" << sample.loweringUs
                      << ",\"ssa_us\":" << sample.ssaUs << ",\"emit_us\":" << sample.emitUs
                      << ",\"total_us\":" << sample.totalUs << "}";
        }
        std::cout << "]}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
