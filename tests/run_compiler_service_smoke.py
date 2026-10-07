#!/usr/bin/env python3
"""Build and exercise the public compiler-service API with real SPIR-V validation."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import re
from register_namespace_tests import lowering_overflow_shader, overflow_shader

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=ROOT / 'build/compiler-service-smoke')
args = parser.parse_args()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
fixtures = output / 'fixtures'
fixtures.mkdir(exist_ok=True)
for name, source in [('LoweringOverflow', lowering_overflow_shader()),
                     ('SSAOverflow', overflow_shader())]:
    source = source.replace('compute "Main" [8,1,1]', 'vertex')
    source = source.replace('pass "Compute"', 'pass "Main"')
    source = source.replace('input.global_id.x', '0u')
    source = source.replace('resources.output[idx]=acc;}}}',
                            'resources.output[idx]=acc;output.position=float4(acc);}'
                            'fragment {output.color=float4(1.0);}}}')
    (fixtures / f'{name}.bwsl').write_text(source, encoding='utf-8')
includes = sorted({p.parent for p in (ROOT / 'src').rglob('*.h')})
binary = output / 'service-smoke'
subprocess.run([*shlex.split(os.environ.get('CXX', 'clang++')), '-std=c++20', '-O0',
                *[f'-I{p}' for p in includes], f'-I{ROOT / "vendor"}',
                str(ROOT / 'tests/compiler_service_smoke.cpp'), '-o', str(binary)], check=True)
result = subprocess.run([str(binary), str(fixtures)], capture_output=True,
                        text=True, check=True, timeout=60)
print(result.stdout, end='')
print(result.stderr, end='')
for message in ['Shader register limit exceeded', 'SSA register limit exceeded']:
    assert re.search(r'line [1-9]\d*:[1-9]\d*: ' + message, result.stderr), result.stderr

assert re.search(r'Shadowing.bwsl:1:\d+: note: Variable \'value\' shadows parameter declared at ', result.stderr), result.stderr
assert result.stderr.count("Variable 'value' shadows parameter") == 1, result.stderr
