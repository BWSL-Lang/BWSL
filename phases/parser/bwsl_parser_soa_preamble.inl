#pragma once

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