export { statistics, randomGenerator, pairedRatio } from "./statistics.mjs";

// CPU expectations are independent of either shader. GGX uses a declared float
// tolerance; the other fixtures use dyadic data with exact representable results.
function words(values) {
    return new Uint32Array(values.buffer);
}

function gaussianCase(count) {
    if (count % 65536 !== 0) throw new Error("Gaussian inputs must contain whole 256×256 images");
    const source = new Float32Array(count * 4);
    const horizontal = new Float32Array(source.length);
    const result = new Float32Array(source.length);
    const kernel = [1, 4, 6, 4, 1];
    for (let index = 0; index < source.length; ++index) {
        const hash = Math.imul(index ^ (index >>> 8), 0x45D9F3B);
        source[index] = ((hash >>> 16) & 255) / 256;
    }
    // A separable CPU convolution is independent of the shaders' 25-tap form.
    for (let index = 0; index < count; ++index) {
        const x = index & 255;
        const row = index - x;
        for (let component = 0; component < 4; ++component) {
            let sum = 0;
            for (let tap = 0; tap < 5; ++tap) {
                const px = Math.max(0, Math.min(255, x + tap - 2));
                sum += source[(row + px) * 4 + component] * kernel[tap];
            }
            horizontal[index * 4 + component] = sum;
        }
    }
    for (let index = 0; index < count; ++index) {
        const x = index & 255;
        const y = (index >>> 8) & 255;
        const tile = index - y * 256 - x;
        for (let component = 0; component < 4; ++component) {
            let sum = 0;
            for (let tap = 0; tap < 5; ++tap) {
                const py = Math.max(0, Math.min(255, y + tap - 2));
                sum += horizontal[(tile + py * 256 + x) * 4 + component] * kernel[tap];
            }
            result[index * 4 + component] = sum / 256;
        }
    }
    return { inputs: [words(source)], expected: words(result) };
}

function skinningCase(count) {
    const positions = new Float32Array(count * 4);
    const weights = new Float32Array(count * 4);
    const joints = new Uint32Array(count * 4);
    const palette = new Float32Array(64 * 12);
    const result = new Float32Array(count * 4);
    for (let bone = 0; bone < 64; ++bone) {
        for (let row = 0; row < 3; ++row) {
            for (let column = 0; column < 4; ++column) {
                palette[bone * 12 + row * 4 + column] = column === row
                    ? 1 + (bone % 4) / 16 : ((bone * 3 + row + column) % 9 - 4) / 32;
            }
        }
    }
    for (let index = 0; index < count; ++index) {
        const base = index * 4;
        positions.set([(index % 103 - 51) / 16, (index % 67 - 33) / 16,
            (index % 31 - 15) / 8, 1], base);
        weights.set([0.125, 0.25, 0.375, 0.25], base);
        for (let influence = 0; influence < 4; ++influence) {
            joints[base + influence] = (index * 7 + influence * 13) % 64;
        }
        // Blend affine matrices first, then apply to a homogeneous position.
        for (let row = 0; row < 3; ++row) {
            let transformed = 0;
            for (let column = 0; column < 4; ++column) {
                let coefficient = 0;
                for (let influence = 0; influence < 4; ++influence) {
                    const bone = joints[base + influence];
                    coefficient += palette[bone * 12 + row * 4 + column] * weights[base + influence];
                }
                transformed += coefficient * positions[base + column];
            }
            result[base + row] = transformed;
        }
        result[base + 3] = 1;
    }
    return { inputs: [words(positions), words(weights), joints, words(palette)], expected: words(result) };
}

function pbrCase(count) {
    const surfaces = new Float32Array(count * 4);
    const materials = new Float32Array(count * 4);
    const positions = new Float32Array(count * 4);
    const lights = new Float32Array([2, 3, 4, 12, -3, 2, 2, 8, 0, -4, 3, 10, 4, 0, 1.5, 6]);
    const result = new Float32Array(count * 4);
    const dot = (left, right) => left.reduce((sum, value, index) => sum + value * right[index], 0);
    const unit = vector => {
        const length = Math.hypot(...vector);
        return vector.map(value => value / length);
    };
    for (let index = 0; index < count; ++index) {
        const base = index * 4;
        surfaces.set([(index % 17 - 8) / 16, (index % 23 - 11) / 16,
            0.5 + (index % 13) / 16, 0.25 + (index % 43) / 64], base);
        materials.set([(1 + index % 31) / 32, (1 + index % 19) / 32,
            (1 + index % 29) / 32, (index % 9) / 8], base);
        positions.set([(index % 97 - 48) / 24, (index % 71 - 35) / 24,
            (index % 37 - 18) / 24, 1], base);
        const point = [...positions.subarray(base, base + 3)];
        const normal = unit([...surfaces.subarray(base, base + 3)]);
        const view = unit([-point[0], -point[1], 6 - point[2]]);
        const roughness = surfaces[base + 3];
        const metallic = materials[base + 3];
        const albedo = [...materials.subarray(base, base + 3)];
        const alpha2 = roughness ** 4;
        const k = (roughness + 1) ** 2 / 8;
        const ndv = Math.max(dot(normal, view), 0.001);
        const gv = ndv / (ndv * (1 - k) + k);
        const f0 = albedo.map(value => 0.04 * (1 - metallic) + value * metallic);
        const color = albedo.map(value => value * 0.03);
        for (let light = 0; light < 4; ++light) {
            const delta = point.map((value, axis) => lights[light * 4 + axis] - value);
            const distance2 = Math.max(dot(delta, delta), 0.01);
            const direction = unit(delta);
            const halfway = unit(view.map((value, axis) => value + direction[axis]));
            const ndl = Math.max(dot(normal, direction), 0);
            const ndh = Math.max(dot(normal, halfway), 0);
            const vdh = Math.max(dot(view, halfway), 0);
            const distribution = alpha2 / (Math.PI * (ndh ** 2 * (alpha2 - 1) + 1) ** 2);
            const geometry = gv * ndl / (ndl * (1 - k) + k);
            const specularScale = distribution * geometry / Math.max(4 * ndv * ndl, 0.001);
            for (let component = 0; component < 3; ++component) {
                const fresnel = f0[component] + (1 - f0[component]) * (1 - vdh) ** 5;
                const diffuse = (1 - fresnel) * (1 - metallic) * albedo[component] / Math.PI;
                color[component] += (diffuse + fresnel * specularScale) * lights[light * 4 + 3] * ndl / distance2;
            }
        }
        result.set([...color.map(value => value / (1 + value)), 1], base);
    }
    return {
        inputs: [words(surfaces), words(materials), words(positions), words(lights)],
        expected: words(result),
        tolerance: { absolute: 0.0002, relative: 0.0002 },
    };
}

