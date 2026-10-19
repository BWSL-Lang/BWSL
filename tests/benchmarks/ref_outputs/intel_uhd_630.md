```
[hsluv] compiling
[hsluv] 1280x720: gl-hand 3.048, gl-bwsl 2.959, gl-direct 3.079, vk-hand 2.940, vk-bwsl 3.065 (4.2s)
[hsluv] 1920x1080: gl-hand 6.262, gl-bwsl 6.751, gl-direct 6.976, vk-hand 6.693, vk-bwsl 7.006 (6.0s)
[hsluv] 3840x2160: gl-hand 25.164, gl-bwsl 26.988, gl-direct 28.108, vk-hand 26.257, vk-bwsl 27.725 (10.1s)
[hsluv_components] compiling
[hsluv_components] 1280x720: gl-hand 1.364, gl-bwsl 1.450, gl-direct 1.522, vk-hand 1.266, vk-bwsl 1.428 (3.1s)
[hsluv_components] 1920x1080: gl-hand 3.093, gl-bwsl 3.175, gl-direct 3.450, vk-hand 2.781, vk-bwsl 3.148 (3.7s)
[hsluv_components] 3840x2160: gl-hand 12.381, gl-bwsl 12.877, gl-direct 13.869, vk-hand 11.145, vk-bwsl 12.627 (8.8s)
[mrt] compiling
[mrt] 1280x720: gl-hand 3.388, gl-bwsl 3.355, gl-direct 3.355, vk-hand 3.444, vk-bwsl 3.445 (7.0s)
[mrt] 1920x1080: gl-hand 7.645, gl-bwsl 7.661, gl-direct 7.632, vk-hand 7.752, vk-bwsl 7.751 (6.3s)
[mrt] 3840x2160: gl-hand 30.669, gl-bwsl 30.637, gl-direct 30.786, vk-hand 31.018, vk-bwsl 30.968 (10.4s)
[noise] compiling
[noise] 1280x720: gl-hand 65.096, gl-bwsl 67.266, gl-direct 67.488, vk-hand 64.730, vk-bwsl 64.701 (12.2s)
[noise] took 12.2s > 10s, skipping 1080p, 4k
[postprocess] compiling
[postprocess] 1280x720: gl-hand 1.286, gl-bwsl 1.253, gl-direct 1.253, vk-hand 1.285, vk-bwsl 1.349 (3.1s)
[postprocess] 1920x1080: gl-hand 2.802, gl-bwsl 2.740, gl-direct 2.737, vk-hand 2.831, vk-bwsl 2.920 (3.8s)
[postprocess] 3840x2160: gl-hand 11.143, gl-bwsl 10.848, gl-direct 10.865, vk-hand 11.281, vk-bwsl 11.654 (8.9s)
[vertex_heavy] compiling
[vertex_heavy] 1280x720: gl-hand 21.512, gl-bwsl 21.558, gl-direct 22.479, vk-hand 21.109, vk-bwsl 21.202 (11.5s)
[vertex_heavy] took 11.5s > 10s, skipping 1080p, 4k
```

## BWSL GPU benchmarks
- OpenGL: Intel(R) UHD Graphics 630 (4.6.0 - Build 31.0.101.2115)
- Vulkan: Intel(R) UHD Graphics 630 (Intel Corporation Intel driver)
- bwslc: build\bwslc.exe, built 2026-10-17 11:29, source d4718bf
- GPU ms per frame, median of up to 100 timed frames (x1 rounds)
- Ratios compare against the hand-written shader on the same API (VK hand against GL hand, i.e. the API's own cost)

| Test             | Resolution | GL hand |        GL bwsl |      GL direct |        VK hand |        VK bwsl |
| ---------------- | ---------- | ------: | -------------: | -------------: | -------------: | -------------: |
| hsluv            | 1280x720   |   3.048 |  2.959 (0.97x) |  3.079 (1.01x) |  2.940 (0.96x) |  3.065 (1.04x) |
| hsluv            | 1920x1080  |   6.262 |  6.751 (1.08x) |  6.976 (1.11x) |  6.693 (1.07x) |  7.006 (1.05x) |
| hsluv            | 3840x2160  |  25.164 | 26.988 (1.07x) | 28.108 (1.12x) | 26.257 (1.04x) | 27.725 (1.06x) |
| hsluv_components | 1280x720   |   1.364 |  1.450 (1.06x) |  1.522 (1.12x) |  1.266 (0.93x) |  1.428 (1.13x) |
| hsluv_components | 1920x1080  |   3.093 |  3.175 (1.03x) |  3.450 (1.12x) |  2.781 (0.90x) |  3.148 (1.13x) |
| hsluv_components | 3840x2160  |  12.381 | 12.877 (1.04x) | 13.869 (1.12x) | 11.145 (0.90x) | 12.627 (1.13x) |
| mrt              | 1280x720   |   3.388 |  3.355 (0.99x) |  3.355 (0.99x) |  3.444 (1.02x) |  3.445 (1.00x) |
| mrt              | 1920x1080  |   7.645 |  7.661 (1.00x) |  7.632 (1.00x) |  7.752 (1.01x) |  7.751 (1.00x) |
| mrt              | 3840x2160  |  30.669 | 30.637 (1.00x) | 30.786 (1.00x) | 31.018 (1.01x) | 30.968 (1.00x) |
| noise            | 1280x720   |  65.096 | 67.266 (1.03x) | 67.488 (1.04x) | 64.730 (0.99x) | 64.701 (1.00x) |
| postprocess      | 1280x720   |   1.286 |  1.253 (0.97x) |  1.253 (0.97x) |  1.285 (1.00x) |  1.349 (1.05x) |
| postprocess      | 1920x1080  |   2.802 |  2.740 (0.98x) |  2.737 (0.98x) |  2.831 (1.01x) |  2.920 (1.03x) |
| postprocess      | 3840x2160  |  11.143 | 10.848 (0.97x) | 10.865 (0.98x) | 11.281 (1.01x) | 11.654 (1.03x) |
| vertex_heavy     | 1280x720   |  21.512 | 21.558 (1.00x) | 22.479 (1.04x) | 21.109 (0.98x) | 21.202 (1.00x) |

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