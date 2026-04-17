#!/usr/bin/env python3
from __future__ import annotations

import argparse
import difflib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


RED = "\033[0;31m"
GREEN = "\033[0;32m"
YELLOW = "\033[1;33m"
BLUE = "\033[0;34m"
NC = "\033[0m"


INLINE_RETURN_TESTS = {
    "inline_return_jump",
    "inline_return_loop",
    "inline_return_nested_loops",
    "inline_return_nested_loops_skip_break",
    "inline_return_outer_skip_break",
    "inline_return_nested_if",
    "inline_return_range_loop",
    "inline_return_count_loop",
    "inline_return_loop_until",
    "inline_return_enum",
    "inline_return_struct",
}

INLINE_RETURN_JUMP_TESTS = {
    "inline_return_jump",
    "inline_return_nested_if",
    "inline_return_enum",
    "inline_return_struct",
}

INLINE_RETURN_LOOP_TESTS = {
    "inline_return_loop",
    "inline_return_nested_loops",
    "inline_return_nested_loops_skip_break",
    "inline_return_outer_skip_break",
    "inline_return_range_loop",
    "inline_return_count_loop",
    "inline_return_loop_until",
}

VARIANT_REFLECTION_TESTS = {
    "variants_basic": {
        "declared": {
            "skinning": ("bool", "false"),
            "lighting": ("LightingMode", "Forward"),
        },
        "implicit": {"has_normal"},
        "default_selected": {
            "skinning": "false",
            "lighting": "Forward",
            "has_normal": "true",
        },
        "override_selected": {
            "skinning": "true",
            "lighting": "Clustered",
            "has_normal": "true",
        },
        "override_args": ["-variant", "skinning=true", "-variant", "lighting=Clustered"],
        "illegal_args": ["-variant", "skinning=true", "-variant", "lighting=Unlit"],
        "illegal_error": "violates rule: conflict",
        "specialization": {
            "default": {
                "vert_metal_contains": ["float4(0.5, 0.0, 0.0, 1.0)"],
                "vert_metal_not_contains": ["float4(0.25, 0.0, 0.0, 1.0)"],
                "vert_hlsl_contains": ["float4(0.5f, 0.0f, 0.0f, 1.0f)"],
                "vert_hlsl_not_contains": ["float4(0.25f, 0.0f, 0.0f, 1.0f)"],
                "vert_ir_contains": ["Instructions: 3", "[0] = 0.5"],
                "vert_ir_not_contains": ["BRANCH", "[0] = 0.25"],
                "frag_ir_contains": [
                    "[  0] STORE_REG        r0           <- 0.5",
                    "FADD             r1           <- r0, 0.25",
                    "[  3] STORE_REG        r2           <- 0.5",
                ],
                "frag_ir_not_contains": ["BRANCH", "0.75"],
                "frag_spirv_contains": ["OpFAdd %float %float_0_5 %float_0_25"],
                "frag_spirv_not_contains": ["OpBranchConditional", "%float_0_75"],
            },
            "override": {
                "vert_metal_contains": ["float4(0.25, 0.0, 0.0, 1.0)"],
                "vert_metal_not_contains": ["float4(0.5, 0.0, 0.0, 1.0)"],
                "vert_hlsl_contains": ["float4(0.25f, 0.0f, 0.0f, 1.0f)"],
                "vert_hlsl_not_contains": ["float4(0.5f, 0.0f, 0.0f, 1.0f)"],
                "vert_ir_contains": ["Instructions: 3", "[0] = 0.25"],
                "vert_ir_not_contains": ["BRANCH", "[0] = 0.5"],
                "frag_ir_contains": [
                    "[  1] STORE_REG        r0           <- 0.75",
                    "[  4] STORE_REG        r2           <- 1",
                ],
                "frag_ir_not_contains": ["BRANCH", "[  3] STORE_REG        r2           <- 0.5"],
                "frag_spirv_contains": ["OpFAdd %float %float_0_75 %float_0_25"],
                "frag_spirv_not_contains": ["OpBranchConditional", "OpFAdd %float %float_0_5 %float_0_25"],
            },
        },
    },
    "variants_module_enum": {
        "declared": {
            "mode": ("TestVariantEnums::Mode", "A"),
            "enabled": ("bool", "false"),
            "strict": ("bool", "false"),
        },
        "implicit": set(),
        "default_selected": {
            "mode": "A",
            "enabled": "false",
            "strict": "false",
        },
        "override_selected": {
            "mode": "B",
            "enabled": "true",
            "strict": "true",
        },
        "override_args": ["-variant", "mode=B", "-variant", "enabled=true", "-variant", "strict=true"],
        "illegal_args": ["-variant", "strict=true"],
        "illegal_error": "violates rule: require",
    },
}

