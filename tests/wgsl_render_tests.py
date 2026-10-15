#!/usr/bin/env python3
"""Native graphics emission, reflected linkage and independent WGSL validation."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from benchmark_wgsl_render import compile_pipeline, prepare

ACCEPTED = {
    "derivative_family": '''pipeline Derivatives {
        pass "Draw" {
            vertex { output.position = float4(0.0, 0.0, 0.0, 1.0); }
            fragment {
                float x = input.position.x;
                float y = ddx(x) + ddy(x) + ddx_fine(x) + ddy_fine(x);
                y += ddx_coarse(x) + ddy_coarse(x);
                y += fwidth(x) + fwidth_fine(x) + fwidth_coarse(x);
                output.color = float4(y, y, y, 1.0);
            }
        }
    }''',
    "unconditional_discard": '''pipeline Discard {
        pass "Draw" {
            vertex { output.position = float4(0.0, 0.0, 0.0, 1.0); }
            fragment { discard; }
        }
    }''',
}

REJECTED = {
    "vertex_storage_write": '''pipeline Reject {
        resources { values: buffer<float4> }
        pass "Draw" { use resources { values }
            vertex { output.position = resources.values[0]; resources.values[0] = output.position; }
        }
    }''',
    "vertex_derivative": '''pipeline Reject {
        attributes { position: float4 }
        pass "Draw" { use attributes { position }
            vertex { output.position = ddx(attributes.position); }
        }
    }''',
    "fragment_output_read_before_store": '''pipeline Reject {
        pass "Draw" {
            vertex { output.position = float4(0.0, 0.0, 0.0, 1.0); }
            fragment { float4 value = output.color; output.color = value; }
        }
    }''',
}


def main():
    suffix = ".exe" if sys.platform == "win32" else ""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=ROOT / f"build/bwslc{suffix}")
    parser.add_argument("--naga", type=Path, default=ROOT / f"build/wgsl-oracle/release/wgsl-oracle{suffix}")
    parser.add_argument("--tint", type=Path, default=ROOT / f"build/wgsl-tools/tint/bwsl-tint-bench{suffix}")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    parser.add_argument("--output", type=Path, default=ROOT / "build/wgsl-render-tests" / stamp)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    def check(name, condition):
        records.append({"name": name, "passed": bool(condition)})
        print(f"[{'PASS' if condition else 'FAIL'}] {name}")
    artifact_dir = args.output / "pipelines"
    if artifact_dir.exists():
        parser.error(f"Use a new output directory; {artifact_dir} already exists")
    manifest = prepare(args.compiler.resolve(), artifact_dir, ["skinned_ggx", "procedural_mrt", "particles", "textured_uniforms"],
                       {"naga": args.naga.resolve(), "tint": args.tint.resolve()}, 3)
    for case in manifest["cases"]:
        check(f"{case['name']}/both_validators", len(case["reference_validation"]) == 2)
        check(f"{case['name']}/no_spirv", not list((artifact_dir / case["name"]).glob("*.spv")))
    interface = manifest["cases"][0]["variants"]["bwsl"]
    vertex = interface["vertex"]["interface"]
    fragment = interface["fragment"]["interface"]
    check("reflection/stages", vertex["stage"] == "vertex" and fragment["stage"] == "fragment")
    check("reflection/workgroups_not_graphics", vertex["workgroupSize"] is None and fragment["workgroupSize"] is None)
    check("reflection/sparse_fragment_input", {item["location"] for item in fragment["inputs"] if item["location"] is not None} == {1, 2})
    check("reflection/integer_flat", any(item["type"] == "UINT" and item["interpolation"] == "flat" for item in fragment["inputs"]))
    check("reflection/linear", any(item["location"] == 1 and item["interpolation"] == "linear" for item in fragment["inputs"]))
    check("reflection/sparse_mrt", {item["location"] for item in fragment["outputs"] if item["location"] is not None} == {0, 2})
    check("reflection/fragment_depth", any(item["builtin"] == "frag_depth" and item["type"] == "FLOAT" for item in fragment["outputs"]))
    check_dir = args.output / "check_only"
    check_dir.mkdir()
    checked = subprocess.run([str(args.compiler.resolve()), str(ROOT / "tests/wgsl_render/interfaces.bwsl"),
                              "-wgsl", "-check", "-o", str(check_dir)], capture_output=True, text=True, cwd=ROOT)
    check("check_only/both_stages", checked.returncode == 0 and not any(check_dir.iterdir()))
    for stage, suffix in (("vertex", "vert"), ("fragment", "frag")):
        stage_dir = args.output / f"stage_{stage}"
        stage_dir.mkdir()
        selected = subprocess.run([str(args.compiler.resolve()), str(ROOT / "tests/wgsl_render/interfaces.bwsl"),
                                   "-wgsl", "-bindings", "-stage", stage, "-o", str(stage_dir)],
                                  capture_output=True, text=True, cwd=ROOT)
        artifacts = list(stage_dir.glob("*.wgsl"))
        check(f"stage_filter/{stage}", selected.returncode == 0 and len(artifacts) == 1 and artifacts[0].name.endswith(f".{suffix}.wgsl"))
        if stage == "fragment" and selected.returncode == 0:
            reflected = json.loads(Path(str(artifacts[0]) + ".bindings.json").read_text())
            check("stage_filter/fragment_retains_linkage", reflected["inputs"] == fragment["inputs"])
    for name, source in ACCEPTED.items():
        directory = args.output / name
        directory.mkdir()
        path = directory / "source.bwsl"
        path.write_text(source)
        try:
            compile_pipeline(args.compiler.resolve(), path, directory)
            accepted = True
        except ValueError as error:
            print(error)
            accepted = False
        check(f"accept/{name}", accepted)
        for tool_name, executable in (("naga", args.naga), ("tint", args.tint)):
            validated = accepted
            for shader in directory.glob("*.wgsl"):
                result = subprocess.run([str(executable.resolve()), "bench", str(shader), "validate", "1", "0"],
                                        capture_output=True, text=True, cwd=ROOT)
                validated = validated and result.returncode == 0
                if result.returncode:
                    print(result.stdout + result.stderr)
            check(f"accept/{name}/{tool_name}", validated)
    for name, source in REJECTED.items():
        directory = args.output / name
        directory.mkdir()
        path = directory / "source.bwsl"
        path.write_text(source)
        result = subprocess.run([str(args.compiler.resolve()), str(path), "-wgsl", "-errors-json", "-o", str(directory)],
                                capture_output=True, text=True, cwd=ROOT)
        check(f"reject/{name}", result.returncode != 0 and "wgsl" in (result.stdout + result.stderr).lower())
        try:
            json.loads(result.stdout)
            valid_json = True
        except ValueError:
            valid_json = False
        check(f"reject/{name}/structured_diagnostic", valid_json)
    (args.output / "results.json").write_text(json.dumps(records, indent=2) + "\n")
    passed = sum(record["passed"] for record in records)
    print(f"WGSL graphics: {passed} passed, {len(records) - passed} failed")
    return int(passed != len(records))


if __name__ == "__main__":
    raise SystemExit(main())
