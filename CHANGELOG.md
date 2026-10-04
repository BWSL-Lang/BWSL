# Changelog

## unreleased

Nothing so far.

## v0.10.0

### Breaking changes
- **Build targets were renamed.** `make bwslc`, `bwslc-debug`,
  `bwslc-win-zig` and `bwslc-win-zig-debug` are gone. Use `make build`, with
  `TARGET_OS=… TARGET_ARCH=…` for cross builds. `build.bat` still works.
- **`fast_sin` was removed from `math.bwsl`.** Shaders that import it must
  define their own.
- **The compiler is stricter.** Code that compiled before may now be rejected:
  - Non-void functions must return on every reachable path.
  - Integer literals that overflow 32 bits, empty radix prefixes (`0x`) and
    float literals that overflow are errors.
  - Bitwise and shift operators on floats are errors.
  - Duplicate local names in the same scope and duplicate struct names are
    errors.
  - Writing to a `const` through a member or swizzle, or taking its mutable
    address, is an error.
  - Constant out-of-bounds array indices are errors, including folded
    arithmetic.
  - Matrix constructors with partial, excess or mixed arguments are rejected
    instead of being padded or truncated.
  - Eval assignments must keep the declared type. Integers can promote to
    float, but other scalar kinds and vector component types or widths must
    match.
- **Signed `%` follows the dividend's sign.** `-4 % 3` is `-1`, in runtime code
  and at compile time. Float `mod()` is unchanged.
- **`&&`, `||` and `?:` now short-circuit.** Side effects in the unselected
  operand no longer happen.
- **AST JSON output is now schema `bwsl.ast.v3`.** See [AST JSON](#ast-json).

### Language
- **`switch` on variants is fixed.** It previously produced invalid SPIR-V.
  Switches on enum or bool variants are now pruned at compile time, and
  duplicate matches are diagnosed.
- **`eval` locals can be vectors** (`float2/3/4`, `int2/3/4`, `uint2/3/4`).
  They previously failed with "Unknown identifier".
- **Pipeline-scope `eval` functions now work.** They are callable from eval
  initializers and runtime expressions. They support loops, locals and early
  return, with a recursion depth limit of 128.
- **Enum pattern-match arms work in ordinary functions.** Previously only enum
  methods supported them. The first enum parameter is matched and the others
  stay in scope.
- **Trailing commas** are accepted in parameter lists, calls, arrays, `use`
  lists, enum payloads, workgroup sizes and `switch` patterns.
- **Binary literals** such as `0b1000` are accepted in workgroup sizes.
- **Ternary on aggregates is fixed.** `?:` selecting between structs, arrays
  or matrices previously produced invalid SPIR-V.
- **Assigning a float to an `int` variable is fixed.** It previously failed
  SPIR-V validation. The result truncates, matching the explicit cast.
- **Range loops are fixed.**
  - Start, end and step are captured once.
  - Runtime negative steps count down.
  - A zero step runs no iterations.
  - Iteration stops before the integer type wraps.
- **Function parameters are passed by value.** Assigning to a parameter
  previously modified the caller's variable or array. Struct and array copies
  are now independent.
- **Compound assignment evaluates its target once**, including any index
  expression. `a[f()] += 1` previously called `f()` twice.

### Backends
- **`texture3D` is now a real 3D texture on every backend.** It was previously
  compiled as a 2D texture and dropped the `.z` coordinate. Sample, load,
  bias, grad, lod and offset variants are supported.
- **Explicit samplers keep their identity.** Previously a sampler shared by
  two textures was duplicated, and two distinct samplers on one texture were
  collapsed into one.
  - Texture/sampler pairs used one-to-one still use combined descriptors.
  - Reusing a texture with different samplers, or sharing a sampler between
    textures, uses separate image and sampler descriptors.
  - Reflection exposes `abi: "separate_image"` and `abi: "sampler"`, with
    samplers in descriptor set 2.
  - Reflection also reports `defaultSamplerFor` and `combinedSamplerUniforms`
    for GLSL/GLES.
  - Hosts should use these mappings instead of inferring bindings from
    declaration order.
- **More than 32 declared resources is now OK** if the ones actually
  referenced fit. Unused declarations are dropped and slots are compacted.
  This previously failed SPIR-V validation.
- **HLSL `input.num_workgroups` now works.** The HLSL cross-compile used to
  fail. The host must bind a 16-byte constant buffer at `b0, space3` with the
  x, y, z dispatch counts plus one padding word. Reflection lists it under
  `hlslResources`.
- **Metal** targets MSL 2.0 by default and switches to MSL 2.1 when you use
  subgroup operations.
- **Direct GLES output** (`-gles-direct`) was reworked. Local pointers still
  go through SPIRV-Cross.
- **Dynamic indexing of arrays in uniform structs** now reads the uniform
  directly instead of copying the whole block to local storage first.

### Diagnostics
These replace SPIR-V validation failures or silent bad output with errors
located in your source.
- Reading `attributes.X` in a fragment stage.
- A stage output assigned values of different types (`float2` then `float3`),
  reported at the conflicting assignment.
- `bool` in a uniform or storage buffer, including nested in structs and
  arrays. Use `uint` and `flag != 0u`. Metal output for these used to crash
  the compiler.
- Referencing more than 32 resources per pipeline, or exceeding the Metal
  binding limits (buffers 0–30, samplers 0–15). The Metal limits were
  previously not checked.
- A requested target that can't be generated now makes the CLI exit nonzero.
  Previously a failed HLSL cross-compile was only a warning. `-all` skips
  unavailable GLSL/GLES targets with a warning.

### Standard library
- **`Color` has new conversions** between RGB and XYZ, Luv, LCh, HSLuv, HPLuv
  and HSV.
- **New `Line2D` module** with intersection, point-to-line distance and
  perpendicularity helpers.

### Tooling
- **Larger shaders compile.** Deeply inlined shaders such as PBR/IBL no longer
  run into the IR register limit (raised from 4096 to 8192) or a fixed 512 KiB
  scratch memory.
- **Faster compiles.** Symbol lookup, module discovery and parser string
  lookups are faster.
- **Builds:** MSYS2 CLANG64 is supported on Windows, and release builds stamp
  the compiler version automatically.

### AST JSON
The schema is now `bwsl.ast.v3`. See `docs/ast-json.md` for the full format.

**Changed (breaking for consumers)**
- Parameters, bindings and fields now use `dataType` (fields: `typeInfo`), so
  `type` always means the node kind.
- Module- and pipeline-level constants are now `VARIABLE_DECL` nodes under
  `consts`. A folded constant use keeps a `foldedFrom` identifier with a `read`
  edge.

**Added**
- A semantic reference sidecar.
- `sourceFile` on modules, pipelines and their declarations. Members merged
  from a submodule keep their own file. Standard-library modules also get a
  GitHub `sourceUrl` pinned to the compiler's release tag.
- `nameLine`/`nameColumn` on every named node.
- Positions and ids on struct fields, pass fragment outputs, `use attributes`
  entries, imports and usings.
- `using X` entries get ids and `using` edges (`writtenName` for aliases).
- `Module::Type` in variable, parameter and field declarations gets a
  `typeQualifier` occurrence with a `qualifier` edge to the module.
- Functions and methods emit `returnTypeLine`/`returnTypeColumn`.
- `-> Module::Type` keeps its qualification in `returnType`, records
  `returnTypeQualifier`, and gets both a return-type edge and a qualifier edge
  to the module. Previously such functions had no edge at all.
- Intrinsic call result types are inferred from the stdlib table, so
  `output.x = normalize(uv)` gives the interface symbol a type.
