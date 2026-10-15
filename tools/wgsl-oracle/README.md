# WGSL validation and GPU oracle

This standalone development tool pins Naga 27.0.3 and wgpu 27.0.1. Its Cargo
lockfile pins transitive dependencies. BWSL has no Rust dependency at runtime or
when building the production compiler.

Build with Rust 1.88 or later from the repository root:

```sh
cargo build --release --locked --manifest-path tools/wgsl-oracle/Cargo.toml --target-dir build/wgsl-oracle
```

The executable is `build/wgsl-oracle/release/wgsl-oracle` (`.exe` on Windows).
Successful commands print one JSON object to stdout; errors use stderr and exit
1. GPU execution exits 77 when no adapter is available. A readback mismatch exits
1 and preserves the JSON result for diagnosis.

```sh
wgsl-oracle validate shader.wgsl
wgsl-oracle translate shader.wgsl shader.spv
wgsl-oracle translate shader.spv shader.wgsl
wgsl-oracle bench shader.wgsl validate 100 10
wgsl-oracle bench shader.wgsl wgsl-to-spv 100 10
wgsl-oracle bench shader.spv spv-to-wgsl 100 10
wgsl-oracle run shader.wgsl case.json
wgsl-oracle dump-msl shader.wgsl shader.metal
```

`bench` performs warmup then measured operations in one process. Each operation
includes parse, validation, output generation where requested, allocation and
destruction; it excludes file I/O. Reported microsecond statistics include all
samples, median, minimum, and p95. These measurements must not be compared with
process-launch-inclusive compiler timings without labeling that difference.

Compute cases default to group 0 storage buffers. Set `bind_group_index` to 1
when testing storage resources translated from the existing BWSL SPIR-V ABI.
Data and integer expectations are raw
little-endian 32-bit words. Omit `entry_point` to select a sole compute entry
automatically. For floats, either supply exact bit-pattern `expected` values or
`expected_f32` with explicit `absolute_tolerance` and `relative_tolerance`.

```json
{
  "entry_point": "main",
  "workgroups": [1, 1, 1],
  "iterations": 10,
  "warmup": 2,
  "buffers": [
    {"binding": 0, "data": [1, 2, 3, 4]},
    {"binding": 1, "data": [0, 0, 0, 0], "expected": [2, 4, 6, 8]}
  ]
}
```

A buffer may override the default group with `"group": 1`. Set `"uniform": true`
for a uniform buffer; otherwise it is storage. This supports cases with uniforms
and storage in different bind groups without rewriting the shader. Uniform data
uses the same raw-word format and must already contain the reflected padding.
`dump-msl` retains Naga's validated Metal translation for diagnosis; it does not
compile or execute that output independently.

Repeated dispatches reuse buffers. Benchmark fixtures must overwrite outputs
deterministically and not accumulate across dispatches. Pipeline creation and
shader-module creation are reported separately from dispatch. `submit_and_wait`
includes command encoding, submission, synchronization and timestamp resolving;
`gpu_timestamp` measures the compute pass itself where the adapter supports
timestamp queries. Buffer mapping/readback is outside dispatch timing. Adapter,
backend and device type are recorded so software adapters cannot be confused
with hardware GPU measurements. Native wgpu validates execution through Naga;
independent Tint validation is run by the Python test and benchmark harnesses.
Pipeline creation can hit driver caches; its timing is not a guaranteed cold
compilation measurement. Native shader execution does not measure browser or
JavaScript API overhead.

The test suite compares generated shaders, handwritten WGSL and Naga roundtrips
against CPU expectations, rather than only comparing generated source text:

```sh
python3 tests/wgsl_backend_tests.py --compiler build/bwslc --require-gpu
```

The signed vector division fixture retains direct Naga/Tint validation and GPU
readback coverage. Naga 27.0.3 fails to revalidate its own SPIR-V division helper
with `IncompatibleOperands("LogicalAnd(vec4<bool>, _)")`; only that exact
roundtrip error is recorded as a known skipped oracle limitation. Other
roundtrip errors fail the suite, and a future successful roundtrip runs normally.
