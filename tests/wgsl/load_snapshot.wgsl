@group(0) @binding(0) var<storage, read_write> result: array<u32>;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    let original = result[id.x];
    let delayed = ((original * 3u) + 7u) ^ 11u;
    result[id.x] = 19u;
    result[id.x] = delayed + result[id.x];
}
