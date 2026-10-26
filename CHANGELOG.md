# Changelog

## unreleased

### Breaking changes
- An enum member without a written value is the previous member's value
  plus one, or 0 for the first: `enum E : uint { A = 3, B, C = 7, D }` gives
  B = 4 and D = 8. Members were numbered by position, ignoring the values
  written before them, and as powers of two once an enum with an underlying
  type wrote any value, so `B` was 2 and members could share a value
  silently (`enum E { A = 3, B, C = 7, D }` gave D = 3, like A). Two members
  with the same value, or a member without one after the largest value of
  its type, are now errors. So are a written value outside the enum's type
  (a negative value without `: int`, or `2147483648` for `: int`) and an
  underlying type other than `int` or `uint`, such as `uint64` or `int2`,
  which were accepted and treated as 32-bit.
- Assignments, initializers, returns and component-wise operators no longer
  change a vector's shape implicitly. `int2 s = size3;` needs `size3.xy`,
  `int3 v = size2;` needs `int3(size2, z)`, `float3 f = i3;` needs
  `float3(i3)`, `float2 h = 0.5;` needs `float2(0.5)`, and `int2 + int3` is
  rejected. Each reports a source error naming the fix. Before, narrowing
  compiled on SPIR-V-based targets but broke `-gles-direct` and `-wgsl`,
  widening silently repeated components (`.xyx`) or read out of bounds, and
  the other cases failed SPIR-V validation.
- `texture_size` returns `int3` for a `texture3D` (width, height, depth) and
  a `texture2DArray` (width, height, layers), as GLSL `textureSize`, HLSL
  `GetDimensions` and Metal do. It was `int2` for every texture, dropping the
  depth and the layer count. `texture2D` and `textureCube` still return
  `int2`. `int2 s = texture_size(volume);` is now a source error; write
  `texture_size(volume).xy`. Overloads on the result pick their `int3`
  version.

### Language
- `@branch if (...)` asks for a real branch, which a wave skips when none of
  its lanes take it, and `@flatten if (...)` for both sides evaluated. They
  set the SPIR-V selection control (`[branch]`/`[flatten]` in HLSL). For
  drivers that ignore the hint, AMD's among them, `enforce-branch-hints`
  also shapes the code so the branch stays.
- `@bounds(...)` promises the range of an attribute, a scalar or vector
  resource, or a struct field: `@bounds(>0, <10) int count;`,
  `@bounds(>=0, <=1) float4 tint;`, or per component, as in
  `@bounds(>0, -10<y<0) float4 offsets;`. A bounded component is never
  NaN. See "Value Bounds" in the language reference.
- Functions and struct methods can take pointer parameters, `Type^ name`,
  to update the caller's variables: `swap :: (int^ a, int^ b)` called as
  `swap(^x, ^y)`. Writes through the parameter reach the caller, and two
  parameters given the same address alias. The argument must point to a
  local variable, a field of one or an array element; pointers to
  resources, stage inputs and outputs or shared memory are rejected. `T^`
  and `T` overload separately, and a constrained `Number^` parameter is
  specialized by its pointee. Functions can't return pointers. See
  "Pointer parameters" in the language reference.
- `Module::helper(...);` is accepted as a statement. It previously failed to
  parse ("Expected variable name"), so a void module function couldn't be
  called.
- A pointer to an element of a struct's array field (`^s.values[i]`) or to
  a `self` field named without `self.` now points at the element. It
  previously pointed at a copy, so writes through it were lost.
- `@centroid` on a vertex output assignment evaluates the varying inside the
  primitive under MSAA, so edge pixels don't extrapolate it past its vertex
  values: `@centroid output.atlasUV = uv;`. It combines with
  `@noperspective` but not `@flat`, and is emitted for SPIR-V, GLSL, GLSL ES,
  HLSL, Metal and WGSL.
- Vector `eval` values work in compile-time arithmetic: `+ - * /` and `%`
  between vectors of the same width or with a scalar on either side, bit
  operations on `int` and `uint` vectors, unary minus, swizzle reads such as
  `tint.y` or `tint.zyx`, and swizzle stores such as `tint.xy = ...`.
  Constructors built from these, like `float3(tint.x, 1.0, tint.z)`, fold
  too. `uint` scalar arithmetic in eval code now works as well.
- Module constant initializers can use vector arithmetic and swizzles, as in
  `const float2 SOUTH = NORTH * -1.0;`.
