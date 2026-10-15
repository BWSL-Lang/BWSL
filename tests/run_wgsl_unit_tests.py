#!/usr/bin/env python3
"""Build and run arena/IR backend tests, optionally with ASan and UBSan."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess


ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    name = "wgsl-unit-tests-sanitize" if args.sanitize else "wgsl-unit-tests"
    binary = ROOT / "build" / name
    binary.parent.mkdir(parents=True, exist_ok=True)
    flags = ["-std=c++20", "-O1", "-g"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    subprocess.run([
        *shlex.split(os.environ.get("CXX", "clang++")), *flags,
        f"-I{ROOT / 'src'}", f"-I{ROOT / 'vendor'}",
        str(ROOT / "tests/wgsl_backend_unit_tests.cpp"), "-o", str(binary),
    ], check=True)
    artifact = ROOT / "build/wgsl-unit-continuing.wgsl"
    environment = os.environ.copy()
    environment["BWSL_WGSL_UNIT_OUTPUT"] = str(artifact)
    subprocess.run([str(binary)], check=True, timeout=60, env=environment)
    suffix = ".exe" if os.name == "nt" else ""
    validators = [ROOT / f"build/wgsl-oracle/release/wgsl-oracle{suffix}",
                  ROOT / f"build/wgsl-tools/tint/bwsl-tint-bench{suffix}"]
    for validator in validators:
        if validator.is_file():
            subprocess.run([str(validator), "bench", str(artifact), "validate", "1", "0"], check=True, timeout=30)


if __name__ == "__main__":
    main()
