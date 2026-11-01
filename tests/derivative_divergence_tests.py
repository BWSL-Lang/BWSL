"""BWSL1302: derivatives in control flow a 2x2 quad may take differently.

Derivatives and implicit-LOD sampling are undefined in quads whose pixels
take different paths. The warning fires where a branch depends on a value
that can differ within a quad, and stays quiet for uniforms, constants and
flat inputs, which are the same for every pixel of a primitive.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

CODE = "BWSL1302"

PIPELINE = '''pipeline Divergence {
    attributes { position: float3 texcoord: float2 tint: float4 }
    resources { tex: texture2D params: Params }
    struct Params { float threshold; int count; }
    struct Frag { float2 uv; float4 tint; }
    fill :: (Frag^ f, float2 uv, float4 tint) -> void { f^.uv = uv; f^.tint = tint; }
    DECLARATIONS
    pass "Main" {
        use attributes { position, texcoord, tint }
        use resources { tex, params }
        vertex {
            output.position = float4(attributes.position, 1.0);
            output.uv = attributes.texcoord;
            @flat output.tint = attributes.tint;
        }
        fragment {
FRAGMENT
        }
    }
}
'''


def run_derivative_divergence_tests(compiler: Path) -> tuple[int, int]:
    compiler = Path(compiler).resolve()

    class DerivativeDivergenceTests(unittest.TestCase):
        def setUp(self):
            self.temp = tempfile.TemporaryDirectory(prefix="bwsl-divergence-")
            self.addCleanup(self.temp.cleanup)
            self.folder = Path(self.temp.name)

        def warnings(self, fragment, declarations=""):
            source = self.folder / "divergence.bwsl"
            text = PIPELINE.replace("DECLARATIONS", declarations).replace("FRAGMENT", fragment)
            source.write_text(text, encoding="utf-8")
            result = subprocess.run(
                [str(compiler), str(source), "-check", "-errors-json"],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads(result.stdout)
            return [d for d in report["diagnostics"] if d["code"] == CODE]

        def assert_warns(self, fragment, intrinsic="sample"):
            found = self.warnings(fragment)
            self.assertEqual(len(found), 1, found)
            self.assertEqual(found[0]["severity"], "warning")
            self.assertEqual(found[0]["stage"], "fragment")
            self.assertTrue(found[0]["message"].startswith(intrinsic + "() runs in control flow"),
                            found[0]["message"])
            self.assertIn("(the branch at line", found[0]["message"])

        def assert_quiet(self, fragment):
            self.assertEqual(self.warnings(fragment), [])

        def test_branch_on_interpolated_input(self):
            self.assert_warns('''float4 c = float4(0.0);
                if (input.uv.x > 0.5) { c = sample(resources.tex, input.uv); }
                output.color = c;''')

        def test_fwidth_in_branch_on_texture_value(self):
            self.assert_warns('''float h = sample_lod(resources.tex, input.uv, 0.0).r;
                float w = 0.0;
                if (h > 0.5) { w = fwidth(h); }
                output.color = float4(w);''', "fwidth")

        def test_early_return(self):
            self.assert_warns('''output.color = float4(0.0);
                if (input.uv.x > 0.5) { return; }
                output.color = sample(resources.tex, input.uv);''')

        def test_value_chosen_by_divergent_branch(self):
            self.assert_warns('''float k = 0.0;
                if (input.uv.y > 0.5) { k = 1.0; }
                float4 c = float4(0.0);
                if (k > 0.5) { c = sample(resources.tex, input.uv); }
                output.color = c;''')

        def test_loop_count_from_interpolated_input(self):
            self.assert_warns('''float4 c = float4(0.0);
                for (int i = 0; i < int(input.uv.x * 8.0); i++) {
                    c += sample(resources.tex, input.uv + float2(float(i) * 0.01, 0.0));
                }
                output.color = c;''')

        def test_break_on_interpolated_input(self):
            self.assert_warns('''float4 c = float4(0.0);
                for (int i = 0; i < 4; i++) {
                    if (input.uv.x > float(i) * 0.25) { break; }
                    c += sample(resources.tex, input.uv);
                }
                output.color = c;''')

        def test_struct_field_written_through_pointer(self):
            self.assert_warns('''Frag f;
                fill(^f, input.uv, input.tint);
                float4 c = float4(0.0);
                if (f.uv.x > 0.5) { c = sample(resources.tex, f.uv); }
                output.color = c;''')

        def test_flat_input_branch_is_quiet(self):
            self.assert_quiet('''float4 c = float4(0.0);
                if (input.tint.a > 0.5) { c = sample(resources.tex, input.uv); }
                output.color = c;''')

        def test_flat_struct_field_is_quiet(self):
            # The struct also holds a varying field; only the one branched on counts.
            self.assert_quiet('''Frag f;
                fill(^f, input.uv, input.tint);
                float4 c = float4(0.0);
                if (f.tint.a > 0.5) { c = sample(resources.tex, f.uv); }
                output.color = c;''')

        def test_uniform_branch_and_loop_are_quiet(self):
            self.assert_quiet('''float4 c = float4(0.0);
                if (resources.params.threshold > 0.5) { c = sample(resources.tex, input.uv); }
                for (int i = 0; i < resources.params.count; i++) {
                    c += sample(resources.tex, input.uv + float2(float(i) * 0.01, 0.0));
                }
                output.color = c;''')

        def test_sample_at_constant_coordinates_is_uniform(self):
            self.assert_quiet('''float4 c = float4(0.0);
                if (sample_lod(resources.tex, float2(0.5), 0.0).r > 0.5) {
                    c = sample(resources.tex, input.uv);
                }
                output.color = c;''')

        def test_discard_keeps_quads_together(self):
            # A discarded pixel stays a helper for its quad's derivatives.
            self.assert_quiet('''if (input.uv.x > 0.5) { discard; }
                output.color = sample(resources.tex, input.uv);''')

        def test_derivative_after_the_branch_is_quiet(self):
            self.assert_quiet('''float v = input.uv.x;
                if (input.uv.y > 0.5) { v = v * 2.0; }
                output.color = float4(ddx(v));''')

        def test_explicit_level_is_quiet(self):
            self.assert_quiet('''float4 c = float4(0.0);
                if (input.uv.x > 0.5) { c = sample_lod(resources.tex, input.uv, 0.0); }
                output.color = c;''')

        def test_one_warning_per_source_location(self):
            # An inlined helper's sample is reported once, not per call.
            found = self.warnings('''float4 c = float4(0.0);
                if (input.uv.x > 0.5) { c = look(input.uv) + look(input.uv * 0.5); }
                output.color = c;''', "look :: (float2 uv) -> float4 { return sample(resources.tex, uv); }")
            self.assertEqual(len(found), 1, found)

    result = unittest.TextTestRunner(verbosity=1).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(DerivativeDivergenceTests))
    failures = len(result.failures) + len(result.errors)
    return result.testsRun - failures, failures


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build" / "bwslc")
    args = parser.parse_args()
    _, failures = run_derivative_divergence_tests(args.compiler)
    raise SystemExit(bool(failures))
