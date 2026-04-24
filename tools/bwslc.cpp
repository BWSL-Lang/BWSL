// BWSL Compiler - Command Line Tool
// BWSL (Brawl Shader Language) is made by Alexander Presthus
// Compiles .bwsl shader files to SPIR-V, Metal, and HLSL
//
// Usage: bwslc <input.bwsl> [options]
//
// Options:
//   -o <dir>       Output directory (default: current directory)
//   -modules <dir> Add module search path (can be specified multiple times)
//   -pass <name>   Compile specific pass (default: all passes)
//   -stage <name>  Compile specific stage: vertex, fragment, compute (default: all)
//   -format <fmt>  Output format: spv, metal, hlsl, all (default: all)
//   -v             Verbose output
//   -h, --help     Show help

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <future>
#include <thread>

// SPIRV-Cross for in-process cross-compilation (much faster than CLI)
// Must be included as a separate compilation unit due to macro conflicts with defs.h
// (defs.h defines u32, f32, f64 as macros which conflict with SPIRV-Cross variable names)
#ifdef USE_SPIRV_CROSS_LIB
// Forward declaration - implemented in separate compilation unit to avoid macro conflicts
namespace spirv_cross_wrapper {
    std::string CompileToMSL(const std::vector<uint32_t>& spirv);
    std::string CompileToHLSL(const std::vector<uint32_t>& spirv, int shaderModel);
    std::string CompileToGLSL(const std::vector<uint32_t>& spirv, int glslVersion, bool es);
}
#endif

#include "../bwsl_spirv_backend.h"
#include "../bwsl_ir_gen.h"
#include "../bwsl_ir_lowering.h"
#include "../bwsl_ir_analysis.h"
#include "../bwsl_cfg.h"
#include "../bwsl_ssa.h"
#include "../bwsl_parser_soa.h"
#include "../bwsl_resource_reflection.h"
#include "../bwsl_lexer.h"
#include "../bwsl_eval_soa.h"
#include "../bwsl_variant_system.h"
#include "../bwsl_arena.h"
#include "../bwsl_mem_pool.h"
#include "../bwsl_render_config.h"
#include "../bwsl_compute_graph.h"

// Unity build: include all implementation files
#include "../bwsl_lexer.cpp"
// #define BWSL_PARSER_TIMING  // Enable parser timing instrumentation (disabled for benchmarking)
#include "../bwsl_parser_soa.cpp"
#include "../bwsl_eval_soa.cpp"
#include "../bwsl_module_cache.cpp"
#include "../bwsl_ir_gen.cpp"
#include "../bwsl_ir_analysis.cpp"
#include "../bwsl_cfg.cpp"
#include "../bwsl_ssa.cpp"
#include "../bwsl_spirv_backend.cpp"
#include "../bwsl_gles_backend.cpp"
#include "../bwsl_compute_graph.cpp"
#include "../bwsl_custom_type_registry.cpp"
#include "../bwsl_variant_system.cpp"

// Note: SPIRV-Cross is compiled separately (spirv_cross_wrapper.cpp) to avoid
// macro conflicts with defs.h (u32, f32, f64 macros)

namespace fs = std::filesystem;

using namespace BWSL;
using namespace BWSL::IR;

#define VERSION "0.4.0"

// ============= Configuration =============

enum class CompilerFlags {
    NONE         = 0,
    VERBOSE      = 1 << 0,
    DUMP_IR      = 1 << 1,
    OUTPUT_SPIRV = 1 << 2,
    OUTPUT_METAL = 1 << 3,
    OUTPUT_HLSL  = 1 << 4,
};
struct CompilerConfig {
    std::string inputFile;
    std::string outputDir = ".";
    std::vector<std::string> modulePaths;  // Additional module search paths
    std::string renderConfigPath;          // if using external render config, if empty will use default
    RenderConfig renderConfig;             // the render config to use
    RenderConfigParser::GeometryConfig geometryConfig;  // Geometry configuration (instancing, etc.)
    std::string passFilter;                // Empty = all passes
    std::string stageFilter;               // Empty = all stages
    bool outputSpirv = true;               // SPIR-V always generated
    bool outputMetal = false;              // Metal output (requires -metal flag)
    bool outputHlsl  = false;              // HLSL output (requires -hlsl flag)
    bool outputGlsl  = false;              // GLSL output (requires -glsl flag)
    bool outputGlslEs = false;             // GLSL ES output for WebGL/mobile (requires -gles flag)
    bool useDirectGles = false;            // Use direct IR→GLES backend (bypass SPIRV-Cross)
    bool verbose     = false;
    bool dumpIr      = false;
    bool debugNames  = false;              // Emit debug names in SPIR-V for easier debugging
    bool showTiming  = false;              // Print timing information
    bool skipValidation = false;           // Skip SPIR-V validation (faster but less safe)
    bool outputInternals = false;          // Output IR dump + SPIR-V disassembly to JSON file
    bool outputBindings = false;           // Output resolved resource bindings JSON
    bool dumpVariantSpace = false;         // Dump variant schema/reflection JSON
    std::vector<VariantOverride> variantOverrides;
};

struct ShaderTiming {
    double irLoweringMs = 0.0;
    double cfgSsaMs = 0.0;
    double spirvGenMs = 0.0;
    double validationMs = 0.0;
    double metalCrossMs = 0.0;
    double hlslCrossMs = 0.0;
    double glslCrossMs = 0.0;
    double glslEsCrossMs = 0.0;
    double totalMs = 0.0;
};

struct TimingInfo {
    double lexMs = 0.0;
    double parseMs = 0.0;
    double totalMs = 0.0;
    std::vector<std::pair<std::string, ShaderTiming>> shaderTimings;  // name -> timing

    void Print() const {
        printf("\n=== Timing Information ===\n");
        printf("  Lexer + Parser:    %8.3f ms\n", lexMs + parseMs);
        printf("    - Lexer init:    %8.3f ms\n", lexMs);
        printf("    - Parse:         %8.3f ms\n", parseMs);
        printf("\n  Per-shader breakdown:\n");
        double totalShaderMs = 0.0;
        double totalValidationMs = 0.0;
        double totalMetalMs = 0.0;
        double totalHlslMs = 0.0;
        double totalGlesMs = 0.0;
        for (const auto& [name, t] : shaderTimings) {
            printf("    %s:\n", name.c_str());
            printf("      IR lowering:   %8.3f ms\n", t.irLoweringMs);
            if (t.cfgSsaMs > 0.001) {
                printf("      CFG + SSA:     %8.3f ms\n", t.cfgSsaMs);
            }
            printf("      SPIR-V gen:    %8.3f ms\n", t.spirvGenMs);
            if (t.validationMs > 0.0) {
                printf("      Validation:    %8.3f ms\n", t.validationMs);
            }
            if (t.metalCrossMs > 0.0) {
                printf("      Metal cross:   %8.3f ms\n", t.metalCrossMs);
            }
            if (t.hlslCrossMs > 0.0) {
                printf("      HLSL cross:    %8.3f ms\n", t.hlslCrossMs);
            }
            if (t.glslEsCrossMs > 0.0) {
                printf("      Gles cross:    %8.3f ms\n", t.glslEsCrossMs);
            }
            printf("      Subtotal:      %8.3f ms\n", t.totalMs);
            totalShaderMs += t.irLoweringMs + t.cfgSsaMs + t.spirvGenMs;
            totalValidationMs += t.validationMs;
            totalMetalMs += t.metalCrossMs;
            totalHlslMs += t.hlslCrossMs;
            totalGlesMs += t.glslEsCrossMs;
        }
        printf("\n  Summary:\n");
        printf("    BWSL compile:    %8.3f ms\n", lexMs + parseMs + totalShaderMs);
        printf("    Validation:      %8.3f ms\n", totalValidationMs);
        printf("    Metal cross:     %8.3f ms\n", totalMetalMs);
        printf("    HLSL cross:      %8.3f ms\n", totalHlslMs);
        printf("    Gles cross:      %8.3f ms\n", totalGlesMs);
        printf("    Total:           %8.3f ms\n", totalMs);
        printf("===========================\n");
    }
};


void print_splash() {
    printf(R"(
    ╭─────────────────────────────────────╮
    │ ░▒▓█ B W S L █▓▒░                   │
    │ ─────────────────────────────       │
    │ » Brawl Shading Language            │
    │ » Compiler v 0.5.0                  │
    │ » Made by Alexander Presthus        │
    │ » https://github.com/apresthus/bwsl │
    │                                     │
    ╰─────────────────────────────────────╯
)" "\n");
}
// ============= Utility Functions =============

void PrintUsage(const char* programName) {
    print_splash();
    printf("Usage: %s <input.bwsl> [options]\n\n", programName);
    printf("Options:\n");
    printf("  -o <dir>       Output directory (default: current directory)\n");
    printf("  -modules <dir> Add module search path (can be used multiple times)\n");
    printf("  -config <file> Add render config path\n");
    printf("  -variant <k=v> Set a named variant value (repeatable)\n");
    printf("  -dump-variant-space  Print variant reflection JSON and exit\n");
    printf("  -pass <name>   Compile specific pass (default: all passes)\n");
    printf("  -stage <name>  Compile specific stage: vertex, fragment, compute (default: all)\n");
    printf("\n");
    printf("Output format flags (SPIR-V is always generated):\n");
    printf("  -metal         Generate Metal Shading Language output\n");
    printf("  -hlsl          Generate HLSL (High-Level Shader Language) output\n");
    printf("  -glsl          Generate GLSL output (version 450)\n");
    printf("  -gles          Generate GLSL ES output for WebGL 2.0 / OpenGL ES 3.0 (version 300 es)\n");
    printf("  -gles-direct   Generate GLSL ES directly from IR (bypass SPIRV-Cross, faster)\n");
    printf("  -webgl         Alias for -gles\n");
    printf("  -bindings      Output resolved resource bindings JSON\n");
    printf("  -all           Generate all outputs\n");
    printf("----------------------------------------\n");
    printf("Debug options:\n");
    printf("  -v             Verbose output\n");
    printf("  -timing        Print timing information\n");
    printf("  -dump-ir       Dump BWSL IR\n");
    printf("  -debug-names   Emit debug names in SPIR-V output\n");
    printf("  -no-validate   Skip SPIR-V validation (faster compilation)\n");
    printf("  -internals     Output SPIR-V disassembly and BWSL IR dump to JSON file\n");
    printf("  -h, --help     Show this help\n");
    printf("----------------------------------------\n");
    printf("\nExamples:\n");
    printf("  %s shader.bwsl                      # SPIR-V only\n", programName);
    printf("  %s shader.bwsl -metal               # SPIR-V + Metal\n", programName);
    printf("  %s shader.bwsl -metal -hlsl -gles   # SPIR-V + Metal + HLSL + WebGL\n", programName);
    printf("  %s shader.bwsl -format all          # All formats\n", programName);
    printf("  %s shader.bwsl -config render.rcfg -gles  # Use render config with WebGL output\n", programName);
    printf("  %s shader.bwsl -variant lighting=Clustered -variant skinning=true\n", programName);
}

std::string ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Split source into lines for error reporting
std::vector<std::string> SplitLines(const std::string& source) {
    std::vector<std::string> lines;
    std::istringstream stream(source);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

// Print error with source context (nice formatting)
void PrintErrorWithContext(const ParseError& err, const std::vector<std::string>& lines,
                           const std::string& source, const TokenStream& stream) {
    fprintf(stderr, "\n  ┌─ Error at line %u, column %u\n", err.line, err.column);
    fprintf(stderr, "  │\n");

    // Show context: 2 lines before, the error line, 2 lines after
    int startLine = std::max(1, (int)err.line - 2);
    int endLine = std::min((int)lines.size(), (int)err.line + 2);

    for (int i = startLine; i <= endLine; i++) {
        if (i <= 0 || i > (int)lines.size()) continue;

        const std::string& line = lines[i - 1];

        if (i == (int)err.line) {
            // Error line - highlight it
            fprintf(stderr, "  │ %4d │ %s\n", i, line.c_str());

            // Print caret pointing to error column
            fprintf(stderr, "  │      │ ");
            for (u32 j = 0; j < err.column && j < line.length(); j++) {
                if (line[j] == '\t') {
                    fprintf(stderr, "    ");  // Tab = 4 spaces
                } else {
                    fprintf(stderr, " ");
                }
            }
            fprintf(stderr, "^\n");

            // Print error message
            fprintf(stderr, "  │      └─ %s\n", err.message ? err.message : "(no message)");
        } else {
            // Context line
            fprintf(stderr, "  │ %4d │ %s\n", i, line.c_str());
        }
    }

    fprintf(stderr, "  │\n");

    // Show token info if available
    if (err.token != INVALID_TOKEN && stream.GetLength(err.token) > 0) {
        std::string_view tokenValue = stream.GetValue(err.token);
        fprintf(stderr, "  │ Token: '%.*s' (type %d)\n",
               (int)tokenValue.length(), tokenValue.data(), (int)stream.GetType(err.token));
    }

    fprintf(stderr, "  └─\n");
}

bool WriteBinaryFile(const fs::path& path, const std::vector<u32>& data) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(u32));
    return file.good();
}

bool WriteTextFile(const fs::path& path, const std::string& content) {
    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }
    file << content;
    return file.good();
}

