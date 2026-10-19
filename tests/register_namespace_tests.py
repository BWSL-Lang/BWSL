"""Live SSA registers must not alias constant operands or wrap the register limit."""
from __future__ import annotations

from pathlib import Path
import argparse
import json
import re
import struct
import subprocess
import tempfile
import unittest

# Registers per stage, and the limit before operands gave registers 15 bits.
REGISTER_LIMIT = 32764
OLD_REGISTER_LIMIT = 16383
# Operand codes from this one up hold float constants; int and uint constants
# and texture handles sit between REGISTER_LIMIT and it.
FLOAT_CONSTANT_CODES = 49149


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


def overflow_shader(calls: int = 200) -> str:
    # A small source expands through inlining, then loop/branch PHIs push SSA
    # beyond the limit. Keep lowering below the boundary to test SSA itself:
    # each call lowers to about 73 registers and SSA adds about 101.
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
        "acc=burst(acc,flip);\n" * calls + "resources.output[idx]=acc;}}}\n")


def integer_shader(count: int, unsigned: bool = False) -> str:
    kind, suffix = ("uint", "u") if unsigned else ("int", "")
    return (
        "pipeline IntegerPool { resources { inbuf: buffer<float> output: buffer<float> }\n"
        'pass "Compute" { use resources {inbuf,output} compute "Main" [8,1,1] {\n'
        f"uint idx=input.global_id.x; {kind} acc={kind}(resources.inbuf[idx]);\n" +
        "".join(f"acc=acc+{i}{suffix};\n" for i in range(count)) +
        "resources.output[idx]=float(acc); }}}\n")


BURST = "burst :: (float x, float delta) -> float {\n" + "x=x+delta;\n" * 64 + "return x;}\n"


def lowering_overflow_shader(calls: int = 520) -> str:
    # Expand a small AST through inlining to isolate the lowering limit from
    # parser arena capacity limits on very large source files. Each call
    # lowers to about 69 registers.
    return (
        "pipeline LoweringOverflow {resources {inbuf: buffer<float> output: buffer<float>}\n" +
        BURST + 'pass "Compute" {use resources {inbuf,output} compute "Main" [8,1,1] {\n'
        "uint idx=input.global_id.x;float delta=resources.inbuf[idx];float acc=delta;\n" +
        "acc=burst(acc,delta);\n" * calls + "resources.output[idx]=acc;}}}\n")


def high_register_shader(calls: int) -> str:
    # Each call adds about 69 live registers, so 380 calls pass 0x6000, where
    # uint constants and texture handles used to start. SSA's spare capacity
    # then reaches past every int and uint constant code. Integer and bool
    # constants feed the result so their decoding is checked numerically.
    return (
        "pipeline HighRegisters {resources {inbuf: buffer<float> output: buffer<float>}\n" +
        BURST + 'pass "Compute" {use resources {inbuf,output} compute "Main" [8,1,1] {\n'
        "uint idx=input.global_id.x;float delta=resources.inbuf[idx];float acc=delta;\n" +
        "acc=burst(acc,delta);\n" * calls +
        "uint bits=(idx^5u)<<1u; int signedBits=int(idx)*-3+2; bool odd=(idx&1u)!=0u;\n"
        "if(odd){acc=acc-3.0;}else{acc=acc+7.0;}\n"
        "resources.output[idx]=acc+float(bits)+float(signedBits);}}}\n")


def high_register_expected(values: list[float], calls: int) -> tuple[float, ...]:
    return tuple(value * (64 * calls + 1) + (-3 if i & 1 else 7) + ((i ^ 5) << 1) + (i * -3 + 2)
                 for i, value in enumerate(values))


def high_register_resources_shader(calls: int = 380) -> str:
    # Texture and sampler handles, every constant kind and gather's invalid
    # marker in a fragment stage whose registers pass 0x6000. SSA's spare
    # capacity then covers the handles' operand code.
    return (
        "pipeline HighRegisterResources { resources { tex: texture2D smp: sampler }\n" +
        BURST + 'pass "Main" { use resources {tex, smp} vertex {output.position=float4(1.0);}\n'
        "fragment {float delta=input.position.x; float acc=delta;\n" +
        "acc=burst(acc,delta);\n" * calls +
        "int lane=int(acc)&3; uint bits=uint(lane)<<2u; bool bright=acc>0.5;\n"
        "float4 color=sample(resources.tex,float2(acc));\n"
        "color=color+sample_lod(resources.tex,resources.smp,float2(acc),0.0);\n"
        "if(bright){color=color+gather(resources.tex,float2(acc),1);}\n"
        "output.color=color*float(bits^5u)+float4(float(lane)); }}}\n")


