@vertex fn main(@builtin(vertex_index) id: u32) -> @builtin(position) vec4<f32> {
    let corner = vec2<f32>(f32((id << 1u) & 2u), f32(id & 2u));
    return vec4<f32>(corner * 2.0 - 1.0, 0.0, 1.0);
}
