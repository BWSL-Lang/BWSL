#!/usr/bin/env python3
"""Check WGSL uniform ABI, texture overloads and real uniform-buffer readback."""

import argparse
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]

UNIFORMS = '''pipeline Uniforms {
    struct Inner { float gain; float2 bias; };
    struct Params { float scale; float[3] weights; Inner inner; mat3 frame; };
    resources { params: Params result: buffer<float> }
    pass "Run" { use resources { params, result }
        compute "Main" [64, 1, 1] {
            uint id = input.global_id.x;
            float weight = resources.params.weights[id % 3u];
            float3 value = resources.params.frame * float3(weight, resources.params.inner.gain, resources.params.scale);
            resources.result[id] = value.x + value.y + value.z + resources.params.inner.bias.x;
        }
    }
}'''

ARRAY_MATRIX = '''pipeline UniformArrayMatrix {
    resources { weights: float[3] transform: mat2 result: buffer<float2> }
    pass "Run" { use resources { weights, transform, result }
        compute "Main" [64, 1, 1] {
            uint id = input.global_id.x;
            float value = resources.weights[id % 3u];
            resources.result[id] = resources.transform * float2(value, 2.0);
        }
    }
}'''


def words(values):
    return [struct.unpack("<I", struct.pack("<f", value))[0] for value in values]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=ROOT / "build/bwslc")
    parser.add_argument("--oracle", type=Path, default=ROOT / "build/wgsl-oracle/release/wgsl-oracle")
    parser.add_argument("--tint", type=Path, default=ROOT / "build/wgsl-tools/tint/bwsl-tint-bench")
    parser.add_argument("--output", type=Path, default=ROOT / "build/wgsl-resource-tests")
    parser.add_argument("--skip-gpu", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    sources = {"uniform_layout": UNIFORMS, "uniform_array_matrix": ARRAY_MATRIX}
    # textureGather reads mip level zero and is valid in every shader stage:
    # https://www.w3.org/TR/WGSL/#texturegather
    for stage, body in (
        ("vertex", "vertex { output.position = gather(resources.tex, float2(0.5), 0); }"),
        ("compute", '''compute "Main" [1, 1, 1] {
            resources.result[0] = gather(resources.tex, float2(0.5), 0);
        }'''),
    ):
        sources[f"texture_gather_{stage}"] = '''pipeline Gather {
            resources { tex: texture2D result: buffer<float4> }
            pass "Main" { use resources { tex, result }
        ''' + body + "}}"
    for name in ("uniform_struct_dynamic_index", "texture_resources_cube_3d_array", "texture_sample_grad_cmp_gather", "texture_query_size_levels"):
        sources[name] = (ROOT / "tests/unsorted" / f"{name}.bwsl").read_text()
    records = []
    for name, source in sources.items():
        directory = args.output / name
        directory.mkdir(exist_ok=True)
        path = directory / "source.bwsl"
        path.write_text(source)
        result = subprocess.run([str(args.compiler), str(path), "-wgsl", "-bindings", "-o", str(directory)],
                                capture_output=True, text=True, timeout=30)
        checks = {"compile": result.returncode == 0}
        (directory / "compile.log").write_text(result.stdout + result.stderr)
        if checks["compile"]:
            for shader in directory.glob("*.wgsl"):
                for tool, executable in (("naga", args.oracle), ("tint", args.tint)):
                    validated = subprocess.run([str(executable), "bench", str(shader), "validate", "5", "1"],
                                               capture_output=True, text=True, timeout=30)
                    checks[f"{shader.name}/{tool}"] = validated.returncode == 0
                    (directory / f"{shader.name}.{tool}.log").write_text(validated.stdout + validated.stderr)
            if name == "uniform_layout":
                reflection = json.loads((directory / "source.comp.wgsl.bindings.json").read_text())
                uniform = next(item for item in reflection["resources"] if item["type"] == "uniform_buffer")
                fields = {item["name"]: item for item in uniform["members"]}
                checks["std140_offsets"] = all(fields[key]["offset"] == offset for key, offset in
                                                 {"scale": 0, "weights": 16, "inner": 64, "frame": 80, "gain": 0, "bias": 8}.items())
                checks["std140_stride_and_size"] = fields["weights"]["stride"] == 16 and uniform["byteSize"] == 128
                data = [0.0] * 32
                data[0] = 3
                data[4], data[8], data[12] = 1, 0.5, 0.25
                data[16], data[18], data[19] = 2, 0.25, 0.5
                for index in (20, 25, 30):
                    data[index] = 1
                # Non-diagonal columns distinguish std140 matrix stride and
                # column-major reconstruction from an accidental transpose.
                data[21], data[26], data[28] = 0.5, 0.25, 0.125
                buffers = [{"group": 1, "binding": 0, "uniform": True, "data": words(data), "expected": words(data)},
                           {"group": 0, "binding": 1, "data": [0xDEADBEEF] * 64,
                            "expected": words([6.125 + 1.5 * [1, 0.5, 0.25][index % 3] for index in range(64)])}]
            elif name == "uniform_array_matrix":
                weights = words([1, 0, 0, 0, 0.5, 0, 0, 0, 0.25, 0, 0, 0])
                matrix = words([2, 1, 0, 0, -1, 3, 0, 0])
                buffers = [{"group": 1, "binding": 0, "uniform": True, "data": weights, "expected": weights},
                           {"group": 1, "binding": 1, "uniform": True, "data": matrix, "expected": matrix},
                           {"group": 0, "binding": 2, "data": [0xDEADBEEF] * 128,
                            "expected": words([component for index in range(64) for component in
                                               (2 * [1, 0.5, 0.25][index % 3] - 2, [1, 0.5, 0.25][index % 3] + 6)])}]
            else:
                buffers = None
            if buffers and not args.skip_gpu and all(checks.values()):
                case = directory / "case.json"
                case.write_text(json.dumps({"workgroups": [1, 1, 1], "buffers": buffers}))
                executed = subprocess.run([str(args.oracle), "run", str(directory / "source.comp.wgsl"), str(case)],
                                          capture_output=True, text=True, timeout=60)
                checks["gpu"] = executed.returncode == 0 and json.loads(executed.stdout).get("passed", False)
                (directory / "gpu.log").write_text(executed.stdout + executed.stderr)
        passed = all(checks.values())
        print(f"[{'PASS' if passed else 'FAIL'}] {name}: {checks}")
        if not checks["compile"]:
            print(result.stdout + result.stderr)
        records.append({"name": name, "passed": passed, "checks": checks})
    (args.output / "results.json").write_text(json.dumps(records, indent=2) + "\n")
    return int(not all(record["passed"] for record in records))


if __name__ == "__main__":
    raise SystemExit(main())
