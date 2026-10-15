import { makeScene, checkReference, compareImages } from "./scenes.mjs";
import { statistics, pairedRatio, randomGenerator } from "./statistics.mjs";

const names = ["handwritten", "bwsl"];
const status = document.querySelector("#status");
let manifest;
let stopped = false;
let latestReport;

function active() {
    if (stopped || document.hidden) throw new Error("Cancelled: keep this tab visible; the current GPU batch has finished");
}

function tableRow(id, values) {
    const row = document.createElement("tr");
    for (const value of values) {
        const cell = document.createElement("td");
        cell.textContent = String(value);
        row.append(cell);
    }
    document.querySelector(id).append(row);
}

function buffer(device, data, usage) {
    const bytes = data instanceof ArrayBuffer ? new Uint8Array(data) : new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    const result = device.createBuffer({ size: bytes.length, usage: usage | GPUBufferUsage.COPY_SRC, mappedAtCreation: true });
    new Uint8Array(result.getMappedRange()).set(bytes);
    result.unmap();
    return { gpu: result, expected: bytes };
}

async function hash(bytes) {
    return [...new Uint8Array(await crypto.subtle.digest("SHA-256", bytes))].map(value => value.toString(16).padStart(2, "0")).join("");
}

async function readBuffer(device, source) {
    const staging = device.createBuffer({ size: source.expected.length, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    try {
        const encoder = device.createCommandEncoder();
        encoder.copyBufferToBuffer(source.gpu, 0, staging, 0, source.expected.length);
        device.queue.submit([encoder.finish()]);
        await staging.mapAsync(GPUMapMode.READ);
        const bytes = new Uint8Array(staging.getMappedRange()).slice();
        staging.unmap();
        return bytes;
    } finally {
        staging.destroy();
    }
}

async function readImage(device, texture, format, size, layers = 1) {
    const pixelBytes = format === "rgba32uint" ? 16 : 4;
    const rowBytes = size * pixelBytes;
    const padded = Math.ceil(rowBytes / 256) * 256;
    const staging = device.createBuffer({ size: padded * size * layers, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    try {
        const encoder = device.createCommandEncoder();
        encoder.copyTextureToBuffer({ texture }, { buffer: staging, bytesPerRow: padded, rowsPerImage: size }, [size, size, layers]);
        device.queue.submit([encoder.finish()]);
        await staging.mapAsync(GPUMapMode.READ);
        const mapped = new Uint8Array(staging.getMappedRange());
        const image = new Uint8Array(rowBytes * size * layers);
        for (let row = 0; row < size * layers; ++row) image.set(mapped.subarray(row * padded, row * padded + rowBytes), row * rowBytes);
        staging.unmap();
        return image;
    } finally {
        staging.destroy();
    }
}

class RenderTimer {
    constructor(device, timestamps, fixture, scene, size) {
        this.device = device;
        this.timestamps = timestamps;
        this.fixture = fixture;
        this.scene = scene;
        this.size = size;
        this.attachments = fixture.targets.map(format => format ? device.createTexture({
            size: [size, size], format, usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC,
        }) : null);
        this.views = this.attachments.map(texture => texture?.createView() ?? null);
        this.depth = fixture.depth ? device.createTexture({ size: [size, size], format: "depth24plus", usage: GPUTextureUsage.RENDER_ATTACHMENT }) : null;
        this.depthView = this.depth?.createView();
        this.storage = scene.storage.map(data => buffer(device, data, GPUBufferUsage.STORAGE));
        this.vertex = scene.vertices ? buffer(device, scene.vertices, GPUBufferUsage.VERTEX) : null;
        this.index = scene.indices ? buffer(device, scene.indices, GPUBufferUsage.INDEX) : null;
        this.uniforms = (scene.uniforms ?? []).map(data => buffer(device, data, GPUBufferUsage.UNIFORM));
        this.sampledTextures = (scene.textures ?? []).map(description => {
            const texture = device.createTexture({ size: [description.width, description.height, description.layers ?? 1], format: "rgba8unorm",
                usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.COPY_SRC });
            device.queue.writeTexture({ texture }, description.data,
                { bytesPerRow: description.width * 4, rowsPerImage: description.height }, [description.width, description.height, description.layers ?? 1]);
            return { gpu: texture, description };
        });
        const entries = new Map();
        for (const [stage, data] of Object.entries(fixture.variants.bwsl)) {
            for (const resource of data.interface.resources) {
                const key = `${resource.group}:${resource.binding}`;
                const entry = entries.get(key) ?? { binding: resource.binding, visibility: 0, description: resource };
                entry.visibility |= stage === "vertex" ? GPUShaderStage.VERTEX : GPUShaderStage.FRAGMENT;
                entries.set(key, entry);
            }
        }
        const maximumGroup = Math.max(0, ...[...entries.values()].map(entry => entry.description.group));
        const layouts = [];
        this.bindings = [];
        for (let group = 0; group <= maximumGroup; ++group) {
            const resources = [...entries.values()].filter(entry => entry.description.group === group);
            const descriptors = resources.map(({ description, ...entry }) => {
                if (description.type === "storage_buffer") entry.buffer = { type: "read-only-storage" };
                else if (description.type === "uniform_buffer") entry.buffer = { type: "uniform", minBindingSize: description.byteSize };
                else if (description.type === "texture") entry.texture = { sampleType: description.sampleType, viewDimension: description.viewDimension };
                else if (description.type === "sampler") entry.sampler = { type: description.comparison ? "comparison" : "filtering" };
                else throw new Error(`Unsupported render resource: ${description.type}`);
                return entry;
            });
            const layout = device.createBindGroupLayout({ entries: descriptors });
            layouts.push(layout);
            this.bindings.push(device.createBindGroup({ layout, entries: resources.map(({ description }) => {
                let resource;
                if (description.type === "storage_buffer") resource = { buffer: this.storage[description.binding].gpu };
                else if (description.type === "uniform_buffer") resource = { buffer: this.uniforms[description.binding].gpu };
                else if (description.type === "texture") resource = this.sampledTextures.find(texture => texture.description.binding === description.binding).gpu.createView({ dimension: description.viewDimension });
                else resource = device.createSampler(description.comparison ? { compare: "less-equal" } : {});
                return { binding: description.binding, resource };
            }) }));
        }
        this.layout = device.createPipelineLayout({ bindGroupLayouts: layouts });
        if (timestamps) {
            this.queries = device.createQuerySet({ type: "timestamp", count: 2 });
            this.resolve = device.createBuffer({ size: 16, usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC });
            this.readback = device.createBuffer({ size: 16, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
        }
    }

    pipelineDescriptor(modules) {
        const blend = this.fixture.blend ? {
            color: { operation: "add", srcFactor: "src-alpha", dstFactor: "one-minus-src-alpha" },
            alpha: { operation: "add", srcFactor: "one", dstFactor: "one-minus-src-alpha" },
        } : undefined;
        return {
            layout: this.layout,
            vertex: { module: modules.vertex, entryPoint: "main", buffers: this.scene.vertexLayouts },
            fragment: { module: modules.fragment, entryPoint: "main",
                targets: this.fixture.targets.map(format => format ? { format, blend } : null) },
            primitive: { topology: this.fixture.topology, cullMode: "none" },
            depthStencil: this.fixture.depth ? { format: "depth24plus", depthWriteEnabled: true, depthCompare: "less" } : undefined,
        };
    }

    async measure(pipeline, frames) {
        active();
        const start = performance.now();
        const encoder = this.device.createCommandEncoder();
        for (let frame = 0; frame < frames; ++frame) {
            const descriptor = {
                colorAttachments: this.views.map((view, index) => view ? {
                    view, loadOp: "clear", storeOp: "store",
                    clearValue: this.fixture.targets[index] === "rgba32uint" ? [165, 165, 165, 165] : this.scene.clear,
                } : null),
                depthStencilAttachment: this.depthView ? { view: this.depthView, depthClearValue: this.scene.depthClear ?? 1,
                    depthLoadOp: "clear", depthStoreOp: "store" } : undefined,
            };
            if (this.timestamps && (frame === 0 || frame === frames - 1)) {
                descriptor.timestampWrites = { querySet: this.queries };
                if (frame === 0) descriptor.timestampWrites.beginningOfPassWriteIndex = 0;
                if (frame === frames - 1) descriptor.timestampWrites.endOfPassWriteIndex = 1;
            }
            const pass = encoder.beginRenderPass(descriptor);
            pass.setPipeline(pipeline);
            this.bindings.forEach((bindings, group) => pass.setBindGroup(group, bindings));
            if (this.vertex) pass.setVertexBuffer(0, this.vertex.gpu);
            const draw = this.scene.draw;
            if (this.index) {
                pass.setIndexBuffer(this.index.gpu, "uint32");
                pass.drawIndexed(draw.indices, draw.instances);
            } else {
                pass.draw(draw.vertices, draw.instances, draw.firstVertex ?? 0, draw.firstInstance ?? 0);
            }
            pass.end();
        }
        if (this.timestamps) {
            encoder.resolveQuerySet(this.queries, 0, 2, this.resolve, 0);
            encoder.copyBufferToBuffer(this.resolve, 0, this.readback, 0, 16);
        }
        this.device.queue.submit([encoder.finish()]);
        await this.device.queue.onSubmittedWorkDone();
        const wall = (performance.now() - start) * 1000;
        let gpu = null;
        if (this.timestamps) {
            await this.readback.mapAsync(GPUMapMode.READ);
            const values = new BigUint64Array(this.readback.getMappedRange());
            const difference = values[1] - values[0];
            this.readback.unmap();
            if (difference < 0n) throw new Error("GPU timestamps moved backwards");
            gpu = Number(difference) / 1000;
        }
        return { wall_us: wall, gpu_batch_us: gpu };
    }

    async verify() {
        let inputBytes = 0;
        for (const source of [...this.storage, ...this.uniforms, this.vertex, this.index].filter(Boolean)) {
            const actual = await readBuffer(this.device, source);
            if (!compareImages(actual, source.expected, 0).passed) throw new Error("Render pipeline modified an input buffer");
            inputBytes += actual.length;
        }
        for (const texture of this.sampledTextures) {
            const actual = await readImage(this.device, texture.gpu, "rgba8unorm", texture.description.width, texture.description.layers ?? 1);
            if (!compareImages(actual, texture.description.data, 0).passed) throw new Error("Shader modified a sampled texture");
            inputBytes += actual.length;
        }
        const images = [];
        const hashes = [];
        for (let index = 0; index < this.attachments.length; ++index) {
            const texture = this.attachments[index];
            const image = texture ? await readImage(this.device, texture, this.fixture.targets[index], this.size) : null;
            images.push(image);
            hashes.push(image ? await hash(image) : null);
        }
        const reference = checkReference(this.scene, images, this.size, this.fixture.targets);
        if (!reference.passed) throw new Error(`CPU image/reference mismatch: ${JSON.stringify(reference)}`);
        return { images, correctness: { ...reference, checked_input_bytes: inputBytes, attachment_sha256: hashes } };
    }

    destroy() {
        for (const source of [...this.storage, ...this.uniforms, this.vertex, this.index].filter(Boolean)) source.gpu.destroy();
        for (const texture of this.attachments) texture?.destroy();
        this.sampledTextures.forEach(texture => texture.gpu.destroy());
        this.depth?.destroy();
        this.queries?.destroy();
        this.resolve?.destroy();
        this.readback?.destroy();
    }
}

async function compile(device, timer, fixture, variant) {
    active();
    const start = performance.now();
    const response = await fetch("./compile", {
        method: "POST", headers: { "Content-Type": "application/json", "X-BWSL-Result-Token": manifest.result_token },
        body: JSON.stringify({ case: fixture.name, variant }),
    });
    if (!response.ok) throw new Error(`Source request failed: ${await response.text()}`);
    const source = await response.json();
    const requestEnd = performance.now();
    const modules = {};
    const stages = {};
    const browserStart = performance.now();
    for (const stage of ["vertex", "fragment"]) {
        if (source.stages[stage].sha256 !== fixture.variants[variant][stage].sha256) throw new Error("Render source changed");
        const moduleStart = performance.now();
        modules[stage] = device.createShaderModule({ code: source.stages[stage].source });
        const info = await modules[stage].getCompilationInfo();
        const errors = info.messages.filter(message => message.type === "error");
        if (errors.length) throw new Error(`${stage}: ${errors.map(error => error.message).join("\n")}`);
        stages[stage] = { module_ready_us: (performance.now() - moduleStart) * 1000 };
    }
    const pipelineStart = performance.now();
    const pipeline = await device.createRenderPipelineAsync(timer.pipelineDescriptor(modules));
    const end = performance.now();
    return { pipeline, timing: {
        full_source_to_ready_us: (end - start) * 1000, source_request_us: (requestEnd - start) * 1000,
        server_source_us: source.server_source_us, browser_ready_us: (end - browserStart) * 1000,
        pipeline_async_us: (end - pipelineStart) * 1000, stages,
        component_sum_us: source.server_source_us + (end - browserStart) * 1000,
        bwsl_reported_phases_us: source.bwsl_reported_phases_us,
    } };
}

function compareVariants(reference, candidate, targets) {
    const comparisons = candidate.map((image, index) => image
        ? compareImages(image, reference[index], targets[index] === "rgba32uint" ? 0 : 3) : null);
    if (comparisons.some(comparison => comparison && !comparison.passed)) {
        throw new Error(`Rendered shader variants disagree: ${JSON.stringify(comparisons)}`);
    }
    return comparisons;
}

async function runCase(device, fixture, settings, timestamps, random) {
    status.textContent = `${fixture.label}: generating independent CPU scene/reference…`;
    const size = fixture.validation_only ? 64 : settings.resolution;
    const scene = makeScene(fixture.name, size);
    const timer = new RenderTimer(device, timestamps, fixture, scene, size);
    const result = {
        status: "running", validation_only: !!fixture.validation_only, workload: { resolution: size, ...scene.configuration },
        bwsl_source_sha256: fixture.bwsl_sha256,
        source_bytes: Object.fromEntries(names.map(name => [name,
            Object.fromEntries(Object.entries(fixture.variants[name]).map(([stage, data]) =>
                [stage, new TextEncoder().encode(data.source).length]))])),
        reference_validation: fixture.reference_validation,
        source_hashes: Object.fromEntries(names.map(name => [name,
            Object.fromEntries(Object.entries(fixture.variants[name]).map(([stage, data]) => [stage, data.sha256]))])),
        compilation: Object.fromEntries(names.map(name => [name, { first_observed: null, repeated: [] }])),
        variants: {}, calibration: [], sample_order: [],
    };
    const offset = random() < 0.5 ? 0 : 1;
    const order = iteration => (iteration + offset) % 2 ? [...names].reverse() : names;
    const pipelines = {};
    const images = {};
    try {
        status.textContent = `${fixture.label}: measuring both stages and render pipeline readiness…`;
        const repetitions = fixture.validation_only ? 1 : settings.compilation_repetitions + 3;
        for (let iteration = 0; iteration < repetitions; ++iteration) {
            for (const name of order(iteration)) {
                const compiled = await compile(device, timer, fixture, name);
                pipelines[name] = compiled.pipeline;
                if (iteration === 0) result.compilation[name].first_observed = compiled.timing;
                if (iteration >= 3) result.compilation[name].repeated.push(compiled.timing);
            }
        }
        for (const name of names) {
            await timer.measure(pipelines[name], 1);
            const verified = await timer.verify();
            images[name] = verified.images;
            result.variants[name] = { correctness_before: verified.correctness, wall_samples: [], gpu_samples: [] };
        }
        result.cross_variant_comparison = compareVariants(images.handwritten, images.bwsl, fixture.targets);
        if (!fixture.validation_only) {
            let frames = 1;
            while (true) {
                const measurements = [];
                for (let probe = 0; probe < 3; ++probe) measurements.push(await timer.measure(pipelines.handwritten, frames));
                result.calibration.push({ frames, measurements });
                const duration = statistics(measurements.map(sample => sample.gpu_batch_us ?? sample.wall_us)).median_us;
                if (duration >= settings.target_batch_us || frames === settings.maximum_frames_per_batch) break;
                frames *= 2;
            }
            result.frames_per_batch = frames;
            for (let pair = -settings.warmup; pair < settings.samples; ++pair) {
                const variantOrder = order(pair + settings.warmup);
                if (pair >= 0) result.sample_order.push([...variantOrder]);
                for (const name of variantOrder) {
                    const measured = await timer.measure(pipelines[name], frames);
                    if (pair >= 0) {
                        result.variants[name].wall_samples.push(measured.wall_us);
                        if (measured.gpu_batch_us !== null) result.variants[name].gpu_samples.push(measured.gpu_batch_us);
                    }
                }
                status.textContent = `${fixture.label}: ${Math.max(0, pair + 1)}/${settings.samples} pairs, ${frames} complete render passes per batch`;
            }
            for (const name of names) {
                const variant = result.variants[name];
                variant.encode_submit_wait_batch = statistics(variant.wall_samples);
                if (variant.gpu_samples.length) {
                    variant.gpu_batch = statistics(variant.gpu_samples);
                    if (variant.gpu_samples.every(value => value > 0)) {
                        variant.gpu_per_frame = statistics(variant.gpu_samples.map(value => value / frames));
                    }
                }
                for (const metric of ["full_source_to_ready_us", "source_request_us", "server_source_us", "browser_ready_us", "component_sum_us"]) {
                    result.compilation[name][metric] = statistics(result.compilation[name].repeated.map(sample => sample[metric]));
                }
            }
            if (result.variants.handwritten.gpu_per_frame && result.variants.bwsl.gpu_per_frame) {
                result.paired_gpu_ratio = pairedRatio(result.variants.handwritten.gpu_batch.samples_us, result.variants.bwsl.gpu_batch.samples_us);
            }
            result.paired_full_compilation_ratio = pairedRatio(
                result.compilation.handwritten.repeated.map(sample => sample.full_source_to_ready_us),
                result.compilation.bwsl.repeated.map(sample => sample.full_source_to_ready_us));
        }
        for (const name of names) {
            await timer.measure(pipelines[name], 1);
            const verified = await timer.verify();
            images[name] = verified.images;
            result.variants[name].correctness_after = verified.correctness;
            delete result.variants[name].wall_samples;
            delete result.variants[name].gpu_samples;
        }
        result.cross_variant_comparison_after = compareVariants(images.handwritten, images.bwsl, fixture.targets);
        result.status = "passed";
        return { result, images, size };
    } catch (error) {
        result.status = error.message.startsWith("Cancelled") ? "cancelled" : "failed";
        result.error = error.message;
        error.partialResult = result;
        throw error;
    } finally {
        timer.destroy();
    }
}

function showResult(fixture, result) {
    if (fixture.validation_only) {
        const checks = document.querySelector("#interfaces");
        if (fixture.name === "interfaces") {
            checks.textContent = "Passed: sparse interfaces, integer flat interpolation, linear interpolation with varying clip W, fragment coordinates/depth, MRT and builtin offsets.";
        } else {
            checks.textContent += ` ${fixture.label}: passed independent CPU reference.`;
        }
        return;
    }
    const gpu = name => result.variants[name].gpu_per_frame?.median_us.toFixed(2) ?? "Unavailable";
    const ratio = result.paired_gpu_ratio;
    tableRow("#gpu-results", [fixture.label, gpu("handwritten"), gpu("bwsl"),
        ratio ? `${ratio.median.toFixed(3)}× [${ratio.bootstrap_95_interval.map(value => value.toFixed(3)).join(", ")}]` : "Unavailable",
        result.frames_per_batch, "Passed"]);
    const comp = result.compilation;
    tableRow("#compile-results", [fixture.label,
        (comp.handwritten.full_source_to_ready_us.median_us / 1000).toFixed(3),
        (comp.bwsl.full_source_to_ready_us.median_us / 1000).toFixed(3),
        (comp.bwsl.server_source_us.median_us / 1000).toFixed(3),
        (comp.handwritten.browser_ready_us.median_us / 1000).toFixed(3),
        (comp.bwsl.browser_ready_us.median_us / 1000).toFixed(3)]);
    tableRow("#cpu-results", [fixture.label,
        (result.variants.handwritten.encode_submit_wait_batch.median_us / 1000).toFixed(3),
        (result.variants.bwsl.encode_submit_wait_batch.median_us / 1000).toFixed(3)]);
}

function preview(fixture, images, size) {
    const section = document.createElement("section");
    const title = document.createElement("h3");
    title.textContent = fixture.label;
    section.append(title);
    const pictures = document.createElement("div");
    pictures.className = "pictures";
    const urls = {};
    for (const name of names) {
        const figure = document.createElement("figure");
        const caption = document.createElement("figcaption");
        caption.textContent = name === "handwritten" ? "Handwritten WGSL" : "BWSL → WGSL";
        const full = document.createElement("canvas");
        full.width = full.height = size;
        full.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(images[name][0]), size, size), 0, 0);
        const canvas = document.createElement("canvas");
        canvas.width = canvas.height = 256;
        canvas.getContext("2d").drawImage(full, 0, 0, 256, 256);
        urls[name] = canvas.toDataURL("image/png");
        figure.append(caption, canvas);
        pictures.append(figure);
    }
    section.append(pictures);
    document.querySelector("#previews").append(section);
    return urls;
}

async function run(event) {
    event.preventDefault();
    const settings = { resolution: Number(document.querySelector("#resolution").value),
        samples: Number(document.querySelector("#samples").value), warmup: Number(document.querySelector("#warmup").value),
        compilation_repetitions: 10, target_batch_us: 20000, maximum_frames_per_batch: 256, seed: 113 };
    if (!Number.isInteger(settings.samples) || settings.samples < 5 || settings.samples > 200 ||
        !Number.isInteger(settings.warmup) || settings.warmup < 1 || settings.warmup > 30) {
        status.textContent = "Use 5–200 measured pairs and 1–30 warmup pairs.";
        return;
    }
    stopped = false;
    const controls = document.querySelectorAll("#settings input, #settings select, #run");
    for (const control of controls) control.disabled = true;
    document.querySelector("#stop").disabled = false;
    document.querySelector("#download").disabled = true;
    document.querySelector("#interfaces").textContent = "Stage interface GPU regression pending.";
    for (const id of ["#gpu-results", "#compile-results", "#cpu-results", "#previews"]) document.querySelector(id).replaceChildren();
    const report = { schema_version: 2, kind: "render", status: "running", created_utc: new Date().toISOString(),
        settings, compiler: manifest.compiler, compilation_mode: manifest.compilation_mode ?? "fresh-cli",
        inprocess_compiler: manifest.inprocess_compiler, harness_sha256: manifest.harness_sha256,
        server_harness_sha256: manifest.server_harness_sha256, shared_server_sha256: manifest.shared_server_sha256,
        corpus_sha256: manifest.corpus_sha256, git_revision: manifest.git_revision,
        reference_tools: manifest.reference_tools,
        environment: { user_agent: navigator.userAgent, secure_context: isSecureContext, cross_origin_isolated: crossOriginIsolated },
        methodology: {
            gpu: "Timestamp interval from the first render-pass beginning to final render-pass end, including color/depth clears, draws and attachment stores; normalized per complete frame; same resources/state/layouts; AB/BA pairs",
            compilation: `Both stage modules + createRenderPipelineAsync. Full source-request-to-ready uses ${manifest.compilation_mode ?? "fresh-cli"}, including native BWSL parsing/lowering/SSA/emission/reflection and localhost handling. In-process mode preloads source and excludes CLI startup/output files; CLI mode includes both. Repeated-source browser/driver caches are not cleared; no cold-cache guarantee.`,
            correctness: "Exact input bytes before/after; all attachment pixels compared across variants; independent full CPU images for procedural MRT, particles and stage interface regression; software-rasterized interior color probes for skinned GGX. RGBA8 tolerance 3 LSB; integer attachment exact.",
            precision: "Normal browser timestamp quantization retained; batches target 20ms using median of three calibration probes. No timing threshold or general parity claim.",
        }, cases: {} };
    let device;
    const previews = [];
    try {
        active();
        if (!navigator.gpu) throw new Error("WebGPU unavailable in this browser");
        const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
        if (!adapter) throw new Error("No WebGPU adapter available");
        if (navigator.userAgentData?.getHighEntropyValues) {
            try { report.environment.user_agent_data = await navigator.userAgentData.getHighEntropyValues(["fullVersionList", "platformVersion", "architecture"]); } catch { /* Browser may redact these fields. */ }
        }
        const info = adapter.info ?? {};
        report.environment.adapter = { vendor: info.vendor, architecture: info.architecture, device: info.device,
            description: info.description, is_fallback_adapter: info.isFallbackAdapter ?? null };
        const timestamps = adapter.features.has("timestamp-query");
        report.environment.timestamp_queries = timestamps;
        device = await adapter.requestDevice({ requiredFeatures: timestamps ? ["timestamp-query"] : [] });
        document.querySelector("#device").textContent = `${info.description || info.vendor || "GPU details redacted"} · ${navigator.userAgent} · GPU timestamps ${timestamps ? "available" : "unavailable; CPU metric only"}`;
        const random = randomGenerator(settings.seed);
        for (const fixture of manifest.cases) {
            device.pushErrorScope("validation");
            let completed;
            try {
                completed = await runCase(device, fixture, settings, timestamps, random);
                report.cases[fixture.name] = completed.result;
            } catch (error) {
                report.cases[fixture.name] = error.partialResult ?? { status: "failed", error: error.message };
                throw error;
            } finally {
                const error = await device.popErrorScope();
                if (error) throw new Error(error.message);
            }
            showResult(fixture, completed.result);
            if (!fixture.validation_only) previews.push({ fixture, ...completed });
        }
        for (const item of previews) {
            report.cases[item.fixture.name].preview_png_256 = preview(item.fixture, item.images, item.size);
        }
        report.status = "passed";
        status.textContent = "Passed: render pipelines matched CPU references and each other, before and after timing.";
    } catch (error) {
        report.status = error.message.startsWith("Cancelled") ? "cancelled" : "failed";
        report.error = error.message;
        status.textContent = `${report.status.toUpperCase()}: ${error.message}`;
    } finally {
        device?.destroy();
        report.finished_utc = new Date().toISOString();
        latestReport = report;
        document.querySelector("#download").disabled = false;
        try {
            const response = await fetch("./results", { method: "POST",
                headers: { "Content-Type": "application/json", "X-BWSL-Result-Token": manifest.result_token }, body: JSON.stringify(report) });
            if (!response.ok) throw new Error(`Report save failed: HTTP ${response.status}`);
            status.textContent += `\nReport saved: ${(await response.json()).saved}`;
        } catch (error) {
            status.textContent += `\n${error.message}; use Download JSON to keep the report.`;
        }
        for (const control of controls) control.disabled = false;
        document.querySelector("#stop").disabled = true;
    }
}

document.querySelector("#settings").addEventListener("submit", run);
document.querySelector("#stop").addEventListener("click", () => { stopped = true; status.textContent = "Stopping after the current GPU batch…"; });
document.querySelector("#download").addEventListener("click", () => {
    const url = URL.createObjectURL(new Blob([JSON.stringify(latestReport, null, 2)], { type: "application/json" }));
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = `bwsl-render-${latestReport.created_utc.replaceAll(":", "-")}.json`;
    anchor.click();
    URL.revokeObjectURL(url);
});

try {
    const response = await fetch("./manifest.json");
    if (!response.ok) throw new Error(`Manifest HTTP ${response.status}`);
    manifest = await response.json();
    if (manifest.compilation_mode === "fresh-source-in-process") {
        document.querySelector("#compilation-method").textContent = "Repeated-source medians in milliseconds. BWSL uses a persistent native worker with preloaded source, creating fresh frontend, IR and arena state for both stages on every request. Localhost and worker communication are included; source/output file I/O and CLI startup are excluded. Browser/driver caches are not cleared: these are not guaranteed cold compilations.";
    }
    for (const fixture of manifest.cases.filter(item => !item.validation_only)) {
        const details = document.createElement("details");
        const summary = document.createElement("summary");
        summary.textContent = fixture.label;
        details.append(summary);
        const sources = [["BWSL", fixture.bwsl_source]];
        for (const name of names) {
            for (const [stage, data] of Object.entries(fixture.variants[name])) sources.push([`${name} ${stage}`, data.source]);
        }
        for (const [label, source] of sources) {
            const title = document.createElement("h4");
            title.textContent = label;
            const pre = document.createElement("pre");
            pre.textContent = source;
            details.append(title, pre);
        }
        document.querySelector("#sources").append(details);
    }
    status.textContent = "Ready. Keep this tab visible and other GPU workloads idle.";
    document.querySelector("#run").disabled = false;
} catch (error) {
    status.textContent = `Preparation failed: ${error.message}`;
}
