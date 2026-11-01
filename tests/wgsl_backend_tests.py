#!/usr/bin/env python3
"""WGSL compiler regression tests, independent validation and GPU readback."""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / "tests/wgsl"


def float_words(values: list[float]) -> list[int]:
    return [struct.unpack("<I", struct.pack("<f", value))[0] for value in values]


def runtime_cases() -> dict[str, dict]:
    source = [(index * 7919) & 0xFFFFFFFF for index in range(128)]
    result = [((value + 17) * 3) & 0xFFFFFFFF for value in source]
    result = [value ^ (value >> 4) for value in result]
    vectors = [(index - 128) * 0.25 for index in range(256)]
    transformed = [
        vectors[index + 3 - component] * 2.0 + component + 1
        for index in range(0, len(vectors), 4)
        for component in range(4)
    ]
    floats = [(index - 32) * 0.25 for index in range(64)]
    words = float_words(floats)
    signed = [value if value < 0x80000000 else value - 0x100000000 for value in words]
    tie_values = [-2.5, -1.5, 1.5, 2.5]
    signed_values = [-7, -4, 4, 7]
    float_expected = ([float(round(value)) for value in tie_values]
                      + [value - 3.0 * math.floor(value / 3.0) for value in tie_values]
                      + [math.fmod(value, 3.0) for value in tie_values])
    integer_expected = [result for value in signed_values
                        for result in (math.trunc(value / 3),
                                       value - 3 * math.trunc(value / 3), value >> 2)]
    vector_integer_expected = ([math.trunc(value / 3) for value in signed_values]
                               + [value - 3 * math.trunc(value / 3) for value in signed_values]
                               + [value >> 2 for value in signed_values]
                               + [(value & 0xFFFFFFFF) >> 2 for value in signed_values]
                               + [-value for value in signed_values])
    reduction_vectors = [
        [-4.0, -3.0, -2.0, -1.0], [1.0, 2.0, 3.0, 4.0],
        [0.0, 0.0, 0.0, 0.0], [-2.0, 0.0, 2.0, 3.0],
        [2.0, 2.0, 3.0, 4.0], [0.0, -1.0, 0.0, -3.0],
        [1.0, 1.0, 1.0, 1.0], [-1.0, -1.0, -1.0, -1.0],
    ] * 8
    reduction_source = float_words([value for vector in reduction_vectors for value in vector])
    reduction_expected = [
        int(any(value < 0 for value in vector))
        + 2 * int(all(value < 0 for value in vector))
        + 4 * int(any(value > 1 for value in vector))
        + 8 * int(all(value > 1 for value in vector))
        for vector in reduction_vectors
    ]
    # Bit indices are -1 when no bit qualifies; a negative int's highest bit
    # is the highest one that differs from its sign bit.
    bit_values = [0, 1, -1, -2, 8, -8, 6, 12, 0x40000000, -0x80000000, 0x7FFFFFFF, -0x40000000,
                  0x12345678, -0x12345678, 255, -256]
    unsigned_bits = [(bit_values[index % 16] + (index // 16) * (index % 3)) & 0xFFFFFFFF for index in range(64)]
    bit_values = [bits - (bits >> 31 << 32) for bits in unsigned_bits]
    # Naga 27's Metal lowering of firstLeadingBit(0xFFFFFFFFu) returns -1, so
    # the unsigned form reads the value with bit 0 cleared.
    bit_expected = [result for value, bits in zip(bit_values, unsigned_bits)
                    for result in (bin(bits).count("1"),
                                   (~value if value < 0 else value).bit_length() - 1,
                                   (bits & -bits).bit_length() - 1,
                                   (bits & ~1).bit_length() - 1,
                                   (bits & -bits).bit_length() - 1)]
    return {
        "bit_intrinsics": {
            "workgroups": [1, 1, 1],
            "buffers": [
                {"binding": 0, "data": unsigned_bits, "expected": unsigned_bits},
                {"binding": 1, "data": [0] * 320,
                 "expected": [value & 0xFFFFFFFF for value in bit_expected]},
            ],
        },
        "scalar_reassignment": {
            "workgroups": [2, 1, 1],
            "buffers": [
                {"binding": 0, "data": source, "expected": source},
                {"binding": 1, "data": [0] * 128, "expected": result},
            ],
        },
        "vector_swizzle": {
            "workgroups": [1, 1, 1],
            "buffers": [
                {"binding": 0, "data": float_words(vectors), "expected": float_words(vectors)},
                {"binding": 1, "data": [0] * 256, "expected": float_words(transformed)},
            ],
        },
        "compute_builtins": {
            "workgroups": [3, 2, 1],
            "buffers": [{
                "binding": 0,
                "data": [0] * 48,
                "expected": [
                    x % 4 + (y % 2) * 10 + (x // 4) * 100 + (y // 2) * 1000 + 230000
                    for y in range(4)
                    for x in range(12)
                ],
            }],
        },
        "bitcast_signed": {
            "workgroups": [1, 1, 1],
            "buffers": [
                {"binding": 0, "data": words, "expected": words},
                {"binding": 1, "data": [0] * 64,
                 "expected": [((value >> 8) ^ index) & 0xFFFFFFFF for index, value in enumerate(signed)]},
            ],
        },
        "storage_order": {
            "workgroups": [1, 1, 1],
            "warmup": 0,
            "buffers": [{"binding": 0, "data": list(range(64)),
                         "expected": [index * 3 for index in range(64)]}],
        },
        "load_snapshot": {
            "workgroups": [1, 1, 1],
            "warmup": 0,
            "buffers": [{"binding": 0, "data": list(range(64)),
                         "expected": [(((index * 3) + 7) ^ 11) + 19 for index in range(64)]}],
        },
        "sparse_binding": {
            "workgroups": [1, 1, 1],
            "buffers": [{"binding": 5, "data": [0] * 64,
                         "expected": [index * 7 + 1 for index in range(64)]}],
        },
        "vector_select": {
            "workgroups": [1, 1, 1],
            "buffers": [
                {"binding": 0, "data": float_words(vectors), "expected": float_words(vectors)},
                {"binding": 1, "data": [0] * 256,
                 "expected": float_words([abs(value) for value in vectors])},
            ],
        },
        "math_semantics": {
            "workgroups": [1, 1, 1],
            "buffers": [
                {"binding": 0, "data": float_words(tie_values * 64),
                 "expected": float_words(tie_values * 64)},
                {"binding": 1, "data": [value & 0xFFFFFFFF for value in signed_values] * 16,
                 "expected": [value & 0xFFFFFFFF for value in signed_values] * 16},
                {"binding": 2, "data": [0] * 768, "expected": float_words(float_expected * 64)},
                {"binding": 3, "data": [0] * 192,
                 "expected": [value & 0xFFFFFFFF for value in integer_expected] * 16},
            ],
        },
        "vector_signed_math": {
            "workgroups": [1, 1, 1],
            "buffers": [
                {"binding": 0, "data": [value & 0xFFFFFFFF for value in signed_values] * 64,
                 "expected": [value & 0xFFFFFFFF for value in signed_values] * 64},
                {"binding": 1, "data": [0] * 1280,
                 "expected": [value & 0xFFFFFFFF for value in vector_integer_expected] * 64},
            ],
        },
        "bool_reduction_reassignment": {
            "workgroups": [1, 1, 1],
            "buffers": [
                {"binding": 0, "data": reduction_source, "expected": reduction_source},
                {"binding": 1, "data": [0] * 64, "expected": reduction_expected},
            ],
        },
    }


REJECTED = {
    "vertex_without_position": """
pipeline MissingVertexPosition {
    pass "Draw" {
        vertex { output.value = float4(0.0, 0.0, 0.0, 1.0); }
    }
}
""",
    "unsupported_uniform_type": """
pipeline UnsupportedUniform {
    resources { scale: double result: buffer<double> }
    pass "Compute" {
        use resources { scale, result }
        compute "Main" [1, 1, 1] {
            resources.result[0] = resources.scale;
        }
    }
}
""",
}


def run_suite(args: argparse.Namespace) -> tuple[int, int, int]:
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    passed = failed = skipped = 0

    def command(argv: list, timeout: int = 120) -> dict:
        result = subprocess.run(
            [str(item) for item in argv], cwd=ROOT, capture_output=True,
            text=True, timeout=timeout,
        )
        return {"command": [str(item) for item in argv], "exit": result.returncode,
                "stdout": result.stdout, "stderr": result.stderr}

    def record(name: str, steps: list[dict], errors: list[str], skip: bool = False) -> None:
        nonlocal passed, failed, skipped
        if errors:
            failed += 1
            label = "FAIL"
        elif skip:
            skipped += 1
            label = "SKIP"
        else:
            passed += 1
            label = "PASS"
        print(f"[{label}] wgsl/{name}")
        for error in errors:
            print(f"  {error}")
        if errors or args.verbose:
            for step in steps:
                if step["exit"] or args.verbose:
                    print(step["stdout"] + step["stderr"])
        records.append({"name": name, "status": label, "errors": errors, "steps": steps})

    if not args.oracle.is_file():
        record("oracle_available", [], [f"Missing {args.oracle}; build tools/wgsl-oracle first"])
        return passed, failed, skipped
    if not args.compiler.is_file():
        record("compiler_available", [], [f"Missing {args.compiler}"])
        return passed, failed, skipped

    for name, case in runtime_cases().items():
        directory = args.output / name
        directory.mkdir(parents=True, exist_ok=True)
        generated = directory / f"{name}.comp.wgsl"
        reflection = directory / f"{name}.comp.wgsl.bindings.json"
        for stale in directory.glob("*.wgsl*"):
            stale.unlink()
        compilation = command([args.compiler, FIXTURES / f"{name}.bwsl", "-wgsl", "-bindings", "-o", directory])
        errors = []
        if compilation["exit"]:
            errors.append("BWSL WGSL compilation failed")
        if not generated.is_file():
            errors.append("Missing WGSL artifact")
        if not reflection.is_file():
            errors.append("Missing WGSL binding reflection")
        else:
            metadata = json.loads(reflection.read_text())
            resources = metadata.get("resources", [])
            if {resource["binding"] for resource in resources} != {buffer["binding"] for buffer in case["buffers"]}:
                errors.append("Reflection bindings do not match actual host buffer bindings")
            if metadata.get("entryPoint") != "main":
                errors.append("Reflection entryPoint must name the emitted function")
            expected_workgroup = [4, 2, 1] if name == "compute_builtins" else [64, 1, 1]
            if metadata.get("workgroupSize") != expected_workgroup:
                errors.append("Reflection lost the declared workgroup dimensions")
            for resource in resources:
                if resource.get("group") != 0:
                    errors.append("Stage 1 storage buffer must be in group 0")
                expected_access = "read_write" if resource["name"].startswith("result") else "read"
                if resource.get("access") != expected_access:
                    errors.append(f"Wrong reflected storage access for {resource['name']}")
                vector_buffer = name in {"vector_swizzle", "vector_select", "vector_signed_math"}
                vector_buffer |= name == "math_semantics" and "floats" in resource["name"]
                vector_buffer |= name == "bool_reduction_reassignment" and resource["name"] == "source"
                expected_stride = 16 if vector_buffer else 4
                if resource.get("stride") != expected_stride:
                    errors.append("Wrong reflected host-visible array stride")
        record(f"{name}/compile_and_reflect", [compilation], errors)
        if errors:
            continue

        case_file = directory / "case.json"
        case_file.write_text(json.dumps(case, indent=2) + "\n")
        roundtrip_spv = directory / "roundtrip.spv"
        roundtrip = directory / "roundtrip.wgsl"
        translation = command([args.oracle, "translate", generated, roundtrip_spv])
        translations = [translation]
        if translation["exit"] == 0:
            translations.append(command([args.oracle, "translate", roundtrip_spv, roundtrip]))
        roundtrip_failed = any(step["exit"] for step in translations)
        # Naga 27.0.3 cannot revalidate its own SPIR-V safe signed vector
        # division helper: its frontend builds a logical-and on vec4<bool>.
        # Keep direct WGSL validation and GPU coverage; exempt only this exact
        # external roundtrip failure, not arbitrary validator failures.
        known_naga_failure = (
            name == "vector_signed_math" and len(translations) == 2
            and translations[0]["exit"] == 0 and translations[1]["exit"] != 0
            and "IncompatibleOperands" in translations[1]["stderr"]
            and "LogicalAnd(vec4<bool>, _)" in translations[1]["stderr"]
        )
        roundtrip_name = "naga_roundtrip_known_vector_division_bug" if known_naga_failure else "naga_roundtrip"
        record(f"{name}/{roundtrip_name}", translations,
               ["Naga WGSL/SPIR-V roundtrip failed"] if roundtrip_failed and not known_naga_failure else [],
               known_naga_failure)
        variants = {"generated": generated, "handwritten": FIXTURES / f"{name}.wgsl"}
        if roundtrip.is_file():
            variants["naga_roundtrip"] = roundtrip
        for variant, shader in variants.items():
            validation = command([args.oracle, "validate", shader])
            record(f"{name}/{variant}/naga_validate", [validation],
                   ["Naga validation failed"] if validation["exit"] else [])
            if args.tint:
                tint = command([args.tint, shader, "--format", "wgsl", "-o", directory / f"{variant}.tint.wgsl"])
                record(f"{name}/{variant}/tint_validate", [tint],
                       ["Tint validation failed"] if tint["exit"] else [])
            if not args.skip_gpu:
                execution = command([args.oracle, "run", shader, case_file])
                unavailable = execution["exit"] == 77
                errors = []
                if execution["exit"] and (not unavailable or args.require_gpu):
                    errors.append("GPU readback failed" if not unavailable else "GPU adapter required but unavailable")
                record(f"{name}/{variant}/gpu_readback", [execution], errors, unavailable and not args.require_gpu)

        # Backend selection must also validate in check-only mode without writing artifacts.
        check_dir = directory / "check_only"
        check_dir.mkdir(exist_ok=True)
        check = command([args.compiler, FIXTURES / f"{name}.bwsl", "-wgsl", "-check", "-o", check_dir])
        errors = ["Check-only compilation failed"] if check["exit"] else []
        if any(check_dir.iterdir()):
            errors.append("Check-only compilation wrote an artifact")
        record(f"{name}/check_only", [check], errors)

    # Both targets consume the SSA program. Alias-marker cleanup must leave
    # valid SPIR-V definitions while WGSL-only builds avoid generating SPIR-V.
    for mixed in (False, True):
        mode = "mixed_targets" if mixed else "wgsl_only_internals"
        directory = args.output / mode
        directory.mkdir(parents=True, exist_ok=True)
        for stale in directory.glob("scalar_reassignment.*"):
            stale.unlink()
        argv = [args.compiler, FIXTURES / "scalar_reassignment.bwsl", "-wgsl",
                "-internals", "-bindings", "-o", directory]
        if mixed:
            argv.extend(["-spv", "-validation", "strict"])
        compilation = command(argv)
        steps = [compilation]
        errors = ["CLI target combination failed"] if compilation["exit"] else []
        shader = directory / "scalar_reassignment.comp.wgsl"
        internals = directory / "scalar_reassignment.comp.internals.json"
        spirv = directory / "scalar_reassignment.comp.spv"
        if not shader.is_file() or not (directory / "scalar_reassignment.comp.wgsl.bindings.json").is_file():
            errors.append("Missing WGSL artifact or bindings")
        if not internals.is_file():
            errors.append("Missing internals document")
        else:
            document = json.loads(internals.read_text())
            if not document.get("ir"):
                errors.append("Internals document has no IR")
            if not mixed:
                if document.get("wgsl") != (shader.read_text() if shader.is_file() else None):
                    errors.append("WGSL-only internals do not contain the emitted WGSL")
                if any("spirv" in key.lower() for key in document):
                    errors.append("WGSL-only internals contain a SPIR-V field")
        translated = directory / "from_spirv.wgsl"
        if mixed:
            if not spirv.is_file():
                errors.append("Mixed-target compilation omitted SPIR-V")
            elif not errors:
                translation = command([args.oracle, "translate", spirv, translated])
                steps.append(translation)
                if translation["exit"]:
                    errors.append("Naga rejected mixed-target SPIR-V")
        elif list(directory.glob("*.spv")):
            errors.append("WGSL-only compilation produced a SPIR-V artifact")
        record(mode, steps, errors)
        if mixed and not errors and not args.skip_gpu:
            case = runtime_cases()["scalar_reassignment"]
            case["bind_group_index"] = 1
            case_file = directory / "case.json"
            case_file.write_text(json.dumps(case, indent=2) + "\n")
            execution = command([args.oracle, "run", translated, case_file])
            unavailable = execution["exit"] == 77
            errors = []
            if execution["exit"] and (not unavailable or args.require_gpu):
                errors.append("Mixed-target SPIR-V GPU readback failed")
            record("mixed_targets/spirv_gpu_readback", [execution], errors,
                   unavailable and not args.require_gpu)

    for name, source in REJECTED.items():
        directory = args.output / f"reject_{name}"
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / f"{name}.bwsl"
        path.write_text(source)
        rejected = command([args.compiler, path, "-wgsl", "-check", "-errors-json", "-o", directory])
        errors = []
        if rejected["exit"] == 0:
            errors.append("Stage 1 accepted an unsupported shader")
        if "wgsl" not in (rejected["stdout"] + rejected["stderr"]).lower():
            errors.append("Missing WGSL-specific unsupported-feature diagnostic")
        try:
            json.loads(rejected["stdout"])
        except json.JSONDecodeError:
            errors.append("-errors-json did not produce a JSON document")
        if list(directory.glob("*.wgsl")) or list(directory.glob("*.spv")):
            errors.append("Rejected shader produced a backend artifact")
        record(f"reject_{name}", [rejected], errors)

    if not args.tint:
        record("tint_available", [], ["Tint is required but was not found"] if args.require_tint else [], not args.require_tint)
    if args.skip_gpu:
        record("gpu_requested_skip", [], [], True)
    (args.output / "results.json").write_text(json.dumps(records, indent=2) + "\n")
    return passed, failed, skipped


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=ROOT / "build/bwslc")
    executable = "wgsl-oracle.exe" if sys.platform == "win32" else "wgsl-oracle"
    parser.add_argument("--oracle", type=Path, default=ROOT / "build/wgsl-oracle/release" / executable)
    parser.add_argument("--output", type=Path, default=ROOT / "build/wgsl-tests")
    parser.add_argument("--tint", default=os.environ.get("TINT") or shutil.which("tint"))
    parser.add_argument("--require-tint", action="store_true")
    gpu = parser.add_mutually_exclusive_group()
    gpu.add_argument("--skip-gpu", action="store_true")
    gpu.add_argument("--require-gpu", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    args.compiler = args.compiler.resolve()
    args.oracle = args.oracle.resolve()
    args.output = args.output.resolve()
    passed, failed, skipped = run_suite(args)
    print(f"WGSL: {passed} passed, {failed} failed, {skipped} skipped")
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
