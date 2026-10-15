@fragment
fn main(@location(0) local: vec2<f32>, @location(1) @interpolate(flat) tint: vec4<f32>,
        @location(2) @interpolate(flat) id: u32) -> @location(0) vec4<f32> {
    let radius = max(1.0 - dot(local, local), 0.0);
    let hash = id * 1664525u + 1013904223u;
    let twinkle = 0.75 + f32((hash >> 24u) & 255u) / 1024.0;
    return vec4<f32>(tint.xyz * twinkle, tint.w * radius * radius * radius);
}