export function makeCase(name, count) {
    if (!Number.isSafeInteger(count) || count < 64 || count % 64 !== 0) {
        throw new Error("Element count must be a positive multiple of 64");
    }
    if (name === "gaussian_5x5") return gaussianCase(count);
    if (name === "skinning_4bone") return skinningCase(count);
    if (name === "pbr_4light") return pbrCase(count);
    let inputs;
    let expected;
    if (name === "saxpy") {
        const x = new Float32Array(count);
        const y = new Float32Array(count);
        const result = new Float32Array(count);
        for (let index = 0; index < count; ++index) {
            x[index] = (index % 128 - 64) * 0.25;
            y[index] = (index % 64 - 32) * 0.5;
            result[index] = x[index] * 2 + y[index];
        }
        inputs = [new Uint32Array(x.buffer), new Uint32Array(y.buffer)];
        expected = new Uint32Array(result.buffer);
    } else if (name === "hash_u32") {
        const source = new Uint32Array(count);
        expected = new Uint32Array(count);
        for (let index = 0; index < count; ++index) {
            source[index] = (Math.imul(index, 2654435761) + 0xFFFFFFFF) >>> 0;
            let value = source[index];
            value = Math.imul(value ^ (value >>> 16), 0x7FEB352D) >>> 0;
            value = Math.imul(value ^ (value >>> 15), 0x846CA68B) >>> 0;
            expected[index] = (value ^ (value >>> 16)) >>> 0;
        }
        inputs = [source];
    } else if (name === "affine_vec4") {
        const source = new Float32Array(count * 4);
        const result = new Float32Array(count * 4);
        const scale = [2, -1, 0.5, 4];
        const bias = [1, 2, -4, 0.25];
        for (let index = 0; index < source.length; ++index) {
            source[index] = (index % 128 - 64) * 0.25;
            result[index] = source[index] * scale[index % 4] + bias[index % 4];
        }
        inputs = [new Uint32Array(source.buffer)];
        expected = new Uint32Array(result.buffer);
    } else {
        throw new Error(`Unknown kernel: ${name}`);
    }
    return { inputs, expected };
}

export function mismatches(actual, expected) {
    const errors = [];
    if (actual.length !== expected.length) {
        return [{ length: actual.length, expected_length: expected.length }];
    }
    for (let index = 0; index < actual.length && errors.length < 8; ++index) {
        if (actual[index] !== expected[index]) {
            errors.push({ index, actual: actual[index], expected: expected[index] });
        }
    }
    return errors;
}

export function compareFloats(actualWords, expectedWords, tolerance) {
    if (actualWords.length !== expectedWords.length) {
        return { errors: [{ length: actualWords.length, expected_length: expectedWords.length }] };
    }
    const actual = new Float32Array(actualWords.buffer, actualWords.byteOffset, actualWords.length);
    const expected = new Float32Array(expectedWords.buffer, expectedWords.byteOffset, expectedWords.length);
    const errors = [];
    let maximumAbsoluteError = 0;
    for (let index = 0; index < actual.length; ++index) {
        const difference = Math.abs(actual[index] - expected[index]);
        maximumAbsoluteError = Math.max(maximumAbsoluteError, difference);
        const allowed = tolerance.absolute + tolerance.relative * Math.abs(expected[index]);
        if ((!Number.isFinite(actual[index]) || !Number.isFinite(expected[index]) || difference > allowed) && errors.length < 8) {
            errors.push({ index, actual: actual[index], expected: expected[index], allowed });
        }
    }
    return { errors, maximum_absolute_error: Number.isFinite(maximumAbsoluteError) ? maximumAbsoluteError : null };
}