VARIANT_ERROR_TESTS = {
    "duplicate_names.bwsl": "Variant already declared in this pipeline",
    "unknown_type.bwsl": "Variant type must be 'bool' or an enum type",
    "invalid_default.bwsl": "Variant default does not match declared type",
    "invalid_rule_ref.bwsl": "Variant rule left-hand side must be a compile-time boolean expression",
}

ERROR_CASE_TESTS = {
    "invalid_intrinsic_arity.bwsl": "'sin' accepts at most 1 arguments, got 2",
    "missing_semicolon.bwsl": "Expected ';' after expression",
    "unknown_module_import.bwsl": "Unknown module 'DoesNotExist'",
}

TEXT_GOLDEN_SUFFIXES = {".metal", ".hlsl", ".glsl", ".gles"}

TRANSLATION_EXPECTATION_TESTS = {
    "ssa_complex": {
        "vert": {
            "ir_contains": ["PHI", "BRANCH"],
            "spirv_contains": ["OpPhi"],
            "hlsl_count_at_least": {"for (": 4, "if (": 8, "break;": 2},
            "glsl_count_at_least": {"for (": 4, "if (": 8, "break;": 2},
        },
    },
    "types_matrices": {
        "vert": {
            "ir_contains": ["MAT_CONSTRUCT", "COS", "SIN"],
            "spirv_contains": ["OpMatrixTimesVector", "OpMatrixTimesMatrix"],
            "metal_contains": ["float4x4"],
            "hlsl_contains": ["float4x4", "mul("],
            "glsl_contains": ["mat4("],
            "gles_contains": ["mat4("],
        },
    },
    "bug_mat3_multiply": {
        "frag": {
            "ir_contains": ["MAT_CONSTRUCT", "CROSS"],
            "spirv_contains": ["OpCompositeConstruct %mat3v3float", "OpMatrixTimesVector"],
            "metal_contains": ["float3x3(", "cross("],
            "hlsl_contains": ["float3x3(", "cross(", "mul("],
            "glsl_contains": ["mat3(", "cross("],
            "gles_contains": ["mat3(", "cross("],
        },
    },
    "backend_vector_math": {
        "vert": {
            "ir_contains": ["CROSS", "MAT_CONSTRUCT", "PHI"],
            "spirv_contains": ["OpPhi", "OpMatrixTimesVector"],
            "metal_contains": ["float3x3(", "cross("],
            "hlsl_contains": ["float3x3(", "cross("],
            "glsl_contains": ["mat3(", "cross("],
            "gles_contains": ["mat3(", "cross("],
        },
        "frag": {
            "ir_contains": ["FACEFORWARD", "REFLECT", "REFRACT", "MAT_CONSTRUCT"],
            "spirv_contains": ["Reflect", "Refract", "FaceForward", "OpMatrixTimesVector"],
            "metal_contains": ["faceforward(", "reflect(", "refract(", "float3x3("],
            "hlsl_contains": ["faceforward(", "reflect(", "refract(", "float3x3("],
            "glsl_contains": ["faceforward(", "reflect(", "refract(", "mat3("],
            "gles_contains": ["faceforward(", "reflect(", "refract(", "mat3("],
        },
    },
    "intrinsics_math_live": {
        "vert": {
            "ir_contains": ["FABS", "FRACT", "FCLAMP", "SMOOTHSTEP", "LERP", "MOD", "SIGN", "NORMALIZE"],
        },
        "frag": {
            "hlsl_contains": ["smoothstep(", "mod(", "normalize("],
            "glsl_contains": ["smoothstep(", "fract(", "mod(", "normalize("],
            "metal_contains": ["smoothstep(", "mod(", "fast::normalize("],
        },
    },
    "intrinsics_exp_trig_live": {
        "vert": {
            "ir_contains": ["RADIANS", "SIN", "COS", "TAN", "ATAN2", "EXP2", "SQRT", "RSQRT"],
        },
        "frag": {
            "hlsl_contains": ["sin(", "cos(", "tan(", "degrees(", "log2(", "pow("],
            "glsl_contains": ["sin(", "cos(", "tan(", "degrees(", "log2(", "pow("],
            "metal_contains": ["sin(", "cos(", "tan(", "degrees(", "log2("],
        },
    },
    "module_math_live": {
        "vert": {
            "ir_contains": ["RSQRT"],
            "hlsl_contains": ["rsqrt("],
        },
        "frag": {
            "ir_contains": ["RSQRT", "NORMALIZE", "SATURATE"],
            "hlsl_contains": ["rsqrt(", "normalize("],
        },
    },
    "module_color_live": {
        "frag": {
            "ir_contains": ["POW"],
            "hlsl_contains": ["pow(", "clamp("],
            "glsl_contains": ["pow(", "clamp("],
            "metal_contains": ["pow", "clamp("],
        },
    },
    "module_random_live": {
        "vert": {
            "ir_contains": ["XOR", "SHR", "IMUL", "SIN", "COS"],
            "hlsl_contains": ["sin(", "cos("],
        },
        "frag": {
            "ir_contains": ["XOR", "SHR", "IMUL", "NORMALIZE", "SIN", "COS"],
            "hlsl_contains": ["normalize("],
        },
    },
    "modules_pbr_lighting": {
        "frag": {
            "ir_contains": ["NORMALIZE", "LERP", "POW"],
            "hlsl_contains": ["lerp(", "normalize(", "pow("],
            "glsl_contains": ["mix(", "normalize(", "pow("],
            "metal_contains": ["normalize(", "pow", "max("],
        },
    },
}


