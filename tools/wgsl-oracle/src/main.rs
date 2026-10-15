//! Development-only validation and GPU oracle. The compiler never links this crate.

use std::borrow::Cow;
use std::env;
use std::fs;
use std::hint::black_box;
use std::path::Path;
use std::sync::mpsc;
use std::time::Instant;

use naga::valid::{Capabilities, ModuleInfo, ValidationFlags, Validator};
use serde::Deserialize;
use serde_json::{json, Value};
use wgpu::util::DeviceExt;

const NAGA_VERSION: &str = "27.0.3";
const WGPU_VERSION: &str = "27.0.1";
type Result<T> = std::result::Result<T, String>;

enum OperationOutput {
    Empty,
    Bytes(Vec<u8>),
    Words(Vec<u32>),
}

impl OperationOutput {
    fn byte_len(&self) -> usize {
        match self {
            Self::Empty => 0,
            Self::Bytes(bytes) => bytes.len(),
            Self::Words(words) => words.len() * 4,
        }
    }

    fn into_bytes(self) -> Vec<u8> {
        match self {
            Self::Empty => Vec::new(),
            Self::Bytes(bytes) => bytes,
            Self::Words(words) => words.iter().flat_map(|word| word.to_le_bytes()).collect(),
        }
    }
}

fn parse_module(bytes: &[u8], spirv: bool) -> Result<(naga::Module, ModuleInfo)> {
    let module = if spirv {
        naga::front::spv::parse_u8_slice(bytes, &naga::front::spv::Options::default())
            .map_err(|error| error.to_string())?
    } else {
        let source = std::str::from_utf8(bytes).map_err(|error| error.to_string())?;
        naga::front::wgsl::parse_str(source).map_err(|error| error.emit_to_string(source))?
    };
    let info = Validator::new(ValidationFlags::all(), Capabilities::empty())
        .validate(&module)
        .map_err(|error| format!("{error:#?}"))?;
    Ok((module, info))
}

fn operation(bytes: &[u8], mode: &str) -> Result<OperationOutput> {
    let (module, info) = parse_module(bytes, mode == "spv-to-wgsl")?;
    match mode {
        "validate" => Ok(OperationOutput::Empty),
        "spv-to-wgsl" => {
            let source = naga::back::wgsl::write_string(
                &module,
                &info,
                naga::back::wgsl::WriterFlags::EXPLICIT_TYPES,
            )
            .map_err(|error| error.to_string())?;
            Ok(OperationOutput::Bytes(source.into_bytes()))
        }
        "wgsl-to-spv" => {
            let options = naga::back::spv::Options::default();
            let words = naga::back::spv::write_vec(&module, &info, &options, None)
                .map_err(|error| error.to_string())?;
            Ok(OperationOutput::Words(words))
        }
        _ => Err(format!("Unknown benchmark mode: {mode}")),
    }
}

fn statistics(samples: &[f64]) -> Value {
    let mut ordered = samples.to_vec();
    ordered.sort_by(f64::total_cmp);
    let middle = ordered.len() / 2;
    let median = if ordered.len().is_multiple_of(2) {
        (ordered[middle - 1] + ordered[middle]) * 0.5
    } else {
        ordered[middle]
    };
    json!({
        "median_us": median,
        "min_us": ordered[0],
        "p95_us": ordered[((ordered.len() as f64 * 0.95).ceil() as usize - 1).min(ordered.len() - 1)],
        "samples_us": samples,
    })
}

fn benchmark(path: &str, mode: &str, iterations: usize, warmup: usize) -> Result<Value> {
    if iterations == 0 {
        return Err("iterations must be positive".into());
    }
    let bytes = fs::read(path).map_err(|error| error.to_string())?;
    for _ in 0..warmup {
        black_box(operation(black_box(&bytes), mode)?);
    }
    let mut samples = Vec::with_capacity(iterations);
    let mut output_size = 0;
    for _ in 0..iterations {
        let start = Instant::now();
        let output = operation(black_box(&bytes), mode)?;
        output_size = black_box(output.byte_len());
        // Output allocation and destruction are included, input I/O is excluded.
        drop(output);
        samples.push(start.elapsed().as_secs_f64() * 1_000_000.0);
    }
    let mut result = statistics(&samples);
    result["mode"] = json!(mode);
    result["naga_version"] = json!(NAGA_VERSION);
    result["iterations"] = json!(iterations);
    result["warmup"] = json!(warmup);
    result["input_bytes"] = json!(bytes.len());
    result["output_bytes"] = json!(output_size);
    Ok(result)
}