std::string BuildOutputBasePath(const std::string& outputDir, const std::string& stem) {
    return (fs::path(outputDir) / stem).string();
}

std::string GetTempSpirvPath() {
    std::error_code ec;
    fs::path tempDir = fs::temp_directory_path(ec);
    if (ec || tempDir.empty()) {
        ec.clear();
        tempDir = fs::current_path(ec);
    }
    if (ec || tempDir.empty()) {
        tempDir = ".";
    }
    return (tempDir / "bwslc_temp.spv").string();
}

std::string RunCommand(const std::string& cmd) {
    FILE* pipe = nullptr;
#if defined(_WIN32)
    pipe = _popen(cmd.c_str(), "r");
#else
    pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return "";

    char buffer[512];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }

#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

// Cross-compilation using CLI tools (fallback)
std::string CrossCompileToMetalCLI(const std::string& spirvFile) {
    std::string cmd = "spirv-cross --msl \"" + spirvFile + "\" 2>&1";
    return RunCommand(cmd);
}

std::string CrossCompileToHLSLCLI(const std::string& spirvFile, bool useWaveOps = false) {
    const char* shaderModel = useWaveOps ? "60" : "50";
    std::string cmd = "spirv-cross --hlsl --shader-model " + std::string(shaderModel) + " \"" + spirvFile + "\" 2>&1";
    return RunCommand(cmd);
}

std::string CrossCompileToGLSLCLI(const std::string& spirvFile, int glslVersion = 450) {
    std::string cmd = "spirv-cross \"" + spirvFile + "\" --version " + std::to_string(glslVersion) + " 2>&1";
    return RunCommand(cmd);
}

#ifdef USE_SPIRV_CROSS_LIB
// Cross-compilation using embedded library (much faster - no process spawn overhead)
std::string CrossCompileToMetal(const std::vector<u32>& spirv) {
    // Convert u32 to uint32_t (they should be the same, but wrapper uses standard types)
    std::vector<uint32_t> spirvData(spirv.begin(), spirv.end());
    return spirv_cross_wrapper::CompileToMSL(spirvData);
}

std::string CrossCompileToHLSL(const std::vector<u32>& spirv, bool useWaveOps = false) {
    std::vector<uint32_t> spirvData(spirv.begin(), spirv.end());
    return spirv_cross_wrapper::CompileToHLSL(spirvData, useWaveOps ? 60 : 50);
}

std::string CrossCompileToGLSL(const std::vector<u32>& spirv, int glslVersion = 450, bool es = false) {
    std::vector<uint32_t> spirvData(spirv.begin(), spirv.end());
    return spirv_cross_wrapper::CompileToGLSL(spirvData, glslVersion, es);
}
#else
// Fallback to CLI when library not available
std::string CrossCompileToMetal(const std::string& spirvFile) {
    return CrossCompileToMetalCLI(spirvFile);
}

std::string CrossCompileToHLSL(const std::string& spirvFile, bool useWaveOps = false) {
    return CrossCompileToHLSLCLI(spirvFile, useWaveOps);
}

std::string CrossCompileToGLSL(const std::string& spirvFile, int glslVersion = 450) {
    return CrossCompileToGLSLCLI(spirvFile, glslVersion);
}
#endif

// ============= Parallel Cross-Compilation =============
// Runs all enabled cross-compilation targets in parallel using std::async
// This provides ~3-4x speedup when multiple targets are enabled

#ifdef USE_SPIRV_CROSS_LIB
struct CrossCompileResult {
    std::string metal;
    std::string hlsl;
    std::string glsl;
    std::string glslEs;
    double metalMs = 0;
    double hlslMs = 0;
    double glslMs = 0;
    double glslEsMs = 0;
};

CrossCompileResult ParallelCrossCompile(
    const std::vector<u32>& spirv,
    bool outputMetal,
    bool outputHlsl,
    bool outputGlsl,
    bool outputGlslEs,
    bool hasWaveOps = false)
{
    using Clock = std::chrono::high_resolution_clock;
    CrossCompileResult result;

    // Convert to uint32_t once (shared by all threads - read-only)
    std::vector<uint32_t> spirvData(spirv.begin(), spirv.end());

    // Launch all enabled targets in parallel
    std::future<std::pair<std::string, double>> metalFuture;
    std::future<std::pair<std::string, double>> hlslFuture;
    std::future<std::pair<std::string, double>> glslFuture;
    std::future<std::pair<std::string, double>> glslEsFuture;

    if (outputMetal) {
        metalFuture = std::async(std::launch::async, [&spirvData]() {
            auto start = Clock::now();
            std::string src = spirv_cross_wrapper::CompileToMSL(spirvData);
            auto end = Clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count();
            return std::make_pair(std::move(src), ms);
        });
    }

    if (outputHlsl) {
        hlslFuture = std::async(std::launch::async, [&spirvData, hasWaveOps]() {
            auto start = Clock::now();
            std::string src = spirv_cross_wrapper::CompileToHLSL(spirvData, hasWaveOps ? 60 : 50);
            auto end = Clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count();
            return std::make_pair(std::move(src), ms);
        });
    }

    if (outputGlsl) {
        glslFuture = std::async(std::launch::async, [&spirvData]() {
            auto start = Clock::now();
            std::string src = spirv_cross_wrapper::CompileToGLSL(spirvData, 450, false);
            auto end = Clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count();
            return std::make_pair(std::move(src), ms);
        });
    }

    if (outputGlslEs) {
        glslEsFuture = std::async(std::launch::async, [&spirvData]() {
            auto start = Clock::now();
            std::string src = spirv_cross_wrapper::CompileToGLSL(spirvData, 300, true);
            auto end = Clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count();
            return std::make_pair(std::move(src), ms);
        });
    }

    // Collect results (blocks until each is ready)
    if (outputMetal && metalFuture.valid()) {
        auto [src, ms] = metalFuture.get();
        result.metal = std::move(src);
        result.metalMs = ms;
    }
    if (outputHlsl && hlslFuture.valid()) {
        auto [src, ms] = hlslFuture.get();
        result.hlsl = std::move(src);
        result.hlslMs = ms;
    }
    if (outputGlsl && glslFuture.valid()) {
        auto [src, ms] = glslFuture.get();
        result.glsl = std::move(src);
        result.glslMs = ms;
    }
    if (outputGlslEs && glslEsFuture.valid()) {
        auto [src, ms] = glslEsFuture.get();
        result.glslEs = std::move(src);
        result.glslEsMs = ms;
    }

    return result;
}
#endif

// ============= IR Dump =============

