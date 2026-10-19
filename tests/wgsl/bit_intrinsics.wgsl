@group(0) @binding(0) var<storage, read> source: array<i32>;
@group(0) @binding(1) var<storage, read_write> result: array<i32>;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let index = global_id.x;
    let value = source[index];
    let bits = bitcast<u32>(value);
    result[index * 5u] = countOneBits(value);
    result[index * 5u + 1u] = firstLeadingBit(value);
    result[index * 5u + 2u] = firstTrailingBit(value);
    result[index * 5u + 3u] = bitcast<i32>(firstLeadingBit(bits & ~1u));
    result[index * 5u + 4u] = bitcast<i32>(firstTrailingBit(bits));
}
