// Four-influence linear blend skinning; three affine rows per bone.
@group(0) @binding(0) var<storage, read> positions: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read> weights: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read> joints: array<vec4<u32>>;
@group(0) @binding(3) var<storage, read> palette: array<vec4<f32>>;
@group(0) @binding(4) var<storage, read_write> result: array<vec4<f32>>;

fn transform(bone: u32, position: vec4<f32>) -> vec3<f32> {
    let base = bone * 3u;
    return vec3<f32>(dot(palette[base], position),
                     dot(palette[base + 1u], position),
                     dot(palette[base + 2u], position));
}

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    let position = positions[id.x];
    let influence = weights[id.x];
    let bones = joints[id.x];
    let blended = transform(bones.x, position) * influence.x
                + transform(bones.y, position) * influence.y
                + transform(bones.z, position) * influence.z
                + transform(bones.w, position) * influence.w;
    result[id.x] = vec4<f32>(blended, 1.0);
}