def run_command(args: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def build_compiler(root: Path) -> bool:
    if os.name == "nt":
        result = run_command(["cmd", "/c", "build.bat", "bwslc"], cwd=root)
    else:
        result = run_command(["make", "bwslc"], cwd=root)
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        return False
    return True


def compiler_path(root: Path) -> Path:
    exe_path = root / "build" / "bwslc.exe"
    if exe_path.exists():
        return exe_path
    return root / "build" / "bwslc"


def has_metal_tooling() -> bool:
    return sys.platform == "darwin" and shutil.which("xcrun") is not None


def is_module_file(path: Path) -> bool:
    pattern = re.compile(r"^(module|pipeline)\b")
    for line in path.read_text(encoding="utf-8").splitlines():
        if pattern.match(line):
            return line.split()[0] == "module"
    return False


def config_args_for_test(script_dir: Path, test_name: str) -> list[str]:
    exact = script_dir / f"{test_name}.rcfg"
    if exact.exists():
        return ["-config", str(exact)]

    prefix = test_name.split("_", 1)[0]
    prefixed = script_dir / f"{prefix}_test.rcfg"
    if prefixed.exists():
        return ["-config", str(prefixed)]

    return []


def check_inline_return_jump(path: Path) -> tuple[bool, str]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)

    ir = data.get("ir", "")
    branch_count = sum(1 for line in ir.splitlines() if " BRANCH " in line)
    if " JUMP " in ir:
        return False, "Unexpected inline return jump in IR"
    if branch_count < 2:
        return False, f"Expected return guard branch, found {branch_count}"
    return True, ""


def check_inline_return_loop(path: Path) -> tuple[bool, str]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)

    ir = data.get("ir", "")
    spirv = data.get("spirv_dis", "")
    has_spirv_disassembly = any(
        marker in spirv for marker in ("OpCapability", "OpMemoryModel", "OpEntryPoint", "OpFunction")
    )

    if has_spirv_disassembly:
        if "OpLogicalAnd" in spirv:
            return True, ""
        return False, "Missing return guard logical AND in SPIR-V"

    and_match = re.search(r"\bAND\s+(r\d+)\s+<-", ir)
    branch_match = re.search(r"\bBRANCH\s+(r\d+)\s+\? ->", ir)
    if and_match and branch_match and and_match.group(1) == branch_match.group(1):
        return True, ""

    return False, "Missing return guard AND->BRANCH pattern in IR fallback"


def find_metal_outputs(output_dir: Path, test_name: str) -> list[Path]:
    files = {path for path in output_dir.glob(f"{test_name}*.metal")}
    exact = output_dir / f"{test_name}.metal"
    if exact.exists():
        files.add(exact)
    return sorted(files)


def run_variant_dump(bwslc: Path, root: Path, test_file: Path, modules_dir: Path,
                     extra_args: list[str] | None = None) -> tuple[bool, dict | None, str]:
    args = [
        str(bwslc),
        str(test_file),
        "-modules",
        str(modules_dir),
        "-dump-variant-space",
    ]
    if extra_args:
        args.extend(extra_args)
    result = run_command(args, cwd=root)
    if result.returncode != 0:
        return False, None, result.stdout.strip() or f"variant dump exited with code {result.returncode}"
    try:
        return True, json.loads(result.stdout), ""
    except json.JSONDecodeError as exc:
        return False, None, f"invalid variant reflection JSON: {exc}"


