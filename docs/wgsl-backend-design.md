# Native WGSL backend design

Status: native compute, vertex and fragment implementation with structured
control flow, std140 uniforms and sampled textures; architecture
for [issue #113](https://github.com/BWSL-Lang/BWSL/issues/113).
Investigated on 2026-10-15 against the current working tree, based on commit
`7f8a2fe`, including the existing uncommitted compiler changes. The CLI now has
an experimental direct WGSL target for all three stages. Later sections describe the intended
full backend; the implemented boundary is listed below.

## Implemented backend

`bwslc shader.bwsl -wgsl` lowers and normalizes BWSL IR to SSA, including
straight-line variable reassignment, then emits WGSL without generating SPIR-V.
`-wgsl -spv` requests both targets. `-all` retains its established targets;
WGSL is opt-in while its capability coverage is limited. `-wgsl -check` runs
target preparation without writing artifacts. External Naga/Tint validation is
provided by the development tests, not the production compiler's SPIR-V
`-validation` option.

The implemented slice supports shaders with `bool`, `i32`,
`u32`, `f32`, their vectors, arithmetic and supported math builtins, conversions,
constructors, swizzles, compute input builtins, vertex attributes and builtins,
stage varyings, fragment coordinates/depth, multiple color outputs, fragment
derivatives, conditional discard, primitive storage buffers, structured branches/
loops/switches, scalar/vector phis, std140 uniforms (including nested structures,
fixed arrays and matrix reads/products), and sampled 2D/3D/cube/array textures.
Resource access and load evaluation order are preserved.

Aggregates and memory follow. Struct types become `BWSLStruct<N>` with fields
`f0`, `f1`, ...; enums with payloads are their lowered struct (an `i32` tag, then
the flattened payload). A register SSA leaves in memory (address taken,
assigned more than once, or a local array) is one WGSL `var`; reads of it stay
in place and copies of it are written out. Local pointers are access paths
written where they are loaded or stored; a pointer reassigned to another target
is a `u32` tag variable dispatched with `switch`, keeping the target's last
dynamic index from the assignment. Struct storage buffers keep the BWSL buffer
layout through `@size` on each member; layouts WGSL cannot express (bool and
`mat2` members, mismatched array strides) are diagnosed. `shared` variables are
`var<workgroup>`; a variable or buffer targeted by an atomic holds
`atomic<i32|u32>` elements and its other accesses become `atomicLoad` and
`atomicStore`. `atomic_cmp_exchange` retries `atomicCompareExchangeWeak` to stay
strong. Memory fences, which WGSL only has as part of control barriers, emit
`storageBarrier()` and `workgroupBarrier()`. Wave operations use the `subgroups`
extension and are listed under `requiredFeatures` in the bindings metadata.
`tests/wgsl_equivalence_tests.py` runs the compute equivalence suite through the
backend on the GPU and compares it with the SPIR-V path.

[`bwsl_wgsl_backend.h`](../src/phases/backends/wgsl/bwsl_wgsl_backend.h) defines
dense value and resource tables as parallel arena arrays. Original registers and
logical resource slots live in separate origin tables. The backend performs a
validation/preparation pass, a text sizing pass, then emits into an exact arena
allocation. It does not use per-instruction heap objects or expression strings.
The CLI copies the final source into its existing owning output result after
emission. Its `-timing` report includes WGSL preparation/emission, source bytes
and arena use.

Logical storage slot N maps to group 0 binding N. Uniforms use group 1, sampled
textures group 2, and samplers group 3. Explicit samplers retain their logical
slot; an implicit sampler for texture N uses binding 32 + N.
Only referenced buffers are emitted. `-bindings` writes
`<shader>.<stage>.wgsl.bindings.json` from the actual backend tables. Schema
version 3 includes physical uniform layouts and sampled resource descriptors,
stage inputs/outputs, locations, builtins, interpolation,
entry point, compute workgroup size (null for graphics), access, original slot and physical array
stride (`vec3` has stride 16). Host bindings must use this WGSL metadata; the
existing SPIR-V binding layout is different. General resource remapping and
cross-stage layout planning remain part of stage 3.

Repeatable checks and measurements:

```sh
make test-wgsl-unit-sanitize  # Arena bounds, dense IDs, invalid IR, storage layout
make test-wgsl               # CLI tests, Naga validation and GPU if available
make test-wgsl-full          # Require Naga, Tint, sanitizer tests and actual GPU
make benchmark-wgsl         # Matched source/compiler/GPU comparisons
make test-wgsl-render       # Graphics linkage, diagnostics, both validators, harness tests
make benchmark-wgsl-render  # Real browser render pipelines and full compilation
make benchmark-wgsl-inprocess # Fresh-source native compilation without CLI/I/O
```

Naga 27.0.3 and wgpu 27.0.1 are pinned in a standalone Rust development tool.
Tint is built from pinned Dawn revision
`af01c4ef653550c1e9bf6c04065e7b4f054ea1e8` into ignored `build/`. These dependencies
are not linked into BWSL. See the [oracle instructions](../tools/wgsl-oracle/README.md)
and [benchmark protocol](../tools/wgsl-bench/README.md). The initial benchmark
corpus contains matched SAXPY, integer hash and vector affine compute kernels;
it establishes an expandable baseline rather than representative production
coverage or a general performance-parity claim.

The WGSL GitHub workflow runs sanitizer tests, both independent validators and
records CPU benchmark samples on relevant pushes and pull requests. It uploads
the reports without enforcing a noisy hosted-runner speed threshold. Hardware
GPU correctness and timing remain explicit local commands; the workflow does
not claim GPU coverage when it runs without an adapter.

The signed-vector arithmetic fixture has a narrowly documented external-tool
limitation: Naga 27.0.3 can generate a signed-division SPIR-V helper that its own
SPIR-V reader rejects on the return trip with a vector `LogicalAnd` type error.
The test retains direct generated/handwritten WGSL validation and real GPU
readback, and records only that exact roundtrip failure as a known limitation.

### Initial measured baseline

Measured locally on 2026-10-15 with Apple M1 Max / Metal and the release compiler
(`-O3`, arm64, Apple M1 target). Compiler SHA-256:
`83f5afbca4f2b241216005722f1f15538acb2378e2f6ee21daebcfaa33682d30`.
The run used 30 process samples, 100 in-process Naga/Tint samples, 30 GPU samples,
five warmups and 65,536 invocations per kernel. All 12 shader variants passed
both validators and exact independent CPU-reference readback.

Median times in microseconds:

| Kernel | WGSL prepare/emit | BWSL source to WGSL reported phases | Handwritten GPU | Native WGSL GPU |
| --- | ---: | ---: | ---: | ---: |
| SAXPY | 10 | 158.5 | 10.917 | 11.084 |
| Unsigned hash | 11 | 157.0 | 10.292 | 10.209 |
| Vector affine | 12 | 163.0 | 17.230 | 17.167 |

The emission column includes preparation, sizing, output allocation and the CLI
source copy. It excludes BWSL parsing/lowering and downstream WGSL compilation.
GPU medians differ by at most about 1.6% on this corpus, but the native hash run's
p95 was 154 microseconds: outliers and dispatch overhead prevent a general
parity or speedup claim. Larger kernels and repeated controlled runs are needed.

| WGSL source to SPIR-V, in process | Handwritten WGSL range | Native emitted WGSL range |
| --- | ---: | ---: |
| Naga 27.0.3 | 27.6–32.0 µs | 35.6–49.9 µs |
| Tint pinned revision | 92.3–96.0 µs | 110.6–127.2 µs |

These measurements support the earlier observation that Naga is faster than
Tint for this small corpus. Native emission avoids their SPIR-V-to-WGSL work
(about 20 microseconds for Naga, 210–218 for Tint here), but the whole BWSL
source-to-WGSL-to-SPIR-V path has not reached handwritten WGSL compilation
parity. Its measured CLI path through Naga took 9.8–12.2 milliseconds versus
4.7–5.7 for handwritten WGSL, including the additional process and file I/O.
Generated WGSL also costs more to parse than the handwritten equivalents;
redundant copy elimination and a cheaper straight-line preparation path are
concrete future tuning targets.

Local raw samples, executable/source hashes, generated shaders and GPU readbacks
are retained in
[`build/wgsl-benchmarks/stage1-baseline-final/`](../build/wgsl-benchmarks/stage1-baseline-final/).
The final build also passed 683 existing regressions, 122 WGSL checks and the
ASan/UBSan backend unit suite, with the single external Naga limitation above.

### Compilation optimization pass

The follow-up profile found three avoidable costs. CFG/SSA preparation cleared a
512 KiB scratch arena even for tiny shaders. Lowering initialized all 16,383
register metadata slots regardless of the number of live registers, and the
frontend sized its initial position maps for a module hierarchy. Generated WGSL
also retained register copies, builtin copies, redundant type annotations and
temporary expressions, which added downstream parser and validation work.

The compiler now allocates uninitialized CFG scratch only when needed, initializes
lowering metadata when each register is allocated, and sizes CLI AST maps from
source length while retaining their existing growth path and arena. SSA allocates
and initializes its own new register metadata range, including spare capacity.
`-timing` exposes frontend and IR setup as nested measurements for future work.

The WGSL value SoA now includes canonical alias operands, use counts and bounded
expression depths. Copies resolve to their original values or typed constants.
Pure expressions used once can be emitted inline, with depth capped at four;
storage loads and mutable vector operations remain materialized in their original
order. Use counting includes repeated arguments in expansions such as floor-based
`mod`, preventing duplicated computation. Type inference is used only after the
backend has verified the explicit result and operand types. Short local names,
component swizzles and exact shorter float literals further reduce source size.

The expanded workload investigation also tracks the consumer instruction in an
arena-backed parallel value array. A single-use vector floating-point sum feeding
another sum more than 16 IR instructions later remains materialized at its
original position. This preserves partial accumulations across substantial
intervening work instead of extending the lifetimes of several completed
contributions. It changes neither arithmetic order nor storage evaluation order.

A paired run measured the preserved stage-1 executable against the optimized
compiler in the same session. All measurements below are median microseconds,
except source bytes. Settings match the initial baseline: 30 process samples,
100 Naga/Tint samples, 30 GPU samples, five warmups and 65,536 invocations.

| Kernel | BWSL source to WGSL phases, before → after | Naga emitted WGSL to SPIR-V, before → after | Tint emitted WGSL to SPIR-V, before → after | WGSL bytes, before → after |
| --- | ---: | ---: | ---: | ---: |
| SAXPY | 163.0 → 101.5 | 37.4 → 30.0 | 109.5 → 95.1 | 755 → 502 |
| Unsigned hash | 161.0 → 107.0 | 47.1 → 35.7 | 125.3 → 99.0 | 985 → 498 |
| Vector affine | 172.0 → 112.5 | 48.8 → 35.0 | 125.3 → 92.0 | 1007 → 503 |

Reported BWSL compilation time fell by 34–38%. CFG/SSA fell from about 43
microseconds to 5–6; lowering fell from 41–46 to 28.5–33. The expression plan
adds roughly 0.5–1 microsecond to emission itself, but saves 7–14 microseconds
in Naga and 14–33 in Tint downstream. Naga took 27.4–32.6 microseconds for the
handwritten WGSL in this paired run; Tint took 90.7–93.2.

Whole compilation still includes the BWSL frontend followed by WGSL parsing and
validation. In the CLI benchmark, BWSL-to-WGSL-to-SPIR-V also launches two tools,
where the handwritten path launches one. Process startup and file I/O dominate
those millisecond-scale measurements, so the phase improvements do not imply an
equivalent reduction in complete CLI latency. Naga process paths were 9.2–11.4
milliseconds for BWSL versus 4.8–5.9 for handwritten WGSL; no full-compilation
parity claim is made.

All 15 shader variants in the paired benchmark passed both validators and exact
CPU-reference GPU readback on M1 Max Metal. Optimized versus handwritten GPU
medians were 11.000/11.063, 10.209/10.250 and 17.417/17.166 microseconds. These
small differences are within the observed timing noise. Correctness checks
passed 683 regressions with both release and ASan/UBSan builds, 134 WGSL checks,
and the backend unit suite. New regressions cover a load consumed after an
intervening store, bounded expression depth, and poisoned SSA metadata allocation.
The one narrowly identified Naga roundtrip limitation remains unchanged.

The benchmark now accepts `--baseline-compiler` to interleave old and new
executables and validate both outputs. Raw results are retained in
[`build/wgsl-benchmarks/opt-final/`](../build/wgsl-benchmarks/opt-final/).
Optimized compiler SHA-256:
`478664d68dd6707cce3ce2f26ec9d2651907d4e583adba4a715c36180b3ecbe1`.

### Expanded compute workloads and browser compilation (historical milestone)

This section records the earlier compute-only capability boundary and measurements.
The later extensions below supersede its then-remaining implementation work.

The browser corpus now includes a 25-tap RGBA Gaussian filter, four-bone linear
blend skinning with a 64-bone palette, and four-light Cook–Torrance GGX shading
with tone mapping. The original three microkernels remain. Both sources use the
same input data, bindings and workgroups. All input buffers and exact outputs
are checked before and after GPU timing; GGX uses an independent double-precision
reference with 0.0002 absolute + 0.0002 relative tolerance and records the actual
maximum error. The two variants' outputs are also checked against each other.

These measurements used **compute shaders**, including the skinning and lighting
workloads, before the graphics implementation described below. Fixed work is explicitly unrolled in BWSL because this slice
rejects control flow. The usual handwritten WGSL uses loops/helpers. Separate
unrolled handwritten controls distinguish that source-structure effect from an
emitter advantage; `--unrolled-reference` selects them.

Two final browser runs on 2026-10-15 used Chrome 155.0.8059.40 and an Apple
`metal-3` hardware adapter, 1,048,576 invocations, 30 measured GPU pairs, five GPU
warmup pairs and ten measured compilation pairs. The latest run's medians:

| Workload | Handwritten GPU (µs) | BWSL WGSL GPU (µs) | Handwritten full source request → ready (ms) | BWSL full source request → ready (ms) |
| --- | ---: | ---: | ---: | ---: |
| Gaussian 5×5 RGBA | 349.18 | 272.38 | 2.067 | 9.360 |
| Four-bone skinning | 236.54 | 237.57 | 1.730 | 7.627 |
| Four-light GGX | 175.10 | 180.74 | 1.865 | 9.820 |

GPU durations are timestamped warm-buffer batches normalized per dispatch.
The Gaussian paired ratios were 0.785 and 0.790 in the final repeats, with both
intervals below 1. Explicitly unrolling its handwritten WGSL removed that
advantage: the earlier control measured a ratio of 1.004, interval 0.971–1.019.
That points to unrolling, not a general BWSL code-generation advantage. Skinning
and the microkernels had no consistent gap outside their recorded intervals.
Final GGX ratios were 1.006 and 1.032: a small residual difference remains
variable; there is no general parity claim.

The first GGX implementation had repeatedly been 4.7–7.1% slower in paired GPU
ratios. Its expression plan deferred partial color sums across subsequent light
calculations. A broad inlining-distance limit hurt skinning in the experiment
and was discarded. The retained rule only materializes distant vector additions
feeding another addition. A direct old-versus-new compiler comparison on the
same browser/device/buffers measured GPU ratios of 0.938 and 0.944, with both
intervals below 1 (5.6–6.2% lower paired GPU time). The other five emitted shaders
are byte-for-byte unchanged. This supports the narrower change here; the exact
vendor register allocation has not been inspected.

Full source-to-ready timing includes localhost request/JSON overhead for both
paths, and a fresh native CLI launch, BWSL frontend, lowering, emission and file
I/O on the BWSL path. Browser module and asynchronous pipeline readiness are
also recorded separately. Repeated identical source can hit browser/driver
caches; neither first-observed nor repeated timing is guaranteed cold. This is
not an in-process or WebAssembly BWSL measurement. The browser path uses WGSL
directly and never emits or loads SPIR-V.

The native diagnostic run used 15 process samples, 150 in-process Naga/Tint
samples, five warmups and five GPU samples at 65,536 invocations. All 30 variants
(handwritten, current native WGSL, previous native WGSL and both existing
SPIR-V-to-WGSL diagnostic paths) passed both validators and GPU reference checks.
Native median microseconds, with handwritten → current emitted WGSL:

| Workload | Naga parse + validate | Tint parse + validate | BWSL reported compile phases | BWSL CLI wall time |
| --- | ---: | ---: | ---: | ---: |
| Gaussian 5×5 | 52.71 → 287.54 | 61.25 → 296.17 | 378 | 7,199 |
| Skinning | 51.71 → 96.96 | 69.12 → 119.73 | 197 | 7,245 |
| GGX | 132.92 → 356.35 | 168.88 → 398.85 | 490 | 8,975 |

The complete CLI path is dominated by work outside the measured compiler
phases, including process startup and file handling. Within the reported phases,
lexing/parsing is largest. Separately, expanded WGSL costs more downstream:
Gaussian grew from 864 handwritten bytes to 4,123 emitted bytes, skinning from
1,152 to 1,749, and GGX from 2,596 to 5,486. Naga/Tint independently confirm more
front-end work for those expanded modules. Their standalone measurements are
diagnostics, not browser GPU or browser cold-compilation times.

The useful next compilation targets are an in-process/WASM integration and
structured control-flow emission to retain compact loops, alongside reducing
unnecessary generated expressions. The subsequent graphics implementation adds
arena-backed stage input/output tables and real render-pipeline readback tests.
Textured production shaders still require texture/sampler and uniform
layout support.

Validation passed 683 existing regressions, 134 WGSL checks with the previously
documented external Naga skip, ASan/UBSan backend units and 14 browser benchmark
expectation/protocol/analysis tests. Reports and exact source/compiler/harness
identities are retained locally in:

* [`build/wgsl-web/complex-final/`](../build/wgsl-web/complex-final/) — final browser repeats.
* [`build/wgsl-web/complex-paired-v1/`](../build/wgsl-web/complex-paired-v1/) — direct old/new GGX comparison.
* [`build/wgsl-web/complex-unrolled-v1/`](../build/wgsl-web/complex-unrolled-v1/) — unrolling controls.
* [`build/wgsl-benchmarks/complex-final/`](../build/wgsl-benchmarks/complex-final/) — Naga/Tint, CLI and native GPU diagnostics.

Compiler SHA-256 for the preceding compute measurements:
`11786485cff98496dc31b4b990f1720c7fe7b2e535f1ecba1ee5a4e951ef22d8`.
See the [benchmark protocol](../tools/wgsl-bench/README.md) for repeatable commands
and the report analyzer, which never pools differing shader/compiler/device
identities.

### Vertex and fragment implementation (initial graphics milestone)

This section records the initial graphics boundary and measurements. The next
section adds resources, structured control flow and in-process compilation.

The graphics path consumes the same optimized SSA IR as compute. Inputs and
outputs each use dense, arena-backed parallel arrays for types, locations,
builtins and interpolation. Logical slots and diagnostic instruction origins
live in separate cold tables. Small slot-to-interface maps deduplicate repeated
loads/stores; there are no per-interface heap objects. Text emission still uses
a sizing pass followed by one exact arena allocation.

Vertex attributes retain their declared locations. Varying location is the
logical varying slot minus `VARYING0`, so an unused varying cannot shift another
stage's locations. Float varyings default to perspective interpolation;
`@noperspective` maps to WGSL `linear, center`. `@centroid` maps to
`perspective, centroid`, or `linear, centroid` with `@noperspective`, and
reflection reports it as `"sampling": "centroid"`. Integer varyings become `flat` on
both sides, as required by [WGSL](https://www.w3.org/TR/WGSL/#interpolation).
Integer linear interpolation is diagnosed. Vertex/instance indices and fragment
coordinates use their WGSL builtins. Vertex position must be `float4` and written.
Fragment colors retain sparse locations; depth maps to `frag_depth`.

Output stores update a local output structure in IR order. Output loads capture
the value at that point, including across subsequent assignments. The output
structure is zero-initialized, so an output read before any assignment reads
zero. Fragment derivatives
include ordinary, fine and coarse `ddx`, `ddy` and `fwidth`; unconditional discard
is supported. Conditional discard still requires structured control flow.
Vertex storage writes are rejected; read-only storage is available to both
graphics stages. Texture/sampler operations, uniforms, matrices, aggregate stage
IO, control flow and the previously listed advanced features remain unsupported.

Reflection schema 2 exposes the prepared interfaces directly. A fragment-only
`-stage fragment` compilation first lowers the vertex interface to preserve
varying types/interpolation, without emitting an unrequested vertex artifact.
The benchmark checks reflected stage linkage and resource agreement, and uses
one explicit layout for both handwritten and BWSL variants.

The render corpus now exercises actual graphics stages:

| Pipeline | Workload |
| --- | --- |
| Skinned GGX | 32,768 triangles across four instances; four-bone skinning from a 64-bone palette, interpolated world positions/normals, four-light GGX and depth testing |
| Procedural MRT | Fullscreen analytic material, normal/roughness attachment, derivative antialiasing and linear interpolation with varying clip W |
| Particles | 16,384 instanced billboards, storage-buffer vertex reads, integer/flat varyings, soft alpha and ordered alpha blending |

A separate interface regression tests sparse attributes/varyings/MRT, integer
flat interpolation, nonzero first vertex/instance offsets, fragment coordinates,
late fragment depth, and output snapshots. It has no performance row.

All attachments are compared between variants before and after timing. Independent
CPU references check full procedural/particle/interface images; skinning/GGX uses
software-transformed, rasterized interior probes and background probes, avoiding
triangle-edge rasterization ambiguity. RGBA8 permits three byte levels of error;
observed maxima were zero for mesh/interface and one for procedural/particles.
Integer MRT is exact. Every input buffer is checked byte-for-byte before and
after timing. Full attachment hashes and reduced previews are retained.

Four final browser runs on 2026-10-15 used Chrome 155.0.8059.40 and the Apple
`metal-3` hardware adapter: two runs each at 512×512 and 1024×1024, 30 GPU pairs,
five warmup pairs and ten repeated compilation pairs. All passed. The latest
1024×1024 run reported:

| Pipeline | Handwritten GPU (µs/frame) | BWSL GPU (µs/frame) | Handwritten full source → ready (ms) | BWSL full source → ready (ms) |
| --- | ---: | ---: | ---: | ---: |
| Skinned GGX | 100.99 | 100.86 | 2.000 | 12.713 |
| Procedural MRT | 69.50 | 70.27 | 1.668 | 9.577 |
| Particles | 92.29 | 92.67 | 1.855 | 9.600 |

The analyzer found no consistent GPU direction outside the recorded paired
intervals at either resolution. Each frame includes target clears, a complete
render pass and stores; timestamped batches are normalized per frame. These are
warm-resource, single-device measurements, not a general parity guarantee.

Full source-to-ready includes localhost request overhead for both paths and a
fresh native CLI compilation of both stages, reflection and file I/O for BWSL.
Repeated browser/driver caches are not cleared. Latest BWSL server medians were
10.092/7.353/7.075 ms, while BWSL browser readiness was 0.405/0.278/0.383 ms versus
0.245/0.198/0.218 ms handwritten. Reported BWSL compiler phases were
0.789/0.264/0.169 ms, including emission at 0.090/0.043/0.028 ms; lexing/parsing
was their largest component. CLI total self-reports were 2.894/0.893/0.630 ms.
Most full-path latency therefore lies outside the compiler phases and browser
readiness. An in-process/WASM integration remains the main latency target;
these timings do not isolate process startup from the remaining host/file work.

Both tools independently validated both stages of both variants. With 100
in-process parse/validate samples after five warmups, sums of the stage medians
were (µs; these are diagnostic sums, not measured pipeline medians):

| Pipeline | Naga handwritten → BWSL | Tint handwritten → BWSL | Source bytes handwritten → BWSL, both stages |
| --- | ---: | ---: | ---: |
| Skinned GGX | 197.17 → 513.54 | 256.43 → 600.85 | 3,910 → 8,155 |
| Procedural MRT | 133.84 → 204.71 | 160.84 → 207.29 | 1,829 → 2,811 |
| Particles | 58.83 → 86.85 | 82.32 → 110.90 | 1,194 → 1,741 |

Expanded straight-line WGSL costs more to parse/validate, especially unrolled
skinning/lighting. Compact structured loops/helpers remain a concrete next
compiler target. No SPIR-V is generated or loaded by the browser render path;
Naga/Tint measurements here stop after WGSL validation.

Final validation passed 683 existing regressions, 134 compute checks (one known
external Naga roundtrip skip), 31 graphics CLI checks, ASan/UBSan backend units,
and 24 browser harness/protocol/analysis tests. Hosted CI runs the graphics CLI
and independent validators; actual browser GPU measurements remain local.

Compiler SHA-256:
`f56cf1610a02de85117dc8c5585e3f474d24f87fc0870101420a755cf3a6bbd4`.
Raw samples, source snapshots, tool/compiler/harness identities, previews and
the resolution-separated analysis are retained in
[`build/wgsl-render-benchmarks/render-final/`](../build/wgsl-render-benchmarks/render-final/).
Reproduce with `make test-wgsl-render` and `make benchmark-wgsl-render`; see the
[render benchmark protocol](../tools/wgsl-bench/README.md#browser-render-pipelines).

### In-process compilation, uniforms, textures and structured control flow

The subsequent implementation adds a fresh-source in-process benchmark driver
which calls the production frontend and `CompileShaderStage`. Each sample creates
and destroys fresh AST/IR/CFG/backend arenas, resolves variants and comptime code,
normalizes SSA, emits WGSL and captures owning source/reflection results. Sources
are read before measurement; process startup and input/output files are excluded.
It reuses no parsed AST or optimized IR. Global process caches and repeated
source remain warm. This is a development driver, not a published embedding ABI.

`benchmark_wgsl_inprocess.py` verifies every driver's source and reflection against
its CLI, interleaves baseline/candidate blocks, retains all observations and
separately measures fresh CLI wall time. Naga/Tint parse/validate observations use
the actual matched WGSL. The browser can use persistent, preloaded workers with
`benchmark_wgsl_render.py --inprocess build/wgsl-inprocess`. Workers compile fresh
source state on every request; their native timing excludes pipe/JSON handling,
which remains in server and full source-to-ready timing. CLI and in-process
browser modes have distinct identities and are never pooled by the analyzer.

The main measured bottleneck was AST tooling lookup, not backend allocation.
Node keys pack their kind in the high byte, but the position maps masked only
the low bits for bucket selection. Different node types with the same local
index collided, creating long probe clusters during parsing and comptime AST
cloning. Mixing the full key fixes start/end/name/declaration maps while retaining
their arena-backed parallel key/value arrays and unchanged logical node IDs.
Growth/update tests cover overlapping indices across node kinds, and AST JSON
position/reference regressions remain green.

The final native comparison used ten alternating blocks of twenty observations,
five warmups per block, ten fresh CLI observations and 100 Naga/Tint observations
after five warmups. Median full source-to-owning-WGSL times (µs):

| Workload | Previous in-process | Current in-process | Reduction | Current CLI wall |
| --- | ---: | ---: | ---: | ---: |
| Gaussian 5×5 compute | 973.98 | 140.90 | 85.5% | 4,347 |
| Four-light PBR compute | 1,309.67 | 233.50 | 82.2% | 4,797 |
| Procedural MRT | 316.23 | 96.60 | 69.5% | 4,740 |
| Skinned GGX graphics | 2,079.29 | 352.31 | 83.1% | 5,173 |
| Textured material, uniforms and loop | unsupported | 66.19 | — | 4,636 |

All nine previously supported corpus shaders emit byte-identical WGSL to the
preserved compiler. The speedup therefore comes from compiler work, without a
change to those GPU programs. Source snapshots, exact binaries/tool hashes,
per-phase and raw observations are in
[`build/wgsl-inprocess-benchmarks/final-v3/`](../build/wgsl-inprocess-benchmarks/final-v3/).
CLI wall time is still dominated by work outside the in-process compilation
window; subtracting the measurements does not isolate startup from all host/file
work. Expanded unrolled WGSL still costs more downstream than handwritten loops;
these improvements do not establish full compilation parity with WGSL source.

The final browser comparison used Chromium 155 on the Apple Metal adapter,
four in-process runs (two at 512² and two at 1024²) and two fresh-CLI runs at
1024². Every run passed images, resource readbacks and the three untimed interface,
static-loop and texture-layer regressions. Latest 1024² observations, in ms:

| Pipeline | Handwritten full, worker run | BWSL full, worker run | BWSL full, fresh CLI run |
| --- | ---: | ---: | ---: |
| Skinned GGX | 1.703 | 2.273 | 10.208 |
| Procedural MRT | 1.598 | 1.748 | 8.175 |
| Particles | 1.613 | 1.795 | 8.268 |
| Textured uniforms and loop | 1.540 | 1.710 | 7.693 |

These are separate repeated-source observations with warm browser/driver caches,
not an isolated subtraction experiment or cold compilation measurements. The
fresh-CLI run's handwritten medians were 1.895, 1.468, 1.470 and 1.338 ms,
respectively. Native compilation is only one component of the worker request;
HTTP, pipe/JSON handling and browser pipeline readiness remain included above.

A separate batch-mode comparison compiled the same four render pipelines in
one sequential CLI process versus four fresh processes. Ten alternating pairs
after two warmups measured 8.617 ms batched versus 22.468 ms separately; every
WGSL/reflection artifact matched the validated corpus. The amortized figures are
2.154 and 5.617 ms per pipeline. Batch shares module-source discovery and retains
source/output files, making it a useful production CLI throughput measurement.
It does not measure each pipeline's latency or browser readiness; dividing total
batch time cannot replace the worker's source-request-to-ready measurement.
[Reproducible batch driver](../scripts/benchmark_wgsl_batch.py) and
[raw observations](../build/wgsl-next/batch-final-v2/results.json) retain this
separate endpoint explicitly.

GPU intervals for the mesh, MRT and particles showed no consistent direction.
The textured material's paired BWSL/handwritten ratios were 0.937–0.961 across
the four 1024² runs, with every recorded interval below one; the two 512² runs
did not establish a direction. This is an observation for this shader/device,
not a general speedup claim. Paired ratios are medians of individual ratios,
not quotients of the separate duration medians.

The remaining native downstream cost is visible in the independent validators.
For skinned GGX, the sum of vertex/fragment parse-and-validate medians is
200.88 µs handwritten versus 519.08 µs emitted in Naga, and 251.00 versus
597.94 µs in Tint. The textured material is 86.31 versus 143.96 µs in Naga,
and 105.65 versus 196.00 µs in Tint. These diagnostic sums are not measured
end-to-end pipeline medians. The larger generated programs, especially the old
unrolled workloads, remain a useful next optimization target.

Final correctness coverage passed 683 existing regressions, 36 AST JSON tests,
134 compute checks, 37 graphics checks, eight control-flow GPU cases, six resource
cases (including independent non-diagonal mat2/mat3 GPU checks), thirteen
sanitized backend units and 27 harness tests. One previously documented Naga
signed-vector SPIR-V roundtrip remains skipped. The separate native static-loop
discrepancy below remains unresolved and is not counted as a passing native GPU
case; the browser checks it against the independent CPU result.

Final local artifacts:

- [Native timings and Naga/Tint samples](../build/wgsl-inprocess-benchmarks/final-v3/results.json)
- [In-process browser report](../build/wgsl-render-benchmarks/inprocess-final-v3/results-20261015T130433.454056Z.json)
- [Fresh-CLI browser report](../build/wgsl-render-benchmarks/cli-final-v3/results-20261015T131140.970794Z.json)
- [All six browser runs, grouped by mode/resolution and exact identities](../build/wgsl-next/browser-summary-final.json)
- [Final artifact verification](../build/wgsl-next/verification-final.json)

The compiler SHA-256 is
`f5828229880422d5b6d660ca5055f4bb43a613569614195ba0a021bf909f2535`;
the driver is
`f1f8d9d99cf3519beb5963c720ba1c50866360136d76d432c94be09d98732e63`.
Raw reports and logs are local build artifacts, not checked-in benchmark data.

Structured emission follows existing IR merge/continue metadata and CFG blocks.
The arena-backed control table stores per-block region data and CSR phi lists.
Only live phis are kept; unreachable blocks and dead incoming edges are excluded.
Parallel edge copies snapshot every incoming value before any phi destination
is assigned, preserving swaps/cycles. Phi and cross-block values have function
scope; ordinary values stay local and bounded pure expressions can inline within
their block. Storage loads retain their original evaluation points. IR undefined
temporaries select a typed zero, which is a valid undefined-value choice; stage
output reads still require definite assignment across every incoming path.

Loops emit WGSL `loop`/`continuing`, including final conditional continuing exits
as `break if`. BWSL switch arms lower to structured condition chains so a BWSL
loop `break` inside a switch retains its target. There is no program-counter
state machine. CFG dataflow checks output initialization and mandatory vertex
position writes on every return path. GPU cases cover nested loops, parallel phi
swaps, short-circuit side effects, switches, skip/break, counted-until loops and
early returns. Conditional fragment discard is covered by browser image checks.

Uniform physical layout has its own arena-backed SoA plan: the generic IR's
packed storage-array layout cannot supply uniform array strides. Scalar/vector
and nested struct layouts use std140 alignment; arrays and matrix columns use
16-byte strides. Uniform blocks are represented by aligned `vec4<u32>` rows,
with aligned vector reads/bitcasts and swizzles, preserving scalar bit patterns
and bool representation without per-component load expansion. Uniform matrix
reads and matrix/vector products cover mat2, mat3 and mat4. Reflection schema 3
records total byte size and member offsets, array strides and nested type IDs.
Independent GPU readbacks cover scalar arrays, nested structs, mat2/mat3 columns,
and the graphics material's mat4 plus vec3/scalar packing.

Sampled textures support f32 2D, 3D, cube and 2D-array resources, and fixed
arrays of them: element i of an array at slot b is the separate texture
b + i (WGSL has no texture arrays outside `binding_array`), and the IR's
branch per element for a dynamic index is ordinary structured control flow.
Reflection lists each element with `arrayLength` and `arrayIndex`; a
comparison on any element makes every element a depth texture. Textures
support ordinary, explicit-LOD, bias, gradient and comparison sampling;
gather; texel fetch; size and level queries. Explicit and implicit samplers have distinct binding ranges;
ordinary/comparison conflicts are diagnosed. Gather components must be literal
integers in 0–3. Texel offsets must be constants in -8 through 7, as WGSL
requires; `load_offset` adds its offset to the texel coordinate. Multisampled
and integer texture types remain unsupported. WGSL uniformity requirements still apply; the backend does not yet
provide a complete uniformity analysis. Both independent validators and browser
pipeline creation remain part of the correctness workflow.

The new browser material samples a deterministic RGBA texture in a runtime loop,
uses a padded scalar uniform array, nested layout metadata and a mat4 transform,
and conditionally discards pixels. Independent CPU sampling checks every image
byte, and all uniform/texture inputs are read back before/after timing. A separate
constant nested-loop continue regression checks a CPU sum of 105 in Dawn.
A three-layer texture regression checks fractional and out-of-range layer
selection. Sampling rounds the floating layer with `floor(layer + 0.5)` before
WGSL's integer layer selection; integer texel fetches keep their discrete layer.
This matches the permitted nearest-layer behavior in the
[Khronos sampling contract](https://docs.vulkan.org/spec/latest/chapters/textures.html#textures-unnormalized-to-integer).

During investigation, a static nested-loop/continue shader produced unexpected
results in the pinned native Naga/wgpu Metal path, including in independently
written WGSL. The same static loop passes the Dawn browser CPU reference for
both sources; dynamically bounded nested loops pass the native oracle. Generated
MSL was retained for diagnosis, and the oracle now provides `dump-msl` to inspect
this path. This observation does not identify whether Naga's backend or the
platform compiler causes the discrepancy. Current generated compute WGSL
returned zero and handwritten WGSL returned 30 instead of 105. The complete
sources, expected inputs, native readbacks and MSL are retained in
[`build/wgsl-next/native-nested-continue/`](../build/wgsl-next/native-nested-continue/).
It is separate from the existing
signed-vector SPIR-V roundtrip limitation.

## Original full-backend architecture investigation

The following design records the original investigation; the implementation
above covers only its stated subset. Graphics IO was delivered ahead of the
original sequence. The current subset now includes std140 uniforms, sampled
textures and structured control flow; the remaining restrictions are listed above.

Build a native C++ backend that consumes BWSL's lowered IR and structured control
flow, performs WGSL-specific legalization in arena-backed SoA tables, and emits
WGSL source. Keep the AST and symbols available for declaration metadata; do not
translate executable expressions from the AST again. Use Naga and Tint as
independent validators and reference implementations.

The user recalls Rust Naga outperforming Tint in earlier measurements. Issue #113
contains no measurement data or comments, so that result remains useful context
rather than a quantified baseline. This investigation makes no new performance
claim.

## Target format and reference implementations

WGSL's standardized interchange is source text, not a binary or standardized IR.
Naga and Tint each provide their own IR. Building one of those representations
would couple this backend to that compiler rather than remove the translation
step. The [WGSL specification](https://www.w3.org/TR/WGSL/) defines the external
contract.

[Naga's IR](https://docs.rs/naga/latest/naga/ir/index.html) separates expression
arenas from structured statements. Its explicit evaluation points are especially
relevant: loads and texture reads depend on preceding stores, derivatives depend
on control flow, and reused expressions must not accidentally be evaluated twice.
Its [statement representation](https://docs.rs/naga/latest/naga/ir/enum.Statement.html)
also distinguishes loop bodies, continuation, and exit conditions.

[Tint's IR design](https://dawn.googlesource.com/dawn/+/refs/heads/main/docs/tint/ir.md)
uses structured SSA with nested blocks and block parameters. Tint explicitly
keeps that IR internal to its implementation. Its
[WGSL raising pipeline](https://github.com/google/dawn/blob/main/src/tint/lang/wgsl/writer/raise/raise.cc)
provides useful examples of value materialization, naming, and pointer/reference
conversion. The older architecture document linked by the issue is useful
background; the IR document and current writer are the better implementation
references.

Adopt the separation of normalization, legality checks, and emission. Retain
BWSL's representation and allocation model instead of importing either compiler's
object model. A full WGSL parser, optimizer, or second general-purpose AST is not
needed for this backend.

## Existing compiler seam

```text
BWSL AST and symbols
    |
    v
Shared lowering and specialization
    |
    v
IR + CFG + SSA + declaration metadata
    |                              |
    v                              v
SPIR-V backend              WGSL preparation
                                   |
                                   v
                         WGSL legalization tables
                                   |
                                   v
                             WGSL source
```

There is already a direct text backend at the appropriate seam. In
[`CompileShaderStage`](../src/bwslc.cpp), direct GLES runs after CFG/SSA and before
`SPIRVBuilder`. It demonstrates integration and control-flow recovery, but its
heap-backed output buffer and compressed formatting should not become the WGSL
implementation template.

The current CLI does not run a separate general optimizer at this seam. It
receives lowered IR, including work performed during lowering, followed by
conditional CFG/SSA conversion. Runtime variant specialization also happens
before CFG/SSA. WGSL should reuse future common optimization passes at this same
boundary, without requiring a SPIR-V round trip.

| Existing component | Reusable information |
| --- | --- |
| [`IRProgram`](../src/phases/ir_generation/bwsl_ir_gen.h) | Parallel instruction arrays, constant pools, register types, struct fields, array declarations, storage provenance, CSR phi and switch tables |
| [`CFG`](../src/phases/control_flow/bwsl_cfg.h) | Block ranges, instruction-to-block mapping, predecessors, successors, and dominance |
| [`SSA`](../src/phases/ssa/bwsl_ssa.cpp) | Value renaming and phi construction, including repeated definitions within one block |
| [`IRAnalysis`](../src/phases/ir_generation/bwsl_ir_analysis.h) | Used resources, access masks, stage IO, interpolation, and capability requirements |
| [`SPIRVBuilder`](../src/phases/backends/spirv/bwsl_spirv_backend.h) | Register-indexed mappings, section separation, type resolution, pointer lowering, and arena allocation patterns |
| [`ResourceData`](../src/core/bwsl_symbol_table.h) and [`reflection`](../src/core/bwsl_resource_reflection.h) | Resource declarations and identity, sampler uses, stage usage, and host binding information |
| [`PassVaryingContext`](../src/phases/ir_lowering/bwsl_ir_lowering.h) | Shared vertex/fragment varying agreement |

Two preparation requirements matter:

1. Straight-line shaders currently skip CFG/SSA. The WGSL preparation path must
   build the CFG even for branchless input and run SSA when input has not been
   normalized, regardless of the resulting block count.
   `ConvertToSSA` can identify repeated definitions within one block. Test that
   route before making it a shared default for other targets. Perform any
   normalization before exposing an immutable prepared stage to emitters; never
   rerun SSA on already converted input. Track an explicit preparation phase;
   `phiCount == 0` does not imply the input is unnormalized. Address-taken locals
   remain storage.
2. CFG construction does not automatically call `RecoverStructure`. Resolve the
   retained `structureInfo` and `continueInfo` instruction targets through
   `instToBlock`, as the direct GLES backend does. Verify region nesting and
   dominance rather than assuming the CFG's merge arrays are populated.

Preparation should accept the stage, workgroup dimensions, vertex-input mode,
pass interface, symbols, and target profile explicitly. Return immutable IR/CFG
views plus resolved target metadata. The prepared stage's owner keeps its IR
pool and CFG arena alive until all requested backends finish. AST, symbol,
source-text, and interface-plan storage must also outlive their consumers.
Collect/analyze participating stages before freezing the pass interface, keeping
vertex lowering before fragment lowering because it supplies varying metadata.
The pass owner retains the prepared stages through interface finalization and
emission; the current sequential, stage-local allocation lifetime cannot simply
be reused for this deferred step.

## Arena-backed representation

Use compact target tables that supplement the existing IR. Ordinary operations
continue to reference their original instruction indices. Only expanded
operations, edge copies, access paths, and generated declarations need new
records.

Each table has one shared index across its parallel arrays. Use `u32` target IDs
and a named invalid-ID sentinel. Synthetic target values must not consume the
IR's tagged 14-bit register namespace. Do not assume every slot below
`ir.registerCount` is a live value: SSA currently leaves spare allocated slots in
that count. Enumerate actual definitions, phi results, and memory objects.

| Index domain | Hot or frequently traversed arrays | Separate cold arrays |
| --- | --- | --- |
| Value ID | Resolved type ID, producer kind/ID, representation kind, flags | Origin ID, optional debug-name ID |
| Region ID | Kind, parent, entry block, merge block, continuation region, child ranges | Diagnostic origin |
| Edge ID | Source/destination block IDs, copy-range offsets | Diagnostic origin |
| Copy ID | Destination value ID, source value ID, temporary value ID | None |
| Type ID | Kind, element type, dimensions, array count, member ranges | Names and layout records used during preparation |
| Resource ID | Type ID, address space, access, target group/binding, stage mask | Original declaration/slot, name, reflection details |
| Access-path ID | Root object, flattened component range, captured index values | Origin ID |
| Action ID | Operation, source instruction, result ID, operand range | Origin ID |

Allocate optional tables only for shaders that use the associated feature. Use
CSR offsets and flat payload arrays for variable-length data. Keep names and
formatted diagnostics out of instruction traversal. Dense register-to-value and
block-to-region maps bridge existing IDs to the new domains.
Producer kind distinguishes original instruction, phi, and synthetic action
domains, as well as inputs/constants/storage. A synthetic value's producer is
its generated action, not the original instruction used for diagnostic origin.

Illustrative declarations, not an implemented API:

```cpp
struct WGSLValueTable {
    // Each array is indexed by the same ValueId.
    u32* typeIds;
    u32* producerIds;
    u8* producerKinds;
    u8* representations;
    u8* flags;
    u32 count;
    u32 capacity;
};

struct WGSLValueOrigins {
    // Cold data, also indexed by ValueId.
    u32* originIds;
    u32* debugNameIds;
};

struct WGSLEdgeCopies {
    // EdgeId indexes offsets; CopyId indexes the payload arrays.
    u32* offsets;
    u32* destinations;
    u32* sources;
    u32* temporaries;
    u32 edgeCount;
    u32 copyCount;
};
```

Follow [`BWEMemoryArena`](../src/core/bwsl_arena.h): bulk lifetime, explicit
ownership, and aligned allocations. `alignas(64)` on a pointer does not align its
payload; request alignment when allocating the arrays. Pre-count known tables,
check multiplication/addition overflow, and check every allocation result.
Report arena exhaustion through the diagnostic stream, with no partial output.
Do not introduce per-instruction `std::string`, nested vectors, hash maps, or
individual heap allocations.

For text, start with a shared count/write emitter: compute section sizes from the
finished plan, allocate exact arena-backed sections, then emit them. Concatenate
once into the owning result string at the public API boundary. Benchmark the
second traversal before replacing it with checked arena chunks. Neither design
may silently truncate output or leave views alive after the arena is released.

## Preparation and emission passes

1. **Analyze and resolve types.** Run shared analysis, index actual definitions,
   and resolve a full target type for every value. Combine `CoreType` with struct
   identity, array shape, and address-space information. SPIR-V's existing type
   overrides demonstrate why `CoreType` alone is insufficient. Reject ambiguous
   metadata rather than defaulting to `f32`.
2. **Build the pass interface.** Resolve resources and IO once for the pass, then
   give stage emitters immutable views. Record required features and limits.
3. **Build structured regions.** Use retained selection/loop/switch annotations,
   validate edge ownership, and create an iterative traversal order. Avoid
   recursive host-stack traversal on arbitrarily deep shaders.
4. **Legalize operations and storage.** Plan physical buffer types, address paths,
   helper functions, aggregate updates, and target-specific operations. Ordinary
   arithmetic remains a direct IR reference.
5. **Schedule values and phi copies.** Assign scopes, materialize reads at their
   original execution points, and create edge-specific simultaneous copies.
6. **Verify and emit.** Check all reachable operations are handled, all uses have
   valid types/scopes, all bindings agree with reflection, and every transfer has
   an equivalent structured form. Emit deterministic, formatted source.

Generate identifiers from category and target ID, such as `bwsl_v_42`, with one
module-level naming policy for values, types, and helpers. Keep source names in
debug metadata. Do not let a source name collide with a WGSL reserved word or a
generated helper.

Use `let` for immutable computed values and `var` for real mutable storage and
phi destinations. Initially materialize each ordinary computed value at its
definition; only inline inexpensive pure expressions after correctness and size
measurements. Texture/sampler handles remain references to resolved resource
declarations in the baseline profile; handle-valued `let` declarations depend on
the optional `texture_and_sampler_let` feature.
Never duplicate loads, texture accesses, atomics, or calls through string
substitution. Preserve lowered short-circuit and ternary control flow.

Declare a phi destination in a scope containing its incoming edges and uses. On
each selected incoming edge, capture all source values before assigning any
destination. A loop-carried swap must produce the equivalent of:

```wgsl
let next_a = b;
let next_b = a;
a = next_a;
b = next_b;
```

Copies belong to edges, not unconditionally to predecessor blocks. Handle
critical edges, loop entry/back edges, `skip`, and break exits explicitly. Do not
turn undefined IR values into an assumed BWSL zero-initialization guarantee.

Model loops with explicit body, continuing, and exit regions. A `skip` must still
execute the intended continuation exactly once. Preserve the meaning of a break
inside nested switches and loops, using local control flags where required.
BWSL currently lowers cases without fallthrough; preserve that behavior and the
actual switch/loop exit target. Diagnose a region that cannot be represented;
do not default to a program-counter loop/switch that changes reconvergence and
complicates uniformity.

## Resource ABI and memory

WGSL resources use group/binding pairs with separate texture and sampler objects.
The target's interface must also satisfy device layout validation.
([Resource interface](https://www.w3.org/TR/WGSL/#resource-interface).)

The current reflection builder can merge a texture/sampler pair into one
SPIR-V-style binding. It also includes backend-specific register namespace and
vertex-pulling adjustments. Extract the common declaration/usage information,
then build a WGSL binding plan; do not reuse the final merged binding list.

Proposed default: assign dense bindings in group 0 from a deterministic ordering
of declared resource identities, including synthesized default samplers. Reserve
that mapping before stage/variant liveness filtering, so unused resources leave
holes instead of changing another stage's bindings. Allocate vertex-pulling
resources through the same plan. Allow an explicit caller mapping, validating
collisions and device limits. Preserve the original resource identity in
reflection alongside the target group/binding. The WGSL host must use that
target mapping, not assume Vulkan or HLSL numbers transfer unchanged.

Normalize BWSL write-only storage-buffer usage to WGSL `read_write`, and validate
access per stage before freezing the plan. Vertex storage-buffer access requires
`read`; the baseline should diagnose incompatible vertex-read/fragment-write use
of one shared binding rather than merge it into a read-write declaration. Add a
pipeline-validation fixture for this case.
([Address spaces](https://www.w3.org/TR/WGSL/#address-spaces).)

Keep logical value types separate from physical uniform/storage types. Existing
IR records std140 field offsets, while SPIR-V contains additional layout logic.
Build one layout plan per logical type and storage role, carrying field offsets,
array strides, matrix column strides, and total size. Use padding/wrappers and
load/store conversions to preserve the existing buffer byte contract. Nested
structs, scalar/vec2 uniform arrays, mat2, and vec3 arrays are mandatory tests.
Larger alignment alone cannot fix an incompatible internal matrix stride.
([WGSL layout constraints](https://www.w3.org/TR/WGSL/#address-space-layout-constraints).)

Existing BWSL resource checks already reject bool buffer data; retain that policy.
Do not add a new implicit bool packing ABI as part of this backend.

Pointers need a distinct address-path representation. For a statically known
target, emit direct accesses or a legal pointer binding. For a rebound local
pointer, retain a target tag and captured dynamic indices, then dispatch the
load/store to the selected path. Reuse the semantics of SPIR-V's pointer handling,
but store candidates in flat arena tables. Do not read or write every candidate:
unselected paths can contain invalid indices or observable accesses. Preserve
aliasing between ordinary access and pointer access to the same local object.

## Compatibility decisions

Start with an explicit portable feature profile. Pin validator revisions and
record optional requirements in reflection. Current WGSL includes optional layout
and pointer features; their presence in the specification does not establish
support on every deployment. ([Language extensions](https://www.w3.org/TR/WGSL/#language-extensions).)

| BWSL area | Required backend treatment |
| --- | --- |
| Scalars, vectors, matrices | Concrete signed/unsigned/float types, explicit conversions, correct constructors and result widths; reject unsupported wide types |
| `OP_FMOD` versus `OP_FREM` | Preserve the existing floor-mod/remainder distinction; a blanket `%` mapping is incorrect |
| Matrix inverse and float classification | Dedicated legalization and numerical tests; do not promise full IEEE exceptional-value equivalence |
| Aggregate insertion and selection | Copy/update or branch-based lowering with correct value semantics and evaluation order |
| Vertex/fragment IO | Resolve builtins, locations, flattened aggregate fields, and interpolation from one shared pass plan; handle integer varyings explicitly |
| Compute | Carry all three workgroup dimensions and builtins; validate stage legality, shared storage, and target limits |
| Textures | Preserve shape, sample kind, explicit/default sampler identity, result shape, storage format/access, and constant-offset legality |
| Local pointer rebinding | Target tags plus captured access paths; reject unsupported cases with a diagnostic |
| Subgroups and optional features | Require an explicit enabled profile and matching validator/device support |

The resource schema needs particular attention: `ResourceData` has shape flags,
but no complete depth/comparison-sampler/storage-format description. The current
storage-image emitter hardcodes 2D `rgba32f`. Sampling comparison also has BWSL
result-shape behavior to preserve. Add explicit metadata where required, or
reject an ambiguous combination; do not guess from an opcode at print time.
Sampling/gather offsets need constant expressions satisfying the target's range
rules. Fold provably constant IR operands into those expressions and diagnose
unsupported dynamic offsets. Include boundary and dynamic-offset fixtures.
([Texture sampling](https://www.w3.org/TR/WGSL/#texturesample).)

WGSL imposes uniformity requirements on derivatives, implicit-LOD sampling, and
barriers. No reusable uniformity analysis was found in the current BWSL pipeline.
([Uniformity](https://www.w3.org/TR/WGSL/#uniformity).) Preserve control flow, add
conservative target analysis, and retain external validation during development.
Do not silence uniformity diagnostics to make generated shaders pass. A full
production support claim requires covering this validation boundary.

### Synchronization requires a language decision

WGSL atomics use relaxed ordering, and its compare-exchange primitive is weak.
Its synchronization builtins are workgroup execution barriers.
([Memory semantics](https://www.w3.org/TR/WGSL/#memory-semantics),
[atomic functions](https://www.w3.org/TR/WGSL/#atomic-builtin-functions).)

The current [`SPIR-V operations`](../src/phases/backends/spirv/bwsl_spirv_backend_ops.inl)
emit acquire/release atomics, strong compare-exchange, and a memory-only barrier
for `OP_MEM_FENCE`. These differences cannot be fixed by changing function names.

Before general atomic support, specify whether BWSL promises the stronger
ordering currently emitted. Until that is resolved, diagnose affected atomic and
memory-fence operations instead of silently weakening their behavior. A strong
compare-exchange helper can retry weak failures until exchange succeeds or the
observed value differs from the comparison value; that still does not restore
acquire/release ordering. Never insert execution barriers around arbitrary
atomics as a substitute.

When atomic support is enabled under an explicit contract, infer atomic physical
storage consistently. Existing shared-array tests mix ordinary and atomic access;
all accesses to an atomic representation must use the matching load/store form.
Test counter use separately from cross-invocation publication/ordering.

WGSL's floating-point rules also permit behavior beyond strict IEEE guarantees.
Bit-based classification helpers alone do not restore full NaN/Inf behavior.
Keep such tests distinguished from portable finite-input equivalence.
([Floating-point evaluation](https://www.w3.org/TR/WGSL/#floating-point-evaluation).)

## Implementation boundaries

Follow the SPIR-V file organization under a new `src/phases/backends/wgsl/`
directory: a builder header and unity implementation, with focused fragments for
preparation, types/layout, resources/IO, control flow, operands/operations, and
final output. Split only where the implementation is large enough to benefit.
Use explicit types, readable multiline control flow, and one statement per line.
Small direct-index opcode tables should remain separate from complex lowering.

The integration work will cover:

- CLI options/help, a WGSL result field and separate timing, stage artifact names,
  reflection, and unity/build dependencies. Proposed output names are
  `name.vert.wgsl`, `name.frag.wgsl`, and `name.comp.wgsl`.
- A WGSL-only route that does not generate or validate SPIR-V. Multi-output
  compilation can reuse prepared stages. The current direct GLES route still
  proceeds through SPIR-V, so merely adding another string at that call site is
  insufficient.
- The separate pipelines in [`bwsl_wasm.cpp`](../src/bwsl_wasm.cpp) and
  [`bwsl_compiler_service_core.h`](../src/core/bwsl_compiler_service_core.h), using
  the same preparation API. One core backend enum already names WGSL; CLI and
  runtime configuration are not fully connected to it.
- Target-aware cache identity including feature profile, binding/interface plan,
  vertex-input mode, and backend version. Wire `-all` only when the advertised
  support profile is ready, rather than introducing broad failures prematurely.

IR currently lacks per-instruction source spans. Add a cold origin-ID array with
AST node and owning-source context where inlining needs it. Preserve/remap origin
IDs through lowering, SSA, and later optimization; synthetic actions inherit the
responsible origin. Resolve full paths/line numbers only when reporting errors,
using the existing AST provenance and arena-backed diagnostic stream. Until then,
diagnostics can identify stage, instruction, and opcode, without pretending to
have a precise source location.

## Validation and performance plan

Use three independent correctness layers:

1. Parse and semantically validate every emitted module with pinned Naga and Tint
   revisions and the intended feature profile. Include negative diagnostics,
   arena exhaustion, deep nesting, and large output. The
   [Naga CLI](https://github.com/gfx-rs/wgpu/blob/trunk/naga/README.md) supports
   validation-only use; its
   [defaults](https://github.com/gfx-rs/wgpu/blob/trunk/naga-cli/src/bin/naga.rs)
   must be checked for permissive capabilities and coordinate adjustment.
2. Translate generated WGSL back to SPIR-V in test tooling and reuse
   [`tests/equivalence`](../tests/equivalence) with the existing Vulkan runner and
   independent expected results. Pin coordinate transformations and buffer ABI;
   do not mistake agreement between two translators for an independent oracle.
3. Create actual WebGPU modules and pipelines, then run compute readbacks and
   raster comparisons. This checks the resource plan, device limits, features,
   stage linkage, and host layout that standalone shader validation cannot prove.

Prioritize existing phi-swap, nested break/skip, lazy-expression, aggregate-value,
pointer-alias, uniform-array, matrix-layout, shared-memory, texture, and multipass
fixtures. [`gles_regression_tests.py`](../tests/gles_regression_tests.py) already
has useful control-flow and evaluation-order cases. Extend
[`register_namespace_tests.py`](../tests/register_namespace_tests.py) for WGSL
binding collisions and separate samplers. Track capability exclusions separately
from failures; do not hide unsupported cases as passing skips.

Benchmark only after correctness gates pass. Compare warm in-process paths:

- BWSL prepared IR to custom WGSL, including preparation/legalization/output.
- The same BWSL IR through SPIR-V emission and Rust Naga's SPIR-V-to-WGSL path.
- The same BWSL IR through SPIR-V emission and Tint's equivalent path.

Also report writer-only time, full BWSL compilation, validator cost, and GPU
pipeline creation separately. Measure median/p95, arena high-water usage, heap
allocations, output size, and binary size across small shaders, loop-heavy
compute, large materials, and multipass workloads. Pin versions, build flags,
features, validation policy, and coordinate handling. Report correctness coverage
beside performance; a smaller supported subset is not an apples-to-apples win.

[`benchmark_backend_cache.py`](../scripts/benchmark_backend_cache.py) supplies
representative workloads and timing conventions, but subprocess wall time is not
a fair comparison against an in-process custom writer. Use library harnesses for
the central comparison. Stage 1 records in-process Naga/Tint timings alongside
BWSL's reported phase timings and separate end-to-end process measurements.
A repeated in-process BWSL compiler driver and larger production workloads are
still needed for fine-grained optimization decisions.

## Delivery sequence and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1 | Prepared-stage contract, single-block normalization, arena tables, simple scalar/vector compute and storage output | Both validators plus a WebGPU compute readback; diagnostics for everything outside the slice |
| 2 | Structured branches/loops/switches, phi edge copies, local arrays and aggregates | Runtime tests for swaps, nested exits, lazy side effects, and straight-line reassignment |
| 3 | Physical buffer layouts, resource plan/reflection, vertex/fragment IO and textures | Byte-layout assertions and real WebGPU graphics pipelines across shared stage interfaces |
| 4 | Pointer rebinding, advanced operations, supported synchronization, feature profiles | Dedicated semantic tests and explicit diagnostics for unresolved contracts |
| 5 | Shared CLI/WASM/runtime plumbing, performance tuning, documented support matrix | Existing backend regressions pass; reproducible Naga/Tint comparisons and full supported corpus validation |

The first implementation should prove the full compute path and the arena/SoA
contract, then expand in these slices. Resolve atomic ordering before step 4;
choose and record actual validator/device versions before step 1. A custom
backend is a good architectural fit, but buffer ABI, control flow, and memory
semantics determine correctness more than the WGSL spelling of arithmetic.
