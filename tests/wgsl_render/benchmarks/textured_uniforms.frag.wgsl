struct Weight { @size(16) value: f32, }
struct Material {
    transform: mat4x4<f32>,
    tint: vec3<f32>,
    exposure: f32,
    weights: array<Weight, 4>,
    count: u32,
}
@group(1) @binding(0) var<uniform> material: Material;
@group(2) @binding(1) var colorTexture: texture_2d<f32>;
@group(3) @binding(2) var nearestSampler: sampler;
@fragment fn main(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
    if (uv.x < 0.125 && uv.y < 0.125) { discard; }
    var color = vec4<f32>(0.0);
    for (var tap = 0u; tap < material.count; tap++) {
        let offset = vec2<f32>(f32(tap & 1u), f32(tap >> 1u)) / 64.0;
        color += textureSampleLevel(colorTexture, nearestSampler, fract(uv + offset), 0.0) * material.weights[tap].value;
    }
    var shaded = color.rgb * material.tint * material.exposure;
    if (uv.x > 0.5) { shaded *= 0.5; }
    return vec4<f32>(shaded, 1.0);
}
