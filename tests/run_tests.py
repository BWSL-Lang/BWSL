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
    "backend_spelling_alias_rejected.bwsl": "SPIR-V validation failed",
    "invalid_intrinsic_arity.bwsl": "'sin' accepts at most 1 arguments, got 2",
    "missing_semicolon.bwsl": "Expected ';' after expression",
    "unknown_module_import.bwsl": "Unknown module 'DoesNotExist'",
    "workgroup_id_wrong_stage.bwsl": "input.workgroup_id is only available in compute shaders",
    "local_id_wrong_stage.bwsl": "input.local_id is only available in compute shaders",
    "matrix_alias_rejected.bwsl": "Matrix aliases float2x2/float3x3/float4x4 are not supported",
    "dereference_non_pointer.bwsl": "dereference (`^` postfix) applied to a non-pointer value",
    "pointer_in_ternary.bwsl": "ternary expression with pointer operands is not supported",
    "recursion_not_supported.bwsl": "recursion is not supported",
    "const_redeclared.bwsl": "Variable already declared in this scope",
    "compute_with_vertex_stage.bwsl": "Compute passes cannot include vertex/fragment stages",
    "duplicate_compute_block.bwsl": "Only one compute block is allowed per pass",
    "array_size_overflow.bwsl": "Invalid array size. Max 256k elements",
    "eval_if_runtime_data.bwsl": "Eval if condition must be a compile-time value",
    "eval_for_runtime_bound.bwsl": "Eval for range bounds must be compile-time values",
    "eval_loop_until_runtime_data.bwsl": "Eval loop until condition must be a compile-time value",
    "eval_while_bad_condition_type.bwsl": "Eval while condition must resolve to bool, int, uint, or float",
    "eval_while_iteration_limit.bwsl": "Eval while exceeded iteration limit",
    "eval_while_runtime_data.bwsl": "Eval while condition must be a compile-time value",
}

FORBIDDEN_SOURCE_ALIAS_NAMES = (
    "mix",
    "frac",
    "inversesqrt",
    "dFdx",
    "dFdy",
    "dFdxFine",
    "dFdyFine",
    "dFdxCoarse",
    "dFdyCoarse",
    "fwidthFine",
    "fwidthCoarse",
)

TOP_LEVEL_EXPECTED_ERROR_TESTS = {
    "resources_undeclared_error": "Resource not declared in pipeline resources block",
}

TEXT_GOLDEN_SUFFIXES = {".metal", ".hlsl", ".glsl", ".gles"}

EQUIV_BACKENDS = ("spirv", "hlsl", "glsl")

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


def has_hlsl_tooling() -> bool:
    return shutil.which("dxc") is not None


def has_glsl_tooling() -> bool:
    return shutil.which("glslangValidator") is not None


def find_tool(name: str) -> Path | None:
    path = shutil.which(name)
    if path is not None:
        return Path(path)

    candidates: list[Path] = []
    vulkan_sdk = os.environ.get("VULKAN_SDK")
    if vulkan_sdk:
        candidates.append(Path(vulkan_sdk) / "bin")

    sdk_root = Path.home() / "VulkanSDK"
    if sdk_root.exists():
        candidates.extend(sdk_root.glob("*/*/bin"))
        candidates.extend(sdk_root.glob("*/bin"))

    candidates.extend(Path(p) for p in ("/usr/local/bin", "/opt/homebrew/bin"))

    names = [name]
    if os.name == "nt" and not name.lower().endswith(".exe"):
        names.insert(0, f"{name}.exe")

    for directory in candidates:
        for exe_name in names:
            candidate = directory / exe_name
            if candidate.is_file():
                return candidate
    return None


def has_spirv_val_tooling() -> bool:
    return find_tool("spirv-val") is not None


def has_spirv_dis_tooling() -> bool:
    return find_tool("spirv-dis") is not None


def validate_spirv(spv_path: Path,
                   target_env: str = "vulkan1.2") -> tuple[bool, str]:
    """Run spirv-val on a SPIR-V binary. Returns (ok, stderr-on-failure).

    Catches malformed modules that would otherwise reach the driver: bad
    binding decorations, missing types, invalid control flow, etc. Cheap
    enough (~1ms/shader) to run on every backend output in the suite.
    """
    spirv_val = find_tool("spirv-val")
    if spirv_val is None:
        return False, "spirv-val was not found in PATH, VULKAN_SDK, or common install locations"

    result = run_command([
        str(spirv_val), "--target-env", target_env, str(spv_path),
    ])
    if result.returncode != 0:
        return False, result.stdout.strip()
    return True, ""


HLSL_PROFILE = {"vert": "vs_6_0", "frag": "ps_6_0", "comp": "cs_6_0"}
GLSLANG_STAGE = {"vert": "vert", "frag": "frag", "comp": "comp"}


def stage_from_filename(path: Path) -> str | None:
    parts = path.stem.rsplit("_", 1)
    if len(parts) != 2:
        return None
    stage = parts[1]
    return stage if stage in ("vert", "frag", "comp") else None


def is_module_file(path: Path) -> bool:
    pattern = re.compile(r"^(module|pipeline)\b")
    for line in path.read_text(encoding="utf-8").splitlines():
        if pattern.match(line):
            return line.split()[0] == "module"
    return False


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
                            output_dir: Path,
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

        spirv_dis_available = has_spirv_dis_tooling()
        if any(name.startswith("ir_") or name.startswith("spirv_") for name in expectations):
            path, message = find_stage_file(output_dir, test_name, stage, "internals.json")
            if path is None:
                return False, f"{stage}: {message}"
            data = json.loads(path.read_text(encoding="utf-8"))
            texts["ir"] = data.get("ir", "")
            if spirv_dis_available:
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


def run_hlsl_compile(hlsl_file: Path) -> subprocess.CompletedProcess[str]:
    stage = stage_from_filename(hlsl_file) or "frag"
    profile = HLSL_PROFILE[stage]
    return run_command(
        [
            "dxc",
            "-T",
            profile,
            "-E",
            "main",
            str(hlsl_file),
            "-Fo",
            os.devnull,
        ]
    )


