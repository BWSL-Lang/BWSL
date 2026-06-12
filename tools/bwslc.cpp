// BWSL Compiler - Command Line Tool
// BWSL (Brawl Shader Language) is made by Alexander Presthus
// Compiles .bwsl shader files to SPIR-V and cross-compiles to Metal, HLSL, GLSL, and GLES
//
// Usage: bwslc <input.bwsl> [options]
//
// Options:
//   -o <dir>       Output directory (default: current directory)
//   -modules <dir> Add module search path (can be specified multiple times)
//   -pass <name>   Compile specific pass (default: all passes)
//   -stage <name>  Compile specific stage: vertex, fragment, compute (default: all)
//   -spv           Write generated SPIR-V files (default when no format is requested)
//   -check         Run diagnostics without writing output files
//   -errors-json    Print machine-readable diagnostics as JSON
//   -ast-json      Print parsed AST JSON and exit
//   -v             Verbose output
//   -h, --help     Show help

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <future>
#include <thread>
#include <memory>
#include <functional>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifdef USE_SPIRV_TOOLS_LIB
#include "spirv-tools/libspirv.hpp"
#endif

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

#include "bwsl_spirv_backend.h"
#include "bwsl_ir_gen.h"
#include "bwsl_ir_lowering.h"
#include "bwsl_ir_analysis.h"
#include "bwsl_cfg.h"
#include "bwsl_ssa.h"
#include "bwsl_parser_soa.h"
#include "bwsl_ast_json.h"
#include "bwsl_resource_reflection.h"
#include "bwsl_reflection_json.h"
#include "bwsl_lexer.h"
#include "bwsl_eval_soa.h"
#include "bwsl_comptime_interpreter.h"
#include "bwsl_variant_system.h"
#include "bwsl_arena.h"
#include "bwsl_mem_pool.h"
#include "bwsl_render_config.h"
#include "bwsl_compute_graph.h"

// Unity build: include all implementation files
#include "../phases/lexing/bwsl_lexer.cpp"
// #define BWSL_PARSER_TIMING  // Enable parser timing instrumentation (disabled for benchmarking)
#include "../phases/parser/bwsl_parser_soa.cpp"
#include "../phases/evaluation/bwsl_eval_soa.cpp"
#include "../phases/evaluation/bwsl_comptime_interpreter.cpp"
#include "../core/bwsl_module_cache.cpp"
#include "../phases/ir_generation/bwsl_ir_gen.cpp"
#include "../phases/ir_generation/bwsl_ir_analysis.cpp"
#include "../phases/control_flow/bwsl_cfg.cpp"
#include "../phases/ssa/bwsl_ssa.cpp"
#include "../phases/backends/spirv/bwsl_spirv_backend.cpp"
#include "../phases/backends/gles/bwsl_gles_backend.cpp"
#include "../phases/ir_generation/bwsl_compute_graph.cpp"
#include "../core/bwsl_custom_type_registry.cpp"
#include "../core/bwsl_variant_system.cpp"

#include "bwsl_tool_common.h"

// Note: SPIRV-Cross is compiled separately (spirv_cross_wrapper.cpp) to avoid
// macro conflicts with defs.h (u32, f32, f64 macros)

namespace fs = std::filesystem;

using namespace BWSL;
using namespace BWSL::IR;
using namespace BWSL::ToolCommon;

#define VERSION "0.7.5"

// ============= Configuration =============

enum class CompilerFlags {
    NONE         = 0,
    VERBOSE      = 1 << 0,
    DUMP_IR      = 1 << 1,
    OUTPUT_SPIRV = 1 << 2,
    OUTPUT_METAL = 1 << 3,
    OUTPUT_HLSL  = 1 << 4,
};

enum class ValidationMode {
    Auto,
    Strict,
    Off,
};

enum class ValidationStatus {
    Passed,
    Failed,
    ToolMissing,
};

struct ValidationResult {
    ValidationStatus status = ValidationStatus::Passed;
    std::string message;
};

struct CompilerConfig {
    std::string inputFile;
    std::string sourceFileOverride;
    std::string outputDir = ".";
    std::vector<std::string> modulePaths;  // Additional module search paths
    RenderConfig renderConfig;             // synthetic config derived from source resources
    std::string passFilter;                // Empty = all passes
    std::string stageFilter;               // Empty = all stages
    bool outputSpirv = false;              // Visible SPIR-V output
    bool outputMetal = false;              // Metal output (requires -metal flag)
    bool outputHlsl  = false;              // HLSL output (requires -hlsl flag)
    bool outputGlsl  = false;              // GLSL output (requires -glsl flag)
    bool outputGlslEs = false;             // GLSL ES output for WebGL/mobile (requires -gles flag)
    bool useDirectGles = false;            // Use direct IR→GLES backend (bypass SPIRV-Cross)
    bool verbose     = false;
    bool dumpIr      = false;
    bool debugNames  = false;              // Emit debug names in SPIR-V for easier debugging
    bool showTiming  = false;              // Print timing information
    ValidationMode validationMode = ValidationMode::Auto;
    bool outputInternals = false;          // Output IR dump + SPIR-V disassembly to JSON file
    bool outputBindings = false;           // Output resolved resource bindings JSON
    bool errorsJson = false;               // Print diagnostics as JSON for IDE integrations
    bool astJson = false;                  // Print parsed AST JSON for IDE integrations
    bool checkOnly = false;                // Run diagnostics without writing outputs
    bool readStdin = false;                // Read source from stdin instead of inputFile
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
    │ » Compiler v 0.7.5                  │
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
    printf("  -variant <k=v> Set a named variant value (repeatable)\n");
    printf("  -dump-variant-space  Print variant reflection JSON and exit\n");
    printf("  -check         Run diagnostics without writing output files\n");
    printf("  --stdin        Read source from stdin instead of the input file\n");
    printf("  --source-file <path>\n");
    printf("                 Source path used for diagnostics/module resolution with --stdin\n");
    printf("  -pass <name>   Compile specific pass (default: all passes)\n");
    printf("  -stage <name>  Compile specific stage: vertex, fragment, compute (default: all)\n");
    printf("\n");
    printf("Output artifact flags (SPIR-V is generated internally for validation/cross-compile):\n");
    printf("  -spv           Write generated SPIR-V files alongside requested artifacts\n");
    printf("                 (default artifact when no format is requested)\n");
    printf("  -metal         Generate Metal Shading Language output via SPIR-V\n");
    printf("  -hlsl          Generate HLSL (High-Level Shader Language) output via SPIR-V\n");
    printf("  -glsl          Generate GLSL output via SPIR-V (version 450)\n");
    printf("  -gles          Generate GLSL ES output for WebGL 2.0 / OpenGL ES 3.0 via SPIR-V\n");
    printf("                 (version 300 es)\n");
    printf("  -gles-direct   Generate GLSL ES directly from IR (bypass SPIRV-Cross, faster)\n");
    printf("  -webgl         Alias for -gles\n");
    printf("  -bindings      Output resolved resource bindings JSON\n");
    printf("  -errors-json   Print machine-readable diagnostics JSON for IDE integrations\n");
    printf("  -ast-json      Print parsed AST JSON for IDE integrations and exit\n");
    printf("  -all           Generate all outputs\n");
    printf("----------------------------------------\n");
    printf("Debug options:\n");
    printf("  -v             Verbose output\n");
    printf("  -timing        Print timing information\n");
    printf("  -dump-ir       Dump BWSL IR\n");
    printf("  -debug-names   Emit debug names in SPIR-V output\n");
    printf("  -validation <auto|strict|off>\n");
    printf("                 SPIR-V validation mode (default: auto)\n");
    printf("  -no-validate   Alias for -validation off (faster compilation)\n");
    printf("  -internals     Output SPIR-V disassembly and BWSL IR dump to JSON file\n");
    printf("  -h, --help     Show this help\n");
    printf("----------------------------------------\n");
    printf("\nExamples:\n");
    printf("  %s shader.bwsl                      # SPIR-V only\n", programName);
    printf("  %s shader.bwsl -metal               # Metal artifact only (SPIR-V generated internally)\n", programName);
    printf("  %s shader.bwsl -gles -spv           # WebGL artifacts + emitted SPIR-V sidecars\n", programName);
    printf("  %s shader.bwsl -metal -hlsl -gles   # Metal + HLSL + WebGL\n", programName);
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

std::string ReadStdin() {
    std::stringstream buffer;
    buffer << std::cin.rdbuf();
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

std::string SanitizeOutputStemPart(const std::string& part, const std::string& fallback) {
    std::string sanitized;
    sanitized.reserve(part.size());
    for (unsigned char ch : part) {
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-' || ch == '.') {
            sanitized.push_back(static_cast<char>(ch));
        } else if (!sanitized.empty() && sanitized.back() != '_') {
            sanitized.push_back('_');
        }
    }

    while (!sanitized.empty() && sanitized.front() == '.') {
        sanitized.erase(sanitized.begin());
    }
    while (!sanitized.empty() && sanitized.back() == '_') {
        sanitized.pop_back();
    }

    return sanitized.empty() ? fallback : sanitized;
}

const char* StageFileExtension(const char* stageName) {
    if (strcmp(stageName, "vertex") == 0) return "vert";
    if (strcmp(stageName, "fragment") == 0) return "frag";
    if (strcmp(stageName, "compute") == 0) return "comp";
    return "glsl";
}

std::string BuildPassOutputStem(const std::string& baseName,
                                const std::string& passName,
                                const std::string& passFallbackName,
                                u32 passCount) {
    if (passCount <= 1) {
        return SanitizeOutputStemPart(baseName, "shader");
    }

    std::string passPart = SanitizeOutputStemPart(passName, passFallbackName);
    return SanitizeOutputStemPart(baseName, "shader") + "_" + passPart;
}

std::string BuildStageOutputPath(const std::string& outputStemPath,
                                 const char* stageName) {
    return outputStemPath + "." + StageFileExtension(stageName);
}

std::string BuildGlslOutputPath(const std::string& outputStemPath,
                                const char* stageName,
                                const char* formatQualifier,
                                bool includeFormatQualifier) {
    if (includeFormatQualifier) {
        return outputStemPath + "." + formatQualifier + "." + StageFileExtension(stageName);
    }
    return BuildStageOutputPath(outputStemPath, stageName);
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
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::stringstream name;
    name << "bwslc_temp_" << now << "_" << std::hash<std::thread::id>{}(std::this_thread::get_id()) << ".spv";
    return (tempDir / name.str()).string();
}

struct CommandResult {
    bool launched = false;
    int exitCode = -1;
    std::string output;
};

CommandResult RunCommandWithStatus(const std::string& cmd) {
    CommandResult commandResult;
    FILE* pipe = nullptr;
#if defined(_WIN32)
    pipe = _popen(cmd.c_str(), "r");
#else
    pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return commandResult;

    commandResult.launched = true;

    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        commandResult.output += buffer;
    }

#if defined(_WIN32)
    commandResult.exitCode = _pclose(pipe);
#else
    int status = pclose(pipe);
    if (status == -1) {
        commandResult.exitCode = -1;
    } else if (WIFEXITED(status)) {
        commandResult.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        commandResult.exitCode = 128 + WTERMSIG(status);
    } else {
        commandResult.exitCode = status;
    }
#endif
    return commandResult;
}

std::string RunCommand(const std::string& cmd) {
    return RunCommandWithStatus(cmd).output;
}

std::string ShellQuote(const fs::path& path) {
    std::string text = path.string();
#if defined(_WIN32)
    std::string out = "\"";
    for (char ch : text) {
        if (ch == '"') out += "\\\"";
        else out += ch;
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char ch : text) {
        if (ch == '\'') out += "'\\''";
        else out += ch;
    }
    out += "'";
    return out;
#endif
}

