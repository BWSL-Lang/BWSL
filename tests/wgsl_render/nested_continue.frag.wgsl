@fragment fn main() -> @location(0) vec4<f32> {
    var sum = 0u;
    for (var i = 0u; i < 3u; i++) {
        for (var j = 0u; j < 6u; j++) {
            if (j == 1u) { continue; }
            if (j == 4u) { break; }
            sum += i * 10u + j;
        }
    }
    return vec4<f32>(f32(sum) / 255.0, 0.0, 0.0, 1.0);
}
