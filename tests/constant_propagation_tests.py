"""Constant propagation folds values through branches, phis and reassignment."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

from local_debug_names_tests import instructions, names, shader

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
                output.color = float4(a, b, 0.0, 1.0) + float4(n, input.uv.x);
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            for call in ("sqrt(", "log(", "pow(", "asin(", "smoothstep(", "normalize("):
                self.assertIn(call, body)

        def test_bit_indices_fold(self):
            # first_bit_high/low give the bit's index or -1 when there's none;
            # for a negative int, the highest bit that differs from the sign (#189).
            target = self.compile(shader('''
                int h = int(first_bit_high(8u));     // 3
                int hz = int(first_bit_high(0u));    // -1
                int hs = first_bit_high(-8);         // 2
                int hm = first_bit_high(-1);         // -1
                int l = int(first_bit_low(12u));     // 2
                int lz = first_bit_low(0);           // -1
                int3 hv = first_bit_high(int3(-8, 0, 1024));
                output.color = float4(float(h * 1000 + hs * 100 + l), float(hz + hm + lz),
                                      float(hv.x + hv.y + hv.z), input.uv.x);
            '''), "-glsl", "-wgsl", "-gles", "-gles-direct")
            for name, folded in (("constprop.glsl.frag", "vec4(3202.0, -3.0, 11.0, v_uv.x)"),
                                 ("constprop.gles.frag", "vec4(3202.0, -3.0, 11.0, "),
                                 ("constprop.frag.wgsl", "vec4<f32>((3202.0f), (-3.0f), (11.0f), ")):
                text = (target / name).read_text(encoding="utf-8")
                self.assertIn(folded, text)
                for call in ("findMSB(", "findLSB(", "bwsl_first_bit", "firstLeadingBit(", "firstTrailingBit("):
                    self.assertNotIn(call, text)

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
            # x^-3 reuses the x * x that x^5 squares (common subexpressions).
            self.assertRegex(body, r"1\.0 / \((_\d+|\(v_uv\.x \* v_uv\.x\)) \* v_uv\.x\)")
            arm = body[body.index("if ("):body.index("else")]
            self.assertEqual(arm.count(" * "), 3)
            for name in ("constprop.frag.wgsl", "constprop.gles.frag"):
                text = (target / name).read_text(encoding="utf-8")
                self.assertNotIn("pow(", text[text.index("main"):], name)

        def test_known_components_of_vectors_fold(self):
            # Only x varies: the other components fold wherever they're
            # read, through a swizzle, an insert, a matrix column and a
            # select whose arms agree on y.
            target = self.compile(shader('''
                float x = input.uv.x;
                float4 v = float4(x, 2.0, 3.0, x);
                v.w = 4.0;
                mat2 m = mat2(float2(5.0, x), float2(6.0, 7.0));
                float3 s = select(float3(x, 1.0, 2.0), float3(-x, 1.0, 3.0), bool3(x > 0.5));
                output.color = float4(v.yzw.x * v.zy.x + v.w, m[0].x * m[1].y, s.y, s.z);
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            self.assertIn("vec4(10.0, 35.0, 1.0, ", body)
            self.assertNotIn("vec4(v_uv.x, 2.0", body)
            self.assertNotIn("mat2", body)

        def test_components_a_pointer_can_change_stay(self):
            # a's components are constants where it's built, but a write
            # through p can change a before a.x is read.
            target = self.compile(shader('''
                float3 a = float3(1.0, 2.0, 3.0);
                float3 b = float3(4.0, 5.0, 6.0);
                float3^ p = ^a;
                if (input.uv.x > 0.5) { p = ^b; }
                p^ = p^ + float3(input.uv.y);
                output.color = float4(a.x, b.x, 0.0, 1.0);
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            self.assertNotIn("vec4(1.0, 4.0", body)

        def test_hsluv_with_one_varying_component(self):
            # The conversion depends on each component in different places;
            # what depends only on the constant ones folds.
            modules = str(compiler.parent.parent / "modules")
            cases = {
                "float3(x, 100.0, 50.0)": (["radians(v_uv.x)"], ["clamp("]),
                "float3(180.0, x, 50.0)": ([], ["radians(", "clamp(", "min("]),
                "float3(10.0, 100.0, x)": (["clamp(v_uv.x"], ["radians(", "cos(", "sin("]),
            }
            for argument, (kept, gone) in cases.items():
                with self.subTest(argument=argument):
                    target = self.compile(shader(f'''
                        float x = input.uv.x;
                        output.color = float4(Color::hsluvToRgb({argument}), 1.0);
                    ''', "import Color"), "-glsl", "-modules", modules)
                    glsl = (target / "constprop.frag").read_text(encoding="utf-8")
                    body = glsl[glsl.index("void main"):]
                    self.assertNotIn("vec3(" + argument.replace("float3(", "").replace("x", "v_uv.x"), body)
                    for text in kept:
                        self.assertIn(text, body)
                    for text in gone:
                        self.assertNotIn(text, body)

        def test_repeated_calculations_are_computed_once(self):
            target = self.compile(shader('''
                float x = input.uv.x;
                float y = input.uv.y;
                float a = sin(x * 2.0) + sin(x * 2.0);
                float b = (x + y) * (y + x);
                float3 v = normalize(float3(x, y, 1.0)) + normalize(float3(x, y, 1.0));
                output.color = float4(a, b, v.x, v.y);
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            self.assertEqual(body.count("sin("), 1)
            self.assertEqual(body.count("* 2.0"), 1)
            self.assertEqual(body.count("v_uv.x + v_uv.y") + body.count("v_uv.y + v_uv.x"), 1)
            self.assertEqual(body.count("normalize("), 1)

        def test_common_subexpressions_follow_dominance(self):
            # x * 3.0 before the branch serves both arms; each arm's x * 5.0
            # is its own, since neither arm runs before the other.
            target = self.compile(shader('''
                float x = input.uv.x;
                float before = x * 3.0;
                float r;
                if (input.uv.y > 0.5) { r = x * 3.0 + x * 5.0; } else { r = x * 3.0 - x * 5.0; }
                output.color = float4(before, r, 0.0, 1.0);
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            self.assertEqual(body.count("* 3.0"), 1)
            self.assertEqual(body.count("* 5.0"), 2)

        def test_reassigned_operands_are_not_merged(self):
            # A single-block stage keeps one register for x; after x changes,
            # x * 2.0 is a different value.
            target = self.compile(shader('''
                float x = input.uv.x;
                float a = x * 2.0;
                x = x + 1.0;
                float b = x * 2.0;
                output.color = float4(a, b, 0.0, 1.0);
            '''), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            self.assertEqual(body.count("* 2.0"), 2)

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

        def bounded_main(self, body):
            target = self.compile('''pipeline Bounds {
                struct Params {
                    @bounds(>0, <10) int count;
                    @bounds(x>=0, y<=1) float2 weights;
                    float free;
                    uint bits;
                }
                attributes {
                    position: float3
                    uv: float2 @bounds(>=0, <=1)
                }
                resources {
                    params: Params
                    @bounds(>=0, <=1) fade: float
                }
                pass "Main" {
                    use attributes { position, uv }
                    use resources { params, fade }
                    vertex {
                        output.position = float4(attributes.position, 1.0);
                        output.uv = attributes.uv;
                    }
                    fragment {
                        float f = resources.fade;
                        float2 w = resources.params.weights;
                        float x = resources.params.free;
                        int n = resources.params.count;
                        ''' + body + '''
                    }
                }
            }''', "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            return glsl[glsl.index("void main"):]

        def test_bounds_remove_calls_that_cant_change_the_value(self):
            body = self.bounded_main('''
                float a = clamp(f, 0.0, 1.0) + saturate(f) + step(0.0, f) + abs(f - 2.0);
                int m = clamp(n, 1, 9) + min(n, 100);
                output.color = float4(a, float(m), max(f, 0.0), min(f, 2.0));
            ''')
            for call in ("clamp(", "min(", "max(", "step(", "abs("):
                self.assertNotIn(call, body)

        def test_bounds_decide_branches(self):
            body = self.bounded_main('''
                float r = f;
                if (n >= 10) { r += 1000.0; }
                if (f > 1.0 || n < 1) { r += 2000.0; }
                output.color = float4(r);
            ''')
            self.assertNotIn("1000.0", body)
            self.assertNotIn("2000.0", body)
            self.assertNotIn("if (", body)

        def test_bounds_per_component(self):
            # x is at least 0 and y at most 1; nothing else is known.
            body = self.bounded_main('''
                output.color = float4(max(w.x, 0.0), min(w.y, 1.0), max(w.y, 0.0), min(w.x, 1.0));
            ''')
            self.assertNotIn("max(bwsl_ub_params.bwsl_u_params.weights.x", body)
            self.assertNotIn("min(bwsl_ub_params.bwsl_u_params.weights.y", body)
            self.assertIn("max(bwsl_ub_params.bwsl_u_params.weights.y, 0.0)", body)
            self.assertIn("min(bwsl_ub_params.bwsl_u_params.weights.x, 1.0)", body)

        def test_branches_narrow_ranges(self):
            # x > 0.5 holding means x is a number above 0.5; where it
            # doesn't, x may be NaN.
            body = self.bounded_main('''
                float r = 0.0;
                if (x > 0.5) { r = max(x, 0.25); } else { r = min(x, 0.75); }
                output.color = float4(r);
            ''')
            self.assertNotIn("max(", body)
            self.assertIn("min(bwsl_ub_params.bwsl_u_params.free, 0.75)", body)

        def test_loop_counters_have_ranges(self):
            body = self.bounded_main('''
                float sum = 0.0;
                for (int i = 0; i < n; i++) {
                    if (i < 9) { sum += 1.0; }
                    sum += saturate(float(i) / 16.0);
                }
                output.color = float4(sum);
            ''')
            self.assertNotIn("min(", body)
            self.assertNotIn("clamp(", body)
            self.assertNotIn("< 9", body)
            self.assertIn("for (", body)

        def test_values_that_may_be_nan_keep_their_clamps(self):
            # Nothing bounds x, and x * f is NaN for an infinite x and f = 0.
            body = self.bounded_main('''
                output.color = float4(clamp(x, 0.0, 1.0), saturate(x * f), max(x, x), 1.0);
            ''')
            self.assertIn("clamp(bwsl_ub_params.bwsl_u_params.free, 0.0, 1.0)", body)
            self.assertIn("clamp(bwsl_ub_params.bwsl_u_params.free * ", body)

        def test_assigned_values_have_ranges(self):
            # No @bounds: the ranges come from the constants assigned and
            # from the operations.
            body = self.bounded_main('''
                float s = 2.0;
                if (x > 0.5) { s = 5.0; }
                int k = int(resources.params.bits & 3u);
                float t = float(k) * 0.5;
                output.color = float4(clamp(s, 0.0, 10.0), float(min(k, 3)), max(t, 0.0), min(t, 2.0));
            ''')
            self.assertNotIn("min(", body)
            self.assertNotIn("max(", body)
            self.assertNotIn("clamp(", body)

        def test_compute_builtins_have_ranges(self):
            # local_id is below the workgroup size [8, 2, 1], local_index
            # below 16, and a dispatch has at least one workgroup.
            target = self.compile('''pipeline Builtins {
                resources { data: buffer<uint> }
                pass "Run" {
                    use resources { data }
                    compute "Main" [8, 2, 1] {
                        uint3 l = input.local_id;
                        uint a = min(l.x, 7u) + l.z + min(input.local_index, 15u) + max(input.num_workgroups.x, 1u);
                        uint b = min(l.x, 6u) + min(input.local_index, 14u);
                        if (l.y >= 2u) { a += 1000u; }
                        resources.data[input.global_id.x] = a + b;
                    }
                }
            }''', "-glsl")
            glsl = (target / "constprop.comp").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            self.assertNotIn("1000u", body)
            self.assertNotIn("gl_LocalInvocationID.z", body)
            self.assertNotIn("max(", body)
            self.assertEqual(body.count("min("), 2)
            self.assertIn("min(gl_LocalInvocationID.x, 6u)", body)
            self.assertIn("min(gl_LocalInvocationIndex, 14u)", body)

        def test_fragment_coordinates_are_numbers_in_the_framebuffer(self):
            # x and y are inside the framebuffer; depth and w can be anything.
            target = self.compile('''pipeline FragBuiltins {
                attributes { position: float3 }
                pass "Main" {
                    use attributes { position }
                    vertex { output.position = float4(attributes.position, 1.0); }
                    fragment {
                        float4 p = input.position;
                        output.color = float4(clamp(p.x, 0.0, 100000.0), max(p.y, 0.0), saturate(p.z), max(p.w, 0.0));
                    }
                }
            }''', "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            self.assertIn("gl_FragCoord.xy", body)
            self.assertIn("clamp(gl_FragCoord.z, 0.0, 1.0)", body)
            self.assertIn("max(gl_FragCoord.w, 0.0)", body)

        def test_remainders_of_values_that_arent_negative_use_native_operators(self):
            # GLSL's `%` isn't defined for negative operands, so a signed
            # remainder is expanded into a division unless the ranges show
            # neither operand is negative: x is a fragment coordinate, the
            # spacing 3 or 4 and n anything.
            target = self.compile('''pipeline NonNegMod {
                attributes { position: float2 }
                struct Params { float tall; int n; }
                resources { params: Params }
                pass "Main" {
                    use attributes { position }
                    use resources { params }
                    vertex { output.position = float4(attributes.position, 0.0, 1.0); }
                    fragment {
                        int x = int(input.position.x);
                        int n = resources.params.n;
                        int spacing = resources.params.tall > 0.0 ? 4 : 3;
                        int a = x % spacing + x % 8 + x / 8;
                        int b = (x - 100) % 8 + n % spacing + x % -4;
                        output.color = float4(float(a), float(b), float((x - 100) / 8), 1.0);
                    }
                }
            }''', "-glsl", "-gles-direct", "-metal", "-hlsl", "-wgsl")
            def body(name):
                text = (target / name).read_text(encoding="utf-8")
                return text[text.index("main"):]
            glsl, gles = body("constprop.glsl.frag"), body("constprop.gles.frag")
            metal, hlsl = body("constprop.frag.metal"), body("constprop.frag.hlsl")
            wgsl = body("constprop.frag.wgsl")
            # SPIRV-Cross emits OpUMod as a native `%`, OpSRem as a division.
            for text in (glsl, metal, hlsl):
                self.assertRegex(text, r"int\(uint\(_\d+\) % uint\(")
                self.assertRegex(text, r"\(_\d+ & 7\)")
                self.assertRegex(text, r"\(_\d+ >> 3\)")
                # Those that may see a negative operand keep the division.
                self.assertRegex(text, r"_\d+ - 8 \* \(_\d+ / 8\)")
                self.assertRegex(text, r"float\(_\d+ / 8\)")
                self.assertIn(" - (-4) * (", text)
                self.assertRegex(text, r"\.n - _\d+ \* \(")
            self.assertRegex(gles, r"= \(r\d+ % r\d+\);")
            self.assertRegex(gles, r"= \(r\d+ & 7\);")
            self.assertRegex(gles, r"= \(r\d+ >> 3\);")
            self.assertRegex(gles, r"= \(r\d+ - 8 \* \(r\d+ / 8\)\);")
            self.assertRegex(gles, r"= \(r\d+ - r\d+ \* \(r\d+ / r\d+\)\);")
            self.assertRegex(gles, r"= \(r\d+ - -4 \* \(r\d+ / -4\)\);")
            self.assertRegex(wgsl, r"& \(7i\)")

        def test_flat_varyings_keep_their_ranges(self):
            # A flat or integer varying is one vertex's value; an interpolated
            # one can leave its vertices' range under MSAA. Division isn't
            # correctly rounded on GPUs, so lit / 100.0 can exceed 1.0, while
            # lit * 0.01 can't.
            target = self.compile('''pipeline Stages {
                attributes {
                    position: float3
                    lit: float @bounds(>=0, <=100)
                    id: uint
                }
                pass "Main" {
                    use attributes { position, lit, id }
                    vertex {
                        output.position = float4(attributes.position, 1.0);
                        @flat output.light = attributes.lit * 0.01;
                        output.shade = saturate(attributes.position.x);
                        output.index = attributes.id & 7u;
                        output.ratio = min(1.0, attributes.lit / 100.0);
                    }
                    fragment {
                        output.color = float4(saturate(input.light), saturate(input.shade),
                                              float(min(input.index, 7u)), input.ratio);
                    }
                }
            }''', "-glsl")
            vertex = (target / "constprop.vert").read_text(encoding="utf-8")
            fragment = (target / "constprop.frag").read_text(encoding="utf-8")
            vertex = vertex[vertex.index("void main"):]
            fragment = fragment[fragment.index("void main"):]
            self.assertIn("min(1.0, ", vertex)
            self.assertNotIn("clamp(v_light", fragment)
            self.assertNotIn("min(v_index", fragment)
            self.assertIn("clamp(v_shade, 0.0, 1.0)", fragment)

        def test_vector_conversions_keep_ranges(self):
            # int2(q) converts each component, as int(q.x) does. q is in
            # [0, 65536 / 0.01].
            target = self.compile('''pipeline VectorConvertBounds {
                attributes { position: float2 }
                struct Params { @bounds(>=0.01) float t; };
                resources { params: Params }
                pass "Main" {
                    use attributes { position }
                    use resources { params }
                    vertex { output.position = float4(attributes.position, 0.0, 1.0); }
                    fragment {
                        float2 q = input.position.xy / resources.params.t;
                        int a = max(0, int(q.x));
                        int2 i = int2(q);
                        uint2 u = uint2(i);
                        output.color = float4(float(a), float(max(0, i.x)), float(min(u.y, 7000000u)), 1.0);
                    }
                }
            }''', "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            self.assertNotIn("max(", body)
            self.assertNotIn("min(", body)

        def test_struct_fields_carry_what_was_stored(self):
            # A field read back after a store in the same block is the value
            # stored. A store through a pointer may change any struct, so
            # what follows it reads memory again.
            target = self.compile('''pipeline StructBounds {
                attributes {
                    position: float2
                    color: float4 @bounds(0<=z<=100)
                }
                struct Inputs { float4 color; };
                pass "Main" {
                    use attributes { position, color }
                    vertex {
                        output.position = float4(attributes.position, 0.0, 1.0);
                        @flat output.vColor = attributes.color;
                    }
                    fragment {
                        Inputs i;
                        i.color = input.vColor;
                        float b = min(1.0, i.color.z * 0.01);
                        float w = i.color.w;
                        Inputs k;
                        k.color = float4(0.5);
                        Inputs^ p = ^k;
                        p^.color = float4(2.0);
                        output.color = float4(b, w, k.color.z, 1.0);
                    }
                }
            }''', "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            self.assertNotIn("min(", body)
            self.assertIn("v_vColor.w", body)
            self.assertNotIn("0.5, 1.0)", body)
            self.assertIn(".color.z, 1.0)", body)

        def test_struct_fields_carry_across_branches(self):
            # A field keeps the value stored before a discard, a branch that
            # writes another field and a loop that doesn't touch it. One a
            # branch or a loop may write is read from memory.
            target = self.compile('''pipeline BranchBounds {
                attributes {
                    position: float2
                    color: float4 @bounds(0<=z<=100)
                }
                struct Inputs { float4 color; float other; };
                pass "Main" {
                    use attributes { position, color }
                    vertex {
                        output.position = float4(attributes.position, 0.0, 1.0);
                        @flat output.vColor = attributes.color;
                    }
                    fragment {
                        Inputs i;
                        i.color = input.vColor;
                        i.other = 0.0;
                        if (input.position.x > 100.0) { discard; }
                        float a = min(1.0, i.color.z * 0.01);
                        if (input.position.y > 100.0) { i.other = 1.0; }
                        float sum = 0.0;
                        for (int m = 0; m < (int(input.position.y) & 7); m++) { sum += float(m); }
                        float b = min(1.0, i.color.z * 0.01) + sum;
                        Inputs k;
                        k.other = 2.0;
                        for (int n = 0; n < int(input.position.x); n++) { k.other = k.other * 0.5; }
                        output.color = float4(a, b, i.other, k.other);
                    }
                }
            }''', "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            body = glsl[glsl.index("void main"):]
            self.assertNotIn("min(", body)
            self.assertNotIn(", 0.0, 2.0)", body)
            self.assertRegex(body, r"_\d+\.other, _\d+\.other\)")

        def test_struct_fields_survive_writes_to_other_structs(self):
            # Separate structs are separate memory: initializing s and
            # writing its fields in a loop, directly or in a method, leaves
            # i.color as stored.
            for update in ("s.best = min(s.best, float(k) * s.radius);", "s.visit(float(k));"):
                with self.subTest(update=update):
                    target = self.compile('''pipeline OtherStructLoop {
                        attributes {
                            position: float2
                            color: float4 @bounds(0<=z<=100)
                        }
                        struct Inputs { float4 color; };
                        struct Sampler {
                            float best;
                            float radius;
                            visit :: (float d) -> void { self.best = min(self.best, d * self.radius); }
                        };
                        pass "Main" {
                            use attributes { position, color }
                            vertex {
                                output.position = float4(attributes.position, 0.0, 1.0);
                                @flat output.vColor = attributes.color;
                            }
                            fragment {
                                Inputs i;
                                i.color = input.vColor;
                                Sampler s;
                                s.best = 1000.0;
                                s.radius = 2.0;
                                for (int k = 0; k < (int(input.position.x) & 7); k++) { ''' + update + ''' }
                                output.color = float4(min(1.0, i.color.z * 0.01), s.best, 0.0, 1.0);
                            }
                        }
                    }''', "-glsl")
                    glsl = (target / "constprop.frag").read_text(encoding="utf-8")
                    body = glsl[glsl.index("void main"):]
                    self.assertNotIn("min(1.0", body)
                    self.assertRegex(body, r"min\(_\d+\.best, ")

        def mod_fold_glsl(self, body, declarations=""):
            target = self.compile(shader(body, declarations), "-glsl")
            glsl = (target / "constprop.frag").read_text(encoding="utf-8")
            return glsl[glsl.index("void main"):]

        def test_float_remainder_folds_like_runtime_mod(self):
            # Float `%` is OpFMod at runtime: x - y * floor(x / y). Parse-time
            # consts, eval code and constant propagation must all agree.
            body = self.mod_fold_glsl('''
                eval float a = 7.5 % -2.0;
                eval float b = -1.25;
                eval { b %= 1.0; }
                float c = -5.0 % 4.0;
                output.color = float4(K, a, b, c);
            ''', "const float K = -7.5 % 2.0;")
            self.assertIn("vec4(0.5, -0.5, 0.75, 3.0)", body)

        def test_fmod_intrinsic_still_truncates(self):
            body = self.mod_fold_glsl("output.color = float4(fmod(-7.25, 2.0));")
            self.assertIn("vec4(-1.25)", body)

        def test_values_that_only_read_each_other_are_removed(self):
            # A loop variable that only its own update and its phis read is
            # dead: the sum and the swizzled trail, and the hit index when
            # only the distance is written. Read by the output, it stays.
            def loop(output):
                return '''pipeline DeadCycles {
                    resources { spheres: buffer<float4> output: buffer<float> }
                    pass "Run" {
                        use resources { spheres, output }
                        compute "Main" [8, 1, 1] {
                            uint idx = input.global_id.x;
                            float best = 1000.0;
                            int bestIndex = -1;
                            float unusedSum = 0.0;
                            float3 trail = float3(0.0);
                            for (int k = 0; k < int(idx) + 4; k++) {
                                float4 s = resources.spheres[k];
                                float d = length(s.xyz) - s.w;
                                unusedSum += d;
                                trail = trail.yzx + s.xyz;
                                if (d > 0.0 && d < best) {
                                    best = d;
                                    bestIndex = k;
                                }
                            }
                            resources.output[idx] = ''' + output + ''';
                        }
                    }
                }'''
            cases = {
                "best": ({"best"}, {"bestIndex", "unusedSum", "trail"}),
                "best + float(bestIndex)": ({"best", "bestIndex"}, {"unusedSum", "trail"}),
                "best + unusedSum + trail.x": ({"best", "unusedSum", "trail"}, {"bestIndex"}),
            }
            for output, (kept, removed) in cases.items():
                with self.subTest(output=output):
                    target = self.compile(loop(output), "-debug-names")
                    present = set(names(instructions(target / "constprop.comp.spv")).values())
                    self.assertLessEqual(kept, present)
                    self.assertFalse(removed & present, removed & present)

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
