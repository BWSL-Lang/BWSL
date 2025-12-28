// bwsl_unity.cpp - Unity build include for BWSL compiler
// This file is included by the main platform file (e.g., osx_bwe.mm) to compile
// all BWSL compiler sources as part of the unity build.
//
// NOTE: Only include the SoA-based implementations. The non-SoA versions
// (bwsl_parser.cpp, bwsl_eval.cpp, bwsl_ast.h, bwsl_eval_cache.cpp, bwsl_cast_registry.cpp)
// are DEPRECATED and use the old ASTNode struct.

#include "bwsl_lexer.cpp"
#include "bwsl_parser_soa.cpp"
#include "bwsl_eval_soa.cpp"
#include "bwsl_module_cache.cpp"
#include "bwsl_ir_gen.cpp"
#include "bwsl_ir_analysis.cpp"
#include "bwsl_cfg.cpp"
#include "bwsl_ssa.cpp"
#include "bwsl_spirv_backend.cpp"
#include "bwsl_compute_graph.cpp"
#include "bwsl_custom_type_registry.cpp"

// Platform-specific middleware (Objective-C++ for Metal)
#include "middleware/bwsl_metal_middleware.mm"
