// Diagnostic control: identical 25 taps, explicitly unrolled instead of nested loops.
@group(0) @binding(0) var<storage, read> image: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> result: array<vec4<f32>>;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    let index = id.x;
    let tile = index & 0xffff0000u;
    let x = i32(index & 255u);
    let y = i32((index >> 8u) & 255u);
    let x0 = u32(clamp(x + (-2), 0, 255));
    let x1 = u32(clamp(x + (-1), 0, 255));
    let x2 = u32(clamp(x + (0), 0, 255));
    let x3 = u32(clamp(x + (1), 0, 255));
    let x4 = u32(clamp(x + (2), 0, 255));
    let y0 = u32(clamp(y + (-2), 0, 255));
    let y1 = u32(clamp(y + (-1), 0, 255));
    let y2 = u32(clamp(y + (0), 0, 255));
    let y3 = u32(clamp(y + (1), 0, 255));
    let y4 = u32(clamp(y + (2), 0, 255));
    var sum = vec4<f32>(0.0);
    sum += image[tile + y0 * 256u + x0] * 1.0;
    sum += image[tile + y0 * 256u + x1] * 4.0;
    sum += image[tile + y0 * 256u + x2] * 6.0;
    sum += image[tile + y0 * 256u + x3] * 4.0;
    sum += image[tile + y0 * 256u + x4] * 1.0;
    sum += image[tile + y1 * 256u + x0] * 4.0;
    sum += image[tile + y1 * 256u + x1] * 16.0;
    sum += image[tile + y1 * 256u + x2] * 24.0;
    sum += image[tile + y1 * 256u + x3] * 16.0;
    sum += image[tile + y1 * 256u + x4] * 4.0;
    sum += image[tile + y2 * 256u + x0] * 6.0;
    sum += image[tile + y2 * 256u + x1] * 24.0;
    sum += image[tile + y2 * 256u + x2] * 36.0;
    sum += image[tile + y2 * 256u + x3] * 24.0;
    sum += image[tile + y2 * 256u + x4] * 6.0;
    sum += image[tile + y3 * 256u + x0] * 4.0;
    sum += image[tile + y3 * 256u + x1] * 16.0;
    sum += image[tile + y3 * 256u + x2] * 24.0;
    sum += image[tile + y3 * 256u + x3] * 16.0;
    sum += image[tile + y3 * 256u + x4] * 4.0;
    sum += image[tile + y4 * 256u + x0] * 1.0;
    sum += image[tile + y4 * 256u + x1] * 4.0;
    sum += image[tile + y4 * 256u + x2] * 6.0;
    sum += image[tile + y4 * 256u + x3] * 4.0;
    sum += image[tile + y4 * 256u + x4] * 1.0;
    result[index] = sum / 256.0;
}
