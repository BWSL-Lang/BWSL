#!/usr/bin/env python3
"""Compare one render-corpus CLI batch with one fresh process per pipeline."""

import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path,
                        help="Prepared render benchmark manifest with validated source snapshots")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=2)
    args = parser.parse_args()
    if args.runs < 1 or args.warmup < 0:
        parser.error("runs must be positive and warmup nonnegative")
    manifest = json.loads(args.manifest.read_text())
    compiler = Path(manifest["compiler"]["path"])
    if sha256(compiler) != manifest["compiler"]["sha256"]:
        parser.error("compiler differs from the prepared manifest")
    cases = [case for case in manifest["cases"] if not case.get("validation_only")]
    if len(cases) < 2:
        parser.error("batch comparison needs at least two pipelines")
    args.output.mkdir(parents=True, exist_ok=False)
    input_directory = args.output / "inputs"
    input_directory.mkdir()
    sources = []
    for case in cases:
        source = args.manifest.parent / case["name"] / "source.bwsl"
        if sha256(source) != case["bwsl_sha256"]:
            parser.error(f"source snapshot changed: {source}")
        named_source = input_directory / f"{case['name']}.bwsl"
        named_source.write_bytes(source.read_bytes())
        sources.append(named_source.resolve())

    def compile_corpus(mode):
        destination = (args.output / mode).resolve()
        destination.mkdir(exist_ok=True)
        inputs = [sources] if mode == "batch" else [[source] for source in sources]
        start = time.perf_counter_ns()
        for group in inputs:
            command = [str(compiler), *map(str, group), "-wgsl", "-bindings",
                       "-validation", "off", "-timing", "-o", str(destination)]
            subprocess.run(command, cwd=ROOT, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, check=True, timeout=60)
        elapsed_us = (time.perf_counter_ns() - start) / 1000
        # Validation is outside the measured window. Every observation must
        # match the independently validated browser corpus, including its ABI.
        for case in cases:
            for stage, suffix in [("vertex", "vert"), ("fragment", "frag")]:
                expected = case["variants"]["bwsl"][stage]
                shader = destination / f"{case['name']}.{suffix}.wgsl"
                if sha256(shader) != expected["sha256"]:
                    raise ValueError(f"WGSL changed: {shader}")
                interface = json.loads(Path(str(shader) + ".bindings.json").read_text())
                if interface != expected["interface"]:
                    raise ValueError(f"reflection changed: {shader}")
        if list(destination.glob("*.spv")):
            raise ValueError("direct WGSL compilation emitted SPIR-V")
        return elapsed_us

    samples = {"batch": [], "separate_processes": []}
    for iteration in range(args.warmup + args.runs):
        order = ["batch", "separate_processes"]
        if iteration % 2:
            order.reverse()
        for mode in order:
            elapsed_us = compile_corpus(mode)
            if iteration >= args.warmup:
                samples[mode].append(elapsed_us)
    if sha256(compiler) != manifest["compiler"]["sha256"]:
        raise ValueError("compiler changed during measurement")
    result = {
        "schema_version": 1,
        "scope": "Whole-corpus source-to-WGSL/reflection CLI wall time. Includes process startup, source reads and output files. No browser/pipeline readiness or per-pipeline latency is measured. Batch jobs execute sequentially and share module source discovery; filesystem/process caches remain warm.",
        "compiler": manifest["compiler"],
        "render_manifest_sha256": sha256(args.manifest),
        "harness_sha256": sha256(Path(__file__)),
        "pipelines": {case["name"]: case["bwsl_sha256"] for case in cases},
        "settings": {"runs": args.runs, "warmup": args.warmup},
        "source_and_reflection_match": True,
        "modes": {mode: {"samples_us": values,
                          "median_corpus_us": statistics.median(values),
                          "amortized_us_per_pipeline": statistics.median(values) / len(cases)}
                  for mode, values in samples.items()},
    }
    (args.output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    for mode, values in result["modes"].items():
        print(f"{mode}: {values['median_corpus_us'] / 1000:.3f} ms / {len(cases)} pipelines")


if __name__ == "__main__":
    main()