const char* OpCodeToString(IR::OpCode op) {
    switch (op) {
        // Control Flow
        case IR::OP_NOP:           return "NOP";
        case IR::OP_JUMP:          return "JUMP";
        case IR::OP_BRANCH:        return "BRANCH";
        case IR::OP_CALL:          return "CALL";
        case IR::OP_RET:           return "RET";
        case IR::OP_SELECT:        return "SELECT";
        case IR::OP_PHI:           return "PHI";
        case IR::OP_SWITCH:        return "SWITCH";
        // Memory
        case IR::OP_LOAD_CONST:    return "LOAD_CONST";
        case IR::OP_LOAD_REG:      return "LOAD_REG";
        case IR::OP_STORE_REG:     return "STORE_REG";
        case IR::OP_LOAD_ATTR:     return "LOAD_ATTR";
        case IR::OP_STORE_OUTPUT:  return "STORE_OUTPUT";
        case IR::OP_LOAD_OUTPUT:   return "LOAD_OUTPUT";
        case IR::OP_LOAD_UNIFORM:  return "LOAD_UNIFORM";
        case IR::OP_LOAD_BUFFER:   return "LOAD_BUFFER";
        case IR::OP_STORE_BUFFER:  return "STORE_BUFFER";
        case IR::OP_LOAD_LOCAL:    return "LOAD_LOCAL";
        case IR::OP_STORE_LOCAL:   return "STORE_LOCAL";
        case IR::OP_LOAD_SHARED:   return "LOAD_SHARED";
        case IR::OP_STORE_SHARED:  return "STORE_SHARED";
        case IR::OP_LOAD_INPUT:    return "LOAD_INPUT";
        case IR::OP_ARRAY_LOAD:    return "ARRAY_LOAD";
        case IR::OP_ARRAY_STORE:   return "ARRAY_STORE";
        case IR::OP_STORAGE_PTR:   return "STORAGE_PTR";
        case IR::OP_STORAGE_FIELD: return "STORAGE_FIELD";
        case IR::OP_STORAGE_INDEX: return "STORAGE_INDEX";
        case IR::OP_STORAGE_LOAD:  return "STORAGE_LOAD";
        // Float Arithmetic
        case IR::OP_FADD:          return "FADD";
        case IR::OP_FSUB:          return "FSUB";
        case IR::OP_FMUL:          return "FMUL";
        case IR::OP_FDIV:          return "FDIV";
        case IR::OP_FMOD:          return "FMOD";
        case IR::OP_FREM:          return "FREM";
        case IR::OP_LDEXP:         return "LDEXP";
        case IR::OP_FNEG:          return "FNEG";
        case IR::OP_FABS:          return "FABS";
        case IR::OP_FMIN:          return "FMIN";
        case IR::OP_FMAX:          return "FMAX";
        case IR::OP_FCLAMP:        return "FCLAMP";
        case IR::OP_FLOOR:         return "FLOOR";
        case IR::OP_CEIL:          return "CEIL";
        case IR::OP_ROUND:         return "ROUND";
        case IR::OP_TRUNC:         return "TRUNC";
        case IR::OP_FRACT:         return "FRACT";
        case IR::OP_FMA:           return "FMA";
        // Integer Arithmetic
        case IR::OP_IADD:          return "IADD";
        case IR::OP_ISUB:          return "ISUB";
        case IR::OP_IMUL:          return "IMUL";
        case IR::OP_IDIV:          return "IDIV";
        case IR::OP_IMOD:          return "IMOD";
        case IR::OP_INEG:          return "INEG";
        case IR::OP_IABS:          return "IABS";
        case IR::OP_IMIN:          return "IMIN";
        case IR::OP_IMAX:          return "IMAX";
        case IR::OP_ICLAMP:        return "ICLAMP";
        case IR::OP_UMIN:          return "UMIN";
        case IR::OP_UMAX:          return "UMAX";
        case IR::OP_UCLAMP:        return "UCLAMP";
        // Bitwise
        case IR::OP_AND:           return "AND";
        case IR::OP_OR:            return "OR";
        case IR::OP_XOR:           return "XOR";
        case IR::OP_NOT:           return "NOT";
        case IR::OP_SHL:           return "SHL";
        case IR::OP_SHR:           return "SHR";
        case IR::OP_ASR:           return "ASR";
        case IR::OP_POPCNT:        return "POPCNT";
        case IR::OP_CLZ:           return "CLZ";
        case IR::OP_CTZ:           return "CTZ";
        case IR::OP_REVERSE_BITS:  return "REVERSE_BITS";
        case IR::OP_BITFIELD_EXTRACT: return "BITFIELD_EXTRACT";
        case IR::OP_BITFIELD_INSERT:  return "BITFIELD_INSERT";
        case IR::OP_PACK_UNORM2X16:   return "PACK_UNORM2X16";
        case IR::OP_UNPACK_UNORM2X16: return "UNPACK_UNORM2X16";
        case IR::OP_PACK_UNORM4X8:    return "PACK_UNORM4X8";
        case IR::OP_UNPACK_UNORM4X8:  return "UNPACK_UNORM4X8";
        case IR::OP_PACK_SNORM2X16:   return "PACK_SNORM2X16";
        case IR::OP_UNPACK_SNORM2X16: return "UNPACK_SNORM2X16";
        case IR::OP_PACK_SNORM4X8:    return "PACK_SNORM4X8";
        // Comparison
        case IR::OP_FEQ:           return "FEQ";
        case IR::OP_FNE:           return "FNE";
        case IR::OP_FLT:           return "FLT";
        case IR::OP_FLE:           return "FLE";
        case IR::OP_FGT:           return "FGT";
        case IR::OP_FGE:           return "FGE";
        case IR::OP_IEQ:           return "IEQ";
        case IR::OP_INE:           return "INE";
        case IR::OP_ILT:           return "ILT";
        case IR::OP_ILE:           return "ILE";
        case IR::OP_IGT:           return "IGT";
        case IR::OP_IGE:           return "IGE";
        case IR::OP_ULT:           return "ULT";
        case IR::OP_ULE:           return "ULE";
        case IR::OP_UGT:           return "UGT";
        case IR::OP_UGE:           return "UGE";
        // Type Conversion
        case IR::OP_F2I:           return "F2I";
        case IR::OP_I2F:           return "I2F";
        case IR::OP_F2U:           return "F2U";
        case IR::OP_U2F:           return "U2F";
        case IR::OP_I2U:           return "I2U";
        case IR::OP_U2I:           return "U2I";
        case IR::OP_F2F16:         return "F2F16";
        case IR::OP_F162F:         return "F162F";
        case IR::OP_BITCAST:       return "BITCAST";
        case IR::OP_SIGN:          return "SIGN";
        // Vector
        case IR::OP_VEC_EXTRACT:   return "VEC_EXTRACT";
        case IR::OP_VEC_INSERT:    return "VEC_INSERT";
        case IR::OP_VEC_SHUFFLE:   return "VEC_SHUFFLE";
        case IR::OP_VEC_CONSTRUCT: return "VEC_CONSTRUCT";
        // Struct
        case IR::OP_STRUCT_CONSTRUCT: return "STRUCT_CONSTRUCT";
        case IR::OP_STRUCT_EXTRACT:   return "STRUCT_EXTRACT";
        case IR::OP_STRUCT_INSERT:    return "STRUCT_INSERT";
        case IR::OP_STRUCT_LOAD:      return "STRUCT_LOAD";
        case IR::OP_STRUCT_STORE:     return "STRUCT_STORE";
        case IR::OP_STRUCT_GEP:       return "STRUCT_GEP";
        // Math Functions
        case IR::OP_SQRT:          return "SQRT";
        case IR::OP_RSQRT:         return "RSQRT";
        case IR::OP_POW:           return "POW";
        case IR::OP_EXP:           return "EXP";
        case IR::OP_EXP2:          return "EXP2";
        case IR::OP_LOG:           return "LOG";
        case IR::OP_LOG2:          return "LOG2";
        case IR::OP_SIN:           return "SIN";
        case IR::OP_COS:           return "COS";
        case IR::OP_TAN:           return "TAN";
        case IR::OP_ASIN:          return "ASIN";
        case IR::OP_ACOS:          return "ACOS";
        case IR::OP_ATAN:          return "ATAN";
        case IR::OP_ATAN2:         return "ATAN2";
        case IR::OP_SINH:          return "SINH";
        case IR::OP_COSH:          return "COSH";
        case IR::OP_TANH:          return "TANH";
        case IR::OP_UNPACK_SNORM4X8: return "UNPACK_SNORM4X8";
        case IR::OP_PACK_HALF2X16: return "PACK_HALF2X16";
        case IR::OP_UNPACK_HALF2X16: return "UNPACK_HALF2X16";
        case IR::OP_MODF_STRUCT: return "MODF_STRUCT";
        case IR::OP_FREXP_STRUCT: return "FREXP_STRUCT";
        case IR::OP_ISNORMAL:      return "ISNORMAL";
        case IR::OP_ISNAN:         return "ISNAN";
        case IR::OP_ISINF:         return "ISINF";
        case IR::OP_ISFINITE:      return "ISFINITE";
        // Geometric
        case IR::OP_DOT:           return "DOT";
        case IR::OP_CROSS:         return "CROSS";
        case IR::OP_LENGTH:        return "LENGTH";
        case IR::OP_NORMALIZE:     return "NORMALIZE";
        case IR::OP_DISTANCE:      return "DISTANCE";
        case IR::OP_REFLECT:       return "REFLECT";
        case IR::OP_REFRACT:       return "REFRACT";
        case IR::OP_FACEFORWARD:   return "FACEFORWARD";
        // Matrix
        case IR::OP_MAT_MUL:       return "MAT_MUL";
        case IR::OP_MAT_TRANSPOSE: return "MAT_TRANSPOSE";
        case IR::OP_MAT_INVERSE:   return "MAT_INVERSE";
        case IR::OP_MAT_DET:       return "MAT_DET";
        case IR::OP_MAT_CONSTRUCT: return "MAT_CONSTRUCT";
        case IR::OP_MAT_IDENTITY:  return "MAT_IDENTITY";
        case IR::OP_MAT_ZERO:      return "MAT_ZERO";
        // Texture
        case IR::OP_TEX_SAMPLE:      return "TEX_SAMPLE";
        case IR::OP_TEX_SAMPLE_LOD:  return "TEX_SAMPLE_LOD";
        case IR::OP_TEX_SAMPLE_BIAS: return "TEX_SAMPLE_BIAS";
        case IR::OP_TEX_SAMPLE_GRAD: return "TEX_SAMPLE_GRAD";
        case IR::OP_TEX_SAMPLE_CMP:  return "TEX_SAMPLE_CMP";
        case IR::OP_TEX_SAMPLE_OFFSET:      return "TEX_SAMPLE_OFFSET";
        case IR::OP_TEX_SAMPLE_LOD_OFFSET:  return "TEX_SAMPLE_LOD_OFFSET";
        case IR::OP_TEX_SAMPLE_BIAS_OFFSET: return "TEX_SAMPLE_BIAS_OFFSET";
        case IR::OP_TEX_GATHER:      return "TEX_GATHER";
        case IR::OP_TEX_GATHER_OFFSET: return "TEX_GATHER_OFFSET";
        case IR::OP_TEX_FETCH:       return "TEX_FETCH";
        case IR::OP_TEX_FETCH_OFFSET: return "TEX_FETCH_OFFSET";
        case IR::OP_TEX_SIZE:        return "TEX_SIZE";
        case IR::OP_TEX_LEVELS:      return "TEX_LEVELS";
        case IR::OP_IMG_LOAD:        return "IMG_LOAD";
        case IR::OP_IMG_STORE:       return "IMG_STORE";
        case IR::OP_LOAD_TEX_HANDLE: return "LOAD_TEX_HANDLE";
        // Derivatives
        case IR::OP_DDX:           return "DDX";
        case IR::OP_DDY:           return "DDY";
        case IR::OP_DDX_FINE:      return "DDX_FINE";
        case IR::OP_DDY_FINE:      return "DDY_FINE";
        case IR::OP_DDX_COARSE:    return "DDX_COARSE";
        case IR::OP_DDY_COARSE:    return "DDY_COARSE";
        case IR::OP_FWIDTH:        return "FWIDTH";
        case IR::OP_FWIDTH_FINE:   return "FWIDTH_FINE";
        case IR::OP_FWIDTH_COARSE: return "FWIDTH_COARSE";
        // Interpolation
        case IR::OP_LERP:          return "LERP";
        case IR::OP_SMOOTHSTEP:    return "SMOOTHSTEP";
        case IR::OP_STEP:          return "STEP";
        case IR::OP_SATURATE:      return "SATURATE";
        case IR::OP_DEGREES:       return "DEGREES";
        case IR::OP_RADIANS:       return "RADIANS";
        // Atomics
        case IR::OP_ATOMIC_ADD:      return "ATOMIC_ADD";
        case IR::OP_ATOMIC_SUB:      return "ATOMIC_SUB";
        case IR::OP_ATOMIC_MIN:      return "ATOMIC_MIN";
        case IR::OP_ATOMIC_MAX:      return "ATOMIC_MAX";
        case IR::OP_ATOMIC_AND:      return "ATOMIC_AND";
        case IR::OP_ATOMIC_OR:       return "ATOMIC_OR";
        case IR::OP_ATOMIC_XOR:      return "ATOMIC_XOR";
        case IR::OP_ATOMIC_XCHG:     return "ATOMIC_XCHG";
        case IR::OP_ATOMIC_CMP_XCHG: return "ATOMIC_CMP_XCHG";
        case IR::OP_ANY:             return "ANY";
        case IR::OP_ALL:             return "ALL";
        // Local pointers
        case IR::OP_LOCAL_VAR_PTR:   return "LOCAL_VAR_PTR";
        case IR::OP_LOCAL_LOAD:      return "LOCAL_LOAD";
        case IR::OP_LOCAL_STORE:     return "LOCAL_STORE";
        case IR::OP_LOCAL_FIELD_PTR: return "LOCAL_FIELD_PTR";
        // Synchronization
        case IR::OP_BARRIER:       return "BARRIER";
        case IR::OP_MEM_FENCE:     return "MEM_FENCE";
        default:                   return "UNKNOWN";
    }
}

const char* CoreTypeToString(CoreType type) {
    switch (type) {
        case CoreType::INVALID: return "INVALID";
        case CoreType::BOOL:    return "BOOL";
        case CoreType::INT:     return "INT";
        case CoreType::UINT:    return "UINT";
        case CoreType::FLOAT:   return "FLOAT";
        case CoreType::INT2:    return "INT2";
        case CoreType::INT3:    return "INT3";
        case CoreType::INT4:    return "INT4";
        case CoreType::UINT2:   return "UINT2";
        case CoreType::UINT3:   return "UINT3";
        case CoreType::UINT4:   return "UINT4";
        case CoreType::FLOAT2:  return "FLOAT2";
        case CoreType::FLOAT3:  return "FLOAT3";
        case CoreType::FLOAT4:  return "FLOAT4";
        case CoreType::MAT2:    return "MAT2";
        case CoreType::MAT3:    return "MAT3";
        case CoreType::MAT4:    return "MAT4";
        case CoreType::VOID:    return "VOID";
        case CoreType::STRING:  return "STRING";
        case CoreType::CUSTOM:  return "CUSTOM";
        default:                return "?";
    }
}

std::string FormatOperand(const IRProgram& prog, u16 op, bool allowZeroReg = true) {
    if (op & 0x8000) {
        // Float constant
        u16 idx = op & 0x7FFF;
        if (idx < prog.floatCount) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.4g", prog.floatConstants[idx]);
            return buf;
        }
        return "f?";
    }
    if (op & 0x4000) {
        // Int constant
        u16 idx = op & 0x3FFF;
        if (idx < prog.intCount) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", (int)prog.intConstants[idx]);
            return buf;
        }
        return "i?";
    }
    // Register (0 is valid register r0)
    if (op == 0 && !allowZeroReg) return "_";
    char buf[16];
    snprintf(buf, sizeof(buf), "r%u", op);
    return buf;
}

void DumpIR(const IRProgram& prog) {
    printf("\n=== BWSL IR Dump ===\n");
    printf("Instructions: %u, Registers: %u\n", prog.instructionCount, prog.registerCount);
    printf("Float constants: %u, Int constants: %u\n\n", prog.floatCount, prog.intCount);

    // Print float constants if any
    if (prog.floatCount > 0) {
        printf("Float Constants:\n");
        for (u32 i = 0; i < prog.floatCount; i++) {
            printf("  [%u] = %.6g\n", i, prog.floatConstants[i]);
        }
        printf("\n");
    }

    // Print int constants if any
    if (prog.intCount > 0) {
        printf("Int Constants:\n");
        for (u32 i = 0; i < prog.intCount; i++) {
            printf("  [%u] = %d\n", i, (int)prog.intConstants[i]);
        }
        printf("\n");
    }

    // Print instructions
    printf("Instructions:\n");
    for (u32 i = 0; i < prog.instructionCount; i++) {
        IR::OpCode op = static_cast<IR::OpCode>(prog.opcodes[i]);
        u16 dest = prog.destinations[i];
        CoreType type = prog.types ? static_cast<CoreType>(prog.types[i]) : CoreType::INVALID;

        // Format: [idx] OP dest:TYPE <- op0, op1, op2, op3
        printf("  [%3u] %-16s", i, OpCodeToString(op));

        // Print destination
        if (dest != 0 || op == IR::OP_STORE_REG || op == IR::OP_STORE_OUTPUT) {
            if (type != CoreType::INVALID) {
                printf(" r%-3u:%-7s <-", dest, CoreTypeToString(type));
            } else {
                printf(" r%-3u         <-", dest);
            }
        } else {
            printf(" %-15s", "");
        }

        // Print operands based on opcode
        u16 op0 = prog.GetOperand(i, 0);
        u16 op1 = prog.GetOperand(i, 1);
        u16 op2 = prog.GetOperand(i, 2);
        u16 op3 = prog.GetOperand(i, 3);

        switch (op) {
            case IR::OP_NOP:
            case IR::OP_RET:
                break;
            case IR::OP_JUMP:
                printf(" -> %u", prog.metadata[i]);
                break;
            case IR::OP_BRANCH:
                printf(" %s ? -> %u : %u", FormatOperand(prog, op0).c_str(),
                       prog.GetBranchTrueTarget(i), prog.GetBranchFalseTarget(i));
                break;
            case IR::OP_LOAD_CONST:
                printf(" %s", FormatOperand(prog, op0).c_str());
                break;
            case IR::OP_LOAD_ATTR:
                printf(" attr[%u]", op0);
                break;
            case IR::OP_LOAD_OUTPUT:
                printf(" output[%u]", op0);
                break;
            case IR::OP_STORE_OUTPUT:
                printf(" output[%u] = %s", op0, FormatOperand(prog, op1).c_str());
                break;
            case IR::OP_STORE_REG:
                printf(" %s", FormatOperand(prog, op0).c_str());
                break;
            case IR::OP_VEC_CONSTRUCT:
                printf(" (%s, %s, %s, %s)",
                    FormatOperand(prog, op0).c_str(),
                    FormatOperand(prog, op1).c_str(),
                    FormatOperand(prog, op2).c_str(),
                    FormatOperand(prog, op3).c_str());
                break;
            // Binary operations: show both operands
            case IR::OP_FADD: case IR::OP_FSUB: case IR::OP_FMUL: case IR::OP_FDIV: case IR::OP_FREM:
            case IR::OP_IADD: case IR::OP_ISUB: case IR::OP_IMUL: case IR::OP_IDIV:
            case IR::OP_AND: case IR::OP_OR: case IR::OP_XOR:
            case IR::OP_FLT: case IR::OP_FLE: case IR::OP_FGT: case IR::OP_FGE: case IR::OP_FEQ: case IR::OP_FNE:
            case IR::OP_ILT: case IR::OP_ILE: case IR::OP_IGT: case IR::OP_IGE: case IR::OP_IEQ: case IR::OP_INE:
                printf(" %s, %s", FormatOperand(prog, op0).c_str(), FormatOperand(prog, op1).c_str());
                break;
            default:
                // Generic: print all operands (r0 is valid)
                printf(" %s", FormatOperand(prog, op0).c_str());
                if (op1 || op2 || op3) printf(", %s", FormatOperand(prog, op1).c_str());
                if (op2 || op3) printf(", %s", FormatOperand(prog, op2).c_str());
                if (op3) printf(", %s", FormatOperand(prog, op3).c_str());
                break;
        }

        printf("\n");
    }

    // Print PHI nodes if any
    if (prog.phiCount > 0) {
        printf("\nPHI Nodes:\n");
        for (u32 i = 0; i < prog.phiCount; i++) {
            printf("  PHI r%u (block %u): ", prog.phiResultRegs[i], prog.phiBlockIndices[i]);
            u32 start = prog.phiOperandOffsets[i];
            u32 end = prog.phiOperandOffsets[i + 1];
            for (u32 j = start; j < end; j++) {
                if (j > start) printf(", ");
                printf("[b%u: %s]", prog.phiOperandBlocks[j],
                       FormatOperand(prog, prog.phiOperandValues[j]).c_str());
            }
            printf("\n");
        }
    }

    printf("=== End IR Dump ===\n\n");
}

