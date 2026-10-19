 
 ```
 > python tests/benchmarks/run.py
[hsluv] compiling
[hsluv] 1280x720: gl-hand 0.054, gl-bwsl 0.052, gl-direct 0.106, vk-hand 0.058, vk-bwsl 0.054 (3.0s)
[hsluv] 1920x1080: gl-hand 0.121, gl-bwsl 0.121, gl-direct 0.244, vk-hand 0.128, vk-bwsl 0.119 (2.5s)
[hsluv] 3840x2160: gl-hand 0.493, gl-bwsl 0.496, gl-direct 0.982, vk-hand 0.509, vk-bwsl 0.467 (2.6s)
[hsluv_components] compiling
[hsluv_components] 1280x720: gl-hand 0.025, gl-bwsl 0.027, gl-direct 0.026, vk-hand 0.032, vk-bwsl 0.031 (2.6s)
[hsluv_components] 1920x1080: gl-hand 0.060, gl-bwsl 0.065, gl-direct 0.062, vk-hand 0.070, vk-bwsl 0.066 (2.3s)
[hsluv_components] 3840x2160: gl-hand 0.254, gl-bwsl 0.269, gl-direct 0.267, vk-hand 0.271, vk-bwsl 0.257 (2.4s)
[mrt] compiling
[mrt] 1280x720: gl-hand 0.126, gl-bwsl 0.127, gl-direct 0.128, vk-hand 0.132, vk-bwsl 0.132 (5.7s)
[mrt] 1920x1080: gl-hand 0.294, gl-bwsl 0.295, gl-direct 0.296, vk-hand 0.303, vk-bwsl 0.302 (2.6s)
[mrt] 3840x2160: gl-hand 1.254, gl-bwsl 1.257, gl-direct 1.245, vk-hand 1.277, vk-bwsl 1.268 (3.0s)
[noise] compiling
[noise] 1280x720: gl-hand 1.193, gl-bwsl 1.190, gl-direct 1.191, vk-hand 1.120, vk-bwsl 1.113 (3.3s)
[noise] 1920x1080: gl-hand 2.675, gl-bwsl 2.670, gl-direct 2.670, vk-hand 2.502, vk-bwsl 2.500 (3.6s)
[noise] 3840x2160: gl-hand 10.693, gl-bwsl 10.673, gl-direct 10.699, vk-hand 10.034, vk-bwsl 10.028 (7.8s)
[postprocess] compiling
[postprocess] 1280x720: gl-hand 0.039, gl-bwsl 0.038, gl-direct 0.038, vk-hand 0.044, vk-bwsl 0.043 (2.6s)
[postprocess] 1920x1080: gl-hand 0.087, gl-bwsl 0.086, gl-direct 0.086, vk-hand 0.109, vk-bwsl 0.110 (2.5s)
[postprocess] 3840x2160: gl-hand 0.501, gl-bwsl 0.504, gl-direct 0.504, vk-hand 0.549, vk-bwsl 0.550 (3.0s)
[vertex_heavy] compiling
[vertex_heavy] 1280x720: gl-hand 0.514, gl-bwsl 0.514, gl-direct 0.556, vk-hand 0.461, vk-bwsl 0.461 (3.2s)
[vertex_heavy] 1920x1080: gl-hand 0.549, gl-bwsl 0.548, gl-direct 0.592, vk-hand 0.502, vk-bwsl 0.491 (2.9s)
[vertex_heavy] 3840x2160: gl-hand 0.616, gl-bwsl 0.617, gl-direct 0.658, vk-hand 0.575, vk-bwsl 0.574 (2.9s)
```

## BWSL GPU benchmarks

