```
[hsluv] compiling
[hsluv] 1280x720: gl-hand 3.021, gl-bwsl 2.951, gl-direct 3.080, vk-hand 2.906, vk-bwsl 3.065 (4.2s)
[hsluv] 1920x1080: gl-hand 6.263, gl-bwsl 6.711, gl-direct 7.016, vk-hand 6.549, vk-bwsl 6.930 (5.7s)
[hsluv] 3840x2160: gl-hand 25.130, gl-bwsl 26.867, gl-direct 28.074, vk-hand 26.262, vk-bwsl 27.665 (10.1s)
[hsluv_components] compiling
[hsluv_components] 1280x720: gl-hand 1.364, gl-bwsl 1.413, gl-direct 1.522, vk-hand 1.266, vk-bwsl 1.428 (3.1s)
[hsluv_components] 1920x1080: gl-hand 3.058, gl-bwsl 3.205, gl-direct 3.416, vk-hand 2.782, vk-bwsl 3.148 (3.7s)
[hsluv_components] 3840x2160: gl-hand 12.374, gl-bwsl 12.840, gl-direct 13.876, vk-hand 11.136, vk-bwsl 12.640 (8.8s)
[mrt] compiling
[mrt] 1280x720: gl-hand 3.396, gl-bwsl 3.400, gl-direct 3.356, vk-hand 3.444, vk-bwsl 3.446 (6.9s)
[mrt] 1920x1080: gl-hand 7.702, gl-bwsl 7.645, gl-direct 7.664, vk-hand 7.715, vk-bwsl 7.741 (6.3s)
[mrt] 3840x2160: gl-hand 30.644, gl-bwsl 30.732, gl-direct 30.679, vk-hand 31.048, vk-bwsl 31.053 (10.5s)
[noise] compiling
[noise] 1280x720: gl-hand 65.085, gl-bwsl 67.418, gl-direct 67.474, vk-hand 64.769, vk-bwsl 64.856 (12.1s)
[noise] took 12.1s > 10s, skipping 1080p, 4k
[postprocess] compiling
[postprocess] 1280x720: gl-hand 1.285, gl-bwsl 1.254, gl-direct 1.254, vk-hand 1.284, vk-bwsl 1.351 (3.1s)
[postprocess] 1920x1080: gl-hand 2.802, gl-bwsl 2.746, gl-direct 2.746, vk-hand 2.834, vk-bwsl 2.923 (3.8s)
[postprocess] 3840x2160: gl-hand 11.174, gl-bwsl 10.722, gl-direct 10.523, vk-hand 11.241, vk-bwsl 11.613 (8.9s)
[vertex_heavy] compiling
[vertex_heavy] 1280x720: gl-hand 21.600, gl-bwsl 21.666, gl-direct 22.664, vk-hand 21.046, vk-bwsl 21.216 (11.5s)
[vertex_heavy] took 11.5s > 10s, skipping 1080p, 4k
```

## BWSL GPU benchmarks

- OpenGL: Intel(R) UHD Graphics 630 (4.6.0 - Build 31.0.101.2115)
- Vulkan: Intel(R) UHD Graphics 630 (Intel Corporation Intel driver)
- bwslc: build\bwslc.exe, built 2026-10-17 10:02, source e6b3708-dirty
- GPU ms per frame, median of up to 100 timed frames (x1 rounds)
- Ratios compare against the hand-written shader on the same API (VK hand against GL hand, i.e. the API's own cost)

| Test             | Resolution | GL hand |        GL bwsl |      GL direct |        VK hand |        VK bwsl |
| ---------------- | ---------- | ------: | -------------: | -------------: | -------------: | -------------: |
| hsluv            | 1280x720   |   3.021 |  2.951 (0.98x) |  3.080 (1.02x) |  2.906 (0.96x) |  3.065 (1.05x) |
| hsluv            | 1920x1080  |   6.263 |  6.711 (1.07x) |  7.016 (1.12x) |  6.549 (1.05x) |  6.930 (1.06x) |
| hsluv            | 3840x2160  |  25.130 | 26.867 (1.07x) | 28.074 (1.12x) | 26.262 (1.05x) | 27.665 (1.05x) |
| hsluv_components | 1280x720   |   1.364 |  1.413 (1.04x) |  1.522 (1.12x) |  1.266 (0.93x) |  1.428 (1.13x) |
| hsluv_components | 1920x1080  |   3.058 |  3.205 (1.05x) |  3.416 (1.12x) |  2.782 (0.91x) |  3.148 (1.13x) |
| hsluv_components | 3840x2160  |  12.374 | 12.840 (1.04x) | 13.876 (1.12x) | 11.136 (0.90x) | 12.640 (1.14x) |
| mrt              | 1280x720   |   3.396 |  3.400 (1.00x) |  3.356 (0.99x) |  3.444 (1.01x) |  3.446 (1.00x) |
| mrt              | 1920x1080  |   7.702 |  7.645 (0.99x) |  7.664 (1.00x) |  7.715 (1.00x) |  7.741 (1.00x) |
| mrt              | 3840x2160  |  30.644 | 30.732 (1.00x) | 30.679 (1.00x) | 31.048 (1.01x) | 31.053 (1.00x) |
| noise            | 1280x720   |  65.085 | 67.418 (1.04x) | 67.474 (1.04x) | 64.769 (1.00x) | 64.856 (1.00x) |
| postprocess      | 1280x720   |   1.285 |  1.254 (0.98x) |  1.254 (0.98x) |  1.284 (1.00x) |  1.351 (1.05x) |
| postprocess      | 1920x1080  |   2.802 |  2.746 (0.98x) |  2.746 (0.98x) |  2.834 (1.01x) |  2.923 (1.03x) |
| postprocess      | 3840x2160  |  11.174 | 10.722 (0.96x) | 10.523 (0.94x) | 11.241 (1.01x) | 11.613 (1.03x) |
| vertex_heavy     | 1280x720   |  21.600 | 21.666 (1.00x) | 22.664 (1.05x) | 21.046 (0.97x) | 21.216 (1.01x) |

GL direct generated the same frag code as GL bwsl (fell back to SPIRV-Cross) for: noise, postprocess

Output vs GL hand (max abs difference; share of channel values over the tolerance):

| Test             | GL bwsl     | GL direct   | VK hand      | VK bwsl     |
| ---------------- | ----------- | ----------- | ------------ | ----------- |
| hsluv            | ok (0.0039) | ok (0.0039) | ok (0.0039)  | ok (0.0039) |
| hsluv_components | ok (0.0039) | ok (0.0039) | ok (0.0039)  | ok (0.0039) |
| mrt              | ok (0.002)  | ok (0.002)  | ok (0.00024) | ok (0.002)  |
| noise            | ok (0.0039) | ok (0.0039) | ok (0.0039)  | ok (0.0039) |
| postprocess      | ok (0)      | ok (0)      | ok (0)       | ok (0)      |
| vertex_heavy     | ok (0.0039) | ok (0.0039) | ok (0.0039)  | ok (0.0039) |