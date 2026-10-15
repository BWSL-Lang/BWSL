#!/usr/bin/env python3
"""Fresh-source in-process compilation, CLI wall time and Naga/Tint diagnostics."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def summary(samples):
    ordered = sorted(samples)
    return {"median_us": statistics.median(ordered), "min_us": ordered[0],
            "p95_us": ordered[min(len(ordered) - 1, int(len(ordered) * 0.95))], "samples_us": samples}


def run(command):
    start = time.perf_counter_ns()
    process = subprocess.run([str(item) for item in command], cwd=ROOT, capture_output=True, text=True, timeout=60)
    elapsed = (time.perf_counter_ns() - start) / 1000
    if process.returncode:
        raise ValueError(f"{command[0]} failed: {process.stdout}{process.stderr}")
    return process, elapsed


def main():
    suffix = ".exe" if sys.platform == "win32" else ""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", type=Path, default=ROOT / f"build/wgsl-inprocess{suffix}")
    parser.add_argument("--compiler", type=Path, default=ROOT / f"build/bwslc{suffix}")
    parser.add_argument("--baseline-driver", type=Path)
    parser.add_argument("--baseline-compiler", type=Path)
    parser.add_argument("--naga", type=Path, default=ROOT / f"build/wgsl-oracle/release/wgsl-oracle{suffix}")
    parser.add_argument("--tint", type=Path, default=ROOT / f"build/wgsl-tools/tint/bwsl-tint-bench{suffix}")
    parser.add_argument("--blocks", type=int, default=10)
    parser.add_argument("--samples-per-block", type=int, default=20)
    parser.add_argument("--cli-runs", type=int, default=10)
    parser.add_argument("--reference-runs", type=int, default=100)
    parser.add_argument("--case", action="append")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if min(args.blocks, args.samples_per_block, args.cli_runs, args.reference_runs) < 1:
        parser.error("Sample counts must be positive")
    if bool(args.baseline_driver) != bool(args.baseline_compiler):
        parser.error("Supply both baseline executables")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output = (args.output or ROOT / "build/wgsl-inprocess-benchmarks" / stamp).resolve()
    output.mkdir(parents=True, exist_ok=False)
    drivers = {"candidate": (args.driver.resolve(), args.compiler.resolve())}
    if args.baseline_driver:
        drivers["baseline"] = (args.baseline_driver.resolve(), args.baseline_compiler.resolve())
    corpus = []
    for directory in (ROOT / "tests/wgsl_benchmarks", ROOT / "tests/wgsl_render/benchmarks"):
        for source in sorted(directory.glob("*.bwsl")):
            if not source.stem.endswith("_unrolled") and (not args.case or source.stem in args.case):
                corpus.append(source)
    if not corpus:
        parser.error("No matching source cases")
    report = {"schema_version": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
              "scope": "Source preloaded; fresh frontend, AST, IR, CFG/SSA, WGSL emission and owning source/reflection each sample, including compiler-state destruction. Excludes source/output I/O and process startup. Native tool diagnostics are WGSL parse/validate, not browser pipeline compilation.",
              "settings": vars(args) | {"output": output}, "git_revision": subprocess.check_output(
                  ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
              "executables": {name: {"driver": {"path": driver, "sha256": sha256(driver)},
                                     "compiler": {"path": compiler, "sha256": sha256(compiler)}}
                              for name, (driver, compiler) in drivers.items()},
              "reference_tools": {name: {"path": path.resolve(), "sha256": sha256(path)}
                                  for name, path in (("naga", args.naga), ("tint", args.tint))}, "cases": {}}
    for source in corpus:
        name = source.stem
        directory = output / name
        directory.mkdir()
        (directory / "source.bwsl").write_bytes(source.read_bytes())
        case = {"bwsl_sha256": sha256(source), "variants": {}, "reference_validation": {}}
        report["cases"][name] = case
        for variant, (driver, compiler) in drivers.items():
            target = directory / variant
            target.mkdir()
            command = [compiler, source, "-wgsl", "-bindings", "-validation", "off", "-o", target]
            try:
                process, _ = run(command)
            except ValueError as error:
                if variant != "baseline":
                    raise
                case["variants"][variant] = {"supported": False, "diagnostic": str(error)}
                continue
            (target / "compile.log").write_text(process.stdout + process.stderr)
            emitted = sorted(target.glob("*.wgsl"))
            driver_output = target / "inprocess"
            process, _ = run([driver, source, 1, 1, driver_output])
            result = json.loads(process.stdout)
            generated = sorted(driver_output.glob("*.wgsl"))
            if sorted(path.read_bytes() for path in emitted) != sorted(path.read_bytes() for path in generated):
                raise ValueError(f"CLI/in-process source mismatch: {name}/{variant}")
            cli_bindings = sorted(json.dumps(json.loads(Path(str(path) + ".bindings.json").read_text()), sort_keys=True) for path in emitted)
            driver_bindings = sorted(json.dumps(json.loads(path.read_text()), sort_keys=True) for path in driver_output.glob("*.json"))
            if cli_bindings != driver_bindings:
                raise ValueError(f"CLI/in-process reflection mismatch: {name}/{variant}")
            case["variants"][variant] = {"supported": True, "source_bytes": result["wgsl_bytes"],
                "arena_bytes": result["arena_bytes"], "source_hashes": [sha256(path) for path in emitted],
                "raw_samples": [], "cli_samples_us": []}
        for block in range(args.blocks):
            order = list(drivers) if block % 2 == 0 else list(reversed(drivers))
            for variant in order:
                data = case["variants"][variant]
                if not data["supported"]:
                    continue
                process, _ = run([drivers[variant][0], source, args.samples_per_block, 5])
                data["raw_samples"].extend(json.loads(process.stdout)["samples"])
        for repetition in range(args.cli_runs):
            order = list(drivers) if repetition % 2 == 0 else list(reversed(drivers))
            for variant in order:
                data = case["variants"][variant]
                if data["supported"]:
                    _, elapsed = run([drivers[variant][1], source, "-wgsl", "-bindings", "-validation", "off", "-o", directory / variant])
                    data["cli_samples_us"].append(elapsed)
        for variant, data in case["variants"].items():
            if not data["supported"]:
                continue
            data["inprocess"] = summary([sample["total_us"] for sample in data["raw_samples"]])
            data["phases"] = {key: summary([sample[key] for sample in data["raw_samples"]])
                              for key in data["raw_samples"][0] if key != "total_us"}
            data["cli"] = summary(data["cli_samples_us"])
        for tool, executable in (("naga", args.naga), ("tint", args.tint)):
            validations = {}
            for variant in ("handwritten", "candidate"):
                files = sorted(source.parent.glob(f"{name}*.wgsl")) if variant == "handwritten" else sorted((directory / variant).glob("*.wgsl"))
                # Expanded compute controls are a separate comparison, not extra stages.
                files = [path for path in files if "_unrolled" not in path.stem]
                validations[variant] = {}
                for shader in files:
                    process, _ = run([executable, "bench", shader, "validate", args.reference_runs, 5])
                    validations[variant][shader.name] = {"sha256": sha256(shader), **json.loads(process.stdout)}
            case["reference_validation"][tool] = validations
        current = case["variants"]["candidate"]
        baseline = case["variants"].get("baseline", {})
        ratio = current["inprocess"]["median_us"] / baseline["inprocess"]["median_us"] if baseline.get("supported") else None
        print(f"{name}: in-process {current['inprocess']['median_us']:.2f} us; CLI {current['cli']['median_us']:.2f} us; candidate/baseline {ratio}", flush=True)
        (output / "results.json").write_text(json.dumps(report, indent=2, default=str) + "\n")
    print(f"Saved {output / 'results.json'}")


if __name__ == "__main__":
    main()
