"""Validation-mode and numerical-comparison regressions from the P1 QA fixes."""
from pathlib import Path
import struct
import subprocess
import unittest


def run_p1_regression_tests(compiler: Path, output: Path) -> tuple[int, int]:
    # Import lazily: the suite imports this module after defining its helpers.
    from run_tests import compare_bytes, missing_equivalence_backends

    class P1RegressionTests(unittest.TestCase):
        def test_finite_reference_rejects_nonfinite_actual(self):
            spec = {"output_type": "float", "tolerance": 0.01}
            for actual in (float("nan"), float("inf"), -float("inf")):
                with self.subTest(actual=actual):
                    ok, _ = compare_bytes(struct.pack("<f", 1.0), struct.pack("<f", actual), spec)
                    self.assertFalse(ok)

        def test_nonfinite_reference_does_not_hide_mismatch(self):
            spec = {"output_type": "float", "tolerance": 0.01}
            for reference, actual in ((float("nan"), 1.0), (float("nan"), float("nan")),
                                      (float("inf"), -float("inf"))):
                with self.subTest(reference=reference, actual=actual):
                    self.assertFalse(compare_bytes(struct.pack("<f", reference),
                                                   struct.pack("<f", actual), spec)[0])
            self.assertTrue(compare_bytes(struct.pack("<f", float("inf")),
                                          struct.pack("<f", float("inf")), spec)[0])

        def test_comparison_preserves_tolerance_and_size_checks(self):
            spec = {"output_type": "float", "tolerance": 0.01}
            ref = struct.pack("<f", 1.0)
            self.assertTrue(compare_bytes(ref, struct.pack("<f", 1.005), spec)[0])
            self.assertFalse(compare_bytes(ref, struct.pack("<f", 1.1), spec)[0])
            self.assertFalse(compare_bytes(ref, b"", spec)[0])

        def test_missing_backends_fail_by_default(self):
            self.assertEqual(missing_equivalence_backends({}, ["spirv"]), ["glsl", "hlsl"])
            self.assertEqual(missing_equivalence_backends({}, ["spirv", "hlsl", "glsl"]), [])
            self.assertEqual(missing_equivalence_backends({"required_backends": ["spirv"]},
                                                        ["spirv"]), [])

        def test_validation_precedes_every_text_backend(self):
            source = Path(__file__).parent / "error_cases" / "struct_as_varying.bwsl"
            for mode in ("-spv", "-metal", "-hlsl", "-glsl", "-gles", "-all"):
                with self.subTest(mode=mode):
                    target = output / mode[1:]
                    target.mkdir(parents=True, exist_ok=True)
                    result = subprocess.run(
                        [str(compiler), str(source), mode, "-validation", "strict", "-o", str(target)],
                        capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                    self.assertIn("SPIR-V validation failed", result.stdout + result.stderr)
                    self.assertFalse(any(target.glob("*.metal")))
                    self.assertFalse(any(target.glob("*.hlsl")))
                    self.assertFalse(any(target.glob("*.glsl")))

    tests = unittest.defaultTestLoader.loadTestsFromTestCase(P1RegressionTests)
    result = unittest.TextTestRunner(verbosity=1).run(tests)
    failures = len(result.failures) + len(result.errors)
    return result.testsRun - failures, failures