def gather_shader(calls: int = 260) -> str:
    # SSA's reservation estimate (about 35000) passes the invalid marker
    # (32764), but the live registers (about 18000) fit. Gather's unused
    # marker operand must never index the register map.
    return (
        "pipeline GatherMarker { resources { tex: texture2D }\n" + BURST +
        'pass "Main" { use resources {tex} vertex {output.position=float4(1.0);}\n'
        "fragment {float delta=input.position.x; float acc=delta;\n" +
        "acc=burst(acc,delta);\n" * calls +
        "if(delta>0.0){acc=acc+delta;}else{acc=acc-delta;}\n"
        "output.color=gather(resources.tex,float2(acc),0); }}}\n")


def power_shader(additions: int) -> str:
    # pow(acc, 4.0) becomes two multiplies, which take a new register.
    return accumulator_shader(additions).replace(
        "resources.output[idx] = acc;", "resources.output[idx] = pow(acc, 4.0);")


def sampler_handle_shader(pointers: int = 6500) -> str:
    # Every `^x` and pointer local fills a register, so register 0x3000 is a
    # pointer when `sample` runs. The sampler handle used to be 0x3000 | slot,
    # that number, and must not be read as a pointer argument.
    return (
        "pipeline SamplerHandle { resources { tex: texture2D smp: sampler }\n"
        'pass "Main" { use resources {tex, smp} vertex {output.position=float4(1.0);}\n'
        "fragment {float x=input.position.x;\n" +
        "".join(f"float^ p{i}=^x;\n" for i in range(pointers)) +
        "output.color=sample(resources.tex,resources.smp,float2(x)); }}}\n")


def sampler_register_shader(before: int, after: int, offsets: bool = True) -> str:
    # Each call lowers to about 69 registers, so after 180 calls the
    # coordinates sit in 0x3000-0x3FFF, the numbers sampler handles used to
    # share (0x3000 | slot). Texture calls without a sampler must not take a
    # coordinate there for one, and naming resources.smp once register
    # 0x3000 + slot exists must not retype that register. WGSL has no
    # gather_offset or load_offset, the other calls that could mistake one.
    return (
        "pipeline SamplerRegisters { resources { tex: texture2D smp: sampler }\n"
        "burst :: (float x, float delta) -> float {\n" + "x=x+delta;\n" * 64 + "return x;}\n"
        'pass "Main" { use resources {tex, smp} vertex {output.position=float4(1.0);}\n'
        "fragment {float delta=input.position.x; float acc=delta;\n" +
        "acc=burst(acc,delta);\n" * before +
        "float2 uv=float2(acc); int2 texel=int2(uv);\n" +
        "acc=burst(acc,delta);\n" * after +
        "float4 color=sample_lod(resources.tex,uv,0.0);\n" +
        ("color=color+gather_offset(resources.tex,uv,1,int2(1,0));\n"
         "color=color+load_offset(resources.tex,texel,0,int2(1,0));\n" if offsets else "") +
        "color=color+sample(resources.tex,resources.smp,uv+float2(acc));\n"
        "color=color+sample_lod(resources.tex,resources.smp,uv,1.0);\n"
        "color=color+gather(resources.tex,resources.smp,uv,2);\n"
        "output.color=color; }}}\n")