def check_variant_reflection(data: dict, expected_declared: dict[str, tuple[str, str]],
                             expected_implicit: set[str],
                             expected_selected: dict[str, str]) -> tuple[bool, str]:
    declared = {entry["name"]: entry for entry in data.get("declared", [])}
    implicit = {entry["name"]: entry for entry in data.get("implicit", [])}
    selected = {entry["name"]: entry for entry in data.get("selected", [])}

    for name, (type_name, default_value) in expected_declared.items():
        entry = declared.get(name)
        if entry is None:
            return False, f"missing declared variant '{name}'"
        if entry.get("type") != type_name:
            return False, f"declared variant '{name}' has type '{entry.get('type')}', expected '{type_name}'"
        if entry.get("default") != default_value:
            return False, f"declared variant '{name}' has default '{entry.get('default')}', expected '{default_value}'"

    for name in expected_implicit:
        if name not in implicit:
            return False, f"missing implicit variant '{name}'"

    for name, expected_value in expected_selected.items():
        entry = selected.get(name)
        if entry is None:
            return False, f"missing selected variant '{name}'"
        if entry.get("value") != expected_value:
            return False, f"selected variant '{name}' has value '{entry.get('value')}', expected '{expected_value}'"

    return True, ""


def compile_variant_outputs(bwslc: Path, root: Path, test_file: Path, modules_dir: Path,
                            output_dir: Path, config_args: list[str],
                            extra_args: list[str] | None = None) -> subprocess.CompletedProcess[str]:
    shutil.rmtree(output_dir, ignore_errors=True)
    output_dir.mkdir(parents=True, exist_ok=True)
    args = [
        str(bwslc),
        str(test_file),
        "-o",
        str(output_dir),
        "-modules",
        str(modules_dir),
        *config_args,
        "-all",
        "-internals",
    ]
    if extra_args:
        args.extend(extra_args)
    return run_command(args, cwd=root)


def find_variant_stage_file(output_dir: Path, test_name: str, stage: str, suffix: str) -> tuple[Path | None, str]:
    matches = sorted(output_dir.glob(f"{test_name}_*_{stage}.{suffix}"))
    if not matches:
        return None, f"missing {stage}.{suffix} output"
    if len(matches) > 1:
        names = ", ".join(path.name for path in matches)
        return None, f"expected one {stage}.{suffix} output, found: {names}"
    return matches[0], ""


def check_text_patterns(label: str, text: str,
                        contains: list[str] | None = None,
                        not_contains: list[str] | None = None) -> tuple[bool, str]:
    for pattern in contains or []:
        if pattern not in text:
            return False, f"{label} missing '{pattern}'"
    for pattern in not_contains or []:
        if pattern in text:
            return False, f"{label} unexpectedly contains '{pattern}'"
    return True, ""


def check_pattern_counts(label: str, text: str,
                         count_at_least: dict[str, int] | None = None) -> tuple[bool, str]:
    for pattern, expected in (count_at_least or {}).items():
        actual = text.count(pattern)
        if actual < expected:
            return False, f"{label} contains '{pattern}' {actual} times, expected at least {expected}"
    return True, ""


def find_stage_file(output_dir: Path, test_name: str, stage: str, suffix: str) -> tuple[Path | None, str]:
    matches = sorted(output_dir.glob(f"{test_name}_*_{stage}.{suffix}"))
    if not matches:
        return None, f"missing {stage}.{suffix} output"
    if len(matches) > 1:
        names = ", ".join(path.name for path in matches)
        return None, f"expected one {stage}.{suffix} output, found: {names}"
    return matches[0], ""


def find_text_golden_files(golden_dir: Path, test_name: str) -> list[Path]:
    return sorted(
        path for path in golden_dir.glob(f"{test_name}_*.*")
        if path.suffix in TEXT_GOLDEN_SUFFIXES
    )


def check_translation_expectations(output_dir: Path, test_name: str,
                                   stage_expectations: dict[str, dict]) -> tuple[bool, str]:
    for stage, expectations in stage_expectations.items():
        texts: dict[str, str] = {}

        for key in ("metal", "hlsl", "glsl", "gles"):
            if any(name.startswith(f"{key}_") for name in expectations):
                path, message = find_stage_file(output_dir, test_name, stage, key)
                if path is None:
                    return False, f"{stage}: {message}"
                texts[key] = path.read_text(encoding="utf-8")

        if any(name.startswith("ir_") or name.startswith("spirv_") for name in expectations):
            path, message = find_stage_file(output_dir, test_name, stage, "internals.json")
            if path is None:
                return False, f"{stage}: {message}"
            data = json.loads(path.read_text(encoding="utf-8"))
            texts["ir"] = data.get("ir", "")
            texts["spirv"] = data.get("spirv_dis", "")

        for key, label in (
            ("ir", "IR"),
            ("spirv", "SPIR-V"),
            ("metal", "Metal"),
            ("hlsl", "HLSL"),
            ("glsl", "GLSL"),
            ("gles", "GLES"),
        ):
            if key not in texts:
                continue

            ok, message = check_text_patterns(
                f"{stage} {label}",
                texts[key],
                expectations.get(f"{key}_contains"),
                expectations.get(f"{key}_not_contains"),
            )
            if not ok:
                return False, message

            ok, message = check_pattern_counts(
                f"{stage} {label}",
                texts[key],
                expectations.get(f"{key}_count_at_least"),
            )
            if not ok:
                return False, message

    return True, ""


