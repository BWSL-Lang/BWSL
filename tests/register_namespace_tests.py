"""Live SSA registers must not alias tagged constants or wrap the register limit."""
from __future__ import annotations

from pathlib import Path
import argparse
import json
import re
import struct
import subprocess
import tempfile
import unittest


def accumulator_shader(additions: int) -> str:
    # Keep every addition input-dependent and observable. The final branch creates
    # a live SSA PHI above the old 0x2000 uint tag, consumed by ARRAY_STORE.
    return """pipeline RegisterNamespace {
    resources { inbuf: buffer<float> output: buffer<float> }
    pass "Compute" {
        use resources { inbuf, output }
        compute "Main" [8, 1, 1] {
            uint idx = input.global_id.x;
            float delta = resources.inbuf[idx];
            float acc = delta;
""" + "            acc = acc + delta;\n" * additions + """
            if ((idx & 1u) == 0u) { acc = acc + 7.0; }
            else { acc = acc - 3.0; }
            resources.output[idx] = acc;
        }
    }
}
"""


def overflow_shader() -> str:
    # A small source expands through inlining, then loop/branch PHIs push SSA
    # beyond the limit. Keep lowering below the boundary to test SSA itself.
    count = 32
    declarations = "".join(f"float v{i}=delta;\n" for i in range(count))
    rotate = "".join(f"v{i}=v{(i + 1) % count};\n" for i in range(count))
    reset = "".join(f"v{i}=delta;\n" for i in range(count))
    return (
        "pipeline SSAOverflow { resources { inbuf: buffer<float> output: buffer<float> }\n"
        "burst :: (float delta, bool flip) -> float {\n" + declarations +
        "for(uint j=0u;j<2u;j=j+1u){if(flip){\n" + rotate + "}else{\n" + reset + "}}\n"
        "return " + "+".join(f"v{i}" for i in range(count)) + ";}\n"
        'pass "Compute" {use resources {inbuf,output} compute "Main" [8,1,1] {\n'
        "uint idx=input.global_id.x;float acc=resources.inbuf[idx];bool flip=(idx&1u)==0u;\n" +
        "acc=burst(acc,flip);\n" * 120 + "resources.output[idx]=acc;}}}\n")


def integer_shader(count: int, unsigned: bool = False) -> str:
    kind, suffix = ("uint", "u") if unsigned else ("int", "")
    return (
        "pipeline IntegerPool { resources { inbuf: buffer<float> output: buffer<float> }\n"
        'pass "Compute" { use resources {inbuf,output} compute "Main" [8,1,1] {\n'
        f"uint idx=input.global_id.x; {kind} acc={kind}(resources.inbuf[idx]);\n" +
        "".join(f"acc=acc+{i}{suffix};\n" for i in range(count)) +
        "resources.output[idx]=float(acc); }}}\n")


def lowering_overflow_shader() -> str:
    # Expand a small AST through inlining to isolate the lowering limit from
    # parser arena capacity limits on very large source files.
    return (
        "pipeline LoweringOverflow {resources {inbuf: buffer<float> output: buffer<float>}\n"
        "burst :: (float x, float delta) -> float {\n" + "x=x+delta;\n" * 64 +
        'return x;} pass "Compute" {use resources {inbuf,output} compute "Main" [8,1,1] {\n'
        "uint idx=input.global_id.x;float delta=resources.inbuf[idx];float acc=delta;\n" +
        "acc=burst(acc,delta);\n" * 260 + "resources.output[idx]=acc;}}}\n")


def gather_shader() -> str:
    # The SSA reservation estimate exceeds 0x3fff, but actual live registers
    # fit. Gather's unused 0x3fff operand must never index the original map.
    return (
        "pipeline GatherMarker { resources { tex: texture2D }\n"
        'pass "Main" { use resources {tex} vertex {output.position=float4(1.0);}\n'
        "fragment {float delta=input.position.x; float acc=delta;\n" +
        "acc=acc+delta;\n" * 9000 +
        "if(delta>0.0){acc=acc+delta;}else{acc=acc-delta;}\n"
        "output.color=gather(resources.tex,float2(acc),0); }}}\n")


