@group(0) @binding(0) var<storage, read> source: array<f32>;
@group(0) @binding(1) var<storage, read_write> result: array<i32>;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let bits = bitcast<i32>(source[global_id.x]);
    result[global_id.x] = (bits >> 8u) ^ i32(global_id.x);
}
