const clearColor = [0.03125, 0.046875, 0.078125, 1];
const lights = new Float32Array([2, 3, 4, 12, -3, 2, 2, 8, 0, -4, 3, 10, 4, 0, 1.5, 6]);
const clamp = (value, low = 0, high = 1) => Math.max(low, Math.min(high, value));
const dot = (left, right) => left.reduce((sum, value, index) => sum + value * right[index], 0);
const unit = vector => {
    const length = Math.hypot(...vector);
    return vector.map(value => value / length);
};
const quantize = value => Math.round(clamp(value) * 255);
const smoothstep = (low, high, value) => {
    const t = clamp((value - low) / (high - low));
    return t * t * (3 - 2 * t);
};

export function shadeGgx(point, normalInput) {
    const normal = unit(normalInput);
    const view = unit([-point[0], -point[1], 6 - point[2]]);
    const roughness = clamp(0.45 + point[1] * 0.1, 0.25, 0.8);
    const metallic = 0.35;
    const albedo = [0.5 + point[0] * 0.25, 0.35 + point[1] * 0.2, 0.6];
    const alpha2 = roughness ** 4;
    const k = (roughness + 1) ** 2 / 8;
    const ndv = Math.max(dot(normal, view), 0.001);
    const gv = ndv / (ndv * (1 - k) + k);
    const f0 = albedo.map(value => 0.04 * (1 - metallic) + value * metallic);
    const color = albedo.map(value => value * 0.03);
    for (let light = 0; light < 4; ++light) {
        const delta = point.map((value, axis) => lights[light * 4 + axis] - value);
        const direction = unit(delta);
        const halfway = unit(view.map((value, axis) => value + direction[axis]));
        const ndl = Math.max(dot(normal, direction), 0);
        const ndh = Math.max(dot(normal, halfway), 0);
        const vdh = Math.max(dot(view, halfway), 0);
        const distribution = alpha2 / (Math.PI * (ndh ** 2 * (alpha2 - 1) + 1) ** 2);
        const geometry = gv * ndl / (ndl * (1 - k) + k);
        const scale = distribution * geometry / Math.max(4 * ndv * ndl, 0.001);
        for (let component = 0; component < 3; ++component) {
            const fresnel = f0[component] + (1 - f0[component]) * (1 - vdh) ** 5;
            const diffuse = (1 - fresnel) * (1 - metallic) * albedo[component] / Math.PI;
            color[component] += (diffuse + fresnel * scale) * lights[light * 4 + 3] * ndl / Math.max(dot(delta, delta), 0.01);
        }
    }
    return [...color.map(value => value / (1 + value)), 1];
}