- A pipeline-scope `const` can call a pipeline `eval` function, as in
  `const float AREA = circleArea(2.0);`. The call is evaluated once and
  every pass sees the folded value. An eval call whose arguments aren't
  compile-time values is reported at the declaration.
- Float `%` with a negative operand now gives the same result at compile
  time as at runtime. `eval` code and `const` initializers truncated, so
  `-7.5 % 2.0` was `-1.5` there but `0.5` in runtime code; both now give
  `0.5`, the sign of the divisor, like `mod()`. `fmod()` still truncates.
- `BwslFrexpResult` and `BwslModfResult`, the structs `frexp` and `modf`
  return, can be named as types again: locals, arrays, parameters, return
  types, struct fields and constructors. Since v0.10 they were rejected with
  "Unknown type". A user struct can't take either name, and reading a field
  they don't have is a source error.

### Backends
- Experimental native WGSL output is available with `-wgsl` for compute,
  vertex and fragment shaders, including structured control flow, std140
  uniforms and sampled textures. `-bindings` emits WebGPU binding metadata.
  WGSL-only compilation bypasses SPIR-V; request `-spv` or another target
  alongside `-wgsl` to generate both. `-all` keeps its existing targets.
  Texture arrays (`texture2D[N]` and the other sampled kinds) bind each
  element as its own WGSL texture, reported per element in the bindings
  metadata with `arrayLength` and `arrayIndex`.
- Constant values are propagated through the whole shader, including across
  branches, loops and inlined functions. Arithmetic, comparisons and
  conversions on constants are folded, `x && true`, `x || false` and their
  integer counterparts become `x`, an `if` whose condition is constant
  keeps only the arm it takes, and values that nothing reads are removed.
  Vectors and matrices fold too: construction, swizzles, `dot`, `cross`,
  `transpose` and matrix products. For example, `gain` set to `2.0` on both
  sides of an `if` becomes the constant `2.0`, and `float3(2.0) * m[0]` with
  a constant `m` becomes a constant vector.
  Intrinsics with constant arguments fold as well, from `clamp` and `max` to
  `pow`, `sin`, `smoothstep`, `normalize`, `reflect`, `determinant` and the
  bit operations. Math functions are computed in double precision and
  rounded once, so they're at least as accurate as the GPU's.
  A loop whose inputs are all constants and that has no side effects runs
  at compile time, so a function like `poly(2.0)` that loops over its
  argument becomes a constant. A loop that runs at most once is no longer
  emitted as a loop: one whose condition is false on entry disappears, and
  one whose body always breaks becomes the code of its single pass.
  A vector or matrix with some constant components keeps them: reading one,
  through a swizzle, component access, `select` or componentwise
  arithmetic, gives the constant. `Color::hsluvToRgb(float3(180.0, x,
  50.0))` keeps only the arithmetic on `x`, and its SPIR-V shrinks from
  4440 to 1920 bytes.
  An index into a vector or matrix that is known at compile time works
  like a literal one: with `int i = 2;`, `v[i]` reads `v.z` instead of
  indexing at run time, `v[i] = s` writes `v.z`, `m[i]` takes the column
  directly instead of selecting among all of them, and a component that is
  a constant folds.
  Operations that a constant or a repeated operand decides simplify too:
  `x * 1.0`, `x + 0.0` and `x / 1.0` become `x`, `0.0 - x` and
  `x * -1.0` become `-x`, `x / 4.0` becomes `x * 0.25`, and
  `pow(x, 0.5)` becomes `sqrt(x)`, along with the integer identities,
  `x ^ x`, `min(x, x)` and double negation. `pow` with an integer exponent
  up to 16, such as Schlick's `pow(1.0 - VdotH, 5.0)`, becomes
  multiplications, which are faster and more accurate than the GPU's
  `pow`. `x * 0.0` and `x - x` stay, since they aren't 0 for an infinite
  or NaN `x`.
  A calculation repeated where an earlier one with the same operands is
  available, such as `sin(x * 2.0)` twice, or `x * 3.0` before a branch
  and again in its arms, is computed once.
  Every value's range is tracked, from `@bounds`, constants, operations and
  the branches it's in, and it removes what can't change the value:
  `clamp`, `min`, `max`, `saturate`, `step` and `abs` of values already in
  range, and branches the range decides. With `@bounds(>0, <10) int n`,
  `clamp(n, 1, 9)` becomes `n` and `if (n >= 10)` disappears, and in
  `for (int i = 0; i < n; i++)`, `saturate(float(i) / 16.0)` becomes
  `float(i) * 0.0625`. Calls on a value that may be NaN stay.
  Built-in inputs count too: in a `[8, 1, 1]` compute stage,
  `input.local_id.y` is 0 and `min(input.local_id.x, 7u)` is
  `input.local_id.x`, and a fragment shader's `input.position.xy` is never
  NaN. A `@flat`, `@centroid` or integer varying keeps its range in the
  fragment stage, so a `saturate` the vertex stage already applied to a
  `@centroid` varying isn't repeated.
  Ranges carry through vector conversions such as `int2(q)`, and a struct
  field read back after a store, as in `i.color = c; ... i.color.z`, is the
  value stored, so its constant and range carry too.
  A signed `%` or `/` whose operands the ranges show aren't negative no
  longer pays for truncating negative values: `x % 8` becomes `x & 7`,
  `x / 8` becomes `x >> 3`, and other remainders, such as
  `int(input.position.x) % spacing`, become an unsigned `%` instead of
  `x - spacing * (x / spacing)`. GLSL, HLSL and MSL get
  `int(uint(x) % uint(spacing))`, and direct GLSL ES and WGSL a plain `%`.
  With `--optimizations split-structs` (not on by default yet), local
  struct variables become one variable per field, so a struct no longer
  goes through memory: `Inputs i; i.color = c; ... i.color.z`
  reads `c.z`, a field assigned in a branch or a loop is merged like any
  variable, and the copies, zero-initializations and field stores the
  output used to carry disappear. The struct is built only where it's used
  whole, such as stored to a buffer or an array. Structs reached through a
  pointer (`^s`) or with an array field stay as they were. Across the test
  corpus, SPIR-V is 5% smaller.
  With `--optimizations loop-unrolling` (not on by default yet), a `for`
  loop that runs a constant number of times, up to 32, becomes one copy of
  its body per iteration, as long as the copies stay under 1024
  instructions. Each copy sees its iterator as a constant, so
  `for (int i = 0; i < 4; i++) { sum += w[i] * float(i); }` reads `w[0]`
  to `w[3]` and multiplies by constants. The bound can be a literal, a
  variable set to a constant just before the loop, or a constant argument
  of an inlined function, and an inner loop's bound can be the iterator of
  an outer loop unrolled around it. A loop with `break`, `skip` or a
  `return`, one whose body changes the iterator or the bound, or one that
  declares an array or a `switch` stays a loop.
  Folding skips results that can differ between GPUs:
  NaN, infinities, denormals, `-0.0`, integer division of negative values,
  conversions that are out of range or inexact, and math functions outside
  their domain, such as `sqrt(-1.0)` or `pow(-2.0, 0.5)`.