def run_glslang_compile(glsl_file: Path) -> subprocess.CompletedProcess[str]:
    stage = stage_from_filename(glsl_file) or "frag"
    return run_command(
        [
            "glslangValidator",
            "-S",
            GLSLANG_STAGE[stage],
            str(glsl_file),
        ]
    )


def find_backend_outputs(output_dir: Path, test_name: str, ext: str) -> list[Path]:
    files = {path for path in output_dir.glob(f"{test_name}*.{ext}")}
    exact = output_dir / f"{test_name}.{ext}"
    if exact.exists():
        files.add(exact)
    return sorted(files)


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

    if not has_spirv_dis_tooling():
        return True, ""

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

    spirv_dis_available = has_spirv_dis_tooling()
    frag_spirv_dis = frag_internals.get("spirv_dis", "") if spirv_dis_available else ""
    checks = [
        ("vertex metal", vert_metal, expectations.get("vert_metal_contains"), expectations.get("vert_metal_not_contains")),
        ("vertex hlsl", vert_hlsl, expectations.get("vert_hlsl_contains"), expectations.get("vert_hlsl_not_contains")),
        ("vertex ir", vert_internals.get("ir", ""), expectations.get("vert_ir_contains"), expectations.get("vert_ir_not_contains")),
        ("fragment ir", frag_internals.get("ir", ""), expectations.get("frag_ir_contains"), expectations.get("frag_ir_not_contains")),
        ("fragment spirv", frag_spirv_dis, expectations.get("frag_spirv_contains") if spirv_dis_available else None, expectations.get("frag_spirv_not_contains") if spirv_dis_available else None),
    ]

    for label, text, contains, not_contains in checks:
        ok, message = check_text_patterns(label, text, contains, not_contains)
        if not ok:
            return False, message

    return True, ""


def equiv_runner_path(root: Path) -> Path:
    return root / "build" / ("equiv_runner.exe" if os.name == "nt" else "equiv_runner")


_HLSL_PROFILE = {"comp": "cs_6_0", "vert": "vs_6_0", "frag": "ps_6_0"}
_GLSL_STAGE = {"comp": "comp", "vert": "vert", "frag": "frag"}


_HLSL_TEXTURE_DECL_RE = re.compile(
    r'^(\s*)(Texture\w+(?:<[^>]+>)?\s+\S+\s*:\s*register\(t(\d+)\)\s*;)\s*$')
_HLSL_SAMPLER_DECL_RE = re.compile(
    r'^(\s*)(SamplerState\s+\S+\s*:\s*register\(s\d+\)\s*;)\s*$')


def _combine_hlsl_image_samplers(src: str) -> str:
    """Pair adjacent Texture2D / SamplerState declarations into one combined
    image sampler descriptor via [[vk::combinedImageSampler]] + vk::binding.

    SPIRV-Cross emits HLSL with a Texture2D and SamplerState per BWSL
    sampler2D resource, each on its own register (t/s). Without annotations
    dxc would place both at the same binding slot on set 0 (illegal) or at
    different binding spaces depending on shift flags. By merging them into
    a single COMBINED_IMAGE_SAMPLER at the original texture binding, the
    re-emitted SPIR-V matches the native BWSL and GLSL layouts so the same
    Vulkan descriptor set works across all three backends.
    """
    lines = src.split('\n')
    out: list[str] = []
    i = 0
    n = len(lines)
    while i < n:
        tm = _HLSL_TEXTURE_DECL_RE.match(lines[i])
        if tm:
            j = i + 1
            while j < n and lines[j].strip() == '':
                j += 1
            sm = _HLSL_SAMPLER_DECL_RE.match(lines[j]) if j < n else None
            if sm:
                binding = tm.group(3)
                attr = f'[[vk::combinedImageSampler]][[vk::binding({binding}, 0)]]'
                out.append(f'{tm.group(1)}{attr} {tm.group(2)}')
                for k in range(i + 1, j):
                    out.append(lines[k])
                out.append(f'{sm.group(1)}{attr} {sm.group(2)}')
                i = j + 1
                continue
        out.append(lines[i])
        i += 1
    return '\n'.join(out)


def convert_hlsl_to_spirv(hlsl_file: Path, out_spv: Path,
                          stage: str = "comp") -> tuple[bool, str]:
    profile = _HLSL_PROFILE.get(stage)
    if profile is None:
        return False, f"unsupported stage for dxc: {stage}"

    src = hlsl_file.read_text(encoding="utf-8")
    patched = _combine_hlsl_image_samplers(src)
    input_path = hlsl_file
    if patched != src:
        input_path = hlsl_file.with_suffix(hlsl_file.suffix + ".vk")
        input_path.write_text(patched, encoding="utf-8")

    result = run_command([
        "dxc", "-spirv", "-T", profile, "-E", "main",
        "-fvk-use-dx-layout",
        str(input_path), "-Fo", str(out_spv),
    ])
    if result.returncode != 0:
        return False, result.stdout.strip()
    return True, ""


def convert_glsl_to_spirv(glsl_file: Path, out_spv: Path,
                          stage: str = "comp") -> tuple[bool, str]:
    gstage = _GLSL_STAGE.get(stage)
    if gstage is None:
        return False, f"unsupported stage for glslangValidator: {stage}"

    # BWSL's GLSL emission targets desktop-GL semantics (gl_VertexID /
    # gl_InstanceID), but compiling to Vulkan SPIR-V with -V requires
    # gl_VertexIndex / gl_InstanceIndex. For vertex/fragment stages, patch
    # the source on the fly. Compute shaders use gl_GlobalInvocationID which
    # is unchanged between GL and Vulkan, so no substitution is needed.
    input_path = glsl_file
    if stage in ("vert", "frag"):
        src = glsl_file.read_text(encoding="utf-8")
        patched = src.replace("gl_VertexID", "gl_VertexIndex") \
                     .replace("gl_InstanceID", "gl_InstanceIndex")
        if patched != src:
            input_path = glsl_file.with_suffix(glsl_file.suffix + ".vk")
            input_path.write_text(patched, encoding="utf-8")

    result = run_command([
        "glslangValidator", "-V", "-S", gstage,
        str(input_path), "-o", str(out_spv),
    ])
    if result.returncode != 0:
        return False, result.stdout.strip()
    return True, ""


