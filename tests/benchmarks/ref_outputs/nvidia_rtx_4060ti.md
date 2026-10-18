 
 ```
 > python tests/benchmarks/run.py --rounds 2 --skip-after 20
[hsluv] compiling
[hsluv] 1280x720: gl-hand 0.052, gl-bwsl 0.052, gl-direct 0.106, vk-hand 0.058, vk-bwsl 0.054 (4.8s)
[hsluv] 1920x1080: gl-hand 0.121, gl-bwsl 0.122, gl-direct 0.244, vk-hand 0.129, vk-bwsl 0.119 (4.7s)
[hsluv] 3840x2160: gl-hand 0.494, gl-bwsl 0.499, gl-direct 0.987, vk-hand 0.509, vk-bwsl 0.477 (5.2s)
[hsluv_components] compiling
[hsluv_components] 1280x720: gl-hand 0.025, gl-bwsl 0.027, gl-direct 0.026, vk-hand 0.032, vk-bwsl 0.031 (4.8s)
[hsluv_components] 1920x1080: gl-hand 0.060, gl-bwsl 0.065, gl-direct 0.063, vk-hand 0.070, vk-bwsl 0.066 (4.5s)
[hsluv_components] 3840x2160: gl-hand 0.254, gl-bwsl 0.271, gl-direct 0.266, vk-hand 0.271, vk-bwsl 0.257 (4.9s)
[mrt] compiling
[mrt] 1280x720: gl-hand 0.127, gl-bwsl 0.127, gl-direct 0.128, vk-hand 0.133, vk-bwsl 0.133 (8.0s)
[mrt] 1920x1080: gl-hand 0.293, gl-bwsl 0.295, gl-direct 0.296, vk-hand 0.302, vk-bwsl 0.302 (5.1s)
[mrt] 3840x2160: gl-hand 1.247, gl-bwsl 1.246, gl-direct 1.235, vk-hand 1.274, vk-bwsl 1.267 (6.1s)
[noise] compiling
[noise] 1280x720: gl-hand 1.193, gl-bwsl 1.190, gl-direct 1.208, vk-hand 1.119, vk-bwsl 1.116 (6.1s)
[noise] 1920x1080: gl-hand 2.688, gl-bwsl 2.670, gl-direct 2.708, vk-hand 2.501, vk-bwsl 2.500 (7.5s)
[noise] 3840x2160: gl-hand 10.857, gl-bwsl 10.786, gl-direct 10.926, vk-hand 10.086, vk-bwsl 10.078 (16.0s)
[postprocess] compiling
[postprocess] 1280x720: gl-hand 0.038, gl-bwsl 0.038, gl-direct 0.040, vk-hand 0.044, vk-bwsl 0.043 (4.9s)
[postprocess] 1920x1080: gl-hand 0.087, gl-bwsl 0.086, gl-direct 0.092, vk-hand 0.111, vk-bwsl 0.111 (4.9s)
[postprocess] 3840x2160: gl-hand 0.503, gl-bwsl 0.504, gl-direct 0.512, vk-hand 0.552, vk-bwsl 0.564 (5.9s)
[struct_folding] compiling
[struct_folding] 1280x720: gl-hand 0.043, gl-bwsl 0.040, gl-direct 0.043, vk-hand 0.044, vk-bwsl 0.044 (4.9s)
[struct_folding] 1920x1080: gl-hand 0.100, gl-bwsl 0.095, gl-direct 0.099, vk-hand 0.096, vk-bwsl 0.096 (4.7s)
[struct_folding] 3840x2160: gl-hand 0.414, gl-bwsl 0.389, gl-direct 0.413, vk-hand 0.381, vk-bwsl 0.382 (5.0s)
[struct_folding_inline] compiling
[struct_folding_inline] 1280x720: gl-hand 0.042, gl-bwsl 0.040, gl-direct 0.043, vk-hand 0.044, vk-bwsl 0.044 (4.8s)
[struct_folding_inline] 1920x1080: gl-hand 0.099, gl-bwsl 0.094, gl-direct 0.099, vk-hand 0.096, vk-bwsl 0.096 (4.7s)
[struct_folding_inline] 3840x2160: gl-hand 0.414, gl-bwsl 0.391, gl-direct 0.410, vk-hand 0.383, vk-bwsl 0.382 (5.0s)
[structs] compiling
[structs] 1280x720: gl-hand 0.105, gl-bwsl 0.105, gl-direct 0.141, vk-hand 0.099, vk-bwsl 0.096 (4.9s)
[structs] 1920x1080: gl-hand 0.239, gl-bwsl 0.239, gl-direct 0.317, vk-hand 0.217, vk-bwsl 0.210 (4.8s)
[structs] 3840x2160: gl-hand 0.952, gl-bwsl 0.956, gl-direct 1.261, vk-hand 0.847, vk-bwsl 0.820 (5.6s)
[vertex_heavy] compiling
[vertex_heavy] 1280x720: gl-hand 0.512, gl-bwsl 0.513, gl-direct 0.556, vk-hand 0.458, vk-bwsl 0.455 (5.8s)
[vertex_heavy] 1920x1080: gl-hand 0.548, gl-bwsl 0.547, gl-direct 0.594, vk-hand 0.504, vk-bwsl 0.489 (5.6s)
[vertex_heavy] 3840x2160: gl-hand 0.618, gl-bwsl 0.615, gl-direct 0.657, vk-hand 0.578, vk-bwsl 0.572 (5.6s)
```

## BWSL GPU benchmarks

