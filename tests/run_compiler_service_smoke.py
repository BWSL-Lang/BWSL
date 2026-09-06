#!/usr/bin/env python3
"""Build and exercise the public compiler-service API with real SPIR-V validation."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=ROOT / 'build/compiler-service-smoke')
args = parser.parse_args()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
includes = sorted({p.parent for p in (ROOT / 'src').rglob('*.h')})
binary = output / 'service-smoke'
subprocess.run([*shlex.split(os.environ.get('CXX', 'clang++')), '-std=c++20', '-O0',
                *[f'-I{p}' for p in includes], f'-I{ROOT / "vendor"}',
                str(ROOT / 'tests/compiler_service_smoke.cpp'), '-o', str(binary)], check=True)
subprocess.run([str(binary), str(output / 'fixtures')], check=True, timeout=60)
