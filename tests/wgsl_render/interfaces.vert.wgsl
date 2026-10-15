struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) unused: vec3<f32>,
    @location(1) @interpolate(linear, center) uv: vec2<f32>,
    @location(2) @interpolate(flat) material: u32,
}

@vertex
fn main(@location(0) position: vec4<f32>, @location(1) uv: vec2<f32>,
        @location(2) normal: vec3<f32>, @location(3) material: u32,
        @builtin(vertex_index) vertex: u32, @builtin(instance_index) instance: u32) -> VertexOutput {
    var output: VertexOutput;
    output.position = position;
    output.position.x += f32(vertex) * 0.125;
    output.unused = normal;
    output.uv = uv;
    output.material = material + instance;
    return output;
}
