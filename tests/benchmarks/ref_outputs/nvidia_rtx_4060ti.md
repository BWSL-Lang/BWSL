 
 ```
 > python tests/benchmarks/run.py --rounds 2 --skip-after 20
[hsluv] compiling
[hsluv] 1280x720: gl-hand 0.052, gl-bwsl 0.052, gl-direct 0.106, vk-hand 0.058, vk-bwsl 0.054 (4.9s)
[hsluv] 1920x1080: gl-hand 0.121, gl-bwsl 0.122, gl-direct 0.245, vk-hand 0.129, vk-bwsl 0.119 (4.7s)
[hsluv] 3840x2160: gl-hand 0.495, gl-bwsl 0.499, gl-direct 0.987, vk-hand 0.509, vk-bwsl 0.476 (5.2s)
[hsluv_components] compiling
[hsluv_components] 1280x720: gl-hand 0.025, gl-bwsl 0.027, gl-direct 0.026, vk-hand 0.032, vk-bwsl 0.031 (4.8s)
[hsluv_components] 1920x1080: gl-hand 0.060, gl-bwsl 0.065, gl-direct 0.063, vk-hand 0.070, vk-bwsl 0.066 (4.6s)
[hsluv_components] 3840x2160: gl-hand 0.254, gl-bwsl 0.271, gl-direct 0.265, vk-hand 0.271, vk-bwsl 0.257 (4.9s)
[mrt] compiling
[mrt] 1280x720: gl-hand 0.127, gl-bwsl 0.127, gl-direct 0.127, vk-hand 0.134, vk-bwsl 0.133 (8.0s)
[mrt] 1920x1080: gl-hand 0.294, gl-bwsl 0.294, gl-direct 0.297, vk-hand 0.302, vk-bwsl 0.302 (5.0s)
[mrt] 3840x2160: gl-hand 1.240, gl-bwsl 1.261, gl-direct 1.247, vk-hand 1.278, vk-bwsl 1.264 (6.0s)
[noise] compiling
[noise] 1280x720: gl-hand 1.192, gl-bwsl 1.190, gl-direct 1.205, vk-hand 1.117, vk-bwsl 1.115 (6.0s)
[noise] 1920x1080: gl-hand 2.678, gl-bwsl 2.669, gl-direct 2.708, vk-hand 2.501, vk-bwsl 2.501 (7.5s)
[noise] 3840x2160: gl-hand 10.811, gl-bwsl 10.789, gl-direct 10.941, vk-hand 10.085, vk-bwsl 10.105 (15.9s)
[postprocess] compiling
[postprocess] 1280x720: gl-hand 0.038, gl-bwsl 0.038, gl-direct 0.040, vk-hand 0.043, vk-bwsl 0.042 (4.9s)
[postprocess] 1920x1080: gl-hand 0.086, gl-bwsl 0.084, gl-direct 0.090, vk-hand 0.107, vk-bwsl 0.108 (4.9s)
[postprocess] 3840x2160: gl-hand 0.502, gl-bwsl 0.503, gl-direct 0.510, vk-hand 0.551, vk-bwsl 0.553 (5.9s)
[structs] compiling
[structs] 1280x720: gl-hand 0.104, gl-bwsl 0.115, gl-direct 0.142, vk-hand 0.099, vk-bwsl 0.096 (4.8s)
[structs] 1920x1080: gl-hand 0.239, gl-bwsl 0.261, gl-direct 0.318, vk-hand 0.217, vk-bwsl 0.210 (4.8s)
[structs] 3840x2160: gl-hand 0.950, gl-bwsl 1.039, gl-direct 1.267, vk-hand 0.847, vk-bwsl 0.820 (5.7s)
[vertex_heavy] compiling
[vertex_heavy] 1280x720: gl-hand 0.514, gl-bwsl 0.513, gl-direct 0.557, vk-hand 0.459, vk-bwsl 0.461 (5.8s)
[vertex_heavy] 1920x1080: gl-hand 0.545, gl-bwsl 0.547, gl-direct 0.593, vk-hand 0.500, vk-bwsl 0.487 (5.6s)
[vertex_heavy] 3840x2160: gl-hand 0.616, gl-bwsl 0.613, gl-direct 0.656, vk-hand 0.580, vk-bwsl 0.572 (5.7s)
```

## BWSL GPU benchmarks