std::string ExternalCommand(const fs::path& executable) {
#if defined(_WIN32)
    return "call " + ShellQuote(executable);
#else
    return ShellQuote(executable);
#endif
}

static void AddPathCandidate(std::vector<fs::path>& dirs, const fs::path& dir) {
    if (dir.empty()) return;
    if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) {
        dirs.push_back(dir);
    }
}


#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // Disable deprecation warnings for std::getenv on MSVC
#endif

std::string FindTool(const char* toolName) {
    std::vector<fs::path> dirs;

    if (const char* pathEnv = std::getenv("PATH")) {
#if defined(_WIN32)
        const char separator = ';';
#else
        const char separator = ':';
#endif
        std::stringstream paths(pathEnv);
        std::string item;
        while (std::getline(paths, item, separator)) {
            AddPathCandidate(dirs, fs::path(item));
        }
    }

    if (const char* sdk = std::getenv("VULKAN_SDK")) {
        AddPathCandidate(dirs, fs::path(sdk) / "bin");
    }

    const char* home = std::getenv("HOME");
#if defined(_WIN32)
    if (home == nullptr || home[0] == '\0') {
        home = std::getenv("USERPROFILE");
    }
#endif
    if (home != nullptr && home[0] != '\0') {
        fs::path sdkRoot = fs::path(home) / "VulkanSDK";
        std::error_code ec;
        if (fs::exists(sdkRoot, ec)) {
            for (const auto& versionDir : fs::directory_iterator(sdkRoot, ec)) {
                if (ec || !versionDir.is_directory()) continue;
                for (const auto& platformDir : fs::directory_iterator(versionDir.path(), ec)) {
                    if (ec || !platformDir.is_directory()) continue;
                    AddPathCandidate(dirs, platformDir.path() / "bin");
                }
                AddPathCandidate(dirs, versionDir.path() / "bin");
            }
        }
    }

#if defined(_WIN32)
    const char* exeName = toolName;
    std::string withExt = std::string(toolName) + ".exe";
#else
    const char* exeName = toolName;
#endif
    AddPathCandidate(dirs, "/usr/local/bin");
    AddPathCandidate(dirs, "/opt/homebrew/bin");

    std::error_code ec;
    for (const fs::path& dir : dirs) {
#if defined(_WIN32)
        fs::path candidateWithExt = dir / withExt;
        if (fs::exists(candidateWithExt, ec) && !fs::is_directory(candidateWithExt, ec)) {
            return candidateWithExt.string();
        }
#endif
        fs::path candidate = dir / exeName;
        if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec)) {
            return candidate.string();
        }
    }

    return "";
}

#if defined(_MSC_VER)
// we are done using std::getenv (safely), so re-enable the warning
#pragma warning(pop)
#endif

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

static bool IsFailedCrossCompileSource(const std::string& source) {
    return source.empty() || source.find("error") != std::string::npos;
}

static bool IsKnownGLESFallbackError(const std::string& source) {
    return source.find("direct GLES") != std::string::npos ||
           source.find("GLSL.std.450 ModfStruct") != std::string::npos ||
           source.find("GLSL.std.450 Ldexp") != std::string::npos ||
           source.find("textureQueryLevels") != std::string::npos ||
           source.find("textureGatherOffset") != std::string::npos ||
           source.find("fine/coarse derivative") != std::string::npos;
}

static std::string SelectGLESSource(const CrossCompileResult& crossResult,
                                    const std::string& directGlesSource,
                                    double directGlesMs,
                                    bool useDirectGles,
                                    double* selectedMs) {
    if (useDirectGles) {
        if (selectedMs) *selectedMs = directGlesMs;
        return directGlesSource;
    }

    if (IsFailedCrossCompileSource(crossResult.glslEs) &&
        IsKnownGLESFallbackError(crossResult.glslEs) &&
        !directGlesSource.empty() &&
        directGlesSource.find("error") == std::string::npos) {
        if (selectedMs) *selectedMs = directGlesMs;
        return directGlesSource;
    }

    if (selectedMs) *selectedMs = crossResult.glslEsMs;
    return crossResult.glslEs;
}
#endif

static uint64_t HashSpirvWords(const std::vector<u32>& spirv) {
    uint64_t hash = 1469598103934665603ull;
    for (u32 word : spirv) {
        hash ^= static_cast<uint64_t>(word);
        hash *= 1099511628211ull;
    }
    return hash;
}

static constexpr u8 BackendCacheOptionWaveOps = 1u << 0;

#ifdef USE_SPIRV_CROSS_LIB
static constexpr u8 CrossCacheMetal = 1u << 0;
static constexpr u8 CrossCacheHlsl = 1u << 1;
static constexpr u8 CrossCacheGlsl = 1u << 2;
static constexpr u8 CrossCacheGlslEs = 1u << 3;
#endif

struct BackendCacheText {
    u32 offset = 0;
    u32 length = 0;
};

struct BackendCache {
    BWSL_Arena* arena = nullptr;

    ArenaArray<uint64_t> spirvHashes;
    ArenaArray<u32> spirvWordCounts;
    ArenaArray<u32> spirvWordOffsets;
    ArenaArray<u8> optionFlags;

    ArenaArray<u8> validationDone;
    ArenaArray<u8> validationStatuses;
    ArenaArray<BackendCacheText> validationMessages;

#ifdef USE_SPIRV_CROSS_LIB
    ArenaArray<u8> crossDoneMask;
    ArenaArray<BackendCacheText> metalSources;
    ArenaArray<BackendCacheText> hlslSources;
    ArenaArray<BackendCacheText> glslSources;
    ArenaArray<BackendCacheText> glslEsSources;
#endif

    u32* spirvWordData = nullptr;
    u32 spirvWordCount = 0;
    u32 spirvWordCapacity = 0;

    char* textData = nullptr;
    u32 textByteCount = 0;
    u32 textByteCapacity = 0;

    void Init(BWSL_Arena* cacheArena, u32 expectedEntries) {
        arena = cacheArena;
        const u32 entryCapacity = expectedEntries > 0 ? expectedEntries : 1;

        spirvHashes.Init(arena, entryCapacity);
        spirvWordCounts.Init(arena, entryCapacity);
        spirvWordOffsets.Init(arena, entryCapacity);
        optionFlags.Init(arena, entryCapacity);

        validationDone.Init(arena, entryCapacity);
        validationStatuses.Init(arena, entryCapacity);
        validationMessages.Init(arena, entryCapacity);
#ifdef USE_SPIRV_CROSS_LIB
        crossDoneMask.Init(arena, entryCapacity);
        metalSources.Init(arena, entryCapacity);
        hlslSources.Init(arena, entryCapacity);
        glslSources.Init(arena, entryCapacity);
        glslEsSources.Init(arena, entryCapacity);
#endif

        spirvWordCapacity = std::max<u32>(1024, entryCapacity * 1024);
        spirvWordData = static_cast<u32*>(
            arena->Allocate(static_cast<size_t>(spirvWordCapacity) * sizeof(u32), alignof(u32)));

        textByteCapacity = std::max<u32>(32 * 1024, entryCapacity * 16 * 1024);
        textData = static_cast<char*>(
            arena->Allocate(textByteCapacity, alignof(char)));

        if (!spirvWordData || !textData) {
            std::fprintf(stderr, "Backend cache arena initialization failed\n");
            std::abort();
        }
    }

    static u32 GrowCapacity(u32 current, u32 required) {
        u32 capacity = current > 0 ? current : 1;
        while (capacity < required) {
            const u32 next = capacity * 2;
            if (next <= capacity) {
                return required;
            }
            capacity = next;
        }
        return capacity;
    }

    bool EnsureSpirvWordCapacity(u32 additionalWords) {
        if (additionalWords <= spirvWordCapacity - spirvWordCount) {
            return true;
        }

        const u32 required = spirvWordCount + additionalWords;
        const u32 newCapacity = GrowCapacity(spirvWordCapacity, required);
        u32* newData = static_cast<u32*>(
            arena->Allocate(static_cast<size_t>(newCapacity) * sizeof(u32), alignof(u32)));
        if (!newData) {
            return false;
        }
        if (spirvWordCount > 0) {
            std::memcpy(newData, spirvWordData,
                        static_cast<size_t>(spirvWordCount) * sizeof(u32));
        }
        spirvWordData = newData;
        spirvWordCapacity = newCapacity;
        return true;
    }

    bool EnsureTextCapacity(u32 additionalBytes) {
        if (additionalBytes <= textByteCapacity - textByteCount) {
            return true;
        }

        const u32 required = textByteCount + additionalBytes;
        const u32 newCapacity = GrowCapacity(textByteCapacity, required);
        char* newData = static_cast<char*>(
            arena->Allocate(newCapacity, alignof(char)));
        if (!newData) {
            return false;
        }
        if (textByteCount > 0) {
            std::memcpy(newData, textData, textByteCount);
        }
        textData = newData;
        textByteCapacity = newCapacity;
        return true;
    }

    bool StoreText(const std::string& text, BackendCacheText& cached) {
        cached = {};
        if (text.empty()) {
            return true;
        }

        const u32 length = static_cast<u32>(text.size());
        if (!EnsureTextCapacity(length)) {
            return false;
        }

        cached.offset = textByteCount;
        cached.length = length;
        std::memcpy(textData + cached.offset, text.data(), length);
        textByteCount += length;
        return true;
    }

    std::string LoadText(BackendCacheText cached) const {
        if (cached.length == 0) {
            return {};
        }
        return std::string(textData + cached.offset, cached.length);
    }

    bool StoreValidation(u32 index, const ValidationResult& result) {
        BackendCacheText message;
        if (!StoreText(result.message, message)) {
            return false;
        }
        validationStatuses[index] = static_cast<u8>(result.status);
        validationMessages[index] = message;
        validationDone[index] = 1;
        return true;
    }

    ValidationResult LoadValidation(u32 index) const {
        ValidationResult result;
        result.status = static_cast<ValidationStatus>(validationStatuses[index]);
        result.message = LoadText(validationMessages[index]);
        return result;
    }

    u32 FindOrAdd(const std::vector<u32>& spirv, u8 options) {
        const uint64_t hash = HashSpirvWords(spirv);
        const u32 wordCount = static_cast<u32>(spirv.size());
        for (u32 i = 0; i < spirvHashes.count; i++) {
            if (spirvHashes[i] != hash ||
                spirvWordCounts[i] != wordCount ||
                optionFlags[i] != options) {
                continue;
            }

            const u32 wordOffset = spirvWordOffsets[i];
            if (wordCount == 0 ||
                std::memcmp(spirvWordData + wordOffset, spirv.data(),
                            static_cast<size_t>(wordCount) * sizeof(u32)) == 0) {
                return i;
            }
        }

        const u32 wordOffset = spirvWordCount;
        if (!EnsureSpirvWordCapacity(wordCount)) {
            std::fprintf(stderr, "Backend cache arena exhausted while storing SPIR-V words\n");
            std::abort();
        }
        if (wordCount > 0) {
            std::memcpy(spirvWordData + wordOffset, spirv.data(),
                        static_cast<size_t>(wordCount) * sizeof(u32));
            spirvWordCount += wordCount;
        }

        const u32 index = spirvHashes.count;
        spirvHashes.Push(arena, hash);
        spirvWordCounts.Push(arena, wordCount);
        spirvWordOffsets.Push(arena, wordOffset);
        optionFlags.Push(arena, options);
        validationDone.Push(arena, 0);
        validationStatuses.Push(arena, static_cast<u8>(ValidationStatus::Passed));
        validationMessages.Push(arena, BackendCacheText{});
#ifdef USE_SPIRV_CROSS_LIB
        crossDoneMask.Push(arena, 0);
        metalSources.Push(arena, BackendCacheText{});
        hlslSources.Push(arena, BackendCacheText{});
        glslSources.Push(arena, BackendCacheText{});
        glslEsSources.Push(arena, BackendCacheText{});
#endif
        return index;
    }
};

