"""Source local names survive SSA without changing non-debug SPIR-V."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def shader(body: str) -> str:
    return '''pipeline LocalNames {
        attributes { }
        pass "Main" {
            vertex {
                output.position = float4(0.0, 0.0, 0.0, 1.0);
                output.uv = float2(0.25, 0.5);
            }
            fragment { ''' + body + " }\n}\n}\n"


def instructions(path: Path):
    data = path.read_bytes()
    words = struct.unpack(f"<{len(data) // 4}I", data)
    assert words[0] == 0x07230203
    offset = 5
    while offset < len(words):
        count, op = words[offset] >> 16, words[offset] & 0xffff
        assert count and offset + count <= len(words)
        yield op, words[offset + 1:offset + count]
        offset += count


def names(ops):
    result = {}
    for op, args in ops:
        if op == 5:  # OpName
            assert args[0] not in result, "duplicate OpName for the same ID"
            text = struct.pack(f"<{len(args) - 1}I", *args[1:])
            result[args[0]] = text.split(b"\0", 1)[0].decode()
    return result


EXAMPLE = '''
    float brightness = input.uv.x * 2.0;
    float3 tint = float3(brightness, brightness * 0.5, 1.0);
    float[4] weights;
    weights[0] = brightness;
    float total = 0.0;
    for (int i = 0; i < 4; i++) {
        total += tint.x * float(i) + weights[0];
    }
    output.color = float4(tint * total, 1.0);
'''


def run_local_debug_names_tests(compiler: Path) -> tuple[int, int]:
    compiler = Path(compiler).resolve()

    class LocalDebugNamesTests(unittest.TestCase):
        def setUp(self):
            self.temp = tempfile.TemporaryDirectory(prefix="bwsl-local-names-")
            self.addCleanup(self.temp.cleanup)
            self.folder = Path(self.temp.name)

        def compile(self, body, debug=True):
            source = self.folder / "locals.bwsl"
            source.write_text(shader(body), encoding="utf-8")
            target = self.folder / ("debug" if debug else "plain")
            result = subprocess.run(
                [str(compiler), str(source), "-all", "-o", str(target),
                 *(["-debug-names"] if debug else [])],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # Validate every stage if tooling is installed. CI provides spirv-val.
            validator = shutil.which("spirv-val")
            if validator:
                for binary in target.glob("*.spv"):
                    result = subprocess.run([validator, str(binary)],
                                            capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            glslang = shutil.which("glslangValidator")
            if glslang:
                for stage in ("vert", "frag"):
                    for kind in ("glsl", "gles"):
                        result = subprocess.run(
                            [glslang, "-S", stage, str(target / f"locals.{kind}.{stage}")],
                            capture_output=True, text=True, timeout=30)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    # HLSL front-end validation here is through glslang, not DXC.
                    result = subprocess.run(
                        [glslang, "-D", "-V", "-S", stage, "-e", "main",
                         "-o", str(target / f"validated-hlsl.{stage}.spv"),
                         str(target / f"locals.{stage}.hlsl")],
                        capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return target, list(instructions(target / "locals.frag.spv"))

        def assert_names(self, ops, expected):
            present = set(names(ops).values())
            self.assertTrue(set(expected) <= present, (expected, present))

        def test_example_scalar_vector_loop_and_array(self):
            target, ops = self.compile(EXAMPLE)
            self.assert_names(ops, ["brightness", "tint", "weights", "i", "total"])
            debug_names = names(ops)
            phi_names = [debug_names.get(args[1]) for op, args in ops if op == 245]
            self.assertIn("i", phi_names)
            self.assertIn("total", phi_names)
            # Both the initialization and increment/accumulation results retain
            # the name, not just the loop's phi. Numeric suffixes belong to Cross.
            for name in ("i", "total"):
                self.assertGreaterEqual(list(debug_names.values()).count(name), 3)
            arrays = [args[1] for op, args in ops if op == 59 and args[2] == 7]
            self.assertTrue(any(debug_names.get(id) == "weights" for id in arrays))
            for suffix in ("glsl.frag", "gles.frag", "frag.metal", "frag.hlsl"):
                text = (target / f"locals.{suffix}").read_text()
                for name in ("brightness", "tint", "weights", "i", "total"):
                    self.assertRegex(text, rf"\b{re.escape(name)}(?:_\d+)?\b")

        def test_reassignment_and_branch_phi(self):
            _, ops = self.compile('''
                float level = input.uv.x * 2.0;
                level = level + input.uv.y;
                if (input.uv.x > 0.5) { level *= input.uv.y; }
                else { level -= input.uv.y; }
                output.color = float4(level, level * level, level + 1.0, 1.0);
            ''')
            debug_names = names(ops)
            self.assertGreaterEqual(list(debug_names.values()).count("level"), 5)
            self.assertTrue(any(op == 245 and debug_names.get(args[1]) == "level"
                                for op, args in ops))

        def test_vector_updates_and_shadowed_scopes(self):
            target, ops = self.compile('''
                float3 tint = float3(input.uv, 1.0);
                tint.x += input.uv.y;
                float total = tint.x;
                { float strength = input.uv.x * 3.0; total += strength * strength; }
                { float strength = input.uv.y * 4.0; total += strength * strength; }
                output.color = float4(tint * total, 1.0);
            ''')
            self.assert_names(ops, ["tint", "strength", "total"])
            self.assertGreaterEqual(list(names(ops).values()).count("strength"), 2)
            self.assertGreaterEqual(list(names(ops).values()).count("tint"), 2)
            glsl = (target / "locals.glsl.frag").read_text()
            self.assertGreaterEqual(len(set(re.findall(r"\bstrength(?:_\d+)?\b", glsl))), 2)

        def test_range_and_collection_iterators(self):
            _, ops = self.compile('''
                float[3] weights = { 0.2, 0.3, 0.5 };
                float total = input.uv.x;
                for (step in 0..3) { total += float(step) * input.uv.y; }
                for (weight in weights) { total += weight * total; }
                output.color = float4(total, total * total, total + 1.0, 1.0);
            ''')
            self.assert_names(ops, ["step", "weight", "total", "weights"])
            self.assertTrue(any(op == 245 and names(ops).get(args[1]) == "step"
                                for op, args in ops))

        def test_nested_shadowing_and_same_named_arrays(self):
            _, ops = self.compile('''
                float strength = input.uv.x * 2.0;
                float total = strength * strength;
                {
                    float strength = input.uv.y * 3.0;
                    float[2] weights = { strength, strength * strength };
                    total += weights[0] + weights[1];
                }
                {
                    float[2] weights;
                    weights[0] = strength;
                    total += weights[0];
                }
                output.color = float4(total, strength, total * strength, 1.0);
            ''')
            debug_names = names(ops)
            self.assertGreaterEqual(list(debug_names.values()).count("strength"), 2)
            self.assertEqual(list(debug_names.values()).count("weights"), 2)

        def test_metadata_growth(self):
            body = "float delta = input.uv.x; float acc = 0.0;\n"
            body += "acc += delta;\n" * 1200
            # Grow both the instruction arrays and the local-array table.
            for i in range(20):
                body += f"float[2] weights{i}; weights{i}[0] = acc; acc += weights{i}[0];\n"
            body += "if (input.uv.y > 0.5) { acc += delta; } else { acc -= delta; }\n"
            body += "output.color = float4(acc, acc * acc, acc + delta, 1.0);"
            _, ops = self.compile(body)
            self.assert_names(ops, ["delta", "acc", *[f"weights{i}" for i in range(20)]])
            debug_names = names(ops)
            self.assertGreaterEqual(list(debug_names.values()).count("acc"), 1200)
            self.assertTrue(any(op == 245 and debug_names.get(args[1]) == "acc"
                                for op, args in ops))

        def test_aliases_do_not_rename_interfaces_or_duplicate_ids(self):
            _, ops = self.compile('''
                float2 uv = input.uv;
                float brightness = uv.x * 2.0;
                float alias = brightness;
                alias += input.uv.y;
                output.color = float4(brightness, alias, uv.x, 1.0);
            ''')
            self.assert_names(ops, ["v_uv", "brightness", "alias"])
            debug_names = names(ops)
            # Names attach to real IDs, including when copy elimination aliases
            # a declaration to an input. OpVariable results are operands[1].
            inputs = [args[1] for op, args in ops if op == 59 and args[2] == 1]
            self.assertTrue(any(debug_names.get(id) == "v_uv" for id in inputs))

        def test_reserved_names_are_left_to_cross(self):
            target, ops = self.compile('''
                float gl_local = input.uv.x * 2.0;
                output.color = float4(gl_local, gl_local * gl_local, gl_local + 1.0, 1.0);
            ''')
            self.assert_names(ops, ["gl_local"])
            glsl = (target / "locals.glsl.frag").read_text()
            self.assertNotRegex(glsl, r"\bfloat\s+gl_local\b")

        def test_debug_names_off_and_semantics_unchanged(self):
            _, plain = self.compile(EXAMPLE, debug=False)
            _, debug = self.compile(EXAMPLE, debug=True)
            locals = {"brightness", "tint", "weights", "i", "total"}
            self.assertTrue(locals.isdisjoint(names(plain).values()))
            outputs = [args[1] for op, args in debug if op == 59 and args[2] == 3]
            self.assertTrue(all(id not in names(debug) for id in outputs))
            # IDs, instructions, decorations, and executable semantics are
            # identical. Debug mode adds only OpName, never an extra copy.
            self.assertEqual([inst for inst in plain if inst[0] != 5],
                             [inst for inst in debug if inst[0] != 5])

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(LocalDebugNamesTests)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    failures = len(result.failures) + len(result.errors)
    return result.testsRun - failures - len(result.skipped), failures


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=ROOT / "build/bwslc")
    args = parser.parse_args()
    passed, failed = run_local_debug_names_tests(args.compiler)
    print(f"Local debug names: {passed} passed, {failed} failed")
    raise SystemExit(bool(failed))