- Range loops with a constant step test only the direction they go, and
  drop the wraparound test when their bounds rule it out, unless the body
  assigns the iterator. `for (x in -1..=1)` now comes out as
  `for (int x = -1; x <= 1; x++)`.
- A shader with more than 256 distinct float constants no longer corrupts
  its integer constants (`+ 7` could become `+ 1132478464`). More than
  16384 distinct float constants is a source error.
- A stage with more than 12288 registers handles separate samplers
  correctly. A `sampler` resource shared its internal number with a
  register, so `sample(tex, smp, uv)` could change that register's type
  (WGSL failed with "SSA copy changes type without an explicit
  conversion"), and `sample_lod(tex, uv, lod)`, `gather_offset` or
  `load_offset` without a sampler could take a coordinate computed late in
  the stage for one, failing validation or sampling with the wrong
  operands.
- `first_bit_high` and `first_bit_low` return the same value on every
  backend: the index of the highest or lowest set bit, or -1 when the value
  is 0. WGSL and direct GLES returned leading and trailing zero counts, so
  `first_bit_high(8u)` was 28 there and 3 elsewhere. On an `int`,
  `first_bit_high` finds the highest bit that differs from the sign bit, as
  GLSL's `findMSB` and HLSL's `firstbithigh` do, so `first_bit_high(-1)` is
  -1 rather than 31. Both fold when their argument is constant.
- Direct GLES output of `count_bits`, `first_bit_high` and `first_bit_low`
  compiles as GLSL ES 3.00 for WebGL2, which has no `bitCount`, `findMSB` or
  `findLSB`; the shader gets small helper functions for the ones it uses.
  `count_bits` on a `uint` no longer emits `uint r = bitCount(x)`, which
  failed to compile.
- Direct GLES output of `reverse_bits`, `bitfield_extract` and
  `bitfield_insert` compiles as GLSL ES 3.00 too. It called the ES 3.10
  builtins `bitfieldReverse`, `bitfieldExtract` and `bitfieldInsert`.
- `bitfield_extract` on a `uint2`, `uint3` or `uint4` zero-extends each
  field, as it does on a `uint`. SPIR-V and the targets compiled from it
  sign-extended them, so a field with its top bit set came out with every
  higher bit set.
- GLSL ES output no longer drops storage buffer, storage image, shared
  memory, atomic and wave operations. A shader using `ldexp` or `modf` with
  a storage buffer was emitted as GLSL ES that read undeclared variables;
  like any other shader using these features, it now fails `-gles` and
  `-gles-direct` with the feature GLSL ES 3.00 lacks, and `-all` omits the
  GLSL ES target with a warning.
- A shader stage can use 32764 registers, up from 16383, so larger shaders
  compile instead of failing with "Shader register limit exceeded" or "SSA
  register limit exceeded". Output for shaders that compiled before is
  unchanged.

### Diagnostics
- A duplicate pass name is reported at the second pass's name, with the line
  of the first, instead of at the end of the second pass.
- Passing a value where a pointer parameter is expected, a pointer where a
  value is expected (including to an intrinsic or constructor), or a pointer
  to a different type reports the argument and how to fix it.
- `sample`, `sample_offset` and `sample_cmp` in a compute stage are a source
  error that recommends `sample_lod` or `sample_lod_offset`; they previously
  failed SPIR-V validation. `sample_offset` in a vertex stage is reported the
  same way. `sample_bias` and the derivative intrinsics (`ddx`, `ddy`,
  `fwidth` and their fine and coarse forms) outside fragment stages get a
  specific message instead of "Intrinsic not available in this shader stage".
  Helper functions are now checked against the stage that calls them, so a
  helper using `sample()` or `ddx()` called from a vertex or compute stage is
  a source error rather than a SPIR-V validation failure.
- Ternary arms of different types, such as `c ? float3(1.0) : float2(1.0)`,
  are a source error. They previously failed SPIR-V validation.
- `select(false_value, true_value, condition)` reports a source error when
  the condition isn't a `bool` (for example when it's passed first), when the
  two values differ in type, or when a `bool` vector condition is narrower or
  wider than the values. Values that differ, as with the condition passed
  first, and a condition of the wrong width previously failed SPIR-V
  validation; a numeric condition with matching values compiled as
  `condition != 0` (see Breaking changes).