fn one() -> usize {
    1
}

#[derive(Deserialize)]
struct BufferCase {
    binding: u32,
    group: Option<u32>,
    #[serde(default)]
    uniform: bool,
    data: Vec<u32>,
    expected: Option<Vec<u32>>,
    expected_f32: Option<Vec<f32>>,
    #[serde(default)]
    absolute_tolerance: f32,
    #[serde(default)]
    relative_tolerance: f32,
}

#[derive(Deserialize)]
struct ComputeCase {
    entry_point: Option<String>,
    #[serde(default)]
    bind_group_index: u32,
    workgroups: [u32; 3],
    buffers: Vec<BufferCase>,
    #[serde(default = "one")]
    iterations: usize,
    #[serde(default = "one")]
    warmup: usize,
}

fn read_buffer(device: &wgpu::Device, buffer: &wgpu::Buffer) -> Result<Vec<u8>> {
    let slice = buffer.slice(..);
    let (sender, receiver) = mpsc::channel();
    slice.map_async(wgpu::MapMode::Read, move |result| {
        let _ = sender.send(result);
    });
    device
        .poll(wgpu::PollType::wait_indefinitely())
        .map_err(|error| error.to_string())?;
    receiver
        .recv()
        .map_err(|error| error.to_string())?
        .map_err(|error| error.to_string())?;
    let bytes = slice.get_mapped_range().to_vec();
    buffer.unmap();
    Ok(bytes)
}

