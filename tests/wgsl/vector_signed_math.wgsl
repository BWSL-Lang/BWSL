@group(0) @binding(0) var<storage, read> source: array<vec4<i32>>;
@group(0) @binding(1) var<storage, read_write> result: array<vec4<i32>>;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let index = global_id.x;
    let value = source[index];
    result[index * 5u] = value / vec4<i32>(3);
    result[index * 5u + 1u] = value % vec4<i32>(3);
    result[index * 5u + 2u] = value >> vec4<u32>(2u);
    result[index * 5u + 3u] = bitcast<vec4<i32>>(bitcast<vec4<u32>>(value) >> vec4<u32>(2u));
    result[index * 5u + 4u] = bitcast<vec4<i32>>(vec4<u32>(0u) - bitcast<vec4<u32>>(value));
}
