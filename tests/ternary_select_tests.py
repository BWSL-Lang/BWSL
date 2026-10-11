"""Issue #169: `?:` with pure, cheap arms lowers to OpSelect."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

from local_debug_names_tests import instructions, shader

OP_SELECT = 169
OP_BRANCH_CONDITIONAL = 250


def run_ternary_select_tests(compiler: Path) -> tuple[int, int]:
    compiler = Path(compiler).resolve()

    class TernarySelectTests(unittest.TestCase):
        def setUp(self):
            self.temp = tempfile.TemporaryDirectory(prefix="bwsl-ternary-")
            self.addCleanup(self.temp.cleanup)
            self.folder = Path(self.temp.name)

        def compile(self, body, declarations=""):
            source = self.folder / "ternary.bwsl"
            source.write_text(shader(body, declarations), encoding="utf-8")
            target = self.folder / "output"
            result = subprocess.run(
                [str(compiler), str(source), "-all", "-validation", "strict",
                 "-o", str(target)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return [op for op, _ in instructions(target / "ternary.frag.spv")]

        def test_issue_repro_uses_selects(self):
            ops = self.compile('''
                float L = input.position.x;
                bool black = L <= 0.00001;
                bool white = L >= 99.99999;
                float3 r = float3(white ? 100.0 : (black ? 0.0 : L),
                                  (black || white) ? 0.0 : L * 2.0, 1.0);
                output.color = float4(r, 1.0);
            ''')
            self.assertNotIn(OP_BRANCH_CONDITIONAL, ops)
            self.assertEqual(ops.count(OP_SELECT), 3)

        def test_vector_arms_with_scalar_condition(self):
            ops = self.compile('''
                bool c = input.uv.x > 0.5;
                float3 v = c ? float3(0.0) : float3(input.uv, 1.0);
                bool2 b = c ? bool2(true, false) : bool2(input.uv.y > 0.5);
                output.color = float4(v, b.x ? 1.0 : 0.0);
            ''')
            self.assertNotIn(OP_BRANCH_CONDITIONAL, ops)
            self.assertEqual(ops.count(OP_SELECT), 3)

        def test_bool_vector_condition_and_arms(self):
            # Selecting bool vectors used only the condition's first lane,
            # which failed SPIR-V validation.
            ops = self.compile('''
                bool2 c = input.uv > float2(0.5);
                bool2 b = c ? bool2(true, false) : bool2(input.uv.x > 0.25);
                output.color = float4(b.x ? 1.0 : 0.0, b.y ? 1.0 : 0.0, 0.0, 1.0);
            ''')
            self.assertIn(OP_SELECT, ops)

        def test_math_intrinsics_and_literal_division(self):
            ops = self.compile('''
                float c = input.uv.x;
                float lin = c > 0.04045 ? pow((c + 0.055) / 1.055, 2.4) : c / 12.92;
                output.color = float4(lin, abs(c) > 0.5 ? sqrt(c) : length(input.uv), 0.0, 1.0);
            ''')
            self.assertNotIn(OP_BRANCH_CONDITIONAL, ops)
            self.assertEqual(ops.count(OP_SELECT), 2)

        def test_struct_and_matrix_arms_merge_after_computing(self):
            ops = self.compile('''
                bool c = input.uv.x > 0.5;
                Pair p = Pair(input.uv.x, input.uv);
                Pair q = Pair(1.0, float2(2.0));
                Pair r = c ? p : q;
                mat2 m = c ? mat2(1.0, 0.0, 0.0, 1.0) : mat2(input.uv.x, 0.0, 0.0, 1.0);
                output.color = float4(r.a, r.b.y, m[0][0], 1.0);
            ''', "struct Pair { float a; float2 b; }")
            # OpSelect can't pick a struct or matrix before SPIR-V 1.4; the
            # branch only chooses between values already computed.
            self.assertEqual(ops.count(OP_BRANCH_CONDITIONAL), 2)

        def test_guarded_arms_keep_branch(self):
            for arm in ("a[i]", "float(100 / i)", "twice(input.uv.x)", "ddx(input.uv.x)"):
                with self.subTest(arm=arm):
                    ops = self.compile(f'''
                        float[4] a;
                        for (int k = 0; k < 4; k++) {{ a[k] = float(k); }}
                        int i = int(input.uv.x * 8.0);
                        output.color = float4((i > 0 && i < 4) ? {arm} : 0.0);
                    ''', "twice :: (float x) -> float { return x * 2.0; }")
                    self.assertNotIn(OP_SELECT, ops)

    result = unittest.TextTestRunner(verbosity=1).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(TernarySelectTests))
    failures = len(result.failures) + len(result.errors)
    return result.testsRun - failures, failures


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build" / "bwslc")
    args = parser.parse_args()
    _, failures = run_ternary_select_tests(args.compiler)
    raise SystemExit(bool(failures))
