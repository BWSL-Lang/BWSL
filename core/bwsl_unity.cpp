// bwsl_unity.cpp - Unity build include for BWSL compiler
// This file is included by the main platform file (e.g., osx_bwe.mm) to compile
// all BWSL compiler sources as part of the unity build.
//
// NOTE: Only include the SoA-based implementations. The non-SoA versions
// (bwsl_parser.cpp, bwsl_eval.cpp, bwsl_ast.h, bwsl_eval_cache.cpp, bwsl_cast_registry.cpp)
// are DEPRECATED and use the old ASTNode struct.

#include "../phases/lexing/bwsl_lexer.cpp"
#include "../phases/parser/bwsl_parser_soa.cpp"
#include "../phases/evaluation/bwsl_eval_soa.cpp"
#include "../phases/evaluation/bwsl_comptime_interpreter.cpp"
#include "bwsl_module_cache.cpp"
#include "../phases/ir_generation/bwsl_ir_gen.cpp"
#include "../phases/ir_generation/bwsl_ir_analysis.cpp"
#include "../phases/control_flow/bwsl_cfg.cpp"
#include "../phases/ssa/bwsl_ssa.cpp"
#include "../phases/backends/spirv/bwsl_spirv_backend.cpp"
#include "../phases/ir_generation/bwsl_compute_graph.cpp"
#include "bwsl_custom_type_registry.cpp"

// Platform-specific middleware (Objective-C++ for Metal)
#include "middleware/bwsl_metal_middleware.mm"