- OpenGL: NVIDIA GeForce RTX 4060 Ti/PCIe/SSE2 (4.6.0 NVIDIA 617.14)
- Vulkan: NVIDIA GeForce RTX 4060 Ti (NVIDIA 617.14)
- bwslc: build\bwslc.exe, built 2026-10-17 11:02, source 1532568
- GPU ms per frame, median of up to 100 timed frames (x1 rounds)
- Ratios compare against the hand-written shader on the same API (VK hand against GL hand, i.e. the API's own cost)

| Test             | Resolution | GL hand |        GL bwsl |      GL direct |        VK hand |        VK bwsl |
| ---------------- | ---------- | ------: | -------------: | -------------: | -------------: | -------------: |
| hsluv            | 1280x720   |   0.054 |  0.052 (0.96x) |  0.106 (1.96x) |  0.058 (1.07x) |  0.054 (0.93x) |
| hsluv            | 1920x1080  |   0.121 |  0.121 (1.00x) |  0.244 (2.02x) |  0.128 (1.06x) |  0.119 (0.92x) |
| hsluv            | 3840x2160  |   0.493 |  0.496 (1.01x) |  0.982 (1.99x) |  0.509 (1.03x) |  0.467 (0.92x) |
| hsluv_components | 1280x720   |   0.025 |  0.027 (1.08x) |  0.026 (1.04x) |  0.032 (1.31x) |  0.031 (0.95x) |
| hsluv_components | 1920x1080  |   0.060 |  0.065 (1.07x) |  0.062 (1.03x) |  0.070 (1.15x) |  0.066 (0.95x) |
| hsluv_components | 3840x2160  |   0.254 |  0.269 (1.06x) |  0.267 (1.05x) |  0.271 (1.07x) |  0.257 (0.95x) |
| mrt              | 1280x720   |   0.126 |  0.127 (1.01x) |  0.128 (1.02x) |  0.132 (1.05x) |  0.132 (1.00x) |
| mrt              | 1920x1080  |   0.294 |  0.295 (1.00x) |  0.296 (1.01x) |  0.303 (1.03x) |  0.302 (0.99x) |
| mrt              | 3840x2160  |   1.254 |  1.257 (1.00x) |  1.245 (0.99x) |  1.277 (1.02x) |  1.268 (0.99x) |
| noise            | 1280x720   |   1.193 |  1.190 (1.00x) |  1.191 (1.00x) |  1.120 (0.94x) |  1.113 (0.99x) |
| noise            | 1920x1080  |   2.675 |  2.670 (1.00x) |  2.670 (1.00x) |  2.502 (0.94x) |  2.500 (1.00x) |
| noise            | 3840x2160  |  10.693 | 10.673 (1.00x) | 10.699 (1.00x) | 10.034 (0.94x) | 10.028 (1.00x) |
| postprocess      | 1280x720   |   0.039 |  0.038 (0.97x) |  0.038 (0.97x) |  0.044 (1.12x) |  0.043 (0.99x) |
| postprocess      | 1920x1080  |   0.087 |  0.086 (0.99x) |  0.086 (0.99x) |  0.109 (1.26x) |  0.110 (1.01x) |
| postprocess      | 3840x2160  |   0.501 |  0.504 (1.01x) |  0.504 (1.01x) |  0.549 (1.10x) |  0.550 (1.00x) |
| vertex_heavy     | 1280x720   |   0.514 |  0.514 (1.00x) |  0.556 (1.08x) |  0.461 (0.90x) |  0.461 (1.00x) |
| vertex_heavy     | 1920x1080  |   0.549 |  0.548 (1.00x) |  0.592 (1.08x) |  0.502 (0.91x) |  0.491 (0.98x) |
| vertex_heavy     | 3840x2160  |   0.616 |  0.617 (1.00x) |  0.658 (1.07x) |  0.575 (0.93x) |  0.574 (1.00x) |

GL direct generated the same frag code as GL bwsl (fell back to SPIRV-Cross) for: noise, postprocess

Output vs GL hand (max abs difference; share of channel values over the tolerance):

| Test             | GL bwsl     | GL direct   | VK hand      | VK bwsl     |
| ---------------- | ----------- | ----------- | ------------ | ----------- |
| hsluv            | ok (0.0039) | ok (0.0039) | ok (0.0039)  | ok (0.0039) |
| hsluv_components | ok (0.0039) | ok (0.0039) | ok (0.0039)  | ok (0.0039) |
| mrt              | ok (0.0044) | ok (0.0044) | ok (0.00049) | ok (0.0044) |
| noise            | ok (0)      | ok (0)      | ok (0.0039)  | ok (0.0039) |
| postprocess      | ok (0.0039) | ok (0.0039) | ok (0.0039)  | ok (0.0039) |
| vertex_heavy     | ok (0)      | ok (0)      | ok (0.0039)  | ok (0.0039) |