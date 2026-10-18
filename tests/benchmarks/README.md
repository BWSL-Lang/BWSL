# GPU benchmarks

Shareable GPU benchmarks that compare hand-written GLSL ES with the code BWSL
generates for the same shader, and check that both render the same image.
Use them when reporting a performance bug: the numbers and the shaders are
all in this repository.

```bash
python tests/benchmarks/run.py                     # every test, 720p / 1080p / 4K
python tests/benchmarks/run.py hsluv noise         # some tests
python tests/benchmarks/run.py --backends gl-hand,gl-bwsl --resolutions 4k
python tests/benchmarks/run.py --rounds 3          # interleave repeats (less drift)
python tests/benchmarks/run.py --list
```

Needs `build/bwslc`, numpy, a GPU with OpenGL 4.3+ (for `#version 300 es` on
desktop GL) and Vulkan 1.2, and the Vulkan SDK to build the runner and for
`glslangValidator`. `run.py` builds `build/bench_runner` when it is missing
or older than `src/bench_runner.cpp` (`build.bat bench_runner` on Windows,
`make bench_runner` elsewhere). macOS has no usable GL; run the Vulkan
backends only (`--backends vk-hand,vk-bwsl`).

Under WSL2, GL uses llvmpipe (CPU) unless you select Mesa's D3D12 driver:
`GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA` (or `AMD`,
`Intel`). Vulkan has no GPU driver there and runs on llvmpipe, so its timings
are only useful for checking that it works.

## Backends

| Column | Shader | API |
|---|---|---|
| GL hand | `<test>_hand.vert` / `<test>_hand.frag` | OpenGL |
| GL bwsl | `bwslc -gles` (SPIR-V, then SPIRV-Cross) | OpenGL |
| GL direct | `bwslc -gles-direct` (IR to GLSL ES) | OpenGL |
| VK hand | `<test>_hand.*` compiled with glslangValidator | Vulkan |
| VK bwsl | `bwslc -spv` | Vulkan |

Ratios compare each column with the hand-written shader on the same API: GL
columns against GL hand, VK bwsl against VK hand. VK hand against GL hand is
the API's own cost on that driver, so a GL/Vulkan gap that VK hand shares is
not BWSL's. The report notes when `-gles-direct` produced the same code as
`-gles` (it falls back to SPIRV-Cross for some shaders).

BWSL's SPIR-V reads vertex attributes from storage buffers ("vertex
pulling", one buffer per attribute indexed by `gl_VertexIndex`); VK hand uses
ordinary vertex inputs. The runner feeds both.

## Timing

`build/bench_runner` renders one draw per frame into offscreen targets and
times every frame on the GPU: `GL_TIME_ELAPSED` queries on GL, timestamps
around the render pass on Vulkan (with a full barrier between frames so they
don't overlap). Each frame clears the targets and draws once; the clear is
inside the timed region on both APIs. CPU clocks only decide when to stop.

Per run: warm-up frames (`--warmup`, at least 0.25 s), then up to `--frames`
timed frames, stopping early after `--budget` seconds once 5 frames are
timed. The table shows the median frame. Each resolution runs every backend
in turn; when a test's resolution took longer than `--skip-after` seconds (10
by default), its larger resolutions are skipped.

The GL and Vulkan device and driver are printed with the table. On laptops
with two GPUs the runner asks for the discrete one on both APIs; check the
header if the numbers look odd. `--vk-device N` runs Vulkan on another GPU
(the index from `vulkaninfo --summary`), e.g. an integrated one with
`--backends vk-hand,vk-bwsl`.

## Verification

The first resolution also reads back every target of every backend and
compares it with GL hand. A value counts as different when it is more than
`tolerance` away (default 2/255); the test fails if more than `max_mismatch`
of the values (default 0) differ. Differing outputs and a red difference map
are written as PNGs to `build/benchmarks/<test>/images/` (`--save-images`
writes every output). `run.py` exits with 1 when an output differs or a
backend fails.

When Vulkan runs on another device than GL, VK bwsl is compared with VK hand
instead (marked "vs VK hand"), since filtering and mip selection differ
between devices.

Keep tolerances tight and explain any widening in `tolerance_note`: a
mismatch is usually a compiler bug.

## Tests

| Test | What it stresses |
|---|---|
| `postprocess` | Fullscreen pass, 4 textures (15 taps), tone mapping, grading, integer-hash grain |
| `mrt` | 4 RGBA16F render targets at once, 25 taps, constant array in a loop |
| `hsluv` | Stdlib `Color` HSLuv conversions, dynamically indexed 72-entry table |
| `hsluv_components` | `hsluvToRgb` with one varying component (hue, saturation and lightness sweeps); BWSL folds what the constant components decide |
| `noise` | Loops in loops: 2x2 supersampled domain-warped fbm and Worley noise |
| `vertex_heavy` | 2.1M triangles; fbm, bone blending and lighting in the vertex shader |

## Adding a test

Create a directory `<test>/` with these files, all named after it:

- `<test>.bwsl`: one pipeline with one pass.
- `<test>_hand.vert`, `<test>_hand.frag`: `#version 300 es`. Use BWSL's GL names so one
  set of bindings fits both: attributes `a_<name>`, uniform blocks
  `layout(std140) uniform ub_<name>`, textures `uniform sampler2D t_<name>`.
  Declare the varyings in the same order in both stages (the Vulkan build
  assigns their locations in declaration order). Don't set bindings or
  attribute locations; the runner resolves them by name.
- `<test>.json`:

```jsonc
{
  "description": "one line for --list",
  "targets": 1,                 // render targets (fragment outputs)
  "format": "rgba8",            // rgba8 | rgba16f | rgba32f
  "depth": false,               // D32F depth test (LESS)
  "mesh": {"type": "fullscreen"},  // or {"type": "grid", "cols": N, "rows": N}
  // In the pipeline's declaration order. Vulkan binds vertex-pulling
  // buffers by this order. fullscreen provides position; grid provides
  // position (z = 0) and uv.
  "attributes": [{"name": "position", "type": "float2"}],
  "uniforms": [{
    "name": "params",            // resource name in <test>.bwsl
    // std140 floats; strings are expressions in width, height, aspect
    "values": ["1 / width", 1.0, 0, 0],
    "random": {"count": 8, "seed": 1, "range": [-1, 1]}  // appended
  }],
  "textures": [{
    "name": "scene",
    "size": "target",           // target | half | quarter | [w, h]
    "format": "rgba16f",        // rgba8 | rgba16f
    "gen": "smooth",            // smooth | random | gradient
    "range": [0, 4], "seed": 1, "detail": 8, "noise": 0.0,
    "filter": "linear",         // linear | nearest
    "wrap": "clamp",            // clamp | repeat | mirror
    "mipmaps": false            // full mip chain, trilinear filtering
  }],
  "tolerance": 0.0079,
  "max_mismatch": 0.0
}
```

Give tiled or scaled-down textures `"mipmaps": true`, as a game would.
Without mipmaps, heavily minified reads thrash the texture cache, and that
cost differs between drivers (NVIDIA's Vulkan driver was up to 1.45x slower
than GL on such reads), drowning out the shader. `run.py` builds the chain
on the CPU (2x2 box filter) so both APIs sample identical levels. Render
targets read back 1:1 (scene, bloom, history) don't need them.

Avoid `gl_FragCoord` / `input.position` in fragment shaders: GL and Vulkan
put the origin in different corners. Derive pixel positions from a varying
and the resolution instead. Integer hashes (PCG) keep noise identical on
every backend; `sin`-based hashes don't.