- OpenGL: NVIDIA GeForce RTX 4060 Ti/PCIe/SSE2 (4.6.0 NVIDIA 617.14)
- Vulkan: NVIDIA GeForce RTX 4060 Ti (NVIDIA 617.14)
- bwslc: build\bwslc.exe, built 2026-10-18 19:57, source e7169c6-dirty
- GPU ms per frame, median of up to 100 timed frames (x2 rounds)
- Ratios compare against the hand-written shader on the same API (VK hand against GL hand, i.e. the API's own cost)

| Test             | Resolution | GL hand |        GL bwsl |      GL direct |        VK hand |        VK bwsl |
| ---------------- | ---------- | ------: | -------------: | -------------: | -------------: | -------------: |
| hsluv            | 1280x720   |   0.052 |  0.052 (1.00x) |  0.106 (2.04x) |  0.058 (1.12x) |  0.054 (0.93x) |
| hsluv            | 1920x1080  |   0.121 |  0.122 (1.01x) |  0.245 (2.03x) |  0.129 (1.07x) |  0.119 (0.92x) |
| hsluv            | 3840x2160  |   0.495 |  0.499 (1.01x) |  0.987 (2.00x) |  0.509 (1.03x) |  0.476 (0.94x) |
| hsluv_components | 1280x720   |   0.025 |  0.027 (1.08x) |  0.026 (1.04x) |  0.032 (1.31x) |  0.031 (0.95x) |
| hsluv_components | 1920x1080  |   0.060 |  0.065 (1.07x) |  0.063 (1.05x) |  0.070 (1.15x) |  0.066 (0.95x) |
| hsluv_components | 3840x2160  |   0.254 |  0.271 (1.07x) |  0.265 (1.04x) |  0.271 (1.07x) |  0.257 (0.95x) |
| mrt              | 1280x720   |   0.127 |  0.127 (1.00x) |  0.127 (1.00x) |  0.134 (1.05x) |  0.133 (0.99x) |
| mrt              | 1920x1080  |   0.294 |  0.294 (1.00x) |  0.297 (1.01x) |  0.302 (1.03x) |  0.302 (1.00x) |
| mrt              | 3840x2160  |   1.240 |  1.261 (1.02x) |  1.247 (1.01x) |  1.278 (1.03x) |  1.264 (0.99x) |
| noise            | 1280x720   |   1.192 |  1.190 (1.00x) |  1.205 (1.01x) |  1.117 (0.94x) |  1.115 (1.00x) |
| noise            | 1920x1080  |   2.678 |  2.669 (1.00x) |  2.708 (1.01x) |  2.501 (0.93x) |  2.501 (1.00x) |
| noise            | 3840x2160  |  10.811 | 10.789 (1.00x) | 10.941 (1.01x) | 10.085 (0.93x) | 10.105 (1.00x) |
| postprocess      | 1280x720   |   0.038 |  0.038 (1.00x) |  0.040 (1.05x) |  0.043 (1.14x) |  0.042 (0.97x) |
| postprocess      | 1920x1080  |   0.086 |  0.084 (0.98x) |  0.090 (1.05x) |  0.107 (1.25x) |  0.108 (1.00x) |
| postprocess      | 3840x2160  |   0.502 |  0.503 (1.00x) |  0.510 (1.02x) |  0.551 (1.10x) |  0.553 (1.00x) |
| structs          | 1280x720   |   0.104 |  0.115 (1.10x) |  0.142 (1.36x) |  0.099 (0.95x) |  0.096 (0.97x) |
| structs          | 1920x1080  |   0.239 |  0.261 (1.09x) |  0.318 (1.33x) |  0.217 (0.91x) |  0.210 (0.97x) |
| structs          | 3840x2160  |   0.950 |  1.039 (1.09x) |  1.267 (1.33x) |  0.847 (0.89x) |  0.820 (0.97x) |
| vertex_heavy     | 1280x720   |   0.514 |  0.513 (1.00x) |  0.557 (1.08x) |  0.459 (0.89x) |  0.461 (1.00x) |
| vertex_heavy     | 1920x1080  |   0.545 |  0.547 (1.00x) |  0.593 (1.09x) |  0.500 (0.92x) |  0.487 (0.97x) |
| vertex_heavy     | 3840x2160  |   0.616 |  0.613 (1.00x) |  0.656 (1.06x) |  0.580 (0.94x) |  0.572 (0.99x) |

Output vs GL hand (max abs difference; share of channel values over the tolerance):

| Test             | GL bwsl     | GL direct   | VK hand      | VK bwsl     |
| ---------------- | ----------- | ----------- | ------------ | ----------- |
| hsluv            | ok (0.0039) | ok (0.0039) | ok (0.0039)  | ok (0.0039) |
| hsluv_components | ok (0.0039) | ok (0.0039) | ok (0.0039)  | ok (0.0039) |
| mrt              | ok (0.0044) | ok (0.0044) | ok (0.00049) | ok (0.0044) |
| noise            | ok (0)      | ok (0)      | ok (0.0039)  | ok (0.0039) |
| postprocess      | ok (0.0039) | ok (0.0039) | ok (0.0039)  | ok (0.0039) |
| structs          | ok (0)      | ok (0)      | ok (0.38)    | ok (0.38)   |
| vertex_heavy     | ok (0)      | ok (0)      | ok (0.0039)  | ok (0.0039) |
