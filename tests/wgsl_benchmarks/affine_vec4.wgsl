@group(0) @binding(0) var<storage, read> values: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> result: array<vec4<f32>>;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    let scale = vec4<f32>(2.0, -1.0, 0.5, 4.0);
    let bias = vec4<f32>(1.0, 2.0, -4.0, 0.25);
    result[id.x] = values[id.x] * scale + bias;
}