def compare_text_golden(generated_file: Path, golden_file: Path,
                        update_golden: bool, verbose: bool) -> tuple[str, str | None]:
    if update_golden:
        shutil.copyfile(generated_file, golden_file)
        return "updated", None

    generated_text = generated_file.read_text(encoding="utf-8")
    golden_text = golden_file.read_text(encoding="utf-8")
    if generated_text == golden_text:
        return "matched", None

    if not verbose:
        return "differ", None

    diff_lines = difflib.unified_diff(
        golden_text.splitlines(),
        generated_text.splitlines(),
        fromfile=str(golden_file),
        tofile=str(generated_file),
        lineterm="",
    )
    return "differ", "\n".join(list(diff_lines)[:20])


def run_metal_compile(metal_file: Path, module_cache_dir: Path) -> subprocess.CompletedProcess[str]:
    module_cache_dir.mkdir(parents=True, exist_ok=True)
    return run_command(
        [
            "xcrun",
            "-sdk",
            "macosx",
            "metal",
            "-fmodules-cache-path=" + str(module_cache_dir),
            "-c",
            str(metal_file),
            "-o",
            os.devnull,
        ]
    )


def check_wave_operations(output_dir: Path) -> tuple[bool, str]:
    expectations = {
        "wave_operations_pass0_comp.internals.json": [
            "OpGroupNonUniformFAdd",
            "OpGroupNonUniformFMul",
            "OpGroupNonUniformFMin",
            "OpGroupNonUniformFMax",
            "OpGroupNonUniformAll",
            "OpGroupNonUniformAny",
            "OpGroupNonUniformBroadcast",
        ],
        "wave_operations_pass1_comp.internals.json": [
            "OpGroupNonUniformFAdd",
            "OpGroupNonUniformFMin",
            "OpGroupNonUniformFMax",
            "OpGroupNonUniformAll",
            "OpGroupNonUniformBroadcast",
        ],
        "wave_operations_pass2_comp.internals.json": [
            "OpGroupNonUniformFAdd",
            "OpGroupNonUniformFMin",
            "OpGroupNonUniformFMax",
            "OpGroupNonUniformAll",
            "OpGroupNonUniformAny",
            "OpGroupNonUniformBroadcast",
        ],
    }

    for file_name, patterns in expectations.items():
        path = output_dir / file_name
        if not path.exists():
            return False, f"missing {file_name}"

        data = json.loads(path.read_text(encoding="utf-8"))
        spirv = data.get("spirv_dis", "")
        for pattern in patterns:
            if pattern not in spirv:
                return False, f"{file_name} missing '{pattern}'"

    return True, ""


def check_variant_specialization(output_dir: Path, test_name: str, expectations: dict[str, list[str]]) -> tuple[bool, str]:
    vert_metal_path, message = find_variant_stage_file(output_dir, test_name, "vert", "metal")
    if vert_metal_path is None:
        return False, message
    vert_hlsl_path, message = find_variant_stage_file(output_dir, test_name, "vert", "hlsl")
    if vert_hlsl_path is None:
        return False, message
    vert_internals_path, message = find_variant_stage_file(output_dir, test_name, "vert", "internals.json")
    if vert_internals_path is None:
        return False, message
    frag_internals_path, message = find_variant_stage_file(output_dir, test_name, "frag", "internals.json")
    if frag_internals_path is None:
        return False, message

    vert_metal = vert_metal_path.read_text(encoding="utf-8")
    vert_hlsl = vert_hlsl_path.read_text(encoding="utf-8")
    vert_internals = json.loads(vert_internals_path.read_text(encoding="utf-8"))
    frag_internals = json.loads(frag_internals_path.read_text(encoding="utf-8"))

    checks = [
        ("vertex metal", vert_metal, expectations.get("vert_metal_contains"), expectations.get("vert_metal_not_contains")),
        ("vertex hlsl", vert_hlsl, expectations.get("vert_hlsl_contains"), expectations.get("vert_hlsl_not_contains")),
        ("vertex ir", vert_internals.get("ir", ""), expectations.get("vert_ir_contains"), expectations.get("vert_ir_not_contains")),
        ("fragment ir", frag_internals.get("ir", ""), expectations.get("frag_ir_contains"), expectations.get("frag_ir_not_contains")),
        ("fragment spirv", frag_internals.get("spirv_dis", ""), expectations.get("frag_spirv_contains"), expectations.get("frag_spirv_not_contains")),
    ]

    for label, text, contains, not_contains in checks:
        ok, message = check_text_patterns(label, text, contains, not_contains)
        if not ok:
            return False, message

    return True, ""


