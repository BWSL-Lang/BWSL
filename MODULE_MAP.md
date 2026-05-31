# BWSL Practical Module Map for Brawl Editor

## Module: Project Guidance
id: project-guidance
files:
- AGENTS.md
- CLAUDE.md
- GEMINI.md
- CODING_STANDARDS.md
- CONTRIBUTING.md
- README.md
- CHANGELOG.md
- NAME_USAGE.md
- features.md
interfaces:
Repository build commands, contribution rules, coding style, language inventory, naming guidance.
depends_on:
None.
notes:
Use as high-level orientation before generating code; coding standards emphasize data-oriented structs, free functions, SoA storage, arenas, and stable IDs over pointers.

## Module: Core Memory and Utilities
id: core-memory-utils
files:
- core/bwsl_defs.h
- core/bwsl_arena.h
- core/bwsl_mem_pool.h
- core/bwsl_utils.h
interfaces:
Primitive aliases, result codes, arena allocation, IR chunk pool, hashing, reverse lookup, source line mapping.
depends_on:
None.
notes:
Foundational layer used nearly everywhere. Preserve alias choices and include-order sensitivity around third-party headers.

## Module: Core Type System and Intrinsics
id: core-type-system-intrinsics
files:
- core/bwsl_types.h
- core/bwsl_compiler_types.h
- core/bwsl_stdlib.h
- core/bwsl_cast_registry.h
- core/bwsl_cast_registry.cpp
- core/bwsl_custom_type_registry.h
- core/bwsl_custom_type_registry.cpp
interfaces:
CoreType, TypeInfo, ArenaString, shader/pass/resource enums, intrinsic catalog, cast lookup, custom struct type registration.
depends_on:
core-memory-utils, lexing-token-stream, ast-storage-model.
notes:
This is the semantic vocabulary shared by parser, lowering, analysis, and backends. Intrinsic metadata is the source of truth for builtin names and SPIR-V operation mapping.

## Module: AST Storage Model
id: ast-storage-model
files:
- core/bwsl_ast_common.h
- core/bwsl_ast_soa.h
interfaces:
ASTNodeType, LiteralValue, NodeRef, ArenaArray, SoA AST pools for pipeline/module/pass/function/expression/statement data.
depends_on:
core-memory-utils, core-type-system-intrinsics.
notes:
Canonical AST representation. New syntax should add compact node data here and keep cross-references as NodeRef indices.

## Module: Lexing and Token Stream
id: lexing-token-stream
files:
- phases/lexing/bwsl_token_defs.h
- phases/lexing/bwsl_token_stream.h
- phases/lexing/bwsl_lexer.h
- phases/lexing/bwsl_lexer.cpp
interfaces:
TokenType, TokenRef, TokenStream, Lexer, keyword lookup, token value/source-location access.
depends_on:
core-memory-utils.
notes:
Owns source-to-token conversion. TokenStream is SoA and uses SIMDe include ordering; avoid introducing dependencies that break portable SIMD/header macro constraints.

## Module: Symbol Table and Name Resolution
id: symbol-table-name-resolution
files:
- core/bwsl_symbol_table.h
interfaces:
SymbolTableData, Symbol, SymbolKind, scoped lookup/registration helpers, overload masks, resource/attribute/function/type metadata.
depends_on:
core-memory-utils, lexing-token-stream, ast-storage-model, module-cache.
notes:
Central semantic index for parser and lowering. Most user-visible declarations eventually register here.

## Module: Parser Frontend
id: parser-frontend
files:
- phases/parser/bwsl_parser_soa.h
- phases/parser/bwsl_parser_soa.cpp
interfaces:
CompilationContext, Parser, ParseError, module search path configuration, source-to-AST parse entry points.
depends_on:
lexing-token-stream, ast-storage-model, symbol-table-name-resolution, compile-time-evaluation, variant-specialization, core-type-system-intrinsics.
notes:
Recursive descent frontend and much of the current semantic validation. ProgressGuard exists to keep recovery loops terminating on malformed input.

## Module: Compile-Time Evaluation
id: compile-time-evaluation
files:
- phases/evaluation/bwsl_eval_soa.h
- phases/evaluation/bwsl_eval_soa.cpp
- phases/evaluation/bwsl_eval_cache.cpp
- phases/evaluation/bwsl_comptime_interpreter.h
- phases/evaluation/bwsl_comptime_interpreter.cpp
interfaces:
EvalCache, EvalStateSoA, literal folding helpers, RunComptimeInterpreter, comptime values/bindings/scopes/diagnostics.
depends_on:
ast-storage-model, parser-frontend, core-type-system-intrinsics.
notes:
Handles constant folding and eval block expansion. Keep budgets and safe numeric conversions intact for fuzz robustness.