// Escape a string for JSON output
std::string EscapeJsonString(const std::string& str) {
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
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    result += buf;
                } else {
                    result += c;
                }
                break;
        }
    }
    return result;
}

static u8 BuildPassAttributeMask(const AST& ast, NodeRef pipelineRef, NodeRef passRef) {
    if (pipelineRef.IsNull() || passRef.IsNull()) {
        return 0;
    }

    const PipelineData& pipeline = ast.GetPipeline(pipelineRef);
    const PassData& pass = ast.GetPass(passRef);

    u8 mask = 0;

    auto addAttributeMask = [&](const ArenaString& attributeName) {
        for (u32 i = 0; i < pipeline.attributes.count; i++) {
            const AttributeDeclData& attr = ast.GetAttributeDecl(pipeline.attributes[i]);
            if (attr.name.nameHash == attributeName.nameHash) {
                mask |= (1u << attr.attributeIndex);
                return;
            }
        }
    };

    if (pass.usedAttributes.count > 0) {
        for (u32 i = 0; i < pass.usedAttributes.count; i++) {
            addAttributeMask(pass.usedAttributes[i]);
        }
        return mask;
    }

    for (u32 i = 0; i < pipeline.attributes.count; i++) {
        const AttributeDeclData& attr = ast.GetAttributeDecl(pipeline.attributes[i]);
        mask |= (1u << attr.attributeIndex);
    }

    return mask;
}

static std::string BuildWebGLSidecarJson(const PassData& pass,
                                         const SymbolTableData& symbols,
                                         const IRAnalysis& analysis,
                                         const char* sourceBase,
                                         const RenderConfigParser::GeometryConfig& geometryConfig,
                                         bool includeAttributes,
                                         const AST* ast = nullptr,
                                         const PipelineData* pipeline = nullptr) {
    std::vector<std::pair<std::string, u32>> uniforms;
    std::vector<std::pair<std::string, u32>> samplers;

    ResourceReflectionConfig reflectionConfig;
    reflectionConfig.vertexPullingMode = ResourceReflectionConfig::VertexPullingMode::Disabled;
    reflectionConfig.descriptorSet = 0;

    const std::vector<ReflectedResourceBinding> reflected =
        BuildResolvedResourceReflection(ast, pipeline, &pass, symbols, sourceBase,
                                        &analysis, nullptr, nullptr, reflectionConfig);

    for (const ReflectedResourceBinding& resource : reflected) {
        if (resource.type == ResourceBinding::UniformBuffer) {
            uniforms.emplace_back(resource.name, resource.binding);
        } else if (resource.type == ResourceBinding::Texture) {
            samplers.emplace_back(resource.name, resource.binding);
        }
    }

    auto sortEntries = [](auto& entries) {
        std::sort(entries.begin(), entries.end(),
                  [](const auto& lhs, const auto& rhs) {
                      if (lhs.second != rhs.second) return lhs.second < rhs.second;
                      return lhs.first < rhs.first;
                  });
    };
    sortEntries(uniforms);
    sortEntries(samplers);

    auto appendMap = [](std::string& json,
                        const char* label,
                        const std::vector<std::pair<std::string, u32>>& entries,
                        bool trailingComma) {
        json += "  \"";
        json += label;
        json += "\": {\n";
        for (size_t i = 0; i < entries.size(); i++) {
            json += "    \"";
            json += EscapeJsonString(entries[i].first);
            json += "\": ";
            json += std::to_string(entries[i].second);
            if (i + 1 < entries.size()) json += ",";
            json += "\n";
        }
        json += "  }";
        if (trailingComma) json += ",";
        json += "\n";
    };

    std::string json = "{\n";
    if (includeAttributes) {
        json += "  \"attributes\": {\n";
        for (u32 i = 0; i < pass.usedAttributes.count; i++) {
            std::string attrName = pass.usedAttributes[i].ToString(sourceBase);
            json += "    \"" + EscapeJsonString(attrName) + "\": " + std::to_string(i);
            if (i + 1 < pass.usedAttributes.count) json += ",";
            json += "\n";
        }
        json += "  },\n";
    }

    appendMap(json, "uniforms", uniforms, true);
    appendMap(json, "samplers", samplers,
              geometryConfig.type == RenderConfigParser::GeometryConfig::Type::Instanced);

    if (geometryConfig.type == RenderConfigParser::GeometryConfig::Type::Instanced) {
        json += "  \"geometry\": {\n";
        json += "    \"type\": \"instanced\",\n";
        json += "    \"instanceCount\": " + std::to_string(geometryConfig.instanceCount) + "\n";
        json += "  }\n";
    }

    json += "}\n";
    return json;
}

static const char* ResourceTypeToString(::ResourceBinding::Type type) {
    switch (type) {
        case ::ResourceBinding::UniformBuffer: return "uniform";
        case ::ResourceBinding::StorageBuffer: return "storage_buffer";
        case ::ResourceBinding::Texture: return "texture";
        case ::ResourceBinding::Sampler: return "sampler";
        case ::ResourceBinding::StorageImage: return "storage_image";
        default: return "buffer";
    }
}

