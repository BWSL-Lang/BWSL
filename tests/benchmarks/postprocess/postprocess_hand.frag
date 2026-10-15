#version 300 es
precision highp float;
precision highp int;

layout(std140) uniform ub_params {
    vec4 texel;   // 1/width, 1/height, width, height
    vec4 grade;   // exposure, bloom strength, vignette strength, grain strength
    vec4 lift;
    vec4 gamma;
    vec4 gain;
    vec4 misc;    // chromatic aberration, frame seed, history blend, dirt strength
} params;

uniform sampler2D t_scene;
uniform sampler2D t_bloom;
uniform sampler2D t_dirt;
uniform sampler2D t_history;

in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

uint pcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

vec3 acesFitted(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 linearToSrgb(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, lessThanEqual(c, vec3(0.0031308)));
}

void main() {
    vec2 uv = v_uv;
    float aspect = params.texel.z * params.texel.y;

    vec2 centered = uv - 0.5;
    vec2 shift = centered * dot(centered, centered) * params.misc.x;
    vec3 color = vec3(texture(t_scene, uv + shift).r,
                      texture(t_scene, uv).g,
                      texture(t_scene, uv - shift).b);

    vec2 bloomTexel = params.texel.xy * 2.0;
    vec3 bloom = vec3(0.0);
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            float weight = float((2 - abs(x)) * (2 - abs(y))) / 16.0;
            bloom += texture(t_bloom, uv + vec2(x, y) * bloomTexel).rgb * weight;
        }
    }
    vec3 dirt = texture(t_dirt, uv * vec2(aspect, 1.0) * 2.0).rgb;
    color += bloom * (params.grade.y + dirt * params.misc.w);

    color = acesFitted(color * params.grade.x);

    color = params.gain.rgb * (color + params.lift.rgb * (1.0 - color));
    color = pow(max(color, 0.0), 1.0 / params.gamma.rgb);

    color *= 1.0 - smoothstep(0.3, 1.0, length(centered * vec2(aspect, 1.0))) * params.grade.z;

    uvec2 pixel = uvec2(uv * params.texel.zw);
    uint h = pcgHash(pixel.x + pcgHash(pixel.y + pcgHash(uint(params.misc.y))));
    color += (float(h & 0xffffu) / 65535.0 - 0.5) * params.grade.w;

    color = clamp(mix(color, texture(t_history, uv).rgb, params.misc.z), 0.0, 1.0);

    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    fragColor = vec4(linearToSrgb(color), luma);
}
