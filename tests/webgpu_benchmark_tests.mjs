import assert from "node:assert/strict";
import test from "node:test";
import { compareFloats, makeCase, mismatches, pairedRatio, statistics } from "../tools/wgsl-bench/web/cases.mjs";

test("SAXPY expectations preserve exact float32 values", () => {
    const data = makeCase("saxpy", 128);
    const values = new Float32Array(data.expected.buffer);
    assert.deepEqual([values[0], values[63], values[64], values[127]], [-48, 15, -16, 47]);
    assert.equal(data.inputs.length, 2);
});

test("vector expectations preserve component layout", () => {
    const data = makeCase("affine_vec4", 64);
    const values = new Float32Array(data.expected.buffer);
    assert.deepEqual([...values.subarray(0, 4)], [-31, 17.75, -11.75, -60.75]);
    assert.equal(values.length, 64 * 4);
});

test("integer hash expectations agree with arbitrary precision arithmetic", () => {
    const data = makeCase("hash_u32", 1024);
    const mask = 0xFFFFFFFFn;
    for (const index of [0, 1, 63, 64, 127, 511, 1023]) {
        let value = (BigInt(index) * 2654435761n + mask) & mask;
        assert.equal(data.inputs[0][index], Number(value));
        value = ((value ^ (value >> 16n)) * 0x7FEB352Dn) & mask;
        value = ((value ^ (value >> 15n)) * 0x846CA68Bn) & mask;
        value ^= value >> 16n;
        assert.equal(data.expected[index], Number(value));
    }
});

test("statistics retain raw samples and use nearest-rank p95", () => {
    const samples = [4, 1, 3, 2];
    assert.deepEqual(statistics(samples), { median_us: 2.5, min_us: 1, p95_us: 4, samples_us: samples });
    assert.throws(() => statistics([]));
    assert.throws(() => statistics([NaN]));
    assert.equal(pairedRatio([], []), null);
    assert.equal(pairedRatio([0, 1], [1, 1]), null);
    const ratio = pairedRatio([1, 2, 3, 4], [2, 4, 6, 8]);
    assert.equal(ratio.median, 2);
    assert.deepEqual(ratio.bootstrap_95_interval, [2, 2]);
});

test("readback comparison detects missing writes and corrupt inputs", () => {
    assert.deepEqual(mismatches(new Uint32Array([0, 7]), new Uint32Array([0, 7])), []);
    assert.deepEqual(mismatches(new Uint32Array([0xA5A5A5A5]), new Uint32Array([0])),
        [{ index: 0, actual: 0xA5A5A5A5, expected: 0 }]);
    assert.notEqual(mismatches(new Uint32Array(1), new Uint32Array(2)).length, 0);
    assert.throws(() => makeCase("saxpy", 65));
    assert.throws(() => makeCase("unknown", 64));
});

test("separable Gaussian oracle agrees with direct 25-tap convolution at edges and tile boundaries", () => {
    const data = makeCase("gaussian_5x5", 131072);
    const source = new Float32Array(data.inputs[0].buffer);
    const expected = new Float32Array(data.expected.buffer);
    const weights = [1, 4, 6, 4, 1];
    for (const index of [0, 1, 255, 256, 32897, 65535, 65536, 131071]) {
        const tile = Math.floor(index / 65536) * 65536;
        const x = index % 256;
        const y = Math.floor((index - tile) / 256);
        for (let component = 0; component < 4; ++component) {
            let sum = 0;
            for (let dy = -2; dy <= 2; ++dy) {
                for (let dx = -2; dx <= 2; ++dx) {
                    const px = Math.max(0, Math.min(255, x + dx));
                    const py = Math.max(0, Math.min(255, y + dy));
                    sum += source[(tile + py * 256 + px) * 4 + component] * weights[dy + 2] * weights[dx + 2];
                }
            }
            assert.equal(expected[index * 4 + component], sum / 256);
        }
    }
    assert.throws(() => makeCase("gaussian_5x5", 64));
});

test("matrix-blend skinning oracle agrees with separately transformed weighted vertices", () => {
    const data = makeCase("skinning_4bone", 128);
    const [positions, weights, joints, palette] = data.inputs.map((words, index) => index === 2
        ? words : new Float32Array(words.buffer));
    const expected = new Float32Array(data.expected.buffer);
    assert.equal(palette.length, 64 * 12);
    for (const vertex of [0, 1, 63, 64, 127]) {
        for (let row = 0; row < 3; ++row) {
            let sum = 0;
            for (let influence = 0; influence < 4; ++influence) {
                let transformed = 0;
                const base = joints[vertex * 4 + influence] * 12 + row * 4;
                for (let column = 0; column < 4; ++column) {
                    transformed += palette[base + column] * positions[vertex * 4 + column];
                }
                sum += transformed * weights[vertex * 4 + influence];
            }
            assert.equal(expected[vertex * 4 + row], sum);
        }
        assert.equal(expected[vertex * 4 + 3], 1);
    }
});

test("GGX expectations stay finite and tone mapped; tolerance rejects NaNs and incorrect output", () => {
    const data = makeCase("pbr_4light", 128);
    const values = new Float32Array(data.expected.buffer);
    assert(values.every(value => Number.isFinite(value) && value >= 0 && value <= 1));
    assert.notEqual(values[0], values[4]);
    assert.equal(values[3], 1);
    const actual = data.expected.slice();
    const floats = new Float32Array(actual.buffer);
    floats[0] += 0.00001;
    assert.deepEqual(compareFloats(actual, data.expected, data.tolerance).errors, []);
    floats[0] += 0.01;
    assert.equal(compareFloats(actual, data.expected, data.tolerance).errors.length, 1);
    actual[0] = 0x7FC00001;
    assert.equal(compareFloats(actual, data.expected, data.tolerance).errors.length, 1);
    assert.equal(compareFloats(actual.subarray(1), data.expected, data.tolerance).errors.length, 1);
});
