"""Issue #152: one guard for the statements after an inlined early return."""
from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import subprocess
import tempfile
import unittest

from local_debug_names_tests import instructions, shader

OP_BRANCH_CONDITIONAL = 250


def run_inline_return_tests(compiler: Path) -> tuple[int, int]:
    compiler = Path(compiler).resolve()

    class InlineReturnTests(unittest.TestCase):
        def setUp(self):
            self.temp = tempfile.TemporaryDirectory(prefix="bwsl-inline-return-")
            self.addCleanup(self.temp.cleanup)
            self.folder = Path(self.temp.name)

        def compile(self, body, declarations=""):
            source = self.folder / "returns.bwsl"
            source.write_text(shader(body, declarations), encoding="utf-8")
            target = self.folder / "output"
            result = subprocess.run(
                [str(compiler), str(source), "-gles", "-spv", "-o", str(target)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return list(instructions(target / "returns.frag.spv"))

        def assert_no_condition_reused(self, ops):
            conditions = Counter(args[0] for op, args in ops if op == OP_BRANCH_CONDITIONAL)
            reused = {id: count for id, count in conditions.items() if count > 1}
            self.assertEqual(reused, {}, "values branched on more than once")

        def test_statements_after_return_share_one_guard(self):
            ops = self.compile('''
                uint2 cell = uint2(input.uv * 8.0);
                output.color = float4(isSet(cell, uint4(1u, 2u, 3u, 4u), 8.0) ? 1.0 : 0.0);
            ''', '''
                isSet :: (uint2 cell, uint4 bits, float size) -> bool {
                    if (cell.x >= uint(size) || cell.y >= uint(size)) {
                        return false;
                    }
                    uint index = uint(size) * cell.y + cell.x;
                    uint word;
                    if (index < 32u) {
                        word = bits.x;
                    } else {
                        word = bits.y;
                    }
                    uint bit = index % 32u;
                    return (word & (1u << bit)) > 0u;
                }
            ''')
            self.assert_no_condition_reused(ops)

        def test_second_return_nests_one_more_guard(self):
            ops = self.compile('''
                output.color = float4(steps(input.uv.x));
            ''', '''
                steps :: (float v) -> float {
                    if (v < 0.25) {
                        return -1.0;
                    }
                    float a = v * 2.0;
                    float b = a + 1.0;
                    if (b > 2.5) {
                        return b;
                    }
                    float c = b * b;
                    c += a;
                    return c;
                }
            ''')
            self.assert_no_condition_reused(ops)

    result = unittest.TextTestRunner(verbosity=1).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(InlineReturnTests))
    failures = len(result.failures) + len(result.errors)
    return result.testsRun - failures, failures


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build" / "bwslc")
    args = parser.parse_args()
    _, failures = run_inline_return_tests(args.compiler)
    raise SystemExit(bool(failures))