def main() -> int:
    parser = argparse.ArgumentParser(description="BWSL Regression Test Runner")
    parser.add_argument("--metal", "-m", action="store_true", help="Enable Metal shader validation (macOS only)")
    parser.add_argument(
        "--update-golden",
        "-u",
        action="store_true",
        help="Update golden files with current Metal output",
    )
    parser.add_argument("--verbose", "-v", action="store_true", help="Show detailed output")
    args = parser.parse_args()

    metal_validation = args.metal or args.update_golden
    update_golden = args.update_golden
    verbose = args.verbose

    script_dir = Path(__file__).resolve().parent
    root = script_dir.parent
    output_dir = script_dir / "output"
    golden_dir = script_dir / "golden"
    modules_dir = root / "modules"
    metal_module_cache_dir = output_dir / ".metal-module-cache"

    if metal_validation and not has_metal_tooling():
        print(f"{YELLOW}Warning: Metal validation requested but not available (requires macOS with Xcode){NC}")
        metal_validation = False

    output_dir.mkdir(parents=True, exist_ok=True)
    golden_dir.mkdir(parents=True, exist_ok=True)

    bwslc = compiler_path(root)
    if not bwslc.exists():
        print(f"{YELLOW}Building compiler...{NC}")
        if not build_compiler(root):
            print(f"{RED}Failed to build compiler{NC}")
            return 1
        bwslc = compiler_path(root)
        if not bwslc.exists():
            print(f"{RED}Failed to build compiler{NC}")
            return 1

    passed = failed = skipped = 0
    metal_passed = metal_failed = 0
    golden_passed = golden_failed = golden_updated = 0

    print("========================================")
    print("BWSL Regression Test Suite")
    print("========================================")
    if metal_validation:
        print(f"Metal validation: {GREEN}enabled{NC}")
    if update_golden:
        print(f"Mode: {BLUE}updating golden files{NC}")
    print()

    for test_file in sorted(script_dir.glob("*.bwsl")):
        test_name = test_file.stem

        if is_module_file(test_file):
            print(f"[{YELLOW}SKIP{NC}] {test_name} (module file)")
            skipped += 1
            continue

        config_args = config_args_for_test(script_dir, test_name)
        translation_expectations = TRANSLATION_EXPECTATION_TESTS.get(test_name)
        text_goldens = find_text_golden_files(golden_dir, test_name)
        compile_args: list[str] = []
        needs_internals = (
            test_name in INLINE_RETURN_TESTS or
            translation_expectations is not None or
            test_name == "wave_operations"
        )
        needs_all_outputs = bool(text_goldens) or translation_expectations is not None

        if needs_all_outputs:
            compile_args.append("-all")
        elif metal_validation:
            compile_args.append("-metal")

        if needs_internals:
            compile_args.append("-internals")

        result = run_command(
            [
                str(bwslc),
                str(test_file),
                "-o",
                str(output_dir),
                "-modules",
                str(modules_dir),
                *config_args,
                *compile_args,
            ],
            cwd=root,
        )

        if result.returncode != 0:
            error_text = result.stdout.strip()
            if not error_text:
                unsigned_code = result.returncode & 0xFFFFFFFF
                error_text = f"process exited with code {result.returncode} (0x{unsigned_code:08X})"
            print(f"[{RED}FAIL{NC}] {test_name}")
            print(f"       Error: {error_text}")
            failed += 1
            continue

        if config_args:
            print(f"[{GREEN}PASS{NC}] {test_name} (config: {Path(config_args[1]).name})")
        else:
            print(f"[{GREEN}PASS{NC}] {test_name}")
        passed += 1

        if test_name in INLINE_RETURN_JUMP_TESTS:
            ok, message = check_inline_return_jump(output_dir / f"{test_name}_pass0_vert.internals.json")
            if not ok:
                print(f"[{RED}FAIL{NC}] {test_name}")
                print("       Error: inline return jump check failed")
                if message:
                    print(f"       Detail: {message}")
                failed += 1
                passed -= 1
                continue

        if test_name in INLINE_RETURN_LOOP_TESTS:
            ok, message = check_inline_return_loop(output_dir / f"{test_name}_pass0_vert.internals.json")
            if not ok:
                print(f"[{RED}FAIL{NC}] {test_name}")
                print("       Error: inline return loop guard check failed")
                if message:
                    print(f"       Detail: {message}")
                failed += 1
                passed -= 1
                continue

        if translation_expectations is not None:
            ok, message = check_translation_expectations(output_dir, test_name, translation_expectations)
            if not ok:
                print(f"[{RED}FAIL{NC}] {test_name}")
                print(f"       Error: translation expectation mismatch: {message}")
                failed += 1
                passed -= 1
                continue

        if test_name == "wave_operations":
            ok, message = check_wave_operations(output_dir)
            if not ok:
                print(f"[{RED}FAIL{NC}] {test_name}")
                print(f"       Error: wave SPIR-V expectation mismatch: {message}")
                failed += 1
                passed -= 1
                continue

        if test_name in VARIANT_REFLECTION_TESTS:
            variant_expectations = VARIANT_REFLECTION_TESTS[test_name]

            ok, data, message = run_variant_dump(bwslc, root, test_file, modules_dir)
            if not ok:
                print(f"[{RED}FAIL{NC}] {test_name}")
                print(f"       Error: variant reflection dump failed: {message}")
                failed += 1
                passed -= 1
                continue

            ok, message = check_variant_reflection(
                data,
                variant_expectations["declared"],
                variant_expectations["implicit"],
                variant_expectations["default_selected"],
            )
            if not ok:
                print(f"[{RED}FAIL{NC}] {test_name}")
                print(f"       Error: variant reflection mismatch: {message}")
                failed += 1
                passed -= 1
                continue

            ok, data, message = run_variant_dump(
                bwslc,
                root,
                test_file,
                modules_dir,
                variant_expectations["override_args"],
            )
            if not ok:
                print(f"[{RED}FAIL{NC}] {test_name}")
                print(f"       Error: overridden variant reflection dump failed: {message}")
                failed += 1
                passed -= 1
                continue

            ok, message = check_variant_reflection(
                data,
                variant_expectations["declared"],
                variant_expectations["implicit"],
                variant_expectations["override_selected"],
            )
            if not ok:
                print(f"[{RED}FAIL{NC}] {test_name}")
                print(f"       Error: overridden variant reflection mismatch: {message}")
                failed += 1
                passed -= 1
                continue

            override_result = run_command(
                [
                    str(bwslc),
                    str(test_file),
                    "-o",
                    str(output_dir),
                    "-modules",
                    str(modules_dir),
                    *variant_expectations["override_args"],
                ],
                cwd=root,
            )
            if override_result.returncode != 0:
                print(f"[{RED}FAIL{NC}] {test_name}")
                print(f"       Error: legal variant override failed: {override_result.stdout.strip()}")
                failed += 1
                passed -= 1
                continue

            illegal_result = run_command(
                [
                    str(bwslc),
                    str(test_file),
                    "-modules",
                    str(modules_dir),
                    *variant_expectations["illegal_args"],
                ],
                cwd=root,
            )
            illegal_error = illegal_result.stdout.strip()
            if illegal_result.returncode == 0:
                print(f"[{RED}FAIL{NC}] {test_name}")
                print("       Error: illegal variant selection unexpectedly succeeded")
                failed += 1
                passed -= 1
                continue
            if variant_expectations["illegal_error"] not in illegal_error:
                print(f"[{RED}FAIL{NC}] {test_name}")
                print(f"       Error: illegal variant selection returned unexpected message: {illegal_error}")
                failed += 1
                passed -= 1
                continue

            specialization = variant_expectations.get("specialization")
            if specialization:
                default_output_dir = output_dir / "variant_default"
                default_result = compile_variant_outputs(
                    bwslc,
                    root,
                    test_file,
                    modules_dir,
                    default_output_dir,
                    config_args,
                )
                if default_result.returncode != 0:
                    error_text = default_result.stdout.strip() or f"variant default compile exited with code {default_result.returncode}"
                    print(f"[{RED}FAIL{NC}] {test_name}")
                    print(f"       Error: default variant specialization compile failed: {error_text}")
                    failed += 1
                    passed -= 1
                    continue

                ok, message = check_variant_specialization(
                    default_output_dir,
                    test_name,
                    specialization["default"],
                )
                if not ok:
                    print(f"[{RED}FAIL{NC}] {test_name}")
                    print(f"       Error: default variant specialization mismatch: {message}")
                    failed += 1
                    passed -= 1
                    continue

                override_output_dir = output_dir / "variant_override"
                override_result = compile_variant_outputs(
                    bwslc,
                    root,
                    test_file,
                    modules_dir,
                    override_output_dir,
                    config_args,
                    variant_expectations["override_args"],
                )
                if override_result.returncode != 0:
                    error_text = override_result.stdout.strip() or f"variant override compile exited with code {override_result.returncode}"
                    print(f"[{RED}FAIL{NC}] {test_name}")
                    print(f"       Error: override variant specialization compile failed: {error_text}")
                    failed += 1
                    passed -= 1
                    continue

                ok, message = check_variant_specialization(
                    override_output_dir,
                    test_name,
                    specialization["override"],
                )
                if not ok:
                    print(f"[{RED}FAIL{NC}] {test_name}")
                    print(f"       Error: override variant specialization mismatch: {message}")
                    failed += 1
                    passed -= 1
                    continue

        if metal_validation:
            for metal_file in find_metal_outputs(output_dir, test_name):
                metal_result = run_metal_compile(metal_file, metal_module_cache_dir)

                if metal_result.returncode != 0:
                    print(f"       {RED}Metal FAIL{NC}: {metal_file.name}")
                    if verbose:
                        for line in metal_result.stdout.splitlines()[:10]:
                            print(f"         {line}")
                    metal_failed += 1
                    continue

                metal_passed += 1

        for golden_file in text_goldens:
            generated_file = output_dir / golden_file.name
            if not generated_file.exists():
                print(f"       {RED}Golden MISS{NC}: {golden_file.name}")
                golden_failed += 1
                continue

            status, diff_text = compare_text_golden(generated_file, golden_file, update_golden, verbose)
            if status == "updated":
                print(f"       {BLUE}Golden updated{NC}: {golden_file.name}")
                golden_updated += 1
            elif status == "matched":
                if verbose:
                    print(f"       {GREEN}Golden match{NC}: {golden_file.name}")
                golden_passed += 1
            else:
                print(f"       {RED}Golden DIFF{NC}: {golden_file.name}")
                if diff_text:
                    for line in diff_text.splitlines():
                        print(f"         {line}")
                golden_failed += 1

    variant_error_dir = script_dir / "variant_errors"
    if variant_error_dir.exists():
        for test_file in sorted(variant_error_dir.glob("*.bwsl")):
            expected_error = VARIANT_ERROR_TESTS.get(test_file.name)
            if expected_error is None:
                continue

            result = run_command(
                [
                    str(bwslc),
                    str(test_file),
                    "-modules",
                    str(modules_dir),
                    "-dump-variant-space",
                ],
                cwd=root,
            )

            if result.returncode == 0:
                print(f"[{RED}FAIL{NC}] variant_errors/{test_file.stem}")
                print("       Error: invalid variant test unexpectedly succeeded")
                failed += 1
                continue

            if expected_error not in result.stdout:
                print(f"[{RED}FAIL{NC}] variant_errors/{test_file.stem}")
                print(f"       Error: expected '{expected_error}' in error output, got: {result.stdout.strip()}")
                failed += 1
                continue

            print(f"[{GREEN}PASS{NC}] variant_errors/{test_file.stem}")
            passed += 1

    error_case_dir = script_dir / "error_cases"
    if error_case_dir.exists():
        for test_file in sorted(error_case_dir.glob("*.bwsl")):
            expected_error = ERROR_CASE_TESTS.get(test_file.name)
            if expected_error is None:
                continue

            result = run_command(
                [
                    str(bwslc),
                    str(test_file),
                    "-modules",
                    str(modules_dir),
                ],
                cwd=root,
            )

            if result.returncode == 0:
                print(f"[{RED}FAIL{NC}] error_cases/{test_file.stem}")
                print("       Error: invalid error-case test unexpectedly succeeded")
                failed += 1
                continue

            if expected_error not in result.stdout:
                print(f"[{RED}FAIL{NC}] error_cases/{test_file.stem}")
                print(f"       Error: expected '{expected_error}' in error output, got: {result.stdout.strip()}")
                failed += 1
                continue

            print(f"[{GREEN}PASS{NC}] error_cases/{test_file.stem}")
            passed += 1

    print()
    print("========================================")
    print(f"Results: {passed} passed, {failed} failed, {skipped} skipped")
    if metal_validation:
        print(f"Metal:   {metal_passed} compiled, {metal_failed} failed")
    if update_golden:
        print(f"Golden:  {golden_updated} files updated")
    elif (golden_passed + golden_failed) > 0:
        print(f"Golden:  {golden_passed} matched, {golden_failed} differ")
    print("========================================")

    return 1 if failed > 0 or metal_failed > 0 or golden_failed > 0 else 0


if __name__ == "__main__":
    raise SystemExit(main())
