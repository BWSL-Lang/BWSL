#!/usr/bin/env python3
"""Regression checks for resource bindings, 3D images and sampler identity."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def run_resource_p1_suite(compiler: Path, output_dir: Path, verbose: bool = False) -> tuple[int, int]:
    compiler = Path(compiler).resolve()
    out = Path(output_dir).resolve() / "resource_p1"
    out.mkdir(parents=True, exist_ok=True)
    records = []
    passed = failed = 0

    def command(args):
        result = subprocess.run([str(v) for v in args], cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        return {"command": [str(v) for v in args], "exit": result.returncode, "output": result.stdout}

    sources = sorted((ROOT / "tests/resource_p1").glob("*.bwsl"))
    sources += [ROOT / "tests/unsorted/Particles.bwsl", ROOT / "tests/unsorted/raster_vertex_skinning.bwsl"]
    for source in sources:
        directory = out / source.stem
        directory.mkdir(exist_ok=True)
        row = {"source": str(source.relative_to(ROOT)), "steps": [], "errors": []}
        result = command([compiler, source, "-metal", "-spv", "-bindings", "-validation", "strict", "-o", directory])
        row["steps"].append(result)
        if result["exit"]:
            row["errors"].append("BWSL compilation failed")
        else:
            metal_files = sorted(directory.glob("*.metal"))
            metal = "\n".join(p.read_text() for p in metal_files)
            if not metal_files:
                row["errors"].append("Missing Metal output")
            bindings = [r for p in directory.glob("*.bindings.json") for r in json.loads(p.read_text())["resources"]]
            by_name = {r["name"]: r for r in bindings}
            if source.stem == "binding_collision":
                if by_name.get("scale", {}).get("binding") != 1 or by_name.get("values", {}).get("binding") != 2:
                    row["errors"].append("Reflection does not match collision-free buffer slots")
            if source.stem.startswith("texture3D_") or source.stem == "texture3d_drops_z":
                if "texture3d<float>" not in metal or "texture2d<float>" in metal:
                    row["errors"].append("3D texture dimension was lost")
            if source.stem == "texture_slot_32":
                if set(by_name) != {"tex32"} or by_name["tex32"]["binding"] != 0 or ".sample(" not in metal:
                    row["errors"].append("High-slot live texture was not compacted/reflected")
            if source.stem == "resource_slot_32_mixed":
                if set(by_name) != {"lead", "tex32"} or by_name["tex32"]["binding"] != 1 or by_name["lead"]["binding"] != 0:
                    row["errors"].append("Compaction changed a live low slot or lost the high texture")
            if source.stem in {"explicit_samplers_collapse", "sampler_gpu", "sampler_cross_stage", "sampler_operations", "sampler_mixed_default", "sampler_helper", "sampler_shared"}:
                samplers = [r for r in bindings if r["type"] == "sampler"]
                if not samplers or any(r.get("abi") != "sampler" or r["set"] != 2 for r in samplers):
                    row["errors"].append("Independent sampler descriptor ABI missing")
                if source.stem != "sampler_shared" and len(samplers) < 2:
                    row["errors"].append("Independent samplers collapsed")
                if source.stem == "sampler_mixed_default" and not any(r.get("defaultSamplerFor") == "tex" for r in samplers):
                    row["errors"].append("Implicit default sampler mapping missing")
            if source.stem == "pipeline_scope" and by_name.get("tex", {}).get("abi") != "combined_sampled_image":
                row["errors"].append("A previous pipeline changed the current pipeline's sampler ABI")
            for path in metal_files:
                for signature in re.findall(r"(?:vertex|fragment|kernel) [^\n]+", path.read_text()):
                    for kind in ("buffer", "texture", "sampler"):
                        slots = re.findall(r"\[\[" + kind + r"\((\d+)\)\]\]", signature)
                        if len(slots) != len(set(slots)):
                            row["errors"].append(f"Duplicate Metal {kind} binding")
                if sys.platform == "darwin" and shutil.which("xcrun"):
                    result = command(["xcrun", "-sdk", "macosx", "metal", "-c", path, "-o", "/dev/null"])
                    row["steps"].append(result)
                    if result["exit"]: row["errors"].append("Metal validation failed")
        good = not row["errors"]
        passed += int(good)
        failed += int(not good)
        print(f"[{'PASS' if good else 'FAIL'}] resource_p1/{source.stem}")
        if not good or verbose:
            for error in row["errors"]: print("  " + error)
            for step in row["steps"]:
                if step["exit"]: print(step["output"])
        records.append(row)

    # Validate both OpenGL output paths and the host-visible pair mapping.
    for name in ("texture3d_drops_z", "texture_slot_32", "resource_slot_32_mixed", "sampler_gpu", "sampler_shared",
                 "sampler_mixed_default", "sampler_cross_stage", "sampler_helper", "pipeline_scope",
                 "sampler_operations", "texture_query_size_levels", "pointer_address_taken_control_flow",
                 "texture_sample_grad_cmp_gather", "gl_interface_names", "gl_interface_names_mixed", "gl_interface_names_keyword_fallback", "gl_interface_names_nested"):
        for mode in ("-gles", "-gles-direct"):
            directory = out / (name + mode)
            directory.mkdir(exist_ok=True)
            source = ROOT / "tests/resource_p1" / (name + ".bwsl")
            if name in {"texture_query_size_levels", "pointer_address_taken_control_flow", "texture_sample_grad_cmp_gather"}:
                source = ROOT / "tests/unsorted" / (name + ".bwsl")
            result = command([compiler, source, mode, "-bindings", "-validation", "strict", "-o", directory])
            row = {"source": name + mode, "steps": [result], "errors": []}
            if result["exit"]: row["errors"].append("GLES compilation failed")
            shaders = [p for p in directory.iterdir() if p.suffix in {".vert", ".frag"}]
            if {p.suffix for p in shaders} != {".vert", ".frag"}:
                row["errors"].append("Missing GLES vertex or fragment artifact")
            text = "\n".join(p.read_text() for p in shaders)
            for shader in shaders:
                if shutil.which("glslangValidator"):
                    validation = command(["glslangValidator", shader])
                    row["steps"].append(validation)
                    if validation["exit"]: row["errors"].append("GLES validation failed")
            for binding_file in directory.glob("*.bindings.json"):
                for resource in json.loads(binding_file.read_text())["resources"]:
                    for pair in resource.get("combinedSamplerUniforms", []):
                        if pair["name"] not in text:
                            row["errors"].append("Reflected sampler pair uniform is missing: " + pair["name"])
                    gl_name = resource.get("glName")
                    if gl_name and not re.search(r"\b" + re.escape(gl_name) + r"\b", text):
                        row["errors"].append("Reflected glName is not declared: " + gl_name)
            # ES 3.00 links varyings by name, so every fragment input must be a
            # vertex output, even when the stages come from different emitters.
            def interface_names(path: Path, qualifier: str) -> set[str]:
                pattern = r"\b" + qualifier + r"\s+(?:\w+\s+)*?(v_\w+|varying\d+)\s*;"
                return set(re.findall(pattern, path.read_text())) if path.exists() else set()
            for shader in [p for p in shaders if p.suffix == ".frag"]:
                missing = interface_names(shader, "in") - interface_names(shader.with_suffix(".vert"), "out")
                if missing:
                    row["errors"].append("Fragment inputs not written by the vertex stage: " + ", ".join(sorted(missing)))
            if name == "gl_interface_names":
                for expected in ("ub_render", "bwsl_ub_render", "ub_input", "t_texture",
                                 "a_position", "a_sample", "v_output", "v_tint"):
                    if not re.search(r"\b" + expected + r"\b", text):
                        row["errors"].append("Missing GL interface name: " + expected)
            if name == "texture3d_drops_z" and "sampler3D" not in text:
                row["errors"].append("GLES lost the 3D texture type")
            if name in {"texture_slot_32", "resource_slot_32_mixed"} and mode == "-gles-direct" and "t_tex32" not in text:
                row["errors"].append("Direct GLES selected the wrong compacted resource name")
            good = not row["errors"]
            passed += int(good)
            failed += int(not good)
            print(f"[{'PASS' if good else 'FAIL'}] resource_p1/{name}{mode}")
            if not good:
                for error in row["errors"]: print("  " + error)
                for step in row["steps"]:
                    if step["exit"]: print(step["output"])
            records.append(row)

    too_many = out / "too_many_resources.bwsl"
    resources = "\n".join(f"buf{i}: buffer<float>" for i in range(33))
    uses = ", ".join(f"buf{i}" for i in range(33))
    expression = " + ".join(f"resources.buf{i}[0]" for i in range(32))
    too_many.write_text(f'pipeline Limit {{ resources {{ {resources} }} pass "Main" {{ use resources {{ {uses} }} compute "Main" [1,1,1] {{ resources.buf32[0] = {expression}; }} }} }}')
    limit_result = command([compiler, too_many, "-check", "-validation", "strict"])
    limit_ok = limit_result["exit"] != 0 and "supported limit of 32 resources" in limit_result["output"]
    passed += int(limit_ok)
    failed += int(not limit_ok)
    print(f"[{'PASS' if limit_ok else 'FAIL'}] resource_p1/resource_capacity_diagnostic")
    records.append({"source": "too_many_resources", "steps": [limit_result]})

    if sys.platform == "darwin" and shutil.which("xcrun"):
        executable = out / "sampler_runtime"
        built = command(["xcrun", "clang++", "-std=c++20", "-fobjc-arc", ROOT / "tests/resource_p1/sampler_runtime.mm",
                         "-framework", "Foundation", "-framework", "Metal", "-o", executable])
        gpu = built if built["exit"] else command([executable, out / "sampler_gpu/sampler_gpu.vert.metal", out / "sampler_gpu/sampler_gpu.frag.metal"])
        if gpu["exit"] != 77:
            passed += int(gpu["exit"] == 0)
            failed += int(gpu["exit"] != 0)
        print(f"[{'PASS' if gpu['exit'] == 0 else 'SKIP' if gpu['exit'] == 77 else 'FAIL'}] resource_p1/metal_sampler_runtime: {gpu['output'].strip()}")
        records.append({"source": "metal_sampler_runtime", "steps": [built, gpu]})
    (out / "results.json").write_text(json.dumps(records, indent=2))
    return passed, failed


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, default=ROOT / "build/bwslc")
    parser.add_argument("--output", type=Path, default=ROOT / "build/resource_p1")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    passed, failed = run_resource_p1_suite(args.compiler, args.output, args.verbose)
    print(f"Resource P1: {passed} passed, {failed} failed")
    raise SystemExit(1 if failed else 0)
