#version 300 es
precision highp float;
precision highp int;

layout(std140) uniform ub_scene {
    vec4 viewX;           // rows of the view-projection matrix
    vec4 viewY;
    vec4 viewZ;
    vec4 viewW;
    vec4 params;          // time, height scale, noise scale, shininess
    vec4 eye;
    vec4 lightDir[4];
    vec4 lightColor[4];   // rgb, specular strength
    vec4 bones[24];       // 8 bones x 3 affine rows (offsets from identity)
} scene;

in vec3 a_position;
in vec2 a_uv;
out vec3 v_shade;

#define OCTAVES 4
#define BONES 8
#define LIGHTS 4

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

float terrainHeight(vec2 uv) {
    vec2 p = uv * scene.params.z + scene.params.x * 0.1;
    float sum = 0.0;
    float amplitude = 0.5;
    for (int octave = 0; octave < OCTAVES; octave++) {
        sum += amplitude * gradientNoise(p);
        p = OCTAVE_ROTATION * p * 2.02;
        amplitude *= 0.5;
    }
    return sum * scene.params.y;
}

void main() {
    vec2 uv = a_uv;
    const float STEP = 1.0 / 256.0;
    float h = terrainHeight(uv);
    float hx = terrainHeight(uv + vec2(STEP, 0.0));
    float hy = terrainHeight(uv + vec2(0.0, STEP));
    vec3 n = normalize(vec3(h - hx, h - hy, STEP * 2.0));
    vec3 pos = vec3(a_position.xy, h);

    vec4 p4 = vec4(pos, 1.0);
    vec3 offset = vec3(0.0);
    vec3 normalOffset = vec3(0.0);
    float total = 0.0;
    for (int b = 0; b < BONES; b++) {
        vec2 center = vec2(fract(float(b) * 0.618034), fract(float(b) * 0.381966 + 0.25));
        vec2 d = uv - center;
        float w = exp(-dot(d, d) * 12.0);
        vec4 rx = scene.bones[b * 3];
        vec4 ry = scene.bones[b * 3 + 1];
        vec4 rz = scene.bones[b * 3 + 2];
        offset += w * vec3(dot(rx, p4), dot(ry, p4), dot(rz, p4));
        normalOffset += w * vec3(dot(rx.xyz, n), dot(ry.xyz, n), dot(rz.xyz, n));
        total += w;
    }
    float inv = 1.0 / max(total, 0.001);
    pos += offset * inv;
    n = normalize(n + normalOffset * inv);

    vec3 color = vec3(0.03, 0.035, 0.05);
    for (int l = 0; l < LIGHTS; l++) {
        vec3 L = normalize(scene.lightDir[l].xyz);
        float diffuse = max(dot(n, L), 0.0);
        vec3 H = normalize(L + scene.eye.xyz);
        float specular = pow(max(dot(n, H), 0.0), scene.params.w);
        color += scene.lightColor[l].rgb * (diffuse + specular * scene.lightColor[l].a);
    }

    vec4 world = vec4(pos, 1.0);
    gl_Position = vec4(dot(scene.viewX, world), dot(scene.viewY, world),
                       dot(scene.viewZ, world), dot(scene.viewW, world));
    v_shade = color;
}
