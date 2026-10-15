#!/usr/bin/env python3
"""Summarize repeated browser runs without pooling different shaders or devices."""

import argparse
import hashlib
import json
from pathlib import Path


def summarize(paths):
    groups = {}
    seen_reports = set()
    for path in paths:
        contents = path.read_bytes()
        digest = hashlib.sha256(contents).hexdigest()
        if digest in seen_reports:
            raise ValueError(f"Duplicate benchmark report: {path}")
        seen_reports.add(digest)
        report = json.loads(contents)
        if report.get("status") != "passed":
            raise ValueError(f"Not a successful benchmark report: {path}")
        for name, case in report["cases"].items():
            if case.get("validation_only"):
                continue
            identity = {
                "case": name,
                "kind": report.get("kind", "compute"),
                "compilation_mode": report.get("compilation_mode", "fresh-cli"),
                "inprocess_compiler": report.get("inprocess_compiler"),
                "compiler_sha256": report["compiler"]["sha256"],
                "reference_compiler": report.get("reference_compiler"),
                "source_hashes": case["source_hashes"],
                "harness_sha256": report["harness_sha256"],
                "server_harness_sha256": report.get("server_harness_sha256"),
                "environment": report["environment"],
                "elements": report["settings"].get("elements"),
                "resolution": report["settings"].get("resolution"),
                "render_workload": case.get("workload"),
                "baseline_style": report.get("baseline_style"),
            }
            key = json.dumps(identity, sort_keys=True)
            group = groups.setdefault(key, {"identity": identity, "runs": []})
            group["runs"].append({
                "report": str(path.resolve()),
                "gpu_ratio": case.get("paired_gpu_ratio"),
                "gpu_medians_us": {
                    variant: (data.get("gpu_per_frame") or data.get("gpu_per_dispatch") or {}).get("median_us")
                    for variant, data in case["variants"].items()
                },
                "full_compilation_medians_us": {
                    variant: data.get("full_source_to_ready_us", {}).get("median_us")
                    for variant, data in case["compilation"].items()
                },
                "batch_iterations": case.get("frames_per_batch", case.get("dispatches_per_batch")),
            })
    for group in groups.values():
        ratios = [run["gpu_ratio"] for run in group["runs"]]
        if len(ratios) < 2:
            conclusion = "Needs repeated runs"
        elif any(ratio is None for ratio in ratios):
            conclusion = "GPU timestamps unavailable"
        elif all(ratio["bootstrap_95_interval"][1] < 1 for ratio in ratios):
            conclusion = "BWSL faster in every recorded interval"
        elif all(ratio["bootstrap_95_interval"][0] > 1 for ratio in ratios):
            conclusion = "BWSL slower in every recorded interval"
        else:
            conclusion = "No consistent direction outside the recorded intervals"
        group["gpu_observation"] = conclusion
    return {
        "schema_version": 1,
        "scope": "Observed repeatability on this browser/GPU/corpus; per-run intervals do not cover thermal drift or systematic quantization. Different source, compiler, harness and device identities are never pooled.",
        "comparisons": list(groups.values()),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, help="New JSON summary file")
    args = parser.parse_args()
    try:
        result = summarize(args.reports)
        if args.output:
            with args.output.open("x") as output:
                json.dump(result, output, indent=2)
                output.write("\n")
        for group in result["comparisons"]:
            identity = group["identity"]
            ratios = [round(run["gpu_ratio"]["median"], 3) if run["gpu_ratio"] else None for run in group["runs"]]
            print(f"{identity['case']}: {group['gpu_observation']}; paired BWSL/reference ratios {ratios}")
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