def pack_input_values(spec: dict, out_path: Path) -> None:
    import struct
    values = spec["input_values"]
    itype = spec.get("input_type", "float")
    fmt_char = {"float": "f", "int": "i", "uint": "I"}.get(itype)
    if fmt_char is None:
        raise ValueError(f"unsupported input_type: {itype}")
    packed = struct.pack(f"{len(values)}{fmt_char}", *values)
    out_path.write_bytes(packed)


def pack_values(itype: str, values: list, out_path: Path) -> None:
    """Generic typed-value packer (float/int/uint) for raster resources."""
    import struct
    fmt_char = {"float": "f", "int": "i", "uint": "I"}.get(itype)
    if fmt_char is None:
        raise ValueError(f"unsupported resource type: {itype}")
    packed = struct.pack(f"{len(values)}{fmt_char}", *values)
    out_path.write_bytes(packed)


def dispatch_raster(runner: Path, vert_spv: Path, frag_spv: Path,
                    output_bin: Path, spec: dict,
                    resource_bins: list[tuple[str, int, Path]] | None = None,
                    texture_bins: list[tuple[Path, int, int, int]] | None = None,
                    depth_output_bin: Path | None = None,
                    vbo_spec: tuple[Path, int, int, list[tuple[int, str, int]]]
                                | None = None
                    ) -> tuple[bool, str]:
    """Run the raster equivalence dispatcher.

    resource_bins: list of (kind, binding, path) tuples, where kind is one of
    "ssbo" or "ubo". Each contributes an upload of `path` into the matching
    descriptor binding, visible to both vertex and fragment stages.

    texture_bins: list of (path, width, height, binding) tuples. Each texture
    is uploaded as an RGBA8_UNORM 2D image bound as a COMBINED_IMAGE_SAMPLER
    at the given binding, with a nearest-clamp sampler.

    depth_output_bin: optional path for float32 depth-buffer readback when
    the spec enables depth testing.

    vbo_spec: optional (path, vertex_count, stride, attribs) tuple; attribs
    is a list of (location, format_name, offset). When provided, enables
    interleaved vertex-buffer input instead of the fullscreen-triangle path.
    """
    args = [
        str(runner),
        "--raster",
        "--vert-spirv", str(vert_spv),
        "--frag-spirv", str(frag_spv),
        "--width", str(spec["width"]),
        "--height", str(spec["height"]),
        "--output", str(output_bin),
        "--output-size", str(spec["output_size"]),
        "--set", str(spec.get("descriptor_set", 1)),
    ]
    mrt = int(spec.get("mrt", 1))
    if mrt != 1:
        args += ["--raster-mrt", str(mrt)]
    if spec.get("depth"):
        args += ["--raster-depth"]
        if depth_output_bin is not None:
            args += ["--raster-depth-output", str(depth_output_bin)]
    if vbo_spec is not None:
        vbo_path, vcount, stride, attribs = vbo_spec
        args += ["--raster-vbo", str(vbo_path), str(vcount), str(stride)]
        for loc, fmt, off in attribs:
            args += ["--raster-vbo-attr", str(loc), fmt, str(off)]
    for kind, binding, path in (resource_bins or []):
        flag = "--raster-ssbo" if kind == "ssbo" else "--raster-ubo"
        args += [flag, str(path), str(binding)]
    for path, width, height, binding in (texture_bins or []):
        args += ["--raster-tex2d", str(path), str(width), str(height),
                 str(binding)]
    result = run_command(args)
    if result.returncode != 0:
        return False, result.stdout.strip()
    return True, ""


def dispatch_equiv(runner: Path, spv_files: list[Path], output_bin: Path,
                   spec: dict, input_bin: Path | None) -> tuple[bool, str]:
    # Per-pass groups: spec["passes"][i]["groups"] for multi-pass; otherwise
    # a single entry from spec["groups"] applied to the sole pass.
    pass_specs = spec.get("passes")
    if pass_specs is None:
        pass_specs = [{"groups": spec.get("groups", [1, 1, 1])}]
    if len(pass_specs) != len(spv_files):
        return False, (f"spec has {len(pass_specs)} passes but "
                       f"{len(spv_files)} spv files were found")

    args = [
        str(runner),
        "--output", str(output_bin),
        "--output-size", str(spec["output_size"]),
        "--output-binding", str(spec.get("output_binding", 0)),
        "--set", str(spec.get("descriptor_set", 1)),
    ]
    if input_bin is not None:
        args += [
            "--input", str(input_bin),
            "--input-binding", str(spec.get("input_binding", 0)),
        ]
    for spv, ps in zip(spv_files, pass_specs):
        g = ps.get("groups", [1, 1, 1])
        args += [
            "--pass-spirv", str(spv),
            "--pass-groups", str(g[0]), str(g[1]), str(g[2]),
        ]
    result = run_command(args)
    if result.returncode != 0:
        return False, result.stdout.strip()
    return True, ""


def compare_bytes(reference: bytes, actual: bytes, spec: dict) -> tuple[bool, str]:
    if len(reference) != len(actual):
        return False, f"size mismatch: ref={len(reference)} actual={len(actual)}"

    tolerance = float(spec.get("tolerance", 0.0))
    output_type = spec.get("output_type", "bytes")

    if tolerance == 0.0 or output_type == "bytes":
        if reference != actual:
            for i, (r, a) in enumerate(zip(reference, actual)):
                if r != a:
                    return False, f"byte {i}: ref=0x{r:02x} actual=0x{a:02x}"
        return True, ""

    import struct
    if output_type == "float":
        count = len(reference) // 4
        ref_vals = struct.unpack(f"{count}f", reference)
        act_vals = struct.unpack(f"{count}f", actual)
    else:
        return False, f"unsupported output_type for tolerance: {output_type}"

    for i, (r, a) in enumerate(zip(ref_vals, act_vals)):
        if abs(r - a) > tolerance:
            return False, f"element {i}: ref={r:.6f} actual={a:.6f} diff={abs(r-a):.6g}"
    return True, ""


