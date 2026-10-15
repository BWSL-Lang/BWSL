# WGSL performance and correctness baselines

For browser comparisons, use the compute or render WebGPU page below. The native Naga/Tint
measurements in the rest of this document answer separate compiler questions.

## In-process BWSL compilation

```sh
make wgsl-inprocess
make benchmark-wgsl-inprocess
```

The development driver reuses the production frontend and stage compiler. Source
is preloaded, but every observation creates fresh compiler contexts and arenas,
including parsing, variant/comptime resolution, lowering, CFG/SSA, emission and
owning WGSL/reflection results. Per-compilation compiler-state destruction is
included. Parsed ASTs and optimized IR are not reused. Source/output files and
process startup are excluded; process-global caches remain warm.

The benchmark checks driver/CLI source and reflection equality before recording
samples, then measures fresh CLI launches separately. It records raw phase times,
executable/source hashes and Naga/Tint WGSL parse/validate diagnostics. To compare
a preserved compiler, build/preserve its in-process driver before editing source,
then run the candidate with both baseline executables:

```sh
python3 scripts/benchmark_wgsl_inprocess.py \
  --baseline-driver build/previous/wgsl-inprocess \
  --baseline-compiler build/previous/bwslc
```

Defaults are ten alternating baseline/candidate blocks, twenty measured native
samples after five warmups per block, ten interleaved CLI observations and 100
reference-tool observations after five warmups. `--case`, `--blocks`,
`--samples-per-block`, `--cli-runs`, `--reference-runs` and `--output` control
the run. Existing output directories are rejected. A baseline which cannot
compile a new feature is explicitly recorded as unsupported, not timed.

The 2026-10-15 comparison found AST position-map collisions were the largest
compiler cost. Mixing the full typed node key preserves the arena-backed SoA
maps and reduces in-process skinned GGX from 2,079.29 to 352.31 µs, Gaussian
from 973.98 to 140.90 µs, and PBR from 1,309.67 to 233.50 µs. All nine previously
supported WGSL outputs remain byte-identical. CLI wall time remains roughly
4–5 ms; it includes startup, files and host work excluded by the native window.
Exact samples and hashes are in
[`build/wgsl-inprocess-benchmarks/final-v3/`](../../build/wgsl-inprocess-benchmarks/final-v3/).

To measure source-to-ready through the browser without repeated CLI launches:

```sh
python3 scripts/benchmark_wgsl_render.py --inprocess build/wgsl-inprocess --port 8776
```

This keeps one worker per source, preloads that source and compiles fresh state
for each request. Worker output is verified against the CLI snapshots before
serving. Native time is retained separately from pipe/JSON/server handling and
browser readiness. The page identifies this mode explicitly. Source hashes and
compiler/harness identities are retained, and the analyzer separates this mode
from CLI runs. Neither browser mode clears driver caches or guarantees cold
compilation. The worker is a benchmark integration, not a public compiler API.

