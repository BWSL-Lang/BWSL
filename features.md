# BWSL Language Feature Inventory

This document inventories the BWSL language surface implemented in this repository, with a maturity assessment and a note on what makes each feature distinctive. It is based on `docs/spec/`, `docs/language.md`, `bwsl_token_defs.h`, `bwsl_parser_soa.cpp`, `bwsl_types.h`, `bwsl_stdlib.h`, `bwsl_ir_lowering.h`, `bwsl_compute_graph.cpp`, the standard modules, and the test corpus.

## Maturity Labels

- **Stable**: implemented, documented in the spec draft, and covered by regression tests.
- **Provisional**: implemented, but syntax, semantics, validation, or public guidance is expected to change.
- **Implementation-defined**: real compiler behavior, but not yet a polished language rule.
- **Reserved or planned**: tokens or scaffolding exist, but the feature is not part of the current supported language.

The current regression suite contains 545 `.bwsl` files, including 150 equivalence tests, 213 fuzz regressions, and 17 explicit frontend/variant error cases. `tests/TEST_RESULTS.md` records a local April 9, 2026 run with 129 passed, 0 failed, and 2 skipped module support files.

## Feature Matrix

| Feature | Maturity | What it includes | Uniqueness |
| --- | --- | --- | --- |
| Pipeline files | Stable | `pipeline Name { ... }` with imports, attributes, resources, variants, compute graphs, constants, constraints, enums, structs, helper functions, passes, and inline modules. | The source file is organized around render and compute pipeline construction instead of standalone shader entry points. |
| Module files | Stable | `module Name { ... }`, module imports, constants, functions, structs, enums, and module-qualified `Name::member` access. | Shader libraries are first-class BWSL source units, not preprocessor includes. |
| Import system | Stable | `import Math` and comma-separated imports, with CLI-configured module search paths. | Keeps reusable shader code modular while preserving compiler-controlled symbol resolution. |
| Lexical structure | Stable | ASCII identifiers, whitespace, line comments, nestable block comments, strings, integers, floats, scientific notation, hex, binary, unsigned suffixes, operators, decorators. | The lexer intentionally handles shader-specific ambiguity such as `0..10` ranges and `value.x` member access. |
| Core scalar/vector/matrix types | Stable | `bool`, `int`, `uint`, `float`, `int2/3/4`, `uint2/3/4`, `float2/3/4`, `mat2/3/4`, `void`. | Uses concise GPU-native type names while targeting SPIR-V, Metal, HLSL, GLSL, and GLES. |
| Matrix aliases | Provisional | `float2x2`, `float3x3`, `float4x4` accepted as aliases for `mat2`, `mat3`, `mat4`. | Smooths migration from backend shader languages while keeping canonical BWSL spelling shorter. |
| Reserved 64-bit types | Reserved or planned | Tokens and `CoreType` entries for `double`, `int64`, `uint64`, vector forms, and `dmat*`. | Indicates future direction, but not a documented stable source surface. |
| Resource types | Stable | `texture2D`, `texture3D`, `textureCube`, `texture2DArray`, `sampler`, `buffer<T>`, `cbuffer<T>`, plus custom struct-backed resources. | BWSL separates source-level resource names from backend binding layout and emits reflection as ABI. |
| Structs | Stable | Pipeline/module structs, custom nominal types, nested structs, arrays in fields, positional constructors, struct returns, and member chains. | Structs are used both as shader data types and as reflected resource payload shapes. |
| Fixed-size arrays | Stable | Local arrays, struct-field arrays, multidimensional arrays, array indexing, dynamic indexing, arrays of structs, and array field access. | The implementation flattens complex array access through IR and backend lowering while preserving source convenience. |
| Pointers | Stable with restrictions | `Type^`, prefix address-of `^value`, postfix dereference `ptr^`, pointer parameters, pointer control flow, pointer-to-field and array-element patterns. | A shader language with explicit pointer syntax but a compact `^` design shared with XOR by position. |
| Constants | Stable with implementation-defined folding | `const` at module, pipeline, pass, and local scopes; compile-time evaluated scalar constants where possible. | Constants participate in parser-time and specialization flows, enabling stage selection and variant pruning. |
| Constructors and casts | Stable | Type constructors such as `float3(...)`, splats, mixed vector construction, matrix construction, numeric conversions, scalar/vector casts. | Constructor calls double as explicit conversion syntax and are validated through the compiler type system. |
| Expressions | Stable | Literals, identifiers, parenthesized expressions, calls, member access, array access, module qualification, enum qualification, ternary, assignments, and compound assignments. | Expression support is broad enough for production shader math without exposing backend-specific syntax. |
| Operators | Stable | Arithmetic, comparison, logical, bitwise, shifts, unary plus/minus/not/bitwise-not, prefix/postfix increment/decrement, assignment and compound assignment. | BWSL keeps familiar C-family precedence while reserving `skip` as the continue-like statement. |
| Swizzles | Stable | Vector `.x/.y/.z/.w`, `.r/.g/.b/.a`, multi-component swizzles, swizzle reads and writes, swizzles on function results. | The implementation tracks swizzles through composite lowering across backends. |
| Control flow | Stable | Blocks, `if`/`else`, `else if`, single-statement bodies, C-style `for`, `while`, range `for`, collection `for`, `foreach`, `loop`, `switch`, `return`, `break`, `skip`, `discard`. | Includes several shader-friendly loop forms and conditional jump variants without macros. |
| Conditional returns | Stable | `return if (cond);` and `return expr if (cond);`. | Concise early-exit syntax maps to guarded IR control flow. |
| Conditional break/skip | Stable | `break if (cond);` and `skip if (cond);`. | Makes branch-heavy shader loops less noisy while preserving structured control flow. |
| Range loops | Stable | `for (i in 0..n)`, inclusive `0..=n`, and stepped `by` ranges. | Gives shader code a compact iteration syntax that still lowers to backend loops. |
| Collection loops | Stable | `for (item in values)` and implicit `for (values)` using `it`. | Provides high-level iteration syntax over compiler-recognized collections and arrays. |
| Multi-range foreach | Stable | `foreach (x in 0..w, y in 0..h)`. | Directly expresses nested grid expansion patterns common in shader generation. |
| Loop statement | Stable | `loop (count)`, `loop { ... }`, and `loop { ... } until (cond)`. | Offers count-driven and post-condition loops in a single construct. |
| Switch | Stable | `switch (expr)`, multi-value `case`, `default`, fallthrough-related behavior covered by regression tests. | Supports shader-friendly finite branching without requiring chained `if` ladders. |
| Discard | Stable | `discard;` in fragment-style control flow. | Maps directly to fragment kill/discard behavior across targets. |
| Functions | Stable | `name :: (params) -> return_type { ... }`, pipeline/pass/module/enum scopes, overloads by parameter types, custom return types. | The `::` declaration form distinguishes definitions from calls and makes module qualification visually consistent. |
| Parameter syntax | Stable canonical, compatibility forms implementation-defined | Canonical `Type name`, plus accepted `name: Type`, module-qualified types, pointers, arrays, and anonymous parameters in some contexts. | Eases authoring and migration while the spec can standardize a canonical style. |
| Function overloading | Stable | Overloads keyed by parameter types, including module functions and methods. | Provides library ergonomics without backend preprocessor tricks. |
| Recursion rejection | Stable error behavior | Recursive calls are rejected by the test suite. | Keeps lowering finite and shader-target friendly. |
| Stage-returning functions | Stable for vertex/fragment, provisional for compute | Functions returning `vertex_function`, `fragment_function`, and parsed `compute_function` return types. Vertex and fragment stage assignment is exercised heavily; compute functions are parsed but not exposed through the normal `compute "Name" [x,y,z]` pass grammar. | Treats shader stage bodies as composable compile-time values. |
| Pass-block-returning functions | Provisional | Parser accepts `-> pass_block` and parses pass bodies inside functions. | Points toward higher-level pass composition, but not a normal documented user feature yet. |
| Stage assignment | Stable | `vertex = func()`, `fragment = func()`, compile-time ternary selection, and parameter substitution. | Makes shader variants compositional without duplicating full pass blocks. |
| Pass-stage reuse | Stable | `vertex = "OtherPass".vertex` and `fragment = "OtherPass".fragment`. | Lets one pass reuse stage code from another pass by name. |
| Depth-only passes | Stable | `fragment = null`. | Explicitly models graphics passes that only need vertex/depth output. |
| Constraint-based generics | Stable | `constraint FloatVectors = float2 | float3 | float4;` and constrained function parameters/returns. | BWSL's current generic model is set-based and shader-type oriented rather than angle-bracket templates. |
| Type-pattern dispatch | Stable | Generic bodies with `float2: expr`, `float3: { ... }`, and `default:` arms. | Compile-time dispatch by concrete shader type gives generic code backend-static output. |
| Unsupported generic syntax | Reserved or planned | `where` token exists, but `where` clauses and `foo<T>` style generics are not supported. | The implementation deliberately favors constraints over a C++-style template surface. |
| Enums | Stable | Plain enums, optional integer underlying type, explicit values, auto values, module-qualified enum access. | Enums can be used both as shader constants and variant choices. |
| Flag enums | Stable with implementation-defined auto-value heuristic | Explicit-valued integer enums support bitwise operations, with flag-like auto values using powers of two. | Gives shader authors bitmask types without separate macro constants. |
| Payload enums | Stable | Sum-type variants such as `Sphere(float radius)` and `Box(float3 size)`. | This is uncommon in shader languages and enables algebraic data modeling in GPU code. |
| Enum methods | Stable | Methods declared inside enum bodies, optional `eval`, implicit `self`. | Brings behavior near data definitions without requiring global helper naming conventions. |
| Pattern-match arms | Stable with implementation-defined lowering details | Variant arms, payload bindings, `_` wildcard, `default`, expression bodies, block bodies, implicit match on `self`. | Adds algebraic-data-style matching to shader code while lowering to ordinary IR. |
| Shader pipelines | Stable | Passes with graphics stages or compute stages, pass-scoped resources/attributes/functions/constants. | The language describes multi-pass pipelines, not just individual shader functions. |
| Graphics passes | Stable | `vertex { ... }` plus optional `fragment { ... }`, `use attributes`, `use resources`, varyings through `output.*` and `input.*`. | Varyings are inferred from vertex `output.*` writes and fragment `input.*` reads. |
| Compute passes | Stable | `compute "Name" [x, y, z] { ... }`, workgroup size, compute-only validation, no attributes. | Compute entry metadata lives in the source next to the shader body. |
| Attributes | Stable | Pipeline `attributes { name: type }`, first attribute must be `position`, pass-level `use attributes { ... }`. | Vertex layout is declared in BWSL source, then selected per pass. |
| Attribute decorators | Stable | `@compressed(...)` and `@instance`. | Supports engine-oriented vertex packing and instancing directly in the language surface. |
| Optional attributes | Stable | `use attributes { normal? }` introduces `variants.has_normal`. | Pass inputs and variant specialization are connected automatically. |
| Pipeline resources block | Stable | `resources { viewProj: mat4; colorTex: texture2D; particles: buffer<Particle>; }`, selected per pass with `use resources`. | Resource names are validated at source level instead of only through external config. |
| Optional resources | Stable | `use resources { colorTex?, colorSampler? }` introduces `variants.has_resource_colorTex` and related facts. | Resource availability becomes part of the variant system automatically. |
| Resource reflection | Stable | Binding JSON/reflection generated by compiler options; resource stage and access usage collected from IR. | The compiler, not the source author, owns the final ABI mapping. |
| Shader built-in namespaces | Stable | `attributes.*`, `input.*`, `output.*`, `resources.*`, `variants.*`, and `self`. | These namespaces provide a compact, target-independent shader ABI. |
| Vertex built-ins | Stable | `input.vertex_id`, `input.instance_id`; `output.position`. | Backend-specific names like `gl_VertexIndex` are hidden behind BWSL names. |
| Fragment built-ins | Stable | `input.position` for fragment coordinates, `output.color`, `output.depth`, `discard`. | Fragment coordinates and depth writes are backend-normalized. |
| Compute built-ins | Stable | `input.global_id`, `input.local_id`, `input.workgroup_id`, `input.num_workgroups`, `input.local_index`. | Mirrors common compute concepts across Metal, HLSL, GLSL, and SPIR-V. |
| User varyings | Stable | Any non-builtin `output.name` from vertex can be read as `input.name` in fragment. | Varying declarations are inferred from use instead of requiring duplicate interface blocks. |
| Shared memory | Stable | `shared Type name[...]` in compute stages. | First-class group-shared storage with tests for reductions, tiling, and atomics. |
| Barriers | Stable with stage validation in lowering | `barrier()`, `memoryBarrier()`, `storageBarrier()` for compute-oriented synchronization. | Abstracts target-specific barrier spellings and SPIR-V opcodes. |
| Atomics | Stable | `atomic_add`, `atomic_min`, `atomic_max`, `atomic_and`, `atomic_or`, `atomic_xor`, `atomic_exchange`, `atomic_cmp_exchange`. | Exposes a compact atomic catalog that lowers to backend-specific intrinsics. |
| Wave/subgroup operations | Stable with some policy implementation-defined | `wave_sum`, `wave_product`, `wave_min`, `wave_max`, `wave_all`, `wave_any`, `wave_broadcast`, `wave_read_first`. | Gives cross-backend subgroup operations under one naming scheme. |
| Texture and image operations | Stable | `sample`, `sample_lod`, `sample_bias`, `sample_grad`, `sample_cmp`, offset variants, `gather`, `load`, `store`, `texture_size`, `texture_levels`. | Supports both combined and explicit texture/sampler call shapes where implemented. |
| Derivatives | Stable | `ddx`, `ddy`, fine/coarse variants, `fwidth`, fine/coarse variants, fragment-only. | Normalizes derivative spelling across shader targets. |
| Math intrinsics | Stable | `lerp`, `smoothstep`, `saturate`, `fract`, `step`, `clamp`, `sign`, `abs`, `min`, `max`, `floor`, `ceil`, `round`, `trunc`, `mod`, `fmod`, `fma`, `pow`, `sqrt`, `rsqrt`, `rcp`, `exp`, `exp2`, `log`, `log2`, `log10`, `frexp`, `ldexp`, `modf`. | The source names are BWSL names; backend aliases like `mix` and `frac` are rejected as source-level compatibility aliases. |
| Trig intrinsics | Stable | `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sincos`, `sinh`, `cosh`, `tanh`, `degrees`, `radians`. | Includes paired and hyperbolic functions tested through backend output and equivalence cases. |
| Vector and matrix intrinsics | Stable | `dot`, `cross`, `normalize`, `length`, `distance`, `reflect`, `refract`, `faceforward`, `transpose`, `determinant`, `inverse`. | Handles matrix/vector semantics through IR rather than textual backend substitution. |
| Bit and packing intrinsics | Stable | `count_bits`, `reverse_bits`, `first_bit_low`, `first_bit_high`, `bitfield_extract`, `bitfield_insert`, `pack/unpack_unorm*`, `pack/unpack_snorm*`, `pack/unpack_half2x16`, `f32tof16`, `f16tof32`, `asfloat`, `asint`, `asuint`. | Provides portable bit manipulation and packing used by compressed vertex formats and GPU data paths. |
| Boolean reductions and classification | Stable | `any`, `all`, `isnan`, `isinf`, `isfinite`, `isnormal`. | Covers vector boolean reduction and floating classification in a backend-neutral way. |
| Select intrinsic | Stable | `select(false_value, true_value, condition)`. | Documents the exact argument order, avoiding target-specific `mix/select` ambiguity. |
| Variants block | Stable | `variants { name: bool = false; mode: Enum = EnumValue; rules { ... } }`. | Variants are first-class compile-time specialization inputs rather than preprocessor defines. |
| Variant rules | Stable | `require lhs -> rhs;` and `conflict lhs, rhs;`, validated against selected variants. | Lets source declare legal feature combinations for shader specialization. |
| Variant specialization | Stable | CLI `-variant name=value`, reflection, compile-time pruning of `variants.*` branches and stage selection. | Ties configuration, code elimination, reflection, and pass input optionality together. |
| Eval blocks and eval statements | Provisional | `eval { ... }`, eval declarations, `eval if`, `eval for`, `eval loop`, eval function declarations, compile-time expansion. | BWSL already has compile-time shader generation, but the README says it is moving toward a cleaner comptime model. |
| Compile-time evaluator | Provisional | Scalar constant evaluation, local eval bindings, substitutions, expansion budget, variant-aware cloning. | Enables parser-driven specialization today, with planned migration out of parser logic. |
| Compute graph | Provisional | Pipeline-level `compute_graph { node "Pass" { inputs { ... } outputs { ... } } }`, dependency validation, topological order, barrier derivation. | Starts to encode pass/resource scheduling in source, but public semantics are not settled. |
| Standard modules | Stable as library surface, not language syntax | `Math`, `Random`, `Noise`, `Color`, `Compression`, `PBR`, `Globals`, `PostFX`, and test modules. | Demonstrates that BWSL can host substantial shader libraries without leaving the language. |
| Production-style examples | Stable evidence | `tests/from_engine` and `tests/prod_shaders` cover world, character, shadow, postprocess, particles, UI, crowd, material preview. | The language is exercised against engine-shaped shaders, not only tiny grammar tests. |
| Backend generation | Stable tooling | SPIR-V first, cross-compilation to Metal, HLSL, GLSL 450, GLSL ES 300, and direct GLES path. | A single source language targets native and web graphics stacks. |
| WASM compiler | Stable tooling | Emscripten build exposing compile and symbol APIs. | Enables editor/browser integration for BWSL compilation and autocomplete. |
| Diagnostics and conformance tests | Stable | Error tests for missing semicolons, bad intrinsics, stage misuse, unknown imports, pointer ternary rejection, recursion, duplicate compute blocks, oversized arrays. | The test suite documents both accepted and intentionally rejected behavior. |
| Fuzz regression corpus | Stable process | More than 200 fuzz-found `.bwsl` regression files. | Maturity is backed by adversarial parser/lowering coverage, not just examples. |
| Custom cast extensions | Reserved or planned | `extend`/`extends` tokens and cast registry scaffolding exist; `modules/ColorCastExtensions.bwsl` shows experimental syntax. | Not currently a supported language feature because parser integration is not visible in the active grammar. |

