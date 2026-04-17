// libFuzzer harness for bwslc. Feeds raw bytes as BWSL source to the
// lex+parse pipeline, swallowing expected errors so libFuzzer only surfaces
// crashes / ASan / UBSan violations.
//
// Scope: lex+parse only. That's where the "weird input bytes" bugs live and
// where the existing equivalence/regression suites have the weakest coverage.
// Backend codegen has dedicated test harnesses already.
//
// Build:
//   make bwslc-fuzz
// Run (seed corpus is tests/*.bwsl):
//   ./build/bwslc-fuzz -max_len=8192 fuzz/corpus/

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <fcntl.h>

#include "../bwsl_defs.h"
#include "../bwsl_arena.h"
#include "../bwsl_mem_pool.h"
#include "../bwsl_render_config.h"
#include "../bwsl_parser_soa.h"
#include "../bwsl_lexer.h"

#include "../bwsl_lexer.cpp"
#include "../bwsl_parser_soa.cpp"
#include "../bwsl_eval_soa.cpp"
#include "../bwsl_module_cache.cpp"
#include "../bwsl_custom_type_registry.cpp"
#include "../bwsl_variant_system.cpp"

using namespace BWSL;

// Redirect stdout+stderr to /dev/null only for the duration of the compile.
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

    RenderConfig defaultConfig;
    SymbolTable::InitFromRenderConfig(&parser.symbolTable, defaultConfig);

    bool isModule = (parser.CurrentTokenType() == TokenType::MODULE);
    if (isModule) {
        (void) parser.ParseModuleFile();
    } else {
        (void) parser.ParsePipeline();
    }

    return 0;
}