function meshScene(size) {
    const subdivisions = 64;
    const side = subdivisions + 1;
    const count = side * side;
    const stride = 60;
    const vertices = new ArrayBuffer(count * stride);
    const floats = new Float32Array(vertices);
    const integers = new Uint32Array(vertices);
    const palette = new Float32Array(64 * 12);
    for (let bone = 0; bone < 64; ++bone) {
        palette.set([1 + (bone % 3 - 1) / 128, (bone % 5 - 2) / 256, 0, (bone % 7 - 3) / 256,
            (bone % 3 - 1) / 256, 1 + (bone % 5 - 2) / 128, 0, (bone % 11 - 5) / 512,
            0, 0, 1, (bone % 3 - 1) / 512], bone * 12);
    }
    const worldPositions = [];
    const worldNormals = [];
    for (let y = 0; y < side; ++y) {
        for (let x = 0; x < side; ++x) {
            const index = y * side + x;
            const offset = index * 15;
            const px = x / 32 - 1;
            const py = y / 32 - 1;
            const position = [px, py, px * py / 8, 1];
            const normal = unit([-py / 8, -px / 8, 1]).map(Math.fround);
            const weights = [0.125, 0.25, 0.375, 0.25];
            const bones = [0, 1, 2, 3].map(influence => (index * 7 + influence * 13) % 64);
            floats.set([...position, ...normal, ...weights], offset);
            integers.set(bones, offset + 11);
            const transformed = [0, 0, 0];
            const transformedNormal = [0, 0, 0];
            for (let row = 0; row < 3; ++row) {
                // Blend matrix coefficients first, independently of shader dot products.
                for (let column = 0; column < 4; ++column) {
                    let coefficient = 0;
                    for (let influence = 0; influence < 4; ++influence) {
                        coefficient += palette[bones[influence] * 12 + row * 4 + column] * weights[influence];
                    }
                    transformed[row] += coefficient * position[column];
                    if (column < 3) transformedNormal[row] += coefficient * normal[column];
                }
            }
            worldPositions.push(transformed);
            worldNormals.push(transformedNormal);
        }
    }
    const indices = new Uint32Array(subdivisions * subdivisions * 6);
    let cursor = 0;
    for (let y = 0; y < subdivisions; ++y) {
        for (let x = 0; x < subdivisions; ++x) {
            const first = y * side + x;
            indices.set([first, first + 1, first + side, first + 1, first + side + 1, first + side], cursor);
            cursor += 6;
        }
    }
    const probes = [];
    for (let y = 0; y < 9; ++y) {
        for (let x = 0; x < 9; ++x) {
            const pixelX = Math.floor((x + 0.37) * size / 9);
            const pixelY = Math.floor((y + 0.61) * size / 9);
            const sx = 2 * (pixelX + 0.5) / size - 1;
            const sy = 1 - 2 * (pixelY + 0.5) / size;
            let depth = 1;
            let expected = clearColor;
            let edgeDistance = 1;
            for (let instance = 0; instance < 4; ++instance) {
                const offset = [(instance & 1) - 0.5, ((instance >>> 1) & 1) - 0.5];
                const px = (sx - offset[0]) / 0.375;
                const py = (sy - offset[1]) / 0.375;
                for (let triangle = 0; triangle < indices.length; triangle += 3) {
                    const ids = [...indices.subarray(triangle, triangle + 3)];
                    const [a, b, c] = ids.map(id => worldPositions[id]);
                    if (px < Math.min(a[0], b[0], c[0]) || px > Math.max(a[0], b[0], c[0]) ||
                        py < Math.min(a[1], b[1], c[1]) || py > Math.max(a[1], b[1], c[1])) continue;
                    const denominator = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
                    const u = ((b[1] - c[1]) * (px - c[0]) + (c[0] - b[0]) * (py - c[1])) / denominator;
                    const v = ((c[1] - a[1]) * (px - c[0]) + (a[0] - c[0]) * (py - c[1])) / denominator;
                    const w = 1 - u - v;
                    if (Math.min(u, v, w) < 0) continue;
                    const weights = [u, v, w];
                    const position = [0, 1, 2].map(axis => weights.reduce((sum, weight, i) => sum + weight * worldPositions[ids[i]][axis], 0));
                    const z = 0.4 + position[2] * 0.0625;
                    if (z >= depth) continue;
                    depth = z;
                    edgeDistance = Math.min(u, v, w);
                    const normal = [0, 1, 2].map(axis => weights.reduce((sum, weight, i) => sum + weight * worldNormals[ids[i]][axis], 0));
                    expected = shadeGgx(position, normal);
                }
            }
            // Coverage-edge rules vary in their subpixel rounding. Independent
            // color probes deliberately stay inside triangles; full images are
            // still compared between shader variants, including every edge.
            if (edgeDistance > 0.03) {
                probes.push({ x: pixelX, y: pixelY, color: expected.map(quantize), foreground: depth < 1 });
            }
        }
    }
    if (probes.filter(probe => probe.foreground).length < 16) throw new Error("Insufficient independent mesh probes");
    return {
        clear: clearColor, storage: [palette, lights], vertices, indices,
        vertexLayouts: [{ arrayStride: stride, attributes: [
            { shaderLocation: 0, offset: 0, format: "float32x4" },
            { shaderLocation: 1, offset: 16, format: "float32x3" },
            { shaderLocation: 2, offset: 28, format: "float32x4" },
            { shaderLocation: 3, offset: 44, format: "uint32x4" },
        ] }],
        draw: { indices: indices.length, instances: 4 },
        configuration: { subdivisions, vertices: count, triangles: indices.length / 3 * 4, instances: 4 },
        probes,
    };
}

