@group(0) @binding(5) var<storage, read_write> result: array<u32>;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    result[global_id.x] = global_id.x * 7u + 1u;
}
