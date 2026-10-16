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
