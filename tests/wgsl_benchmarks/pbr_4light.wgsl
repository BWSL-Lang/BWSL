// Cook-Torrance GGX distribution, Smith geometry, Schlick Fresnel, Reinhard map.
@group(0) @binding(0) var<storage, read> normal_roughness: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read> albedo_metallic: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read> positions: array<vec4<f32>>;
@group(0) @binding(3) var<storage, read> lights: array<vec4<f32>>;
@group(0) @binding(4) var<storage, read_write> result: array<vec4<f32>>;

fn contribution(light: vec4<f32>, position: vec3<f32>, normal: vec3<f32>,
                view: vec3<f32>, albedo: vec3<f32>, metallic: f32,
                alpha2: f32, k: f32, ndv: f32, gv: f32, f0: vec3<f32>) -> vec3<f32> {
    let delta = light.xyz - position;
    let distance2 = max(dot(delta, delta), 0.01);
    let direction = normalize(delta);
    let halfway = normalize(view + direction);
    let ndl = max(dot(normal, direction), 0.0);
    let ndh = max(dot(normal, halfway), 0.0);
    let vdh = max(dot(view, halfway), 0.0);
    let denominator = ndh * ndh * (alpha2 - 1.0) + 1.0;
    let distribution = alpha2 / (3.14159265 * denominator * denominator);
    let geometry = gv * ndl / (ndl * (1.0 - k) + k);
    let fresnel_base = 1.0 - vdh;
    let fresnel2 = fresnel_base * fresnel_base;
    let fresnel5 = fresnel2 * fresnel2 * fresnel_base;
    let fresnel = f0 + (vec3<f32>(1.0) - f0) * fresnel5;
    let specular = fresnel * (distribution * geometry / max(4.0 * ndv * ndl, 0.001));
    let diffuse = (vec3<f32>(1.0) - fresnel) * ((1.0 - metallic) / 3.14159265) * albedo;
    return (diffuse + specular) * (light.w * ndl / distance2);
}

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    let surface = normal_roughness[id.x];
    let material = albedo_metallic[id.x];
    let position = positions[id.x].xyz;
    let normal = normalize(surface.xyz);
    let view = normalize(vec3<f32>(0.0, 0.0, 6.0) - position);
    let roughness = surface.w;
    let metallic = material.w;
    let albedo = material.xyz;
    let alpha = roughness * roughness;
    let alpha2 = alpha * alpha;
    let k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    let ndv = max(dot(normal, view), 0.001);
    let gv = ndv / (ndv * (1.0 - k) + k);
    let f0 = vec3<f32>(0.04) * (1.0 - metallic) + albedo * metallic;
    var color = albedo * 0.03;
    for (var light = 0u; light < 4u; light++) {
        color += contribution(lights[light], position, normal, view, albedo,
                              metallic, alpha2, k, ndv, gv, f0);
    }
    result[id.x] = vec4<f32>(color / (vec3<f32>(1.0) + color), 1.0);
}
