#pragma once
// =============================================================================
// Parser implementation table of contents
// =============================================================================
// The parser keeps one Parser object and one SoA AST representation. These
// slices are included into this translation unit in dependency order so the
// unity build, data layout, and hot parser state stay unchanged.
//
// 0. bwsl_parser_soa_preamble.inl         - includes, forward declarations, and utility macros
// 1. bwsl_parser_soa_core.inl             - helpers, token navigation, diagnostics
// 2. bwsl_parser_soa_decls.inl            - pipeline/pass/stage/resource declarations
// 3. bwsl_parser_soa_statements.inl       - blocks, statements, local declarations
// 4. bwsl_parser_soa_expressions.inl      - expressions, calls, member/array access
// 5. bwsl_parser_soa_functions.inl        - module lookup, functions, compute bodies
// 6. bwsl_parser_soa_variants_eval.inl    - eval blocks and variant resolution
// 7. bwsl_parser_soa_types.inl            - arrays, enums, pattern matches, structs
// 8. bwsl_parser_soa_modules_generics.inl - modules, constraints, generics
// 9. bwsl_parser_soa_specialization.inl   - cloning, specialization, final resolution

#include "bwsl_parser_soa_preamble.inl"
#include "bwsl_parser_soa_core.inl"
#include "bwsl_parser_soa_decls.inl"
#include "bwsl_parser_soa_statements.inl"
#include "bwsl_parser_soa_expressions.inl"
#include "bwsl_parser_soa_functions.inl"
#include "bwsl_parser_soa_variants_eval.inl"
#include "bwsl_parser_soa_types.inl"
#include "bwsl_parser_soa_modules_generics.inl"
#include "bwsl_parser_soa_specialization.inl"
