import assert from "node:assert/strict";
import test from "node:test";
import { makeScene, checkReference, compareImages, shadeGgx } from "../tools/wgsl-bench/render-web/scenes.mjs";

test("interface reference exercises nonzero builtin offsets, varying W, sparse integer MRT and late depth", () => {
    const scene = makeScene("interfaces", 64);
    assert.deepEqual(scene.draw, { vertices: 3, instances: 1, firstVertex: 2, firstInstance: 5 });
    assert.equal(scene.depthClear, 0.6);
    const vertices = new Float32Array(scene.vertices);
    assert.deepEqual([...vertices.subarray(20, 24)], [-1.25, -1, 0.75, 1]);
    assert.equal(vertices[33], 2);
    assert.deepEqual([...new Uint32Array(scene.reference[2].buffer).subarray(0, 4)], [14, 7, 9, 1]);
    assert.equal(scene.reference[0][2], 191);
    assert(checkReference(scene, scene.reference, 64, ["rgba8unorm", null, "rgba32uint"]).passed);
});

test("procedural reference includes two distinct images and a normal/roughness attachment", () => {
    const scene = makeScene("procedural_mrt", 64);
    assert.equal(scene.reference.length, 2);
    assert.notDeepEqual(scene.reference[0], scene.reference[1]);
    assert.equal(scene.reference[0].length, 64 * 64 * 4);
    for (let pixel = 0; pixel < 64 * 64; ++pixel) {
        assert.equal(scene.reference[0][pixel * 4 + 3], 255);
        assert.equal(scene.reference[1][pixel * 4 + 3], 153);
    }
    const broken = scene.reference.map(image => image.slice());
    broken[1][4] ^= 255;
    assert(!checkReference(scene, broken, 64, ["rgba8unorm", "rgba8unorm"]).passed);
});

test("particle reference has four overlapping layers and detects a blank image", () => {
    const scene = makeScene("particles", 64);
    assert.equal(scene.draw.instances, 16384);
    assert.equal(scene.configuration.layers, 4);
    assert.equal(scene.storage[0].length, 16384 * 4);
    const blank = new Uint8Array(scene.reference[0].length).fill(0);
    assert(!checkReference(scene, [blank], 64, ["rgba8unorm"]).passed);
    assert(scene.reference[0].some((value, index) => index % 4 !== 3 && value > 30));
});

test("skinned mesh references have bounded bone indices, normalized weights and interior raster probes", () => {
    const scene = makeScene("skinned_ggx", 128);
    assert.equal(scene.configuration.triangles, 32768);
    assert.equal(scene.vertices.byteLength, 65 * 65 * 60);
    const floats = new Float32Array(scene.vertices);
    const integers = new Uint32Array(scene.vertices);
    for (let vertex = 0; vertex < 65 * 65; ++vertex) {
        const base = vertex * 15;
        assert.equal([...floats.subarray(base + 7, base + 11)].reduce((a, b) => a + b, 0), 1);
        assert([...integers.subarray(base + 11, base + 15)].every(bone => bone < 64));
    }
    assert(scene.probes.filter(probe => probe.foreground).length >= 16);
    const color = shadeGgx([0, 0, 0], [0, 0, 1]);
    assert(color.every(value => value >= 0 && value <= 1));
    assert.equal(color[3], 1);
});

test("image comparison preserves integer exactness and rejects errors above the declared RGBA8 tolerance", () => {
    assert(compareImages(Uint8Array.from([14, 7, 9, 1]), Uint8Array.from([14, 7, 9, 1]), 0).passed);
    assert(!compareImages(Uint8Array.from([15, 7, 9, 1]), Uint8Array.from([14, 7, 9, 1]), 0).passed);
    assert(compareImages(Uint8Array.from([17, 7]), Uint8Array.from([14, 7]), 3).passed);
    assert(!compareImages(Uint8Array.from([18, 7]), Uint8Array.from([14, 7]), 3).passed);
    assert(!compareImages(new Uint8Array(4), new Uint8Array(8), 3).passed);
    assert.throws(() => makeScene("particles", 65));
    assert.throws(() => makeScene("missing", 64));
});

test("textured material reference includes std140 padding, independent sampling and discarded coverage", () => {
    const scene = makeScene("textured_uniforms", 64);
    assert.equal(scene.uniforms[0].byteLength, 160);
    const floats = new Float32Array(scene.uniforms[0].buffer);
    const words = new Uint32Array(scene.uniforms[0].buffer);
    assert.equal(floats[19], 0.75);
    assert.equal(floats[20], 0.125);
    assert.equal(floats[24], 0.25);
    assert.equal(words[36], 4);
    assert.equal(scene.textures[0].data.byteLength, 64 * 64 * 4);
    assert(checkReference(scene, scene.reference, 64, ["rgba8unorm"]).passed);
    const broken = scene.reference[0].slice();
    broken[1000] ^= 255;
    assert(!checkReference(scene, [broken], 64, ["rgba8unorm"]).passed);
    assert.deepEqual([...makeScene("nested_continue", 64).reference[0].subarray(0, 4)], [105, 0, 0, 255]);
});

test("texture arrays round fractional layers and clamp before selecting independent colors", () => {
    const scene = makeScene("texture_array_layers", 64);
    const pixel = x => [...scene.reference[0].subarray(x * 4, x * 4 + 4)];
    assert.equal(scene.textures[0].layers, 3);
    assert.deepEqual(pixel(0), [225, 30, 15, 255]);
    assert.deepEqual(pixel(16), [15, 210, 30, 255]);
    assert.deepEqual(pixel(63), [30, 40, 230, 255]);
    assert(checkReference(scene, scene.reference, 64, ["rgba8unorm"]).passed);
});