static u8 BuildBackendCacheOptions(bool hasWaveOps) {
    return hasWaveOps ? BackendCacheOptionWaveOps : 0;
}

#ifdef USE_SPIRV_CROSS_LIB
static u8 BuildCrossRequestMask(const CompilerConfig& config) {
    u8 mask = 0;
    if (config.outputMetal) mask |= CrossCacheMetal;
    if (config.outputHlsl) mask |= CrossCacheHlsl;
    if (config.outputGlsl) mask |= CrossCacheGlsl;
    if (config.outputGlslEs && !config.useDirectGles) mask |= CrossCacheGlslEs;
    return mask;
}

static CrossCompileResult ParallelCrossCompileMasked(const std::vector<u32>& spirv,
                                                     u8 mask,
                                                     bool hasWaveOps) {
    return ParallelCrossCompile(
        spirv,
        (mask & CrossCacheMetal) != 0,
        (mask & CrossCacheHlsl) != 0,
        (mask & CrossCacheGlsl) != 0,
        (mask & CrossCacheGlslEs) != 0,
        hasWaveOps);
}

static void StoreCrossCompileCache(BackendCache& cache,
                                   u32 index,
                                   u8 mask,
                                   const CrossCompileResult& result) {
    u8 storedMask = 0;
    if ((mask & CrossCacheMetal) != 0 &&
        cache.StoreText(result.metal, cache.metalSources[index])) {
        storedMask |= CrossCacheMetal;
    }
    if ((mask & CrossCacheHlsl) != 0 &&
        cache.StoreText(result.hlsl, cache.hlslSources[index])) {
        storedMask |= CrossCacheHlsl;
    }
    if ((mask & CrossCacheGlsl) != 0 &&
        cache.StoreText(result.glsl, cache.glslSources[index])) {
        storedMask |= CrossCacheGlsl;
    }
    if ((mask & CrossCacheGlslEs) != 0 &&
        cache.StoreText(result.glslEs, cache.glslEsSources[index])) {
        storedMask |= CrossCacheGlslEs;
    }
    cache.crossDoneMask[index] |= storedMask;
}

static CrossCompileResult BuildCrossCompileResultFromCache(
    const BackendCache& cache,
    u32 index,
    u8 requestMask,
    u8 computedMask,
    const CrossCompileResult& computed) {
    CrossCompileResult result;

    if ((requestMask & CrossCacheMetal) != 0) {
        result.metal = (computedMask & CrossCacheMetal) != 0
            ? computed.metal
            : cache.LoadText(cache.metalSources[index]);
        result.metalMs = (computedMask & CrossCacheMetal) ? computed.metalMs : 0.0;
    }
    if ((requestMask & CrossCacheHlsl) != 0) {
        result.hlsl = (computedMask & CrossCacheHlsl) != 0
            ? computed.hlsl
            : cache.LoadText(cache.hlslSources[index]);
        result.hlslMs = (computedMask & CrossCacheHlsl) ? computed.hlslMs : 0.0;
    }
    if ((requestMask & CrossCacheGlsl) != 0) {
        result.glsl = (computedMask & CrossCacheGlsl) != 0
            ? computed.glsl
            : cache.LoadText(cache.glslSources[index]);
        result.glslMs = (computedMask & CrossCacheGlsl) ? computed.glslMs : 0.0;
    }
    if ((requestMask & CrossCacheGlslEs) != 0) {
        result.glslEs = (computedMask & CrossCacheGlslEs) != 0
            ? computed.glslEs
            : cache.LoadText(cache.glslEsSources[index]);
        result.glslEsMs = (computedMask & CrossCacheGlslEs) ? computed.glslEsMs : 0.0;
    }

    return result;
}
#endif

static bool ProgramNeedsDirectGLESFallback(const IRProgram& program) {
    for (u32 i = 0; i < program.instructionCount; i++) {
        switch (static_cast<IR::OpCode>(program.opcodes[i])) {
            case IR::OP_LDEXP:
            case IR::OP_MODF_STRUCT:
            case IR::OP_TEX_LEVELS:
            case IR::OP_TEX_GATHER:
            case IR::OP_TEX_GATHER_OFFSET:
            case IR::OP_DDX_FINE:
            case IR::OP_DDY_FINE:
            case IR::OP_DDX_COARSE:
            case IR::OP_DDY_COARSE:
            case IR::OP_FWIDTH_FINE:
            case IR::OP_FWIDTH_COARSE:
                return true;
            default:
                break;
        }
    }
    return false;
}

// ============= IR Dump =============

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
    return ReflectionJson::EscapeJsonString(str);
}

static bool IsErrorsJsonArg(const std::string& arg) {
    return arg == "-errors-json" || arg == "--errors-json" ||
           arg == "-json-errors" || arg == "--json-errors";
}

static bool ArgsRequestErrorsJson(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (IsErrorsJsonArg(argv[i])) return true;
    }
    return false;
}

static bool IsCheckArg(const std::string& arg) {
    return arg == "-check" || arg == "--check";
}

static bool IsStdinArg(const std::string& arg) {
    return arg == "-stdin" || arg == "--stdin";
}

static bool IsVirtualInputFile(const std::string& inputFile) {
    return inputFile.empty() || inputFile == "<stdin>";
}

static std::string TrimDiagnosticMessage(std::string message) {
    while (!message.empty() &&
           (message.back() == '\n' || message.back() == '\r' ||
            message.back() == ' ' || message.back() == '\t')) {
        message.pop_back();
    }
    const std::string prefix = "Error:";
    if (message.rfind(prefix, 0) == 0) {
        message.erase(0, prefix.size());
        while (!message.empty() && (message.front() == ' ' || message.front() == '\t')) {
            message.erase(message.begin());
        }
    }
    return message;
}

static const char* DiagnosticSeverityTitle(DiagnosticSeverity severity) {
    switch (severity) {
        case DiagnosticSeverity::Error: return "Error";
        case DiagnosticSeverity::Warning: return "Warning";
        case DiagnosticSeverity::Note: return "Note";
        case DiagnosticSeverity::Hint: return "Hint";
        default: return "Error";
    }
}

static void CountDiagnostics(const DiagnosticStream& diagnostics,
                             u32& errorCount,
                             u32& warningCount,
                             u32& noteCount,
                             u32& hintCount) {
    errorCount = 0;
    warningCount = 0;
    noteCount = 0;
    hintCount = 0;
    for (u32 i = 0; i < diagnostics.Count(); i++) {
        switch (diagnostics.GetSeverity(i)) {
            case DiagnosticSeverity::Error: errorCount++; break;
            case DiagnosticSeverity::Warning: warningCount++; break;
            case DiagnosticSeverity::Note: noteCount++; break;
            case DiagnosticSeverity::Hint: hintCount++; break;
            default: errorCount++; break;
        }
    }
}

