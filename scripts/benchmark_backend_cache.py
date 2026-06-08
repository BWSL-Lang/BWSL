#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MODULES = REPO_ROOT / "modules"
DEFAULT_CANDIDATE = REPO_ROOT / "build" / ("bwslc.exe" if sys.platform == "win32" else "bwslc")

SUMMARY_RE = re.compile(r"^\s*(BWSL compile|Validation|Metal cross|HLSL cross|Gles cross|Total):\s+([0-9.]+)\s+ms", re.MULTILINE)


@dataclass(frozen=True)
class BenchmarkCase:
    name: str
    source: Path
    args: tuple[str, ...]
    description: str


@dataclass
class RunResult:
    wall_ms: float
    compiler_ms: dict[str, float]


DEFAULT_CASES = (
    BenchmarkCase(
        name="shadow_all",
        source=REPO_ROOT / "tests" / "from_engine" / "Shadow.bwsl",
        args=("-modules", str(DEFAULT_MODULES), "-all", "-validation", "strict", "-timing"),
        description="Real 4-pass shadow shader; contains duplicate vertex/fragment SPIR-V.",
    ),
    BenchmarkCase(
        name="particles_all",
        source=REPO_ROOT / "tests" / "from_engine" / "Particles.bwsl",
        args=("-modules", str(DEFAULT_MODULES), "-all", "-validation", "strict", "-timing"),
        description="Real particle shader; duplicate vertex stage across blend passes.",
    ),
    BenchmarkCase(
        name="postprocess_all",
        source=REPO_ROOT / "tests" / "from_engine" / "PostProcess.bwsl",
        args=("-modules", str(DEFAULT_MODULES), "-all", "-validation", "strict", "-timing"),
        description="Larger multipass shader; useful as a less duplicate-heavy control.",
    ),
    BenchmarkCase(
        name="shadow_check_strict",
        source=REPO_ROOT / "tests" / "from_engine" / "Shadow.bwsl",
        args=("-modules", str(DEFAULT_MODULES), "-check", "-validation", "strict", "-timing"),
        description="Validation-only duplicate reuse; no output or SPIRV-Cross work.",
    ),
    BenchmarkCase(
        name="particles_gles_direct",
        source=REPO_ROOT / "tests" / "from_engine" / "Particles.bwsl",
        args=("-modules", str(DEFAULT_MODULES), "-gles-direct", "-validation", "strict", "-timing"),
        description="Direct GLES path; exercises validation cache and avoids unused GLES SPIRV-Cross.",
    ),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Benchmark BWSL backend cache/validation overlap behavior. "
            "Pass both --baseline and --candidate to compute speedups."
        )
    )
    parser.add_argument("--baseline", type=Path, help="Path to a baseline bwslc binary.")
    parser.add_argument("--candidate", type=Path, default=DEFAULT_CANDIDATE, help="Path to the candidate bwslc binary.")
    parser.add_argument("--runs", type=int, default=30, help="Measured runs per case/compiler.")
    parser.add_argument("--warmup", type=int, default=5, help="Warmup runs per case/compiler.")
    parser.add_argument(
        "--case",
        action="append",
        choices=[case.name for case in DEFAULT_CASES],
        help="Run only this case. Can be repeated.",
    )
    parser.add_argument("--json", type=Path, help="Write machine-readable benchmark results.")
    parser.add_argument("--keep-output", type=Path, help="Keep compiler outputs under this directory.")
    parser.add_argument("--quiet", action="store_true", help="Only print result tables.")
    return parser.parse_args()


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    rank = (len(ordered) - 1) * pct
    lo = math.floor(rank)
    hi = math.ceil(rank)
    if lo == hi:
        return ordered[lo]
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (rank - lo)


def summarize(values: list[float]) -> dict[str, float]:
    return {
        "min": min(values),
        "median": statistics.median(values),
        "mean": statistics.fmean(values),
        "p95": percentile(values, 0.95),
        "max": max(values),
    }


def parse_timing(output: str) -> dict[str, float]:
    timings: dict[str, float] = {}
    for name, value in SUMMARY_RE.findall(output):
        key = {
            "BWSL compile": "bwsl_compile_ms",
            "Validation": "validation_ms",
            "Metal cross": "metal_cross_ms",
            "HLSL cross": "hlsl_cross_ms",
            "Gles cross": "gles_cross_ms",
            "Total": "compiler_total_ms",
        }[name]
        timings[key] = float(value)
    return timings


def compiler_label(path: Path) -> str:
    return str(path.resolve())


def checked_compiler(path: Path) -> Path:
    resolved = path.resolve()
    if not resolved.exists():
        raise FileNotFoundError(f"Compiler not found: {resolved}")
    if not resolved.is_file():
        raise FileNotFoundError(f"Compiler is not a file: {resolved}")
    return resolved


