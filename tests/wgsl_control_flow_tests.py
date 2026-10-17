#!/usr/bin/env python3
"""Validate structured WGSL and phi transfers against independent GPU results."""

import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    "phi_swap": ('''uint a = 1u; uint b = 2u;
        for (uint i = 0u; i < 5u; i++) { uint previous = a; a = b; b = previous; }
        uint result = a * 10u + b;''', lambda index: 21),
    "nested_loops": ('''uint result = 0u;
        for (uint i = 0u; i < id % 2u + 2u; i++) {
            for (uint j = 0u; j < 6u; j++) {
                skip if (j == 1u); break if (j == 4u);
                result += i * 10u + j;
            }
        }''', lambda index: 40 if index % 2 == 0 else 105),
    "while_zero_and_skip": ('''uint result = 0u; uint i = 0u;
        while (i < 0u) { result += 999u; }
        while (i < 7u) { i++; skip if (i % 2u == 0u); result += i; }''', lambda index: 16),
    "switch_loop_exit": ('''uint result = 0u;
        for (uint i = 0u; i < 8u; i++) {
            switch (i) {
                case 0: { result += 1u; skip; }
                case 1: { result += 2u; }
                default: { result += 4u; break; }
            }
            result += 10u;
        }''', lambda index: 17),
    "signed_switch": ('''int key = int(id % 3u) - 1; uint result = 0u;
        switch (key) {
            case -1: { result = 11u; }
            case 0: { result = 22u; }
            default: { result = 33u; }
        }''', lambda index: [11, 22, 33][index % 3]),
    "short_circuit_side_effect": ('''uint result = 0u; uint value = id;
        if ((id & 1u) == 0u && ++value > id) { result = 10u; }
        if ((id & 1u) != 0u || ++value > id) { result += value; }''',
        lambda index: index + (12 if index % 2 == 0 else 0)),
    "counted_until": ('''uint result = 0u;
        loop(10) { result++; } until (result == 4u);''', lambda index: 4),
    "early_return": ('''if ((id & 1u) == 0u) { resources.result[id] = 99u; return; }
        uint result = 17u;''', lambda index: 99 if index % 2 == 0 else 17),
    "constant_branch_in_loop": ('''uint result = 0u; int mode = 2;
        for (uint i = 0u; i < id % 3u + 1u; i++) { if (mode == 2) { result += 2u; } result += 1u; }''',
        lambda index: (index % 3 + 1) * 3),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=ROOT / "build/bwslc")
    parser.add_argument("--oracle", type=Path, default=ROOT / "build/wgsl-oracle/release/wgsl-oracle")
    parser.add_argument("--tint", type=Path, default=ROOT / "build/wgsl-tools/tint/bwsl-tint-bench")
    parser.add_argument("--output", type=Path, default=ROOT / "build/wgsl-control-tests")
    parser.add_argument("--skip-gpu", action="store_true")
    args = parser.parse_args()
    records = []
    for name, (body, expected) in CASES.items():
        directory = args.output / name
        directory.mkdir(parents=True, exist_ok=True)
        source = directory / "source.bwsl"
        source.write_text('''pipeline Control {
            resources { result: buffer<uint> }
            pass "Run" { use resources { result }
                compute "Main" [64, 1, 1] {
                    uint id = input.global_id.x;
                    ''' + body + '''
                    resources.result[id] = result;
                }
            }
        }''')
        command = [str(args.compiler), str(source), "-wgsl", "-o", str(directory)]
        process = subprocess.run(command, capture_output=True, text=True, timeout=30)
        checks = {"compile": process.returncode == 0}
        (directory / "compile.log").write_text(process.stdout + process.stderr)
        shader = directory / "source.comp.wgsl"
        if checks["compile"]:
            for tool, executable in (("naga", args.oracle), ("tint", args.tint)):
                validated = subprocess.run([str(executable), "bench", str(shader), "validate", "3", "1"],
                                           capture_output=True, text=True, timeout=30)
                checks[tool] = validated.returncode == 0
                (directory / f"{tool}.log").write_text(validated.stdout + validated.stderr)
            if not args.skip_gpu and all(checks.values()):
                case = directory / "case.json"
                case.write_text(json.dumps({"workgroups": [1, 1, 1], "buffers": [
                    {"binding": 0, "data": [0xDEADBEEF] * 64, "expected": [expected(index) for index in range(64)]}]}))
                executed = subprocess.run([str(args.oracle), "run", str(shader), str(case)],
                                          capture_output=True, text=True, timeout=60)
                checks["gpu"] = executed.returncode == 0 and json.loads(executed.stdout).get("passed", False)
                (directory / "gpu.log").write_text(executed.stdout + executed.stderr)
        passed = all(checks.values())
        print(f"[{'PASS' if passed else 'FAIL'}] {name}: {checks}")
        if not checks["compile"]:
            print(process.stdout + process.stderr)
        records.append({"name": name, "passed": passed, "checks": checks})
    (args.output / "results.json").write_text(json.dumps(records, indent=2) + "\n")
    return int(not all(record["passed"] for record in records))


if __name__ == "__main__":
    raise SystemExit(main())
