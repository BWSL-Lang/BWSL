"""Large sources grow the compilation arena; exhausting its limit is a diagnostic."""
from __future__ import annotations

from pathlib import Path
import argparse
import json
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# The compilation arena used to be a fixed 16 MiB block. Sources that need
# more crashed the compiler; this many module helpers need about 25 MiB.
LARGE_MODULE_HELPERS = 4000


def helper(name: str, index: int) -> str:
    return (f"    {name}{index} :: (float x, float y) -> float {{\n"
            f"        float a = x * {index + 1}.0 + y;\n"
            "        float b = a * a - x;\n"
            "        if (b > y) { a = a + b * 0.5; } else { a = a - b * 0.25; }\n"
            "        return a + b;\n"
            "    }\n")


def compute_body(calls: list[str]) -> str:
    return ('    pass "Compute" {\n'
            "        use resources { inbuf, output }\n"
            '        compute "Main" [8, 1, 1] {\n'
            "            uint idx = input.global_id.x;\n"
            "            float v = resources.inbuf[idx];\n" +
            "".join(f"            v = {call}(v, 0.5);\n" for call in calls) +
            "            resources.output[idx] = v;\n"
            "        }\n"
            "    }\n")


def large_module_sources(count: int) -> tuple[str, str]:
    module = "module ArenaStressLib {\n" + "".join(helper("helper", i) for i in range(count)) + "}\n"
    step = max(1, count // 32)
    pipeline = ("pipeline ArenaStress {\n"
                "    import ArenaStressLib\n"
                "    resources { inbuf: buffer<float> output: buffer<float> }\n" +
                compute_body([f"ArenaStressLib::helper{i}" for i in range(0, count, step)]) +
                "}\n")
    return module, pipeline


SMALL_SOURCE = """pipeline SmallUnit {
    resources { inbuf: buffer<float> output: buffer<float> }
""" + compute_body([]) + "}\n"


def run_arena_growth_tests(compiler: Path, output: Path) -> tuple[int, int]:
    compiler = compiler.resolve()
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    module, pipeline = large_module_sources(LARGE_MODULE_HELPERS)
    (output / "ArenaStressLib.bwsl").write_text(module, encoding="utf-8")
    large = output / "arena_stress.bwsl"
    large.write_text(pipeline, encoding="utf-8")
    small = output / "small_unit.bwsl"
    small.write_text(SMALL_SOURCE, encoding="utf-8")

    def compile_units(name: str, sources: list[Path], *extra: str) -> tuple[subprocess.CompletedProcess[str], Path]:
        target = output / name
        if target.exists():
            shutil.rmtree(target)
        target.mkdir()
        result = subprocess.run(
            [str(compiler), *map(str, sources), "-modules", str(output), "-o", str(target),
             "-spv", "-validation", "strict", *extra],
            capture_output=True, text=True, timeout=120)
        return result, target

    class ArenaGrowthTests(unittest.TestCase):
        def assert_clean_exhaustion(self, result, target):
            text = result.stdout + result.stderr
            # A signal shows up as a negative return code.
            self.assertEqual(result.returncode, 1, text[-2000:])
            report = json.loads(result.stdout)
            errors = [d for d in report["diagnostics"] if d["code"] == "BWSL1402"]
            self.assertEqual(len(errors), 1, report)
            self.assertIn("compiler memory limit", errors[0]["message"])
            self.assertIn("-arena-limit", errors[0]["message"])
            self.assertNotIn("Allocation failed due to insufficient capacity", text)
            self.assertFalse(any(target.rglob("*.spv")), text[-2000:])

        def test_large_source_grows_past_first_block(self):
            result, target = compile_units("large", [large])
            self.assertEqual(result.returncode, 0, (result.stdout + result.stderr)[-2000:])
            self.assertTrue((target / "arena_stress.comp.spv").is_file())

        def test_large_source_needs_more_than_old_fixed_arena(self):
            # 16 MiB was the old fixed size: the stress source must not fit it,
            # or the growth test above proves nothing.
            result, target = compile_units("old_size", [large], "-arena-limit", "16", "-errors-json")
            self.assert_clean_exhaustion(result, target)

        def test_limit_below_first_block(self):
            result, target = compile_units("tiny_limit", [large], "-arena-limit", "1", "-errors-json")
            self.assert_clean_exhaustion(result, target)

        def test_output_is_deterministic(self):
            first, first_target = compile_units("deterministic_a", [large])
            second, second_target = compile_units("deterministic_b", [large])
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
            self.assertEqual((first_target / "arena_stress.comp.spv").read_bytes(),
                             (second_target / "arena_stress.comp.spv").read_bytes())

        def test_batch_continues_after_exhaustion(self):
            alone, alone_target = compile_units("small_alone", [small])
            self.assertEqual(alone.returncode, 0, alone.stdout + alone.stderr)
            batch, batch_target = compile_units("batch", [large, small], "-arena-limit", "16")
            text = batch.stdout + batch.stderr
            self.assertEqual(batch.returncode, 1, text[-2000:])
            self.assertIn("succeeded: 1, failed: 1", batch.stdout)
            self.assertIn("BWSL1402", text)
            self.assertEqual((alone_target / "small_unit.comp.spv").read_bytes(),
                             (batch_target / "small_unit.comp.spv").read_bytes())

        def test_invalid_limit_is_rejected(self):
            for value in (["0"], ["lots"], ["-4"], []):
                with self.subTest(value=value):
                    result, _ = compile_units("bad_limit", [small], "-arena-limit", *value)
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    self.assertIn("Expected a size in MiB", result.stdout + result.stderr)

        @unittest.skipUnless(shutil.which(shlex.split(os.environ.get("CXX", "clang++"))[0]),
                             "the arena unit test needs a C++ compiler")
        def test_arena_unit(self):
            binary = output / "arena-tests"
            build = subprocess.run(
                [*shlex.split(os.environ.get("CXX", "clang++")), "-std=c++20", "-O1", "-g",
                 f"-I{ROOT / 'src'}", str(ROOT / "tests/arena_tests.cpp"), "-o", str(binary)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    tests = unittest.defaultTestLoader.loadTestsFromTestCase(ArenaGrowthTests)
    result = unittest.TextTestRunner(verbosity=2).run(tests)
    # Count tests, not failing subtests, so the totals stay consistent.
    failures = len({getattr(test, "test_case", test).id()
                    for test, _ in result.failures + result.errors})
    return result.testsRun - failures - len(result.skipped), failures


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=ROOT / "build/bwslc")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="bwsl-arena-tests-") as directory:
        passed, failed = run_arena_growth_tests(args.compiler, Path(directory))
    print(f"Arena growth: {passed} passed, {failed} failed")
    raise SystemExit(bool(failed))
