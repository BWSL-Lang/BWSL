struct Weight { @size(16) value: f32, }
struct Material {
    transform: mat4x4<f32>,
    tint: vec3<f32>,
    exposure: f32,
    weights: array<Weight, 4>,
    count: u32,
}
@group(1) @binding(0) var<uniform> material: Material;
struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
}
@vertex fn main(@builtin(vertex_index) id: u32) -> VertexOutput {
    let corner = vec2<f32>(f32((id << 1u) & 2u), f32(id & 2u));
    return VertexOutput(material.transform * vec4<f32>(corner * 2.0 - 1.0, 0.0, 1.0), corner);
}
