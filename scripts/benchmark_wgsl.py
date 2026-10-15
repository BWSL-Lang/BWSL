#!/usr/bin/env python3
"""Compare matched BWSL/WGSL kernels at explicit compilation and GPU endpoints."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import platform
from pathlib import Path
import random
import re
import statistics
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/wgsl_benchmarks"
CASES = tuple(json.loads((FIXTURES / "corpus.json").read_text()))
EXE_SUFFIX = ".exe" if sys.platform == "win32" else ""
TIMING = re.compile(r"^\s*([A-Za-z][A-Za-z0-9 +_-]*):\s*([0-9.]+)\s+ms\s*$", re.MULTILINE)


def execute(command: list[str]) -> str:
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
    if result.returncode:
        raise RuntimeError(
            f"Command failed ({result.returncode}): {' '.join(command)}\n"
            f"{result.stdout[-8000:]}{result.stderr[-8000:]}"
        )
    return result.stdout


def execute_json(command: list[str]) -> dict:
    return json.loads(execute(command))


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stats(samples: list[float]) -> dict:
    ordered = sorted(samples)
    # Nearest-rank p95, shared with the Naga/GPU oracle.
    return {
        "median_us": statistics.median(ordered),
        "min_us": ordered[0],
        "p95_us": ordered[math.ceil(len(ordered) * 0.95) - 1],
        "samples_us": samples,
    }


def process_benchmarks(paths: dict[str, list[list[str]]], iterations: int, warmup: int) -> dict:
    """Interleave paths in a reproducible order to reduce systematic run-order bias."""
    samples: dict[str, list[float]] = {name: [] for name in paths}
    phases: dict[str, dict[str, list[float]]] = {name: {} for name in paths}
    rng = random.Random(113)
    for iteration in range(-warmup, iterations):
        names = list(paths)
        rng.shuffle(names)
        for name in names:
            start = time.perf_counter_ns()
            outputs = [execute(command) for command in paths[name]]
            elapsed_us = (time.perf_counter_ns() - start) / 1000.0
            if iteration < 0:
                continue
            samples[name].append(elapsed_us)
            # Phase names come from bwslc -timing. No subprocess time is included.
            # The final occurrence is the summary when a label appears twice.
            measured = dict(TIMING.findall("\n".join(outputs)))
            for phase, milliseconds in measured.items():
                phases[name].setdefault(phase, []).append(float(milliseconds) * 1000.0)
    return {
        name: {
            "commands": commands,
            "process_wall": stats(samples[name]),
            "bwsl_reported_phases": {phase: stats(values) for phase, values in phases[name].items()},
        }
        for name, commands in paths.items()
    }


def float_word(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def runtime_case(name: str, count: int, iterations: int, warmup: int) -> dict:
    """Independent host formulas; floats use exact dyadic inputs and arithmetic."""
    if name in {"gaussian_5x5", "skinning_4bone", "pbr_4light"}:
        return execute_json([
            "node", str(ROOT / "tools/wgsl-bench/runtime_case.mjs"), name,
            str(count), str(iterations), str(warmup),
        ])
    if name == "saxpy":
        x = [(i % 128 - 64) * 0.25 for i in range(count)]
        y = [(i % 64 - 32) * 0.5 for i in range(count)]
        expected = [float_word(a * 2.0 + b) for a, b in zip(x, y)]
        inputs = [[float_word(value) for value in x], [float_word(value) for value in y]]
    elif name == "affine_vec4":
        source = [(i % 128 - 64) * 0.25 for i in range(count * 4)]
        scale = (2.0, -1.0, 0.5, 4.0)
        bias = (1.0, 2.0, -4.0, 0.25)
        expected = [float_word(value * scale[i % 4] + bias[i % 4]) for i, value in enumerate(source)]
        inputs = [[float_word(value) for value in source]]
    elif name == "hash_u32":
        values = [(i * 2654435761 + 0xFFFFFFFF) & 0xFFFFFFFF for i in range(count)]
        expected = []
        for value in values:
            b = ((value ^ (value >> 16)) * 0x7FEB352D) & 0xFFFFFFFF
            c = ((b ^ (b >> 15)) * 0x846CA68B) & 0xFFFFFFFF
            expected.append(c ^ (c >> 16))
        inputs = [values]
    else:
        raise ValueError(f"Unknown kernel: {name}")
    buffers = [
        {"binding": index, "data": values, "expected": values}
        for index, values in enumerate(inputs)
    ]
    # A nonzero sentinel catches missing writes, including expected zero results.
    buffers.append({"binding": len(inputs), "data": [0xA5A5A5A5] * len(expected), "expected": expected})
    return {
        "entry_point": None,
        "workgroups": [count // 64, 1, 1],
        "buffers": buffers,
        "iterations": iterations,
        "warmup": warmup,
    }


def single_output(directory: Path, suffix: str) -> Path:
    files = list(directory.glob(f"*{suffix}"))
    if len(files) != 1:
        raise RuntimeError(f"Expected exactly one {suffix} output in {directory}, got {files}")
    return files[0]


def bind_group_index(shader: Path) -> int:
    groups = {int(value) for value in re.findall(r"@group\(\s*(\d+)u?\s*\)", shader.read_text())}
    if len(groups) != 1:
        raise RuntimeError(f"Expected one resource group in benchmark fixture {shader}, got {groups}")
    return groups.pop()


def run_case(name: str, args: argparse.Namespace, engines: dict[str, Path], directory: Path) -> dict:
    source = FIXTURES / f"{name}.bwsl"
    handwritten = FIXTURES / f"{name}.wgsl"
    native_dir = directory / "native"
    spirv_dir = directory / "spirv"
    native_dir.mkdir(parents=True)
    spirv_dir.mkdir()
    common = [str(args.compiler), str(source), "-validation", "off", "-timing"]
    native_command = common + ["-wgsl", "-o", str(native_dir)]
    spirv_command = common + ["-spv", "-o", str(spirv_dir)]
    (directory / "native-compile.log").write_text(execute(native_command))
    (directory / "spirv-compile.log").write_text(execute(spirv_command))
    native = single_output(native_dir, ".comp.wgsl")
    spirv = single_output(spirv_dir, ".spv")

    variants = {"handwritten": handwritten, "bwsl_native": native}
    baseline_command = None
    if args.baseline_compiler:
        baseline_dir = directory / "baseline"
        baseline_dir.mkdir()
        baseline_command = [
            str(args.baseline_compiler), str(source), "-validation", "off", "-timing",
            "-wgsl", "-o", str(baseline_dir),
        ]
        (directory / "baseline-compile.log").write_text(execute(baseline_command))
        variants["bwsl_baseline"] = single_output(baseline_dir, ".comp.wgsl")
    conversions: dict[str, list[str]] = {}
    for engine, executable in engines.items():
        output = directory / f"{engine}-from-spirv.wgsl"
        command = [str(executable), "translate", str(spirv), str(output)]
        execute(command)
        variants[f"bwsl_spirv_{engine}"] = output
        conversions[engine] = command

    validation: dict[str, dict] = {}
    inprocess: dict[str, dict] = {}
    for engine, executable in engines.items():
        validation[engine] = {}
        inprocess[engine] = {}
        for variant, shader in variants.items():
            validation[engine][variant] = execute_json([str(executable), "validate", str(shader)])
            for mode in ("validate", "wgsl-to-spv"):
                key = f"{variant}/{mode}"
                inprocess[engine][key] = execute_json([
                    str(executable), "bench", str(shader), mode,
                    str(args.inprocess_runs), str(args.warmup),
                ])
        inprocess[engine]["bwsl_spirv/spv-to-wgsl"] = execute_json([
            str(executable), "bench", str(spirv), "spv-to-wgsl",
            str(args.inprocess_runs), str(args.warmup),
        ])

    paths = {
        "bwsl_to_native_wgsl": [native_command],
        "bwsl_to_spirv": [spirv_command],
    }
    if baseline_command:
        paths["baseline_bwsl_to_native_wgsl"] = [baseline_command]
    for engine, executable in engines.items():
        endpoint = directory / f"{engine}-endpoint.spv"
        handwritten_to_spv = [str(executable), "translate", str(handwritten), str(endpoint)]
        native_to_spv = [str(executable), "translate", str(native), str(endpoint)]
        roundtrip_to_spv = [
            str(executable), "translate", str(variants[f"bwsl_spirv_{engine}"]), str(endpoint),
        ]
        paths[f"handwritten_wgsl_to_{engine}_spirv"] = [handwritten_to_spv]
        paths[f"bwsl_to_native_wgsl_to_{engine}_spirv"] = [native_command, native_to_spv]
        if baseline_command:
            baseline_to_spv = [
                str(executable), "translate", str(variants["bwsl_baseline"]), str(endpoint),
            ]
            paths[f"baseline_bwsl_to_native_wgsl_to_{engine}_spirv"] = [baseline_command, baseline_to_spv]
        paths[f"bwsl_to_spirv_to_{engine}_wgsl"] = [spirv_command, conversions[engine]]
        paths[f"bwsl_to_spirv_to_{engine}_wgsl_to_spirv"] = [
            spirv_command, conversions[engine], roundtrip_to_spv,
        ]
    measured = process_benchmarks(paths, args.runs, args.warmup)

    runtime: dict[str, dict] = {}
    if not args.skip_runtime:
        device_signature = None
        for variant, shader in variants.items():
            case_path = directory / f"runtime-case-{variant}.json"
            case = runtime_case(name, args.elements, args.gpu_runs, args.warmup)
            case["bind_group_index"] = bind_group_index(shader)
            case_path.write_text(json.dumps(case))
            data = execute_json([str(args.oracle), "run", str(shader), str(case_path)])
            # Preserve the full oracle response separately; keep the main report compact.
            (directory / f"runtime-{variant}.json").write_text(json.dumps(data, indent=2) + "\n")
            signature = tuple(data.get(key) for key in ("adapter", "backend", "device_type"))
            if device_signature is not None and signature != device_signature:
                raise RuntimeError(f"GPU adapter changed between variants: {device_signature} -> {signature}")
            device_signature = signature
            if not data.get("passed", False):
                raise RuntimeError(
                    f"GPU output mismatch for {name}/{variant}: {data.get('mismatches', [])[:10]}"
                )
            runtime[variant] = {key: value for key, value in data.items() if key != "buffers"}
    return {
        "source": str(source),
        "source_sha256": digest(source),
        "source_bytes": source.stat().st_size,
        "wgsl_variants": {
            name: {
                "path": str(path), "bytes": path.stat().st_size,
                "sha256": digest(path), "bind_group_index": bind_group_index(path),
            }
            for name, path in variants.items()
        },
        "validation": validation,
        "in_process_cpu": inprocess,
        "process_paths": measured,
        "runtime": runtime,
    }


def metadata(executable: Path) -> dict:
    return {"path": str(executable), "sha256": digest(executable), "bytes": executable.stat().st_size}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=ROOT / f"build/bwslc{EXE_SUFFIX}")
    parser.add_argument("--baseline-compiler", type=Path,
                        help="Also measure a previous compiler, interleaved with the candidate")
    parser.add_argument("--oracle", type=Path, default=ROOT / f"build/wgsl-oracle/release/wgsl-oracle{EXE_SUFFIX}")
    parser.add_argument("--tint", type=Path, default=ROOT / f"build/wgsl-tools/tint/bwsl-tint-bench{EXE_SUFFIX}")
    parser.add_argument("--allow-missing-tint", action="store_true")
    parser.add_argument("--runs", type=int, default=30, help="Process wall-time repetitions per path")
    parser.add_argument("--inprocess-runs", type=int, default=100)
    parser.add_argument("--gpu-runs", type=int, default=30)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--elements", type=int, default=65536)
    parser.add_argument("--case", action="append", choices=CASES)
    parser.add_argument("--skip-runtime", action="store_true")
    parser.add_argument("--output", type=Path, help="New directory for report, generated shaders, and logs")
    args = parser.parse_args()
    if min(args.runs, args.inprocess_runs, args.gpu_runs) < 1 or args.warmup < 0:
        parser.error("Iteration counts must be positive; warmup must be nonnegative")
    if args.elements < 64 or args.elements % 64:
        parser.error("--elements must be a positive multiple of 64")
    args.compiler = args.compiler.resolve()
    if args.baseline_compiler:
        args.baseline_compiler = args.baseline_compiler.resolve()
        if not args.baseline_compiler.is_file():
            parser.error(f"Missing baseline compiler: {args.baseline_compiler}")
    args.oracle = args.oracle.resolve()
    args.tint = args.tint.resolve()
    engines = {"naga": args.oracle}
    if args.tint.is_file():
        engines["tint"] = args.tint
    elif not args.allow_missing_tint:
        parser.error("Tint driver missing; run tools/wgsl-bench/bootstrap_tint.py or explicitly --allow-missing-tint")
    for executable in (args.compiler, *engines.values()):
        if not executable.is_file():
            parser.error(f"Missing executable: {executable}")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    directory = (args.output or ROOT / "build/wgsl-benchmarks" / stamp).resolve()
    directory.mkdir(parents=True, exist_ok=False)
    report = {
        "schema_version": 1,
        "created_utc": stamp,
        "status": "running",
        "git_revision": execute(["git", "rev-parse", "HEAD"]).strip(),
        "git_status": execute(["git", "status", "--short"]),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": sys.version,
        "harness_sha256": digest(Path(__file__)),
        "settings": {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()},
        "tools": {"bwslc": metadata(args.compiler), **{name: metadata(path) for name, path in engines.items()}},
        "coverage": {"naga": True, "tint": "tint" in engines, "gpu_runtime": not args.skip_runtime},
        "methodology": {
            "process_wall": "Measured full subprocess sequences, including startup, file I/O, and shutdown; randomized path order with seed 113",
            "bwsl_reported_phases": "bwslc -timing self-report, displayed precision 0.001 ms; validation off; not equivalent to Naga/Tint whole translation",
            "in_process_cpu": "Input files preloaded; parse/validate/emit/allocation/destruction measured; each compiler's default writer options",
            "endpoints": "Compare paths ending in SPIR-V for source compilation; WGSL emission-only and SPIRV-to-WGSL paths are separate",
            "runtime": "Same wgpu/Naga adapter, buffers and independent CPU reference; exact except declared GGX float tolerance; module/pipeline timings are single observations",
            "limits": "Microkernels plus image filtering, skinning and GGX compute workloads; BWSL fixed work is unrolled; no vertex/fragment, texture or workgroup-memory coverage; default robustness transforms may differ",
            "bwsl_build": "Compiler executable hash is recorded; build flags must be controlled by the caller",
        },
        "cases": {},
    }
    tint_metadata = args.tint.parent / "build-metadata.json"
    if args.baseline_compiler:
        report["tools"]["bwslc_baseline"] = metadata(args.baseline_compiler)
    if "tint" in engines and tint_metadata.is_file():
        report["tools"]["tint"]["build"] = json.loads(tint_metadata.read_text())
    lockfile = ROOT / "tools/wgsl-oracle/Cargo.lock"
    if lockfile.is_file():
        report["tools"]["naga"]["cargo_lock_sha256"] = digest(lockfile)
    report_path = directory / "results.json"
    try:
        for name in args.case or CASES:
            print(f"Benchmarking {name}...", flush=True)
            case = run_case(name, args, engines, directory / name)
            report["cases"][name] = case
            for path, measured in case["process_paths"].items():
                summary = measured["process_wall"]
                print(f"  {path:<52} median {summary['median_us']:9.2f} us   p95 {summary['p95_us']:9.2f} us")
            report_path.write_text(json.dumps(report, indent=2) + "\n")
        report["status"] = "passed"
    except (RuntimeError, OSError, ValueError) as error:
        report["status"] = "failed"
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
    finally:
        report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Report: {report_path}")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