def run_raster_equiv_test(test_name: str, test_out: Path, spec: dict,
                          runner: Path, verbose: bool,
                          spirv_val: bool = False) -> bool:
    """Dispatch a single raster-mode equivalence test across backends.

    Expects a single-pass pipeline with a vertex + fragment stage. Looks for
    *_vert.spv + *_frag.spv (native SPIR-V), *_vert.hlsl + *_frag.hlsl (dxc
    cross-compile), and *_vert.glsl + *_frag.glsl (glslang cross-compile).
    """
    native_vert = list(test_out.glob(f"{test_name}_*_vert.spv"))
    native_frag = list(test_out.glob(f"{test_name}_*_frag.spv"))
    if len(native_vert) != 1 or len(native_frag) != 1:
        print(f"[{RED}FAIL{NC}] {test_name} (raster: need exactly 1 vert/frag pair, got "
              f"{len(native_vert)}/{len(native_frag)})")
        return False

    if spirv_val:
        for label, spv in (("native vert", native_vert[0]),
                           ("native frag", native_frag[0])):
            ok_v, msg_v = validate_spirv(spv)
            if not ok_v:
                print(f"[{RED}FAIL{NC}] {test_name} ({label} spirv-val)")
                print(f"       {msg_v}")
                return False

    # Backend -> (vert_spv_path, frag_spv_path) pair.
    backends: dict[str, tuple[Path, Path]] = {
        "spirv": (native_vert[0], native_frag[0]),
    }

    for cross_name, vert_suffix, frag_suffix, converter, stage_vert, stage_frag in (
        ("hlsl", "vert.hlsl", "frag.hlsl", convert_hlsl_to_spirv, "vert", "frag"),
        ("glsl", "vert.glsl", "frag.glsl", convert_glsl_to_spirv, "vert", "frag"),
    ):
        vert_src = list(test_out.glob(f"{test_name}_*_{vert_suffix}"))
        frag_src = list(test_out.glob(f"{test_name}_*_{frag_suffix}"))
        if len(vert_src) != 1 or len(frag_src) != 1:
            if verbose:
                print(f"       {YELLOW}{cross_name.upper()} raster skipped{NC}: "
                      f"vert={len(vert_src)} frag={len(frag_src)}")
            continue
        vert_spv = test_out / f"{test_name}_{cross_name}_vert.spv"
        frag_spv = test_out / f"{test_name}_{cross_name}_frag.spv"
        ok_v, msg_v = converter(vert_src[0], vert_spv, stage=stage_vert)
        if not ok_v:
            if verbose:
                print(f"       {YELLOW}{cross_name.upper()} vert -> SPIR-V skipped{NC}: {msg_v}")
            continue
        ok_f, msg_f = converter(frag_src[0], frag_spv, stage=stage_frag)
        if not ok_f:
            if verbose:
                print(f"       {YELLOW}{cross_name.upper()} frag -> SPIR-V skipped{NC}: {msg_f}")
            continue
        if spirv_val:
            ok_vv, msg_vv = validate_spirv(vert_spv)
            ok_fv, msg_fv = validate_spirv(frag_spv)
            if not (ok_vv and ok_fv):
                print(f"[{RED}FAIL{NC}] {test_name} ({cross_name} spirv-val)")
                if not ok_vv: print(f"       vert: {msg_vv}")
                if not ok_fv: print(f"       frag: {msg_fv}")
                return False
        backends[cross_name] = (vert_spv, frag_spv)

    # Materialize raster resource bindings (SSBO / UBO) if requested. The same
    # on-disk buffer is reused by every backend since contents are identical.
    resource_bins: list[tuple[str, int, Path]] = []
    for i, res in enumerate(spec.get("resources", [])):
        kind = res.get("kind", "ssbo")
        binding = int(res["binding"])
        if "values" in res:
            res_path = test_out / f"{test_name}_res{i}.bin"
            pack_values(res.get("type", "float"), res["values"], res_path)
        elif "bytes_path" in res:
            res_path = Path(res["bytes_path"])
        else:
            print(f"[{RED}FAIL{NC}] {test_name} (resource {i} has no values/bytes_path)")
            return False
        resource_bins.append((kind, binding, res_path))

    # Materialize raster textures. Accepts RGBA8 pixel data either as raw
    # bytes, as a list of [r,g,b,a] quads in 0..255, or as "values" of length
    # width*height*4 in 0..255. A single image is shared across all backends
    # since every descriptor layout matches after the HLSL rewrite pass.
    texture_bins: list[tuple[Path, int, int, int]] = []
    for i, tex in enumerate(spec.get("textures", [])):
        binding = int(tex["binding"])
        w = int(tex["width"])
        h = int(tex["height"])
        tex_path = test_out / f"{test_name}_tex{i}.bin"
        expected = w * h * 4
        data: bytes
        if "values" in tex:
            vs = tex["values"]
            data = bytes(int(v) & 0xFF for v in vs)
        elif "pixels" in tex:
            flat: list[int] = []
            for px in tex["pixels"]:
                if len(px) != 4:
                    print(f"[{RED}FAIL{NC}] {test_name} (texture {i} pixel "
                          f"must be [r,g,b,a])")
                    return False
                flat.extend(int(c) & 0xFF for c in px)
            data = bytes(flat)
        elif "bytes_path" in tex:
            data = Path(tex["bytes_path"]).read_bytes()
        elif "fill" in tex:
            fill = tex["fill"]
            if len(fill) != 4:
                print(f"[{RED}FAIL{NC}] {test_name} (texture {i} fill must "
                      f"be [r,g,b,a])")
                return False
            data = bytes([int(c) & 0xFF for c in fill]) * (w * h)
        else:
            print(f"[{RED}FAIL{NC}] {test_name} (texture {i} has no "
                  f"values/pixels/bytes_path/fill)")
            return False
        if len(data) != expected:
            print(f"[{RED}FAIL{NC}] {test_name} (texture {i} size "
                  f"{len(data)} != {expected} for {w}x{h} RGBA8)")
            return False
        tex_path.write_bytes(data)
        texture_bins.append((tex_path, w, h, binding))

    # Materialize an optional interleaved vertex buffer. The spec shape is:
    #   "vertices": {
    #      "stride": <bytes per vertex>,
    #      "attributes": [{"location": 0, "format": "R32G32B32_SFLOAT",
    #                      "offset": 0}, ...],
    #      "data": [<flat list of typed components per vertex>]   OR
    #      "bytes_path": "<path to prebuilt VBO file>"
    #   }
    # We pack "data" as an interleaved raw byte blob according to each
    # attribute's format; the Vulkan runner treats it as opaque bytes.
    vbo_spec = None
    verts = spec.get("vertices")
    if verts is not None:
        import struct
        stride = int(verts["stride"])
        attrs = verts.get("attributes", [])
        attrib_tuples: list[tuple[int, str, int]] = [
            (int(a["location"]), a["format"], int(a["offset"]))
            for a in attrs
        ]
        vbo_path = test_out / f"{test_name}_vbo.bin"
        if "bytes_path" in verts:
            vbo_path = Path(verts["bytes_path"])
            vcount = int(verts["count"])
        elif "data" in verts:
            # `data` is a list-of-lists: one entry per vertex, each vertex a
            # flat list of per-attribute components in attribute order. E.g.
            # for [pos3, color3] stride=24, a vertex is [x,y,z,r,g,b].
            rows = verts["data"]
            if not rows:
                print(f"[{RED}FAIL{NC}] {test_name} (vertices.data is empty)")
                return False
            # Determine the struct pack char for each attribute.
            fmt_to_pack = {
                "R32_SFLOAT": ("f", 1),
                "R32G32_SFLOAT": ("f", 2),
                "R32G32B32_SFLOAT": ("f", 3),
                "R32G32B32A32_SFLOAT": ("f", 4),
                "R32_SINT": ("i", 1),
                "R32_UINT": ("I", 1),
                "R32G32_UINT": ("I", 2),
                "R32G32B32_UINT": ("I", 3),
                "R32G32B32A32_UINT": ("I", 4),
            }
            packers: list[tuple[str, int, int]] = []
            for a in attrs:
                p = fmt_to_pack.get(a["format"])
                if p is None:
                    print(f"[{RED}FAIL{NC}] {test_name} "
                          f"(unsupported vertex format {a['format']})")
                    return False
                packers.append((p[0], p[1], int(a["offset"])))
            buf = bytearray(stride * len(rows))
            for vi, row in enumerate(rows):
                cursor = 0
                base = vi * stride
                for pack_char, ncomp, off in packers:
                    comps = row[cursor:cursor + ncomp]
                    cursor += ncomp
                    packed = struct.pack(f"{ncomp}{pack_char}", *comps)
                    buf[base + off:base + off + len(packed)] = packed
            vbo_path.write_bytes(bytes(buf))
            vcount = len(rows)
        else:
            print(f"[{RED}FAIL{NC}] {test_name} (vertices: need data/bytes_path)")
            return False
        vbo_spec = (vbo_path, vcount, stride, attrib_tuples)

    depth_enabled = bool(spec.get("depth"))
    depth_check = depth_enabled and bool(spec.get("check_depth", depth_enabled))

    outputs: dict[str, bytes] = {}
    depth_outputs: dict[str, bytes] = {}
    dispatch_errors: list[str] = []
    for backend, (vs, fs) in backends.items():
        out_bin = test_out / f"{test_name}_{backend}.bin"
        depth_bin = (test_out / f"{test_name}_{backend}_depth.bin"
                     if depth_check else None)
        ok, msg = dispatch_raster(runner, vs, fs, out_bin, spec,
                                  resource_bins=resource_bins,
                                  texture_bins=texture_bins,
                                  depth_output_bin=depth_bin,
                                  vbo_spec=vbo_spec)
        if not ok:
            dispatch_errors.append(f"{backend}: {msg}")
            continue
        outputs[backend] = out_bin.read_bytes()
        if depth_check and depth_bin is not None and depth_bin.exists():
            depth_outputs[backend] = depth_bin.read_bytes()

    if "spirv" not in outputs:
        print(f"[{RED}FAIL{NC}] {test_name} (native raster dispatch failed)")
        for e in dispatch_errors:
            print(f"       {e}")
        return False

    reference = outputs["spirv"]
    test_failed = False
    details: list[str] = []
    for backend, data in outputs.items():
        if backend == "spirv":
            continue
        ok, msg = compare_bytes(reference, data, spec)
        if not ok:
            test_failed = True
            details.append(f"{backend}: {msg}")
    # Depth readback is always float32; compare with the same tolerance as
    # the color output (or 0 by default). We synthesize a depth-only spec so
    # compare_bytes picks the float path.
    if depth_check and "spirv" in depth_outputs:
        depth_ref = depth_outputs["spirv"]
        depth_spec = {
            "tolerance": float(spec.get("depth_tolerance",
                                        spec.get("tolerance", 0.0))),
            "output_type": "float",
        }
        for backend, data in depth_outputs.items():
            if backend == "spirv":
                continue
            ok, msg = compare_bytes(depth_ref, data, depth_spec)
            if not ok:
                test_failed = True
                details.append(f"{backend} depth: {msg}")
    for e in dispatch_errors:
        test_failed = True
        details.append(e)

    if test_failed:
        print(f"[{RED}FAIL{NC}] {test_name}")
        for d in details:
            print(f"       {d}")
        return False

    backend_names = ", ".join(sorted(outputs.keys()))
    extra = []
    if int(spec.get("mrt", 1)) > 1:
        extra.append(f"mrt={spec['mrt']}")
    if depth_enabled:
        extra.append("depth")
    if vbo_spec is not None:
        extra.append("vbo")
    tag = ", ".join(["raster", *extra])
    print(f"[{GREEN}PASS{NC}] {test_name} ({backend_names}, {tag})")
    return True


