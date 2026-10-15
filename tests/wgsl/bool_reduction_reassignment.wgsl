@group(0) @binding(0) var<storage, read> source: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> result: array<u32>;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let index = global_id.x;
    var flags = source[index] > vec4<f32>(0.0);
    flags = source[index] < vec4<f32>(0.0);
    var value = select(0u, 1u, any(flags)) + select(0u, 2u, all(flags));
    flags = source[index] > vec4<f32>(1.0);
    value = value + select(0u, 4u, any(flags)) + select(0u, 8u, all(flags));
    result[index] = value;
}
