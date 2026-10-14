 
 ```
 > python tests/benchmarks/run.py
[hsluv] compiling
[hsluv] 1280x720: gl-hand 0.052, gl-bwsl 0.053, gl-direct 0.078, vk-hand 0.058, vk-bwsl 0.054 (2.4s)
[hsluv] 1920x1080: gl-hand 0.121, gl-bwsl 0.124, gl-direct 0.178, vk-hand 0.129, vk-bwsl 0.120 (2.3s)
[hsluv] 3840x2160: gl-hand 0.493, gl-bwsl 0.504, gl-direct 0.723, vk-hand 0.512, vk-bwsl 0.480 (2.5s)
[mrt] compiling
[mrt] 1280x720: gl-hand 0.127, gl-bwsl 0.128, gl-direct 0.127, vk-hand 0.133, vk-bwsl 0.132 (5.5s)
[mrt] 1920x1080: gl-hand 0.294, gl-bwsl 0.296, gl-direct 0.297, vk-hand 0.305, vk-bwsl 0.302 (2.5s)
[mrt] 3840x2160: gl-hand 1.250, gl-bwsl 1.264, gl-direct 1.257, vk-hand 1.286, vk-bwsl 1.271 (3.0s)
[noise] compiling
[noise] 1280x720: gl-hand 1.206, gl-bwsl 1.271, gl-direct 1.269, vk-hand 1.127, vk-bwsl 1.123 (3.1s)
[noise] 1920x1080: gl-hand 2.692, gl-bwsl 2.847, gl-direct 2.857, vk-hand 2.515, vk-bwsl 2.513 (3.8s)
[noise] 3840x2160: gl-hand 10.813, gl-bwsl 11.422, gl-direct 11.424, vk-hand 10.087, vk-bwsl 10.082 (8.1s)
[postprocess] compiling
[postprocess] 1280x720: gl-hand 0.039, gl-bwsl 0.038, gl-direct 0.038, vk-hand 0.044, vk-bwsl 0.043 (2.5s)
[postprocess] gl-bwsl output differs from gl-hand: max 1, 0.024% over tolerance; images in C:\Users\lundis\BWSL\BWSL\build\benchmarks\postprocess\images
[postprocess] gl-direct output differs from gl-hand: max 1, 0.024% over tolerance; images in C:\Users\lundis\BWSL\BWSL\build\benchmarks\postprocess\images
[postprocess] 1920x1080: gl-hand 0.088, gl-bwsl 0.088, gl-direct 0.087, vk-hand 0.105, vk-bwsl 0.105 (2.4s)
[postprocess] 3840x2160: gl-hand 0.503, gl-bwsl 0.506, gl-direct 0.506, vk-hand 0.550, vk-bwsl 0.553 (2.9s)
[vertex_heavy] compiling
[vertex_heavy] 1280x720: gl-hand 0.518, gl-bwsl 0.510, gl-direct error, vk-hand 0.461, vk-bwsl 0.458 (2.5s)
[vertex_heavy] 1920x1080: gl-hand 0.551, gl-bwsl 0.543, gl-direct error, vk-hand 0.504, vk-bwsl 0.492 (2.4s)
[vertex_heavy] 3840x2160: gl-hand 0.623, gl-bwsl 0.609, gl-direct error, vk-hand 0.582, vk-bwsl 0.577 (2.4s)
```

## BWSL GPU benchmarks