async fn run_compute(path: &str, case_path: &str) -> Result<Value> {
    let source = fs::read_to_string(path).map_err(|error| error.to_string())?;
    let case: ComputeCase =
        serde_json::from_slice(&fs::read(case_path).map_err(|error| error.to_string())?)
            .map_err(|error| error.to_string())?;
    if case.iterations == 0 || case.buffers.iter().any(|buffer| buffer.data.is_empty()) {
        return Err("iterations and buffer lengths must be positive".into());
    }
    parse_module(source.as_bytes(), false)?;

    let instance = wgpu::Instance::new(&wgpu::InstanceDescriptor::default());
    let adapter = instance
        .request_adapter(&wgpu::RequestAdapterOptions::default())
        .await
        .map_err(|error| format!("SKIP: no WebGPU adapter: {error}"))?;
    let adapter_info = adapter.get_info();
    let timestamps = adapter.features().contains(wgpu::Features::TIMESTAMP_QUERY);
    let requested_features = if timestamps {
        wgpu::Features::TIMESTAMP_QUERY
    } else {
        wgpu::Features::empty()
    };
    let (device, queue) = adapter
        .request_device(&wgpu::DeviceDescriptor {
            label: Some("WGSL regression oracle"),
            required_features: requested_features,
            ..Default::default()
        })
        .await
        .map_err(|error| error.to_string())?;
    device.push_error_scope(wgpu::ErrorFilter::Validation);
    let module_start = Instant::now();
    let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some(path),
        source: wgpu::ShaderSource::Wgsl(Cow::Borrowed(&source)),
    });
    let module_us = module_start.elapsed().as_secs_f64() * 1_000_000.0;
    let pipeline_start = Instant::now();
    let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some(path),
        layout: None,
        module: &module,
        entry_point: case.entry_point.as_deref(),
        compilation_options: wgpu::PipelineCompilationOptions::default(),
        cache: None,
    });
    let pipeline_us = pipeline_start.elapsed().as_secs_f64() * 1_000_000.0;
    if let Some(error) = device.pop_error_scope().await {
        return Err(error.to_string());
    }

    device.push_error_scope(wgpu::ErrorFilter::Validation);
    let buffers: Vec<wgpu::Buffer> = case
        .buffers
        .iter()
        .map(|buffer| {
            let bytes: Vec<u8> = buffer
                .data
                .iter()
                .flat_map(|word| word.to_le_bytes())
                .collect();
            device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: None,
                contents: &bytes,
                usage: (if buffer.uniform {
                    wgpu::BufferUsages::UNIFORM
                } else {
                    wgpu::BufferUsages::STORAGE
                }) | wgpu::BufferUsages::COPY_SRC
                    | wgpu::BufferUsages::COPY_DST,
            })
        })
        .collect();
    let maximum_group = case
        .buffers
        .iter()
        .map(|buffer| buffer.group.unwrap_or(case.bind_group_index))
        .max()
        .unwrap_or(case.bind_group_index);
    let bind_groups: Vec<wgpu::BindGroup> = (0..=maximum_group)
        .map(|index| {
            let entries: Vec<wgpu::BindGroupEntry> = case
                .buffers
                .iter()
                .zip(&buffers)
                .filter(|(description, _)| {
                    description.group.unwrap_or(case.bind_group_index) == index
                })
                .map(|(description, buffer)| wgpu::BindGroupEntry {
                    binding: description.binding,
                    resource: buffer.as_entire_binding(),
                })
                .collect();
            device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: None,
                layout: &pipeline.get_bind_group_layout(index),
                entries: &entries,
            })
        })
        .collect();
    let query_set = timestamps.then(|| {
        device.create_query_set(&wgpu::QuerySetDescriptor {
            label: None,
            ty: wgpu::QueryType::Timestamp,
            count: 2,
        })
    });
    let query_resolve = timestamps.then(|| {
        device.create_buffer(&wgpu::BufferDescriptor {
            label: None,
            size: 16,
            usage: wgpu::BufferUsages::QUERY_RESOLVE | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        })
    });
    let query_readback = timestamps.then(|| {
        device.create_buffer(&wgpu::BufferDescriptor {
            label: None,
            size: 16,
            usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        })
    });
    if let Some(error) = device.pop_error_scope().await {
        return Err(error.to_string());
    }

    let mut submit_samples = Vec::with_capacity(case.iterations);
    let mut gpu_samples = Vec::with_capacity(case.iterations);
    for iteration in 0..case.warmup + case.iterations {
        // Each sample is one dispatch, excludes pipeline creation and buffer readback.
        let start = Instant::now();
        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
        {
            let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: None,
                timestamp_writes: query_set.as_ref().map(|queries| {
                    wgpu::ComputePassTimestampWrites {
                        query_set: queries,
                        beginning_of_pass_write_index: Some(0),
                        end_of_pass_write_index: Some(1),
                    }
                }),
            });
            pass.set_pipeline(&pipeline);
            for (index, group) in bind_groups.iter().enumerate() {
                pass.set_bind_group(index as u32, group, &[]);
            }
            pass.dispatch_workgroups(case.workgroups[0], case.workgroups[1], case.workgroups[2]);
        }
        if let Some(queries) = &query_set {
            encoder.resolve_query_set(queries, 0..2, query_resolve.as_ref().unwrap(), 0);
            encoder.copy_buffer_to_buffer(
                query_resolve.as_ref().unwrap(),
                0,
                query_readback.as_ref().unwrap(),
                0,
                16,
            );
        }
        queue.submit([encoder.finish()]);
        device
            .poll(wgpu::PollType::wait_indefinitely())
            .map_err(|error| error.to_string())?;
        let elapsed_us = start.elapsed().as_secs_f64() * 1_000_000.0;
        if iteration >= case.warmup {
            submit_samples.push(elapsed_us);
        }
        if let Some(readback) = &query_readback {
            let bytes = read_buffer(&device, readback)?;
            let begin = u64::from_le_bytes(bytes[0..8].try_into().unwrap());
            let end = u64::from_le_bytes(bytes[8..16].try_into().unwrap());
            if iteration >= case.warmup {
                gpu_samples.push(
                    end.wrapping_sub(begin) as f64 * queue.get_timestamp_period() as f64 / 1_000.0,
                );
            }
        }
    }

    let mut output_buffers = Vec::new();
    let mut mismatches = Vec::new();
    for (description, buffer) in case.buffers.iter().zip(&buffers) {
        let readback = device.create_buffer(&wgpu::BufferDescriptor {
            label: None,
            size: buffer.size(),
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
        encoder.copy_buffer_to_buffer(buffer, 0, &readback, 0, buffer.size());
        queue.submit([encoder.finish()]);
        let bytes = read_buffer(&device, &readback)?;
        let words: Vec<u32> = bytes
            .as_chunks::<4>()
            .0
            .iter()
            .map(|word| u32::from_le_bytes(*word))
            .collect();
        if let Some(expected) = &description.expected {
            if words.len() != expected.len() {
                mismatches.push(format!(
                    "binding {}: expected {} words, got {}",
                    description.binding,
                    expected.len(),
                    words.len()
                ));
            }
            for (index, (actual, expected)) in words.iter().zip(expected).enumerate() {
                if actual != expected {
                    mismatches.push(format!(
                        "binding {} word {index}: expected {expected}, got {actual}",
                        description.binding
                    ));
                    break;
                }
            }
        }
        if let Some(expected) = &description.expected_f32 {
            if words.len() != expected.len() {
                mismatches.push(format!(
                    "binding {}: expected {} floats, got {}",
                    description.binding,
                    expected.len(),
                    words.len()
                ));
            }
            for (index, (bits, expected)) in words.iter().zip(expected).enumerate() {
                let actual = f32::from_bits(*bits);
                let tolerance = description.absolute_tolerance
                    + description.relative_tolerance * expected.abs();
                if !actual.is_finite()
                    || !expected.is_finite()
                    || (actual - expected).abs() > tolerance
                {
                    mismatches.push(format!("binding {} float {index}: expected {expected}, got {actual}, tolerance {tolerance}", description.binding));
                    break;
                }
            }
        }
        output_buffers.push(json!({"binding": description.binding, "data": words}));
    }
    let result = json!({
        "wgpu_version": WGPU_VERSION,
        "naga_version": NAGA_VERSION,
        "adapter": adapter_info.name,
        "backend": format!("{:?}", adapter_info.backend),
        "device_type": format!("{:?}", adapter_info.device_type),
        "module_us": module_us,
        "pipeline_us": pipeline_us,
        "iterations": case.iterations,
        "warmup": case.warmup,
        "submit_and_wait": statistics(&submit_samples),
        "gpu_timestamp": if timestamps { statistics(&gpu_samples) } else { Value::Null },
        "buffers": output_buffers,
        "mismatches": mismatches,
        "passed": mismatches.is_empty(),
    });
    Ok(result)
}

fn execute(args: &[String]) -> Result<Value> {
    match args.get(1).map(String::as_str) {
        Some("--version") => Ok(json!({"naga": NAGA_VERSION, "wgpu": WGPU_VERSION})),
        Some("validate") if args.len() == 3 => {
            let bytes = fs::read(&args[2]).map_err(|error| error.to_string())?;
            let (module, _) = parse_module(&bytes, false)?;
            Ok(json!({"valid": true, "naga_version": NAGA_VERSION,
                "entry_points": module.entry_points.iter().map(|entry| &entry.name).collect::<Vec<_>>()}))
        }
        Some("dump-msl") if args.len() == 4 => {
            let bytes = fs::read(&args[2]).map_err(|error| error.to_string())?;
            let (module, info) = parse_module(&bytes, false)?;
            let (source, _) = naga::back::msl::write_string(
                &module, &info, &naga::back::msl::Options::default(),
                &naga::back::msl::PipelineOptions::default(),
            ).map_err(|error| error.to_string())?;
            fs::write(&args[3], &source).map_err(|error| error.to_string())?;
            Ok(json!({"mode": "wgsl-to-msl-diagnostic", "output_bytes": source.len()}))
        }
        Some("translate") if args.len() == 4 => {
            let bytes = fs::read(&args[2]).map_err(|error| error.to_string())?;
            let input_ext = Path::new(&args[2]).extension().and_then(|value| value.to_str());
            let output_ext = Path::new(&args[3]).extension().and_then(|value| value.to_str());
            let mode = match (input_ext, output_ext) {
                (Some("spv"), Some("wgsl")) => "spv-to-wgsl",
                (Some("wgsl"), Some("spv")) => "wgsl-to-spv",
                _ => return Err("translate requires .spv -> .wgsl or .wgsl -> .spv".into()),
            };
            let output = operation(&bytes, mode)?.into_bytes();
            fs::write(&args[3], &output).map_err(|error| error.to_string())?;
            Ok(json!({"mode": mode, "input_bytes": bytes.len(), "output_bytes": output.len()}))
        }
        Some("bench") if args.len() == 5 || args.len() == 6 => {
            let iterations = args[4].parse().map_err(|_| "invalid iterations")?;
            let warmup = args.get(5).map_or(Ok(10), |value| value.parse().map_err(|_| "invalid warmup"))?;
            benchmark(&args[2], &args[3], iterations, warmup)
        }
        Some("run") if args.len() == 4 => pollster::block_on(run_compute(&args[2], &args[3])),
        _ => Err("Usage: wgsl-oracle validate INPUT.wgsl | translate INPUT OUTPUT | bench INPUT MODE ITERATIONS [WARMUP] | run SHADER.wgsl CASE.json | --version".into()),
    }
}

fn main() {
    match execute(&env::args().collect::<Vec<_>>()) {
        Ok(result) => {
            println!("{}", serde_json::to_string(&result).unwrap());
            if result.get("passed") == Some(&Value::Bool(false)) {
                std::process::exit(1);
            }
        }
        Err(error) => {
            eprintln!("{error}");
            std::process::exit(if error.starts_with("SKIP:") { 77 } else { 1 });
        }
    }
}
