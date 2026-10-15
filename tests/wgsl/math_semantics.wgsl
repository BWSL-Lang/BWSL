@group(0) @binding(0) var<storage, read> floats: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read> integers: array<i32>;
@group(0) @binding(2) var<storage, read_write> result_floats: array<vec4<f32>>;
@group(0) @binding(3) var<storage, read_write> result_integers: array<i32>;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let index = global_id.x;
    let value = floats[index];
    let signed_value = integers[index];
    result_floats[index * 3u] = round(value);
    result_floats[index * 3u + 1u] = value - vec4<f32>(3.0) * floor(value / vec4<f32>(3.0));
    result_floats[index * 3u + 2u] = value % vec4<f32>(3.0);
    result_integers[index * 3u] = signed_value / 3;
    result_integers[index * 3u + 1u] = signed_value % 3;
    result_integers[index * 3u + 2u] = signed_value >> 2u;
}
