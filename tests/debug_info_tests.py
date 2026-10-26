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

DEBUG_FUNCTION = 20
DEBUG_SCOPE = 23
DEBUG_INLINED_AT = 25
DEBUG_LOCAL_VARIABLE = 26
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
