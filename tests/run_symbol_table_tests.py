#!/usr/bin/env python3
"""Build and run focused symbol-table regressions, optionally with ASan/UBSan."""
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
    binary = ROOT / "build" / ("symbol-table-tests-sanitize" if args.sanitize else "symbol-table-tests")
    binary.parent.mkdir(parents=True, exist_ok=True)
    includes = sorted({p.parent for p in (ROOT / "src").rglob("*.h")})
    flags = ["-std=c++20", "-O1", "-g"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    subprocess.run([*shlex.split(os.environ.get("CXX", "clang++")), *flags,
                    f"-I{ROOT / 'src'}", f"-I{ROOT / 'vendor'}",
                    *[f"-I{p}" for p in includes],
                    str(ROOT / "tests/symbol_table_tests.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
