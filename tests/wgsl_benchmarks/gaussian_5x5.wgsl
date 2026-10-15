// Independent 256x256 RGBA images, with clamped edges and a 5x5 binomial kernel.
@group(0) @binding(0) var<storage, read> image: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> result: array<vec4<f32>>;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    let index = id.x;
    let tile = index & 0xffff0000u;
    let x = i32(index & 255u);
    let y = i32((index >> 8u) & 255u);
    let weights = array<f32, 5>(1.0, 4.0, 6.0, 4.0, 1.0);
    var sum = vec4<f32>(0.0);
    for (var row = 0; row < 5; row++) {
        let py = u32(clamp(y + row - 2, 0, 255));
        for (var column = 0; column < 5; column++) {
            let px = u32(clamp(x + column - 2, 0, 255));
            sum += image[tile + py * 256u + px] * (weights[row] * weights[column]);
        }
    }
    result[index] = sum / 256.0;
}
