 
 ```
 > python tests/benchmarks/run.py
[hsluv] compiling
[hsluv] 1280x720: gl-hand 0.051, gl-bwsl 0.052, gl-direct 0.103, vk-hand 0.059, vk-bwsl 0.054 (3.0s)
[hsluv] 1920x1080: gl-hand 0.121, gl-bwsl 0.123, gl-direct 0.238, vk-hand 0.129, vk-bwsl 0.119 (2.4s)
[hsluv] 3840x2160: gl-hand 0.493, gl-bwsl 0.502, gl-direct 0.955, vk-hand 0.509, vk-bwsl 0.475 (2.6s)
[hsluv_components] compiling
[hsluv_components] 1280x720: gl-hand 0.025, gl-bwsl 0.027, gl-direct 0.027, vk-hand 0.032, vk-bwsl 0.030 (2.7s)
[hsluv_components] 1920x1080: gl-hand 0.060, gl-bwsl 0.065, gl-direct 0.065, vk-hand 0.070, vk-bwsl 0.065 (2.3s)
[hsluv_components] 3840x2160: gl-hand 0.254, gl-bwsl 0.267, gl-direct 0.272, vk-hand 0.271, vk-bwsl 0.252 (2.4s)
[mrt] compiling
[mrt] 1280x720: gl-hand 0.128, gl-bwsl 0.128, gl-direct 0.128, vk-hand 0.133, vk-bwsl 0.133 (5.7s)
[mrt] 1920x1080: gl-hand 0.293, gl-bwsl 0.298, gl-direct 0.296, vk-hand 0.303, vk-bwsl 0.300 (2.5s)
[mrt] 3840x2160: gl-hand 1.251, gl-bwsl 1.257, gl-direct 1.229, vk-hand 1.291, vk-bwsl 1.274 (3.0s)
[noise] compiling
[noise] 1280x720: gl-hand 1.193, gl-bwsl 1.187, gl-direct 1.187, vk-hand 1.113, vk-bwsl 1.113 (3.2s)
[noise] 1920x1080: gl-hand 2.674, gl-bwsl 2.662, gl-direct 2.659, vk-hand 2.500, vk-bwsl 2.500 (3.7s)
[noise] 3840x2160: gl-hand 10.750, gl-bwsl 10.636, gl-direct 10.690, vk-hand 10.034, vk-bwsl 9.975 (7.8s)
[postprocess] compiling
[postprocess] 1280x720: gl-hand 0.038, gl-bwsl 0.038, gl-direct 0.038, vk-hand 0.043, vk-bwsl 0.043 (2.6s)
[postprocess] 1920x1080: gl-hand 0.087, gl-bwsl 0.086, gl-direct 0.086, vk-hand 0.109, vk-bwsl 0.109 (2.4s)
[postprocess] 3840x2160: gl-hand 0.504, gl-bwsl 0.504, gl-direct 0.505, vk-hand 0.552, vk-bwsl 0.554 (3.0s)
[vertex_heavy] compiling
[vertex_heavy] 1280x720: gl-hand 0.514, gl-bwsl 0.509, gl-direct 0.556, vk-hand 0.461, vk-bwsl 0.455 (3.1s)
[vertex_heavy] 1920x1080: gl-hand 0.548, gl-bwsl 0.542, gl-direct 0.595, vk-hand 0.502, vk-bwsl 0.488 (2.7s)
[vertex_heavy] 3840x2160: gl-hand 0.622, gl-bwsl 0.606, gl-direct 0.656, vk-hand 0.578, vk-bwsl 0.578 (2.9s)
```

## BWSL GPU benchmarks


- OpenGL: NVIDIA GeForce RTX 4060 Ti/PCIe/SSE2 (4.6.0 NVIDIA 617.14)
- Vulkan: NVIDIA GeForce RTX 4060 Ti (NVIDIA 617.14)
- bwslc: build\bwslc.exe, built 2026-10-17 09:34, source 67a3e83
- GPU ms per frame, median of up to 100 timed frames (x1 rounds)
- Ratios compare against the hand-written shader on the same API (VK hand against GL hand, i.e. the API's own cost)

| Test             | Resolution | GL hand |        GL bwsl |      GL direct |        VK hand |       VK bwsl |
| ---------------- | ---------- | ------: | -------------: | -------------: | -------------: | ------------: |
| hsluv            | 1280x720   |   0.051 |  0.052 (1.02x) |  0.103 (2.02x) |  0.059 (1.14x) | 0.054 (0.93x) |
| hsluv            | 1920x1080  |   0.121 |  0.123 (1.02x) |  0.238 (1.97x) |  0.129 (1.07x) | 0.119 (0.92x) |
| hsluv            | 3840x2160  |   0.493 |  0.502 (1.02x) |  0.955 (1.94x) |  0.509 (1.03x) | 0.475 (0.93x) |
| hsluv_components | 1280x720   |   0.025 |  0.027 (1.08x) |  0.027 (1.08x) |  0.032 (1.30x) | 0.030 (0.93x) |
| hsluv_components | 1920x1080  |   0.060 |  0.065 (1.07x) |  0.065 (1.07x) |  0.070 (1.15x) | 0.065 (0.93x) |
| hsluv_components | 3840x2160  |   0.254 |  0.267 (1.05x) |  0.272 (1.07x) |  0.271 (1.07x) | 0.252 (0.93x) |
| mrt              | 1280x720   |   0.128 |  0.128 (1.00x) |  0.128 (1.00x) |  0.133 (1.04x) | 0.133 (1.00x) |
| mrt              | 1920x1080  |   0.293 |  0.298 (1.02x) |  0.296 (1.01x) |  0.303 (1.03x) | 0.300 (0.99x) |
| mrt              | 3840x2160  |   1.251 |  1.257 (1.00x) |  1.229 (0.98x) |  1.291 (1.03x) | 1.274 (0.99x) |
| noise            | 1280x720   |   1.193 |  1.187 (0.99x) |  1.187 (0.99x) |  1.113 (0.93x) | 1.113 (1.00x) |
| noise            | 1920x1080  |   2.674 |  2.662 (1.00x) |  2.659 (0.99x) |  2.500 (0.94x) | 2.500 (1.00x) |
| noise            | 3840x2160  |  10.750 | 10.636 (0.99x) | 10.690 (0.99x) | 10.034 (0.93x) | 9.975 (0.99x) |
| postprocess      | 1280x720   |   0.038 |  0.038 (1.00x) |  0.038 (1.00x) |  0.043 (1.14x) | 0.043 (1.00x) |
| postprocess      | 1920x1080  |   0.087 |  0.086 (0.99x) |  0.086 (0.99x) |  0.109 (1.26x) | 0.109 (1.00x) |
| postprocess      | 3840x2160  |   0.504 |  0.504 (1.00x) |  0.505 (1.00x) |  0.552 (1.10x) | 0.554 (1.00x) |
| vertex_heavy     | 1280x720   |   0.514 |  0.509 (0.99x) |  0.556 (1.08x) |  0.461 (0.90x) | 0.455 (0.99x) |
| vertex_heavy     | 1920x1080  |   0.548 |  0.542 (0.99x) |  0.595 (1.09x) |  0.502 (0.92x) | 0.488 (0.97x) |
| vertex_heavy     | 3840x2160  |   0.622 |  0.606 (0.97x) |  0.656 (1.05x) |  0.578 (0.93x) | 0.578 (1.00x) |

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