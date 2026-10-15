import { compareFloats, makeCase, mismatches, pairedRatio, randomGenerator, statistics } from "./cases.mjs";

const names = ["handwritten", "bwsl"];
const status = document.querySelector("#status");
let manifest;
let cancelled = false;
let latestReport = null;

function checkActive() {
    if (cancelled) throw new Error("Cancelled by user");
    if (document.hidden) throw new Error("Cancelled: keep the benchmark tab visible");
}

function row(target, cells) {
    const element = document.createElement("tr");
    for (const text of cells) {
        const cell = document.createElement("td");
        cell.textContent = text;
        element.append(cell);
    }
    document.querySelector(target).append(element);
}

function createBuffer(device, words) {
    const buffer = device.createBuffer({
        size: words.byteLength,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST,
        mappedAtCreation: true,
    });
    new Uint32Array(buffer.getMappedRange()).set(words);
    buffer.unmap();
    return buffer;
}

async function readBuffer(device, buffer, length) {
    const staging = device.createBuffer({
        size: length * 4,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    try {
        const encoder = device.createCommandEncoder();
        encoder.copyBufferToBuffer(buffer, 0, staging, 0, length * 4);
        device.queue.submit([encoder.finish()]);
        await staging.mapAsync(GPUMapMode.READ);
        const words = new Uint32Array(staging.getMappedRange()).slice();
        staging.unmap();
        return words;
    } finally {
        staging.destroy();
    }
}

async function compile(device, source, layout) {
    const start = performance.now();
    const module = device.createShaderModule({ code: source });
    const callEnd = performance.now();
    const info = await module.getCompilationInfo();
    const moduleEnd = performance.now();
    const errors = info.messages.filter(message => message.type === "error");
    if (errors.length) throw new Error(errors.map(error => error.message).join("\n"));
    const pipeline = await device.createComputePipelineAsync({
        layout,
        compute: { module, entryPoint: "main" },
    });
    const end = performance.now();
    return {
        pipeline,
        timing: {
            module_call_us: (callEnd - start) * 1000,
            module_ready_us: (moduleEnd - start) * 1000,
            pipeline_async_us: (end - moduleEnd) * 1000,
            total_ready_us: (end - start) * 1000,
        },
    };
}

async function compileFromSource(device, fixture, variant, layout) {
    const start = performance.now();
    const response = await fetch("./compile", {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-BWSL-Result-Token": manifest.result_token },
        body: JSON.stringify({ case: fixture.name, variant }),
    });
    if (!response.ok) throw new Error(`Source compilation failed: ${await response.text()}`);
    const source = await response.json();
    if (source.sha256 !== fixture.variants[variant].sha256) throw new Error("Shader source changed during compilation");
    const requestEnd = performance.now();
    const compiled = await compile(device, source.source, layout);
    compiled.timing.full_source_to_ready_us = (performance.now() - start) * 1000;
    compiled.timing.source_request_us = (requestEnd - start) * 1000;
    compiled.timing.server_source_us = source.server_source_us;
    compiled.timing.component_sum_us = source.server_source_us + compiled.timing.total_ready_us;
    compiled.timing.bwsl_reported_phases_us = source.bwsl_reported_phases_us;
    return compiled;
}

class BatchTimer {
    constructor(device, timestamps) {
        this.device = device;
        this.timestamps = timestamps;
        if (timestamps) {
            this.queries = device.createQuerySet({ type: "timestamp", count: 2 });
            this.resolve = device.createBuffer({
                size: 16,
                usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC,
            });
            this.readback = device.createBuffer({
                size: 16,
                usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
            });
        }
    }

    async measure(pipeline, bindings, workgroups, dispatches) {
        checkActive();
        const start = performance.now();
        const encoder = this.device.createCommandEncoder();
        const descriptor = this.timestamps ? {
            timestampWrites: {
                querySet: this.queries,
                beginningOfPassWriteIndex: 0,
                endOfPassWriteIndex: 1,
            },
        } : {};
        const pass = encoder.beginComputePass(descriptor);
        pass.setPipeline(pipeline);
        pass.setBindGroup(0, bindings);
        for (let index = 0; index < dispatches; ++index) {
            pass.dispatchWorkgroups(workgroups);
        }
        pass.end();
        if (this.timestamps) {
            encoder.resolveQuerySet(this.queries, 0, 2, this.resolve, 0);
            encoder.copyBufferToBuffer(this.resolve, 0, this.readback, 0, 16);
        }
        this.device.queue.submit([encoder.finish()]);
        await this.device.queue.onSubmittedWorkDone();
        const wallUs = (performance.now() - start) * 1000;
        let gpuUs = null;
        if (this.timestamps) {
            await this.readback.mapAsync(GPUMapMode.READ);
            const values = new BigUint64Array(this.readback.getMappedRange());
            const difference = values[1] - values[0];
            this.readback.unmap();
            if (difference < 0n) throw new Error("GPU timestamps moved backwards");
            gpuUs = Number(difference) / 1000;
        }
        return { wall_us: wallUs, gpu_batch_us: gpuUs };
    }

    destroy() {
        this.queries?.destroy();
        this.resolve?.destroy();
        this.readback?.destroy();
    }
}

async function verify(device, buffers, expectations, tolerance, outputComparison) {
    let checked = 0;
    let maximumAbsoluteError = 0;
    for (let index = 0; index < buffers.length; ++index) {
        const actual = await readBuffer(device, buffers[index], expectations[index].length);
        const output = index === buffers.length - 1;
        const comparison = output && tolerance ? compareFloats(actual, expectations[index], tolerance) : null;
        const errors = comparison?.errors ?? mismatches(actual, expectations[index]);
        if (errors.length) {
            throw new Error(`Buffer ${index} readback mismatch: ${JSON.stringify(errors)}`);
        }
        maximumAbsoluteError = Math.max(maximumAbsoluteError, comparison?.maximum_absolute_error ?? 0);
        if (output && outputComparison) outputComparison(actual);
        checked += actual.length;
    }
    return { passed: true, checked_words: checked, output_tolerance: tolerance ?? null,
        maximum_absolute_error: maximumAbsoluteError };
}

async function runCase(device, timer, fixture, settings, random) {
    status.textContent = `${fixture.label}: preparing identical buffers and CPU expectations…`;
    const data = makeCase(fixture.name, settings.elements);
    const expectations = [...data.inputs, data.expected];
    if (expectations.some(words => words.byteLength > device.limits.maxStorageBufferBindingSize ||
        words.byteLength > device.limits.maxBufferSize)) {
        throw new Error("Selected input size exceeds this device’s buffer limits");
    }
    const workgroups = settings.elements / fixture.workgroup_size[0];
    if (workgroups > device.limits.maxComputeWorkgroupsPerDimension) {
        throw new Error("Selected input size exceeds this device’s dispatch limit");
    }
    // A NaN float sentinel cannot accidentally pass a tolerance around zero.
    const sentinel = new Uint32Array(data.expected.length).fill(0x7FC00001);
    const buffers = [...data.inputs, sentinel].map(words => createBuffer(device, words));
    const layout = device.createBindGroupLayout({
        entries: fixture.resources.map(resource => ({
            binding: resource.binding,
            visibility: GPUShaderStage.COMPUTE,
            buffer: { type: resource.access === "read" ? "read-only-storage" : "storage" },
        })),
    });
    const pipelineLayout = device.createPipelineLayout({ bindGroupLayouts: [layout] });
    const bindings = device.createBindGroup({
        layout,
        entries: buffers.map((buffer, binding) => ({ binding, resource: { buffer } })),
    });
    const pipelines = {};
    const result = {
        bwsl_source_sha256: fixture.bwsl_sha256,
        source_bytes: Object.fromEntries(names.map(name => [name, new TextEncoder().encode(fixture.variants[name].source).length])),
        source_hashes: Object.fromEntries(names.map(name => [name, fixture.variants[name].sha256])),
        workgroups,
        compilation: Object.fromEntries(names.map(name => [name, { first_observed: null, repeated: [] }])),
        variants: {},
        sample_order: [],
        calibration: [],
    };
    const offset = random() < 0.5 ? 0 : 1;
    const order = iteration => (iteration + offset) % 2 ? [...names].reverse() : names;
    try {
        status.textContent = `${fixture.label}: measuring browser shader and pipeline readiness…`;
        for (let iteration = 0; iteration < 13; ++iteration) {
            for (const name of order(iteration)) {
                checkActive();
                const compiled = await compileFromSource(device, fixture, name, pipelineLayout);
                pipelines[name] = compiled.pipeline;
                if (iteration === 0) result.compilation[name].first_observed = compiled.timing;
                if (iteration >= 3) result.compilation[name].repeated.push(compiled.timing);
            }
        }
        for (const name of names) {
            result.compilation[name].repeated_total_ready = statistics(
                result.compilation[name].repeated.map(sample => sample.total_ready_us),
            );
            for (const field of ["full_source_to_ready_us", "source_request_us", "server_source_us", "component_sum_us"]) {
                result.compilation[name][field] = statistics(
                    result.compilation[name].repeated.map(sample => sample[field]),
                );
            }
        }
        let handwrittenOutput;
        for (const name of names) {
            device.queue.writeBuffer(buffers.at(-1), 0, sentinel);
            await timer.measure(pipelines[name], bindings, workgroups, 1);
            result.variants[name] = {
                correctness_before: await verify(device, buffers, expectations, data.tolerance, actual => {
                    if (name === "handwritten") {
                        handwrittenOutput = actual;
                    } else {
                        const comparison = data.tolerance
                            ? compareFloats(actual, handwrittenOutput, data.tolerance)
                            : { errors: mismatches(actual, handwrittenOutput), maximum_absolute_error: 0 };
                        if (comparison.errors.length) throw new Error("Shader variants disagree with each other");
                        result.cross_variant_comparison = comparison;
                    }
                }),
                wall_samples: [],
                gpu_samples: [],
            };
        }
        status.textContent = `${fixture.label}: calibrating a common GPU batch size…`;
        let dispatches = 1;
        while (true) {
            const measured = await timer.measure(pipelines.handwritten, bindings, workgroups, dispatches);
            result.calibration.push({ dispatches, ...measured });
            const duration = measured.gpu_batch_us ?? measured.wall_us;
            if (duration >= 10000 || dispatches === 1024) break;
            dispatches *= 2;
        }
        result.dispatches_per_batch = dispatches;
        for (let iteration = -settings.warmup; iteration < settings.samples; ++iteration) {
            const variantOrder = order(iteration + settings.warmup);
            if (iteration >= 0) result.sample_order.push([...variantOrder]);
            for (const name of variantOrder) {
                const measured = await timer.measure(pipelines[name], bindings, workgroups, dispatches);
                if (iteration >= 0) {
                    result.variants[name].wall_samples.push(measured.wall_us);
                    if (measured.gpu_batch_us !== null) {
                        result.variants[name].gpu_samples.push(measured.gpu_batch_us);
                    }
                }
            }
            status.textContent = `${fixture.label}: ${Math.max(0, iteration + 1)}/${settings.samples} measured pairs, ${dispatches} dispatches per batch`;
        }
        for (const name of names) {
            const variant = result.variants[name];
            variant.encode_submit_wait_batch = statistics(variant.wall_samples);
            if (variant.gpu_samples.length) {
                variant.gpu_batch = statistics(variant.gpu_samples);
                variant.gpu_per_dispatch = variant.gpu_samples.every(value => value > 0)
                    ? statistics(variant.gpu_samples.map(value => value / dispatches)) : null;
            }
            delete variant.wall_samples;
            delete variant.gpu_samples;
            device.queue.writeBuffer(buffers.at(-1), 0, sentinel);
            await timer.measure(pipelines[name], bindings, workgroups, 1);
            variant.correctness_after = await verify(device, buffers, expectations, data.tolerance);
        }
        if (result.variants.handwritten.gpu_per_dispatch && result.variants.bwsl.gpu_per_dispatch) {
            result.paired_gpu_ratio = pairedRatio(
                result.variants.handwritten.gpu_batch.samples_us,
                result.variants.bwsl.gpu_batch.samples_us,
            );
        }
        result.paired_full_compilation_ratio = pairedRatio(
            result.compilation.handwritten.repeated.map(sample => sample.full_source_to_ready_us),
            result.compilation.bwsl.repeated.map(sample => sample.full_source_to_ready_us),
        );
        result.status = "passed";
        return result;
    } finally {
        for (const buffer of buffers) buffer.destroy();
    }
}

function showResult(fixture, result) {
    const handwritten = result.variants.handwritten;
    const bwsl = result.variants.bwsl;
    const gpu = value => value.gpu_per_dispatch?.median_us.toFixed(2) ?? "Unavailable";
    const ratio = result.paired_gpu_ratio;
    row("#gpu-results", [fixture.label, gpu(handwritten), gpu(bwsl),
        ratio ? `${ratio.median.toFixed(3)}× (95% ${ratio.bootstrap_95_interval.map(value => value.toFixed(3)).join("–")})` : "Unavailable",
        result.dispatches_per_batch, bwsl.correctness_before.output_tolerance ? "Float tolerance passed" : "Exact readback passed"]);
    const compilation = result.compilation;
    row("#compile-results", [fixture.label,
        (compilation.handwritten.first_observed.total_ready_us / 1000).toFixed(3),
        (compilation.bwsl.first_observed.total_ready_us / 1000).toFixed(3),
        (compilation.handwritten.repeated_total_ready.median_us / 1000).toFixed(3),
        (compilation.bwsl.repeated_total_ready.median_us / 1000).toFixed(3)]);
    row("#full-compile-results", [fixture.label,
        (compilation.handwritten.full_source_to_ready_us.median_us / 1000).toFixed(3),
        (compilation.bwsl.full_source_to_ready_us.median_us / 1000).toFixed(3),
        (compilation.bwsl.server_source_us.median_us / 1000).toFixed(3),
        result.paired_full_compilation_ratio.median.toFixed(2) + "×"]);
    row("#cpu-results", [fixture.label,
        (handwritten.encode_submit_wait_batch.median_us / 1000).toFixed(3),
        (bwsl.encode_submit_wait_batch.median_us / 1000).toFixed(3),
        (handwritten.encode_submit_wait_batch.p95_us / 1000).toFixed(3),
        (bwsl.encode_submit_wait_batch.p95_us / 1000).toFixed(3)]);
}

async function persist(report) {
    latestReport = report;
    document.querySelector("#download").disabled = false;
    const response = await fetch("./results", {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-BWSL-Result-Token": manifest.result_token },
        body: JSON.stringify(report),
    });
    if (!response.ok) throw new Error(`Could not save browser report: HTTP ${response.status}`);
    return (await response.json()).saved;
}

async function run(event) {
    event.preventDefault();
    const settings = {
        elements: Number(document.querySelector("#elements").value),
        samples: Number(document.querySelector("#samples").value),
        warmup: Number(document.querySelector("#warmup").value),
        compilation_repetitions: 10,
        seed: 113,
        target_batch_us: 10000,
        maximum_dispatches_per_batch: 1024,
    };
    if (!Number.isInteger(settings.samples) || settings.samples < 5 || settings.samples > 200 ||
        !Number.isInteger(settings.warmup) || settings.warmup < 1 || settings.warmup > 30) {
        status.textContent = "Use 5–200 samples and 1–30 warmup pairs.";
        return;
    }
    cancelled = false;
    for (const id of ["#gpu-results", "#compile-results", "#full-compile-results", "#cpu-results"]) document.querySelector(id).replaceChildren();
    const controls = document.querySelectorAll("#settings input, #settings select, #run");
    for (const control of controls) control.disabled = true;
    document.querySelector("#stop").disabled = false;
    document.querySelector("#download").disabled = true;
    const report = {
        schema_version: 1,
        status: "running",
        created_utc: new Date().toISOString(),
        settings,
        compiler: manifest.compiler,
        reference_compiler: manifest.reference_compiler,
        harness_sha256: manifest.harness_sha256,
        server_harness_sha256: manifest.server_harness_sha256,
        corpus_sha256: manifest.corpus_sha256,
        git_revision: manifest.git_revision,
        source_path: manifest.path,
        baseline_style: manifest.baseline_style,
        environment: {
            user_agent: navigator.userAgent,
            secure_context: isSecureContext,
            cross_origin_isolated: crossOriginIsolated,
        },
        methodology: {
            gpu: "Timestamped compute batches, normalized per dispatch; warm shared buffers; AB/BA order with seeded starting variant",
            cpu: "Encoding + submission + onSubmittedWorkDone; excludes readback/mapping and source translation",
            compilation: "Full source-request-to-ready time includes localhost fetch/JSON and native BWSL CLI translation when applicable, then module getCompilationInfo and createComputePipelineAsync; repeated identical source, no cold-cache guarantee. Component sums exclude request overhead.",
            precision: "Default browser timestamp quantization retained; batching amortizes quantization but does not remove it",
            correctness: "Independent CPU expectations; exact inputs and dyadic outputs, declared float tolerance for GGX; cross-variant output comparison; before/after timing",
        },
        cases: {},
    };
    let device;
    let timer;
    try {
        checkActive();
        if (navigator.userAgentData?.getHighEntropyValues) {
            try {
                report.environment.user_agent_data = await navigator.userAgentData.getHighEntropyValues(
                    ["fullVersionList", "platformVersion", "architecture", "bitness"],
                );
            } catch {
                report.environment.user_agent_data = null;
            }
        }
        if (!navigator.gpu) {
            report.status = "unavailable";
            throw new Error("WebGPU is unavailable in this browser. Open this localhost page in a browser with WebGPU enabled.");
        }
        const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
        if (!adapter) {
            report.status = "unavailable";
            throw new Error("This browser did not provide a WebGPU adapter.");
        }
        const info = adapter.info ?? await adapter.requestAdapterInfo?.() ?? {};
        report.environment.adapter = {
            vendor: info.vendor ?? "",
            architecture: info.architecture ?? "",
            device: info.device ?? "",
            description: info.description ?? "",
            is_fallback_adapter: info.isFallbackAdapter ?? adapter.isFallbackAdapter ?? null,
        };
        const timestamps = adapter.features.has("timestamp-query");
        report.environment.timestamp_queries = timestamps;
        device = await adapter.requestDevice({ requiredFeatures: timestamps ? ["timestamp-query"] : [] });
        report.environment.limits = {
            max_storage_buffer_binding_size: device.limits.maxStorageBufferBindingSize,
            max_compute_workgroups_per_dimension: device.limits.maxComputeWorkgroupsPerDimension,
        };
        document.querySelector("#device").textContent = `${info.description || info.vendor || "GPU details limited by browser"} · ${navigator.userAgent} · GPU timestamps ${timestamps ? "available" : "unavailable; CPU submission metric only"}`;
        timer = new BatchTimer(device, timestamps);
        const random = randomGenerator(settings.seed);
        for (const fixture of manifest.cases) {
            device.pushErrorScope("validation");
            let result;
            try {
                result = await runCase(device, timer, fixture, settings, random);
            } catch (error) {
                const stopped = error.message.startsWith("Cancelled");
                report.cases[fixture.name] = { status: stopped ? "cancelled" : "failed", error: error.message };
                if (stopped) throw error;
                throw new Error(`${fixture.label}: ${error.message}`);
            } finally {
                const error = await device.popErrorScope();
                if (error) throw new Error(error.message);
            }
            report.cases[fixture.name] = result;
            showResult(fixture, result);
        }
        report.status = "passed";
        status.textContent = `Passed: all ${manifest.cases.length} shader pairs matched CPU expectations and each other. Browser timing complete.`;
    } catch (error) {
        if (report.status !== "unavailable") report.status = error.message.startsWith("Cancelled") ? "cancelled" : "failed";
        report.error = error.message;
        status.textContent = `${report.status.toUpperCase()}: ${error.message}`;
    } finally {
        timer?.destroy();
        device?.destroy();
        report.finished_utc = new Date().toISOString();
        try {
            const saved = await persist(report);
            status.textContent += `\nReport saved: ${saved}`;
        } catch (error) {
            status.textContent += `\n${error.message}. Use Download JSON to keep the report.`;
        }
        for (const control of controls) control.disabled = false;
        document.querySelector("#stop").disabled = true;
    }
}

document.querySelector("#settings").addEventListener("submit", run);
document.querySelector("#stop").addEventListener("click", () => {
    cancelled = true;
    status.textContent = "Stopping after the current GPU batch…";
});
document.querySelector("#download").addEventListener("click", () => {
    const blob = new Blob([JSON.stringify(latestReport, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = `bwsl-webgpu-${latestReport.created_utc.replaceAll(":", "-")}.json`;
    link.click();
    URL.revokeObjectURL(url);
});

try {
    const response = await fetch("./manifest.json");
    if (!response.ok) throw new Error(`Could not load shaders: HTTP ${response.status}`);
    manifest = await response.json();
    if (manifest.reference_compiler) {
        document.querySelector("#intro").textContent = "Previous and current BWSL compilers translate the same source directly to WGSL. Both outputs run through this browser’s WebGPU API; full compilation includes fresh native translation for each compiler.";
        document.querySelector("#full-compile-method").textContent = "Repeated-source median milliseconds, measured from the browser. Both paths launch their native compiler, read the same BWSL and write WGSL, then create a browser pipeline. Localhost request/JSON overhead is included. Browser/driver caches may already be warm.";
    }
    for (const header of document.querySelectorAll("[data-reference]")) {
        header.textContent = manifest.reference_label + (header.dataset.suffix ?? "");
    }
    document.querySelector("#baseline-style").textContent = `Reference: ${manifest.baseline_style}. Compute stages only.`;
    for (const fixture of manifest.cases) {
        const title = document.createElement("h3");
        title.textContent = fixture.label;
        document.querySelector("#sources").append(title);
        for (const [label, source] of [
            ["BWSL source", fixture.bwsl_source],
            [manifest.reference_label, fixture.variants.handwritten.source],
            ["BWSL-generated WGSL", fixture.variants.bwsl.source],
        ]) {
            const caption = document.createElement("h4");
            caption.textContent = label;
            const pre = document.createElement("pre");
            pre.textContent = source;
            document.querySelector("#sources").append(caption, pre);
        }
    }
    status.textContent = "Ready. Run this comparison with the tab visible and other GPU workloads idle.";
    document.querySelector("#run").disabled = false;
} catch (error) {
    status.textContent = `Could not prepare benchmark: ${error.message}`;
}