export function proceduralPixel(x, y, size) {
    const uv = [(x + 0.5) / size, 1 - (y + 0.5) / size];
    const tx = uv[0] * 12;
    const ty = uv[1] * 9;
    const mixed = tx * 2.3 + ty * 1.7;
    const wave = Math.sin(tx) * Math.cos(ty) + 0.5 * Math.sin(mixed);
    const dx = (Math.cos(tx) * Math.cos(ty) + 1.15 * Math.cos(mixed)) * 12;
    const dy = (-Math.sin(tx) * Math.sin(ty) + 0.85 * Math.cos(mixed)) * 9;
    const normal = unit([-0.08 * dx, -0.08 * dy, 1]);
    const light = unit([0.4, 0.6, 1]);
    const view = unit([(uv[0] - 0.5) * 0.8, (uv[1] - 0.5) * 0.8, 1.5]);
    const diffuse = Math.max(dot(normal, light), 0);
    const reflected = normal.map((value, i) => 2 * dot(light, normal) * value - light[i]);
    const specular = Math.max(dot(reflected, view), 0) ** 32;
    const t = smoothstep(-0.7, 0.9, wave);
    const base = [0.07, 0.14, 0.24].map((value, i) => value * (1 - t) + [0.75, 0.43, 0.12][i] * t);
    const edge = uv.map(value => Math.abs(value * 32 - Math.floor(value * 32) - 0.5));
    const footprint = 32 / size;
    const grid = 1 - smoothstep(footprint * 0.5, footprint * 1.5, Math.min(...edge));
    const color = base.map(value => (value * (0.15 + 0.85 * diffuse) + specular * 0.25) * (0.8 + 0.2 * grid));
    return [[...color, 1], [...normal.map(value => value * 0.5 + 0.5), 0.6]].map(values => values.map(quantize));
}

function particleScene(size) {
    const cells = 64;
    const layers = 4;
    const count = cells * cells * layers;
    const positions = new Float32Array(count * 4);
    const colors = new Float32Array(count * 4);
    for (let instance = 0; instance < count; ++instance) {
        const cell = instance % (cells * cells);
        const layer = Math.floor(instance / (cells * cells));
        positions.set([((cell % cells) + 0.5) / 32 - 1,
            (Math.floor(cell / cells) + 0.5) / 32 - 1, 0.5, (5 + layer * 0.25) / 512], instance * 4);
        colors.set([0.25 + (instance % 17) / 32, 0.125 + (instance % 13) / 32,
            0.5 + (instance % 7) / 16, 0.375], instance * 4);
    }
    const reference = new Uint8Array(size * size * 4);
    for (let y = 0; y < size; ++y) {
        for (let x = 0; x < size; ++x) {
            const sx = 2 * (x + 0.5) / size - 1;
            const sy = 1 - 2 * (y + 0.5) / size;
            const cx = Math.min(cells - 1, Math.floor((sx + 1) * 32));
            const cy = Math.min(cells - 1, Math.floor((sy + 1) * 32));
            let color = clearColor.map(quantize);
            for (let layer = 0; layer < layers; ++layer) {
                const id = layer * cells * cells + cy * cells + cx;
                const px = (sx - positions[id * 4]) / positions[id * 4 + 3];
                const py = (sy - positions[id * 4 + 1]) / positions[id * 4 + 3];
                if (Math.abs(px) > 1 || Math.abs(py) > 1) continue;
                const radius = Math.max(1 - px * px - py * py, 0);
                const alpha = colors[id * 4 + 3] * radius ** 3;
                const hash = (Math.imul(id, 1664525) + 1013904223) >>> 0;
                const twinkle = 0.75 + (hash >>> 24) / 1024;
                color = color.map((destination, component) => component === 3
                    ? quantize(alpha + destination / 255 * (1 - alpha))
                    : quantize(colors[id * 4 + component] * twinkle * alpha + destination / 255 * (1 - alpha)));
            }
            reference.set(color, (y * size + x) * 4);
        }
    }
    return {
        clear: clearColor, storage: [positions, colors], vertexLayouts: [],
        draw: { vertices: 4, instances: count }, reference: [reference],
        configuration: { particles: count, layers, cells },
    };
}