def run_register_namespace_tests(compiler: Path, output: Path,
                                 runner: Path | None = None) -> tuple[int, int]:
    compiler = compiler.resolve()
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    additions = 9000

    def compile_shader(name: str, code: str, internals: bool = False, json_errors: bool = False,
                       direct_gles: bool = False, wgsl: bool = False, all_targets: bool = False):
        source = output / f"{name}.bwsl"
        source.write_text(code, encoding="utf-8")
        target = output / name
        target.mkdir(exist_ok=True)
        result = subprocess.run(
            [str(compiler), str(source), "-o", str(target), "-spv", "-validation", "strict",
             *(["-internals"] if internals else []),
             *(["-gles-direct"] if direct_gles else []),
             *(["-wgsl"] if wgsl else []),
             *(["-all"] if all_targets else []),
             *(["-errors-json"] if json_errors else [])],
            capture_output=True, text=True, timeout=60)
        return result, target

    def run_compute(spirv: Path, values: list[float]) -> tuple[float, ...]:
        # One workgroup of 8 over binding 0 in, binding 1 out.
        source, destination = spirv.parent / "input.bin", spirv.parent / "output.bin"
        source.write_bytes(struct.pack("<8f", *values))
        result = subprocess.run(
            [str(runner.resolve()), "--input", str(source), "--input-binding", "0",
             "--output", str(destination), "--output-size", "32", "--output-binding", "1",
             "--set", "1", "--pass-spirv", str(spirv), "--pass-groups", "1", "1", "1"],
            capture_output=True, text=True, timeout=60)
        if result.returncode != 0:
            raise AssertionError(result.stdout + result.stderr)
        return struct.unpack("<8f", destination.read_bytes())

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
            self.assertTrue(any(8192 <= register < OLD_REGISTER_LIMIT for register in phis & outputs),
                            ir[-1800:])

        def test_registers_past_old_limit_reach_storage(self):
            result, target = compile_shader("high_registers", high_register_shader(380), True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            ir = json.loads((target / "high_registers.comp.internals.json").read_text())["ir"]
            # A live PHI and the stored value both sit past 0x6000, where uint
            # constants and texture handles used to start.
            phis = {int(register) for register in re.findall(r"PHI r(\d+) \(block", ir)}
            outputs = {int(register) for register in re.findall(r"ARRAY_STORE\s+r\d+\s+<- r\d+, r(\d+)", ir)}
            self.assertTrue(any(0x6000 <= register < REGISTER_LIMIT for register in phis), ir[-1800:])
            self.assertTrue(any(0x6000 <= register < REGISTER_LIMIT for register in outputs), ir[-1800:])
            capacity = re.search(r"Registers: (\d+)", ir)
            self.assertIsNotNone(capacity)
            self.assertGreater(int(capacity[1]), FLOAT_CONSTANT_CODES)
            wgsl, _ = compile_shader("high_registers_wgsl", high_register_shader(380), wgsl=True)
            self.assertEqual(wgsl.returncode, 0, wgsl.stdout + wgsl.stderr)

        @unittest.skipIf(runner is None, "native execution requires the Vulkan equivalence runner")
        def test_registers_past_old_limit_numeric_output(self):
            calls = 380
            result, target = compile_shader("high_registers_numeric", high_register_shader(calls))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            values = [.125, -.25, .5, -1., 2., -4., 0., 8.]
            self.assertEqual(run_compute(target / "high_registers_numeric.comp.spv", values),
                             high_register_expected(values, calls))

        def test_resources_and_constants_past_old_limit(self):
            source = high_register_resources_shader()
            for name, flags in (("resources_all", {"all_targets": True}),
                                ("resources_direct", {"direct_gles": True, "wgsl": True})):
                with self.subTest(name=name):
                    result, target = compile_shader(name, source, **flags)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertTrue(any(target.glob("*.frag.spv")))

        def test_fitting_shaders_keep_the_old_rewrite_cutoff(self):
            # New registers come after SSA's spare capacity, which for a large
            # shader passed the old limit before its registers did, so pow(x, 4.0)
            # stayed a pow. A shader whose registers fit below the old limit
            # must compile as it did then.
            for additions, kept in ((100, False), (9000, True)):
                with self.subTest(additions=additions):
                    result, target = compile_shader(f"power_{additions}", power_shader(additions), True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    ir = json.loads((target / f"power_{additions}.comp.internals.json").read_text())["ir"]
                    capacity = int(re.search(r"Registers: (\d+)", ir)[1])
                    self.assertEqual(capacity > OLD_REGISTER_LIMIT, kept, capacity)
                    self.assertEqual("POW" in ir, kept, ir[-1200:])

        @unittest.skipIf(runner is None, "native execution requires the Vulkan equivalence runner")
        def test_high_register_numeric_output(self):
            self.assertEqual(self.compiled.returncode, 0, self.compiled.stdout + self.compiled.stderr)
            values = [.125, -.25, .5, -1., 2., -4., 0., 8.]
            expected = tuple(value * (additions + 1) + (7 if i % 2 == 0 else -3)
                             for i, value in enumerate(values))
            self.assertEqual(run_compute(self.target / "live_register.comp.spv", values), expected)

        def test_ssa_overflow_is_a_diagnostic(self):
            result, target = compile_shader("ssa_overflow", overflow_shader(), json_errors=True)
            diagnostics = result.stdout + result.stderr
            self.assertEqual(result.returncode, 1, diagnostics)
            self.assertIn(f"SSA register limit exceeded ({REGISTER_LIMIT} registers)", diagnostics)
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
            self.assert_located_error(result, f"Shader register limit exceeded ({REGISTER_LIMIT} registers)")
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

        def test_sampler_handle_is_not_a_pointer_argument(self):
            result, _ = compile_shader("sampler_handle", sampler_handle_shader())
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        def assert_sampler_registers(self, after: int, registers: int):
            # The samplers each WGSL texture call names, in order.
            def wgsl_samplers(target: Path, stem: str) -> list[str]:
                wgsl = (target / f"{stem}.frag.wgsl").read_text()
                return re.findall(r"texture\w+\([^;]*?\b(bwsl_sampler_\d+)", wgsl)

            control, control_target = compile_shader(
                "sampler_registers_control", sampler_register_shader(2, 1, offsets=False), wgsl=True)
            self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
            expected = wgsl_samplers(control_target, "sampler_registers_control")
            self.assertEqual(len(expected), 4, expected)
            for name, offsets, flags in (("wgsl", False, {"wgsl": True}),
                                         ("direct", True, {"direct_gles": True, "internals": True}),
                                         ("all", True, {"all_targets": True})):
                stem = f"sampler_registers_{after}_{name}"
                result, target = compile_shader(stem, sampler_register_shader(180, after, offsets),
                                                **flags)
                diagnostics = result.stdout + result.stderr
                with self.subTest(targets=name):
                    self.assertEqual(result.returncode, 0, diagnostics)
                    if name == "wgsl":
                        self.assertEqual(wgsl_samplers(target, stem), expected)
                    if name == "direct":
                        ir = json.loads((target / f"{stem}.frag.internals.json").read_text())["ir"]
                        self.assertGreater(int(re.search(r"Registers: (\d+)", ir)[1]), registers)
                        # Three calls without a sampler and one with.
                        coordinates = [int(register) for register in re.findall(
                            r"TEX_(?:SAMPLE_LOD|GATHER_OFFSET|FETCH_OFFSET)\s+r\d+\s+<- [^,]+, r(\d+)", ir)]
                        self.assertEqual(len(coordinates), 4, coordinates)
                        self.assertTrue(all(0x3000 <= register < 0x4000 for register in coordinates),
                                        coordinates)

        def test_sampler_handles_are_not_registers(self):
            self.assert_sampler_registers(after=10, registers=13000)

        def test_sampler_handles_past_old_register_limit(self):
            self.assert_sampler_registers(after=200, registers=26000)

        def test_gather_invalid_marker_with_large_ssa_capacity(self):
            result, target = compile_shader("gather_marker", gather_shader(), internals=True,
                                            direct_gles=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            metadata = json.loads((target / "gather_marker.frag.internals.json").read_text())
            self.assertIn("TEX_GATHER", metadata["ir"])
            capacity = re.search(r"Registers: (\d+)", metadata["ir"])
            self.assertIsNotNone(capacity)
            self.assertGreater(int(capacity[1]), REGISTER_LIMIT)

        @unittest.skipIf(runner is None, "native execution requires the Vulkan equivalence runner")
        def test_grown_integer_pool_numeric_output(self):
            for unsigned in (False, True):
                result, target = compile_shader(f"integer_numeric_{unsigned}", integer_shader(600, unsigned))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                values = [float(i) for i in range(8)]
                self.assertEqual(run_compute(target / f"integer_numeric_{unsigned}.comp.spv", values),
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
