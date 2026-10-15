@group(0) @binding(0) var<storage, read> palette: array<vec4<f32>>;

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) world_position: vec3<f32>,
    @location(1) world_normal: vec3<f32>,
}

fn transform(bone: u32, value: vec4<f32>) -> vec3<f32> {
    let base = bone * 3u;
    return vec3<f32>(dot(palette[base], value),
                     dot(palette[base + 1u], value),
                     dot(palette[base + 2u], value));
}

@vertex
fn main(@location(0) position: vec4<f32>, @location(1) normal: vec3<f32>,
        @location(2) weights: vec4<f32>, @location(3) joints: vec4<u32>,
        @builtin(instance_index) instance: u32) -> VertexOutput {
    let world_position = transform(joints.x, position) * weights.x
                       + transform(joints.y, position) * weights.y
                       + transform(joints.z, position) * weights.z
                       + transform(joints.w, position) * weights.w;
    let normal4 = vec4<f32>(normal, 0.0);
    let world_normal = transform(joints.x, normal4) * weights.x
                     + transform(joints.y, normal4) * weights.y
                     + transform(joints.z, normal4) * weights.z
                     + transform(joints.w, normal4) * weights.w;
    let offset = vec2<f32>(f32(instance & 1u), f32((instance >> 1u) & 1u)) - 0.5;
    var output: VertexOutput;
    output.position = vec4<f32>(world_position.xy * 0.375 + offset,
                                0.4 + world_position.z * 0.0625, 1.0);
    output.world_position = world_position;
    output.world_normal = world_normal;
    return output;
}