## Module: Module Cache and Imports
id: module-cache-imports
files:
- core/bwsl_common_modules.h
- core/bwsl_module_cache.h
- core/bwsl_module_cache.cpp
- core/bwsl_module_integration.h
interfaces:
Common module hashes, ModuleCache, global g_moduleCache, import handling, module recompilation/invalidation, export lookup.
depends_on:
core-memory-utils, ast-storage-model, parser-frontend, symbol-table-name-resolution, compiler-service-core.
notes:
Supports incremental module compilation and dependency invalidation. Brawl Editor should treat this as the bridge between editor file/module state and compiler symbols.

## Module: Variant Specialization
id: variant-specialization
files:
- core/bwsl_variant_system.h
- core/bwsl_variant_system.cpp
- core/bwsl_variant_analysis.h
- core/bwsl_variant_analysis.cpp
interfaces:
Variant selection data, implicit variant facts, variant reflection JSON, variant analysis points and required variant calculation.
depends_on:
ast-storage-model, symbol-table-name-resolution, render-config-model.
notes:
Variant analysis file appears to reference older ASTNode-style APIs; use variant_system as the current reflection/selection surface and verify before extending analysis.

## Module: Render Config and Reflection
id: render-config-reflection
files:
- core/bwsl_render_config.h
- core/bwsl_resource_reflection.h
- core/bwsl_reflection_json.h
interfaces:
RenderConfig structures, resource binding descriptors, ResourceReflectionConfig, ReflectedResourceBinding, compact/full JSON reflection emitters.
depends_on:
ast-storage-model, ir-core-analysis, symbol-table-name-resolution, core-type-system-intrinsics.
notes:
This is the ABI-facing surface Brawl Editor will likely consume for resources, bindings, stages, attributes, and generated metadata.

## Module: IR Core and Analysis
id: ir-core-analysis
files:
- phases/ir_generation/bwsl_ir_gen.h
- phases/ir_generation/bwsl_ir_gen.cpp
- phases/ir_generation/bwsl_ir_analysis.h
- phases/ir_generation/bwsl_ir_analysis.cpp
interfaces:
IR::IRProgram, opcodes/registers/constants/metadata arrays, optimization pass hooks, IRAnalysis capability/resource/input/output masks.
depends_on:
core-memory-utils, core-type-system-intrinsics, control-flow-ssa.
notes:
Register-based SoA IR shared by lowering, CFG/SSA, reflection, and backends. Resource and capability masks drive backend declarations.

## Module: IR Lowering
id: ir-lowering
files:
- phases/ir_lowering/bwsl_ir_lowering.h
interfaces:
AST-to-IR lowering, PassVaryingContext, stage/resource/attribute lowering, type and overload lowering helpers.
depends_on:
ast-storage-model, symbol-table-name-resolution, ir-core-analysis, render-config-reflection, core-type-system-intrinsics.
notes:
Large header-only implementation and key semantic choke point. Most language behavior that affects generated shader code is finalized here.

## Module: Compute Graph Compilation
id: compute-graph-compilation
files:
- phases/ir_generation/bwsl_compute_graph.h
- phases/ir_generation/bwsl_compute_graph.cpp
interfaces:
CompileComputeGraph, CompiledComputeGraph, DerivedBarrier, BarrierType.
depends_on:
ast-storage-model, render-config-reflection.
notes:
Compiles pipeline compute_graph declarations into execution order and resource barriers. Useful for editor visualization and pass scheduling.

## Module: Control Flow and SSA
id: control-flow-ssa
files:
- phases/control_flow/bwsl_cfg.h
- phases/control_flow/bwsl_cfg.cpp
- phases/ssa/bwsl_ssa.h
- phases/ssa/bwsl_ssa.cpp
- phases/ssa/bwsl_ssa_verify.cpp
interfaces:
CFG, CFGBuilder, dominance/frontier helpers, SSAConstructor, SSA verifier.
depends_on:
ir-core-analysis, core-memory-utils.
notes:
Transforms linear IR into block graph and SSA form for structured SPIR-V emission. Keep instruction-to-block and phi metadata consistent when changing control-flow opcodes.

## Module: SPIR-V Backend
id: spirv-backend
files:
- phases/backends/spirv/bwsl_spirv_backend.h
- phases/backends/spirv/bwsl_spirv_backend.cpp
interfaces:
SPIRVBuilder and SPIR-V binary generation from IR, CFG, symbol/reflection data.
depends_on:
ir-core-analysis, ir-lowering, control-flow-ssa, render-config-reflection, core-type-system-intrinsics.
notes:
Primary binary backend. Implementation is split through included .inl files not listed in the manifest; preserve include order and section responsibilities.

## Module: Direct GLES Backend
id: direct-gles-backend
files:
- phases/backends/gles/bwsl_gles_backend.h
- phases/backends/gles/bwsl_gles_backend.cpp
interfaces:
GLESBuilder and direct GLSL ES 300 text emission from IR.
depends_on:
ir-core-analysis, ir-lowering, control-flow-ssa, render-config-reflection.
notes:
Bypasses SPIR-V for WebGL/GLES output. Inlining and hoisting decisions depend on register use counts and CFG block placement.

