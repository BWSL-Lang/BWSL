 ```
 > python tests/benchmarks/run.py --rounds 2 --skip-after 20
[hsluv] compiling
[hsluv] 1280x720: gl-hand 0.052, gl-bwsl 0.052, gl-direct 0.052, vk-hand 0.058, vk-bwsl 0.054 (5.1s)
[hsluv] 1920x1080: gl-hand 0.121, gl-bwsl 0.122, gl-direct 0.123, vk-hand 0.129, vk-bwsl 0.119 (4.7s)
[hsluv] 3840x2160: gl-hand 0.493, gl-bwsl 0.498, gl-direct 0.498, vk-hand 0.509, vk-bwsl 0.472 (5.2s)
[hsluv_components] compiling
[hsluv_components] 1280x720: gl-hand 0.025, gl-bwsl 0.027, gl-direct 0.026, vk-hand 0.032, vk-bwsl 0.031 (4.9s)
[hsluv_components] 1920x1080: gl-hand 0.060, gl-bwsl 0.065, gl-direct 0.063, vk-hand 0.070, vk-bwsl 0.066 (4.7s)
[hsluv_components] 3840x2160: gl-hand 0.254, gl-bwsl 0.270, gl-direct 0.263, vk-hand 0.271, vk-bwsl 0.257 (4.8s)
[mrt] compiling
[mrt] 1280x720: gl-hand 0.125, gl-bwsl 0.126, gl-direct 0.126, vk-hand 0.131, vk-bwsl 0.131 (8.2s)
[mrt] 1920x1080: gl-hand 0.291, gl-bwsl 0.293, gl-direct 0.296, vk-hand 0.303, vk-bwsl 0.301 (5.0s)
[mrt] 3840x2160: gl-hand 1.238, gl-bwsl 1.246, gl-direct 1.237, vk-hand 1.256, vk-bwsl 1.235 (6.1s)
[noise] compiling
[noise] 1280x720: gl-hand 1.193, gl-bwsl 1.191, gl-direct 1.190, vk-hand 1.119, vk-bwsl 1.122 (6.0s)
[noise] 1920x1080: gl-hand 2.689, gl-bwsl 2.672, gl-direct 2.673, vk-hand 2.514, vk-bwsl 2.501 (7.4s)
[noise] 3840x2160: gl-hand 10.866, gl-bwsl 10.782, gl-direct 10.788, vk-hand 10.085, vk-bwsl 10.079 (15.9s)
[postprocess] compiling
[postprocess] 1280x720: gl-hand 0.038, gl-bwsl 0.038, gl-direct 0.038, vk-hand 0.043, vk-bwsl 0.043 (4.9s)
[postprocess] 1920x1080: gl-hand 0.088, gl-bwsl 0.086, gl-direct 0.086, vk-hand 0.111, vk-bwsl 0.110 (4.8s)
[postprocess] 3840x2160: gl-hand 0.503, gl-bwsl 0.506, gl-direct 0.505, vk-hand 0.552, vk-bwsl 0.553 (5.9s)
[struct_folding] compiling
[struct_folding] 1280x720: gl-hand 0.043, gl-bwsl 0.040, gl-direct 0.043, vk-hand 0.044, vk-bwsl 0.043 (4.9s)
[struct_folding] 1920x1080: gl-hand 0.100, gl-bwsl 0.094, gl-direct 0.099, vk-hand 0.096, vk-bwsl 0.095 (4.7s)
[struct_folding] 3840x2160: gl-hand 0.412, gl-bwsl 0.390, gl-direct 0.415, vk-hand 0.381, vk-bwsl 0.377 (5.0s)
[struct_folding_inline] compiling
[struct_folding_inline] 1280x720: gl-hand 0.043, gl-bwsl 0.040, gl-direct 0.043, vk-hand 0.044, vk-bwsl 0.043 (4.9s)
[struct_folding_inline] 1920x1080: gl-hand 0.099, gl-bwsl 0.095, gl-direct 0.100, vk-hand 0.096, vk-bwsl 0.095 (4.6s)
[struct_folding_inline] 3840x2160: gl-hand 0.413, gl-bwsl 0.390, gl-direct 0.413, vk-hand 0.381, vk-bwsl 0.379 (5.0s)
[structs] compiling
[structs] 1280x720: gl-hand 0.105, gl-bwsl 0.100, gl-direct 0.121, vk-hand 0.099, vk-bwsl 0.099 (5.0s)
[structs] 1920x1080: gl-hand 0.239, gl-bwsl 0.229, gl-direct 0.273, vk-hand 0.217, vk-bwsl 0.216 (4.7s)
[structs] 3840x2160: gl-hand 0.952, gl-bwsl 0.921, gl-direct 1.081, vk-hand 0.847, vk-bwsl 0.841 (5.7s)
[vertex_heavy] compiling
[vertex_heavy] 1280x720: gl-hand 0.515, gl-bwsl 0.515, gl-direct 0.555, vk-hand 0.461, vk-bwsl 0.453 (5.9s)
[vertex_heavy] 1920x1080: gl-hand 0.549, gl-bwsl 0.548, gl-direct 0.594, vk-hand 0.504, vk-bwsl 0.490 (5.6s)
[vertex_heavy] 3840x2160: gl-hand 0.624, gl-bwsl 0.617, gl-direct 0.654, vk-hand 0.579, vk-bwsl 0.574 (5.8s)
```

