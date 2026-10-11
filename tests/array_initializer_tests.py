"""Issues #130 and #155: compact local tables and constant evaluation."""
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

        def compile(self, body, declarations="", prelude=""):
            source = self.folder / "table.bwsl"
            source.write_text(prelude + shader(body, declarations), encoding="utf-8")
            return self.compile_source(source)

        def compile_source(self, source, module_dir=None):
            target = self.folder / "output"
            command = [str(compiler), str(source), "-all", "-validation", "strict",
                       "-debug-names", "-o", str(target)]
            if module_dir is not None:
                command.extend(["-modules", str(module_dir)])
            result = subprocess.run(
                command,
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

        def test_const_arrays_in_all_scopes(self):
            source = self.folder / "table.bwsl"
            source.write_text('''
                module Tables {
                    const float[4] VALUES = {1.0, 2.0, 3.0, 4.0};
                    lookup :: (int i) -> float { return VALUES[i]; }
                }
                pipeline ConstArrays {
                    import Tables
                    const float[4] PIPE = {5.0, 6.0, 7.0, 8.0};
                    pass "Main" {
                        const float[4] PASS = {9.0, 10.0, 11.0, 12.0};
                        vertex { output.position = float4(0.0, 0.0, 0.0, 1.0); }
                        fragment {
                            const float[4] LOCAL = {13.0, 14.0, 15.0, 16.0};
                            int i = int(input.position.x) % 4;
                            output.color = float4(Tables::VALUES[i] +
                                Tables::lookup(i) + PIPE[i] + PASS[i] + LOCAL[i]);
                        }
                    }
                }
            ''', encoding="utf-8")
            target, ops = self.compile_source(source)
            self.assertTrue((target / "table.frag.spv").exists())
            arrays = {args[0] for op, args in ops if op == 28}
            self.assertGreaterEqual(sum(op == 44 and args[0] in arrays
                                        for op, args in ops), 4)

        def test_const_array_element_cannot_be_written(self):
            source = self.folder / "table.bwsl"
            source.write_text(shader('''
                const float[2] table = {1.0, 2.0};
                table[0] = 3.0;
                output.color = float4(table[0]);
            '''), encoding="utf-8")
            result = subprocess.run(
                [str(compiler), str(source), "-check", "-validation", "off"],
                capture_output=True, text=True, timeout=60)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Cannot assign to const variable 'table'", result.stdout + result.stderr)

        def test_global_const_arrays_initialize_before_control_flow(self):
            source = self.folder / "table.bwsl"
            source.write_text('''
                module Tables {
                    const float[2] VALUES = {1.0, 2.0};
                }
                pipeline InitBeforeBranch {
                    import Tables
                    const float[2] PIPE = {3.0, 4.0};
                    pass "Main" {
                        vertex { output.position = float4(0.0, 0.0, 0.0, 1.0); }
                        fragment {
                            float x = 0.0;
                            if (input.position.x > 0.0) {
                                x = Tables::VALUES[0] + PIPE[0];
                            }
                            output.color = float4(x + Tables::VALUES[1] + PIPE[1]);
                        }
                    }
                }
            ''', encoding="utf-8")
            target, ops = self.compile_source(source)
            array_types = {args[0] for op, args in ops if op == 28}  # OpTypeArray
            composites = {args[1] for op, args in ops
                          if op == 44 and args[0] in array_types}
            first_branch = next(i for i, (op, _) in enumerate(ops) if op == 250)
            array_stores = [i for i, (op, args) in enumerate(ops)
                            if op == 62 and len(args) > 1 and args[1] in composites]
            self.assertEqual(len(array_stores), 2)
            self.assertTrue(all(i < first_branch for i in array_stores))
            vertex_ops = list(instructions(target / "table.vert.spv"))
            self.assertFalse(any(op == 28 for op, _ in vertex_ops))

        def test_module_const_vector_array(self):
            source = self.folder / "table.bwsl"
            source.write_text('''
                module Kernel {
                    const float2[2] OFFSETS = {
                        float2(-1.0, 0.5), float2(1.0, -0.5)
                    };
                }
                pipeline ConstVectors {
                    import Kernel
                    pass "Main" {
                        vertex { output.position = float4(0.0, 0.0, 0.0, 1.0); }
                        fragment {
                            int i = int(input.position.x) % 2;
                            output.color = float4(Kernel::OFFSETS[i], 0.0, 1.0);
                        }
                    }
                }
            ''', encoding="utf-8")
            target, ops = self.compile_source(source)
            self.assertTrue((target / "table.frag.spv").exists())
            arrays = {args[0] for op, args in ops if op == 28}
            self.assertTrue(any(op == 44 and args[0] in arrays for op, args in ops))

        def test_imported_module_const_array(self):
            modules = self.folder / "modules"
            modules.mkdir()
            (modules / "ConstTableMod.bwsl").write_text('''
                module ConstTableMod {
                    const float[3] VALUES = {2.0, 4.0, 8.0};
                }
            ''', encoding="utf-8")
            source = self.folder / "table.bwsl"
            source.write_text('''
                pipeline ImportedConstArray {
                    import ConstTableMod
                    pass "Main" {
                        vertex { output.position = float4(0.0, 0.0, 0.0, 1.0); }
                        fragment {
                            int i = int(input.position.x) % ConstTableMod::VALUES.length;
                            output.color = float4(ConstTableMod::VALUES[i]);
                        }
                    }
                }
            ''', encoding="utf-8")
            target, ops = self.compile_source(source, modules)
            self.assertTrue((target / "table.frag.spv").exists())
            bits = [struct.unpack("<I", struct.pack("<f", value))[0]
                    for value in (2.0, 4.0, 8.0)]
            self.assert_constant_table(ops, bits)
            for kind in ("glsl", "gles"):
                self.assertNotRegex((target / f"table.{kind}.frag").read_text(),
                                    r"\bconst\b[^;]*\[")

        def test_module_const_array_names_are_scoped(self):
            source = self.folder / "table.bwsl"
            source.write_text('''
                module FirstTable {
                    const float[2] VALUES = {1.0, 2.0};
                    lookup :: (int i) -> float { return VALUES[i]; }
                }
                module SecondTable {
                    const float[2] VALUES = {3.0, 4.0};
                    lookup :: (int i) -> float { return VALUES[i]; }
                }
                pipeline ScopedTables {
                    import FirstTable
                    import SecondTable
                    const float[2] VALUES = {9.0, 10.0};
                    pass "Main" {
                        vertex { output.position = float4(0.0, 0.0, 0.0, 1.0); }
                        fragment {
                            int i = int(input.position.x) % 2;
                            output.color = float4(VALUES[i] + FirstTable::lookup(i) +
                                                   SecondTable::lookup(i));
                        }
                    }
                }
            ''', encoding="utf-8")
            target, ops = self.compile_source(source)
            self.assertTrue((target / "table.frag.spv").exists())
            arrays = {args[0] for op, args in ops if op == 28}
            self.assertGreaterEqual(sum(op == 44 and args[0] in arrays
                                        for op, args in ops), 3)

        def test_const_array_requires_full_initializer(self):
            source = self.folder / "table.bwsl"
            source.write_text(shader('''
                const float[3] table = {1.0, 2.0};
                output.color = float4(table[0]);
            '''), encoding="utf-8")
            result = subprocess.run(
                [str(compiler), str(source), "-check", "-validation", "off"],
                capture_output=True, text=True, timeout=60)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("const array initializer must provide exactly one value",
                          result.stdout + result.stderr)

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

        def test_dynamically_indexed_table_is_not_a_const_array(self):
            # Issue #151: SPIRV-Cross emits a table that is stored once as a
            # module-scope `const` array, which drivers index slowly.
            target, _ = self.compile('''
                int index = min(ELEMENT_COUNT - 1, int(ELEMENT_COUNT * input.uv.x));
                output.color = float4(lookup(index));
            ''', '''
                const int ELEMENT_COUNT = 4;
                lookup :: (int index) -> float {
                    float[ELEMENT_COUNT] table = {1.5, 2.5, 3.5, 4.5};
                    return table[index];
                }
            ''')
            for kind in ("glsl", "gles"):
                frag = (target / f"table.{kind}.frag").read_text()
                self.assertNotRegex(frag, r"\bconst\b[^;]*\[")
                self.assertRegex(frag, r"\n\s+(highp )?float \w+\[4\] = float\[\]\(1\.5, 2\.5, 3\.5, 4\.5\);")

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
                float[3] table;
                table[0u] = input.uv.x;
                table[1u] = input.uv.y * 2.0;
                table[2u] = input.uv.x + 3.0;
                output.color = float4(table[int(input.uv.y * 2.0)]);
            ''')
            _, _, stores = self.array_info(ops)
            self.assertEqual(stores, [])
            self.assertEqual(sum(op == 65 for op, _ in ops), 4)

        def test_runtime_initializer_elements_form_one_composite(self):
            _, ops = self.compile('''
                float[3] table = {input.uv.x, input.uv.y * 2.0, input.uv.x + 3.0};
                output.color = float4(table[int(input.uv.y * 2.0)]);
            ''')
            arrays, _, stores = self.array_info(ops)
            constructs = {args[1]: args[2:] for op, args in ops if op == 80 and args[0] in arrays}
            self.assertEqual(len(stores), 1)
            self.assertIn(stores[0], constructs)
            # Elements keep their source order; only the dynamic read indexes.
            mul = next(args[1] for op, args in ops if op == 133)  # OpFMul
            add = next(args[1] for op, args in ops if op == 129)  # OpFAdd
            self.assertEqual(list(constructs[stores[0]][1:]), [mul, add])
            self.assertEqual(sum(op == 65 for op, _ in ops), 1)

        def vector_table(self, ops):
            """Components of the one array, which must be stored once as a constant."""
            arrays, _, stores = self.array_info(ops)
            self.assertEqual(len(stores), 1)
            composites = {args[1]: args[2:] for op, args in ops if op == 44}
            constants = {args[1]: args[2] for op, args in ops if op == 43 and len(args) == 3}
            self.assertIn(stores[0], composites)
            to_float = lambda id: struct.unpack("<f", struct.pack("<I", constants[id]))[0]
            # No element vector is built at runtime.
            elements = {args[1] for op, args in ops if op == 28}
            self.assertFalse(any(op == 80 and args[0] in elements for op, args in ops))
            return [[to_float(c) for c in composites[v]] for v in composites[stores[0]]]

        def test_constant_vector_table(self):
            # Issue #155: arrays of vectors with constant elements were built
            # with one store per element, in local memory.
            target, ops = self.compile('''
                float2[4] table = {
                    float2(0.0, 0.0),
                    float2(0.0625, -0.1875),
                    float2(-0.0625),
                    float2(0.3125, 0.0625)
                };
                output.color = float4(table[int(input.uv.x * 3.0)], 0.0, 1.0);
            ''')
            self.assertEqual(self.vector_table(ops), [
                [0.0, 0.0], [0.0625, -0.1875], [-0.0625, -0.0625], [0.3125, 0.0625]])
            gles = (target / "table.gles.frag").read_text()
            self.assertRegex(gles, r"vec2 \w+\[4\] = vec2\[\]\(vec2\(0\.0\), "
                                   r"vec2\(0\.0625, -0\.1875\), vec2\(-0\.0625\), "
                                   r"vec2\(0\.3125, 0\.0625\)\);")

        def test_constant_vector_arithmetic_folds(self):
            _, ops = self.compile('''
                float3[4] table = {
                    float3(1.0, -3.0, 2.0) / 16.0,
                    2.0 * float3(1.0, 2.0, 3.0),
                    float3(1.0, 2.0, 3.0) - float3(0.5, 0.5, 4.0),
                    (float3(1.0) + 1.0) * float3(0.25, 0.5, 1.0)
                };
                output.color = float4(table[int(input.uv.x * 3.0)], 1.0);
            ''')
            self.assertEqual(self.vector_table(ops), [
                [0.0625, -0.1875, 0.125], [2.0, 4.0, 6.0], [0.5, 1.5, -1.0], [0.5, 1.0, 2.0]])
            # Only the dynamic index's multiply is left.
            arithmetic = [op for op, _ in ops if op in (129, 131, 133, 136, 142)]
            self.assertEqual(arithmetic, [133])

        def test_module_vector_constants_fold_into_table(self):
            _, ops = self.compile('''
                float2[3] table = {Kernel::A / 16.0, Kernel::B / 16.0, Kernel::A + Kernel::B};
                output.color = float4(table[int(input.uv.x * 2.0)], 0.0, 1.0);
            ''', "import Kernel", '''
                module Kernel {
                    const float2 A = float2(1.0, -3.0);
                    const float2 B = float2(-1.0, 3.0);
                }
            ''')
            self.assertEqual(self.vector_table(ops), [
                [0.0625, -0.1875], [-0.0625, 0.1875], [0.0, 0.0]])

        def test_vector_division_by_zero_stays_at_runtime(self):
            _, ops = self.compile('''
                float2[2] table = {float2(1.0, 2.0) / 0.0, float2(input.uv.x, 1.0)};
                output.color = float4(table[int(input.uv.x)], 0.0, 1.0);
            ''')
            arrays, _, stores = self.array_info(ops)
            constructs = {args[1] for op, args in ops if op == 80 and args[0] in arrays}
            self.assertEqual(len(stores), 1)
            self.assertIn(stores[0], constructs)
            self.assertTrue(any(op == 136 for op, _ in ops))  # OpFDiv

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
