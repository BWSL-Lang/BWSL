#version 300 es
precision highp float;
precision highp int;

#define SPHERES 12
#define LIGHTS 3
#define BOUNCES 3

layout(std140) uniform ub_scene {
    vec4 cameraPosition;
    vec4 cameraForward;
    vec4 cameraRight;
    vec4 cameraUp;
    vec4 spheres[SPHERES];        // xyz center, w radius
    vec4 materials[SPHERES];      // rgb albedo, w roughness
    vec4 lightPositions[LIGHTS];
    vec4 lightColors[LIGHTS];     // rgb color, w intensity
    vec4 sky;                     // rgb horizon, w aspect
} scene;

in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

struct Ray {
    vec3 origin;
    vec3 direction;
};

struct Hit {
    float t;
    vec3 position;
    vec3 normal;
    int material;
    bool found;
};

struct Material {
    vec3 albedo;
    float roughness;
    float metallic;
    vec3 emission;
};

struct Surface {
    Hit hit;
    Material material;
    vec3 view;
};

struct Lighting {
    vec3 diffuse;
    vec3 specular;
    vec3 ambient;
};

Hit intersectScene(Ray ray) {
    Hit hit = Hit(1.0e9, vec3(0.0), vec3(0.0), 0, false);
    for (int i = 0; i < SPHERES; i++) {
        vec4 sphere = scene.spheres[i];
        vec3 oc = ray.origin - sphere.xyz;
        float b = dot(oc, ray.direction);
        float c = dot(oc, oc) - sphere.w * sphere.w;
        float h = b * b - c;
        if (h > 0.0) {
            float t = -b - sqrt(h);
            if (t > 0.001 && t < hit.t) {
                hit.t = t;
                hit.material = i;
                hit.found = true;
            }
        }
    }
    if (hit.found) {
        hit.position = ray.origin + ray.direction * hit.t;
        hit.normal = normalize(hit.position - scene.spheres[hit.material].xyz);
    }
    return hit;
}

Material materialOf(int index) {
    vec4 data = scene.materials[index];
    return Material(data.rgb, data.w, fract(float(index) * 0.37), data.rgb * 0.1);
}

Lighting shade(Surface surface) {
    Lighting lighting = Lighting(vec3(0.0), vec3(0.0), surface.material.albedo * 0.03);
    float shininess = 2.0 / max(surface.material.roughness * surface.material.roughness, 0.002);
    for (int l = 0; l < LIGHTS; l++) {
        vec3 toLight = scene.lightPositions[l].xyz - surface.hit.position;
        float distance = length(toLight);
        vec3 L = toLight / distance;
        Hit blocker = intersectScene(Ray(surface.hit.position + surface.hit.normal * 0.002, L));
        float visible = (blocker.found && blocker.t < distance) ? 0.0 : 1.0;
        vec4 light = scene.lightColors[l];
        vec3 radiance = light.rgb * light.w * visible / (1.0 + distance * distance * 0.1);
        float ndl = max(dot(surface.hit.normal, L), 0.0);
        vec3 H = normalize(L + surface.view);
        float ndh = max(dot(surface.hit.normal, H), 0.0);
        lighting.diffuse += surface.material.albedo * radiance * ndl * (1.0 - surface.material.metallic);
        lighting.specular += radiance * pow(ndh, shininess) * ndl;
    }
    return lighting;
}

void main() {
    vec2 p = v_uv * 2.0 - 1.0;
    Ray ray = Ray(scene.cameraPosition.xyz,
                  normalize(scene.cameraForward.xyz * 1.6 + scene.cameraRight.xyz * (p.x * scene.sky.w)
                            + scene.cameraUp.xyz * p.y));
    vec3 color = vec3(0.0);
    vec3 throughput = vec3(1.0);
    for (int bounce = 0; bounce < BOUNCES; bounce++) {
        Hit hit = intersectScene(ray);
        if (!hit.found) {
            float horizon = clamp(ray.direction.y * 0.5 + 0.5, 0.0, 1.0);
            color += throughput * mix(scene.sky.rgb, vec3(0.15, 0.25, 0.5), horizon);
            break;
        }
        Surface surface = Surface(hit, materialOf(hit.material), -ray.direction);
        Lighting lighting = shade(surface);
        color += throughput * (lighting.diffuse + lighting.specular + lighting.ambient);
        throughput *= mix(vec3(0.04), surface.material.albedo, surface.material.metallic)
                      * (1.0 - surface.material.roughness * 0.5);
        ray = Ray(hit.position + hit.normal * 0.002, reflect(ray.direction, hit.normal));
    }
    color = color / (color + 1.0);
    fragColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
