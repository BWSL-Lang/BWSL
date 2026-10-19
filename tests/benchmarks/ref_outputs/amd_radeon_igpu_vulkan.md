 ```
 > python tests/benchmarks/run.py --backends vk-hand,vk-bwsl --vk-device 1 --rounds 2 --skip-after 20
[hsluv] compiling
[hsluv] 1280x720: vk-hand 1.716, vk-bwsl 1.799 (2.7s)
[hsluv] 1920x1080: vk-hand 3.860, vk-bwsl 4.050 (3.4s)
[hsluv] 3840x2160: vk-hand 15.431, vk-bwsl 16.186 (7.9s)
[hsluv_components] compiling
[hsluv_components] 1280x720: vk-hand 1.058, vk-bwsl 1.125 (2.5s)
[hsluv_components] 1920x1080: vk-hand 2.375, vk-bwsl 2.524 (2.9s)
[hsluv_components] 3840x2160: vk-hand 9.484, vk-bwsl 10.082 (6.2s)
[mrt] compiling
[mrt] 1280x720: vk-hand 2.651, vk-bwsl 2.666 (6.3s)
[mrt] 1920x1080: vk-hand 6.215, vk-bwsl 6.198 (4.4s)
[mrt] 3840x2160: vk-hand 25.640, vk-bwsl 25.871 (8.2s)
[noise] compiling
[noise] 1280x720: vk-hand 58.590, vk-bwsl 59.150 (9.2s)
[noise] 1920x1080: vk-hand 131.735, vk-bwsl 132.986 (9.2s)
[noise] 3840x2160: vk-hand 527.263, vk-bwsl 532.141 (19.9s)
[postprocess] compiling
[postprocess] 1280x720: vk-hand 0.914, vk-bwsl 0.899 (2.5s)
[postprocess] 1920x1080: vk-hand 1.920, vk-bwsl 1.827 (2.7s)
[postprocess] 3840x2160: vk-hand 7.623, vk-bwsl 7.228 (5.3s)
[struct_folding] compiling
[struct_folding] 1280x720: vk-hand 1.387, vk-bwsl 1.353 (2.6s)
[struct_folding] 1920x1080: vk-hand 3.113, vk-bwsl 3.038 (3.0s)
[struct_folding] 3840x2160: vk-hand 12.433, vk-bwsl 12.152 (6.9s)
[struct_folding_inline] compiling
[struct_folding_inline] 1280x720: vk-hand 1.389, vk-bwsl 1.355 (2.6s)
[struct_folding_inline] 1920x1080: vk-hand 3.112, vk-bwsl 3.041 (3.0s)
[struct_folding_inline] 3840x2160: vk-hand 12.462, vk-bwsl 12.178 (6.9s)
[structs] compiling
[structs] 1280x720: vk-hand 2.405, vk-bwsl 3.045 (3.2s)
[structs] 1920x1080: vk-hand 5.365, vk-bwsl 6.794 (4.3s)
[structs] 3840x2160: vk-hand 21.244, vk-bwsl 26.908 (8.0s)
[vertex_heavy] compiling
[vertex_heavy] 1280x720: vk-hand 38.118, vk-bwsl 38.148 (8.8s)
[vertex_heavy] 1920x1080: vk-hand 38.214, vk-bwsl 38.164 (8.5s)
[vertex_heavy] 3840x2160: vk-hand 38.410, vk-bwsl 38.430 (8.5s)
```

## BWSL GPU benchmarks

- Vulkan: AMD Radeon(TM) Graphics (AMD proprietary driver 2026.04.17 (AMD proprietary shader compiler))
- bwslc: build\bwslc.exe, built 2026-10-19 20:35, source 6bed3f0-dirty
- GPU ms per frame, median of up to 100 timed frames (x2 rounds)
- Ratios compare against the hand-written shader on the same API (VK hand against GL hand, i.e. the API's own cost)

| Test                  | Resolution | VK hand |         VK bwsl |
| --------------------- | ---------- | ------: | --------------: |
| hsluv                 | 1280x720   |   1.716 |   1.799 (1.05x) |
| hsluv                 | 1920x1080  |   3.860 |   4.050 (1.05x) |
| hsluv                 | 3840x2160  |  15.431 |  16.186 (1.05x) |
| hsluv_components      | 1280x720   |   1.058 |   1.125 (1.06x) |
| hsluv_components      | 1920x1080  |   2.375 |   2.524 (1.06x) |
| hsluv_components      | 3840x2160  |   9.484 |  10.082 (1.06x) |
| mrt                   | 1280x720   |   2.651 |   2.666 (1.01x) |
| mrt                   | 1920x1080  |   6.215 |   6.198 (1.00x) |
| mrt                   | 3840x2160  |  25.640 |  25.871 (1.01x) |
| noise                 | 1280x720   |  58.590 |  59.150 (1.01x) |
| noise                 | 1920x1080  | 131.735 | 132.986 (1.01x) |
| noise                 | 3840x2160  | 527.263 | 532.141 (1.01x) |
| postprocess           | 1280x720   |   0.914 |   0.899 (0.98x) |
| postprocess           | 1920x1080  |   1.920 |   1.827 (0.95x) |
| postprocess           | 3840x2160  |   7.623 |   7.228 (0.95x) |
| struct_folding        | 1280x720   |   1.387 |   1.353 (0.98x) |
| struct_folding        | 1920x1080  |   3.113 |   3.038 (0.98x) |
| struct_folding        | 3840x2160  |  12.433 |  12.152 (0.98x) |
| struct_folding_inline | 1280x720   |   1.389 |   1.355 (0.98x) |
| struct_folding_inline | 1920x1080  |   3.112 |   3.041 (0.98x) |
| struct_folding_inline | 3840x2160  |  12.462 |  12.178 (0.98x) |
| structs               | 1280x720   |   2.405 |   3.045 (1.27x) |
| structs               | 1920x1080  |   5.365 |   6.794 (1.27x) |
| structs               | 3840x2160  |  21.244 |  26.908 (1.27x) |
| vertex_heavy          | 1280x720   |  38.118 |  38.148 (1.00x) |
| vertex_heavy          | 1920x1080  |  38.214 |  38.164 (1.00x) |
| vertex_heavy          | 3840x2160  |  38.410 |  38.430 (1.00x) |

Output vs VK hand (max abs difference; share of channel values over the tolerance):

| Test                  | VK bwsl     |
| --------------------- | ----------- |
| hsluv                 | ok (0.0039) |
| hsluv_components      | ok (0.0039) |
| mrt                   | ok (0)      |
| noise                 | ok (0.0039) |
| postprocess           | ok (0)      |
| struct_folding        | ok (0.0039) |
| struct_folding_inline | ok (0.0039) |
| structs               | ok (0.0039) |
| vertex_heavy          | ok (0.0039) |