- OpenGL: NVIDIA GeForce RTX 4060 Ti/PCIe/SSE2 (4.6.0 NVIDIA 617.14)
- Vulkan: NVIDIA GeForce RTX 4060 Ti (NVIDIA 617.14)
- bwslc: build\bwslc.exe, built 2026-10-18 21:12, source 862a618
- GPU ms per frame, median of up to 100 timed frames (x2 rounds)
- Ratios compare against the hand-written shader on the same API (VK hand against GL hand, i.e. the API's own cost)

| Test                  | Resolution | GL hand |        GL bwsl |      GL direct |        VK hand |        VK bwsl |
| --------------------- | ---------- | ------: | -------------: | -------------: | -------------: | -------------: |
| hsluv                 | 1280x720   |   0.052 |  0.052 (1.00x) |  0.106 (2.04x) |  0.058 (1.12x) |  0.054 (0.93x) |
| hsluv                 | 1920x1080  |   0.121 |  0.122 (1.01x) |  0.244 (2.02x) |  0.129 (1.07x) |  0.119 (0.92x) |
| hsluv                 | 3840x2160  |   0.494 |  0.499 (1.01x) |  0.987 (2.00x) |  0.509 (1.03x) |  0.477 (0.94x) |
| hsluv_components      | 1280x720   |   0.025 |  0.027 (1.08x) |  0.026 (1.04x) |  0.032 (1.31x) |  0.031 (0.95x) |
| hsluv_components      | 1920x1080  |   0.060 |  0.065 (1.07x) |  0.063 (1.05x) |  0.070 (1.15x) |  0.066 (0.95x) |
| hsluv_components      | 3840x2160  |   0.254 |  0.271 (1.07x) |  0.266 (1.05x) |  0.271 (1.07x) |  0.257 (0.95x) |
| mrt                   | 1280x720   |   0.127 |  0.127 (1.00x) |  0.128 (1.01x) |  0.133 (1.05x) |  0.133 (0.99x) |
| mrt                   | 1920x1080  |   0.293 |  0.295 (1.01x) |  0.296 (1.01x) |  0.302 (1.03x) |  0.302 (1.00x) |
| mrt                   | 3840x2160  |   1.247 |  1.246 (1.00x) |  1.235 (0.99x) |  1.274 (1.02x) |  1.267 (0.99x) |
| noise                 | 1280x720   |   1.193 |  1.190 (1.00x) |  1.208 (1.01x) |  1.119 (0.94x) |  1.116 (1.00x) |
| noise                 | 1920x1080  |   2.688 |  2.670 (0.99x) |  2.708 (1.01x) |  2.501 (0.93x) |  2.500 (1.00x) |
| noise                 | 3840x2160  |  10.857 | 10.786 (0.99x) | 10.926 (1.01x) | 10.086 (0.93x) | 10.078 (1.00x) |
| postprocess           | 1280x720   |   0.038 |  0.038 (1.00x) |  0.040 (1.05x) |  0.044 (1.15x) |  0.043 (0.99x) |
| postprocess           | 1920x1080  |   0.087 |  0.086 (0.99x) |  0.092 (1.06x) |  0.111 (1.28x) |  0.111 (1.00x) |
| postprocess           | 3840x2160  |   0.503 |  0.504 (1.00x) |  0.512 (1.02x) |  0.552 (1.10x) |  0.564 (1.02x) |
| struct_folding        | 1280x720   |   0.043 |  0.040 (0.93x) |  0.043 (1.00x) |  0.044 (1.02x) |  0.044 (1.00x) |
| struct_folding        | 1920x1080  |   0.100 |  0.095 (0.95x) |  0.099 (0.99x) |  0.096 (0.96x) |  0.096 (1.00x) |
| struct_folding        | 3840x2160  |   0.414 |  0.389 (0.94x) |  0.413 (1.00x) |  0.381 (0.92x) |  0.382 (1.00x) |
| struct_folding_inline | 1280x720   |   0.042 |  0.040 (0.95x) |  0.043 (1.02x) |  0.044 (1.04x) |  0.044 (1.00x) |
| struct_folding_inline | 1920x1080  |   0.099 |  0.094 (0.95x) |  0.099 (1.00x) |  0.096 (0.96x) |  0.096 (1.00x) |
| struct_folding_inline | 3840x2160  |   0.414 |  0.391 (0.95x) |  0.410 (0.99x) |  0.383 (0.93x) |  0.382 (1.00x) |
| structs               | 1280x720   |   0.105 |  0.105 (1.00x) |  0.141 (1.34x) |  0.099 (0.94x) |  0.096 (0.97x) |
| structs               | 1920x1080  |   0.239 |  0.239 (1.00x) |  0.317 (1.33x) |  0.217 (0.91x) |  0.210 (0.97x) |
| structs               | 3840x2160  |   0.952 |  0.956 (1.00x) |  1.261 (1.32x) |  0.847 (0.89x) |  0.820 (0.97x) |
| vertex_heavy          | 1280x720   |   0.512 |  0.513 (1.00x) |  0.556 (1.09x) |  0.458 (0.90x) |  0.455 (0.99x) |
| vertex_heavy          | 1920x1080  |   0.548 |  0.547 (1.00x) |  0.594 (1.08x) |  0.504 (0.92x) |  0.489 (0.97x) |
| vertex_heavy          | 3840x2160  |   0.618 |  0.615 (1.00x) |  0.657 (1.06x) |  0.578 (0.93x) |  0.572 (0.99x) |

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
