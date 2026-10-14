"""Constant propagation folds values through branches, phis and reassignment."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

from local_debug_names_tests import instructions, shader

OP_PHI = 245
OP_LOOP_MERGE = 246
OP_SELECTION_MERGE = 247
OP_BRANCH_CONDITIONAL = 250
OP_FADD = 129
OP_SDIV = 135


def run_constant_propagation_tests(compiler: Path) -> tuple[int, int]:
    compiler = Path(compiler).resolve()

    class ConstantPropagationTests(unittest.TestCase):
        def setUp(self):
            self.temp = tempfile.TemporaryDirectory(prefix="bwsl-constprop-")
            self.addCleanup(self.temp.cleanup)
            self.folder = Path(self.temp.name)

        def compile(self, source, *flags):
            path = self.folder / "constprop.bwsl"
            path.write_text(source, encoding="utf-8")
            target = self.folder / "output"
            result = subprocess.run(
                [str(compiler), str(path), *flags, "-validation", "strict", "-o", str(target)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return target

        def fragment_ops(self, body, declarations=""):
            target = self.compile(shader(body, declarations), "-all")
            return [op for op, _ in instructions(target / "constprop.frag.spv")], target

        def test_constant_branches_keep_one_arm(self):
            ops, target = self.fragment_ops('''
                float exposure = 1.0;
                float gain;
                if (exposure > 0.5) { gain = 2.0; } else { gain = 3.0; }
                float3 c = input.position.xyz * (gain * exposure);
                output.color = float4(tonemap(c, 1), 1.0);
            ''', '''
                tonemap :: (float3 c, int mode) -> float3 {
                    if (mode == 1) { return c / (c + float3(1.0)); }
                    return c;
                }
            ''')
            self.assertNotIn(OP_BRANCH_CONDITIONAL, ops)
            self.assertNotIn(OP_PHI, ops)
            glsl = (target / "constprop.glsl.frag").read_text(encoding="utf-8")
            self.assertIn("vec3(2.0)", glsl)
            self.assertNotIn("3.0", glsl)

        def test_equal_constants_from_a_runtime_branch(self):
            ops, _ = self.fragment_ops('''
                float scale;
                if (input.uv.x > 0.5) { scale = 4.0; } else { scale = 4.0; }
                output.color = float4(input.uv.y * scale);
            ''')
            self.assertNotIn(OP_PHI, ops)

        def test_runtime_branch_stays(self):
            ops, _ = self.fragment_ops('''
                float scale = 1.0;
                if (input.uv.x > 0.5) { scale = 4.0; }
                output.color = float4(input.uv.y * scale);
            ''')
            self.assertIn(OP_BRANCH_CONDITIONAL, ops)
            self.assertIn(OP_PHI, ops)

        def test_constant_condition_in_loop_folds_but_loop_stays(self):
            ops, _ = self.fragment_ops('''
                int mode = 2;
                float sum = 0.0;
                for (int i = 0; i < int(input.uv.x * 8.0); i++) {
                    if (mode == 2) { sum += 1.0; } else { sum -= 100.0; }
                }
                output.color = float4(sum);
            ''')
            self.assertEqual(ops.count(OP_LOOP_MERGE), 1)
            self.assertEqual(ops.count(OP_SELECTION_MERGE), 0)
            self.assertEqual(ops.count(OP_BRANCH_CONDITIONAL), 1)
            self.assertIn(OP_PHI, ops)

        def test_straight_line_reassignment_folds(self):
            ops, target = self.fragment_ops('''
                float brightness = 0.5;
                brightness = brightness + 0.25;
                output.color = float4(brightness, 0.0, 0.0, 1.0);
            ''')
            self.assertNotIn(OP_FADD, ops)
            glsl = (target / "constprop.glsl.frag").read_text(encoding="utf-8")
            self.assertIn("vec4(0.75, 0.0, 0.0, 1.0)", glsl)

        def test_division_by_zero_is_left_to_run_time(self):
            ops, _ = self.fragment_ops('''
                int zero = 0;
                if (input.uv.x > 2.0) { zero = 1; }
                int value = 5 / zero;
                output.color = float4(float(value));
            ''')
            self.assertIn(OP_SDIV, ops)

        def test_direct_gles_emits_only_the_taken_arm(self):
            target = self.compile(shader('''
                int mode = 1;
                float3 color;
                if (mode == 1) { color = float3(input.uv, 0.0); } else { color = float3(1.0, 0.0, 1.0); }
                output.color = float4(color, 1.0);
            '''), "-gles", "-gles-direct")
            gles = (target / "constprop.frag").read_text(encoding="utf-8")
            self.assertNotIn("if (", gles)
            self.assertNotIn("vec3(1.0, 0.0, 1.0)", gles)

        def test_many_float_constants_keep_int_constants(self):
            # The float pool started at 256 entries and overflowed into the
            # int constants, so `+ 7` came out as the bits of a float.
            terms = " + ".join(f"{i}.5 * input.position.x" for i in range(300))
            target = self.compile(shader(f'''
                int k = int(input.position.y) + 7;
                float v = {terms};
                output.color = float4(v, float(k), 0.0, 1.0);
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            self.assertIn("int(gl_FragCoord.y) + 7", glsl)

    result = unittest.TextTestRunner(verbosity=1).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(ConstantPropagationTests))
    failures = len(result.failures) + len(result.errors)
    return result.testsRun - failures, failures


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build" / "bwslc")
    args = parser.parse_args()
    _, failures = run_constant_propagation_tests(args.compiler)
    raise SystemExit(bool(failures))
