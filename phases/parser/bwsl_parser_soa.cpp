#include <cstddef>
#include "bwsl_parser_soa.h"
#include "bwsl_eval_soa.h"
#include "bwsl_utils.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <stdexcept>
#include "bwsl_custom_type_registry.h"

namespace {
// Fuzzer-proof integer parsers: return 0 on empty / malformed / out-of-range
// input instead of throwing. The lexer's number rules are not strict enough
// to guarantee std::stoul / std::stoi success (e.g. "0x" with no digits,
// "0b2", trailing garbage); these helpers keep the parser safe against
// adversarial input.
inline u32 SafeParseU32(std::string_view s, int base = 0) {
    if (s.empty()) return 0;
    try {
        return static_cast<u32>(std::stoul(std::string(s), nullptr, base));
    } catch (const std::exception&) {
        return 0;
    }
}
inline int SafeParseInt(std::string_view s, int base = 0) {
    if (s.empty()) return 0;
    try {
        return std::stoi(std::string(s), nullptr, base);
    } catch (const std::exception&) {
        return 0;
    }
}
inline float SafeParseFloat(std::string_view s) {
    if (s.empty()) return 0.0f;
    try {
        return std::stof(std::string(s));
    } catch (const std::exception&) {
        return 0.0f;
    }
}
}

// Parser timing instrumentation - define BWSL_PARSER_TIMING to enable
#ifdef BWSL_PARSER_TIMING
#include <chrono>
#include <cstdio>

namespace parser_timing {
    struct Stats {
        double advance_time = 0;
        double expression_time = 0;
        double statement_time = 0;
        double block_time = 0;
        double function_time = 0;
        double string_alloc_time = 0;
        int advance_count = 0;
        int expression_count = 0;
        int statement_count = 0;
        int string_alloc_count = 0;

        void print() {
            fprintf(stderr, "\n=== Parser Timing ===\n");
            fprintf(stderr, "Advance():     %.3f ms (%d calls, %.3f us/call)\n",
                    advance_time, advance_count, advance_count ? advance_time*1000/advance_count : 0);
            fprintf(stderr, "Expression:    %.3f ms (%d calls)\n", expression_time, expression_count);
            fprintf(stderr, "Statement:     %.3f ms (%d calls)\n", statement_time, statement_count);
            fprintf(stderr, "Block:         %.3f ms\n", block_time);
            fprintf(stderr, "Function:      %.3f ms\n", function_time);
            fprintf(stderr, "String alloc:  %.3f ms (%d allocs, %.3f us/alloc)\n",
                    string_alloc_time, string_alloc_count, string_alloc_count ? string_alloc_time*1000/string_alloc_count : 0);
            fprintf(stderr, "=====================\n\n");
        }

        void reset() { *this = Stats{}; }
    };

    inline Stats& get() { static Stats s; return s; }

    struct Timer {
        double* acc;
        std::chrono::high_resolution_clock::time_point start;
        Timer(double* a) : acc(a), start(std::chrono::high_resolution_clock::now()) {}
        ~Timer() {
            auto end = std::chrono::high_resolution_clock::now();
            *acc += std::chrono::duration<double, std::milli>(end - start).count();
        }
    };
}

#define PARSER_TIME_ADVANCE() parser_timing::Timer _t(&parser_timing::get().advance_time); parser_timing::get().advance_count++
#define PARSER_TIME_EXPR() parser_timing::Timer _t(&parser_timing::get().expression_time); parser_timing::get().expression_count++
#define PARSER_TIME_STMT() parser_timing::Timer _t(&parser_timing::get().statement_time); parser_timing::get().statement_count++
#define PARSER_TIME_BLOCK() parser_timing::Timer _t(&parser_timing::get().block_time)
#define PARSER_TIME_FUNC() parser_timing::Timer _t(&parser_timing::get().function_time)
#define PARSER_TIME_STRING() parser_timing::Timer _t(&parser_timing::get().string_alloc_time); parser_timing::get().string_alloc_count++
#define PARSER_TIMING_PRINT() parser_timing::get().print()
#define PARSER_TIMING_RESET() parser_timing::get().reset()

#else

#define PARSER_TIME_ADVANCE()
#define PARSER_TIME_EXPR()
#define PARSER_TIME_STMT()
#define PARSER_TIME_BLOCK()
#define PARSER_TIME_FUNC()
#define PARSER_TIME_STRING()
#define PARSER_TIMING_PRINT()
#define PARSER_TIMING_RESET()

#endif

namespace BWSL {

// =============================================================================
// Parser implementation table of contents
// =============================================================================
// The parser keeps one Parser object and one SoA AST representation. These
// slices are included into this translation unit in dependency order so the
// unity build, data layout, and hot parser state stay unchanged.
//
// 1. bwsl_parser_soa_core.inl             - helpers, token navigation, diagnostics
// 2. bwsl_parser_soa_decls.inl            - pipeline/pass/stage/resource declarations
// 3. bwsl_parser_soa_statements.inl       - blocks, statements, local declarations
// 4. bwsl_parser_soa_expressions.inl      - expressions, calls, member/array access
// 5. bwsl_parser_soa_functions.inl        - module lookup, functions, compute bodies
// 6. bwsl_parser_soa_variants_eval.inl    - eval blocks and variant resolution
// 7. bwsl_parser_soa_types.inl            - arrays, enums, pattern matches, structs
// 8. bwsl_parser_soa_modules_generics.inl - modules, constraints, generics
// 9. bwsl_parser_soa_specialization.inl   - cloning, specialization, final resolution

#include "bwsl_parser_soa_core.inl"
#include "bwsl_parser_soa_decls.inl"
#include "bwsl_parser_soa_statements.inl"
#include "bwsl_parser_soa_expressions.inl"
#include "bwsl_parser_soa_functions.inl"
#include "bwsl_parser_soa_variants_eval.inl"
#include "bwsl_parser_soa_types.inl"
#include "bwsl_parser_soa_modules_generics.inl"
#include "bwsl_parser_soa_specialization.inl"

} // namespace BWSL
