// Handwritten reference, with the same binding ABI and arithmetic as BWSL.
@group(0) @binding(0) var<storage, read> x: array<f32>;
@group(0) @binding(1) var<storage, read> y: array<f32>;
@group(0) @binding(2) var<storage, read_write> result: array<f32>;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    let scaled = x[id.x] * 2.0;
    result[id.x] = scaled + y[id.x];
}
