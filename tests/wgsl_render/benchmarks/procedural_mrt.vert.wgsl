struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) unused: vec3<f32>,
    @location(1) @interpolate(linear, center) uv: vec2<f32>,
}

@vertex
fn main(@builtin(vertex_index) id: u32) -> VertexOutput {
    let point = vec2<f32>(f32((id << 1u) & 2u), f32(id & 2u)) * 2.0 - 1.0;
    let w = 1.0 + f32(id) * 0.5;
    var output: VertexOutput;
    output.position = vec4<f32>(point * w, 0.5 * w, w);
    output.unused = vec3<f32>(0.25, 0.5, 0.75);
    output.uv = point * 0.5 + 0.5;
    return output;
}
