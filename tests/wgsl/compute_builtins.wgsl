@group(0) @binding(0) var<storage, read_write> result: array<u32>;

@compute @workgroup_size(4, 2, 1)
fn main(
    @builtin(global_invocation_id) global_id: vec3<u32>,
    @builtin(local_invocation_id) local_id: vec3<u32>,
    @builtin(workgroup_id) workgroup_id: vec3<u32>,
    @builtin(num_workgroups) num_workgroups: vec3<u32>,
) {
    let index = global_id.x + global_id.y * 12u;
    let value = local_id.x + local_id.y * 10u
        + workgroup_id.x * 100u + workgroup_id.y * 1000u
        + num_workgroups.x * 10000u + num_workgroups.y * 100000u;
    result[index] = value;
}