function interfaceScene(size) {
    const vertices = new ArrayBuffer(5 * 40);
    const floats = new Float32Array(vertices);
    const integers = new Uint32Array(vertices);
    for (let vertex = 0; vertex < 3; ++vertex) {
        const id = vertex + 2;
        const w = vertex + 1;
        const point = [[-1, -1], [3, -1], [-1, 3]][vertex];
        floats.set([point[0] * w - id * 0.125, point[1] * w, 0.75 * w, w,
            point[0] * 0.5 + 0.5, point[1] * 0.5 + 0.5, 0, 0, 1], id * 10);
        integers[id * 10 + 9] = 9;
    }
    const color = new Uint8Array(size * size * 4);
    const material = new Uint32Array(size * size * 4);
    for (let y = 0; y < size; ++y) {
        for (let x = 0; x < size; ++x) {
            const offset = (y * size + x) * 4;
            color.set([(x + 0.5) / size, 1 - (y + 0.5) / size, 0.75, 1].map(quantize), offset);
            material.set([14, 7, 9, 1], offset);
        }
    }
    return {
        clear: clearColor, depthClear: 0.6, storage: [], vertices,
        vertexLayouts: [{ arrayStride: 40, attributes: [
            { shaderLocation: 0, offset: 0, format: "float32x4" },
            { shaderLocation: 1, offset: 16, format: "float32x2" },
            { shaderLocation: 2, offset: 24, format: "float32x3" },
            { shaderLocation: 3, offset: 36, format: "uint32" },
        ] }],
        draw: { vertices: 3, instances: 1, firstVertex: 2, firstInstance: 5 },
        reference: [color, null, new Uint8Array(material.buffer)],
        configuration: { regression: "sparse interfaces, perspective divisor, depth, MRT, output snapshots, builtin offsets" },
    };
}