def run_register_namespace_tests(compiler: Path, output: Path,
                                 runner: Path | None = None) -> tuple[int, int]:
    compiler = compiler.resolve()
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    additions = 9000

    def compile_shader(name: str, code: str, internals: bool = False, json_errors: bool = False,
                       direct_gles: bool = False):
        source = output / f"{name}.bwsl"
        source.write_text(code, encoding="utf-8")
        target = output / name
        target.mkdir(exist_ok=True)
        result = subprocess.run(
            [str(compiler), str(source), "-o", str(target), "-spv", "-validation", "strict",
             *(["-internals"] if internals else []),
             *(["-gles-direct"] if direct_gles else []),
             *(["-errors-json"] if json_errors else [])],
            capture_output=True, text=True, timeout=60)
        return result, target

    class RegisterNamespaceTests(unittest.TestCase):
        @classmethod
        def setUpClass(cls):
            cls.compiled, cls.target = compile_shader("live_register", accumulator_shader(additions), True)

        def test_live_high_phi_reaches_storage(self):
            self.assertEqual(self.compiled.returncode, 0, self.compiled.stdout + self.compiled.stderr)
            metadata = json.loads((self.target / "live_register.comp.internals.json").read_text())
            ir = metadata["ir"]
            phis = {int(register) for register in re.findall(r"PHI r(\d+) \(block", ir)}
            outputs = {int(register) for register in re.findall(r"ARRAY_STORE\s+r\d+\s+<- r\d+, r(\d+)", ir)}
            # registerCount includes unused reserved capacity: prove a real high PHI
            # is consumed by storage, rather than checking the dump's header alone.
            self.assertTrue(any(8192 <= register < 16383 for register in phis & outputs), ir[-1800:])

        @unittest.skipIf(runner is None, "native execution requires the Vulkan equivalence runner")
        def test_high_register_numeric_output(self):
            self.assertEqual(self.compiled.returncode, 0, self.compiled.stdout + self.compiled.stderr)
            values = [.125, -.25, .5, -1., 2., -4., 0., 8.]
            source = self.target / "input.bin"
            destination = self.target / "output.bin"
            source.write_bytes(struct.pack("<8f", *values))
            result = subprocess.run(
                [str(runner.resolve()), "--input", str(source), "--input-binding", "0",
                 "--output", str(destination), "--output-size", "32", "--output-binding", "1",
                 "--set", "1", "--pass-spirv", str(self.target / "live_register.comp.spv"),
                 "--pass-groups", "1", "1", "1"],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            expected = tuple(value * (additions + 1) + (7 if i % 2 == 0 else -3)
                             for i, value in enumerate(values))
            self.assertEqual(struct.unpack("<8f", destination.read_bytes()), expected)

        def test_ssa_overflow_is_a_diagnostic(self):
            result, target = compile_shader("ssa_overflow", overflow_shader(), json_errors=True)
            diagnostics = result.stdout + result.stderr
            self.assertEqual(result.returncode, 1, diagnostics)
            self.assertIn("SSA register limit exceeded (16383 registers)", diagnostics)
            self.assertNotIn("SPIR-V validation failed", diagnostics)
            self.assertFalse(any(target.glob("*.spv")), diagnostics)
            self.assert_located_error(result, "SSA register limit exceeded")

        def assert_located_error(self, result, message):
            report = json.loads(result.stdout)
            errors = [d for d in report["diagnostics"] if message in d["message"]]
            self.assertEqual(len(errors), 1, report)
            self.assertGreater(errors[0]["line"], 0, errors)
            self.assertGreater(errors[0]["column"], 0, errors)

        def test_lowering_overflow_is_located(self):
            result, target = compile_shader("lowering_overflow", lowering_overflow_shader(), json_errors=True)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assert_located_error(result, "Shader register limit exceeded")
            self.assertFalse(any(target.glob("*.spv")))

        def test_integer_pools_grow_and_stop_before_tag_collision(self):
            for unsigned in (False, True):
                for count in (600, 8192, 8193):
                    with self.subTest(unsigned=unsigned, count=count):
                        result, target = compile_shader(
                            f"integers_{unsigned}_{count}", integer_shader(count, unsigned), json_errors=True)
                        if count <= 8192:
                            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        else:
                            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                            self.assert_located_error(result, "integer constant limit exceeded")
                            self.assertFalse(any(target.glob("*.spv")))

        def test_gather_invalid_marker_with_large_ssa_capacity(self):
            result, target = compile_shader("gather_marker", gather_shader(), internals=True,
                                            direct_gles=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            metadata = json.loads((target / "gather_marker.frag.internals.json").read_text())
            self.assertIn("TEX_GATHER", metadata["ir"])
            capacity = re.search(r"Registers: (\d+)", metadata["ir"])
            self.assertIsNotNone(capacity)
            self.assertGreater(int(capacity[1]), 16383)

        @unittest.skipIf(runner is None, "native execution requires the Vulkan equivalence runner")
        def test_grown_integer_pool_numeric_output(self):
            for unsigned in (False, True):
                result, target = compile_shader(f"integer_numeric_{unsigned}", integer_shader(600, unsigned))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                source, destination = target / "input.bin", target / "output.bin"
                values = tuple(float(i) for i in range(8))
                source.write_bytes(struct.pack("<8f", *values))
                result = subprocess.run(
                    [str(runner.resolve()), "--input", str(source), "--input-binding", "0",
                     "--output", str(destination), "--output-size", "32", "--output-binding", "1",
                     "--set", "1", "--pass-spirv", str(target / f"integer_numeric_{unsigned}.comp.spv"),
                     "--pass-groups", "1", "1", "1"], capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(struct.unpack("<8f", destination.read_bytes()),
                                 tuple(value + sum(range(600)) for value in values))

    tests = unittest.defaultTestLoader.loadTestsFromTestCase(RegisterNamespaceTests)
    result = unittest.TextTestRunner(verbosity=2).run(tests)
    failures = len(result.failures) + len(result.errors)
    return result.testsRun - failures - len(result.skipped), failures


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=root / "build/bwslc")
    parser.add_argument("--runner", type=Path, default=None)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="bwsl-register-tests-") as directory:
        passed, failed = run_register_namespace_tests(args.compiler, Path(directory), args.runner)
    print(f"Register namespace: {passed} passed, {failed} failed")
    raise SystemExit(bool(failed))