The final browser runs reduced skinned GGX source-request-to-ready time from
10.208 ms through the fresh CLI path to 2.273 ms through the worker path;
handwritten WGSL was 1.703 ms in that worker run. The textured material measured
7.693 ms through CLI, 1.710 ms through the worker and 1.540 ms handwritten in
the worker run. These are separate repeated-source observations with warm caches.
GPU performance remained close to handwritten across the render corpus; the
textured material showed a small improvement at 1024² on this adapter.
See the [full findings and raw report links](../../docs/wgsl-backend-design.md#in-process-compilation-uniforms-textures-and-structured-control-flow)
for repeatability, native Naga/Tint costs and the documented native Metal
static-loop discrepancy.

## Batch CLI throughput

```sh
python3 scripts/benchmark_wgsl_batch.py \
  --manifest build/wgsl-render-benchmarks/inprocess-final-v3/manifest.json \
  --output build/wgsl-batch-benchmarks/new-run
```

The prepared manifest supplies validated source/ABI expectations. The driver
compiles all measured render cases in one CLI batch and, separately, one fresh
process per case. Ten alternating pairs follow two warmups; every output and
reflection record must match the manifest. Filesystem caches remain warm.
The final four-pipeline corpus took 8.617 ms batched versus 22.468 ms in four
processes, or 2.154 versus 5.617 ms per pipeline amortized. Batch execution is
sequential and shares module-source discovery. This measures source-to-WGSL
throughput, including files and startup. It does not include WebGPU pipeline
creation or measure individual source-request-to-ready latency. Use the worker
browser benchmark for that separate integration question.
[Raw batch observations](../../build/wgsl-next/batch-final-v2/results.json).

## Browser render pipelines

```sh
make test-wgsl-render
make benchmark-wgsl-render
```

The first target runs sanitizer units, benchmark harness tests, native graphics
compilation/linkage checks and both Naga/Tint validators. The second prepares
matched shaders and starts a localhost server. Open its printed URL and click
**Run render benchmark**. Both vertex/fragment sources go directly to browser
WGSL modules and `createRenderPipelineAsync`; this path never uses SPIR-V.

The corpus includes a four-instance skinned GGX mesh (32,768 triangles, four
bones per vertex, four lights and depth), a procedural material with normal/
roughness MRT and derivative antialiasing, and 16,384 alpha-blended particle
billboards, plus a textured material using padded scalar uniforms, a mat4
transform, a runtime sampling loop and conditional discard. Both variants share
geometry, buffers, explicit pipeline layouts,
attachments, depth/blend state and draw calls. A separate untimed regression
checks sparse stage locations, integer flat and linear interpolation, varying
clip W, fragment coordinates/depth, output snapshots and nonzero vertex/instance
offsets. Other untimed regressions check constant nested loops with continue and
fractional/clamped texture-array layer selection.
The earlier mesh workload retains its unrolled BWSL source for comparable
historical measurements; the new material uses structured control flow.
The backend supports sampled textures, std140 uniforms and matrix reads/products;
general aggregate values, local arrays, storage textures, offset overloads,
workgroup storage and atomics remain outside this slice.

Independent CPU references check full procedural/particle/interface images and
interior software-rasterized mesh probes. All attachment pixels are also compared
between the variants. RGBA8 tolerance is three byte levels; integer MRT is exact.
Uniform buffers and sampled texture bytes are also read back. All input bytes
and images are checked before and after timing. Reports retain
actual errors, full attachment hashes and reduced previews. Preview generation
runs after all measurements.

Choose 256, 512 or 1024 square resolution. Defaults are 30 measured GPU pairs,
five warmup pairs and ten repeated compilation pairs after two compilation
warmups; order alternates AB/BA with a seeded start. Keep the tab active. GPU
timestamps bracket batches of complete render passes, including clears and
stores; common batch calibration targets 20 ms up to 256 frames. Reported times
are normalized per frame on reused resources. CPU encoding/submission/wait is
separate, and never substitutes for unavailable GPU timestamps.

**Full source request → render pipeline ready** starts before the localhost
source request and ends after both modules' compilation information and the
asynchronous pipeline. Handwritten reads two snapshots. By default BWSL launches one fresh
CLI process for both stages, including reflection and output files. Server time,
frontend/per-stage/summary compiler self-reports, module readiness and pipeline
readiness are recorded separately. Compiler timings are nested; do not sum
subtotals or add `IR setup` to `IR lowering`. Neither repeated nor first-observed
browser times are guaranteed cold. `--inprocess` selects the native worker path
described above; neither mode measures WebAssembly.

Preparation also records 100 in-process Naga/Tint parse/validate samples after
five warmups for every stage/variant. These native diagnostics exclude GPU and
downstream pipeline compilation. Tool executable hashes and raw samples are
retained alongside source/compiler/harness hashes. The standalone validators
remain development dependencies, not production compiler dependencies.

Using existing builds:

```sh
python3 scripts/benchmark_wgsl_render.py --port 8775
python3 scripts/benchmark_wgsl_render.py --prepare-only --output build/render-prepared
python3 scripts/benchmark_wgsl_render.py --serve-existing build/render-prepared
python3 scripts/analyze_wgsl_web.py build/wgsl-render-benchmarks/RUN/results-*.json --output build/wgsl-render-benchmarks/RUN/summary.json
```

Use repeated `--case skinned_ggx`, `--case procedural_mrt`, or `--case particles`
to select pipelines. `--reference-runs` controls native tool samples;
`--skip-reference-tools` explicitly creates a partial run. `--output` must name
a new artifact directory. Both schemas' JSON reports are saved locally by the
server and downloadable from the page; nothing is uploaded externally. The
analyzer separates resolutions as well as compiler/source/harness/device
identities and never treats the interface regression as a performance result.

On 2026-10-15, two runs each at 512×512 and 1024×1024 on Chrome 155 / Apple
`metal-3` passed all checks. No pipeline had a consistent GPU direction outside
the recorded intervals. Latest 1024×1024 GPU medians, handwritten → BWSL, were
100.99 → 100.86 µs (mesh), 69.50 → 70.27 µs (MRT), and 92.29 → 92.67 µs
(particles). Full source-to-ready was 2.000 → 12.713 ms, 1.668 → 9.577 ms and
1.855 → 9.600 ms respectively. BWSL native process/source preparation dominates
that full-path gap; repeated browser readiness was only 0.278–0.405 ms.
These are single-device observations with cache/quantization limits. See
[`docs/wgsl-backend-design.md`](../../docs/wgsl-backend-design.md#vertex-and-fragment-implementation)
for the breakdown and
[`build/wgsl-render-benchmarks/render-final/`](../../build/wgsl-render-benchmarks/render-final/)
for exact raw reports and hashes.

## Browser compute pipelines

```sh
make benchmark-wgsl-web
```

Or, using an existing compiler build:

```sh
python3 scripts/benchmark_wgsl_web.py
```

Open the printed localhost URL in a WebGPU browser and click **Run browser
benchmark**. This prepares snapshots, then sends the handwritten and generated
WGSL strings directly to `createShaderModule` and
`createComputePipelineAsync`. The browser path does not generate or load SPIR-V,
and requires no standalone Naga/Tint tool. No browser flags are changed.

The page runs six compute kernels with an explicit shared pipeline layout and
identical buffers and workgroups. It retains the three microkernels and adds:

* A 25-tap RGBA Gaussian filter over independent 256×256 images, with clamped edges.
* Four-bone linear blend skinning with dynamic gathers from a 64-bone palette.
* Four-light Cook–Torrance shading with GGX distribution, Smith geometry, Schlick
  Fresnel and Reinhard tone mapping.

These are compute implementations of graphics-related work, **not vertex or
fragment stage coverage**. BWSL fixtures retain explicitly unrolled fixed work
for comparable historical measurements. The normal handwritten WGSL uses
loops/helpers where appropriate. This compute corpus does not exercise textures,
workgroup tiling, atomics or divergent control flow.

Every input and output buffer is checked before and after timing, and the two
shader outputs are also compared directly. Inputs and all outputs except GGX
are bit-exact. GGX allows 0.0002 absolute + 0.0002 relative error against an
independent double-precision CPU formula; reports retain maximum observed error.
Float output buffers start with NaN sentinels to catch missing writes. The default
uses 1,048,576 invocations, 30 measured pairs and five warmup pairs. Variant order
alternates AB/BA with a seeded starting order. Keep the tab visible; backgrounding
or cancelling stops the run.

GPU timestamps measure a batch of identical dispatches, with a common batch size
calibrated against the handwritten shader. Calibration targets at least 10 ms,
up to 1,024 dispatches per batch, to amortize the browser's timestamp quantization.
Reported microseconds per dispatch are batch averages, not the precision of an
individual timestamp. Inputs and outputs are reused, so these are warm-buffer
throughput measurements. Without usable timestamps, the page reports only CPU
encoding/submission/wait time and does not substitute that for GPU duration.

**Full source-to-ready latency** starts before the browser requests a source from
the local server and ends when its compute pipeline is ready. The handwritten
path reads its WGSL snapshot. The BWSL path launches a fresh CLI process, reads
the BWSL snapshot, parses/lowers/optimizes it and writes native WGSL. Both include
localhost request/JSON handling. Reports separately retain server preparation,
BWSL phase self-reports, browser readiness and the per-sample component sum
(server preparation + browser readiness, excluding request overhead). Phase
measurements are nested; do not add `IR setup` to `IR lowering` again.

This is a native-CLI-to-browser integration measurement; it does not represent
an in-process or WebAssembly BWSL compiler. No SPIR-V is used on this path.
Snapshot source hashes are verified on every request; executable hashes are
checked at server startup, with size/mtime guards during the run.

Shader/pipeline readiness is also separate: module compilation information is awaited,
then asynchronous pipeline creation. The page records first-observed timings,
two warmups, then ten repeated same-source observations. Browser and driver
caches cannot be reliably cleared by this page; first-observed timings are not
guaranteed cold compilation, and repeated medians may reflect cache hits.
See the [WebGPU pipeline contract](https://www.w3.org/TR/webgpu/#dom-gpudevice-createcomputepipelineasync)
and [Chrome's timestamp precision documentation](https://developer.chrome.com/docs/web-platform/webgpu/developer-features).

JSON reports include raw timings, paired GPU ratios and bootstrap intervals,
browser details, exposed adapter information, settings, compiler/source/harness
hashes and correctness counts. These intervals describe this run's sample noise;
they do not account for systematic timer quantization or thermal changes.
The ratio is the median of the sample-pair ratios, which can differ from the
quotient of the two separately summarized medians.
Results are saved by the local server under `build/wgsl-web/<run>/results-*.json`
and are also downloadable from the page. Nothing is uploaded to an external
service. `--output` chooses a new artifact directory; `--prepare-only` prepares
files without starting a server; `--serve-existing PATH` reopens a prior run.

Run the browser benchmark's CPU expectation/statistics tests with:

```sh
make test-wgsl-web
```

The GPU comparison must run in a real WebGPU browser. The unit tests and the
native wgpu oracle do not replace that browser measurement. This page's corpus
is compute-only; use the render page above for actual vertex/fragment stages.

Use `--case` repeatedly to select workloads. To investigate loop/unrolling
effects, `--unrolled-reference` selects the separately written unrolled WGSL
controls for Gaussian and GGX. To compare compiler changes directly on the same
browser/device/buffers, preserve an executable and use:

```sh
python3 scripts/benchmark_wgsl_web.py --baseline-compiler build/bwslc-before
```

In that mode, both full-compilation paths launch their own BWSL compiler on the
same source, and the reference is visibly labeled **Previous BWSL**. The two
compilers must agree on the buffer ABI. This mode and `--unrolled-reference`
are mutually exclusive. The report's internal `handwritten` variant key denotes
the reference, which can be the previous compiler in this diagnostic mode.

Repeat the browser run, then summarize observations with:

```sh
python3 scripts/analyze_wgsl_web.py build/wgsl-web/RUN/results-*.json --output build/wgsl-web/RUN/summary.json
```

The analyzer requires successful, distinct reports and separates differing
shader, compiler, harness, browser/device, input-size and reference identities.
It checks the direction of each run's interval rather than pooling unlike data.
Its repeatability observations are not general performance guarantees.

### Initial browser measurements

On 2026-10-15, Chrome 155.0.8059.40 exposed an Apple GPU with the `metal-3`
architecture and hardware execution (no fallback adapter). Two complete runs
of the final harness passed exact readback checks for every variant. With
1,048,576 invocations, 30 measured pairs and five warmup pairs, the second run
reported these median GPU batch durations normalized per dispatch:

| Kernel | Handwritten WGSL (µs) | BWSL → WGSL (µs) |
| --- | ---: | ---: |
| SAXPY | 21.12 | 22.72 |
| Unsigned hash | 17.28 | 17.44 |
| Vector affine | 56.32 | 56.45 |

The paired BWSL/handwritten ratios were 1.043, 1.014 and 0.993 respectively;
all three bootstrap intervals included 1. Differences changed direction between
runs, so this small corpus establishes no repeatable GPU speedup. Pipeline
readiness measurements also varied with caching and are not cold compilation
comparisons. BWSL translation happens before page loading and is excluded from
these browser timings.

The local reports are
`build/wgsl-web/browser-final/results-20261015T104009.917366Z.json` and
`build/wgsl-web/browser-final/results-20261015T104053.705341Z.json`.
Their raw samples, browser metadata and hashes identify the exact measured
inputs; build artifacts are not checked into Git.

## Native compiler baselines

The benchmark compares handwritten WGSL with the same kernels expressed in BWSL.
Each shader performs a real compute dispatch over storage buffers. The initial
corpus covers scalar SAXPY, an unsigned integer avalanche hash, vector affine
transforms, Gaussian filtering, skinning and GGX shading. It fits the first
backend milestone; use the separate render corpus for graphics pipelines.
Textured production shaders are not covered. Complex native GPU references reuse the browser CPU
oracle through Node.js.

## Build the independent tools

From the repository root:

```sh
make build
cargo build --release --locked --manifest-path tools/wgsl-oracle/Cargo.toml --target-dir build/wgsl-oracle
python3 tools/wgsl-bench/bootstrap_tint.py
```

The Tint bootstrap requires Git, CMake, Ninja, Python, and a C++20 compiler. It
downloads Dawn and its pinned dependencies into ignored `build/wgsl-tools/`, and
builds only the standalone Tint CLI and our timing driver. It installs nothing
globally and refuses to replace a different existing checkout. The first build
is substantial. The fixed Dawn revision is
`af01c4ef653550c1e9bf6c04065e7b4f054ea1e8`; update it deliberately together with the
driver API usage. Naga and wgpu versions and transitive dependencies are locked by
`tools/wgsl-oracle/Cargo.lock`.

Tint's [CMake quickstart](https://dawn.googlesource.com/dawn/+/af01c4ef653550c1e9bf6c04065e7b4f054ea1e8/docs/quickstart-cmake.md),
[benchmark design](https://dawn.googlesource.com/dawn/+/af01c4ef653550c1e9bf6c04065e7b4f054ea1e8/docs/tint/benchmark.md),
and [public conversion API](https://dawn.googlesource.com/dawn/+/af01c4ef653550c1e9bf6c04065e7b4f054ea1e8/src/tint/api/tint.h)
describe the upstream tools used here. Our driver reads each fixture at runtime
so the same files feed both compilers without modifying Tint's embedded corpus.

## Run

```sh
python3 scripts/benchmark_wgsl.py
```

For a smoke run:

```sh
python3 scripts/benchmark_wgsl.py --runs 3 --inprocess-runs 10 --gpu-runs 3 --warmup 1
```

For an optimization comparison, preserve the previous executable before building
the candidate, then include it in the same run:

```sh
cp build/bwslc build/bwslc-before
make build
python3 scripts/benchmark_wgsl.py --baseline-compiler build/bwslc-before
```

The baseline and candidate process paths are interleaved. Both emitted WGSL
variants are independently validated, timed through Naga and Tint, and checked
on the same GPU against the CPU reference. The report records both executable
hashes and keeps all generated outputs. This avoids relying on compiler timings
from a different session or machine state when evaluating a change.

The default run requires both Naga and Tint. `--allow-missing-tint` explicitly
allows a partial local run. `--skip-runtime` explicitly omits GPU correctness and
timing for hosts without a usable adapter. Neither omission is a full validation
result. `--case`, `--elements`, `--runs`, `--inprocess-runs`, and `--gpu-runs` control
the corpus and sample counts. `--elements` must be a multiple of the 64-invocation
workgroup size. The caller must supply a new directory for `--output`; existing
results are never removed or overwritten.

Each run saves `results.json`, generated WGSL/SPIR-V, compiler logs, input cases,
and full GPU output under `build/wgsl-benchmarks/<timestamp>/`. The report records
source and executable hashes, Git revision and dirty state, tool versions, Tint
build configuration, Cargo lock hash, platform, settings, and every measured
sample. Errors fail the command and are retained in the report.

## What the measurements mean

* **Process wall time:** actual time for each complete command sequence, including
  process startup, file reads/writes, and shutdown. Paths are interleaved with a
  fixed randomized order. Compare paths ending in SPIR-V to assess source-to-SPIR-V
  compilation; source-to-WGSL emission has a different endpoint.
* **BWSL phase time:** `bwslc -timing` self-reports, including WGSL emission. These
  are separate from process time and are currently rounded to 0.001 ms. BWSL's
  external SPIR-V validation is disabled for timing; every resulting WGSL variant
  is independently validated outside the timed process loops.
  `Frontend setup` is included in `Lexer init`; `IR setup` is included in
  `IR lowering`. Do not add those nested measurements again to the phase total.
* **In-process Naga/Tint time:** parse, semantic validation, optional emission,
  allocation, and result destruction with preloaded input. Validation and
  source-to-SPIR-V conversion are measured separately. Both tools use their
  default backend options, including their own robustness transforms; this is a
  real default-tool comparison, not identical optimization pipelines.
* **GPU time:** the same wgpu adapter executes handwritten WGSL, native BWSL WGSL,
  BWSL SPIR-V through Naga, and BWSL SPIR-V through Tint. GPU timestamps, when
  supported, are separate from CPU submission-and-wait time. Module and pipeline
  creation are single observations, not warmed statistical measurements.

Warmups are excluded. Statistical summaries include median, minimum, nearest-rank
p95, and raw samples. All runtime variants are checked against independent host
formulas; input buffers are also checked for accidental modification. Float
fixtures use small dyadic values for exact comparisons. Output buffers start
with sentinels to catch missing writes. The same buffers are reused because these
kernels overwrite their outputs on every dispatch.

The native WGSL backend currently assigns storage to group 0. Existing BWSL
SPIR-V assigns storage to descriptor set 1, which Naga and Tint preserve as group
1. The runtime harness reads the declared group from each artifact and binds the
same logical buffers there; it does not rewrite the generated shaders. Reports
include this binding difference.

Small kernels can be dominated by process startup or dispatch overhead. Repeated
runs on an idle machine and a larger corpus are needed before claiming parity
or a speedup. No speed claim or performance gate is inferred from compilation
success. The initial harness intentionally has no absolute timing threshold.

## Tint driver protocol

```sh
build/wgsl-tools/tint/bwsl-tint-bench validate shader.wgsl
build/wgsl-tools/tint/bwsl-tint-bench translate shader.wgsl shader.spv
build/wgsl-tools/tint/bwsl-tint-bench translate shader.spv shader.wgsl
build/wgsl-tools/tint/bwsl-tint-bench bench shader.wgsl wgsl-to-spv 100 5
build/wgsl-tools/tint/bwsl-tint-bench bench shader.spv spv-to-wgsl 100 5
```

The other benchmark mode is `validate`. Validation and benchmark output is JSON;
errors go to stderr with a nonzero exit status. Translation writes the destination
file. This protocol intentionally matches the development Naga oracle.
