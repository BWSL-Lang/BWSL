struct FragmentOutput {
    @location(0) color: vec4<f32>,
    @location(2) material: vec4<u32>,
    @builtin(frag_depth) depth: f32,
}

@fragment
fn main(@location(1) @interpolate(linear, center) uv: vec2<f32>,
        @location(2) @interpolate(flat) material: u32,
        @builtin(position) position: vec4<f32>) -> FragmentOutput {
    var output: FragmentOutput;
    output.color = vec4<f32>(uv, position.z, 1.0);
    let snapshot = output.color;
    output.color = vec4<f32>(0.0);
    output.color = snapshot;
    output.material = vec4<u32>(material, 7u, 9u, 1u);
    output.depth = 0.5;
    return output;
}