## Module: Compiler Service Core
id: compiler-service-core
files:
- core/bwsl_compiler_service_core.h
interfaces:
BWSLCompilerServiceCore, TargetBackend, CompilationConfig, VertexPullingConfig, CompiledVariant, compilation/hot-reload orchestration.
depends_on:
parser-frontend, compile-time-evaluation, ir-lowering, ir-core-analysis, control-flow-ssa, spirv-backend, render-config-reflection.
notes:
Platform-neutral runtime API for compiling shaders and variants. This is the likely integration point for Brawl Editor compilation requests.

## Module: Platform Middleware
id: platform-middleware
files:
- core/middleware/bwsl_middleware_interface.h
- core/middleware/bwsl_metal_middleware.h
- core/middleware/bwsl_metal_middleware.mm
- core/bwsl_compiler_service.h
interfaces:
ShaderMiddleware function table, MiddlewareCompilationResult, Metal middleware factory, Metal-specific compiler service wrapper.
depends_on:
compiler-service-core, spirv-backend, render-config-reflection.
notes:
Bridges compiler output to platform shader objects. Metal code is Objective-C++ and Apple-gated; keep SPIRV-Cross macro isolation in mind.

## Module: Unity Build Integration
id: unity-build-integration
files:
- core/bwsl_unity.cpp
interfaces:
Single include aggregation for engine-side unity builds.
depends_on:
lexing-token-stream, parser-frontend, compile-time-evaluation, module-cache-imports, ir-core-analysis, control-flow-ssa, spirv-backend, compute-graph-compilation, platform-middleware.
notes:
Includes first-party implementation units directly. Update only when adding/removing implementation files intended for unity builds.

## Module: Command Line and Native Tools
id: cli-native-tools
files:
- tools/bwslc.cpp
- tools/spirv_cross_wrapper.cpp
- tools/equiv_runner.cpp
interfaces:
Native bwslc executable, isolated SPIRV-Cross wrapper, equivalence runner.
depends_on:
compiler-service-core, spirv-backend, direct-gles-backend, platform-middleware.
notes:
bwslc drives local compilation and regression output. spirv_cross_wrapper is compiled separately to avoid alias/macro conflicts.

## Module: WebAssembly and Browser Tooling
id: wasm-browser-tooling
files:
- tools/bwsl_wasm.cpp
- tools/defs_wasm.h
- tools/render_config_wasm.h
interfaces:
Emscripten/WASM compiler entry point, WASM compatibility typedefs, browser-facing render config bridge.
depends_on:
compiler-service-core, render-config-reflection, direct-gles-backend, spirv-backend.
notes:
Use for embedding compiler features into Brawl Editor web surfaces. Excludes generated tools/wasm_build/bwsl.js.

## Module: Fuzz Harness and Fuzz Docs
id: fuzz-harness-docs
files:
- tools/bwslc_fuzz.cpp
- fuzz/README.md
- fuzz/HANDOFF.md
interfaces:
libFuzzer full-pipeline harness and operational fuzzing guidance.
depends_on:
parser-frontend, compile-time-evaluation, ir-lowering, control-flow-ssa, spirv-backend, cli-native-tools.
notes:
Harness covers lex, parse, IR lowering, CFG/SSA, and SPIR-V. Read HANDOFF before changing parser recovery or numeric parsing.

## Module: Language Documentation
id: language-documentation
files:
- docs/language.md
- docs/spec/README.md
- docs/spec/00-status.md
- docs/spec/01-lexical-structure.md
- docs/spec/02-program-structure.md
- docs/spec/03-types-and-declarations.md
- docs/spec/04-expressions.md
- docs/spec/05-statements-and-control-flow.md
- docs/spec/06-functions-modules-and-generics.md
- docs/spec/07-enums-and-pattern-matching.md
- docs/spec/08-pipelines-passes-and-shader-io.md
- docs/spec/09-intrinsics-and-builtins.md
- docs/spec/10-conformance.md
interfaces:
Normative language draft, compact language reference, conformance/editing rules.
depends_on:
lexing-token-stream, parser-frontend, core-type-system-intrinsics, ir-lowering, test-suite.
notes:
Spec should move with compiler and tests when syntax or semantics change. Brawl Editor codegen should prefer spec wording, then implementation behavior when incomplete.

## Module: Test Suite
id: test-suite
files:
- tests/GEMINI.md
- tests/TEST_RESULTS.md
- tests/ideal/README.md
- tests/run_tests.py
interfaces:
Regression runner, golden/ideal output guidance, recorded test results and testing notes.
depends_on:
cli-native-tools, language-documentation.
notes:
Executable conformance baseline. Add focused .bwsl tests when changing parser, semantic validation, IR, reflection, or backend output.