static void PrintDiagnosticText(const CompilerConfig& config,
                                const DiagnosticStream& diagnostics,
                                DiagnosticRef ref,
                                const TokenStream* stream = nullptr,
                                const std::vector<std::string>* sourceLines = nullptr) {
    DiagnosticSeverity severity = diagnostics.GetSeverity(ref);
    const char* severityTitle = DiagnosticSeverityTitle(severity);
    std::string code = diagnostics.GetCode(ref);
    const u32 spanFlags = diagnostics.spanFlags[ref];
    const bool hasLocation = DiagnosticSpan::HasAll(spanFlags, DiagnosticSpan::HasLocationFlag);
    std::string message = TrimDiagnosticMessage(diagnostics.FormatMessage(ref));
    std::string file = diagnostics.GetFile(ref);
    std::string pass = diagnostics.GetPass(ref);
    std::string stage = diagnostics.GetStage(ref);
    if (file.empty()) file = config.inputFile;

    if (!hasLocation) {
        fprintf(stderr, "%s[%s]: %s",
                severityTitle,
                code.c_str(),
                message.empty() ? "(no message)" : message.c_str());
        if (!file.empty()) {
            fprintf(stderr, " [%s]", file.c_str());
        }
        if (!pass.empty() || !stage.empty()) {
            fprintf(stderr, " (");
            if (!pass.empty()) fprintf(stderr, "pass %s", pass.c_str());
            if (!pass.empty() && !stage.empty()) fprintf(stderr, ", ");
            if (!stage.empty()) fprintf(stderr, "stage %s", stage.c_str());
            fprintf(stderr, ")");
        }
        fprintf(stderr, "\n");
        return;
    }

    const u32 line = diagnostics.lines[ref];
    const u32 column = diagnostics.columns[ref];
    fprintf(stderr, "\n  ┌─ %s[%s] at line %u, column %u\n",
            severityTitle, code.c_str(), line, column);
    if (!file.empty()) {
        fprintf(stderr, "  │ File: %s\n", file.c_str());
    }
    if (!pass.empty() || !stage.empty()) {
        fprintf(stderr, "  │ Context:");
        if (!pass.empty()) fprintf(stderr, " pass %s", pass.c_str());
        if (!stage.empty()) fprintf(stderr, " stage %s", stage.c_str());
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  │\n");

    if (sourceLines) {
        int startLine = std::max(1, static_cast<int>(line) - 2);
        int endLine = std::min(static_cast<int>(sourceLines->size()), static_cast<int>(line) + 2);

        for (int i = startLine; i <= endLine; i++) {
            if (i <= 0 || i > static_cast<int>(sourceLines->size())) continue;

            const std::string& sourceLine = (*sourceLines)[i - 1];
            if (i == static_cast<int>(line)) {
                fprintf(stderr, "  │ %4d │ %s\n", i, sourceLine.c_str());
                fprintf(stderr, "  │      │ ");
                for (u32 j = 1; j < column && j <= sourceLine.length(); j++) {
                    if (sourceLine[j - 1] == '\t') {
                        fprintf(stderr, "    ");
                    } else {
                        fprintf(stderr, " ");
                    }
                }
                fprintf(stderr, "^\n");
                fprintf(stderr, "  │      └─ %s\n", message.empty() ? "(no message)" : message.c_str());
            } else {
                fprintf(stderr, "  │ %4d │ %s\n", i, sourceLine.c_str());
            }
        }
    } else {
        fprintf(stderr, "  │ %s\n", message.empty() ? "(no message)" : message.c_str());
    }

    fprintf(stderr, "  │\n");

    if (stream && diagnostics.tokens[ref] != INVALID_DIAGNOSTIC_TOKEN) {
        u32 token = diagnostics.tokens[ref];
        if (!stream->IsEOF(token) && stream->GetLength(token) > 0) {
            std::string_view tokenValue = stream->GetValue(token);
            fprintf(stderr, "  │ Token: '%.*s' (type %s)\n",
                    static_cast<int>(tokenValue.length()), tokenValue.data(),
                    TokenTypeName(stream->GetType(token)));
        }
    }

    fprintf(stderr, "  └─\n");
}

static void PrintDiagnosticsText(const CompilerConfig& config,
                                 const DiagnosticStream& diagnostics,
                                 const TokenStream* stream = nullptr,
                                 const std::vector<std::string>* sourceLines = nullptr,
                                 u32 maxDiagnostics = 10) {
    if (diagnostics.Count() == 0) return;

    u32 errorCount = 0;
    u32 warningCount = 0;
    u32 noteCount = 0;
    u32 hintCount = 0;
    CountDiagnostics(diagnostics, errorCount, warningCount, noteCount, hintCount);

    fprintf(stderr, "\nDiagnostics: %u error(s), %u warning(s)", errorCount, warningCount);
    if (noteCount > 0) fprintf(stderr, ", %u note(s)", noteCount);
    if (hintCount > 0) fprintf(stderr, ", %u hint(s)", hintCount);
    fprintf(stderr, "\n");

    u32 shown = 0;
    for (u32 i = 0; i < diagnostics.Count(); i++) {
        if (shown >= maxDiagnostics) break;
        PrintDiagnosticText(config, diagnostics, i, stream, sourceLines);
        shown++;
    }

    if (diagnostics.Count() > shown) {
        fprintf(stderr, "\n  ... and %u more diagnostic(s)\n", diagnostics.Count() - shown);
    }
}

static void AppendJsonComma(std::string& json, bool& first) {
    if (!first) json += ",\n";
    first = false;
}

static void AppendJsonStringField(std::string& json,
                                  const char* name,
                                  const std::string& value,
                                  bool& first,
                                  bool omitEmpty = true) {
    if (omitEmpty && value.empty()) return;
    AppendJsonComma(json, first);
    json += "      \"";
    json += name;
    json += "\": \"";
    json += EscapeJsonString(value);
    json += "\"";
}

static void AppendJsonUintField(std::string& json,
                                const char* name,
                                u32 value,
                                bool& first) {
    AppendJsonComma(json, first);
    json += "      \"";
    json += name;
    json += "\": ";
    json += std::to_string(value);
}

static void AddStageDiagnostic(DiagnosticStream& diagnostics,
                               DiagnosticSeverity severity,
                               DiagnosticPhase phase,
                               DiagnosticMessageId messageId,
                               const std::string& message,
                               const std::string& file,
                               const std::string& pass,
                               const std::string& stage) {
    diagnostics.AddRaw(severity,
                       phase,
                       TrimDiagnosticMessage(message),
                       DiagnosticSpan{},
                       file,
                       pass,
                       stage,
                       messageId);
}

static void PrintDiagnosticsJson(const CompilerConfig& config,
                                 const DiagnosticStream& diagnostics,
                                 bool success,
                                 const TokenStream* stream = nullptr,
                                 const std::vector<std::string>* sourceLines = nullptr) {
    u32 errorCount = 0;
    u32 warningCount = 0;
    for (u32 i = 0; i < diagnostics.Count(); i++) {
        switch (diagnostics.GetSeverity(i)) {
            case DiagnosticSeverity::Error:
                errorCount++;
                break;
            case DiagnosticSeverity::Warning:
                warningCount++;
                break;
            default:
                break;
        }
    }

    std::string json;
    json += "{\n";
    json += "  \"schemaVersion\": 2,\n";
    json += "  \"tool\": \"bwslc\",\n";
    json += "  \"version\": \"" VERSION "\",\n";
    json += "  \"success\": ";
    json += success ? "true" : "false";
    json += ",\n";
    json += "  \"file\": \"" + EscapeJsonString(config.inputFile) + "\",\n";
    json += "  \"errorCount\": " + std::to_string(errorCount) + ",\n";
    json += "  \"warningCount\": " + std::to_string(warningCount) + ",\n";
    json += "  \"diagnostics\": [";

    for (u32 i = 0; i < diagnostics.Count(); i++) {
        if (i > 0) json += ",";
        json += "\n    {\n";

        bool first = true;
        DiagnosticSeverity severity = diagnostics.GetSeverity(i);
        DiagnosticPhase phase = diagnostics.GetPhase(i);
        DiagnosticMessageId messageId = diagnostics.GetMessageId(i);
        std::string file = diagnostics.GetFile(i);
        if (file.empty()) file = config.inputFile;

        AppendJsonStringField(json, "severity", DiagnosticStream::SeverityName(severity), first, false);
        AppendJsonStringField(json, "phase", DiagnosticStream::PhaseName(phase), first);
        AppendJsonStringField(json, "code", diagnostics.GetCode(i), first, false);
        AppendJsonStringField(json, "name", DiagnosticStream::MessageName(messageId), first);
        AppendJsonStringField(json, "message", TrimDiagnosticMessage(diagnostics.FormatMessage(i)), first, false);
        AppendJsonStringField(json, "file", file, first);
        AppendJsonStringField(json, "pass", diagnostics.GetPass(i), first);
        AppendJsonStringField(json, "stage", diagnostics.GetStage(i), first);
        const u32 spanFlags = diagnostics.spanFlags[i];
        if (DiagnosticSpan::HasAll(spanFlags, DiagnosticSpan::HasLocationFlag)) {
            AppendJsonUintField(json, "line", diagnostics.lines[i], first);
            AppendJsonUintField(json, "column", diagnostics.columns[i], first);
        }
        if (DiagnosticSpan::HasAll(spanFlags, DiagnosticSpan::HasEndLocationFlag)) {
            AppendJsonUintField(json, "endLine", diagnostics.endLines[i], first);
            AppendJsonUintField(json, "endColumn", diagnostics.endColumns[i], first);
        }
        if (DiagnosticSpan::HasAll(spanFlags, DiagnosticSpan::HasOffsetFlag)) {
            AppendJsonUintField(json, "offset", diagnostics.offsets[i], first);
            AppendJsonUintField(json, "length", diagnostics.lengths[i], first);
        }
        if (stream && diagnostics.tokens[i] != INVALID_DIAGNOSTIC_TOKEN) {
            u32 token = diagnostics.tokens[i];
            if (!stream->IsEOF(token)) {
                std::string_view tokenValue = stream->GetValue(token);
                AppendJsonStringField(json, "token", std::string(tokenValue), first);
                AppendJsonStringField(json, "tokenType", TokenTypeName(stream->GetType(token)), first);
            }
        }

        if (sourceLines && DiagnosticSpan::HasAll(spanFlags, DiagnosticSpan::HasLocationFlag)) {
            std::vector<u32> contextLineNumbers;
            u32 line = diagnostics.lines[i];
            if (line > 1) contextLineNumbers.push_back(line - 1);
            contextLineNumbers.push_back(line);
            contextLineNumbers.push_back(line + 1);

            AppendJsonComma(json, first);
            json += "      \"context\": [";
            bool firstContext = true;
            for (u32 contextLine : contextLineNumbers) {
                if (contextLine == 0 || contextLine > sourceLines->size()) continue;
                if (!firstContext) json += ",";
                firstContext = false;
                json += "\n        {\"line\": " + std::to_string(contextLine) +
                        ", \"text\": \"" + EscapeJsonString((*sourceLines)[contextLine - 1]) + "\"}";
            }
            json += "\n      ]";
        }

        json += "\n    }";
    }

    if (diagnostics.Count() > 0) json += "\n  ";
    json += "]\n";
    json += "}\n";
    printf("%s", json.c_str());
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
    appendMap(json, "samplers", samplers, false);

    json += "}\n";
    return json;
}

static std::string BuildResourceBindingsJson(const std::string& passName,
                                             const std::vector<ReflectedResourceBinding>& bindings) {
    return ReflectionJson::BuildPrettyResourceBindingsJson(passName, bindings);
}

// Get SPIR-V disassembly using spirv-dis
#ifdef USE_SPIRV_TOOLS_LIB
std::string FormatSpirvToolsMessageLevel(spv_message_level_t level) {
    switch (level) {
        case SPV_MSG_FATAL:
        case SPV_MSG_INTERNAL_ERROR:
        case SPV_MSG_ERROR:
            return "error";
        case SPV_MSG_WARNING:
            return "warning";
        case SPV_MSG_INFO:
            return "info";
        case SPV_MSG_DEBUG:
            return "debug";
    }
    return "message";
}

std::string FormatSpirvToolsMessage(spv_message_level_t level,
                                    const spv_position_t& position,
                                    const char* message) {
    std::ostringstream out;
    out << FormatSpirvToolsMessageLevel(level) << ": " << position.index << ": ";
    out << (message ? message : "");
    return out.str();
}

std::string GetSpirvDisassembly(const std::vector<u32>& spirv) {
    if (spirv.empty()) {
        return "";
    }

    spvtools::SpirvTools tools(SPV_ENV_UNIVERSAL_1_6);
    std::string diagnostic;
    tools.SetMessageConsumer([&diagnostic](spv_message_level_t level,
                                           const char*,
                                           const spv_position_t& position,
                                           const char* message) {
        if (!diagnostic.empty()) {
            diagnostic += "\n";
        }
        diagnostic += FormatSpirvToolsMessage(level, position, message);
    });

    std::string text;
    if (tools.Disassemble(reinterpret_cast<const uint32_t*>(spirv.data()),
                          spirv.size(), &text)) {
        return text;
    }

    return diagnostic.empty()
        ? "SPIR-V disassembly failed"
        : diagnostic;
}
#endif

std::string GetSpirvDisassembly(const std::string& spirvFile) {
    std::string spirvDis = FindTool("spirv-dis");
    if (spirvDis.empty()) {
        return "spirv-dis was not found in PATH, VULKAN_SDK, or common install locations";
    }
    std::string cmd = ExternalCommand(spirvDis) + " " + ShellQuote(spirvFile) + " 2>&1";
    return RunCommand(cmd);
}

bool HasSpirvValidator() {
#ifdef USE_SPIRV_TOOLS_LIB
    return true;
#else
    static bool validatorAvailableCached = false;
    static bool validatorAvailable = false;
    if (!validatorAvailableCached) {
        validatorAvailable = !FindTool("spirv-val").empty();
        validatorAvailableCached = true;
    }
    return validatorAvailable;
#endif
}

#ifdef USE_SPIRV_TOOLS_LIB
ValidationResult ValidateSpirv(const std::vector<u32>& spirv) {
    spvtools::SpirvTools tools(SPV_ENV_UNIVERSAL_1_6);
    std::string diagnostics;
    tools.SetMessageConsumer([&diagnostics](spv_message_level_t level,
                                            const char*,
                                            const spv_position_t& position,
                                            const char* message) {
        if (!diagnostics.empty()) {
            diagnostics += "\n";
        }
        diagnostics += FormatSpirvToolsMessage(level, position, message);
    });

    if (tools.Validate(reinterpret_cast<const uint32_t*>(spirv.data()), spirv.size())) {
        return {ValidationStatus::Passed, ""};
    }

    return {
        ValidationStatus::Failed,
        diagnostics.empty() ? "SPIR-V validation failed" : diagnostics
    };
}
#endif

ValidationResult ValidateSpirv(const std::string& spirvFile) {
    if (!HasSpirvValidator()) {
        return {
            ValidationStatus::ToolMissing,
            "spirv-val was not found in PATH, VULKAN_SDK, or common install locations"
        };
    }

    std::string spirvVal = FindTool("spirv-val");
    std::string cmd = ExternalCommand(spirvVal) + " " + ShellQuote(spirvFile) + " 2>&1";
    CommandResult result = RunCommandWithStatus(cmd);
    if (!result.launched) {
        return {
            ValidationStatus::ToolMissing,
            "failed to launch spirv-val"
        };
    }

    if (result.exitCode == 0) {
        return {ValidationStatus::Passed, ""};
    }

    std::string message = result.output.empty()
        ? "spirv-val exited with code " + std::to_string(result.exitCode)
        : result.output;
    return {ValidationStatus::Failed, message};
}

struct TimedValidationResult {
    ValidationResult result;
    double ms = 0.0;
};

static TimedValidationResult ValidateSpirvTimed(const std::vector<u32>& spirv,
                                                const std::string& spvPath) {
    using Clock = std::chrono::high_resolution_clock;
    auto start = Clock::now();
#ifdef USE_SPIRV_TOOLS_LIB
    (void)spvPath;
    ValidationResult validation = ValidateSpirv(spirv);
#else
    (void)spirv;
    ValidationResult validation = ValidateSpirv(spvPath);
#endif
    auto end = Clock::now();

    TimedValidationResult timed;
    timed.result = std::move(validation);
    timed.ms = std::chrono::duration<double, std::milli>(end - start).count();
    return timed;
}


// ============= Compilation =============

struct CompileResult {
    bool success = false;
    std::vector<u32> spirv;
    std::string metalSource;
    std::string hlslSource;
    std::string directGlesSource;  // Direct GLES output (bypasses SPIRV-Cross)
    std::string error;
    std::vector<std::string> loweringDiagnostics;
    bool diagnosticsAlreadyReported = false;
    std::string irDump;       // IR dump for -internals output
    bool hasWaveOps = false;
    IRAnalysis analysis{};
    std::vector<ExplicitSamplerUse> explicitSamplerUses;
    ShaderTiming timing;
};

static void AddCompileResultDiagnostics(DiagnosticStream& diagnostics,
                                        const CompileResult& result,
                                        const std::string& file,
                                        const std::string& pass,
                                        const std::string& stage) {
    if (result.diagnosticsAlreadyReported) {
        return;
    }

    if (!result.loweringDiagnostics.empty()) {
        for (const std::string& message : result.loweringDiagnostics) {
            AddStageDiagnostic(diagnostics,
                               DiagnosticSeverity::Error,
                               DiagnosticPhase::Lowering,
                               DiagnosticMessageId::LoweringError,
                               message,
                               file,
                               pass,
                               stage);
        }
        return;
    }

    AddStageDiagnostic(diagnostics,
                       DiagnosticSeverity::Error,
                       DiagnosticPhase::Compile,
                       DiagnosticMessageId::ShaderStageCompileFailed,
                       result.error.empty() ? "Shader stage compilation failed" : result.error,
                       file, pass, stage);
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
    bool allowDirectGlesFallback = false,  // Precompute direct GLES for known SPIRV-Cross ES gaps
    const RenderConfig* renderConfig = nullptr,  // For GLES backend
    DiagnosticStream* diagnosticStream = nullptr,
    const std::string& diagnosticPassName = "",
    bool suppressDiagnostics = false
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

    // Resolve pass-stage reuse (vertex = "Base".vertex): an inherited stage
    // node only carries an empty placeholder body, so swap in the source
    // pass's stage before lowering. Without this the reused stage compiles
    // to an empty main() and the pass's varying interface is lost.
    for (u32 hop = 0; shaderStageData->isInherited && hop < 8; hop++) {
        if (pipelineRef.IsNull()) break;
        const PipelineData& pipelineData = context.ast.GetPipeline(pipelineRef);
        const ShaderStageData* resolved = nullptr;
        for (u32 i = 0; i < pipelineData.passes.count; i++) {
            const PassData& srcPass = context.ast.GetPass(pipelineData.passes[i]);
            if (srcPass.name.nameHash != shaderStageData->inheritsFrom.nameHash) {
                continue;
            }
            NodeRef srcStage = (stage == ShaderStage::Vertex)   ? srcPass.vertexShader
                               : (stage == ShaderStage::Fragment) ? srcPass.fragmentShader
                                                                  : srcPass.computeShader;
            if (!srcStage.IsNull()) {
                resolved = &context.ast.GetShaderStage(srcStage);
            }
            break;
        }
        if (!resolved) {
            result.error = "Pass-stage reuse target not found in pipeline";
            return result;
        }
        shaderStageData = resolved;
        shaderBody = shaderStageData->body;
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
    lowering.suppressErrorOutput = suppressDiagnostics || diagnosticStream != nullptr;
    lowering.diagnosticStream = diagnosticStream;
    lowering.diagnosticPassName = diagnosticPassName;
    lowering.currentStage = stage;
    lowering.currentPipeline = pipelineRef;
    lowering.currentPass = passRef;  // For pass-scoped function lookup
    lowering.currentPassData = &pass;
    lowering.currentPassVaryings = varyingContext;  // Set varying context for vertex->fragment data flow

    for (u32 i = 0; i < pass.consts.count; i++) {
        lowering.LowerStatement(pass.consts[i]);
    }

    const BlockData& block = context.ast.GetBlock(shaderBody);
    for (u32 i = 0; i < block.statements.count; i++) {
        lowering.LowerStatement(block.statements[i]);
    }

    // If IR lowering reported recursion, short-circuit: downstream SSA /
    // SPIR-V emission would produce spurious validator errors drowning
    // the diagnostic that already went to stderr.
    if (lowering.recursionDiagnosed) {
        result.loweringDiagnostics = lowering.diagnostics;
        result.diagnosticsAlreadyReported = (diagnosticStream != nullptr);
        result.error = "Recursion is not supported by SPIR-V.";
        return result;
    }

    if (lowering.hadError) {
        result.loweringDiagnostics = lowering.diagnostics;
        result.diagnosticsAlreadyReported = (diagnosticStream != nullptr);
        result.error = "IR lowering failed. See diagnostic above.";
        return result;
    }

    // Ensure return
    if (lowering.program.instructionCount == 0 ||
        lowering.program.opcodes[lowering.program.instructionCount - 1] != OP_RET) {
        lowering.builder.EmitInstruction(OP_RET, 0, 0);
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

    const bool emitDirectGles =
        useDirectGles || (allowDirectGlesFallback && ProgramNeedsDirectGLESFallback(lowering.program));

    // Direct GLES output (bypasses SPIRV-Cross)
    if (emitDirectGles) {
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

struct StageOutputResult {
    bool success = false;
    bool skipRemainingPass = false;
};

static bool WriteCrossCompileOutput(bool enabled,
                                    double ms,
                                    double& timingSlot,
                                    const std::string& source,
                                    const std::string& path,
                                    const char* warningLabel,
                                    bool quiet,
                                    DiagnosticStream* diagnostics = nullptr,
                                    const std::string& file = "",
                                    const std::string& pass = "",
                                    const std::string& stage = "") {
    if (!enabled) return true;

    timingSlot = ms;
    if (!source.empty() && source.find("error") == std::string::npos) {
        if (WriteTextFile(path, source)) {
            if (!quiet) {
                printf("    -> %s\n", path.c_str());
            }
        }
        return true;
    }

    if (diagnostics) {
        AddStageDiagnostic(*diagnostics,
                           DiagnosticSeverity::Warning,
                           DiagnosticPhase::Compile,
                           DiagnosticMessageId::CrossCompileFailed,
                           std::string(warningLabel) + " cross-compilation failed",
                           file, pass, stage);
    }
    return false;
}

static void WriteWebGLSidecarIfNeeded(const CompileResult& result,
                                      const std::string& sidecarSourcePath,
                                      const PassData& pass,
                                      const Parser& parser,
                                      const char* sourceBase,
                                      const CompilationContext& context,
                                      const PipelineData& pipeline,
                                      bool writeWebGLSidecar,
                                      bool isVertexStage,
                                      bool quiet) {
    if (!writeWebGLSidecar) return;

    std::string json = BuildWebGLSidecarJson(
        pass, parser.symbolTable, result.analysis,
        sourceBase, isVertexStage,
        &context.ast, &pipeline);
    std::string jsonPath = sidecarSourcePath + ".json";
    if (WriteTextFile(jsonPath, json)) {
        if (!quiet) {
            printf("    -> %s\n", jsonPath.c_str());
        }
    }
}

static StageOutputResult WriteStageOutputs(const CompilerConfig& config,
                                           const CompileResult& result,
                                           ShaderTiming& shaderTime,
                                           const std::string& outputStemPath,
                                           const std::string& passName,
                                           const char* stageName,
                                           const PassData& pass,
                                           const Parser& parser,
                                           const char* sourceBase,
                                           const CompilationContext& context,
                                           const PipelineData& pipeline,
                                           bool writeWebGLSidecar,
                                           bool isVertexStage,
                                           BackendCache& backendCache,
                                           DiagnosticStream* diagnostics = nullptr) {
    StageOutputResult output;
    const bool quiet = config.errorsJson;
    const std::string stageOutputPath = BuildStageOutputPath(outputStemPath, stageName);
    const bool disambiguateGlslOutputs = config.outputGlsl && config.outputGlslEs;

    if (!config.outputSpirv &&
        config.validationMode == ValidationMode::Off &&
        !config.outputMetal &&
        !config.outputHlsl &&
        !config.outputGlsl &&
        !config.outputGlslEs &&
        !config.outputInternals) {
        shaderTime.totalMs = shaderTime.irLoweringMs + shaderTime.cfgSsaMs + shaderTime.spirvGenMs +
                             shaderTime.validationMs + shaderTime.metalCrossMs + shaderTime.hlslCrossMs +
                             shaderTime.glslCrossMs + shaderTime.glslEsCrossMs;
        output.success = true;
        return output;
    }

    struct ScopedTempFile {
        bool enabled = false;
        std::string path;

        ~ScopedTempFile() {
            if (!enabled || path.empty()) return;
            std::error_code ec;
            fs::remove(path, ec);
        }
    };

    const bool validationRequested = config.validationMode != ValidationMode::Off;
    const bool validatorAvailable = validationRequested && HasSpirvValidator();
#ifdef USE_SPIRV_TOOLS_LIB
    const bool validationNeedsSpirvFile = false;
    const bool internalsNeedSpirvFile = false;
#else
    const bool validationNeedsSpirvFile = validationRequested && validatorAvailable;
    const bool internalsNeedSpirvFile = config.outputInternals;
#endif
#ifdef USE_SPIRV_CROSS_LIB
    const bool crossCompileNeedsSpirvFile = false;
#else
    const bool crossCompileNeedsSpirvFile =
        config.outputMetal || config.outputHlsl || config.outputGlsl || config.outputGlslEs;
#endif
    const bool needsSpirvFile =
        config.outputSpirv ||
        validationNeedsSpirvFile ||
        internalsNeedSpirvFile ||
        crossCompileNeedsSpirvFile;

    std::string spvPath;
    ScopedTempFile tempSpirv;
    if (needsSpirvFile) {
        spvPath = config.outputSpirv ? (stageOutputPath + ".spv") : GetTempSpirvPath();
        tempSpirv.enabled = !config.outputSpirv;
        tempSpirv.path = spvPath;

        if (!WriteBinaryFile(spvPath, result.spirv)) {
            if (diagnostics) {
                AddStageDiagnostic(*diagnostics,
                                   DiagnosticSeverity::Error,
                                   DiagnosticPhase::IO,
                                   DiagnosticMessageId::CouldNotWriteSpirv,
                                   "Could not write SPIR-V file",
                                   config.inputFile, passName, stageName);
            }
            return output;
        }

        if (config.outputSpirv && !quiet) {
            printf("    -> %s\n", spvPath.c_str());
        }
    }

    static bool warnedMissingValidator = false;
    auto handleMissingValidator = [&]() -> bool {
        const std::string message = "spirv-val was not found in PATH, VULKAN_SDK, or common install locations";
        if (config.validationMode == ValidationMode::Strict) {
            if (diagnostics) {
                AddStageDiagnostic(*diagnostics,
                                   DiagnosticSeverity::Error,
                                   DiagnosticPhase::Validation,
                                   DiagnosticMessageId::ValidationStrictToolMissing,
                                   "SPIR-V validation requested but " + message,
                                   config.inputFile, passName, stageName);
            }
            output.skipRemainingPass = true;
            return true;
        }

        if (!warnedMissingValidator) {
            if (diagnostics) {
                AddStageDiagnostic(*diagnostics,
                                   DiagnosticSeverity::Warning,
                                   DiagnosticPhase::Validation,
                                   DiagnosticMessageId::ValidationAutoToolMissing,
                                   message + "; skipping SPIR-V validation (-validation auto)",
                                   config.inputFile, passName, stageName);
            }
            warnedMissingValidator = true;
        }
        return false;
    };

    if (validationRequested && !validatorAvailable) {
        if (handleMissingValidator()) {
            return output;
        }
    }

    const u32 backendCacheIndex =
        backendCache.FindOrAdd(result.spirv, BuildBackendCacheOptions(result.hasWaveOps));

#ifdef USE_SPIRV_CROSS_LIB
    const u8 crossRequestMask = BuildCrossRequestMask(config);
    const u8 crossMissingMask =
        static_cast<u8>(crossRequestMask & ~backendCache.crossDoneMask[backendCacheIndex]);
#endif

    const bool validationNeeded = validationRequested && validatorAvailable;
    TimedValidationResult validationTimed;
    bool hasValidationResult = false;
    std::future<TimedValidationResult> validationFuture;
    if (validationNeeded) {
        if (backendCache.validationDone[backendCacheIndex]) {
            validationTimed.result = backendCache.LoadValidation(backendCacheIndex);
            shaderTime.validationMs = 0.0;
            hasValidationResult = true;
        } else {
#ifdef USE_SPIRV_CROSS_LIB
            if (crossMissingMask != 0) {
                validationFuture = std::async(std::launch::async, [&result, &spvPath]() {
                    return ValidateSpirvTimed(result.spirv, spvPath);
                });
            } else {
                validationTimed = ValidateSpirvTimed(result.spirv, spvPath);
                shaderTime.validationMs = validationTimed.ms;
                backendCache.StoreValidation(backendCacheIndex, validationTimed.result);
                hasValidationResult = true;
            }
#else
            validationTimed = ValidateSpirvTimed(result.spirv, spvPath);
            shaderTime.validationMs = validationTimed.ms;
            backendCache.StoreValidation(backendCacheIndex, validationTimed.result);
            hasValidationResult = true;
#endif
        }
    }

#ifdef USE_SPIRV_CROSS_LIB
    std::future<CrossCompileResult> crossFuture;
    if (crossMissingMask != 0) {
        crossFuture = std::async(std::launch::async,
                                 [&result, crossMissingMask]() {
                                     return ParallelCrossCompileMasked(result.spirv,
                                                                       crossMissingMask,
                                                                       result.hasWaveOps);
                                 });
    }
#endif

    if (validationFuture.valid()) {
        validationTimed = validationFuture.get();
        shaderTime.validationMs = validationTimed.ms;
        backendCache.StoreValidation(backendCacheIndex, validationTimed.result);
        hasValidationResult = true;
    }

    if (validationNeeded && hasValidationResult) {
        const ValidationResult& validation = validationTimed.result;
        if (validation.status == ValidationStatus::ToolMissing) {
            if (config.validationMode == ValidationMode::Strict) {
                if (diagnostics) {
                    AddStageDiagnostic(*diagnostics,
                                       DiagnosticSeverity::Error,
                                       DiagnosticPhase::Validation,
                                       DiagnosticMessageId::ValidationStrictToolMissing,
                                       "SPIR-V validation requested but " + validation.message,
                                       config.inputFile, passName, stageName);
                }
#ifdef USE_SPIRV_CROSS_LIB
                if (crossFuture.valid()) {
                    (void)crossFuture.get();
                }
#endif
                output.skipRemainingPass = true;
                return output;
            }

            if (!warnedMissingValidator) {
                if (diagnostics) {
                    AddStageDiagnostic(*diagnostics,
                                       DiagnosticSeverity::Warning,
                                       DiagnosticPhase::Validation,
                                       DiagnosticMessageId::ValidationAutoToolMissing,
                                       validation.message + "; skipping SPIR-V validation (-validation auto)",
                                       config.inputFile, passName, stageName);
                }
                warnedMissingValidator = true;
            }
        } else if (validation.status == ValidationStatus::Failed) {
            if (diagnostics) {
                AddStageDiagnostic(*diagnostics,
                                   DiagnosticSeverity::Error,
                                   DiagnosticPhase::Validation,
                                   DiagnosticMessageId::ValidationFailed,
                                   "SPIR-V validation failed:\n" + validation.message,
                                   config.inputFile, passName, stageName);
            }
#ifdef USE_SPIRV_CROSS_LIB
            if (crossFuture.valid()) {
                (void)crossFuture.get();
            }
#endif
            output.skipRemainingPass = true;
            return output;
        }
    }

#ifdef USE_SPIRV_CROSS_LIB
    CrossCompileResult computedCrossResult;
    u8 computedCrossMask = 0;
    if (crossFuture.valid()) {
        computedCrossResult = crossFuture.get();
        computedCrossMask = crossMissingMask;
        StoreCrossCompileCache(backendCache, backendCacheIndex,
                               computedCrossMask, computedCrossResult);
    }

    CrossCompileResult crossResult =
        BuildCrossCompileResultFromCache(backendCache, backendCacheIndex,
                                         crossRequestMask, computedCrossMask,
                                         computedCrossResult);

    WriteCrossCompileOutput(config.outputMetal, crossResult.metalMs,
                            shaderTime.metalCrossMs, crossResult.metal,
                            stageOutputPath + ".metal", "Metal", quiet,
                            diagnostics, config.inputFile, passName, stageName);
    WriteCrossCompileOutput(config.outputHlsl, crossResult.hlslMs,
                            shaderTime.hlslCrossMs, crossResult.hlsl,
                            stageOutputPath + ".hlsl", "HLSL", quiet,
                            diagnostics, config.inputFile, passName, stageName);
    WriteCrossCompileOutput(config.outputGlsl, crossResult.glslMs,
                            shaderTime.glslCrossMs, crossResult.glsl,
                            BuildGlslOutputPath(outputStemPath, stageName, "glsl", disambiguateGlslOutputs),
                            "GLSL", quiet,
                            diagnostics, config.inputFile, passName, stageName);

    if (config.outputGlslEs) {
        double glesMs = 0.0;
        std::string glesSource = SelectGLESSource(
            crossResult, result.directGlesSource,
            result.timing.glslEsCrossMs,
            config.useDirectGles, &glesMs);
        shaderTime.glslEsCrossMs = glesMs;
        if (!glesSource.empty() && glesSource.find("error") == std::string::npos) {
            std::string glslEsPath = BuildGlslOutputPath(outputStemPath, stageName, "gles", disambiguateGlslOutputs);
            if (WriteTextFile(glslEsPath, glesSource)) {
                if (!quiet) {
                    printf("    -> %s\n", glslEsPath.c_str());
                }
            }
            WriteWebGLSidecarIfNeeded(result, glslEsPath, pass, parser,
                                      sourceBase, context, pipeline,
                                      writeWebGLSidecar, isVertexStage, quiet);
        } else {
            if (diagnostics) {
                AddStageDiagnostic(*diagnostics,
                                   DiagnosticSeverity::Warning,
                                   DiagnosticPhase::Compile,
                                   DiagnosticMessageId::CrossCompileFailed,
                                   "GLSL ES cross-compilation failed",
                                   config.inputFile, passName, stageName);
            }
        }
    }
#else
    if (config.outputMetal) {
        using Clock = std::chrono::high_resolution_clock;
        auto metalStart = Clock::now();
        std::string metalSource = CrossCompileToMetal(spvPath);
        auto metalEnd = Clock::now();
        double metalMs = std::chrono::duration<double, std::milli>(metalEnd - metalStart).count();
        WriteCrossCompileOutput(true, metalMs, shaderTime.metalCrossMs,
                                metalSource, stageOutputPath + ".metal", "Metal", quiet,
                                diagnostics, config.inputFile, passName, stageName);
    }
    if (config.outputHlsl) {
        using Clock = std::chrono::high_resolution_clock;
        auto hlslStart = Clock::now();
        std::string hlslSource = CrossCompileToHLSL(spvPath, result.hasWaveOps);
        auto hlslEnd = Clock::now();
        double hlslMs = std::chrono::duration<double, std::milli>(hlslEnd - hlslStart).count();
        WriteCrossCompileOutput(true, hlslMs, shaderTime.hlslCrossMs,
                                hlslSource, stageOutputPath + ".hlsl", "HLSL", quiet,
                                diagnostics, config.inputFile, passName, stageName);
    }
    if (config.outputGlsl) {
        using Clock = std::chrono::high_resolution_clock;
        auto glslStart = Clock::now();
        std::string glslSource = CrossCompileToGLSL(spvPath);
        auto glslEnd = Clock::now();
        double glslMs = std::chrono::duration<double, std::milli>(glslEnd - glslStart).count();
        WriteCrossCompileOutput(true, glslMs, shaderTime.glslCrossMs,
                                glslSource,
                                BuildGlslOutputPath(outputStemPath, stageName, "glsl", disambiguateGlslOutputs),
                                "GLSL", quiet,
                                diagnostics, config.inputFile, passName, stageName);
    }
    if (config.outputGlslEs) {
        using Clock = std::chrono::high_resolution_clock;
        auto glslEsStart = Clock::now();
        std::string glslEsSource = CrossCompileToGLSLCLI(spvPath, 300);
        auto glslEsEnd = Clock::now();
        shaderTime.glslEsCrossMs = std::chrono::duration<double, std::milli>(glslEsEnd - glslEsStart).count();
        if (!glslEsSource.empty() && glslEsSource.find("error") == std::string::npos) {
            std::string glslEsPath = BuildGlslOutputPath(outputStemPath, stageName, "gles", disambiguateGlslOutputs);
            if (WriteTextFile(glslEsPath, glslEsSource)) {
                if (!quiet) {
                    printf("    -> %s\n", glslEsPath.c_str());
                }
            }
            WriteWebGLSidecarIfNeeded(result, glslEsPath, pass, parser,
                                      sourceBase, context, pipeline,
                                      writeWebGLSidecar, isVertexStage, quiet);
        } else {
            if (diagnostics) {
                AddStageDiagnostic(*diagnostics,
                                   DiagnosticSeverity::Warning,
                                   DiagnosticPhase::Compile,
                                   DiagnosticMessageId::CrossCompileFailed,
                                   "GLSL ES cross-compilation failed",
                                   config.inputFile, passName, stageName);
            }
        }
    }
#endif

    if (config.outputInternals && !result.irDump.empty()) {
#ifdef USE_SPIRV_TOOLS_LIB
        std::string spirvDis = GetSpirvDisassembly(result.spirv);
#else
        std::string spirvDis = GetSpirvDisassembly(spvPath);
#endif
        std::string internalsJson = "{\n";
        internalsJson += "  \"pass\": \"" + EscapeJsonString(passName) + "\",\n";
        internalsJson += "  \"stage\": \"" + std::string(stageName) + "\",\n";
        internalsJson += "  \"ir\": \"" + EscapeJsonString(result.irDump) + "\",\n";
        internalsJson += "  \"spirv_dis\": \"" + EscapeJsonString(spirvDis) + "\"\n";
        internalsJson += "}\n";

        std::string internalsPath = stageOutputPath + ".internals.json";
        if (WriteTextFile(internalsPath, internalsJson)) {
            if (!quiet) {
                printf("    -> %s\n", internalsPath.c_str());
            }
        }
    }

    shaderTime.totalMs = shaderTime.irLoweringMs + shaderTime.cfgSsaMs + shaderTime.spirvGenMs +
                         shaderTime.validationMs + shaderTime.metalCrossMs + shaderTime.hlslCrossMs +
                         shaderTime.glslCrossMs + shaderTime.glslEsCrossMs;

    output.success = true;
    return output;
}

// ============= Main Entry Point =============

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    CompilerConfig config;
    config.errorsJson = ArgsRequestErrorsJson(argc, argv);
    static constexpr size_t DIAGNOSTIC_ARENA_SIZE = 4 * 1024 * 1024;
    std::unique_ptr<u8[]> diagnosticMemory = std::make_unique<u8[]>(DIAGNOSTIC_ARENA_SIZE);
    Memory::BWEMemoryArena diagnosticArena;
    diagnosticArena.Initialize(diagnosticMemory.get(), DIAGNOSTIC_ARENA_SIZE);
    DiagnosticStream diagnostics;
    diagnostics.Init(&diagnosticArena);
    auto emitFailure = [&](const TokenStream* stream = nullptr,
                           const std::vector<std::string>* sourceLines = nullptr,
                           bool printUsage = false) -> int {
        if (config.errorsJson) {
            PrintDiagnosticsJson(config, diagnostics, false, stream, sourceLines);
        } else {
            PrintDiagnosticsText(config, diagnostics, stream, sourceLines);
            if (printUsage) {
                PrintUsage(argv[0]);
            }
        }
        return 1;
    };

    try {
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        } else if (arg == "-v") {
            config.verbose = true;
        } else if (IsCheckArg(arg)) {
            config.checkOnly = true;
        } else if (IsStdinArg(arg)) {
            config.readStdin = true;
        } else if ((arg == "-source-file" || arg == "--source-file") && i + 1 < argc) {
            config.sourceFileOverride = argv[++i];
        } else if (arg == "-source-file" || arg == "--source-file") {
            diagnostics.AddRaw(DiagnosticSeverity::Error,
                               DiagnosticPhase::CLI,
                               "Expected path after " + arg,
                               DiagnosticSpan{},
                               config.inputFile,
                               "",
                               "",
                               DiagnosticMessageId::Raw);
            return emitFailure(nullptr, nullptr, true);
        } else if (arg.rfind("--source-file=", 0) == 0) {
            config.sourceFileOverride = arg.substr(strlen("--source-file="));
        } else if (arg == "-o" && i + 1 < argc) {
            config.outputDir = argv[++i];
        } else if (arg == "-pass" && i + 1 < argc) {
            config.passFilter = argv[++i];
        } else if (arg == "-stage" && i + 1 < argc) {
            config.stageFilter = argv[++i];
        } else if (arg == "-spv") {
            config.outputSpirv = true;
        } else if (arg == "-metal") {
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
            config.outputSpirv = true;
            config.outputMetal = true;
            config.outputHlsl = true;
            config.outputGlsl = true;
            config.outputGlslEs = true;
            config.outputBindings = true;
        } else if (arg == "-modules" && i + 1 < argc) {
            config.modulePaths.push_back(argv[++i]);
        } else if (arg == "-variant" && i + 1 < argc) {
            std::string spec = argv[++i];
            size_t eq = spec.find('=');
            if (eq == std::string::npos || eq == 0 || eq == spec.size() - 1) {
                diagnostics.AddMessage(DiagnosticSeverity::Error,
                                       DiagnosticPhase::CLI,
                                       DiagnosticMessageId::VariantExpectsNameValue);
                return emitFailure();
            }
            VariantOverride overrideValue;
            overrideValue.name = spec.substr(0, eq);
            overrideValue.value = spec.substr(eq + 1);
            config.variantOverrides.push_back(std::move(overrideValue));
        } else if (arg == "-dump-variant-space") {
            config.dumpVariantSpace = true;
        } else if (arg == "-ast-json" || arg == "--ast-json") {
            config.astJson = true;
        } else if ((arg == "-validation" || arg == "--validation") && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "auto") {
                config.validationMode = ValidationMode::Auto;
            } else if (mode == "strict") {
                config.validationMode = ValidationMode::Strict;
            } else if (mode == "off") {
                config.validationMode = ValidationMode::Off;
            } else {
                diagnostics.AddMessage(DiagnosticSeverity::Error,
                                       DiagnosticPhase::CLI,
                                       DiagnosticMessageId::ValidationModeExpected);
                return emitFailure();
            }
        } else if (arg == "-validation" || arg == "--validation") {
            diagnostics.AddMessage(DiagnosticSeverity::Error,
                                   DiagnosticPhase::CLI,
                                   DiagnosticMessageId::ValidationModeExpected);
            return emitFailure();
        } else if (arg.rfind("--validation=", 0) == 0) {
            std::string mode = arg.substr(strlen("--validation="));
            if (mode == "auto") {
                config.validationMode = ValidationMode::Auto;
            } else if (mode == "strict") {
                config.validationMode = ValidationMode::Strict;
            } else if (mode == "off") {
                config.validationMode = ValidationMode::Off;
            } else {
                diagnostics.AddMessage(DiagnosticSeverity::Error,
                                       DiagnosticPhase::CLI,
                                       DiagnosticMessageId::ValidationModeExpected);
                return emitFailure();
            }
        } else if (arg == "-") {
            config.readStdin = true;
            if (config.inputFile.empty()) {
                config.inputFile = "<stdin>";
            }
        } else if (arg[0] != '-') {
            config.inputFile = arg;
        } else if (IsErrorsJsonArg(arg)) {
            config.errorsJson = true;
        } else if (arg == "-dump-ir") {
            config.dumpIr = true;
        } else if (arg == "-debug-names") {
            config.debugNames = true;
        } else if (arg == "-timing") {
            config.showTiming = true;
        } else if (arg == "-no-validate") {
            config.validationMode = ValidationMode::Off;
        } else if (arg == "-internals") {
            config.outputInternals = true;
        } else if (arg == "-bindings") {
            config.outputBindings = true;
        } else {
            diagnostics.AddMessage(DiagnosticSeverity::Error,
                                   DiagnosticPhase::CLI,
                                   DiagnosticMessageId::UnknownOption,
                                   {DiagnosticArg::String(arg)});
            return emitFailure(nullptr, nullptr, true);
        }
    }

    if (config.readStdin && !config.sourceFileOverride.empty()) {
        config.inputFile = config.sourceFileOverride;
    }
    if (config.readStdin && config.inputFile.empty()) {
        config.inputFile = "<stdin>";
    }

    if (!config.astJson &&
        !config.outputSpirv &&
        !config.outputMetal &&
        !config.outputHlsl &&
        !config.outputGlsl &&
        !config.outputGlslEs) {
        config.outputSpirv = true;
    }

    if (config.checkOnly) {
        config.outputSpirv = false;
        config.outputMetal = false;
        config.outputHlsl = false;
        config.outputGlsl = false;
        config.outputGlslEs = false;
        config.outputInternals = false;
        config.outputBindings = false;
        config.dumpIr = false;
    }

    if (config.errorsJson) {
        config.verbose = false;
        config.dumpIr = false;
        config.showTiming = false;
    }

    if (config.astJson) {
        config.outputSpirv = false;
        config.outputMetal = false;
        config.outputHlsl = false;
        config.outputGlsl = false;
        config.outputGlslEs = false;
        config.outputInternals = false;
        config.outputBindings = false;
        config.dumpIr = false;
        config.showTiming = false;
        config.verbose = false;
    }

    if (config.inputFile.empty()) {
        diagnostics.AddMessage(DiagnosticSeverity::Error,
                               DiagnosticPhase::CLI,
                               DiagnosticMessageId::NoInputFile);
        return emitFailure(nullptr, nullptr, true);
    }

    // Read input source
    std::string source = config.readStdin ? ReadStdin() : ReadFile(config.inputFile);
    if (source.empty()) {
        diagnostics.AddMessage(DiagnosticSeverity::Error,
                               DiagnosticPhase::IO,
                               DiagnosticMessageId::CouldNotReadFile,
                               {DiagnosticArg::String(config.inputFile)},
                               DiagnosticSpan{},
                               config.inputFile);
        return emitFailure();
    }

    // Get base name for output files
    const bool virtualInputFile = IsVirtualInputFile(config.inputFile);
    fs::path inputPath = virtualInputFile ? fs::path("stdin.bwsl") : fs::path(config.inputFile);
    std::string baseName = inputPath.stem().string();
    if (baseName.empty()) {
        baseName = "stdin";
    }

    fs::path absoluteInputPath;
    if (virtualInputFile) {
        std::error_code ec;
        absoluteInputPath = fs::current_path(ec);
        if (ec || absoluteInputPath.empty()) {
            absoluteInputPath = ".";
        }
        absoluteInputPath /= "stdin.bwsl";
    } else {
        absoluteInputPath = fs::absolute(inputPath);
    }
    if (config.errorsJson && !virtualInputFile) {
        config.inputFile = absoluteInputPath.string();
    }

    // Set up module search paths
    BWSL::ClearModuleSearchPaths();

    // Add user-specified module paths first (highest priority)
    for (const auto& modulePath : config.modulePaths) {
        fs::path absPath = fs::absolute(modulePath);
        BWSL::AddModuleSearchPath(absPath.string());
    }

    // Add input file's directory (allows finding modules relative to the shader)
    fs::path inputDir = absoluteInputPath.parent_path();
    BWSL::AddModuleSearchPath(inputDir.string());

    // Create output directory if needed
    if (!config.checkOnly && !config.outputDir.empty() && config.outputDir != ".") {
        fs::create_directories(config.outputDir);
    }

    if (config.verbose) {
        printf("BWSL Compiler\n");
        printf("Input: %s\n", config.inputFile.c_str());
        if (config.checkOnly) {
            printf("Mode: check\n");
        } else {
            printf("Output: %s\n", config.outputDir.c_str());
        }
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

    // Split source into lines for error reporting
    std::vector<std::string> sourceLines = SplitLines(source);

    // Start timing
    using Clock = std::chrono::high_resolution_clock;
    auto totalStart = Clock::now();
    TimingInfo timing;

    // Lexer initialization
    auto lexStart = Clock::now();
    CompilationContext context;
    context.diagnosticSink = &diagnostics;
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

    (void)parser.ParseDocument();
    auto parseEnd = Clock::now();
    timing.parseMs = std::chrono::duration<double, std::milli>(parseEnd - parseStart).count();

    if (parser.hadError) {
        return emitFailure(&stream, &sourceLines);
    }

    if (config.astJson) {
        std::cout << BWSL::AstJson::SerializeASTJson(context.ast,
                                                     context.root,
                                                     config.inputFile)
                  << "\n";
        return 0;
    }

    bool isModule = (context.ast.pipelines.count == 0 && context.ast.modules.count > 0);

    if (!isModule && context.root.IsValid()) {
        std::string variantResolveError;
        if (!parser.ResolveVariants(context.root, &variantResolveError)) {
            std::string message = variantResolveError.empty()
                ? "invalid variant declarations"
                : variantResolveError;
            diagnostics.AddRaw(DiagnosticSeverity::Error,
                               DiagnosticPhase::Variant,
                               "Variant resolution failed: " + message,
                               DiagnosticSpan{},
                               config.inputFile,
                               "",
                               "",
                               DiagnosticMessageId::VariantResolutionFailed);
            return emitFailure(&stream, &sourceLines);
        }
        if (parser.hadError) {
            return emitFailure(&stream, &sourceLines);
        }
    }

    std::string comptimeError;
    parser.diagnosticPhase = DiagnosticPhase::Comptime;
    bool comptimeOk = BWSL::Comptime::RunComptimeInterpreter(&context, &parser, context.root, &comptimeError);
    parser.diagnosticPhase = DiagnosticPhase::Parse;
    if (!comptimeOk) {
        if (parser.errors.count == 0) {
            diagnostics.AddRaw(DiagnosticSeverity::Error,
                               DiagnosticPhase::Comptime,
                               comptimeError.empty() ? "Comptime interpretation failed" : comptimeError,
                               DiagnosticSpan{},
                               config.inputFile,
                               "",
                               "",
                               DiagnosticMessageId::ComptimeError);
        }
        return emitFailure(&stream, &sourceLines);
    }

    // Handle module files differently - they don't have pipelines to compile
    if (isModule) {
        if (config.errorsJson) {
            PrintDiagnosticsJson(config, diagnostics, true);
        } else {
            printf("Module '%s' parsed successfully.\n", baseName.c_str());
            printf("Note: Modules define reusable functions/structs but have no shaders to compile.\n");
            printf("To compile shaders, use a pipeline file that imports this module.\n");
        }
        return 0;
    }

    if (context.ast.pipelines.count == 0) {
        diagnostics.AddMessage(DiagnosticSeverity::Error,
                               DiagnosticPhase::Parse,
                               DiagnosticMessageId::NoPipelineFound,
                               {DiagnosticArg::String(config.inputFile)},
                               DiagnosticSpan{},
                               config.inputFile);
        return emitFailure(&stream, &sourceLines);
    }

    NodeRef originalPipelineRef = context.root;
    parser.ResolveShaderStages(originalPipelineRef);
    if (parser.hadError) {
        return emitFailure(&stream, &sourceLines);
    }

    VariantSelectionData variantSelection;
    std::string variantError;
    if (!parser.BuildVariantSelection(originalPipelineRef, nullptr, 0, false,
                                      config.variantOverrides, &variantSelection,
                                      &variantError)) {
        diagnostics.AddRaw(DiagnosticSeverity::Error,
                           DiagnosticPhase::Variant,
                           variantError.empty() ? "Variant selection failed" : variantError,
                           DiagnosticSpan{},
                           config.inputFile,
                           "",
                           "",
                           DiagnosticMessageId::VariantSelectionFailed);
        return emitFailure(&stream, &sourceLines);
    }

    VariantReflectionData variantReflection;
    if (!parser.BuildVariantReflection(originalPipelineRef, &variantSelection,
                                       &variantReflection, &variantError)) {
        diagnostics.AddRaw(DiagnosticSeverity::Error,
                           DiagnosticPhase::Variant,
                           variantError.empty() ? "Variant reflection failed" : variantError,
                           DiagnosticSpan{},
                           config.inputFile,
                           "",
                           "",
                           DiagnosticMessageId::VariantReflectionFailed);
        return emitFailure(&stream, &sourceLines);
    }

    if (config.dumpVariantSpace) {
        printf("%s\n", SerializeVariantReflectionJson(variantReflection).c_str());
        return 0;
    }

    NodeRef specializedPipelineRef = parser.SpecializePipelineForVariants(originalPipelineRef,
                                                                          variantSelection,
                                                                          &variantError);
    if (parser.hadError) {
        return emitFailure(&stream, &sourceLines);
    }
    if (specializedPipelineRef.IsNull()) {
        diagnostics.AddRaw(DiagnosticSeverity::Error,
                           DiagnosticPhase::Variant,
                           variantError.empty() ? "Variant specialization failed" : variantError,
                           DiagnosticSpan{},
                           config.inputFile,
                           "",
                           "",
                           DiagnosticMessageId::VariantSpecializationFailed);
        return emitFailure(&stream, &sourceLines);
    }

    const PipelineData& pipeline = context.ast.GetPipeline(specializedPipelineRef);

    // Get source base for string lookups
    const char* sourceBase = lexer.GetSourceBase();

    config.renderConfig = CreateSyntheticRenderConfig(context.ast,
                                                      pipeline,
                                                      parser.symbolTable,
                                                      sourceBase,
                                                      "Pipeline");

    ComputeGraphCompileResult graphResult = CompileComputeGraph(
        context.ast, context.ast.GetPipeline(originalPipelineRef), config.renderConfig, sourceBase);
    if (!graphResult.success) {
        diagnostics.AddRaw(DiagnosticSeverity::Error,
                           DiagnosticPhase::ComputeGraph,
                           graphResult.error.empty() ? "Compute graph validation failed" : graphResult.error,
                           DiagnosticSpan{},
                           config.inputFile,
                           "",
                           "",
                           DiagnosticMessageId::ComputeGraphValidationFailed);
        return emitFailure(&stream, &sourceLines);
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

    auto getPassOutputName = [](u32 passIndex) -> std::string {
        char buf[32];
        snprintf(buf, sizeof(buf), "pass%u", passIndex);
        return buf;
    };

    if (config.verbose) {
        printf("Found pipeline '%s' with %u passes\n\n",
               getString(pipeline.name).c_str(),
               pipeline.passes.count);
    }

    int compiledCount = 0;
    int errorCount = 0;
    const u32 backendCacheEntryCapacity =
        std::max<u32>(1, pipeline.passes.count * 3);
    const size_t backendCacheArenaSize =
        std::max<size_t>(8ull * 1024ull * 1024ull,
                         static_cast<size_t>(backendCacheEntryCapacity) * 512ull * 1024ull);
    std::unique_ptr<u8[]> backendCacheMemory(new u8[backendCacheArenaSize]);
    BWSL_Arena backendCacheArena;
    backendCacheArena.Initialize(backendCacheMemory.get(), backendCacheArenaSize);
    BackendCache backendCache;
    backendCache.Init(&backendCacheArena, backendCacheEntryCapacity);

    // Compile each pass
    for (u32 passIdx = 0; passIdx < pipeline.passes.count; passIdx++) {
        const PassData& pass = context.ast.GetPass(pipeline.passes[passIdx]);
        std::string passName = getString(pass.name, passIdx);
        std::string passOutputName = getPassOutputName(passIdx);
        std::string outputStem = BuildPassOutputStem(baseName, passName, passOutputName, pipeline.passes.count);
        std::string outputStemPath = BuildOutputBasePath(config.outputDir, outputStem);

        // Check pass filter
        if (!config.passFilter.empty() &&
            passName != config.passFilter &&
            passOutputName != config.passFilter) {
            continue;
        }

        if (!config.errorsJson) {
            printf("%s pass '%s'...\n", config.checkOnly ? "Checking" : "Compiling", passName.c_str());
        }

        // Create varying context for this pass - shared between vertex and fragment
        // Vertex shader populates it, fragment shader uses it to resolve input.xxx
        PassVaryingContext passVaryings;
        passVaryings.diagnosticStream = &diagnostics;
        passVaryings.diagnosticPassName = passName;
        passVaryings.diagnosticStageName = "vertex";
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

            if (!config.errorsJson) {
                printf("  Vertex shader:\n");
            }
            CompileResult result = CompileShaderStage(context, parser, pass,
                                                       ShaderStage::Vertex, config.verbose, config.dumpIr, config.debugNames,
                                                       true, config.outputGlslEs, specializedPipelineRef,
                                                       pipeline.passes[passIdx], &passVaryings, config.outputInternals,
                                                       config.useDirectGles, config.outputGlslEs, &config.renderConfig,
                                                       &diagnostics,
                                                       passName,
                                                       config.errorsJson);

            if (!result.success) {
                AddCompileResultDiagnostics(diagnostics, result, config.inputFile, passName, "vertex");
                errorCount++;
            } else {
                vertexReflectionAnalysis = result.analysis;
                vertexReflectionSamplerUses = result.explicitSamplerUses;
                haveVertexReflectionAnalysis = true;
                // Record timing for this shader
                timing.shaderTimings.push_back({outputStem + ".vert", result.timing});
                ShaderTiming& shaderTime = timing.shaderTimings.back().second;

                StageOutputResult output = WriteStageOutputs(
                    config, result, shaderTime, outputStemPath, passName, "vertex",
                    pass, parser, sourceBase, context, pipeline, true, true,
                    backendCache, &diagnostics);
                if (output.success) {
                    compiledCount++;
                } else {
                    errorCount++;
                    if (output.skipRemainingPass) continue;
                }
            }
        }

        // Compile fragment shader
        if ((config.stageFilter.empty() || config.stageFilter == "fragment") &&
            !pass.fragmentShader.IsNull()) {

            if (!config.errorsJson) {
                printf("  Fragment shader:\n");
                fflush(stdout);
            }
            CompileResult result = CompileShaderStage(context, parser, pass,
                                                       ShaderStage::Fragment, config.verbose, config.dumpIr, config.debugNames,
                                                       true, false, specializedPipelineRef,
                                                       pipeline.passes[passIdx], &passVaryings, config.outputInternals,
                                                       config.useDirectGles, config.outputGlslEs, &config.renderConfig,
                                                       &diagnostics,
                                                       passName,
                                                       config.errorsJson);  // Use same varying context populated by vertex shader

            if (!result.success) {
                AddCompileResultDiagnostics(diagnostics, result, config.inputFile, passName, "fragment");
                errorCount++;
            } else {
                fragmentReflectionAnalysis = result.analysis;
                fragmentReflectionSamplerUses = result.explicitSamplerUses;
                haveFragmentReflectionAnalysis = true;
                // Record timing for this shader
                timing.shaderTimings.push_back({outputStem + ".frag", result.timing});
                ShaderTiming& shaderTime = timing.shaderTimings.back().second;

                StageOutputResult output = WriteStageOutputs(
                    config, result, shaderTime, outputStemPath, passName, "fragment",
                    pass, parser, sourceBase, context, pipeline, true, false,
                    backendCache, &diagnostics);
                if (output.success) {
                    compiledCount++;
                } else {
                    errorCount++;
                    if (output.skipRemainingPass) continue;
                }
            }
        }

        // Compile compute shader
        if ((config.stageFilter.empty() || config.stageFilter == "compute") &&
            !pass.computeShader.IsNull()) {

            if (!config.errorsJson) {
                printf("  Compute shader:\n");
                fflush(stdout);
            }
            CompileResult result = CompileShaderStage(context, parser, pass,
                                                       ShaderStage::Compute, config.verbose, config.dumpIr, config.debugNames,
                                                       true, false, specializedPipelineRef,
                                                       pipeline.passes[passIdx], nullptr, config.outputInternals,
                                                       config.useDirectGles, config.outputGlslEs, &config.renderConfig,
                                                       &diagnostics,
                                                       passName,
                                                       config.errorsJson);

            if (!result.success) {
                AddCompileResultDiagnostics(diagnostics, result, config.inputFile, passName, "compute");
                errorCount++;
            } else {
                computeReflectionAnalysis = result.analysis;
                computeReflectionSamplerUses = result.explicitSamplerUses;
                haveComputeReflectionAnalysis = true;
                // Record timing for this shader
                timing.shaderTimings.push_back({outputStem + ".comp", result.timing});
                ShaderTiming& shaderTime = timing.shaderTimings.back().second;

                StageOutputResult output = WriteStageOutputs(
                    config, result, shaderTime, outputStemPath, passName, "compute",
                    pass, parser, sourceBase, context, pipeline, false, false,
                    backendCache, &diagnostics);
                if (output.success) {
                    compiledCount++;
                } else {
                    errorCount++;
                    if (output.skipRemainingPass) continue;
                }
            }
        }

        if (!config.checkOnly && (config.outputInternals || config.outputBindings)) {
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

            std::string bindingsPath = outputStemPath + ".bindings.json";
            std::string bindingsJson = BuildResourceBindingsJson(passName, bindings);
            if (WriteTextFile(bindingsPath, bindingsJson)) {
                if (!config.errorsJson) {
                    printf("  -> %s\n", bindingsPath.c_str());
                }
            }
        }
    }

    if (config.errorsJson) {
        PrintDiagnosticsJson(config, diagnostics, errorCount == 0, &stream, &sourceLines);
    } else {
        PrintDiagnosticsText(config, diagnostics, &stream, &sourceLines);
        printf("\n%s: %d shaders %s",
               config.checkOnly ? "Check complete" : "Done",
               compiledCount,
               config.checkOnly ? "checked" : "compiled");
        if (errorCount > 0) {
            printf(", %d errors", errorCount);
        }
        printf("\n");
    }

    // Calculate and print timing if requested
    if (config.showTiming && !config.errorsJson) {
        auto totalEnd = Clock::now();
        timing.totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
        timing.Print();
    }

    return errorCount > 0 ? 1 : 0;
    } catch (const std::exception& e) {
        diagnostics.AddMessage(DiagnosticSeverity::Error,
                               DiagnosticPhase::Internal,
                               DiagnosticMessageId::UnhandledException,
                               {DiagnosticArg::String(e.what())},
                               DiagnosticSpan{},
                               config.inputFile);
        return emitFailure();
    } catch (...) {
        diagnostics.AddMessage(DiagnosticSeverity::Error,
                               DiagnosticPhase::Internal,
                               DiagnosticMessageId::UnhandledUnknownException,
                               {},
                               DiagnosticSpan{},
                               config.inputFile);
        return emitFailure();
    }
}
