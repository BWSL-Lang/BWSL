"""-debug-info: source text, lines, inlined scopes and variables in the SPIR-V (#250)."""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import textwrap
import unittest

from local_debug_names_tests import instructions

OP_SOURCE = 3
OP_STRING = 7
OP_LINE = 8
OP_EXT_INST_IMPORT = 11
OP_EXT_INST = 12
OP_CONSTANT = 43

DEBUG_TYPE_ARRAY = 5
DEBUG_TYPE_COMPOSITE = 10
DEBUG_TYPE_MEMBER = 11
DEBUG_FUNCTION = 20
DEBUG_SCOPE = 23
DEBUG_INLINED_AT = 25
DEBUG_LOCAL_VARIABLE = 26
DEBUG_DECLARE = 28
DEBUG_VALUE = 29
DEBUG_SOURCE = 35
DEBUG_LINE = 103

SHADER = textwrap.dedent("""\
    pipeline P {
        attributes { position: float4  uv: float2 }

        shade :: (float2 uv) -> float4 {
            float r = uv.x * 2.0;
            return float4(r, uv.y, 0.0, 1.0);
        }

        pass "Main" {
            use attributes { position, uv }
            outputs { color: float4 }
            vertex {
                output.position = attributes.position;
                output.uv = attributes.uv;
            }
            fragment {
                output.color = shade(input.uv);
            }
        }
    }
""")


COMPOSITES = textwrap.dedent("""\
    pipeline P {
        attributes { position: float4  uv: float2 }

        struct Inner {
            float2 offset;
            float[3] taps;
        }

        struct Outer {
            Inner inner;
            mat3 basis;
            int count;
        }

        make :: (float2 uv) -> Outer {
            Outer o;
            o.inner.offset = uv;
            o.inner.taps[0] = uv.x;
            o.inner.taps[1] = uv.y;
            o.inner.taps[2] = 1.0;
            o.basis = mat3(1.0);
            o.count = 3;
            return o;
        }

        pass "Main" {
            use attributes { position, uv }
            outputs { color: float4 }
            vertex {
                output.position = attributes.position;
                output.uv = attributes.uv;
            }
            fragment {
                Outer made = make(input.uv);
                Inner[2] list;
                list[0] = made.inner;
                list[1] = made.inner;
                float total = list[1].taps[2] + made.inner.taps[int(input.uv.x)];
                output.color = float4(made.inner.offset, total, 1.0);
            }
        }
    }
""")


def literal(words) -> str:
    return struct.pack(f"<{len(words)}I", *words).split(b"\0", 1)[0].decode()


class Module:
    """What a SPIR-V module says about its sources."""

    def __init__(self, path: Path):
        self.ops = list(instructions(path))
        self.strings = {args[0]: literal(args[1:]) for op, args in self.ops if op == OP_STRING}
        self.constants = {args[1]: args[2] for op, args in self.ops if op == OP_CONSTANT and len(args) == 3}
        imports = {args[0]: literal(args[1:]) for op, args in self.ops if op == OP_EXT_INST_IMPORT}
        debug = {id for id, name in imports.items() if name == "NonSemantic.Shader.DebugInfo.100"}
        self.debug = [(args[1], args[3], args[4:]) for op, args in self.ops
                      if op == OP_EXT_INST and args[2] in debug]

    def debug_ops(self, instruction):
        return [(result, operands) for result, kind, operands in self.debug if kind == instruction]

    def number(self, id):
        return self.constants[id]

    def functions(self):
        return {result: self.strings[operands[0]] for result, operands in self.debug_ops(DEBUG_FUNCTION)}

    def variables(self):
        """DebugLocalVariable id -> (name, function, line, type id)."""
        functions = self.functions()
        return {result: (self.strings[operands[0]], functions[operands[5]], self.number(operands[3]), operands[1])
                for result, operands in self.debug_ops(DEBUG_LOCAL_VARIABLE)}

    def composites(self):
        """DebugTypeComposite id -> (name, line, [(member name, member type id)])."""
        members = {result: (self.strings[operands[0]], operands[1]) for result, operands in self.debug_ops(DEBUG_TYPE_MEMBER)}
        return {result: (self.strings[operands[0]], self.number(operands[3]), [members[m] for m in operands[9:]])
                for result, operands in self.debug_ops(DEBUG_TYPE_COMPOSITE)}

    def arrays(self):
        """DebugTypeArray id -> (element type id, count)."""
        return {result: (operands[0], self.number(operands[1])) for result, operands in self.debug_ops(DEBUG_TYPE_ARRAY)}

    def sources(self):
        return {result: self.strings[operands[0]] for result, operands in self.debug_ops(DEBUG_SOURCE)}