## Distinctive Language Themes

1. **Pipeline-first design**: BWSL describes complete shader pipelines, passes, stage composition, resource use, and variants in one source language.
2. **Composable shader stages**: `vertex_function`, `fragment_function`, stage assignment, ternary stage selection, pass-stage reuse, and `fragment = null` make shader bodies reusable units. `compute_function` is parsed but still less public-facing.
3. **Typed specialization instead of textual preprocessing**: Variants, optional attributes/resources, enum-valued variant choices, rules, reflection, and branch pruning replace many preprocessor use cases.
4. **Algebraic features in shader code**: Payload enums, enum methods, and pattern arms are unusual for shader languages and support higher-level modeling.
5. **Constraint-based generics**: Generic functions are constrained by concrete shader type sets and can dispatch with type-pattern arms, keeping generated code static.
6. **Source-level engine integration**: Attributes, compressed vertex data decorators, instance attributes, resources, render config interop, and reflection map directly to engine concerns.
7. **Cross-backend intrinsic vocabulary**: BWSL intentionally accepts its own intrinsic names and rejects backend spellings where tests cover them, leaving backend translation to the compiler.
8. **Compute and graphics in one language**: Raster stages, compute workgroups, shared memory, barriers, atomics, wave operations, storage buffers, storage images, and compute graph work all live in one syntax.

## Current Limitations and Non-Features

- Angle-bracket generics and `where` clauses are not supported.
- Ternary expressions with pointer operands are intentionally rejected.
- Recursion is intentionally rejected.
- `eval` is useful but provisional; the README explicitly calls out planned movement toward a dedicated comptime model.
- `compute_graph` is implemented but lacks settled public documentation.
- `pass_block` is accepted by the parser but should not be treated as a normal public feature yet.
- 64-bit scalar/vector/matrix tokens exist but are not part of the stable documented source surface.
- Custom cast extension syntax is scaffolded but not currently a stable parser feature.
- Some overload resolution, constant substitution, flag-enum auto-value details, and pattern/eval interactions remain implementation-defined.
