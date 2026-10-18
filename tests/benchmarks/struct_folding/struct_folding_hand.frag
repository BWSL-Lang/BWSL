#version 300 es
precision highp float;
precision highp int;

#define LIGHTS 4
const float PI = 3.14159265;

layout(std140) uniform ub_scene {
    vec4 lightPositions[LIGHTS];
    vec4 lightColors[LIGHTS];   // rgb color, w intensity
    vec4 camera;                // xyz position, w pattern scale
} scene;

in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

struct MaterialInputs {
    vec3 baseColor;
    float metallic;
    float roughness;
    vec3 emissive;
    float clearcoat;
    float clearcoatRoughness;
    float sheen;
    vec3 sheenColor;
    float anisotropy;
    vec3 anisotropyDirection;
    int lightingModel;
    bool unlit;
};

struct Surface {
    vec3 position;
    vec3 normal;
    vec3 tangent;
    vec3 view;
};

MaterialInputs defaultMaterial() {
    return MaterialInputs(vec3(1.0), 0.0, 0.5, vec3(0.0), 0.0, 0.1, 0.0, vec3(0.0),
                          0.0, vec3(1.0, 0.0, 0.0), 0, false);
}

float distributionGgx(float ndh, float alpha) {
    float a2 = alpha * alpha;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float visibilitySmith(float ndl, float ndv, float alpha) {
    float k = alpha * 0.5;
    return ndl / (ndl * (1.0 - k) + k) * (ndv / (ndv * (1.0 - k) + k));
}

vec3 shadeLight(MaterialInputs m, Surface s, vec3 L, vec3 radiance) {
    if (m.unlit) {
        return vec3(0.0);
    }
    vec3 H = normalize(L + s.view);
    float ndl = max(dot(s.normal, L), 0.0);
    float ndv = max(dot(s.normal, s.view), 0.0001);
    float ndh = max(dot(s.normal, H), 0.0);
    float vdh = max(dot(s.view, H), 0.0);
    float alpha = m.roughness * m.roughness;

    float D;
    if (m.anisotropy != 0.0) {
        vec3 t = normalize(s.tangent + m.anisotropyDirection * 0.1);
        vec3 b = cross(s.normal, t);
        float at = max(alpha * (1.0 + m.anisotropy), 0.001);
        float ab = max(alpha * (1.0 - m.anisotropy), 0.001);
        float tdh = dot(t, H) / at;
        float bdh = dot(b, H) / ab;
        float d = tdh * tdh + bdh * bdh + ndh * ndh;
        D = 1.0 / (PI * at * ab * d * d);
    } else {
        D = distributionGgx(ndh, alpha);
    }

    vec3 F0 = mix(vec3(0.04), m.baseColor, m.metallic);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - vdh, 5.0);
    vec3 specular = D * visibilitySmith(ndl, ndv, alpha) * F / max(4.0 * ndl * ndv, 0.0001);

    vec3 diffuse;
    switch (m.lightingModel) {
        case 1:
            diffuse = m.baseColor * max((dot(s.normal, L) + 0.5) / 1.5, 0.0) / (PI * max(ndl, 0.0001));
            break;
        case 2:
            diffuse = m.baseColor * smoothstep(0.45, 0.55, ndl) / (PI * max(ndl, 0.0001));
            break;
        default:
            diffuse = m.baseColor / PI;
            break;
    }

    vec3 color = (diffuse * (1.0 - F) * (1.0 - m.metallic) + specular) * radiance * ndl;

    if (m.sheen > 0.0) {
        float sheenAlpha = max(m.roughness, 0.07);
        float sinH = sqrt(max(1.0 - ndh * ndh, 0.0));
        float sheenD = (2.0 + 1.0 / sheenAlpha) * pow(sinH, 1.0 / sheenAlpha) / (2.0 * PI);
        float sheenV = 1.0 / (4.0 * (ndl + ndv - ndl * ndv));
        color += m.sheenColor * m.sheen * sheenD * sheenV * radiance * ndl;
    }

    if (m.clearcoat > 0.0) {
        float coatAlpha = m.clearcoatRoughness * m.clearcoatRoughness;
        float coatD = distributionGgx(ndh, coatAlpha);
        float coatV = 0.25 / max(vdh * vdh, 0.0001);
        float coatF = 0.04 + 0.96 * pow(1.0 - vdh, 5.0);
        color = color * (1.0 - coatF * m.clearcoat) + coatD * coatV * coatF * m.clearcoat * radiance * ndl;
    }
    return color;
}

void main() {
    vec2 p = v_uv * 2.0 - 1.0;
    float scale = scene.camera.w;
    vec2 wave = vec2(sin(p.x * scale), cos(p.y * scale * 1.3));
    Surface s;
    s.position = vec3(p, 0.2 * wave.x * wave.y);
    s.normal = normalize(vec3(-0.2 * scale * cos(p.x * scale) * wave.y,
                              0.26 * scale * wave.x * sin(p.y * scale * 1.3), 1.0));
    s.tangent = normalize(cross(vec3(0.0, 1.0, 0.0), s.normal));
    s.view = normalize(scene.camera.xyz - s.position);

    MaterialInputs m = defaultMaterial();
    float pattern = 0.5 + 0.5 * sin(p.x * 9.0) * sin(p.y * 7.0);
    m.baseColor = mix(vec3(0.8, 0.3, 0.2), vec3(0.2, 0.5, 0.8), pattern);
    m.roughness = 0.2 + 0.6 * pattern;
    m.metallic = step(0.7, pattern);

    vec3 color = m.emissive;
    for (int l = 0; l < LIGHTS; l++) {
        vec3 toLight = scene.lightPositions[l].xyz - s.position;
        float distance = length(toLight);
        vec3 radiance = scene.lightColors[l].rgb * scene.lightColors[l].w / (distance * distance);
        color += shadeLight(m, s, toLight / distance, radiance);
    }
    color = color / (color + 1.0);
    fragColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