- A `switch` on a `float` or runtime `bool` selector, or with a `float` case
  label, is a source error at the selector or label. The selector previously
  failed SPIR-V validation, and a `float` label was reported without a
  location. A `bool` variant selector still works, because the switch is
  reduced to one arm at compile time.

### Tooling
- Large shaders no longer crash the compiler. The compilation arena was a
  fixed 16 MiB block, and a 3000-line material shader importing two modules
  needs about 23 MiB; every variant of it crashed with a segmentation fault.
  The arena now grows in 16 MiB blocks without moving earlier allocations,
  up to 1 GiB per compilation by default. `-arena-limit <MiB>` changes the
  limit. Reaching it is error BWSL1402 for that unit, and the other units of
  a batch still compile. The CFG/SSA, SPIR-V and direct GLSL ES scratch
  arenas grow the same way.
- The comptime pass may clone up to 1,000,000 AST nodes, up from 100,000.
  The same shader clones about 94,000.

### Tooling
- `--optimizations <list>` chooses which optimizations run, in bwslc and in
  the WASM module's flags. The list holds at most one group (`off`,
  `default`, `amd`, `nvidia`, `intel`) and optimization names, each enabled
  as written or disabled with `no-`:
  `--optimizations off,common-subexpressions` runs only common
  subexpression elimination, `--optimizations amd,no-dead-values` runs the
  AMD set but keeps unread values. Names win over the group. Without the
  option, or with no group in the list, `default` applies: every
  optimization not known to slow down any vendor's GPU. The vendor groups
  start out equal to `default`. `phi-variables`, in no group yet, keeps a
  variable carried through loops and branches as one variable in Metal,
  HLSL and GLSL output, instead of copying it on every edge.
  `bwslc -h` lists the names.