def run_equivalence_suite(root: Path, bwslc: Path, runner: Path,
                          modules_dir: Path, verbose: bool,
                          spirv_val: bool = False) -> tuple[int, int]:
    equiv_dir = root / "tests" / "equivalence"
    if not equiv_dir.exists():
        return 0, 0

    output_root = equiv_dir / "output"
    shutil.rmtree(output_root, ignore_errors=True)
    output_root.mkdir(parents=True, exist_ok=True)

    passed = 0
    failed = 0

    print()
    print("========================================")
    print("Equivalence Test Suite")
    print("========================================")

    for spec_file in sorted(equiv_dir.glob("*.json")):
        test_name = spec_file.stem
        shader_file = equiv_dir / f"{test_name}.bwsl"
        if not shader_file.exists():
            print(f"[{YELLOW}SKIP{NC}] {test_name} (missing shader)")
            continue

        with spec_file.open("r", encoding="utf-8") as f:
            spec = json.load(f)

        test_out = output_root / test_name
        test_out.mkdir(exist_ok=True)

        compile_result = run_command(
            [
                str(bwslc),
                str(shader_file),
                "-o", str(test_out),
                "-modules", str(modules_dir),
                "-all",
            ],
            cwd=root,
        )
        if compile_result.returncode != 0:
            print(f"[{RED}FAIL{NC}] {test_name} (compile)")
            if verbose:
                print(compile_result.stdout)
            failed += 1
            continue

        if spec.get("mode") == "raster":
            ok = run_raster_equiv_test(test_name, test_out, spec, runner,
                                       verbose, spirv_val=spirv_val)
            if ok:
                passed += 1
            else:
                failed += 1
            continue

        # Discover native compute .spv files. Multi-pass tests produce
        # pass0, pass1, ... suffixes in filename order; sorted() keeps them
        # in that order since bwslc emits zero-padded suffix-less indices.
        native_spvs = sorted(test_out.glob(f"{test_name}_*_comp.spv"))
        if not native_spvs:
            print(f"[{RED}FAIL{NC}] {test_name} (no native .spv produced)")
            failed += 1
            continue

        if spirv_val:
            spv_val_failed = False
            for spv in native_spvs:
                ok_v, msg_v = validate_spirv(spv)
                if not ok_v:
                    print(f"[{RED}FAIL{NC}] {test_name} (native spirv-val)")
                    print(f"       {spv.name}: {msg_v}")
                    spv_val_failed = True
                    break
            if spv_val_failed:
                failed += 1
                continue

        # Backend -> list of per-pass .spv paths.
        backends_spv: dict[str, list[Path]] = {"spirv": list(native_spvs)}

        # HLSL / GLSL cross-compile: one per pass, converted individually.
        hlsl_files = sorted(test_out.glob(f"{test_name}_*_comp.hlsl"))
        if hlsl_files and len(hlsl_files) == len(native_spvs):
            converted = []
            all_ok = True
            for idx, f in enumerate(hlsl_files):
                spv = test_out / f"{test_name}_pass{idx}_hlsl.spv"
                ok, msg = convert_hlsl_to_spirv(f, spv)
                if not ok:
                    if verbose:
                        print(f"       {YELLOW}HLSL -> SPIR-V skipped{NC}: {msg}")
                    all_ok = False
                    break
                converted.append(spv)
            if all_ok and spirv_val:
                for spv in converted:
                    ok_v, msg_v = validate_spirv(spv)
                    if not ok_v:
                        print(f"[{RED}FAIL{NC}] {test_name} (hlsl-rt spirv-val)")
                        print(f"       {spv.name}: {msg_v}")
                        all_ok = False
                        break
            if all_ok:
                backends_spv["hlsl"] = converted

        glsl_files = sorted(test_out.glob(f"{test_name}_*_comp.glsl"))
        if glsl_files and len(glsl_files) == len(native_spvs):
            converted = []
            all_ok = True
            for idx, f in enumerate(glsl_files):
                spv = test_out / f"{test_name}_pass{idx}_glsl.spv"
                ok, msg = convert_glsl_to_spirv(f, spv)
                if not ok:
                    if verbose:
                        print(f"       {YELLOW}GLSL -> SPIR-V skipped{NC}: {msg}")
                    all_ok = False
                    break
                converted.append(spv)
            if all_ok and spirv_val:
                for spv in converted:
                    ok_v, msg_v = validate_spirv(spv)
                    if not ok_v:
                        print(f"[{RED}FAIL{NC}] {test_name} (glsl-rt spirv-val)")
                        print(f"       {spv.name}: {msg_v}")
                        all_ok = False
                        break
            if all_ok:
                backends_spv["glsl"] = converted

        input_bin: Path | None = None
        if "input_values" in spec:
            input_bin = test_out / f"{test_name}_input.bin"
            pack_input_values(spec, input_bin)
        elif "input_bytes" in spec:
            input_bin = Path(spec["input_bytes"])

        outputs: dict[str, bytes] = {}
        dispatch_errors: list[str] = []
        for backend, spv_files in backends_spv.items():
            out_bin = test_out / f"{test_name}_{backend}.bin"
            ok, msg = dispatch_equiv(runner, spv_files, out_bin, spec, input_bin)
            if not ok:
                dispatch_errors.append(f"{backend}: {msg}")
                continue
            outputs[backend] = out_bin.read_bytes()

        if "spirv" not in outputs:
            print(f"[{RED}FAIL{NC}] {test_name} (native dispatch failed)")
            for e in dispatch_errors:
                print(f"       {e}")
            failed += 1
            continue

        reference = outputs["spirv"]
        test_failed = False
        details: list[str] = []
        for backend, data in outputs.items():
            if backend == "spirv":
                continue
            ok, msg = compare_bytes(reference, data, spec)
            if not ok:
                test_failed = True
                details.append(f"{backend}: {msg}")

        for e in dispatch_errors:
            test_failed = True
            details.append(e)

        if test_failed:
            print(f"[{RED}FAIL{NC}] {test_name}")
            for d in details:
                print(f"       {d}")
            failed += 1
        else:
            backend_names = ", ".join(sorted(outputs.keys()))
            print(f"[{GREEN}PASS{NC}] {test_name} ({backend_names})")
            passed += 1

    print("----------------------------------------")
    print(f"Equivalence: {passed} passed, {failed} failed")

    return passed, failed


