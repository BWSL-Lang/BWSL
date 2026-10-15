struct FragmentOutput {
    @location(0) color: vec4<f32>,
    @location(1) normal_roughness: vec4<f32>,
}

@fragment
fn main(@location(1) @interpolate(linear, center) uv: vec2<f32>) -> FragmentOutput {
    let t = uv * vec2<f32>(12.0, 9.0);
    let mixed = t.x * 2.3 + t.y * 1.7;
    let wave = sin(t.x) * cos(t.y) + 0.5 * sin(mixed);
    let dx = (cos(t.x) * cos(t.y) + 1.15 * cos(mixed)) * 12.0;
    let dy = (-sin(t.x) * sin(t.y) + 0.85 * cos(mixed)) * 9.0;
    let normal = normalize(vec3<f32>(-0.08 * dx, -0.08 * dy, 1.0));
    let light = normalize(vec3<f32>(0.4, 0.6, 1.0));
    let view = normalize(vec3<f32>((uv - 0.5) * 0.8, 1.5));
    let diffuse = max(dot(normal, light), 0.0);
    let specular = pow(max(dot(reflect(-light, normal), view), 0.0), 32.0);
    let base = mix(vec3<f32>(0.07, 0.14, 0.24), vec3<f32>(0.75, 0.43, 0.12), smoothstep(-0.7, 0.9, wave));
    let edge = abs(fract(uv * 32.0) - 0.5);
    let footprint = fwidth(uv * 32.0);
    let grid = 1.0 - smoothstep(footprint.x * 0.5, footprint.x * 1.5, min(edge.x, edge.y));
    let color = (base * (0.15 + 0.85 * diffuse) + specular * 0.25) * (0.8 + 0.2 * grid);
    var output: FragmentOutput;
    output.color = vec4<f32>(color, 1.0);
    output.normal_roughness = vec4<f32>(normal * 0.5 + 0.5, 0.6);
    return output;
}