static std::string StageFlagsToJsonArray(u8 stageFlags) {
    std::string json = "[";
    bool first = true;
    auto appendStage = [&](const char* stageName, ShaderStage stage) {
        if ((stageFlags & SymbolTable::ShaderStageToBit(stage)) == 0) return;
        if (!first) json += ", ";
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

static const char* ResourceAccessToString(BWSL::ResourceAccessMode access) {
    switch (access) {
        case BWSL::ResourceAccessMode::ReadOnly: return "readonly";
        case BWSL::ResourceAccessMode::WriteOnly: return "writeonly";
        case BWSL::ResourceAccessMode::ReadWrite: return "readwrite";
        default: return "readonly";
    }
}

static std::string BuildResourceBindingsJson(const std::string& passName,
                                             const std::vector<ReflectedResourceBinding>& bindings) {
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
        json += "      \"stages\": " + StageFlagsToJsonArray(binding.stages) + ",\n";
        json += "      \"access\": \"" + std::string(ResourceAccessToString(binding.access)) + "\"";
        if (binding.combinedSampledImage) {
            json += ",\n      \"abi\": \"combined_sampled_image\"";
            if (!binding.combinedWith.empty()) {
                json += ",\n      \"combinedWith\": \"" + EscapeJsonString(binding.combinedWith) + "\"";
            }
        }
        json += "\n";
        json += "    }";
        if (i + 1 < bindings.size()) json += ",";
        json += "\n";
    }

    json += "  ]\n";
    json += "}\n";
    return json;
}

// Dump IR to a string (for -internals output)
std::string DumpIRToString(const IRProgram& prog) {
    std::string out;
    char buf[256];

    snprintf(buf, sizeof(buf), "Instructions: %u, Registers: %u\n", prog.instructionCount, prog.registerCount);
    out += buf;
    snprintf(buf, sizeof(buf), "Float constants: %u, Int constants: %u\n\n", prog.floatCount, prog.intCount);
    out += buf;

    // Print float constants if any
    if (prog.floatCount > 0) {
        out += "Float Constants:\n";
        for (u32 i = 0; i < prog.floatCount; i++) {
            snprintf(buf, sizeof(buf), "  [%u] = %.6g\n", i, prog.floatConstants[i]);
            out += buf;
        }
        out += "\n";
    }

    // Print int constants if any
    if (prog.intCount > 0) {
        out += "Int Constants:\n";
        for (u32 i = 0; i < prog.intCount; i++) {
            snprintf(buf, sizeof(buf), "  [%u] = %d\n", i, (int)prog.intConstants[i]);
            out += buf;
        }
        out += "\n";
    }

    // Print instructions
    out += "Instructions:\n";
    for (u32 i = 0; i < prog.instructionCount; i++) {
        IR::OpCode op = static_cast<IR::OpCode>(prog.opcodes[i]);
        u16 dest = prog.destinations[i];
        CoreType type = prog.types ? static_cast<CoreType>(prog.types[i]) : CoreType::INVALID;

        snprintf(buf, sizeof(buf), "  [%3u] %-16s", i, OpCodeToString(op));
        out += buf;

        // Print destination
        if (dest != 0 || op == IR::OP_STORE_REG || op == IR::OP_STORE_OUTPUT) {
            if (type != CoreType::INVALID) {
                snprintf(buf, sizeof(buf), " r%-3u:%-7s <-", dest, CoreTypeToString(type));
            } else {
                snprintf(buf, sizeof(buf), " r%-3u         <-", dest);
            }
            out += buf;
        } else {
            out += "                ";
        }

        // Print operands based on opcode
        u16 op0 = prog.GetOperand(i, 0);
        u16 op1 = prog.GetOperand(i, 1);
        u16 op2 = prog.GetOperand(i, 2);
        u16 op3 = prog.GetOperand(i, 3);

        switch (op) {
            case IR::OP_NOP:
            case IR::OP_RET:
                break;
            case IR::OP_JUMP:
                snprintf(buf, sizeof(buf), " -> %u", prog.metadata[i]);
                out += buf;
                break;
            case IR::OP_BRANCH:
                snprintf(buf, sizeof(buf), " %s ? -> %u : %u",
                    FormatOperand(prog, op0).c_str(),
                    prog.GetBranchTrueTarget(i), prog.GetBranchFalseTarget(i));
                out += buf;
                break;
            case IR::OP_LOAD_CONST:
                out += " " + FormatOperand(prog, op0);
                break;
            case IR::OP_LOAD_ATTR:
                snprintf(buf, sizeof(buf), " attr[%u]", op0);
                out += buf;
                break;
            case IR::OP_LOAD_OUTPUT:
                snprintf(buf, sizeof(buf), " output[%u]", op0);
                out += buf;
                break;
            case IR::OP_STORE_OUTPUT:
                snprintf(buf, sizeof(buf), " output[%u] = %s", op0, FormatOperand(prog, op1).c_str());
                out += buf;
                break;
            case IR::OP_STORE_REG:
                out += " " + FormatOperand(prog, op0);
                break;
            case IR::OP_VEC_CONSTRUCT:
                snprintf(buf, sizeof(buf), " (%s, %s, %s, %s)",
                    FormatOperand(prog, op0).c_str(),
                    FormatOperand(prog, op1).c_str(),
                    FormatOperand(prog, op2).c_str(),
                    FormatOperand(prog, op3).c_str());
                out += buf;
                break;
            case IR::OP_FADD: case IR::OP_FSUB: case IR::OP_FMUL: case IR::OP_FDIV: case IR::OP_FREM:
            case IR::OP_IADD: case IR::OP_ISUB: case IR::OP_IMUL: case IR::OP_IDIV:
            case IR::OP_AND: case IR::OP_OR: case IR::OP_XOR:
            case IR::OP_FLT: case IR::OP_FLE: case IR::OP_FGT: case IR::OP_FGE: case IR::OP_FEQ: case IR::OP_FNE:
            case IR::OP_ILT: case IR::OP_ILE: case IR::OP_IGT: case IR::OP_IGE: case IR::OP_IEQ: case IR::OP_INE:
                snprintf(buf, sizeof(buf), " %s, %s", FormatOperand(prog, op0).c_str(), FormatOperand(prog, op1).c_str());
                out += buf;
                break;
            default: {
                out += " " + FormatOperand(prog, op0);
                if (op1 || op2 || op3) out += ", " + FormatOperand(prog, op1);
                if (op2 || op3) out += ", " + FormatOperand(prog, op2);
                if (op3) out += ", " + FormatOperand(prog, op3);
                break;
            }
        }

        out += "\n";
    }

    // Print PHI nodes if any
    if (prog.phiCount > 0) {
        out += "\nPHI Nodes:\n";
        for (u32 i = 0; i < prog.phiCount; i++) {
            snprintf(buf, sizeof(buf), "  PHI r%u (block %u): ", prog.phiResultRegs[i], prog.phiBlockIndices[i]);
            out += buf;
            u32 start = prog.phiOperandOffsets[i];
            u32 end = prog.phiOperandOffsets[i + 1];
            for (u32 j = start; j < end; j++) {
                if (j > start) out += ", ";
                snprintf(buf, sizeof(buf), "[b%u: %s]", prog.phiOperandBlocks[j],
                         FormatOperand(prog, prog.phiOperandValues[j]).c_str());
                out += buf;
            }
            out += "\n";
        }
    }

    return out;
}

// Get SPIR-V disassembly using spirv-dis
std::string GetSpirvDisassembly(const std::string& spirvFile) {
    std::string cmd = "spirv-dis \"" + spirvFile + "\" 2>&1";
    return RunCommand(cmd);
}

// Returns empty string on success, error message on failure
std::string ValidateSpirv(const std::string& spirvFile) {
    std::string cmd = "spirv-val \"" + spirvFile + "\" 2>&1";
    std::string result = RunCommand(cmd);
    if (result.empty() || result.find("error") == std::string::npos) {
        return "";  // Success
    }
    return result;  // Return error message
}


// ============= Compilation =============

struct CompileResult {
    bool success = false;
    std::vector<u32> spirv;
    std::string metalSource;
    std::string hlslSource;
    std::string directGlesSource;  // Direct GLES output (bypasses SPIRV-Cross)
    std::string error;
    std::string irDump;       // IR dump for -internals output
    bool hasWaveOps = false;
    IRAnalysis analysis{};
    std::vector<ExplicitSamplerUse> explicitSamplerUses;
    ShaderTiming timing;
};

RenderConfig CreateDefaultRenderConfig(const std::string& pipelineName) {
    RenderConfig config;
    config.name = pipelineName;

    config.renderTargets = {
        RenderTargetHelpers::ColorTarget("SceneColor", 1.0f, PixelFormat::RGBA16Float),
        RenderTargetHelpers::ColorTarget("Velocity", 1.0f, PixelFormat::RG16Float),
        RenderTargetHelpers::DepthTarget("SceneDepth", 1.0f),
    };

    RenderConfig::PassData mainPass;
    mainPass.name = "Main";
    mainPass.type = PassType::Standard;
    mainPass.descriptor.name = "Main";
    mainPass.descriptor.pipelineName = pipelineName;
    config.passes.push_back(mainPass);

    return config;
}

CompileResult CompileShaderStage(
    const CompilationContext& context,
    const Parser& parser,
    const PassData& pass,
    ShaderStage stage,
    bool verbose,
    bool dumpIr = false,
    bool debugNames = false,
    bool useStd430Padding = true,
    bool useInterleavedVertices = false,
    NodeRef pipelineRef = NodeRef::Null(),
    NodeRef passRef = NodeRef::Null(),  // For pass-scoped function lookup
    PassVaryingContext* varyingContext = nullptr,  // Shared context for vertex->fragment varyings
    bool captureIr = false,  // Capture IR dump for -internals output
    bool useDirectGles = false,  // Generate GLES directly from IR (bypass SPIRV-Cross)
    const RenderConfig* renderConfig = nullptr  // For GLES backend
) {
    using Clock = std::chrono::high_resolution_clock;
    auto shaderStart = Clock::now();

    CompileResult result;

    // Get shader body
    NodeRef shaderBody;
    const ShaderStageData* shaderStageData = nullptr;
    if (stage == ShaderStage::Vertex) {
        if (pass.vertexShader.IsNull()) {
            result.error = "No vertex shader in pass";
            return result;
        }
        shaderStageData = &context.ast.GetShaderStage(pass.vertexShader);
        shaderBody = shaderStageData->body;
    } else if (stage == ShaderStage::Fragment) {
        if (pass.fragmentShader.IsNull()) {
            result.error = "No fragment shader in pass";
            return result;
        }
        shaderStageData = &context.ast.GetShaderStage(pass.fragmentShader);
        shaderBody = shaderStageData->body;
    } else if (stage == ShaderStage::Compute) {
        if (pass.computeShader.IsNull()) {
            result.error = "No compute shader in pass";
            return result;
        }
        shaderStageData = &context.ast.GetShaderStage(pass.computeShader);
        shaderBody = shaderStageData->body;
    } else {
        result.error = "Unsupported shader stage";
        return result;
    }

    if (verbose) {
        printf("    Lowering to IR...\n");
    }

    // Lower to IR
    auto irStart = Clock::now();

    IRMemoryPool irPool;
    IRLowering lowering;
    lowering.Initialize(&irPool, const_cast<SymbolTableData*>(&parser.symbolTable),
                        const_cast<AST*>(&context.ast), parser.sourceBase());
    lowering.currentStage = stage;
    lowering.currentPipeline = pipelineRef;
    lowering.currentPass = passRef;  // For pass-scoped function lookup
    lowering.currentPassVaryings = varyingContext;  // Set varying context for vertex->fragment data flow

    for (u32 i = 0; i < pass.consts.count; i++) {
        lowering.LowerStatement(pass.consts[i]);
    }

    const BlockData& block = context.ast.GetBlock(shaderBody);
    for (u32 i = 0; i < block.statements.count; i++) {
        lowering.LowerStatement(block.statements[i]);
    }

    if (lowering.hadError) {
        result.error = "IR lowering failed. See diagnostic above.";
        return result;
    }

    // Ensure return
    if (lowering.program.instructionCount == 0 ||
        lowering.program.opcodes[lowering.program.instructionCount - 1] != OP_RET) {
        lowering.builder.EmitInstruction(OP_RET, 0, 0);
    }

    // If IR lowering reported recursion, short-circuit: downstream SSA /
    // SPIR-V emission would produce spurious validator errors drowning
    // the diagnostic that already went to stderr.
    if (lowering.recursionDiagnosed) {
        result.error = "Recursion is not supported by SPIR-V. See diagnostic above.";
        return result;
    }

    auto irEnd = Clock::now();
    result.timing.irLoweringMs = std::chrono::duration<double, std::milli>(irEnd - irStart).count();

    if (verbose) {
        printf("    Generated %u IR instructions\n", lowering.program.instructionCount);
    }

    // Dump IR if requested (before SSA conversion to see original form)
    if (dumpIr) {
        printf("    --- Pre-SSA IR ---");
        DumpIR(lowering.program);
    }

    // CFG Construction
    auto cfgStart = Clock::now();
    Memory::BWEMemoryArena cfgArena;
    std::vector<char> cfgMem(512 * 1024);
    cfgArena.Initialize(cfgMem.data(), cfgMem.size());

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
        if (verbose) {
            printf("    Building CFG and converting to SSA...\n");
        }
        cfgBuilder.Init(&lowering.program, &cfgArena);
        cfgBuilder.Build();
        cfgPtr = &cfgBuilder.cfg;

        if (cfgBuilder.cfg.blockCount > 1) {
            SSA::ConvertToSSA(&lowering.program, &cfgBuilder.cfg, &cfgBuilder, &cfgArena);
        }

        // Dump IR after SSA conversion
        if (dumpIr) {
            printf("    --- Post-SSA IR ---");
            DumpIR(lowering.program);
        }
    }

    auto cfgEnd = Clock::now();
    result.timing.cfgSsaMs = std::chrono::duration<double, std::milli>(cfgEnd - cfgStart).count();

    // Capture IR for -internals output (after SSA conversion for final form)
    if (captureIr) {
        result.irDump = DumpIRToString(lowering.program);
    }

    result.explicitSamplerUses = CollectExplicitSamplerUses(lowering.program, stage);

    // Direct GLES output (bypasses SPIRV-Cross)
    if (useDirectGles) {
        auto glesStart = Clock::now();

        // Run IR analysis (needed for attribute/output types)
        IRAnalysis glesAnalysis = {};
        AnalyzeIR(&glesAnalysis, &lowering.program);

        // Create GLES arena
        Memory::BWEMemoryArena glesArena;
        std::vector<char> glesMem(128 * 1024);
        glesArena.Initialize(glesMem.data(), glesMem.size());

        // Initialize and emit GLES
        GLES::GLESBuilder glesBuilder;
        glesBuilder.Initialize(&glesArena, parser.sourceBase(),
                               &lowering.program, cfgPtr, stage,
                               &pass, renderConfig, &glesAnalysis, varyingContext);
        if (stage == ShaderStage::Compute && shaderStageData) {
            glesBuilder.SetComputeWorkgroupSize(shaderStageData->workgroupSizeX,
                                                shaderStageData->workgroupSizeY,
                                                shaderStageData->workgroupSizeZ);
        }
        std::string_view glesOutput = glesBuilder.Emit();
        result.directGlesSource = std::string(glesOutput);

        auto glesEnd = Clock::now();
        result.timing.glslEsCrossMs = std::chrono::duration<double, std::milli>(glesEnd - glesStart).count();

        if (verbose) {
            printf("    Generated GLES directly (%zu chars)\n", result.directGlesSource.size());
        }
    }

    if (verbose) {
        printf("    Generating SPIR-V...\n");
    }

    // Generate SPIR-V
    auto spirvStart = Clock::now();

    Memory::BWEMemoryArena spirvArena;
    std::vector<char> spirvMem(512 * 1024);
    spirvArena.Initialize(spirvMem.data(), spirvMem.size());

    SPIRVBuilder builder;
    builder.Initialize(&spirvArena, &lowering.program, stage,
                       const_cast<SymbolTableData*>(&parser.symbolTable), cfgPtr);
    if (stage == ShaderStage::Compute && shaderStageData) {
        builder.SetComputeWorkgroupSize(shaderStageData->workgroupSizeX,
                                        shaderStageData->workgroupSizeY,
                                        shaderStageData->workgroupSizeZ);
    }

    // Configure vertex input mode
    // WebGL/GLES uses traditional interleaved inputs (in vec3 position;)
    // Other backends use vertex pulling via storage buffers
    SPIRVBuilder::VertexPullingConfig vpConfig;
    vpConfig.mode = useInterleavedVertices
        ? SPIRVBuilder::VertexInputMode::Interleaved
        : SPIRVBuilder::VertexInputMode::SeparateBuffers;
    vpConfig.attributeMask = useInterleavedVertices ? 0 : BuildPassAttributeMask(context.ast, pipelineRef, passRef);
    vpConfig.baseBufferBinding = 0;
    vpConfig.descriptorSet = 0;
    builder.SetVertexPullingConfig(vpConfig);
    builder.SetEmitDebugNames(debugNames);
    builder.SetUseStd430Padding(useStd430Padding);

    builder.EmitFunction();
    result.spirv = builder.Finalize();
    IRAnalysis finalAnalysis{};
    AnalyzeIR(&finalAnalysis, &lowering.program);
    result.analysis = finalAnalysis;
    result.hasWaveOps = finalAnalysis.Has(IRAnalysis::CAP_WAVE_OPS);

    auto spirvEnd = Clock::now();
    result.timing.spirvGenMs = std::chrono::duration<double, std::milli>(spirvEnd - spirvStart).count();

    if (result.spirv.size() <= 5) {
        result.error = "SPIR-V generation failed";
        return result;
    }

    if (verbose) {
        printf("    Generated %zu SPIR-V words\n", result.spirv.size());
    }

    // Calculate total shader time
    auto shaderEnd = Clock::now();
    result.timing.totalMs = std::chrono::duration<double, std::milli>(shaderEnd - shaderStart).count();

    result.success = true;
    return result;
}

