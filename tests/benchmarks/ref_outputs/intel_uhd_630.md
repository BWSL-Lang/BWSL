```
> python tests/benchmarks/run.py
[hsluv] compiling
[hsluv] 1280x720: gl-hand 3.021, gl-bwsl 2.937, gl-direct 3.120, vk-hand 3.013, vk-bwsl 3.172 (4.5s)
[hsluv] 1920x1080: gl-hand 6.309, gl-bwsl 6.683, gl-direct 7.049, vk-hand 6.716, vk-bwsl 7.098 (5.8s)
[hsluv] 3840x2160: gl-hand 25.170, gl-bwsl 26.756, gl-direct 28.196, vk-hand 26.123, vk-bwsl 28.091 (10.1s)
[mrt] compiling
[mrt] 1280x720: gl-hand 3.397, gl-bwsl 3.403, gl-direct 3.404, vk-hand 3.550, vk-bwsl 3.553 (7.2s)
[mrt] 1920x1080: gl-hand 7.685, gl-bwsl 7.661, gl-direct 7.692, vk-hand 7.968, vk-bwsl 7.926 (6.4s)
[mrt] 3840x2160: gl-hand 30.738, gl-bwsl 30.917, gl-direct 30.377, vk-hand 30.670, vk-bwsl 30.644 (10.6s)
[noise] compiling
[noise] 1280x720: gl-hand 64.782, gl-bwsl 70.015, gl-direct 69.863, vk-hand 64.398, vk-bwsl 64.581 (12.8s)
[noise] took 12.8s > 10s, skipping 1080p, 4k
[postprocess] compiling
[postprocess] 1280x720: gl-hand 1.290, gl-bwsl 1.259, gl-direct 1.258, vk-hand 1.253, vk-bwsl 1.315 (3.3s)
[postprocess] 1920x1080: gl-hand 2.843, gl-bwsl 2.784, gl-direct 2.764, vk-hand 2.820, vk-bwsl 2.910 (4.0s)
[postprocess] 3840x2160: gl-hand 11.248, gl-bwsl 10.941, gl-direct 10.973, vk-hand 11.513, vk-bwsl 11.912 (9.1s)
[vertex_heavy] compiling
[vertex_heavy] 1280x720: gl-hand 21.491, gl-bwsl 21.368, gl-direct error, vk-hand 20.997, vk-bwsl 21.618 (9.8s)
[vertex_heavy] 1920x1080: gl-hand 22.128, gl-bwsl 22.169, gl-direct error, vk-hand 21.959, vk-bwsl 22.023 (9.1s)
[vertex_heavy] 3840x2160: gl-hand 23.915, gl-bwsl 24.091, gl-direct error, vk-hand 23.228, vk-bwsl 23.482 (9.2s)
```

## BWSL GPU benchmarks

- OpenGL: Intel(R) UHD Graphics 630 (4.6.0 - Build 31.0.101.2115)
- Vulkan: Intel(R) UHD Graphics 630 (Intel Corporation Intel driver)
- bwslc: build\bwslc.exe, built 2026-10-14 21:53, source 141f22e
- GPU ms per frame, median of up to 100 timed frames (x1 rounds)
- Ratios compare against the hand-written shader on the same API (VK hand against GL hand, i.e. the API's own cost)

| Test         | Resolution | GL hand |        GL bwsl |      GL direct |        VK hand |        VK bwsl |
| ------------ | ---------- | ------: | -------------: | -------------: | -------------: | -------------: |
| hsluv        | 1280x720   |   3.021 |  2.937 (0.97x) |  3.120 (1.03x) |  3.013 (1.00x) |  3.172 (1.05x) |
| hsluv        | 1920x1080  |   6.309 |  6.683 (1.06x) |  7.049 (1.12x) |  6.716 (1.06x) |  7.098 (1.06x) |
| hsluv        | 3840x2160  |  25.170 | 26.756 (1.06x) | 28.196 (1.12x) | 26.123 (1.04x) | 28.091 (1.08x) |
| mrt          | 1280x720   |   3.397 |  3.403 (1.00x) |  3.404 (1.00x) |  3.550 (1.05x) |  3.553 (1.00x) |
| mrt          | 1920x1080  |   7.685 |  7.661 (1.00x) |  7.692 (1.00x) |  7.968 (1.04x) |  7.926 (0.99x) |
| mrt          | 3840x2160  |  30.738 | 30.917 (1.01x) | 30.377 (0.99x) | 30.670 (1.00x) | 30.644 (1.00x) |
| noise        | 1280x720   |  64.782 | 70.015 (1.08x) | 69.863 (1.08x) | 64.398 (0.99x) | 64.581 (1.00x) |
| postprocess  | 1280x720   |   1.290 |  1.259 (0.98x) |  1.258 (0.98x) |  1.253 (0.97x) |  1.315 (1.05x) |
| postprocess  | 1920x1080  |   2.843 |  2.784 (0.98x) |  2.764 (0.97x) |  2.820 (0.99x) |  2.910 (1.03x) |
| postprocess  | 3840x2160  |  11.248 | 10.941 (0.97x) | 10.973 (0.98x) | 11.513 (1.02x) | 11.912 (1.03x) |
| vertex_heavy | 1280x720   |  21.491 | 21.368 (0.99x) |          error | 20.997 (0.98x) | 21.618 (1.03x) |
| vertex_heavy | 1920x1080  |  22.128 | 22.169 (1.00x) |          error | 21.959 (0.99x) | 22.023 (1.00x) |
| vertex_heavy | 3840x2160  |  23.915 | 24.091 (1.01x) |          error | 23.228 (0.97x) | 23.482 (1.01x) |

GL direct generated the same frag code as GL bwsl (fell back to SPIRV-Cross) for: noise, postprocess

Output vs GL hand (max abs difference; share of channel values over the tolerance):

| Test         | GL bwsl     | GL direct   | VK hand      | VK bwsl     |
| ------------ | ----------- | ----------- | ------------ | ----------- |
| hsluv        | ok (0.0039) | ok (0.0039) | ok (0.0039)  | ok (0.0039) |
| mrt          | ok (0.002)  | ok (0.002)  | ok (0.00024) | ok (0.002)  |
| noise        | ok (0.0039) | ok (0.0039) | ok (0.0039)  | ok (0.0039) |
| postprocess  | ok (0)      | ok (0)      | ok (0)       | ok (0)      |
| vertex_heavy | ok (0.0039) | -           | ok (0.0039)  | ok (0.0039) |

Errors:
- vertex_heavy / gl-direct / 1280x720:
    bench_runner: GL compile failed for C:\Users\lundis\BWSL\BWSL-pr181\build\benchmarks\vertex_heavy\gles-direct\vertex_heavy.vert:
    ERROR: 0:2398: 'assign' :  cannot convert from '24-element array of 4-component vector of float' to '4-component vector of highp float'
    ERROR: 0:2403: 'assign' :  cannot convert from '24-element array of 4-component vector of float' to '4-component vector of highp float'
    ERROR: 0:2409: 'assign' :  cannot convert from '24-element array of 4-component vector of float' to '4-component vector of highp float'
    ERROR: 0:2490: 'assign' :  cannot convert from '4-element array of 4-component vector of float' to '4-component vector of highp float'
    ERROR: 0:2496: 'assign' :  cannot convert from '4-element array of 4-component vector of float' to '4-component vector of highp float'
