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
        compile_args: list[str] = []
        if test_name in INLINE_RETURN_TESTS:
            compile_args.append("-internals")
        if metal_validation:
            compile_args.append("-metal")

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
            print(f"[{RED}FAIL{NC}] {test_name}")
            print(f"       Error: {result.stdout.strip()}")
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

        if not metal_validation:
            continue

        for metal_file in find_metal_outputs(output_dir, test_name):
            golden_file = golden_dir / metal_file.name
            metal_result = run_command(["xcrun", "-sdk", "macosx", "metal", "-c", str(metal_file), "-o", os.devnull])

            if metal_result.returncode != 0:
                print(f"       {RED}Metal FAIL{NC}: {metal_file.name}")
                if verbose:
                    for line in metal_result.stdout.splitlines()[:10]:
                        print(f"         {line}")
                metal_failed += 1
                continue

            metal_passed += 1

            if update_golden:
                shutil.copyfile(metal_file, golden_file)
                print(f"       {BLUE}Golden updated{NC}: {metal_file.name}")
                golden_updated += 1
            elif golden_file.exists():
                if metal_file.read_text(encoding="utf-8") == golden_file.read_text(encoding="utf-8"):
                    if verbose:
                        print(f"       {GREEN}Golden match{NC}: {metal_file.name}")
                    golden_passed += 1
                else:
                    print(f"       {RED}Golden DIFF{NC}: {metal_file.name}")
                    if verbose:
                        diff_lines = difflib.unified_diff(
                            golden_file.read_text(encoding="utf-8").splitlines(),
                            metal_file.read_text(encoding="utf-8").splitlines(),
                            fromfile=str(golden_file),
                            tofile=str(metal_file),
                            lineterm="",
                        )
                        for line in list(diff_lines)[:20]:
                            print(f"         {line}")
                    golden_failed += 1

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
