#version 300 es
precision highp float;
precision highp int;

layout(std140) uniform ub_params {
    vec4 view;    // scale, time, 1/width, 1/height
    vec4 colorA;
    vec4 colorB;
    vec4 colorC;
} params;

in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

#define OCTAVES 5

// 2D PCG hash (Jarzynski & Olano, "Hash Functions for GPU Rendering").
uvec2 pcg2d(uvec2 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return v;
}

vec2 cellRandom(vec2 cell) {
    uvec2 h = pcg2d(uvec2(ivec2(cell)));
    return vec2(h >> 24u) / 127.5 - 1.0;
}

float gradientNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = p - i;
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = dot(cellRandom(i), f);
    float b = dot(cellRandom(i + vec2(1.0, 0.0)), f - vec2(1.0, 0.0));
    float c = dot(cellRandom(i + vec2(0.0, 1.0)), f - vec2(0.0, 1.0));
    float d = dot(cellRandom(i + vec2(1.0, 1.0)), f - vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

const mat2 OCTAVE_ROTATION = mat2(0.8, 0.6, -0.6, 0.8);

float fbm(vec2 p) {
    float sum = 0.0;
    float amplitude = 0.5;
    for (int octave = 0; octave < OCTAVES; octave++) {
        sum += amplitude * gradientNoise(p);
        p = OCTAVE_ROTATION * p * 2.02;
        amplitude *= 0.5;
    }
    return sum;
}

float worley(vec2 p) {
    vec2 i = floor(p);
    vec2 f = p - i;
    float nearest = 8.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 offset = vec2(x, y);
            vec2 r = offset + cellRandom(i + offset) * 0.5 + 0.5 - f;
            nearest = min(nearest, dot(r, r));
        }
    }
    return sqrt(nearest);
}

void main() {
    float scale = params.view.x;
    float time = params.view.y;
    vec3 color = vec3(0.0);
    for (int s = 0; s < 4; s++) {
        vec2 jitter = (vec2(s & 1, s >> 1) - 0.5) * 0.5 * params.view.zw;
        vec2 pos = (v_uv + jitter) * scale;

        vec2 q = vec2(fbm(pos), fbm(pos + vec2(5.2, 1.3)));
        vec2 r = vec2(fbm(pos + 4.0 * q + vec2(1.7, 9.2) + 0.15 * time),
                      fbm(pos + 4.0 * q + vec2(8.3, 2.8) + 0.126 * time));
        float n = fbm(pos + 4.0 * r);
        float cells = worley(pos * 3.0 + r);

        vec3 c = mix(params.colorA.rgb, params.colorB.rgb, clamp(n * n * 4.0, 0.0, 1.0));
        c = mix(c, params.colorC.rgb, clamp(length(q), 0.0, 1.0));
        c *= 0.6 + 0.4 * smoothstep(0.0, 0.8, cells);
        color += c;
    }
    fragColor = vec4(color * 0.25, 1.0);
}
