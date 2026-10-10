# Changelog

## unreleased

### Breaking changes
- Unknown resource payload types now produce source errors, including unused
  resources and `buffer`/`cbuffer` element types.
- AST JSON now preserves array parameter dimensions and local element types.
  Declaration/reference `typeInfo` includes `elementType`, `arraySizes`, rank,
  and total length; field vector types no longer appear as arrays. Function
  stable IDs include array parameter dimensions.
- Unresolved names now fail checking in unused functions and standalone
  modules, with diagnostics pointing to the original source token.
- Function and method names matching a built-in intrinsic are rejected at
  their declaration, including unused and module functions. Rename such
  functions; the bundled SDF method `distance` is now `signed_distance`.
- **GL and GLES output use source names for interface objects.** Hosts that
  bind by name must update their lookups. Read `glName` from
  `bindings.json` instead of hard-coding names:
  - uniform block `ub_<resource>`, instance `bwsl_ub_<resource>`
    (was `_12_14`, or `UB_<resource>` with `-gles-direct`)
  - combined sampler `t_<texture>` (was `_18`, or `u_<texture>`)
  - vertex input `a_<attribute>` (was `_6`, or `attr0`)
  - varying `v_<name>` (was `varying0` in the CLI)

  Metal and HLSL output now carry the same names.
- **Struct and sampler names that clash with an interface name are
  rejected.** These names are emitted as written, so a struct named
  `a_position` would clash with attribute `position`, and a sampler named
  `t_atlas` with texture `atlas`. Varying names must also differ within
  their first 31 characters.

### Language
- Imported resource payloads use `Module::Type`; `Module.Type` remains a
  deprecated compatibility alias.
- Struct fields can use a previously declared type in the same module.
- Vertex attributes beyond the supported 16 slots produce a source error.
- `.length` works on local arrays and array function parameters. It was
  previously rejected as an invalid swizzle.
- `min` and `max` with more than two arguments reduce over all of them, up
  to 16. Previously only the first two were used and the rest were
  silently dropped. More than 16 arguments is a source error.
- `min` and `max` with both float and integer (or bool) arguments produce a
  source error. Previously they failed SPIR-V validation.

### Backends
- Struct locals are updated in place. Writing a field or an array element of
  a struct no longer copies the whole struct at every branch merge and loop
  iteration, which made shaders that mutate structs in loops (for example
  through a mutating method) several times slower than equivalent GLSL.
- `-gles-direct` output for struct locals compiles: the zero value a struct
  starts from was declared as `void`. Boolean `&&`/`||` are no longer emitted
  as `&`/`|`, and a fragment stage's `input.position` is `gl_FragCoord`.
- A mutating struct method no longer wipes the receiver's other fields, and
  writing a field of a struct parameter no longer resets the parameter's
  other fields. The first field write zero-initialized the whole struct.
- With `-debug-names`, calling a function with an early `return` more than
  once in a stage no longer gives GLSL two globals with the same name
  (`'squared' : redefinition`).
- Struct type and member names are preserved without `-debug-names`, so nested
  uniforms link across GL shader stages, including mixed GLES emitters.
- `-debug-names` preserves source local-variable and array names in SPIR-V
  and cross-compiled shaders, including reassignment and loop/branch phis.
  Aliased or inlined values may share or lose a visible name; SPIRV-Cross
  handles duplicate and target-reserved names. Default output is unchanged.
- Large shaders keep live registers separate from constant and texture tags.
  Integer constant pools grow safely up to 8192 values per signedness, and
  register or integer-constant exhaustion reports a source location.
- WebAssembly compilation routes GLES 3.00 gathers through the direct
  fallback before SPIRV-Cross can abort.
- Direct GLES output preserves array resource lengths and supports constant
  and dynamic reads. Arrays of structs use the correct uniform-buffer stride.
- The compiler service shares varying types and locations between stages.
- A pass whose fragment stage falls back to the direct GLES emitter (for
  example with `ddx_fine`) now links. Both stages use the same varying
  names.

### Diagnostics
- Shadowed local, parameter, and loop-variable declarations produce a note
  with both source locations. Compilation still succeeds; errors and
  warnings take priority over notes in the CLI's limited text display.
- WASM results include non-blocking notes in a `diagnostics` array.
- Reading `input.<name>` in a fragment stage when the vertex stage never
  writes `output.<name>` is a source error. It previously compiled with an
  invented `varying0` input that could not link in GL.

### Standard library
- `Color::hsluvToRgb`, `rgbToHsluv`, `hpluvToRgb` and `rgbToHpluv` return
  correct colors. They over-saturated because of the `min` bug above.

### Tooling
- `make wasm` and `make wasm-debug` link with `em++`, so they build with
  current Emscripten (6.x). `em++` must be on `PATH` instead of
  `emcc`.
- `bindings.json` reports `glName` for uniform buffers and combined
  textures: the name to pass to `glGetUniformBlockIndex` or
  `glGetUniformLocation`.
- The WASM build runs the same GLSL ES compatibility checks as the CLI.
- The compiler service no longer reads an uninitialized value when
  specializing a variant, which could crash it.

### AST JSON
- Resource declarations include type positions and references to payload types
  and module qualifiers. Their `typeName` uses canonical module qualification,
  including when the source uses an import alias or the legacy dot spelling.

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
