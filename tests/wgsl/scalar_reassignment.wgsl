@group(0) @binding(0) var<storage, read> source: array<u32>;
@group(0) @binding(1) var<storage, read_write> result: array<u32>;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    var value = source[global_id.x];
    value = value + 17u;
    value = value * 3u;
    value = value ^ (value >> 4u);
    result[global_id.x] = value;
}