function texturedScene(size) {
    const width = 64;
    const data = new Uint8Array(width * width * 4);
    for (let y = 0; y < width; ++y) {
        for (let x = 0; x < width; ++x) {
            data.set([(x * 7 + y * 3) % 256, (x * 5 + y * 11) % 256,
                ((x >>> 3) ^ (y >>> 3)) & 1 ? 220 : 40, 255], (y * width + x) * 4);
        }
    }
    // Existing BWSL std140 ABI: vec3+scalar pack at byte 64, scalar array
    // elements occupy 16 bytes, and count follows at byte 144.
    const material = new ArrayBuffer(160);
    const floats = new Float32Array(material);
    const words = new Uint32Array(material);
    floats.set([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
    floats.set([0.8, 0.6, 1.0, 0.75], 16);
    const weights = [0.125, 0.25, 0.375, 0.25];
    weights.forEach((weight, index) => { floats[20 + index * 4] = weight; });
    words[36] = 4;
    const reference = new Uint8Array(size * size * 4);
    for (let y = 0; y < size; ++y) {
        for (let x = 0; x < size; ++x) {
            const u = (x + 0.5) / size;
            const v = 1 - (y + 0.5) / size;
            const pixel = (y * size + x) * 4;
            if (u < 0.125 && v < 0.125) {
                reference.set(clearColor.map(quantize), pixel);
                continue;
            }
            const color = [0, 0, 0];
            for (let tap = 0; tap < 4; ++tap) {
                const tx = (Math.floor(u * width) + tap % 2) % width;
                const ty = (Math.floor(v * width) + Math.floor(tap / 2)) % width;
                for (let channel = 0; channel < 3; ++channel) {
                    color[channel] += data[(ty * width + tx) * 4 + channel] / 255 * weights[tap];
                }
            }
            const shaded = color.map((value, channel) => value * floats[16 + channel] * floats[19] * (u > 0.5 ? 0.5 : 1));
            reference.set([...shaded, 1].map(quantize), pixel);
        }
    }
    return { clear: clearColor, storage: [], uniforms: [new Uint8Array(material)],
        textures: [{ binding: 1, width, height: width, data }], vertexLayouts: [], draw: { vertices: 3, instances: 1 },
        reference: [reference], configuration: { triangles: 1, textureSize: width, uniformBytes: 160, taps: 4 } };
}

export function makeScene(name, size) {
    if (!Number.isInteger(size) || size < 64 || size % 64 !== 0) throw new Error("Use a resolution divisible by 64");
    if (name === "texture_array_layers") {
        const colors = [[225, 30, 15, 255], [15, 210, 30, 255], [30, 40, 230, 255]];
        const data = new Uint8Array(64 * 64 * 4 * colors.length);
        for (let layer = 0; layer < colors.length; ++layer) {
            for (let pixel = 0; pixel < 64 * 64; ++pixel) data.set(colors[layer], (layer * 64 * 64 + pixel) * 4);
        }
        const reference = new Uint8Array(size * size * 4);
        for (let y = 0; y < size; ++y) {
            for (let x = 0; x < size; ++x) {
                const layer = Math.max(0, Math.min(2, Math.floor((x + 0.5) / 8 - 0.75)));
                reference.set(colors[layer], (y * size + x) * 4);
            }
        }
        return { clear: clearColor, storage: [], textures: [{ binding: 0, width: 64, height: 64, layers: 3, data }],
            vertexLayouts: [], draw: { vertices: 3, instances: 1 }, reference: [reference],
            configuration: { triangles: 1, arrayLayers: 3 } };
    }
    if (name === "nested_continue") {
        const reference = new Uint8Array(size * size * 4);
        for (let pixel = 0; pixel < size * size; ++pixel) reference.set([105, 0, 0, 255], pixel * 4);
        return { clear: clearColor, storage: [], vertexLayouts: [], draw: { vertices: 3, instances: 1 },
            reference: [reference], configuration: { triangles: 1, nestedLoopSum: 105 } };
    }
    if (name === "textured_uniforms") return texturedScene(size);
    if (name === "skinned_ggx") return meshScene(size);
    if (name === "particles") return particleScene(size);
    if (name === "interfaces") return interfaceScene(size);
    if (name === "procedural_mrt") {
        const reference = [new Uint8Array(size * size * 4), new Uint8Array(size * size * 4)];
        for (let y = 0; y < size; ++y) {
            for (let x = 0; x < size; ++x) {
                const pixel = proceduralPixel(x, y, size);
                for (let target = 0; target < 2; ++target) reference[target].set(pixel[target], (y * size + x) * 4);
            }
        }
        return { clear: clearColor, storage: [], vertexLayouts: [], draw: { vertices: 3, instances: 1 },
            reference, configuration: { triangles: 1, colorAttachments: 2 } };
    }
    throw new Error(`Unknown render scene: ${name}`);
}

export function compareImages(actual, expected, tolerance) {
    if (actual.length !== expected.length) return { passed: false, error: "Image byte lengths differ" };
    let maximumError = 0;
    let differentBytes = 0;
    const errors = [];
    for (let index = 0; index < actual.length; ++index) {
        const error = Math.abs(actual[index] - expected[index]);
        maximumError = Math.max(maximumError, error);
        if (error > tolerance) {
            ++differentBytes;
            if (errors.length < 8) errors.push({ index, actual: actual[index], expected: expected[index] });
        }
    }
    return { passed: differentBytes === 0, maximum_byte_error: maximumError, outside_tolerance_bytes: differentBytes,
        compared_bytes: actual.length, tolerance_bytes: tolerance, errors };
}

export function checkReference(scene, images, size, targets) {
    const comparisons = [];
    if (scene.reference) {
        for (let target = 0; target < images.length; ++target) {
            if (!images[target]) continue;
            const tolerance = targets[target] === "rgba32uint" ? 0 : 3;
            comparisons.push(compareImages(images[target], scene.reference[target], tolerance));
        }
    }
    if (scene.probes) {
        const errors = [];
        let maximum = 0;
        for (const probe of scene.probes) {
            const offset = (probe.y * size + probe.x) * 4;
            const result = compareImages(images[0].subarray(offset, offset + 4), Uint8Array.from(probe.color), 3);
            maximum = Math.max(maximum, result.maximum_byte_error);
            if (!result.passed) errors.push({ x: probe.x, y: probe.y, expected: probe.color, actual: [...images[0].subarray(offset, offset + 4)] });
        }
        comparisons.push({ passed: errors.length === 0, probes: scene.probes.length,
            foreground_probes: scene.probes.filter(probe => probe.foreground).length,
            maximum_byte_error: maximum, tolerance_bytes: 3, errors });
    }
    return { passed: comparisons.every(comparison => comparison.passed), comparisons };
}
