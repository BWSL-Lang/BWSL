 
 ```
 > python tests/benchmarks/run.py --backends vk-hand,vk-bwsl --vk-device 1 --rounds 2 --skip-after 20
[hsluv] compiling
[hsluv] 1280x720: vk-hand 1.719, vk-bwsl 1.802 (2.8s)
[hsluv] 1920x1080: vk-hand 3.853, vk-bwsl 4.042 (3.4s)
[hsluv] 3840x2160: vk-hand 15.402, vk-bwsl 16.163 (7.9s)
[hsluv_components] compiling
[hsluv_components] 1280x720: vk-hand 1.056, vk-bwsl 1.122 (2.5s)
[hsluv_components] 1920x1080: vk-hand 2.369, vk-bwsl 2.519 (2.8s)
[hsluv_components] 3840x2160: vk-hand 9.467, vk-bwsl 10.064 (5.8s)
[mrt] compiling
[mrt] 1280x720: vk-hand 2.638, vk-bwsl 2.652 (6.0s)
[mrt] 1920x1080: vk-hand 6.086, vk-bwsl 6.141 (4.4s)
[mrt] 3840x2160: vk-hand 25.154, vk-bwsl 25.357 (8.1s)
[noise] compiling
[noise] 1280x720: vk-hand 58.502, vk-bwsl 59.046 (9.3s)
[noise] 1920x1080: vk-hand 131.598, vk-bwsl 132.828 (9.3s)
[noise] 3840x2160: vk-hand 526.335, vk-bwsl 531.334 (19.9s)
[postprocess] compiling
[postprocess] 1280x720: vk-hand 0.909, vk-bwsl 0.881 (2.4s)
[postprocess] 1920x1080: vk-hand 1.915, vk-bwsl 1.819 (2.7s)
[postprocess] 3840x2160: vk-hand 7.585, vk-bwsl 7.196 (5.3s)
[struct_folding] compiling
[struct_folding] 1280x720: vk-hand 1.386, vk-bwsl 1.352 (2.6s)
[struct_folding] 1920x1080: vk-hand 3.112, vk-bwsl 3.037 (3.1s)
[struct_folding] 3840x2160: vk-hand 12.431, vk-bwsl 12.152 (6.9s)
[struct_folding_inline] compiling
[struct_folding_inline] 1280x720: vk-hand 1.386, vk-bwsl 1.351 (2.6s)
[struct_folding_inline] 1920x1080: vk-hand 3.112, vk-bwsl 3.038 (3.0s)
[struct_folding_inline] 3840x2160: vk-hand 12.436, vk-bwsl 12.156 (6.9s)
[structs] compiling
[structs] 1280x720: vk-hand 2.400, vk-bwsl 3.045 (3.1s)
[structs] 1920x1080: vk-hand 5.361, vk-bwsl 6.801 (4.3s)
[structs] 3840x2160: vk-hand 21.196, vk-bwsl 26.904 (8.0s)
[vertex_heavy] compiling
[vertex_heavy] 1280x720: vk-hand 38.041, vk-bwsl 38.065 (8.8s)
[vertex_heavy] 1920x1080: vk-hand 38.124, vk-bwsl 38.149 (8.5s)
[vertex_heavy] 3840x2160: vk-hand 38.312, vk-bwsl 38.351 (8.6s)
```

## BWSL GPU benchmarks

- Vulkan: AMD Radeon(TM) Graphics (AMD proprietary driver 2026.04.17 (AMD proprietary shader compiler))
- bwslc: build\bwslc.exe, built 2026-10-18 21:12, source 7a892a9-dirty
- GPU ms per frame, median of up to 100 timed frames (x2 rounds)
- Ratios compare against the hand-written shader on the same API (VK hand against GL hand, i.e. the API's own cost)

| Test                  | Resolution | VK hand |         VK bwsl |
| --------------------- | ---------- | ------: | --------------: |
| hsluv                 | 1280x720   |   1.719 |   1.802 (1.05x) |
| hsluv                 | 1920x1080  |   3.853 |   4.042 (1.05x) |
| hsluv                 | 3840x2160  |  15.402 |  16.163 (1.05x) |
| hsluv_components      | 1280x720   |   1.056 |   1.122 (1.06x) |
| hsluv_components      | 1920x1080  |   2.369 |   2.519 (1.06x) |
| hsluv_components      | 3840x2160  |   9.467 |  10.064 (1.06x) |
| mrt                   | 1280x720   |   2.638 |   2.652 (1.01x) |
| mrt                   | 1920x1080  |   6.086 |   6.141 (1.01x) |
| mrt                   | 3840x2160  |  25.154 |  25.357 (1.01x) |
| noise                 | 1280x720   |  58.502 |  59.046 (1.01x) |
| noise                 | 1920x1080  | 131.598 | 132.828 (1.01x) |
| noise                 | 3840x2160  | 526.335 | 531.334 (1.01x) |
| postprocess           | 1280x720   |   0.909 |   0.881 (0.97x) |
| postprocess           | 1920x1080  |   1.915 |   1.819 (0.95x) |
| postprocess           | 3840x2160  |   7.585 |   7.196 (0.95x) |
| struct_folding        | 1280x720   |   1.386 |   1.352 (0.98x) |
| struct_folding        | 1920x1080  |   3.112 |   3.037 (0.98x) |
| struct_folding        | 3840x2160  |  12.431 |  12.152 (0.98x) |
| struct_folding_inline | 1280x720   |   1.386 |   1.351 (0.98x) |
| struct_folding_inline | 1920x1080  |   3.112 |   3.038 (0.98x) |
| struct_folding_inline | 3840x2160  |  12.436 |  12.156 (0.98x) |
| structs               | 1280x720   |   2.400 |   3.045 (1.27x) |
| structs               | 1920x1080  |   5.361 |   6.801 (1.27x) |
| structs               | 3840x2160  |  21.196 |  26.904 (1.27x) |
| vertex_heavy          | 1280x720   |  38.041 |  38.065 (1.00x) |
| vertex_heavy          | 1920x1080  |  38.124 |  38.149 (1.00x) |
| vertex_heavy          | 3840x2160  |  38.312 |  38.351 (1.00x) |

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
