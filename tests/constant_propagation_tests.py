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
OP_SWITCH = 251
OP_FADD = 129
OP_SDIV = 135
OP_MATRIX_TIMES_VECTOR = 145
OP_DOT = 148


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

        def test_constant_switch_keeps_one_case(self):
            # Older DXC rejects `switch (2)`, so a constant selector must not
            # reach HLSL as a switch.
            ops, target = self.fragment_ops('''
                int mode = 2;
                float weight = input.uv.x;
                switch (mode) {
                    case 1: weight = 0.25; break;
                    case 2: weight = weight * 3.0; break;
                    default: break;
                }
                output.color = float4(weight);
            ''')
            self.assertNotIn(OP_SWITCH, ops)
            self.assertNotIn(OP_PHI, ops)
            self.assertNotIn("switch", (target / "constprop.frag.hlsl").read_text(encoding="utf-8"))

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

        def test_bitwise_identities_fold(self):
            ops, target = self.fragment_ops('''
                bool enabled = true;
                bool off = false;
                int mask = -1;
                bool wide = input.uv.x > 0.5;
                int bits = (int(input.uv.y * 8.0) & mask) | 0;
                float v = 0.0;
                if ((wide && enabled) || off) { v = 1.0; }
                if (wide && off) { v = 2.0; }
                output.color = float4(v, float(bits), 0.0, 1.0);
            ''')
            self.assertEqual(ops.count(OP_BRANCH_CONDITIONAL), 1)
            glsl = (target / "constprop.glsl.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            self.assertNotIn("true", body)
            self.assertNotIn("false", body)
            self.assertNotIn("&", body)
            self.assertNotIn("|", body)
            self.assertNotIn("2.0", body)

        def test_loop_in_inlined_function_has_plain_condition(self):
            # An inlined function's loop also checks the function's return
            # flag, which is a constant `false` when the loop can't return.
            target = self.compile(shader('''
                output.color = float4(sum5(input.uv.x));
            ''', '''
                sum5 :: (float x) -> float {
                    float s = 0.0;
                    for (int i = 0; i < 5; i++) { s += x * float(i); }
                    return s;
                }
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            self.assertNotIn("true", glsl)

        def test_vector_and_matrix_constants_fold(self):
            ops, target = self.fragment_ops('''
                mat3 m = mat3(float3(1.0, 2.0, 3.0), float3(4.0, 5.0, 6.0), float3(7.0, 8.0, 9.0));
                float3 a = float3(2.0) * m[0] - float3(0.5) * m[2];
                float3 b = m * float3(1.0, 0.0, 2.0);
                float d = dot(a, b);
                float3 c = float3(0.25, 0.5, 0.75).zyx + a.xxy;
                output.color = float4(a * input.uv.x + c, d);
            ''')
            self.assertNotIn(OP_MATRIX_TIMES_VECTOR, ops)
            self.assertNotIn(OP_DOT, ops)
            glsl = (target / "constprop.glsl.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            self.assertIn("vec3(-1.5, 0.0, 1.5)", body)
            self.assertIn("vec3(-0.75, -1.0, 0.25)", body)
            self.assertIn("9.0", body)
            self.assertNotIn("mat3", body)

        def test_intrinsics_fold(self):
            target = self.compile(shader('''
                float a = clamp(50.0, 0.0, 100.0) + float(max(0, 10)) + float(clamp(50, 0, 100));
                float b = sqrt(16.0) + pow(2.0, 3.0) + exp2(3.0) + log2(8.0) + abs(-1.5);
                float c = lerp(1.0, 3.0, 0.25) + smoothstep(0.0, 1.0, 0.5) + step(0.5, 0.75);
                float d = length(float2(3.0, 4.0)) + distance(float3(1.0, 2.0, 3.0), float3(1.0, 2.0, 5.0));
                float3 n = normalize(float3(3.0, 0.0, 4.0));
                float3 r = reflect(float3(1.0, -1.0, 0.0), float3(0.0, 1.0, 0.0));
                float e = determinant(mat2(float2(2.0, 0.0), float2(1.0, 3.0))) + float(count_bits(255u));
                float3 s = saturate(float3(-1.0, 0.5, 2.0)) + pow(float3(1.0, 2.0, 3.0), 2.0);
                bool f = any(bool2(false, true)) && !isnan(2.0);
                output.color = float4(a + input.uv.x, b + c, d + e, f ? 1.0 : 0.0) + float4(n + r + s, 0.0);
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            for call in ("clamp", "max", "sqrt", "pow", "exp2", "log2", "abs", "mix", "smoothstep",
                         "step", "length", "distance", "normalize", "reflect", "determinant",
                         "bitCount", "any", "isnan"):
                self.assertNotIn(call + "(", body)
            self.assertIn("110.0 + v_uv.x", body)           # 50 + 10 + 50
            self.assertIn("27.5", body)                     # 4 + 8 + 8 + 3 + 1.5 + 1.5 + 0.5 + 1
            # d + e = 5 + 2 + 6 + 8, and n + r + s = (0.6 + 1 + 1, 0 + 1 + 4.5, 0.8 + 0 + 10)
            self.assertIn("21.0", body)
            self.assertIn("vec4(2.599999904632568359375, 5.5, 10.8000001907348632812", body)

        def test_intrinsics_outside_their_domain_stay(self):
            target = self.compile(shader('''
                float a = sqrt(-1.0) + log(0.0) + pow(-2.0, 0.5) + asin(2.0);
                float b = smoothstep(1.0, 0.0, 0.5);
                float3 n = normalize(float3(0.0));
                uint high = first_bit_high(8u);
                output.color = float4(a, b, float(high), 1.0) + float4(n, input.uv.x);
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            for call in ("sqrt(", "log(", "pow(", "asin(", "smoothstep(", "normalize(", "findMSB("):
                self.assertIn(call, body)

        def test_loops_with_constant_inputs_fold(self):
            # A user function's loop, nested loops, and a loop whose bound is
            # another loop's result.
            target = self.compile(shader('''
                float p = poly(2.0);
                int grid = 0;
                for (int x = 0; x < 3; x++) {
                    for (int y = 0; y <= x; y++) { grid += x * y; }
                }
                float powers = 1.0;
                for (int i = 0; i < grid; i++) { powers *= 1.5; }
                output.color = float4(p, float(grid), powers, input.uv.x);
            ''', '''
                poly :: (float x) -> float {
                    float r = 0.0;
                    for (int i = 0; i < 3; i++) { r = r * x + 1.0; }
                    return r;
                }
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            # grid = 0 + (0 + 1) + (0 + 2 + 4) = 7, and 1.5^7 = 17.0859375.
            self.assertIn("vec4(7.0, 7.0, 17.0859375, v_uv.x)", body)
            self.assertNotIn("for (", body)
            self.assertNotIn("while", body)

        def test_loops_that_run_at_most_once_are_removed(self):
            # A condition that's false on entry, a body that always breaks,
            # and a loop left by evaluation that exits through a break. The
            # first two have run-time values and side effects in them.
            target = self.compile(shader('''
                float r = input.uv.x;
                for (int i = 0; i < 0; i++) { r = r * 2.0 + 1.0; output.color = float4(r); }
                for (int i = 0; i < 10; i++) { r += input.uv.y; break; }
                int found = -1;
                for (k in 0..100) {
                    if (k * k > 50) { found = k; break; }
                }
                output.color = float4(r, float(found), 0.0, 1.0);
            '''), "-glsl", "-gles", "-gles-direct", "-wgsl")
            for name in ("constprop.glsl.frag", "constprop.gles.frag", "constprop.frag.wgsl"):
                text = (target / name).read_text(encoding="utf-8")
                body = text[text.index("main"):]
                self.assertNotIn("for (", body, name)
                self.assertNotIn("while", body, name)
                self.assertNotIn("loop", body, name)
                self.assertNotIn("* 2.0", body, name)
                self.assertIn("8.0", body, name)

        def test_loops_that_can_repeat_stay(self):
            # The body breaks on one path only.
            target = self.compile(shader('''
                float r = 0.0;
                for (int i = 0; i < 4; i++) {
                    if (input.uv.x > 0.5) { break; }
                    r += input.uv.y;
                }
                output.color = float4(r);
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            self.assertIn("for (", glsl[glsl.index("void main"):])

        def test_loops_with_run_time_inputs_stay(self):
            target = self.compile(shader('''
                float r = 0.0;
                for (int i = 0; i < int(input.uv.x * 4.0); i++) { r = r * 2.0 + 1.0; }
                float s = input.uv.y;
                for (int i = 0; i < 3; i++) { s = s * 2.0 + 1.0; }
                output.color = float4(r, s, 0.0, 1.0);
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            self.assertEqual(body.count("* 2.0) + 1.0"), 2)

        def test_long_loops_are_left_to_run_time(self):
            # Past the evaluation budget, and a loop that never ends: both
            # compile, and stay loops.
            target = self.compile(shader('''
                int n = 0;
                for (int i = 0; i < 1000000; i++) { n += i & 3; }
                int m = 0;
                for (int i = 0; i >= 0; i += 0) { m += 1; }
                output.color = float4(float(n), float(m), input.uv);
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            self.assertIn("1000000", body)
            self.assertIn("& 3", body)

        def test_algebraic_identities_simplify(self):
            target = self.compile(shader('''
                float x = input.uv.x;
                float3 v = float3(input.uv, x);
                int i = int(input.uv.y * 8.0);
                float a = x * 1.0 + 0.0;
                float b = (0.0 - x) / 4.0;
                float3 c = v * float3(1.0) / -1.0;
                float d = pow(x, 2.0) + pow(x, 0.5) + pow(x, -1.0) + pow(x, 1.0) + pow(x, 0.0);
                float3 e = pow(v, float3(-0.5));
                int f = (i + 0) * 1 - 0 + (i << 0) + (i ^ i) + (i * -1) + (i - i) + i / 1;
                float g = -(-x) + min(x, x) + max(x, x);
                output.color = float4(a + b + d + g, float(f), 0.0, 1.0) + float4(c + e, 0.0);
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            for gone in ("* 1.0", "+ 0.0", "0.0 -", "/ 4.0", "vec3(1.0)", "vec3(-1.0)", "pow(",
                         "min(", "max(", "-(-", "+ 0)", "* 1)", "- 0)", "<< 0", "^", "* (-1)", "/ 1)"):
                self.assertNotIn(gone, body)
            for kept in ("* 0.25", "v_uv.x * v_uv.x", "sqrt(v_uv.x)", "1.0 / v_uv.x",
                         "inversesqrt(", "(-_"):
                self.assertIn(kept, body)

        def test_identities_that_need_finite_operands_stay(self):
            # x * 0 is NaN for an infinite x, x - x too, and x / 3 isn't
            # exactly x * (1 / 3). pow(x, 2.5) isn't a product, and
            # pow(x, 17.0) is past the longest chain.
            target = self.compile(shader('''
                float x = input.uv.x;
                output.color = float4(x * 0.0, x - x, x / 3.0, pow(x, 2.5) + pow(x, 17.0));
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            for kept in ("* 0.0", "v_uv.x - v_uv.x", "/ 3.0", "pow(v_uv.x, 2.5)", "pow(v_uv.x, 17.0)"):
                self.assertIn(kept, body)

        def test_integer_powers_become_products(self):
            # x^5 = (x^2)^2 * x, x^-3 = 1 / x^3, and a vector power; the
            # one in the `if` arm is inserted inside that arm.
            target = self.compile(shader('''
                float x = input.uv.x;
                float3 v = float3(input.uv, x);
                float c = 0.0;
                if (x > 0.5) { c = pow(1.0 - x, 5.0); }
                output.color = float4(pow(x, 5.0), pow(x, -3.0), c, 1.0) + float4(pow(v, float3(4.0)), 0.0);
            '''), "-glsl", "-wgsl", "-gles", "-gles-direct")
            glsl = (target / "constprop.glsl.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            self.assertNotIn("pow(", body)
            self.assertIn("1.0 / ((v_uv.x * v_uv.x) * v_uv.x)", body)
            arm = body[body.index("if ("):body.index("else")]
            self.assertEqual(arm.count(" * "), 3)
            for name in ("constprop.frag.wgsl", "constprop.gles.frag"):
                text = (target / name).read_text(encoding="utf-8")
                self.assertNotIn("pow(", text[text.index("main"):], name)

        def test_vectors_with_runtime_components_stay(self):
            ops, target = self.fragment_ops('''
                mat2 m = mat2(float2(1.0, 2.0), float2(3.0, input.uv.x));
                float2 v = m * float2(1.0, 1.0);
                output.color = float4(v, dot(v, float2(2.0, 0.5)), 1.0);
            ''')
            self.assertIn(OP_MATRIX_TIMES_VECTOR, ops)
            self.assertIn(OP_DOT, ops)

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
