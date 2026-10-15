// Export the browser's independent CPU expectations for the native GPU oracle.
import { makeCase } from "./web/cases.mjs";

const [name, countText, iterationsText, warmupText] = process.argv.slice(2);
const count = Number(countText);
const data = makeCase(name, count);
const buffers = data.inputs.map((values, binding) => ({
    binding, data: [...values], expected: [...values],
}));
const output = { binding: buffers.length, data: Array(data.expected.length).fill(0x7FC00001) };
if (data.tolerance) {
    output.expected_f32 = [...new Float32Array(data.expected.buffer)];
    output.absolute_tolerance = data.tolerance.absolute;
    output.relative_tolerance = data.tolerance.relative;
} else {
    output.expected = [...data.expected];
}
buffers.push(output);
process.stdout.write(JSON.stringify({
    entry_point: null, workgroups: [count / 64, 1, 1], buffers,
    iterations: Number(iterationsText), warmup: Number(warmupText),
}));