def main() -> int:
    parser = argparse.ArgumentParser(description="BWSL Regression Test Runner")
    parser.add_argument("--metal", "-m", action="store_true", help="Enable Metal shader validation (macOS only)")
    parser.add_argument("--hlsl", action="store_true", help="Enable HLSL validation via dxc")
    parser.add_argument("--glsl", action="store_true", help="Enable GLSL validation via glslangValidator")
    parser.add_argument("--gles", action="store_true", help="Enable GLES validation via glslangValidator")
    parser.add_argument("--all-validators", "-A", action="store_true", help="Enable Metal/HLSL/GLSL/GLES validators")
    parser.add_argument("--equivalence", "-E", action="store_true", help="Run cross-backend equivalence compute tests via Vulkan")
    parser.add_argument("--spirv-val", action="store_true",
                        help="Run spirv-val on every produced SPIR-V module (auto-on if spirv-val is installed)")
    parser.add_argument("--no-spirv-val", action="store_true",
                        help="Disable spirv-val even if the tool is installed")
    parser.add_argument("--compiler",
                        help="Path to an alternate bwslc binary (e.g. bwslc-sanitize). "
                             "If omitted, the default build/bwslc is used.")
    parser.add_argument(
        "--update-golden",
        "-u",
        action="store_true",
        help="Update golden files with current backend output",
    )
    parser.add_argument("--verbose", "-v", action="store_true", help="Show detailed output")
    args = parser.parse_args()

    metal_validation = args.metal or args.all_validators or args.update_golden
    hlsl_validation = args.hlsl or args.all_validators
    glsl_validation = args.glsl or args.all_validators
    gles_validation = args.gles or args.all_validators
    update_golden = args.update_golden
    verbose = args.verbose
    if args.no_spirv_val:
        spirv_val = False
    elif args.spirv_val:
        spirv_val = True
    else:
        spirv_val = has_spirv_val_tooling()

    script_dir = Path(__file__).resolve().parent
    root = script_dir.parent
    output_dir = script_dir / "output"
    golden_dir = script_dir / "golden"
    modules_dir = root / "modules"
    metal_module_cache_dir = output_dir / ".metal-module-cache"

    if metal_validation and not has_metal_tooling():
        print(f"{YELLOW}Warning: Metal validation requested but not available (requires macOS with Xcode){NC}")
        metal_validation = False

    if hlsl_validation and not has_hlsl_tooling():
        print(f"{YELLOW}Warning: HLSL validation requested but `dxc` not found on PATH{NC}")
        hlsl_validation = False

    if (glsl_validation or gles_validation) and not has_glsl_tooling():
        print(f"{YELLOW}Warning: GLSL/GLES validation requested but `glslangValidator` not found on PATH{NC}")
        glsl_validation = False
        gles_validation = False

    output_dir.mkdir(parents=True, exist_ok=True)
    golden_dir.mkdir(parents=True, exist_ok=True)

    if args.compiler:
        bwslc = Path(args.compiler).resolve()
        if not bwslc.exists():
            print(f"{RED}--compiler path does not exist: {bwslc}{NC}")
            return 1
        print(f"Compiler: {BLUE}{bwslc}{NC} (overridden)")
    else:
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
    hlsl_passed = hlsl_failed = 0
    glsl_passed = glsl_failed = 0
    gles_passed = gles_failed = 0
    golden_passed = golden_failed = golden_updated = 0

    print("========================================")
    print("BWSL Regression Test Suite")
    print("========================================")
    if metal_validation:
        print(f"Metal validation: {GREEN}enabled{NC}")
    if hlsl_validation:
        print(f"HLSL validation:  {GREEN}enabled{NC}")
    if glsl_validation:
        print(f"GLSL validation:  {GREEN}enabled{NC}")
    if gles_validation:
        print(f"GLES validation:  {GREEN}enabled{NC}")
    if spirv_val:
        print(f"SPIR-V validation: {GREEN}enabled{NC} (spirv-val)")
    if update_golden:
        print(f"Mode: {BLUE}updating golden files{NC}")
    print()

    for test_file in sorted(script_dir.glob("*.bwsl")):
        test_name = test_file.stem

        if is_module_file(test_file):
            print(f"[{YELLOW}SKIP{NC}] {test_name} (module file)")
            skipped += 1
            continue

        translation_expectations = TRANSLATION_EXPECTATION_TESTS.get(test_name)
        text_goldens = find_text_golden_files(golden_dir, test_name)
        compile_args: list[str] = []
        needs_internals = (
            test_name in INLINE_RETURN_TESTS or
            translation_expectations is not None or
            test_name == "wave_operations"
        )
        multi_backend_validation = hlsl_validation or glsl_validation or gles_validation
        needs_all_outputs = (
            bool(text_goldens)
            or translation_expectations is not None
            or multi_backend_validation
        )

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
                *compile_args,
            ],
            cwd=root,
        )

        expected_error = TOP_LEVEL_EXPECTED_ERROR_TESTS.get(test_name)
        if expected_error is not None:
            error_text = result.stdout.strip()
            if result.returncode == 0:
                print(f"[{RED}FAIL{NC}] {test_name}")
                print("       Error: invalid top-level error test unexpectedly succeeded")
                failed += 1
                continue
            if expected_error not in error_text:
                print(f"[{RED}FAIL{NC}] {test_name}")
                print(f"       Error: expected '{expected_error}' in error output, got: {error_text}")
                failed += 1
                continue
            print(f"[{GREEN}PASS{NC}] {test_name}")
            passed += 1
            continue

        if result.returncode != 0:
            error_text = result.stdout.strip()
            if not error_text:
                unsigned_code = result.returncode & 0xFFFFFFFF
                error_text = f"process exited with code {result.returncode} (0x{unsigned_code:08X})"
            print(f"[{RED}FAIL{NC}] {test_name}")
            print(f"       Error: {error_text}")
            failed += 1
            continue

        if spirv_val:
            spv_val_failed = False
            for spv_file in sorted(output_dir.glob(f"{test_name}_pass*.spv")):
                ok_sv, msg_sv = validate_spirv(spv_file)
                if not ok_sv:
                    print(f"[{RED}FAIL{NC}] {test_name}")
                    print(f"       spirv-val {spv_file.name}: {msg_sv}")
                    failed += 1
                    spv_val_failed = True
                    break
            if spv_val_failed:
                continue

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

        if hlsl_validation:
            for hlsl_file in find_backend_outputs(output_dir, test_name, "hlsl"):
                hlsl_result = run_hlsl_compile(hlsl_file)

                if hlsl_result.returncode != 0:
                    print(f"       {RED}HLSL FAIL{NC}: {hlsl_file.name}")
                    if verbose:
                        for line in hlsl_result.stdout.splitlines()[:10]:
                            print(f"         {line}")
                    hlsl_failed += 1
                    continue

                hlsl_passed += 1

        if glsl_validation:
            for glsl_file in find_backend_outputs(output_dir, test_name, "glsl"):
                glsl_result = run_glslang_compile(glsl_file)

                if glsl_result.returncode != 0:
                    print(f"       {RED}GLSL FAIL{NC}: {glsl_file.name}")
                    if verbose:
                        for line in glsl_result.stdout.splitlines()[:10]:
                            print(f"         {line}")
                    glsl_failed += 1
                    continue

                glsl_passed += 1

        if gles_validation:
            for gles_file in find_backend_outputs(output_dir, test_name, "gles"):
                gles_result = run_glslang_compile(gles_file)

                if gles_result.returncode != 0:
                    print(f"       {RED}GLES FAIL{NC}: {gles_file.name}")
                    if verbose:
                        for line in gles_result.stdout.splitlines()[:10]:
                            print(f"         {line}")
                    gles_failed += 1
                    continue

                gles_passed += 1

        if update_golden and text_goldens:
            base_names = {path.stem for path in text_goldens}
            for base in sorted(base_names):
                for ext in ("metal", "hlsl", "glsl", "gles"):
                    generated_file = output_dir / f"{base}.{ext}"
                    golden_file = golden_dir / f"{base}.{ext}"
                    if generated_file.exists() and not golden_file.exists():
                        shutil.copyfile(generated_file, golden_file)
                        print(f"       {BLUE}Golden created{NC}: {golden_file.name}")
                        golden_updated += 1
            text_goldens = find_text_golden_files(golden_dir, test_name)

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

            if "SPIR-V validation" in expected_error and not has_spirv_val_tooling():
                print(f"[{YELLOW}SKIP{NC}] error_cases/{test_file.stem} (spirv-val not available)")
                skipped += 1
                continue

            result = run_command(
                [
                    str(bwslc),
                    str(test_file),
                    "-modules",
                    str(modules_dir),
                    "-o",
                    str(output_dir),
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

    stdlib_source = (root / "core" / "bwsl_stdlib.h").read_text(encoding="utf-8")
    token_sources = "\n".join(
        (root / path).read_text(encoding="utf-8")
        for path in (
            "phases/lexing/bwsl_token_defs.h",
            "phases/lexing/bwsl_token_stream.h",
            "phases/lexing/bwsl_lexer.cpp",
        )
    )
    forbidden_hits: list[str] = []
    for name in FORBIDDEN_SOURCE_ALIAS_NAMES:
        intrinsic_pattern = rf'INTRINSIC_FIXED\([^,\n]+,\s*"{re.escape(name)}"'
        if re.search(intrinsic_pattern, stdlib_source):
            forbidden_hits.append(f"intrinsic alias '{name}'")
    if re.search(r"\bMIX\b", token_sources):
        forbidden_hits.append("mix token/keyword")

    if forbidden_hits:
        print(f"[{RED}FAIL{NC}] source_alias_surface")
        print("       Error: forbidden source spelling(s): " + ", ".join(forbidden_hits))
        failed += 1
    else:
        print(f"[{GREEN}PASS{NC}] source_alias_surface")
        passed += 1

    # Fuzzer-found regressions. Each file was a crash or hang until the
    # referenced fix landed. Pass = bwslc exits in bounded time with code 0 or
    # 1. Fail = signal death (negative code), timeout, or other non-zero exit.
    fuzz_regr_dir = script_dir / "fuzz_regressions"
    if fuzz_regr_dir.exists():
        for test_file in sorted(fuzz_regr_dir.glob("*.bwsl")):
            try:
                result = subprocess.run(
                    [str(bwslc), str(test_file), "-modules", str(modules_dir), "-o", str(output_dir)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    cwd=root,
                    timeout=5,
                )
            except subprocess.TimeoutExpired:
                print(f"[{RED}FAIL{NC}] fuzz_regressions/{test_file.stem}")
                print("       Error: bwslc hung — fix likely reverted")
                failed += 1
                continue

            if result.returncode not in (0, 1):
                unsigned_code = result.returncode & 0xFFFFFFFF
                print(f"[{RED}FAIL{NC}] fuzz_regressions/{test_file.stem}")
                print(f"       Error: unexpected exit {result.returncode} (0x{unsigned_code:08X}) — regression in fuzzer-found bug")
                failed += 1
                continue

            print(f"[{GREEN}PASS{NC}] fuzz_regressions/{test_file.stem}")
            passed += 1

    equiv_passed = equiv_failed = 0
    if args.equivalence:
        runner = equiv_runner_path(root)
        if not runner.exists():
            print(f"{YELLOW}Warning: equiv_runner not found at {runner}. Build with `make equiv_runner`.{NC}")
        elif not has_hlsl_tooling() or not has_glsl_tooling():
            print(f"{YELLOW}Warning: equivalence tests require both dxc and glslangValidator on PATH{NC}")
        else:
            equiv_passed, equiv_failed = run_equivalence_suite(
                root, bwslc, runner, modules_dir, verbose,
                spirv_val=spirv_val,
            )

    print()
    print("========================================")
    print(f"Results: {passed} passed, {failed} failed, {skipped} skipped")
    if metal_validation:
        print(f"Metal:   {metal_passed} compiled, {metal_failed} failed")
    if hlsl_validation:
        print(f"HLSL:    {hlsl_passed} compiled, {hlsl_failed} failed")
    if glsl_validation:
        print(f"GLSL:    {glsl_passed} compiled, {glsl_failed} failed")
    if gles_validation:
        print(f"GLES:    {gles_passed} compiled, {gles_failed} failed")
    if update_golden:
        print(f"Golden:  {golden_updated} files updated")
    elif (golden_passed + golden_failed) > 0:
        print(f"Golden:  {golden_passed} matched, {golden_failed} differ")
    print("========================================")

    if args.equivalence:
        print(f"Equiv:   {equiv_passed} passed, {equiv_failed} failed")

    backend_failed = metal_failed + hlsl_failed + glsl_failed + gles_failed
    return 1 if failed > 0 or backend_failed > 0 or golden_failed > 0 or equiv_failed > 0 else 0


if __name__ == "__main__":
    raise SystemExit(main())
