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


def run_register_namespace_tests(compiler: Path, output: Path,
                                 runner: Path | None = None) -> tuple[int, int]:
    compiler = compiler.resolve()
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    additions = 9000

    def compile_shader(name: str, code: str, internals: bool = False):
        source = output / f"{name}.bwsl"
        source.write_text(code, encoding="utf-8")
        target = output / name
        target.mkdir(exist_ok=True)
        result = subprocess.run(
            [str(compiler), str(source), "-o", str(target), "-spv", "-validation", "strict",
             *(["-internals"] if internals else [])],
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
            result, target = compile_shader("ssa_overflow", overflow_shader())
            diagnostics = result.stdout + result.stderr
            self.assertEqual(result.returncode, 1, diagnostics)
            self.assertIn("SSA register limit exceeded (16383 registers)", diagnostics)
            self.assertNotIn("SPIR-V validation failed", diagnostics)
            self.assertFalse(any(target.glob("*.spv")), diagnostics)

    tests = unittest.defaultTestLoader.loadTestsFromTestCase(RegisterNamespaceTests)
    result = unittest.TextTestRunner(verbosity=2).run(tests)
    failures = len(result.failures) + len(result.errors)
    return result.testsRun - failures - len(result.skipped), failures


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=root / "build/bwslc")
    parser.add_argument("--runner", type=Path, default=root / "build/equiv_runner")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="bwsl-register-tests-") as directory:
        passed, failed = run_register_namespace_tests(args.compiler, Path(directory), args.runner)
    print(f"Register namespace: {passed} passed, {failed} failed")
    raise SystemExit(bool(failed))
