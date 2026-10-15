@group(0) @binding(0) var<storage, read> values: array<u32>;
@group(0) @binding(1) var<storage, read_write> result: array<u32>;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    let a = values[id.x];
    let b = (a ^ (a >> 16u)) * 0x7feb352du;
    let c = (b ^ (b >> 15u)) * 0x846ca68bu;
    result[id.x] = c ^ (c >> 16u);
}
