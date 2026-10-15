@group(0) @binding(0) var<storage, read> source: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> result: array<vec4<f32>>;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let value = source[global_id.x].wzyx;
    result[global_id.x] = value * 2.0 + vec4<f32>(1.0, 2.0, 3.0, 4.0);
}