def run_debug_info_tests(compiler: Path) -> tuple[int, int]:
    compiler = Path(compiler).resolve()

    class DebugInfoTests(unittest.TestCase):
        def setUp(self):
            self.temp = tempfile.TemporaryDirectory(prefix="bwsl-debug-info-")
            self.addCleanup(self.temp.cleanup)
            self.folder = Path(self.temp.name)

        def compile(self, source, *flags, files=None):
            for name, text in (files or {}).items():
                (self.folder / name).write_text(text, encoding="utf-8")
            path = self.folder / "shader.bwsl"
            path.write_text(source, encoding="utf-8")
            target = self.folder / "output"
            result = subprocess.run(
                [str(compiler), str(path), *flags, "-validation", "strict", "-modules", str(self.folder),
                 "-o", str(target)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return target

        def test_source_lines_and_inlined_scopes(self):
            module = Module(self.compile(SHADER, "-debug-info") / "shader.frag.spv")
            # The stage's file and its text, for OpSource and DebugSource.
            source = next(args for op, args in module.ops if op == OP_SOURCE)
            self.assertTrue(module.strings[source[2]].endswith("shader.bwsl"))
            self.assertIn("shade :: (float2 uv)", literal(source[3:]))
            self.assertIn("shade :: (float2 uv)", module.strings[module.debug_ops(DEBUG_SOURCE)[0][1][1]])
            # The entry and the inlined function.
            functions = module.functions()
            self.assertEqual(sorted(functions.values()), ["Main.fragment", "shade"])
            # shade is inlined at line 17, so its lines 5 and 6 run in a
            # scope inlined from there; the scope starts with the parameter,
            # bound at the call.
            inlined = module.debug_ops(DEBUG_INLINED_AT)
            self.assertEqual([module.number(operands[0]) for _, operands in inlined], [17])
            shade = next(id for id, name in functions.items() if name == "shade")
            self.assertIn((shade, inlined[0][0]), [tuple(operands) for _, operands in module.debug_ops(DEBUG_SCOPE)])
            lines = [module.number(operands[1]) for _, operands in module.debug_ops(DEBUG_LINE)]
            self.assertEqual(lines, [17, 17, 5, 6, 17])
            self.assertEqual([args[1] for op, args in module.ops if op == OP_LINE], [17, 17, 5, 6, 17])

        def test_local_variables(self):
            module = Module(self.compile(SHADER, "-debug-info") / "shader.frag.spv")
            functions = module.functions()
            variables = {result: (module.strings[operands[0]], functions[operands[5]], module.number(operands[3]))
                         for result, operands in module.debug_ops(DEBUG_LOCAL_VARIABLE)}
            # shade's local belongs to shade, declared where it's first
            # assigned.
            self.assertIn(("r", "shade", 5), variables.values())
            valued = {operands[0] for _, operands in module.debug_ops(DEBUG_VALUE)}
            self.assertEqual(valued, set(variables))
            self.assertFalse(module.debug_ops(DEBUG_DECLARE))

        def test_struct_and_array_locals(self):
            module = Module(self.compile(COMPOSITES, "-debug-info") / "shader.frag.spv")
            composites = module.composites()
            by_name = {name: (id, line, members) for id, (name, line, members) in composites.items()}
            arrays = module.arrays()
            # Structs at their declarations, a field per member, nested
            # structs and array fields typed as such.
            inner, inner_line, inner_members = by_name["Inner"]
            self.assertEqual(inner_line, 4)
            self.assertEqual([name for name, _ in inner_members], ["offset", "taps"])
            self.assertEqual(arrays[inner_members[1][1]][1], 3)
            outer, outer_line, outer_members = by_name["Outer"]
            self.assertEqual(outer_line, 9)
            self.assertEqual([name for name, _ in outer_members], ["inner", "basis", "count"])
            self.assertEqual(outer_members[0][1], inner)
            # Locals in memory are declared once, each in the function that
            # names it: `made` is the fragment's although make's inlined body
            # assigns it, at the call.
            variables = module.variables()
            declared = {operands[0]: operands[1] for _, operands in module.debug_ops(DEBUG_DECLARE)}
            described = {variables[v][:3]: variables[v][3] for v in declared}
            self.assertEqual(described[("made", "Main.fragment", 34)], outer)
            self.assertEqual(described[("o", "make", 16)], outer)
            self.assertEqual(arrays[described[("list", "Main.fragment", 36)]], (inner, 2))
            self.assertEqual([v[0] for v in variables.values()].count("made"), 1)
            # A declared variable's value is its memory's.
            valued = {operands[0] for _, operands in module.debug_ops(DEBUG_VALUE)}
            self.assertFalse(valued & set(declared))

        def test_enum_fields_keep_their_struct(self):
            # Plain and flag enums lower to their integer type and payload
            # enums to a struct, so a struct with enum fields is described
            # whole (a field that couldn't be would drop it).
            quality = textwrap.dedent("""\
                module Shading {
                    enum Quality : uint { Low, High }
                }
            """)
            source = SHADER.replace("pipeline P {", textwrap.dedent("""\
                pipeline P {
                    import Shading
                    enum Mode { Off, Dim }
                    enum Flags : uint { A = 1, B = 2 }
                    enum Shape { Sphere(float radius), Box(float3 size) }
                    struct Light { float3 color; Mode mode; Flags flags; Shading::Quality q; Shape shape; }
                """).rstrip("\n"), 1).replace("output.color = shade(input.uv);", textwrap.dedent("""\
                    Light light;
                    light.color = float3(input.uv, 1.0);
                    light.mode = input.uv.x > 0.5 ? Mode::Dim : Mode::Off;
                    light.flags = input.uv.y > 0.5 ? Flags::A : Flags::B;
                    light.q = input.uv.x > 0.25 ? Shading::Quality::High : Shading::Quality::Low;
                    light.shape = Shape::Sphere(input.uv.x);
                    float k = (light.mode == Mode::Dim && light.flags == Flags::A) ? 1.0 : 0.5;
                    if (light.q == Shading::Quality::Low) { k = 0.0; }
                    output.color = shade(input.uv) * k;"""))
            module = Module(self.compile(source, "-debug-info", files={"Shading.bwsl": quality}) / "shader.frag.spv")
            composites = {name: members for name, _, members in module.composites().values()}
            self.assertEqual([name for name, _ in composites["Light"]], ["color", "mode", "flags", "q", "shape"])
            self.assertIn("Shape", composites)
            declared = {module.variables()[operands[0]][0] for _, operands in module.debug_ops(DEBUG_DECLARE)}
            self.assertIn("light", declared)

        @unittest.skipUnless(shutil.which("xcrun"), "needs the Metal compiler")
        def test_metal_lines_point_at_the_source(self):
            target = self.compile(SHADER, "-debug-info", "-metal")
            ir = self.folder / "shader.frag.ll"
            result = subprocess.run(
                ["xcrun", "-sdk", "macosx", "metal", "-S", "-emit-llvm", "-gline-tables-only",
                 str(target / "shader.frag.metal"), "-o", str(ir)],
                capture_output=True, text=True, timeout=120)
            if result.returncode != 0 and "unable to find utility" in result.stderr:
                self.skipTest(result.stderr.strip())
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            text = ir.read_text(encoding="utf-8")
            self.assertRegex(text, r'DIFile\(filename: "[^"]*shader\.bwsl"')
            self.assertRegex(text, r'DISubprogram\(name: "main0", scope: (![0-9]+), file: \1, line: 17')

        def test_module_code_points_at_its_file(self):
            helper = textwrap.dedent("""\
                module Helper {
                    brighten :: (float4 c) -> float4 {
                        float4 lifted = c * 1.5;
                        return lifted;
                    }
                }
            """)
            source = SHADER.replace("pipeline P {", "pipeline P {\n    import Helper", 1).replace(
                "output.color = shade(input.uv);", "output.color = Helper::brighten(shade(input.uv));")
            module = Module(self.compile(source, "-debug-info", files={"Helper.bwsl": helper}) / "shader.frag.spv")
            sources = module.sources()
            helper_source = next(id for id, path in sources.items() if path.endswith("Helper.bwsl"))
            text = next(module.strings[operands[1]] for result, operands in module.debug_ops(DEBUG_SOURCE)
                        if result == helper_source)
            self.assertIn("brighten :: (float4 c)", text)
            helper_lines = [module.number(operands[1]) for _, operands in module.debug_ops(DEBUG_LINE)
                            if operands[0] == helper_source]
            self.assertIn(3, helper_lines)
            helper_file = next(id for id, string in module.strings.items() if string.endswith("Helper.bwsl"))
            self.assertIn((helper_file, 3), [(args[0], args[1]) for op, args in module.ops if op == OP_LINE])

        def test_generated_glsl_has_standard_line_directives(self):
            target = self.compile(SHADER, "-debug-info", "-glsl", "-hlsl")
            glsl = (target / "shader.frag").read_text(encoding="utf-8")
            self.assertIn("#line 17 0", glsl)
            self.assertIn("//   0: ", glsl)
            self.assertNotIn("GL_GOOGLE_cpp_style_line_directive", glsl)
            self.assertNotIn('#line 17 "', glsl)
            hlsl = (target / "shader.frag.hlsl").read_text(encoding="utf-8")
            self.assertRegex(hlsl, r'#line 17 ".*shader\.bwsl"')
            glslang = shutil.which("glslangValidator")
            if glslang:
                result = subprocess.run([glslang, "-S", "frag", str(target / "shader.frag")],
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        def test_off_without_the_flag(self):
            for flags in ((), ("-debug-names",)):
                with self.subTest(flags=flags):
                    target = self.compile(SHADER, *flags, "-glsl", "-spv")
                    module = Module(target / "shader.frag.spv")
                    self.assertFalse(module.debug)
                    self.assertFalse([op for op, _ in module.ops if op in (OP_LINE, OP_SOURCE)])
                    self.assertNotIn("#line", (target / "shader.frag").read_text(encoding="utf-8"))

    result = unittest.TextTestRunner(verbosity=1).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(DebugInfoTests))
    failures = len(result.failures) + len(result.errors)
    return result.testsRun - failures, failures


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build" / "bwslc")
    args = parser.parse_args()
    _, failures = run_debug_info_tests(args.compiler)
    raise SystemExit(bool(failures))