- `-debug-info` emits source-level debug info for GPU debuggers such as
  RenderDoc and Nsight Graphics, which can then step through the `.bwsl`
  source. It implies `-debug-names`. The SPIR-V carries:
  - the source text (`OpSource`, and a `DebugSource` for each file code
    comes from, modules included);
  - `OpLine` and `DebugLine` where the line changes;
  - a `DebugFunction` with a `DebugInlinedAt` scope for each inlined
    function;
  - local variables (`DebugLocalVariable`, `DebugValue`).

  It uses NonSemantic.Shader.DebugInfo.100, which needs
  `VK_KHR_shader_non_semantic_info` before Vulkan 1.3. The GLSL, HLSL and
  Metal output gets `#line` directives, in GLSL's standard numbered form.

### AST JSON
- Enum variants have the value the compiler uses, also for members without
  a written one, signed for an `: int` enum, and `valueWritten` says whether
  it was written. A member without a value reported `4294967295`.
- Attribute declarations, resource declarations and struct fields written with
  `@bounds(...)` have a `bounds` object: the clause's position and the range
  it promises per component, resolved as the compiler uses it.
- A pointer parameter has `isPointer: true` and a `dataType` spelled as
  written, such as `int^`; its `typeInfo` describes the pointee. Function
  stable IDs mark pointer parameters with `^`, as in `swap(int^,int^)`.
- `frexp` and `modf` calls and variables of their result types are typed as
  `builtin:type:BwslFrexpResult` / `builtin:type:BwslModfResult`, and their
  field reads have `member` edges.
- A call that passes a struct resource or an enum variant directly, such as
  `read(resources.data)` or `weight(variants.quality)`, now has its `call`
  edge in `referenceIndex.references`.

## v0.11.0

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
- Module constants can be vectors, and their initializers can use scalar
  arithmetic and constructors, e.g. `const float2 NORTH = float2(0.0, -1.0);`.
  Previously only a single literal was allowed. Vector arithmetic, swizzles
  and matrices are not supported in these initializers yet.
- Imported resource payloads use `Module::Type`; `Module.Type` remains a
  deprecated compatibility alias.
- Struct fields can use a previously declared type in the same module.
- Texture resources can be arrays, e.g. `units: texture2D[4]`, indexed with a
  constant or a dynamic `int`/`uint` index (`resources.units[input.unit]`).
  GL output declares `sampler2D t_units[4]` and indexes it with constants
  only, as GLSL ES 3.00 requires. Each element takes a resource slot, and
  `bindings.json` reports the array's `count`. Previously such a declaration
  failed SPIR-V validation with `Id is 0`.
- Vertex attributes beyond the supported 16 slots produce a source error.
- `.length` works on local arrays and array function parameters. It was
  previously rejected as an invalid swizzle.
- `.length` on a multi-dimensional array is the outer dimension, and on a
  partly indexed one the next dimension: for `float[2][3] grid`,
  `grid.length` is 2 and `grid[i].length` is 3. Previously `grid.length` was
  the flattened element count, 6, and `grid[i].length` was rejected as an
  invalid swizzle. Multi-dimensional function parameters, e.g.
  `(float[2][3] g)`, can also be indexed as `g[i][j]`; previously such reads
  were undefined.
- `min` and `max` with more than two arguments reduce over all of them, up
  to 16. Previously only the first two were used and the rest were
  silently dropped. More than 16 arguments is a source error.
- `min` and `max` with both float and integer (or bool) arguments produce a
  source error. Previously they failed SPIR-V validation.
- Functions with `texture2D`, `texture3D`, `textureCube`, `texture2DArray`
  or `sampler` parameters can be called with resources, e.g.
  `shade(resources.albedo, uv)`. Such calls previously failed with
  `Function not found`.
- Fixed-size arrays can be declared `const` at module, pipeline, pass or
  local scope, e.g. `const float[4] WEIGHTS = {0.125, 0.25, 0.375, 0.25};`.
  Brace initializers must supply every element, and elements cannot be
  assigned. Module array elements must be compile-time expressions.
- Any swizzle works on an expression result, e.g.
  `float2(0.25, 0.75).xyxy`. Previously only `.x`–`.w`, `.xy` and `.xyz`
  did; others kept the original type and failed SPIR-V validation.
- Swizzle components past the end of a vector, such as `.z` on a `float2`,
  produce a source error. Previously reads failed SPIR-V validation or
  repeated earlier components, and stores like `v.xz = ...` dropped the
  out-of-range write.
