"""Issue #130: compact local tables and scalar constant evaluation."""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from local_debug_names_tests import instructions, shader


def run_array_initializer_tests(compiler: Path) -> tuple[int, int]:
    compiler = Path(compiler).resolve()

    class ArrayInitializerTests(unittest.TestCase):
        def setUp(self):
            self.temp = tempfile.TemporaryDirectory(prefix="bwsl-array-init-")
            self.addCleanup(self.temp.cleanup)
            self.folder = Path(self.temp.name)

        def compile(self, body, declarations=""):
            source = self.folder / "table.bwsl"
            source.write_text(shader(body, declarations), encoding="utf-8")
            target = self.folder / "output"
            result = subprocess.run(
                [str(compiler), str(source), "-all", "-validation", "strict",
                 "-debug-names", "-o", str(target)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
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
                            [glslang, "-S", stage, str(target / f"table.{kind}.{stage}")],
                            capture_output=True, text=True, timeout=30)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return target, list(instructions(target / "table.frag.spv"))

        def array_info(self, ops):
            arrays = {args[0] for op, args in ops if op == 28}  # OpTypeArray
            pointers = {args[0]: args[2] for op, args in ops if op == 32}
            variables = [args[1] for op, args in ops if op == 59 and
                         args[2] == 7 and pointers.get(args[0]) in arrays]
            self.assertEqual(len(variables), 1, variables)
            variable = variables[0]
            stores = [args[1] for op, args in ops if op == 62 and args[0] == variable]
            return arrays, variable, stores

        def assert_constant_table(self, ops, expected):
            arrays, _, stores = self.array_info(ops)
            self.assertEqual(len(stores), 1)
            composites = {args[1]: args for op, args in ops if op == 44 and args[0] in arrays}
            self.assertIn(stores[0], composites)
            constituents = composites[stores[0]][2:]
            self.assertEqual(len(constituents), len(expected))
            constants = {args[1]: args[2] for op, args in ops if op == 43 and len(args) == 3}
            self.assertEqual([constants[id] for id in constituents], expected)

        def test_reported_integer_literal_table(self):
            target, ops = self.compile('''
                int index = min(ELEMENT_COUNT - 1, int(ELEMENT_COUNT * input.uv.x));
                output.color = float4(lookup(index));
            ''', '''
                const int ELEMENT_COUNT = 10;
                lookup :: (int index) -> float {
                    float[ELEMENT_COUNT] table = {1,2,3,4,5,6,7,8,9,10};
                    return table[index];
                }
            ''')
            bits = [struct.unpack("<I", struct.pack("<f", float(i)))[0] for i in range(1, 11)]
            self.assert_constant_table(ops, bits)
            self.assertFalse(any(op in (111, 112, 130) for op, _ in ops))
            gles = (target / "table.gles.frag").read_text()
            self.assertIn("float[](", gles)
            self.assertNotIn("float(10)", gles)
            self.assertNotIn("10 - 1", gles)

        def test_float_table_larger_than_operand_width(self):
            values = ",".join(f"{i}.0" for i in range(72))
            target, ops = self.compile(
                f"float[72] table = {{{values}}}; output.color = float4(table[int(input.uv.x * 71.0)]);")
            bits = [struct.unpack("<I", struct.pack("<f", float(i)))[0] for i in range(72)]
            self.assert_constant_table(ops, bits)
            self.assertIn("float[](", (target / "table.gles.frag").read_text())

        def test_literal_conversions_and_arithmetic(self):
            _, ops = self.compile('''
                float[8] table = {float(3), -2, (10-1)*2, 9/2, 9%2,
                                  1.5+2.5, uint(7), float(true)};
                output.color = float4(table[int(input.uv.x * 7.0)]);
            ''')
            expected = (3.0, -2.0, 18.0, 4.0, 1.0, 4.0, 7.0, 1.0)
            bits = [struct.unpack("<I", struct.pack("<f", value))[0] for value in expected]
            self.assert_constant_table(ops, bits)
            self.assertFalse(any(op in (111, 112, 126, 127, 128, 129, 130, 131, 132, 134, 135, 138)
                                 for op, _ in ops))

        def test_unsigned_arithmetic_wrap_and_integer_bitcasts(self):
            _, ops = self.compile('''
                uint[4] table = {0u-1u, 4294967295u+1u, uint(-1), uint(6.75)};
                output.color = float4(float(table[int(input.uv.x * 3.0)]));
            ''')
            self.assert_constant_table(ops, [0xffffffff, 0, 0xffffffff, 6])
            self.assertFalse(any(op in (109, 124, 128, 130) for op, _ in ops))

        def test_signed_float_cast_truncates_toward_zero(self):
            _, ops = self.compile('''
                int[3] table = {int(-6.75), int(4294967295u), -9%2};
                output.color = float4(float(table[int(input.uv.x * 2.0)]));
            ''')
            self.assert_constant_table(ops, [0xfffffffa, 0xffffffff, 0xffffffff])
            # The dynamic index still needs one float-to-int conversion.
            self.assertEqual(sum(op == 110 for op, _ in ops), 1)

        def test_runtime_sequence_uses_composite_and_remains_mutable(self):
            _, ops = self.compile('''
                float[3] table = {input.uv.x, 2.0, 3.0};
                table[1] = input.uv.y;
                output.color = float4(table[int(input.uv.x * 2.0)]);
            ''')
            arrays, _, stores = self.array_info(ops)
            constructs = {args[1] for op, args in ops if op == 80 and args[0] in arrays}
            self.assertEqual(len(stores), 1)
            self.assertIn(stores[0], constructs)
            # The later element write still uses an access chain and store.
            self.assertEqual(sum(op == 65 for op, _ in ops), 2)

        def test_partial_initializer_keeps_element_stores(self):
            _, ops = self.compile('''
                float[4] table = {1, 2, 3};
                output.color = float4(table[0] + table[1] + table[2]);
            ''')
            _, _, stores = self.array_info(ops)
            self.assertEqual(stores, [])
            self.assertEqual(sum(op == 65 for op, _ in ops), 6)

        def test_boolean_table_uses_boolean_constants(self):
            _, ops = self.compile('''
                bool[3] table = {true, false, true};
                output.color = float4(table[int(input.uv.x * 2.0)] ? 1.0 : 0.0);
            ''')
            arrays, _, stores = self.array_info(ops)
            self.assertEqual(len(stores), 1)
            composite = next(args for op, args in ops if op == 44 and
                             args[0] in arrays and args[1] == stores[0])
            booleans = {args[1]: op == 41 for op, args in ops if op in (41, 42)}
            self.assertEqual([booleans[id] for id in composite[2:]], [True, False, True])

        def test_store_sequences_do_not_cross_branch_boundaries(self):
            _, ops = self.compile('''
                float[3] table;
                table[0u] = 1.0;
                table[1u] = 2.0;
                if (input.uv.x > 0.5) { table[2u] = 3.0; }
                else { table[2u] = 4.0; }
                output.color = float4(table[2u]);
            ''')
            _, _, stores = self.array_info(ops)
            self.assertEqual(stores, [])
            self.assertEqual(sum(op == 65 for op, _ in ops), 5)

        def test_interrupted_stores_keep_expression_order(self):
            _, ops = self.compile('''
                float[3] table = {input.uv.x, input.uv.y * 2.0, input.uv.x + 3.0};
                output.color = float4(table[int(input.uv.y * 2.0)]);
            ''')
            _, _, stores = self.array_info(ops)
            self.assertEqual(stores, [])
            self.assertEqual(sum(op == 65 for op, _ in ops), 4)

        def test_loop_initializer_and_mutable_arithmetic(self):
            _, ops = self.compile('''
                float sum = 0.0;
                for (int i = 0; i < 3; i++) {
                    float[3] table = {1,2,3};
                    table[1] = float(i);
                    sum += table[i];
                }
                output.color = float4(sum);
            ''')
            _, _, stores = self.array_info(ops)
            self.assertEqual(len(stores), 1)
            self.assertTrue(any(op == 245 for op, _ in ops))  # SSA phi
            self.assertTrue(any(op == 128 for op, _ in ops))  # runtime i+1
            self.assertTrue(any(op == 111 for op, _ in ops))  # runtime float(i)
            loop = next(i for i, (op, _) in enumerate(ops) if op == 246)
            store = next(i for i, (op, args) in enumerate(ops) if op == 62 and args[1] == stores[0])
            self.assertGreater(store, loop)

        def test_undefined_constant_operations_stay_at_runtime(self):
            _, ops = self.compile('''
                int value = int(2147483648.0);
                value += 1/0;
                value += 2147483647+1;
                output.color = float4(float(value) + input.uv.x);
            ''')
            self.assertTrue(any(op == 110 for op, _ in ops))
            self.assertTrue(any(op == 135 for op, _ in ops))
            self.assertTrue(any(op == 128 for op, _ in ops))

    result = unittest.TextTestRunner(verbosity=1).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(ArrayInitializerTests))
    failures = len(result.failures) + len(result.errors)
    return result.testsRun - failures, failures


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build" / "bwslc")
    args = parser.parse_args()
    _, failures = run_array_initializer_tests(args.compiler)
    raise SystemExit(bool(failures))
