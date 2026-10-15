@group(0) @binding(0) var<storage, read> particles: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read> colors: array<vec4<f32>>;
struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) local: vec2<f32>,
    @location(1) @interpolate(flat) tint: vec4<f32>,
    @location(2) @interpolate(flat) id: u32,
}

@vertex
fn main(@builtin(instance_index) instance: u32, @builtin(vertex_index) vertex: u32) -> VertexOutput {
    let point = particles[instance];
    let local = vec2<f32>(f32(vertex & 1u), f32((vertex >> 1u) & 1u)) * 2.0 - 1.0;
    var output: VertexOutput;
    output.position = vec4<f32>(point.xy + local * point.w, point.z, 1.0);
    output.local = local;
    output.tint = colors[instance];
    output.id = instance;
    return output;
}