- OpenGL: NVIDIA GeForce RTX 4060 Ti/PCIe/SSE2 (4.6.0 NVIDIA 617.14)
- Vulkan: NVIDIA GeForce RTX 4060 Ti (NVIDIA 617.14)
- bwslc: build\bwslc.exe, built 2026-10-14 09:56, source df549e9-dirty
- GPU ms per frame, median of up to 100 timed frames (x1 rounds)
- Ratios compare against the hand-written shader on the same API (VK hand against GL hand, i.e. the API's own cost)

| Test         | Resolution | GL hand |        GL bwsl |      GL direct |        VK hand |        VK bwsl |
| ------------ | ---------- | ------: | -------------: | -------------: | -------------: | -------------: |
| hsluv        | 1280x720   |   0.052 |  0.053 (1.02x) |  0.078 (1.49x) |  0.058 (1.12x) |  0.054 (0.93x) |
| hsluv        | 1920x1080  |   0.121 |  0.124 (1.03x) |  0.178 (1.47x) |  0.129 (1.07x) |  0.120 (0.93x) |
| hsluv        | 3840x2160  |   0.493 |  0.504 (1.02x) |  0.723 (1.47x) |  0.512 (1.04x) |  0.480 (0.94x) |
| mrt          | 1280x720   |   0.127 |  0.128 (1.01x) |  0.127 (1.00x) |  0.133 (1.05x) |  0.132 (1.00x) |
| mrt          | 1920x1080  |   0.294 |  0.296 (1.01x) |  0.297 (1.01x) |  0.305 (1.04x) |  0.302 (0.99x) |
| mrt          | 3840x2160  |   1.250 |  1.264 (1.01x) |  1.257 (1.01x) |  1.286 (1.03x) |  1.271 (0.99x) |
| noise        | 1280x720   |   1.206 |  1.271 (1.05x) |  1.269 (1.05x) |  1.127 (0.93x) |  1.123 (1.00x) |
| noise        | 1920x1080  |   2.692 |  2.847 (1.06x) |  2.857 (1.06x) |  2.515 (0.93x) |  2.513 (1.00x) |
| noise        | 3840x2160  |  10.813 | 11.422 (1.06x) | 11.424 (1.06x) | 10.087 (0.93x) | 10.082 (1.00x) |
| postprocess  | 1280x720   |   0.039 |  0.038 (0.97x) |  0.038 (0.97x) |  0.044 (1.13x) |  0.043 (0.98x) |
| postprocess  | 1920x1080  |   0.088 |  0.088 (1.00x) |  0.087 (0.99x) |  0.105 (1.19x) |  0.105 (1.00x) |
| postprocess  | 3840x2160  |   0.503 |  0.506 (1.01x) |  0.506 (1.01x) |  0.550 (1.09x) |  0.553 (1.00x) |
| vertex_heavy | 1280x720   |   0.518 |  0.510 (0.98x) |          error |  0.461 (0.89x) |  0.458 (0.99x) |
| vertex_heavy | 1920x1080  |   0.551 |  0.543 (0.99x) |          error |  0.504 (0.92x) |  0.492 (0.97x) |
| vertex_heavy | 3840x2160  |   0.623 |  0.609 (0.98x) |          error |  0.582 (0.93x) |  0.577 (0.99x) |

GL direct generated the same frag code as GL bwsl (fell back to SPIRV-Cross) for: noise, postprocess

Output vs GL hand (max abs difference; share of channel values over the tolerance):

| Test         | GL bwsl               | GL direct             | VK hand      | VK bwsl     |
| ------------ | --------------------- | --------------------- | ------------ | ----------- |
| hsluv        | ok (0.0039)           | ok (0.0039)           | ok (0.0039)  | ok (0.0039) |
| mrt          | ok (0.0044)           | ok (0.0044)           | ok (0.00049) | ok (0.0044) |
| noise        | ok (0.0039)           | ok (0.0039)           | ok (0.0039)  | ok (0.0039) |
| postprocess  | **MISMATCH** 1, 0.02% | **MISMATCH** 1, 0.02% | ok (0.0039)  | ok (0.0039) |
| vertex_heavy | ok (0)                | -                     | ok (0.0039)  | ok (0.0039) |

Errors:
- vertex_heavy / gl-direct / 1280x720:
  bench_runner: GL compile failed for C:\Users\lundis\BWSL\BWSL\build\benchmarks\vertex_heavy\gles-direct\shader.vert:
  0(2398) : error C1035: assignment of incompatible types
  0(2403) : error C1035: assignment of incompatible types
  0(2409) : error C1035: assignment of incompatible types
  0(2490) : error C1035: assignment of incompatible types
  0(2496) : error C1035: assignment of incompatible types