#!/usr/bin/env python3
"""Runs the compute equivalence suite through the WGSL backend on the GPU.

Each compute test in tests/equivalence is compiled with -wgsl and with
SPIR-V. The WGSL runs through the wgsl-oracle (wgpu) and its output buffer
is compared with the test's expected_values when it has them, otherwise with
the same shader's SPIR-V, which Naga translates to WGSL and runs alongside.
Raster tests and tests with several compute shaders are not covered here.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
EQUIVALENCE = ROOT / "tests/equivalence"
GPU_UNAVAILABLE = 77


def words(values: list, kind: str) -> list[int]:
    if kind == "float":
        return [struct.unpack("<I", struct.pack("<f", value))[0] for value in values]
    return [int(value) & 0xFFFFFFFF for value in values]


def as_float(word: int) -> float:
    return struct.unpack("<f", struct.pack("<I", word))[0]


def command(argv: list) -> subprocess.CompletedProcess:
    return subprocess.run([str(item) for item in argv], cwd=ROOT, capture_output=True,
                          text=True, timeout=300)


def run_buffers(oracle: Path, shader: Path, case: dict, path: Path) -> tuple[str, list[int]]:
    path.write_text(json.dumps(case, indent=2) + "\n")
    result = command([oracle, "run", shader, path])
    if result.returncode == GPU_UNAVAILABLE:
        return "skip", []
    if result.returncode != 0:
        return result.stdout + result.stderr, []
    output = json.loads(result.stdout)
    binding = case["buffers"][-1]["binding"]
    for buffer in output.get("buffers", []):
        if buffer["binding"] == binding:
            return "", buffer["data"]
    return "Oracle output has no result buffer", []


def compare(actual: list[int], reference: list[int], kind: str, tolerance: float) -> list[str]:
    errors = []
    for index, (got, want) in enumerate(zip(actual, reference)):
        if got == want:
            continue
        if kind == "float":
            got_value, want_value = as_float(got), as_float(want)
            if math.isnan(got_value) and math.isnan(want_value):
                continue
            # An infinite bound would accept any value.
            if not math.isinf(want_value) and \
                    abs(got_value - want_value) <= max(tolerance, 1e-5) * max(1.0, abs(want_value)):
                continue
            errors.append(f"word {index}: {got_value} != {want_value}")
        else:
            errors.append(f"word {index}: {got} != {want}")
        if len(errors) == 4:
            break
    return errors


def run_test(source: Path, args: argparse.Namespace) -> tuple[str, str, list[str]]:
    name = source.stem
    config = json.loads(source.with_suffix(".json").read_text())
    if config.get("mode") == "raster" or any(key in config for key in ("resources", "textures", "passes")):
        return name, "skip", ["raster test"]
    directory = args.output / name
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    wgsl = command([args.compiler, source, "-wgsl", "-modules", ROOT / "modules", "-o", directory / "wgsl"])
    if wgsl.returncode != 0:
        return name, "fail", ["WGSL compilation failed", (wgsl.stdout + wgsl.stderr).strip()[-600:]]
    shaders = list((directory / "wgsl").glob("*.comp.wgsl"))
    if len(shaders) != 1:
        return name, "skip", ["not exactly one compute shader"]
    if "enable subgroups;" in shaders[0].read_text():
        return name, "skip", ["Naga 27 does not parse `enable subgroups;`"]

    output_words = config.get("output_size", 16) // 4
    output_binding = config.get("output_binding", 0)
    kind = config.get("output_type", "float")

    def case(group: int) -> dict:
        buffers = []
        if "input_values" in config:
            buffers.append({"binding": config.get("input_binding", 0), "group": group,
                            "data": words(config["input_values"], config.get("input_type", "float"))})
        buffers.append({"binding": output_binding, "group": group, "data": [0] * output_words})
        return {"workgroups": config.get("groups", [1, 1, 1]), "iterations": 1, "warmup": 0,
                "buffers": buffers}

    status, actual = run_buffers(args.oracle, shaders[0], case(0), directory / "wgsl.json")
    if status == "skip":
        return name, "skip", ["no WebGPU adapter"]
    if status:
        return name, "fail", ["WGSL run failed", status.strip()[-600:]]

    if "expected_values" in config:
        reference = words(config["expected_values"], kind)
    else:
        # The SPIR-V binds storage buffers in descriptor set 1.
        spirv = command([args.compiler, source, "-modules", ROOT / "modules", "-o", directory / "spirv"])
        modules = list((directory / "spirv").glob("*.comp.spv"))
        if spirv.returncode != 0 or len(modules) != 1:
            return name, "fail", ["SPIR-V compilation failed", (spirv.stdout + spirv.stderr).strip()[-600:]]
        translated = directory / "spirv.wgsl"
        translation = command([args.oracle, "translate", modules[0], translated])
        if translation.returncode != 0:
            return name, "skip", ["Naga cannot translate the SPIR-V reference and the test has no expected_values"]
        status, reference = run_buffers(args.oracle, translated, case(1), directory / "spirv.json")
        if status:
            return name, "fail", ["SPIR-V reference run failed", status.strip()[-600:]]
    errors = compare(actual, reference, kind, config.get("tolerance", 0.0))
    return name, "fail" if errors else "pass", errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=ROOT / "build/bwslc")
    executable = "wgsl-oracle.exe" if sys.platform == "win32" else "wgsl-oracle"
    parser.add_argument("--oracle", type=Path, default=ROOT / "build/wgsl-oracle/release" / executable)
    parser.add_argument("--output", type=Path, default=ROOT / "build/wgsl-equivalence")
    parser.add_argument("--require-gpu", action="store_true")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("tests", nargs="*", help="test names to run (default: all)")
    args = parser.parse_args()
    args.compiler = args.compiler.resolve()
    args.oracle = args.oracle.resolve()
    args.output = args.output.resolve()
    for tool in (args.compiler, args.oracle):
        if not tool.is_file():
            print(f"Missing {tool}")
            return 1
    sources = sorted(path for path in EQUIVALENCE.glob("*.bwsl") if path.with_suffix(".json").is_file())
    if args.tests:
        sources = [path for path in sources if path.stem in args.tests]
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(lambda source: run_test(source, args), sources))
    counts = {"pass": 0, "fail": 0, "skip": 0}
    for name, status, notes in results:
        counts[status] += 1
        if status == "fail" or (status == "skip" and notes[0] != "raster test"):
            print(f"[{status.upper()}] {name}")
            for note in notes:
                print(f"  {note}")
    gpu_missing = any(notes == ["no WebGPU adapter"] for _, _, notes in results)
    print(f"WGSL equivalence: {counts['pass']} passed, {counts['fail']} failed, {counts['skip']} skipped")
    if gpu_missing and args.require_gpu:
        print("A GPU was required but no WebGPU adapter is available")
        return 1
    return int(counts["fail"] != 0)


if __name__ == "__main__":
    raise SystemExit(main())
