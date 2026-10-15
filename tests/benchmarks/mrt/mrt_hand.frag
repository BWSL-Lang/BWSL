#version 300 es
precision highp float;
precision highp int;

layout(std140) uniform ub_params {
    vec4 terrain;      // uv tiling, height-to-normal strength, AO radius
    vec4 heightTexel;  // 1/height map size, detail tiling, detail strength
    vec4 grassTint;
    vec4 rockTint;
    vec4 lavaColor;
} params;

uniform sampler2D t_heightMap;
uniform sampler2D t_detailNormals;
uniform sampler2D t_albedoAtlas;

in vec2 v_uv;
layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec4 outEmissive;

const vec2 AO_RING[8] = vec2[8](
    vec2(1.0, 0.0), vec2(0.7071, 0.7071), vec2(0.0, 1.0), vec2(-0.7071, 0.7071),
    vec2(-1.0, 0.0), vec2(-0.7071, -0.7071), vec2(0.0, -1.0), vec2(0.7071, -0.7071));

void main() {
    vec2 uv = v_uv * params.terrain.x;
    vec2 texel = params.heightTexel.xy;

    float h = texture(t_heightMap, uv).r;
    float hl = texture(t_heightMap, uv - vec2(texel.x, 0.0)).r;
    float hr = texture(t_heightMap, uv + vec2(texel.x, 0.0)).r;
    float hd = texture(t_heightMap, uv - vec2(0.0, texel.y)).r;
    float hu = texture(t_heightMap, uv + vec2(0.0, texel.y)).r;
    vec3 n = normalize(vec3((hl - hr) * params.terrain.y, (hd - hu) * params.terrain.y, 1.0));

    vec3 detail = texture(t_detailNormals, uv * params.heightTexel.z).xyz * 2.0 - 1.0;
    n = normalize(vec3(n.xy + detail.xy * params.heightTexel.w, n.z));
    float slope = 1.0 - n.z;

    vec3 grass = texture(t_albedoAtlas, uv * 4.0).rgb * params.grassTint.rgb;
    vec3 rock = texture(t_albedoAtlas, uv * 3.0 + vec2(0.5, 0.0)).rgb * params.rockTint.rgb;
    vec3 snow = vec3(0.9, 0.92, 0.95) + texture(t_albedoAtlas, uv * 6.0 + vec2(0.0, 0.5)).rgb * 0.05;
    float rockWeight = smoothstep(0.15, 0.35, slope);
    float snowWeight = smoothstep(0.65, 0.8, h) * (1.0 - rockWeight);
    vec3 albedo = mix(mix(grass, rock, rockWeight), snow, snowWeight);

    float occlusion = 0.0;
    for (int i = 0; i < 8; i++) {
        vec2 offset = AO_RING[i] * params.terrain.z;
        float ring = texture(t_heightMap, uv + offset).r;
        float ringFar = texture(t_heightMap, uv + offset * 2.5).r;
        occlusion += max(ring - h, 0.0) + 0.5 * max(ringFar - h, 0.0);
    }
    float ao = clamp(1.0 - occlusion * 1.5, 0.0, 1.0);

    float lava = 1.0 - smoothstep(0.08, 0.15, h);
    float roughness = mix(0.85, 0.45, rockWeight) * (1.0 - 0.6 * snowWeight);

    outAlbedo = vec4(albedo * (1.0 - lava), ao);
    outNormal = vec4(n * 0.5 + 0.5, h);
    outMaterial = vec4(roughness, rockWeight * 0.1, slope, lava);
    outEmissive = vec4(params.lavaColor.rgb * lava * (0.5 + 0.5 * detail.z), 1.0);
}