## BWSL GPU benchmarks

- OpenGL: NVIDIA GeForce RTX 4060 Ti/PCIe/SSE2 (4.6.0 NVIDIA 617.14)
- Vulkan: NVIDIA GeForce RTX 4060 Ti (NVIDIA 617.14)
- bwslc: build\bwslc.exe, built 2026-10-19 20:35, source 6bed3f0
- GPU ms per frame, median of up to 100 timed frames (x2 rounds)
- Ratios compare against the hand-written shader on the same API (VK hand against GL hand, i.e. the API's own cost)

| Test                  | Resolution | GL hand |        GL bwsl |      GL direct |        VK hand |        VK bwsl |
| --------------------- | ---------- | ------: | -------------: | -------------: | -------------: | -------------: |
| hsluv                 | 1280x720   |   0.052 |  0.052 (1.00x) |  0.052 (1.00x) |  0.058 (1.12x) |  0.054 (0.93x) |
| hsluv                 | 1920x1080  |   0.121 |  0.122 (1.01x) |  0.123 (1.02x) |  0.129 (1.07x) |  0.119 (0.92x) |
| hsluv                 | 3840x2160  |   0.493 |  0.498 (1.01x) |  0.498 (1.01x) |  0.509 (1.03x) |  0.472 (0.93x) |
| hsluv_components      | 1280x720   |   0.025 |  0.027 (1.08x) |  0.026 (1.04x) |  0.032 (1.31x) |  0.031 (0.95x) |
| hsluv_components      | 1920x1080  |   0.060 |  0.065 (1.07x) |  0.063 (1.05x) |  0.070 (1.15x) |  0.066 (0.95x) |
| hsluv_components      | 3840x2160  |   0.254 |  0.270 (1.06x) |  0.263 (1.04x) |  0.271 (1.07x) |  0.257 (0.95x) |
| mrt                   | 1280x720   |   0.125 |  0.126 (1.01x) |  0.126 (1.01x) |  0.131 (1.05x) |  0.131 (1.00x) |
| mrt                   | 1920x1080  |   0.291 |  0.293 (1.01x) |  0.296 (1.02x) |  0.303 (1.04x) |  0.301 (0.99x) |
| mrt                   | 3840x2160  |   1.238 |  1.246 (1.01x) |  1.237 (1.00x) |  1.256 (1.01x) |  1.235 (0.98x) |
| noise                 | 1280x720   |   1.193 |  1.191 (1.00x) |  1.190 (1.00x) |  1.119 (0.94x) |  1.122 (1.00x) |
| noise                 | 1920x1080  |   2.689 |  2.672 (0.99x) |  2.673 (0.99x) |  2.514 (0.93x) |  2.501 (0.99x) |
| noise                 | 3840x2160  |  10.866 | 10.782 (0.99x) | 10.788 (0.99x) | 10.085 (0.93x) | 10.079 (1.00x) |
| postprocess           | 1280x720   |   0.038 |  0.038 (1.00x) |  0.038 (1.00x) |  0.043 (1.13x) |  0.043 (0.99x) |
| postprocess           | 1920x1080  |   0.088 |  0.086 (0.98x) |  0.086 (0.98x) |  0.111 (1.26x) |  0.110 (1.00x) |
| postprocess           | 3840x2160  |   0.503 |  0.506 (1.01x) |  0.505 (1.00x) |  0.552 (1.10x) |  0.553 (1.00x) |
| struct_folding        | 1280x720   |   0.043 |  0.040 (0.93x) |  0.043 (1.00x) |  0.044 (1.02x) |  0.043 (0.99x) |
| struct_folding        | 1920x1080  |   0.100 |  0.094 (0.94x) |  0.099 (0.99x) |  0.096 (0.96x) |  0.095 (0.99x) |
| struct_folding        | 3840x2160  |   0.412 |  0.390 (0.95x) |  0.415 (1.01x) |  0.381 (0.93x) |  0.377 (0.99x) |
| struct_folding_inline | 1280x720   |   0.043 |  0.040 (0.93x) |  0.043 (1.00x) |  0.044 (1.02x) |  0.043 (0.99x) |
| struct_folding_inline | 1920x1080  |   0.099 |  0.095 (0.96x) |  0.100 (1.01x) |  0.096 (0.96x) |  0.095 (0.99x) |
| struct_folding_inline | 3840x2160  |   0.413 |  0.390 (0.95x) |  0.413 (1.00x) |  0.381 (0.92x) |  0.379 (0.99x) |
| structs               | 1280x720   |   0.105 |  0.100 (0.95x) |  0.121 (1.15x) |  0.099 (0.94x) |  0.099 (0.99x) |
| structs               | 1920x1080  |   0.239 |  0.229 (0.96x) |  0.273 (1.15x) |  0.217 (0.91x) |  0.216 (0.99x) |
| structs               | 3840x2160  |   0.952 |  0.921 (0.97x) |  1.081 (1.14x) |  0.847 (0.89x) |  0.841 (0.99x) |
| vertex_heavy          | 1280x720   |   0.515 |  0.515 (1.00x) |  0.555 (1.08x) |  0.461 (0.90x) |  0.453 (0.98x) |
| vertex_heavy          | 1920x1080  |   0.549 |  0.548 (1.00x) |  0.594 (1.08x) |  0.504 (0.92x) |  0.490 (0.97x) |
| vertex_heavy          | 3840x2160  |   0.624 |  0.617 (0.99x) |  0.654 (1.05x) |  0.579 (0.93x) |  0.574 (0.99x) |

GL direct generated the same frag code as GL bwsl (fell back to SPIRV-Cross) for: noise, postprocess

Output vs GL hand (max abs difference; share of channel values over the tolerance):

| Test                  | GL bwsl     | GL direct   | VK hand      | VK bwsl     |
| --------------------- | ----------- | ----------- | ------------ | ----------- |
| hsluv                 | ok (0.0039) | ok (0.0039) | ok (0.0039)  | ok (0.0039) |
| hsluv_components      | ok (0.0039) | ok (0.0039) | ok (0.0039)  | ok (0.0039) |
| mrt                   | ok (0.0044) | ok (0.0044) | ok (0.00049) | ok (0.0044) |
| noise                 | ok (0)      | ok (0)      | ok (0.0039)  | ok (0.0039) |
| postprocess           | ok (0.0039) | ok (0.0039) | ok (0.0039)  | ok (0.0039) |
| struct_folding        | ok (0)      | ok (0)      | ok (0.0039)  | ok (0.0039) |
| struct_folding_inline | ok (0)      | ok (0)      | ok (0.0039)  | ok (0.0039) |
| structs               | ok (0)      | ok (0)      | ok (0.38)    | ok (0.38)   |
| vertex_heavy          | ok (0)      | ok (0)      | ok (0.0039)  | ok (0.0039) |
