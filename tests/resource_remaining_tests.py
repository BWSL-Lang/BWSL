#!/usr/bin/env python3
"""Source/target diagnostic regressions for the remaining resource QA issues."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def run_resource_remaining_suite(compiler: Path, output_dir: Path, verbose=False):
    compiler = Path(compiler).resolve()
    output = Path(output_dir).resolve() / "resource_remaining"
    output.mkdir(parents=True, exist_ok=True)
    fixtures = ROOT / "tests/resource_remaining"
    records = []

    def run(name, source, flags, expected=None, artifact=None):
        directory = output / name
        if directory.exists():
            shutil.rmtree(directory)
        directory.mkdir()
        command = [str(compiler), str(source), *flags, "-validation", "strict", "-o", str(directory)]
        row = {"name": name, "command": command, "errors": []}
        try:
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=60)
            row.update(exit=result.returncode, output=result.stdout + result.stderr)
            if expected:
                if result.returncode == 0 or expected not in row["output"]:
                    row["errors"].append("Missing nonzero exit or requested diagnostic: " + expected)
                failed_stage = "*.frag.metal" if source.stem == "limits_textures_17" else "*.metal"
                if list(directory.glob(failed_stage)):
                    row["errors"].append("Failed Metal target wrote a shader artifact")
            elif result.returncode != 0:
                row["errors"].append("Unexpected compilation failure")
            if artifact and not list(directory.glob(artifact)):
                row["errors"].append("Missing successful target artifact: " + artifact)
            if expected is None and sys.platform == "darwin" and shutil.which("xcrun"):
                for shader in directory.glob("*.metal"):
                    validation = subprocess.run(["xcrun", "-sdk", "macosx", "metal", "-c", str(shader), "-o", "/dev/null"],
                                                capture_output=True, text=True, timeout=60)
                    if validation.returncode:
                        row["errors"].append("Metal validation failed: " + validation.stdout + validation.stderr)
        except (subprocess.SubprocessError, OSError) as error:
            row["errors"].append(str(error))
        print(f"[{'FAIL' if row['errors'] else 'PASS'}] resource_remaining/{name}")
        if row["errors"] or verbose:
            print(row.get("output", ""))
            for error in row["errors"]:
                print("  " + error)
        records.append(row)
        return row

    for source in sorted(fixtures.glob("*.bwsl")):
        if "bool" in source.stem:
            for flag in ("-check", "-all"):
                row = run(source.stem + flag, source, [flag], "contains bool storage, which is unsupported")
                if "SPIR-V validation failed" in row.get("output", ""):
                    row["errors"].append("Bool resource reached SPIR-V validation instead of source diagnosis")
        elif source.stem.startswith("limits_"):
            run(source.stem + "-spv", source, ["-spv"], artifact="*.spv")
            expected = ("Metal supports buffer bindings 0 through 30" if source.stem == "limits_buffers_31" else
                        "Metal supports at most 16 active samplers" if source.stem == "limits_textures_17" else None)
            run(source.stem + "-metal", source, ["-metal"], expected, None if expected else "*.metal")
            if expected:
                run(source.stem + "-all", source, ["-all"], expected)
        elif source.stem.startswith("num_workgroups"):
            row = run(source.stem, source, ["-all"], artifact="*.hlsl")
            for reflection in (output / source.stem).glob("*.bindings.json"):
                data = json.loads(reflection.read_text())
                auxiliary = data.get("hlslResources", [])
                if len(auxiliary) != 1 or auxiliary[0].get("builtin") != "num_workgroups" or \
                        auxiliary[0].get("set") != 3 or auxiliary[0].get("binding") != 0 or auxiliary[0].get("byteSize") != 16:
                    row["errors"].append("Incorrect HLSL dispatch-count ABI reflection")
                if any(resource["name"] == "bwsl_num_workgroups" for resource in data["resources"]):
                    row["errors"].append("HLSL auxiliary buffer leaked into common resource bindings")
        else:
            run(source.stem, source, ["-metal"], artifact="*.metal")

    compute = fixtures / "uint_storage_control.bwsl"
    run("explicit-gles-unavailable", compute, ["-gles"], "does not support storage buffers")
    combined = run("all-gles-capability-exclusion", compute, ["-all"], artifact="*.metal")
    if "target omitted by -all" not in combined.get("output", ""):
        combined["errors"].append("-all omitted an unavailable target without explaining it")
    wave = ROOT / "tests/equivalence/test_wave_ops.bwsl"
    run("explicit-glsl-subgroup-unavailable", wave, ["-glsl"], "only supported in Vulkan semantics")
    run("all-glsl-subgroup-exclusion", wave, ["-all"], artifact="*.hlsl")

    (output / "results.json").write_text(json.dumps(records, indent=2))
    failed = sum(bool(row["errors"]) for row in records)
    return len(records) - failed, failed


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=ROOT / "build/bwslc")
    parser.add_argument("--output", type=Path, default=ROOT / "build/resource_remaining")
    args = parser.parse_args()
    passed, failed = run_resource_remaining_suite(args.compiler, args.output)
    print(f"Remaining resource checks: {passed} passed, {failed} failed")
    raise SystemExit(bool(failed))