// ============= Main Entry Point =============

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    try {
    CompilerConfig config;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        } else if (arg == "-v") {
            config.verbose = true;
        } else if (arg == "-o" && i + 1 < argc) {
            config.outputDir = argv[++i];
        } else if (arg == "-pass" && i + 1 < argc) {
            config.passFilter = argv[++i];
        } else if (arg == "-stage" && i + 1 < argc) {
            config.stageFilter = argv[++i];
        }else if (arg == "-metal") {
            config.outputMetal = true;
        } else if (arg == "-hlsl") {
            config.outputHlsl = true;
        } else if (arg == "-glsl") {
            config.outputGlsl = true;
        } else if (arg == "-gles" || arg == "-webgl") {
            config.outputGlslEs = true;
        } else if (arg == "-gles-direct") {
            config.outputGlslEs = true;
            config.useDirectGles = true;
        } else if (arg == "-all") {
            config.outputMetal = true;
            config.outputHlsl = true;
            config.outputGlsl = true;
            config.outputGlslEs = true;
            config.outputBindings = true;
        } else if (arg == "-modules" && i + 1 < argc) {
            config.modulePaths.push_back(argv[++i]);
        } else if (arg == "-config" && i + 1 < argc) {
            config.renderConfigPath = argv[++i];
        } else if (arg == "-variant" && i + 1 < argc) {
            std::string spec = argv[++i];
            size_t eq = spec.find('=');
            if (eq == std::string::npos || eq == 0 || eq == spec.size() - 1) {
                fprintf(stderr, "Error: -variant expects name=value\n");
                return 1;
            }
            VariantOverride overrideValue;
            overrideValue.name = spec.substr(0, eq);
            overrideValue.value = spec.substr(eq + 1);
            config.variantOverrides.push_back(std::move(overrideValue));
        } else if (arg == "-dump-variant-space") {
            config.dumpVariantSpace = true;
        } else if (arg[0] != '-') {
            config.inputFile = arg;
        } else if (arg == "-dump-ir") {
            config.dumpIr = true;
        } else if (arg == "-debug-names") {
            config.debugNames = true;
        } else if (arg == "-timing") {
            config.showTiming = true;
        } else if (arg == "-no-validate") {
            config.skipValidation = true;
        } else if (arg == "-internals") {
            config.outputInternals = true;
        } else if (arg == "-bindings") {
            config.outputBindings = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (config.inputFile.empty()) {
        fprintf(stderr, "Error: No input file specified\n\n");
        PrintUsage(argv[0]);
        return 1;
    }

    // Read input file
    std::string source = ReadFile(config.inputFile);
    if (source.empty()) {
        fprintf(stderr, "Error: Could not read file '%s'\n", config.inputFile.c_str());
        return 1;
    }

    // Get base name for output files
    fs::path inputPath(config.inputFile);
    std::string baseName = inputPath.stem().string();

    // Set up module search paths
    BWSL::ClearModuleSearchPaths();

    // Add user-specified module paths first (highest priority)
    for (const auto& modulePath : config.modulePaths) {
        fs::path absPath = fs::absolute(modulePath);
        BWSL::AddModuleSearchPath(absPath.string());
    }

    // Add input file's directory (allows finding modules relative to the shader)
    fs::path absoluteInputPath = fs::absolute(inputPath);
    fs::path inputDir = absoluteInputPath.parent_path();
    BWSL::AddModuleSearchPath(inputDir.string());

    // Create output directory if needed
    if (!config.outputDir.empty() && config.outputDir != ".") {
        fs::create_directories(config.outputDir);
    }

    if (config.verbose) {
        printf("BWSL Compiler\n");
        printf("Input: %s\n", config.inputFile.c_str());
        printf("Output: %s\n", config.outputDir.c_str());
        if (!config.modulePaths.empty()) {
            printf("Module paths:\n");
            for (const auto& p : config.modulePaths) {
                printf("  %s\n", p.c_str());
            }
        }
        printf("\n");
    }

    // Parse the BWSL file
    if (config.verbose) {
        printf("Parsing...\n");
    }

    // Load render config (from file or use default)
    if (config.renderConfigPath.empty()) {
        config.renderConfig = CreateDefaultRenderConfig(baseName);
    } else {
        auto parseResult = RenderConfigParser::ParseFile(config.renderConfigPath);
        if (!parseResult.success) {
            fprintf(stderr, "Error parsing render config: %s\n", parseResult.error.c_str());
            return 1;
        }
        config.renderConfig = std::move(parseResult.config);
        config.geometryConfig = parseResult.geometry;

        if (config.verbose) {
            printf("Loaded render config '%s' with %zu passes\n",
                   config.renderConfig.name.c_str(),
                   config.renderConfig.passes.size());
            if (config.geometryConfig.type == RenderConfigParser::GeometryConfig::Type::Instanced) {
                printf("  Geometry: instanced with %u instances\n", config.geometryConfig.instanceCount);
            }
        }
    }

    // Split source into lines for error reporting
    std::vector<std::string> sourceLines = SplitLines(source);

    // Start timing
    using Clock = std::chrono::high_resolution_clock;
    auto totalStart = Clock::now();
    TimingInfo timing;

    // Lexer initialization
    auto lexStart = Clock::now();
    CompilationContext context;
    TokenStream stream;
    stream.Init(&context.arena, source.c_str(), source.length());
    Lexer lexer(source, stream);
    lexer.Tokenize();
    auto lexEnd = Clock::now();
    timing.lexMs = std::chrono::duration<double, std::milli>(lexEnd - lexStart).count();

    // Parsing
    auto parseStart = Clock::now();
    Parser parser;
    parser.Init(&lexer, &stream, &context);
    SymbolTable::InitFromRenderConfig(&parser.symbolTable, config.renderConfig);

    // Check first token to determine file type (Init already advanced to first token)
    bool isModule = (parser.CurrentTokenType() == TokenType::MODULE);

    if (isModule) {
        // Parse as module file
        (void)parser.ParseModuleFile();
    } else {
        // Parse as pipeline
        (void)parser.ParsePipeline();
    }
    auto parseEnd = Clock::now();
    timing.parseMs = std::chrono::duration<double, std::milli>(parseEnd - parseStart).count();

    if (parser.hadError) {
        fprintf(stderr, "\nParse failed with %u error(s):\n", parser.errors.count);

        // Print each error with context (limit to 10 errors)
        for (u32 i = 0; i < parser.errors.count && i < 10; i++) {
            PrintErrorWithContext(parser.errors[i], sourceLines, source, stream);
        }

        if (parser.errors.count > 10) {
            fprintf(stderr, "\n  ... and %u more errors\n", parser.errors.count - 10);
        }

        return 1;
    }

    // Handle module files differently - they don't have pipelines to compile
    if (isModule) {
        printf("Module '%s' parsed successfully.\n", baseName.c_str());
        printf("Note: Modules define reusable functions/structs but have no shaders to compile.\n");
        printf("To compile shaders, use a pipeline file that imports this module.\n");
        return 0;
    }

    if (context.ast.pipelines.count == 0) {
        fprintf(stderr, "Error: No pipeline found in '%s'\n", config.inputFile.c_str());
        return 1;
    }

    NodeRef originalPipelineRef = context.root;
    VariantSelectionData variantSelection;
    std::string variantError;
    if (!parser.BuildVariantSelection(originalPipelineRef, nullptr, 0, false,
                                      config.variantOverrides, &variantSelection,
                                      &variantError)) {
        fprintf(stderr, "Error: %s\n", variantError.c_str());
        return 1;
    }

    VariantReflectionData variantReflection;
    if (!parser.BuildVariantReflection(originalPipelineRef, &variantSelection,
                                       &variantReflection, &variantError)) {
        fprintf(stderr, "Error: %s\n", variantError.c_str());
        return 1;
    }

    if (config.dumpVariantSpace) {
        printf("%s\n", SerializeVariantReflectionJson(variantReflection).c_str());
        return 0;
    }

    NodeRef specializedPipelineRef = parser.SpecializePipelineForVariants(originalPipelineRef,
                                                                          variantSelection,
                                                                          &variantError);
    if (specializedPipelineRef.IsNull()) {
        fprintf(stderr, "Error: %s\n", variantError.c_str());
        return 1;
    }

    const PipelineData& pipeline = context.ast.GetPipeline(specializedPipelineRef);

    // Get source base for string lookups
    const char* sourceBase = lexer.GetSourceBase();

    ComputeGraphCompileResult graphResult = CompileComputeGraph(
        context.ast, context.ast.GetPipeline(originalPipelineRef), config.renderConfig, sourceBase);
    if (!graphResult.success) {
        fprintf(stderr, "Error: %s\n", graphResult.error.c_str());
        return 1;
    }

    // Helper to get string from ArenaString
    // Note: pass/pipeline names are stored as hash-only in the AST,
    // so we fall back to an index-based name for output files
    auto getString = [sourceBase](const ArenaString& str, u32 fallbackIndex = 0) -> std::string {
        if (!str.isHashOnly() && sourceBase) {
            return std::string(str.view(sourceBase));
        }
        // Try reverse lookup for built-in names
        std::string lookup = str.ToString(sourceBase);
        if (lookup.find("<hash:") == std::string::npos) {
            return lookup;
        }
        // Fallback to index-based name
        char buf[32];
        snprintf(buf, sizeof(buf), "pass%u", fallbackIndex);
        return buf;
    };

    if (config.verbose) {
        printf("Found pipeline '%s' with %u passes\n\n",
               getString(pipeline.name).c_str(),
               pipeline.passes.count);
    }

    int compiledCount = 0;
    int errorCount = 0;

    // Compile each pass
    for (u32 passIdx = 0; passIdx < pipeline.passes.count; passIdx++) {
        const PassData& pass = context.ast.GetPass(pipeline.passes[passIdx]);
        std::string passName = getString(pass.name, passIdx);

        // Check pass filter
        if (!config.passFilter.empty() && passName != config.passFilter) {
            continue;
        }

        printf("Compiling pass '%s'...\n", passName.c_str());

        // Create varying context for this pass - shared between vertex and fragment
        // Vertex shader populates it, fragment shader uses it to resolve input.xxx
        PassVaryingContext passVaryings;
        IRAnalysis vertexReflectionAnalysis{};
        IRAnalysis fragmentReflectionAnalysis{};
        IRAnalysis computeReflectionAnalysis{};
        std::vector<ExplicitSamplerUse> vertexReflectionSamplerUses;
        std::vector<ExplicitSamplerUse> fragmentReflectionSamplerUses;
        std::vector<ExplicitSamplerUse> computeReflectionSamplerUses;
        bool haveVertexReflectionAnalysis = false;
        bool haveFragmentReflectionAnalysis = false;
        bool haveComputeReflectionAnalysis = false;

        // Compile vertex shader
        if ((config.stageFilter.empty() || config.stageFilter == "vertex") &&
            !pass.vertexShader.IsNull()) {

            printf("  Vertex shader:\n");
            CompileResult result = CompileShaderStage(context, parser, pass,
                                                       ShaderStage::Vertex, config.verbose, config.dumpIr, config.debugNames,
                                                       true, config.outputGlslEs, specializedPipelineRef,
                                                       pipeline.passes[passIdx], &passVaryings, config.outputInternals,
                                                       config.useDirectGles, &config.renderConfig);

            if (!result.success) {
                fprintf(stderr, "    Error: %s\n", result.error.c_str());
                errorCount++;
            } else {
                vertexReflectionAnalysis = result.analysis;
                vertexReflectionSamplerUses = result.explicitSamplerUses;
                haveVertexReflectionAnalysis = true;
                // Record timing for this shader
                timing.shaderTimings.push_back({passName + "_vert", result.timing});
                ShaderTiming& shaderTime = timing.shaderTimings.back().second;
                std::string outBase = BuildOutputBasePath(config.outputDir, baseName + "_" + passName + "_vert");

                // Always write SPIR-V (to temp if not requested as output)
                std::string spvPath = config.outputSpirv ? (outBase + ".spv") : GetTempSpirvPath();
                if (WriteBinaryFile(spvPath, result.spirv)) {
                    if (config.outputSpirv) {
                        printf("    -> %s\n", spvPath.c_str());
                    }

                    // Validate SPIR-V (unless skipped)
                    if (!config.skipValidation) {
                        auto valStart = Clock::now();
                        std::string validationError = ValidateSpirv(spvPath);
                        auto valEnd = Clock::now();
                        shaderTime.validationMs = std::chrono::duration<double, std::milli>(valEnd - valStart).count();

                        if (!validationError.empty()) {
                            fprintf(stderr, "    Error: SPIR-V validation failed:\n");
                            fprintf(stderr, "      %s\n", validationError.c_str());
                            errorCount++;
                            continue;  // Skip cross-compilation for invalid SPIR-V
                        }
                    }

                    // Cross-compile to all enabled targets in parallel
#ifdef USE_SPIRV_CROSS_LIB
                    {
                        auto crossStart = Clock::now();
                        CrossCompileResult crossResult = ParallelCrossCompile(
                            result.spirv,
                            config.outputMetal,
                            config.outputHlsl,
                            config.outputGlsl,
                            config.outputGlslEs,
                            result.hasWaveOps);
                        auto crossEnd = Clock::now();
                        (void)crossEnd; (void)crossStart; // Timing is per-target now

                        // Handle Metal output
                        if (config.outputMetal) {
                            shaderTime.metalCrossMs = crossResult.metalMs;
                            if (!crossResult.metal.empty() && crossResult.metal.find("error") == std::string::npos) {
                                std::string metalPath = outBase + ".metal";
                                if (WriteTextFile(metalPath, crossResult.metal)) {
                                    printf("    -> %s\n", metalPath.c_str());
                                }
                            } else {
                                fprintf(stderr, "    Warning: Metal cross-compilation failed\n");
                            }
                        }

                        // Handle HLSL output
                        if (config.outputHlsl) {
                            shaderTime.hlslCrossMs = crossResult.hlslMs;
                            if (!crossResult.hlsl.empty() && crossResult.hlsl.find("error") == std::string::npos) {
                                std::string hlslPath = outBase + ".hlsl";
                                if (WriteTextFile(hlslPath, crossResult.hlsl)) {
                                    printf("    -> %s\n", hlslPath.c_str());
                                }
                            } else {
                                fprintf(stderr, "    Warning: HLSL cross-compilation failed\n");
                            }
                        }

                        // Handle GLSL output
                        if (config.outputGlsl) {
                            shaderTime.glslCrossMs = crossResult.glslMs;
                            if (!crossResult.glsl.empty() && crossResult.glsl.find("error") == std::string::npos) {
                                std::string glslPath = outBase + ".glsl";
                                if (WriteTextFile(glslPath, crossResult.glsl)) {
                                    printf("    -> %s\n", glslPath.c_str());
                                }
                            } else {
                                fprintf(stderr, "    Warning: GLSL cross-compilation failed\n");
                            }
                        }

                        // Handle GLSL ES output
                        if (config.outputGlslEs) {
                            // Use direct GLES output if available (bypasses SPIRV-Cross)
                            std::string glesSource = config.useDirectGles
                                ? result.directGlesSource
                                : crossResult.glslEs;
                            double glesMs = config.useDirectGles
                                ? result.timing.glslEsCrossMs
                                : crossResult.glslEsMs;

                            shaderTime.glslEsCrossMs = glesMs;
                            if (!glesSource.empty() && glesSource.find("error") == std::string::npos) {
                                std::string glslEsPath = outBase + ".gles";
                                if (WriteTextFile(glslEsPath, glesSource)) {
                                    printf("    -> %s\n", glslEsPath.c_str());
                                }

                                std::string json = BuildWebGLSidecarJson(
                                    pass, parser.symbolTable, result.analysis,
                                    sourceBase, config.geometryConfig, true,
                                    &context.ast, &pipeline);
                                std::string jsonPath = outBase + ".json";
                                if (WriteTextFile(jsonPath, json)) {
                                    printf("    -> %s\n", jsonPath.c_str());
                                }
                            } else {
                                fprintf(stderr, "    Warning: GLSL ES cross-compilation failed\n");
                            }
                        }
                    }
#else
                    // Fallback: Sequential CLI-based cross-compilation
                    if (config.outputMetal) {
                        auto metalStart = Clock::now();
                        std::string metalSource = CrossCompileToMetal(spvPath);
                        auto metalEnd = Clock::now();
                        shaderTime.metalCrossMs = std::chrono::duration<double, std::milli>(metalEnd - metalStart).count();
                        if (!metalSource.empty() && metalSource.find("error") == std::string::npos) {
                            std::string metalPath = outBase + ".metal";
                            if (WriteTextFile(metalPath, metalSource)) printf("    -> %s\n", metalPath.c_str());
                        } else fprintf(stderr, "    Warning: Metal cross-compilation failed\n");
                    }
                    if (config.outputHlsl) {
                        auto hlslStart = Clock::now();
                        std::string hlslSource = CrossCompileToHLSL(spvPath, result.hasWaveOps);
                        auto hlslEnd = Clock::now();
                        shaderTime.hlslCrossMs = std::chrono::duration<double, std::milli>(hlslEnd - hlslStart).count();
                        if (!hlslSource.empty() && hlslSource.find("error") == std::string::npos) {
                            std::string hlslPath = outBase + ".hlsl";
                            if (WriteTextFile(hlslPath, hlslSource)) printf("    -> %s\n", hlslPath.c_str());
                        } else fprintf(stderr, "    Warning: HLSL cross-compilation failed\n");
                    }
                    if (config.outputGlsl) {
                        auto glslStart = Clock::now();
                        std::string glslSource = CrossCompileToGLSL(spvPath);
                        auto glslEnd = Clock::now();
                        shaderTime.glslCrossMs = std::chrono::duration<double, std::milli>(glslEnd - glslStart).count();
                        if (!glslSource.empty() && glslSource.find("error") == std::string::npos) {
                            std::string glslPath = outBase + ".glsl";
                            if (WriteTextFile(glslPath, glslSource)) printf("    -> %s\n", glslPath.c_str());
                        } else fprintf(stderr, "    Warning: GLSL cross-compilation failed\n");
                    }
                    if (config.outputGlslEs) {
                        auto glslEsStart = Clock::now();
                        std::string glslEsSource = CrossCompileToGLSLCLI(spvPath, 300);
                        auto glslEsEnd = Clock::now();
                        shaderTime.glslEsCrossMs = std::chrono::duration<double, std::milli>(glslEsEnd - glslEsStart).count();
                        if (!glslEsSource.empty() && glslEsSource.find("error") == std::string::npos) {
                            std::string glslEsPath = outBase + ".gles";
                            if (WriteTextFile(glslEsPath, glslEsSource)) printf("    -> %s\n", glslEsPath.c_str());
                            std::string json = BuildWebGLSidecarJson(
                                pass, parser.symbolTable, result.analysis,
                                sourceBase, config.geometryConfig, true,
                                &context.ast, &pipeline);
                            std::string jsonPath = outBase + ".json";
                            if (WriteTextFile(jsonPath, json)) printf("    -> %s\n", jsonPath.c_str());
                        } else fprintf(stderr, "    Warning: GLSL ES cross-compilation failed\n");
                    }
#endif

                    // Output internals JSON (SPIR-V disassembly + BWSL IR dump)
                    if (config.outputInternals && !result.irDump.empty()) {
                        std::string spirvDis = GetSpirvDisassembly(spvPath);
                        std::string internalsJson = "{\n";
                        internalsJson += "  \"pass\": \"" + EscapeJsonString(passName) + "\",\n";
                        internalsJson += "  \"stage\": \"vertex\",\n";
                        internalsJson += "  \"ir\": \"" + EscapeJsonString(result.irDump) + "\",\n";
                        internalsJson += "  \"spirv_dis\": \"" + EscapeJsonString(spirvDis) + "\"\n";
                        internalsJson += "}\n";

                        std::string internalsPath = outBase + ".internals.json";
                        if (WriteTextFile(internalsPath, internalsJson)) {
                            printf("    -> %s\n", internalsPath.c_str());
                        }
                    }

                    // Update total time to include validation and cross-compilation
                    shaderTime.totalMs = shaderTime.irLoweringMs + shaderTime.cfgSsaMs + shaderTime.spirvGenMs +
                                         shaderTime.validationMs + shaderTime.metalCrossMs + shaderTime.hlslCrossMs +
                                         shaderTime.glslCrossMs + shaderTime.glslEsCrossMs;

                    compiledCount++;
                } else {
                    fprintf(stderr, "    Error: Could not write SPIR-V file\n");
                    errorCount++;
                }
            }
        }

        // Compile fragment shader
        if ((config.stageFilter.empty() || config.stageFilter == "fragment") &&
            !pass.fragmentShader.IsNull()) {

            printf("  Fragment shader:\n");
            fflush(stdout);
            CompileResult result = CompileShaderStage(context, parser, pass,
                                                       ShaderStage::Fragment, config.verbose, config.dumpIr, config.debugNames,
                                                       true, false, specializedPipelineRef,
                                                       pipeline.passes[passIdx], &passVaryings, config.outputInternals,
                                                       config.useDirectGles, &config.renderConfig);  // Use same varying context populated by vertex shader

            if (!result.success) {
                fprintf(stderr, "    Error: %s\n", result.error.c_str());
                errorCount++;
            } else {
                fragmentReflectionAnalysis = result.analysis;
                fragmentReflectionSamplerUses = result.explicitSamplerUses;
                haveFragmentReflectionAnalysis = true;
                // Record timing for this shader
                timing.shaderTimings.push_back({passName + "_frag", result.timing});
                ShaderTiming& shaderTime = timing.shaderTimings.back().second;
                std::string outBase = BuildOutputBasePath(config.outputDir, baseName + "_" + passName + "_frag");

                // Always write SPIR-V (to temp if not requested as output)
                std::string spvPath = config.outputSpirv ? (outBase + ".spv") : GetTempSpirvPath();
                if (WriteBinaryFile(spvPath, result.spirv)) {
                    if (config.outputSpirv) {
                        printf("    -> %s\n", spvPath.c_str());
                    }

                    // Validate SPIR-V (unless skipped)
                    if (!config.skipValidation) {
                        auto valStart = Clock::now();
                        std::string validationError = ValidateSpirv(spvPath);
                        auto valEnd = Clock::now();
                        shaderTime.validationMs = std::chrono::duration<double, std::milli>(valEnd - valStart).count();

                        if (!validationError.empty()) {
                            fprintf(stderr, "    Error: SPIR-V validation failed:\n");
                            fprintf(stderr, "      %s\n", validationError.c_str());
                            errorCount++;
                            continue;  // Skip cross-compilation for invalid SPIR-V
                        }
                    }

                    // Cross-compile to all enabled targets in parallel
#ifdef USE_SPIRV_CROSS_LIB
                    {
                        CrossCompileResult crossResult = ParallelCrossCompile(
                            result.spirv,
                            config.outputMetal,
                            config.outputHlsl,
                            config.outputGlsl,
                            config.outputGlslEs,
                            result.hasWaveOps);

                        if (config.outputMetal) {
                            shaderTime.metalCrossMs = crossResult.metalMs;
                            if (!crossResult.metal.empty() && crossResult.metal.find("error") == std::string::npos) {
                                std::string metalPath = outBase + ".metal";
                                if (WriteTextFile(metalPath, crossResult.metal)) printf("    -> %s\n", metalPath.c_str());
                            } else fprintf(stderr, "    Warning: Metal cross-compilation failed\n");
                        }
                        if (config.outputHlsl) {
                            shaderTime.hlslCrossMs = crossResult.hlslMs;
                            if (!crossResult.hlsl.empty() && crossResult.hlsl.find("error") == std::string::npos) {
                                std::string hlslPath = outBase + ".hlsl";
                                if (WriteTextFile(hlslPath, crossResult.hlsl)) printf("    -> %s\n", hlslPath.c_str());
                            } else fprintf(stderr, "    Warning: HLSL cross-compilation failed\n");
                        }
                        if (config.outputGlsl) {
                            shaderTime.glslCrossMs = crossResult.glslMs;
                            if (!crossResult.glsl.empty() && crossResult.glsl.find("error") == std::string::npos) {
                                std::string glslPath = outBase + ".glsl";
                                if (WriteTextFile(glslPath, crossResult.glsl)) printf("    -> %s\n", glslPath.c_str());
                            } else fprintf(stderr, "    Warning: GLSL cross-compilation failed\n");
                        }
                        if (config.outputGlslEs) {
                            // Use direct GLES output if available (bypasses SPIRV-Cross)
                            std::string glesSource = config.useDirectGles
                                ? result.directGlesSource
                                : crossResult.glslEs;
                            double glesMs = config.useDirectGles
                                ? result.timing.glslEsCrossMs
                                : crossResult.glslEsMs;

                            shaderTime.glslEsCrossMs = glesMs;
                            if (!glesSource.empty() && glesSource.find("error") == std::string::npos) {
                                std::string glslEsPath = outBase + ".gles";
                                if (WriteTextFile(glslEsPath, glesSource)) printf("    -> %s\n", glslEsPath.c_str());
                                std::string json = BuildWebGLSidecarJson(
                                    pass, parser.symbolTable, result.analysis,
                                    sourceBase, config.geometryConfig, false,
                                    &context.ast, &pipeline);
                                std::string jsonPath = outBase + ".json";
                                if (WriteTextFile(jsonPath, json)) printf("    -> %s\n", jsonPath.c_str());
                            } else fprintf(stderr, "    Warning: GLSL ES cross-compilation failed\n");
                        }
                    }
#else
                    // Fallback: Sequential CLI-based cross-compilation
                    if (config.outputMetal) {
                        auto metalStart = Clock::now();
                        std::string metalSource = CrossCompileToMetal(spvPath);
                        auto metalEnd = Clock::now();
                        shaderTime.metalCrossMs = std::chrono::duration<double, std::milli>(metalEnd - metalStart).count();
                        if (!metalSource.empty() && metalSource.find("error") == std::string::npos) {
                            std::string metalPath = outBase + ".metal";
                            if (WriteTextFile(metalPath, metalSource)) printf("    -> %s\n", metalPath.c_str());
                        } else fprintf(stderr, "    Warning: Metal cross-compilation failed\n");
                    }
                    if (config.outputHlsl) {
                        auto hlslStart = Clock::now();
                        std::string hlslSource = CrossCompileToHLSL(spvPath, result.hasWaveOps);
                        auto hlslEnd = Clock::now();
                        shaderTime.hlslCrossMs = std::chrono::duration<double, std::milli>(hlslEnd - hlslStart).count();
                        if (!hlslSource.empty() && hlslSource.find("error") == std::string::npos) {
                            std::string hlslPath = outBase + ".hlsl";
                            if (WriteTextFile(hlslPath, hlslSource)) printf("    -> %s\n", hlslPath.c_str());
                        } else fprintf(stderr, "    Warning: HLSL cross-compilation failed\n");
                    }
                    if (config.outputGlsl) {
                        auto glslStart = Clock::now();
                        std::string glslSource = CrossCompileToGLSL(spvPath);
                        auto glslEnd = Clock::now();
                        shaderTime.glslCrossMs = std::chrono::duration<double, std::milli>(glslEnd - glslStart).count();
                        if (!glslSource.empty() && glslSource.find("error") == std::string::npos) {
                            std::string glslPath = outBase + ".glsl";
                            if (WriteTextFile(glslPath, glslSource)) printf("    -> %s\n", glslPath.c_str());
                        } else fprintf(stderr, "    Warning: GLSL cross-compilation failed\n");
                    }
                    if (config.outputGlslEs) {
                        auto glslEsStart = Clock::now();
                        std::string glslEsSource = CrossCompileToGLSLCLI(spvPath, 300);
                        auto glslEsEnd = Clock::now();
                        shaderTime.glslEsCrossMs = std::chrono::duration<double, std::milli>(glslEsEnd - glslEsStart).count();
                        if (!glslEsSource.empty() && glslEsSource.find("error") == std::string::npos) {
                            std::string glslEsPath = outBase + ".gles";
                            if (WriteTextFile(glslEsPath, glslEsSource)) printf("    -> %s\n", glslEsPath.c_str());
                            std::string json = BuildWebGLSidecarJson(
                                pass, parser.symbolTable, result.analysis,
                                sourceBase, config.geometryConfig, false,
                                &context.ast, &pipeline);
                            std::string jsonPath = outBase + ".json";
                            if (WriteTextFile(jsonPath, json)) printf("    -> %s\n", jsonPath.c_str());
                        } else fprintf(stderr, "    Warning: GLSL ES cross-compilation failed\n");
                    }
#endif

                    // Output internals JSON (SPIR-V disassembly + BWSL IR dump)
                    if (config.outputInternals && !result.irDump.empty()) {
                        std::string spirvDis = GetSpirvDisassembly(spvPath);
                        std::string internalsJson = "{\n";
                        internalsJson += "  \"pass\": \"" + EscapeJsonString(passName) + "\",\n";
                        internalsJson += "  \"stage\": \"fragment\",\n";
                        internalsJson += "  \"ir\": \"" + EscapeJsonString(result.irDump) + "\",\n";
                        internalsJson += "  \"spirv_dis\": \"" + EscapeJsonString(spirvDis) + "\"\n";
                        internalsJson += "}\n";

                        std::string internalsPath = outBase + ".internals.json";
                        if (WriteTextFile(internalsPath, internalsJson)) {
                            printf("    -> %s\n", internalsPath.c_str());
                        }
                    }

                    // Update total time to include validation and cross-compilation
                    shaderTime.totalMs = shaderTime.irLoweringMs + shaderTime.cfgSsaMs + shaderTime.spirvGenMs +
                                         shaderTime.validationMs + shaderTime.metalCrossMs + shaderTime.hlslCrossMs +
                                         shaderTime.glslCrossMs + shaderTime.glslEsCrossMs;

                    compiledCount++;
                } else {
                    fprintf(stderr, "    Error: Could not write SPIR-V file\n");
                    errorCount++;
                }
            }
        }

        // Compile compute shader
        if ((config.stageFilter.empty() || config.stageFilter == "compute") &&
            !pass.computeShader.IsNull()) {

            printf("  Compute shader:\n");
            fflush(stdout);
            CompileResult result = CompileShaderStage(context, parser, pass,
                                                       ShaderStage::Compute, config.verbose, config.dumpIr, config.debugNames,
                                                       true, false, specializedPipelineRef,
                                                       pipeline.passes[passIdx], nullptr, config.outputInternals,
                                                       config.useDirectGles, &config.renderConfig);

            if (!result.success) {
                fprintf(stderr, "    Error: %s\n", result.error.c_str());
                errorCount++;
            } else {
                computeReflectionAnalysis = result.analysis;
                computeReflectionSamplerUses = result.explicitSamplerUses;
                haveComputeReflectionAnalysis = true;
                // Record timing for this shader
                timing.shaderTimings.push_back({passName + "_comp", result.timing});
                ShaderTiming& shaderTime = timing.shaderTimings.back().second;
                std::string outBase = BuildOutputBasePath(config.outputDir, baseName + "_" + passName + "_comp");

                // Always write SPIR-V (to temp if not requested as output)
                std::string spvPath = config.outputSpirv ? (outBase + ".spv") : GetTempSpirvPath();
                if (WriteBinaryFile(spvPath, result.spirv)) {
                    if (config.outputSpirv) {
                        printf("    -> %s\n", spvPath.c_str());
                    }

                    // Validate SPIR-V (unless skipped)
                    if (!config.skipValidation) {
                        auto valStart = Clock::now();
                        std::string validationError = ValidateSpirv(spvPath);
                        auto valEnd = Clock::now();
                        shaderTime.validationMs = std::chrono::duration<double, std::milli>(valEnd - valStart).count();

                        if (!validationError.empty()) {
                            fprintf(stderr, "    Error: SPIR-V validation failed:\n");
                            fprintf(stderr, "      %s\n", validationError.c_str());
                            errorCount++;
                            continue;  // Skip cross-compilation for invalid SPIR-V
                        }
                    }

                    // Cross-compile to all enabled targets in parallel
#ifdef USE_SPIRV_CROSS_LIB
                    {
                        CrossCompileResult crossResult = ParallelCrossCompile(
                            result.spirv,
                            config.outputMetal,
                            config.outputHlsl,
                            config.outputGlsl,
                            config.outputGlslEs,
                            result.hasWaveOps);

                        if (config.outputMetal) {
                            shaderTime.metalCrossMs = crossResult.metalMs;
                            if (!crossResult.metal.empty() && crossResult.metal.find("error") == std::string::npos) {
                                std::string metalPath = outBase + ".metal";
                                if (WriteTextFile(metalPath, crossResult.metal)) printf("    -> %s\n", metalPath.c_str());
                            } else fprintf(stderr, "    Warning: Metal cross-compilation failed\n");
                        }
                        if (config.outputHlsl) {
                            shaderTime.hlslCrossMs = crossResult.hlslMs;
                            if (!crossResult.hlsl.empty() && crossResult.hlsl.find("error") == std::string::npos) {
                                std::string hlslPath = outBase + ".hlsl";
                                if (WriteTextFile(hlslPath, crossResult.hlsl)) printf("    -> %s\n", hlslPath.c_str());
                            } else fprintf(stderr, "    Warning: HLSL cross-compilation failed\n");
                        }
                        if (config.outputGlsl) {
                            shaderTime.glslCrossMs = crossResult.glslMs;
                            if (!crossResult.glsl.empty() && crossResult.glsl.find("error") == std::string::npos) {
                                std::string glslPath = outBase + ".glsl";
                                if (WriteTextFile(glslPath, crossResult.glsl)) printf("    -> %s\n", glslPath.c_str());
                            } else fprintf(stderr, "    Warning: GLSL cross-compilation failed\n");
                        }
                        if (config.outputGlslEs) {
                            // Use direct GLES output if available (bypasses SPIRV-Cross)
                            std::string glesSource = config.useDirectGles
                                ? result.directGlesSource
                                : crossResult.glslEs;
                            double glesMs = config.useDirectGles
                                ? result.timing.glslEsCrossMs
                                : crossResult.glslEsMs;
                            shaderTime.glslEsCrossMs = glesMs;
                            if (!glesSource.empty() && glesSource.find("error") == std::string::npos) {
                                std::string glslEsPath = outBase + ".gles";
                                if (WriteTextFile(glslEsPath, glesSource)) printf("    -> %s\n", glslEsPath.c_str());
                            } else fprintf(stderr, "    Warning: GLSL ES cross-compilation failed\n");
                        }
                    }
#else
                    // Fallback: Sequential CLI-based cross-compilation
                    if (config.outputMetal) {
                        auto metalStart = Clock::now();
                        std::string metalSource = CrossCompileToMetal(spvPath);
                        auto metalEnd = Clock::now();
                        shaderTime.metalCrossMs = std::chrono::duration<double, std::milli>(metalEnd - metalStart).count();
                        if (!metalSource.empty() && metalSource.find("error") == std::string::npos) {
                            std::string metalPath = outBase + ".metal";
                            if (WriteTextFile(metalPath, metalSource)) printf("    -> %s\n", metalPath.c_str());
                        } else fprintf(stderr, "    Warning: Metal cross-compilation failed\n");
                    }
                    if (config.outputHlsl) {
                        auto hlslStart = Clock::now();
                        std::string hlslSource = CrossCompileToHLSL(spvPath, result.hasWaveOps);
                        auto hlslEnd = Clock::now();
                        shaderTime.hlslCrossMs = std::chrono::duration<double, std::milli>(hlslEnd - hlslStart).count();
                        if (!hlslSource.empty() && hlslSource.find("error") == std::string::npos) {
                            std::string hlslPath = outBase + ".hlsl";
                            if (WriteTextFile(hlslPath, hlslSource)) printf("    -> %s\n", hlslPath.c_str());
                        } else fprintf(stderr, "    Warning: HLSL cross-compilation failed\n");
                    }
                    if (config.outputGlsl) {
                        auto glslStart = Clock::now();
                        std::string glslSource = CrossCompileToGLSL(spvPath);
                        auto glslEnd = Clock::now();
                        shaderTime.glslCrossMs = std::chrono::duration<double, std::milli>(glslEnd - glslStart).count();
                        if (!glslSource.empty() && glslSource.find("error") == std::string::npos) {
                            std::string glslPath = outBase + ".glsl";
                            if (WriteTextFile(glslPath, glslSource)) printf("    -> %s\n", glslPath.c_str());
                        } else fprintf(stderr, "    Warning: GLSL cross-compilation failed\n");
                    }
                    if (config.outputGlslEs) {
                        auto glslEsStart = Clock::now();
                        std::string glslEsSource = CrossCompileToGLSLCLI(spvPath, 300);
                        auto glslEsEnd = Clock::now();
                        shaderTime.glslEsCrossMs = std::chrono::duration<double, std::milli>(glslEsEnd - glslEsStart).count();
                        if (!glslEsSource.empty() && glslEsSource.find("error") == std::string::npos) {
                            std::string glslEsPath = outBase + ".gles";
                            if (WriteTextFile(glslEsPath, glslEsSource)) printf("    -> %s\n", glslEsPath.c_str());
                        } else fprintf(stderr, "    Warning: GLSL ES cross-compilation failed\n");
                    }
#endif

                    // Output internals JSON (SPIR-V disassembly + BWSL IR dump)
                    if (config.outputInternals && !result.irDump.empty()) {
                        std::string spirvDis = GetSpirvDisassembly(spvPath);
                        std::string internalsJson = "{\n";
                        internalsJson += "  \"pass\": \"" + EscapeJsonString(passName) + "\",\n";
                        internalsJson += "  \"stage\": \"compute\",\n";
                        internalsJson += "  \"ir\": \"" + EscapeJsonString(result.irDump) + "\",\n";
                        internalsJson += "  \"spirv_dis\": \"" + EscapeJsonString(spirvDis) + "\"\n";
                        internalsJson += "}\n";

                        std::string internalsPath = outBase + ".internals.json";
                        if (WriteTextFile(internalsPath, internalsJson)) {
                            printf("    -> %s\n", internalsPath.c_str());
                        }
                    }

                    // Update total time to include validation and cross-compilation
                    shaderTime.totalMs = shaderTime.irLoweringMs + shaderTime.cfgSsaMs + shaderTime.spirvGenMs +
                                         shaderTime.validationMs + shaderTime.metalCrossMs + shaderTime.hlslCrossMs +
                                         shaderTime.glslCrossMs + shaderTime.glslEsCrossMs;

                    compiledCount++;
                } else {
                    fprintf(stderr, "    Error: Could not write SPIR-V file\n");
                    errorCount++;
                }
            }
        }

        if (config.outputInternals || config.outputBindings) {
            ResourceReflectionConfig reflectionConfig;
            reflectionConfig.vertexPullingMode = config.outputGlslEs
                ? ResourceReflectionConfig::VertexPullingMode::Disabled
                : ResourceReflectionConfig::VertexPullingMode::SeparateBuffers;
            reflectionConfig.attributeMask = 0;
            if (!config.outputGlslEs) {
                reflectionConfig.attributeMask = haveVertexReflectionAnalysis
                    ? static_cast<u8>(vertexReflectionAnalysis.usedAttributeMask)
                    : BuildPassAttributeMask(context.ast, specializedPipelineRef, pipeline.passes[passIdx]);
            }
            reflectionConfig.baseBufferBinding = 0;
            reflectionConfig.descriptorSet = 0;

            std::vector<ExplicitSamplerUse> reflectionSamplerUses;
            reflectionSamplerUses.insert(reflectionSamplerUses.end(),
                                         vertexReflectionSamplerUses.begin(),
                                         vertexReflectionSamplerUses.end());
            reflectionSamplerUses.insert(reflectionSamplerUses.end(),
                                         fragmentReflectionSamplerUses.begin(),
                                         fragmentReflectionSamplerUses.end());
            reflectionSamplerUses.insert(reflectionSamplerUses.end(),
                                         computeReflectionSamplerUses.begin(),
                                         computeReflectionSamplerUses.end());

            const std::vector<ReflectedResourceBinding> bindings =
                BuildResolvedResourceReflection(&context.ast,
                                               &pipeline,
                                               &pass,
                                               parser.symbolTable,
                                               sourceBase,
                                               haveVertexReflectionAnalysis ? &vertexReflectionAnalysis : nullptr,
                                               haveFragmentReflectionAnalysis ? &fragmentReflectionAnalysis : nullptr,
                                               haveComputeReflectionAnalysis ? &computeReflectionAnalysis : nullptr,
                                               reflectionConfig,
                                               reflectionSamplerUses.empty() ? nullptr : &reflectionSamplerUses);

            std::string bindingsPath = BuildOutputBasePath(config.outputDir, baseName + "_" + passName) + ".bindings.json";
            std::string bindingsJson = BuildResourceBindingsJson(passName, bindings);
            if (WriteTextFile(bindingsPath, bindingsJson)) {
                printf("  -> %s\n", bindingsPath.c_str());
            }
        }
    }

    printf("\nDone: %d shaders compiled", compiledCount);
    if (errorCount > 0) {
        printf(", %d errors", errorCount);
    }
    printf("\n");

    // Calculate and print timing if requested
    if (config.showTiming) {
        auto totalEnd = Clock::now();
        timing.totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
        timing.Print();
    }

    return errorCount > 0 ? 1 : 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "Unhandled exception: %s\n", e.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "Unhandled unknown exception\n");
        return 1;
    }
}
