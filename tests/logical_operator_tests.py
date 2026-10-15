"""Issue #157: `&&`/`||` with a pure right operand lower without branches."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

from local_debug_names_tests import instructions, shader

OP_LOGICAL_OR = 166
OP_LOGICAL_AND = 167
OP_BRANCH_CONDITIONAL = 250


def run_logical_operator_tests(compiler: Path) -> tuple[int, int]:
    compiler = Path(compiler).resolve()

    class LogicalOperatorTests(unittest.TestCase):
        def setUp(self):
            self.temp = tempfile.TemporaryDirectory(prefix="bwsl-logical-")
            self.addCleanup(self.temp.cleanup)
            self.folder = Path(self.temp.name)

        def compile(self, body):
            source = self.folder / "logical.bwsl"
            source.write_text(shader(body), encoding="utf-8")
            target = self.folder / "output"
            result = subprocess.run(
                [str(compiler), str(source), "-all", "-o", str(target)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return [op for op, _ in instructions(target / "logical.frag.spv")]

        def test_pure_or_chain_is_one_branch(self):
            ops = self.compile('''
                float2 p = input.position.xy;
                if (p.x < 10.0 || p.x > 20.0 || p.y < 10.0 || p.y > 20.0) {
                    discard;
                }
                output.color = float4(1.0);
            ''')
            self.assertEqual(ops.count(OP_BRANCH_CONDITIONAL), 1)
            self.assertEqual(ops.count(OP_LOGICAL_OR), 3)

        def test_pure_and_of_bools_and_negation(self):
            ops = self.compile('''
                bool a = input.uv.x > 0.5;
                bool b = input.uv.y > 0.5;
                output.color = float4((a && !b && input.uv.x != input.uv.y) ? 1.0 : 0.0);
            ''')
            self.assertEqual(ops.count(OP_LOGICAL_AND), 2)

        def test_side_effect_keeps_short_circuit(self):
            ops = self.compile('''
                int n = 0;
                bool b = input.uv.x < 0.0;
                bool a = b && ++n > 0;
                output.color = float4(float(n), a ? 1.0 : 0.0, 0.0, 1.0);
            ''')
            self.assertNotIn(OP_LOGICAL_AND, ops)
            self.assertIn(OP_BRANCH_CONDITIONAL, ops)

        def test_guarded_index_keeps_short_circuit(self):
            ops = self.compile('''
                float[4] a;
                for (int k = 0; k < 4; k++) { a[k] = float(k); }
                int i = int(input.uv.x * 8.0);
                output.color = float4((i < 4 && a[i] > 1.0) ? 1.0 : 0.0);
            ''')
            self.assertNotIn(OP_LOGICAL_AND, ops)

        def test_guarded_division_keeps_short_circuit(self):
            ops = self.compile('''
                int n = int(input.uv.x * 4.0);
                output.color = float4((n != 0 && 100 / n > 2) ? 1.0 : 0.0);
            ''')
            self.assertNotIn(OP_LOGICAL_AND, ops)

    result = unittest.TextTestRunner(verbosity=1).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(LogicalOperatorTests))
    failures = len(result.failures) + len(result.errors)
    return result.testsRun - failures, failures


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build" / "bwslc")
    args = parser.parse_args()
    _, failures = run_logical_operator_tests(args.compiler)
    raise SystemExit(bool(failures))