- A module's local or built-in types are no longer mistaken for a qualified
  type from another imported module that sits at the same line and column
  in its own file.

### Backends
- Struct locals are updated in place. Writing a field or an array element of
  a struct no longer copies the whole struct at every branch merge and loop
  iteration, which made shaders that mutate structs in loops (for example
  through a mutating method) several times slower than equivalent GLSL.
- Local arrays with constant elements, such as `float[4] t = {1, 2, 3, 4}`
  or `float2[4] t = {float2(1.0, -3.0) / 16.0, ...}`, get one array
  initializer instead of one store per element, with constant arithmetic
  and conversions folded. The element stores made NVIDIA drivers rebuild
  the array in local memory on every invocation.
- GLSL and GLES output declare such arrays where they were, as
  `float t[N] = float[](...)`, instead of module-scope `const` tables, which
  NVIDIA drivers also copy to local memory on every invocation.
- `a || b` and `a && b` whose right operand is a short, side-effect-free
  expression (comparisons, arithmetic, locals, swizzles) compile to a
  logical `or`/`and` instead of a branch. Calls, array indexing, pointer
  reads, integer division and shifts still short-circuit, so guards such as
  `i < n && a[i] > 0.0` stay safe.
- `c ? a : b` whose arms are short and side-effect-free compiles to a select
  instead of an `if`/`else`. Math intrinsics such as `pow`, constructors and
  division by a literal now count as side-effect-free here and in `&&`/`||`
  operands. Calls to your functions, array indexing, derivatives and texture
  reads keep the branch.
- `c ? a : b` with `bool2`/`bool3`/`bool4` arms compiles. It failed SPIR-V
  validation when the condition was a bool vector.
- An `if` branch ending in `return` inside a called function makes the rest
  of the function its other branch. The function no longer tracks a
  "returned" flag and branches on it a second time, which drivers don't
  merge.
- A loop that calls a function no longer carries the function's locals and
  return flag from one iteration to the next. They became copies in the
  loop increment of GLSL, HLSL and Metal output, which drivers don't
  always remove.
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
  handles duplicate names. With `-glsl` or `-gles`, a name reserved in GLSL
  gets a `_` suffix (`vec4` becomes `vec4_`). Default output is unchanged.
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
- Passes using more than 8 vertex attributes compile for vertex-pulling
  targets (SPIR-V, Metal, HLSL, GLSL and `-check`), up to the documented
  limit of 16. The 9th and later attributes got no storage buffer, so
  SPIR-V validation failed.

### Diagnostics
- Shadowed local, parameter, and loop-variable declarations produce a note
  with both source locations. Compilation still succeeds; errors and
  warnings take priority over notes in the CLI's limited text display.
- WASM results include non-blocking notes in a `diagnostics` array.
- Reading `input.<name>` in a fragment stage when the vertex stage never
  writes `output.<name>` is a source error. It previously compiled with an
  invented `varying0` input that could not link in GL.
- A call that fails with `Function not found` inside a vector constructor
  no longer also reports `vector constructor expects N components`.
- Naming a variable, parameter, field or `foreach` iterator after a keyword
  such as `inputs` or `pass` reports `'inputs' is a keyword and cannot be
  used as a variable name`. Parsing continues, so later errors in the file
  are still reported.

### Standard library
- `Color::hsluvToRgb`, `rgbToHsluv`, `hpluvToRgb` and `rgbToHpluv` return
  correct colors. They over-saturated because of the `min` bug above.
- The HSLuv and HPLuv conversions in `Color` no longer return early for
  black and white, which made shaders calling `Color::hsluvToRgb`
  measurably faster. `hsluvToLch` and `hpluvToLch` clamp lightness to
  [0, 100] instead of special-casing the ends, so near black and white they
  return the lightness as given with a tiny chroma rather than exactly 0;
  the RGB result is unchanged.
- `Math::fast_atan2(y, x)` (with `float2`/`float3`/`float4` overloads) is a
  branch-free `atan2` within 1.2e-5 rad. GPUs have no `atan` instruction, so
  the exact version is a software routine with branches for IEEE edge
  cases. `fast_atan2(0, 0)` is 0; infinities and the sign of zero aren't
  handled.

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
- `self.field` inside a struct method has a `member` edge (or `write` when
  assigned) and `self.method()` a `call` edge, even when a local or
  parameter shadows the field. Each `self` has a `type` edge to its struct.
- Type qualifiers in a module are no longer attached to nodes of another
  module file that share their line and column.

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
