@group(2) @binding(0) var layers: texture_2d_array<f32>;
@group(3) @binding(1) var nearestSampler: sampler;
@fragment fn main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let layer = i32(floor(position.x / 8.0 - 1.25 + 0.5));
    return textureSampleLevel(layers, nearestSampler, vec2<f32>(0.5), layer, 0.0);
}