def run_once(compiler: Path, case: BenchmarkCase, output_dir: Path, run_index: int) -> RunResult:
    run_output = output_dir / f"{case.name}_{run_index:04d}"
    if run_output.exists():
        shutil.rmtree(run_output)
    run_output.mkdir(parents=True)

    cmd = [str(compiler), str(case.source), *case.args, "-o", str(run_output)]
    start = time.perf_counter()
    completed = subprocess.run(
        cmd,
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    wall_ms = (time.perf_counter() - start) * 1000.0
    if completed.returncode != 0:
        tail = "\n".join(completed.stdout.splitlines()[-40:])
        raise RuntimeError(
            f"{case.name} failed for {compiler} with exit code {completed.returncode}\n{tail}"
        )

    timings = parse_timing(completed.stdout)
    if "compiler_total_ms" not in timings:
        tail = "\n".join(completed.stdout.splitlines()[-40:])
        raise RuntimeError(f"{case.name} did not produce timing summary for {compiler}\n{tail}")

    return RunResult(wall_ms=wall_ms, compiler_ms=timings)


def run_case(compiler: Path, case: BenchmarkCase, runs: int, warmup: int, root_output: Path, quiet: bool) -> dict[str, object]:
    if not quiet:
        print(f"  {case.name}: warmup {warmup}, measured {runs}")

    for i in range(warmup):
        run_once(compiler, case, root_output / "warmup", i)

    run_results = [run_once(compiler, case, root_output / "measured", i) for i in range(runs)]
    metric_names = sorted({key for result in run_results for key in result.compiler_ms})
    metrics: dict[str, dict[str, float]] = {
        "wall_ms": summarize([result.wall_ms for result in run_results])
    }
    for metric in metric_names:
        metrics[metric] = summarize([result.compiler_ms[metric] for result in run_results])

    return {
        "description": case.description,
        "source": str(case.source.relative_to(REPO_ROOT)),
        "args": list(case.args),
        "metrics": metrics,
    }


def speedup_pct(baseline_ms: float, candidate_ms: float) -> float:
    if baseline_ms <= 0:
        return math.nan
    return (baseline_ms - candidate_ms) / baseline_ms * 100.0


def print_single_table(label: str, results: dict[str, object]) -> None:
    print(f"\n{label}")
    print(f"{'case':<24} {'wall med':>10} {'compiler med':>13} {'validation':>11} {'metal':>9} {'hlsl':>9} {'gles':>9}")
    for case_name, case_result in results.items():
        metrics = case_result["metrics"]
        print(
            f"{case_name:<24} "
            f"{metrics['wall_ms']['median']:10.3f} "
            f"{metrics['compiler_total_ms']['median']:13.3f} "
            f"{metrics.get('validation_ms', {'median': 0.0})['median']:11.3f} "
            f"{metrics.get('metal_cross_ms', {'median': 0.0})['median']:9.3f} "
            f"{metrics.get('hlsl_cross_ms', {'median': 0.0})['median']:9.3f} "
            f"{metrics.get('gles_cross_ms', {'median': 0.0})['median']:9.3f}"
        )


def print_comparison_table(baseline: dict[str, object], candidate: dict[str, object]) -> None:
    print("\nComparison")
    print(
        f"{'case':<24} {'base wall':>10} {'cand wall':>10} {'wall win':>9} "
        f"{'base total':>11} {'cand total':>11} {'total win':>10}"
    )
    for case_name in baseline:
        base_metrics = baseline[case_name]["metrics"]
        cand_metrics = candidate[case_name]["metrics"]
        base_wall = base_metrics["wall_ms"]["median"]
        cand_wall = cand_metrics["wall_ms"]["median"]
        base_total = base_metrics["compiler_total_ms"]["median"]
        cand_total = cand_metrics["compiler_total_ms"]["median"]
        print(
            f"{case_name:<24} "
            f"{base_wall:10.3f} {cand_wall:10.3f} {speedup_pct(base_wall, cand_wall):8.1f}% "
            f"{base_total:11.3f} {cand_total:11.3f} {speedup_pct(base_total, cand_total):9.1f}%"
        )


def main() -> int:
    args = parse_args()
    if args.runs <= 0:
        raise ValueError("--runs must be positive")
    if args.warmup < 0:
        raise ValueError("--warmup cannot be negative")

    selected_names = set(args.case or [case.name for case in DEFAULT_CASES])
    cases = [case for case in DEFAULT_CASES if case.name in selected_names]
    compilers = {"candidate": checked_compiler(args.candidate)}
    if args.baseline:
        compilers = {
            "baseline": checked_compiler(args.baseline),
            "candidate": checked_compiler(args.candidate),
        }

    output_context = tempfile.TemporaryDirectory(prefix="bwsl_backend_bench_")
    output_root = Path(output_context.name)
    if args.keep_output:
        output_context.cleanup()
        output_root = args.keep_output.resolve()
        if output_root.exists():
            shutil.rmtree(output_root)
        output_root.mkdir(parents=True)

    results: dict[str, object] = {
        "runs": args.runs,
        "warmup": args.warmup,
        "compilers": {name: compiler_label(path) for name, path in compilers.items()},
        "cases": {},
    }

    try:
        for compiler_name, compiler in compilers.items():
            if not args.quiet:
                print(f"\nBenchmarking {compiler_name}: {compiler}")
            compiler_results: dict[str, object] = {}
            for case in cases:
                compiler_results[case.name] = run_case(
                    compiler,
                    case,
                    args.runs,
                    args.warmup,
                    output_root / compiler_name / case.name,
                    args.quiet,
                )
            results["cases"][compiler_name] = compiler_results

        for compiler_name, compiler_results in results["cases"].items():
            print_single_table(compiler_name, compiler_results)

        if "baseline" in results["cases"]:
            print_comparison_table(results["cases"]["baseline"], results["cases"]["candidate"])

        if args.json:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(json.dumps(results, indent=2), encoding="utf-8")
            print(f"\nWrote {args.json}")
    finally:
        if not args.keep_output:
            output_context.cleanup()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
