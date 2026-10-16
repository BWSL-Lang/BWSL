"""Memory writes must not redefine a live SSA value in register zero."""
from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from local_debug_names_tests import shader


COMPUTE = '''pipeline StoreDestination {
    struct Payload { float weight; }
    resources { values: buffer<float4> output: buffer<float4> }
    pass "Compute" {
        use resources { values, output }
        compute "Main" [4, 1, 1] {
            float4 color = resources.values[input.global_id.x];
            Payload payload;
            payload.weight = 0.0;
            for (int i = 0; i < 3; i++) {
                if (color.x > 0.0) { color += float4(2.0); }
                else { color -= float4(1.0); }
                payload.weight = color.y;
            }
            resources.output[input.global_id.x] = color * payload.weight;
        }
    }
}
'''


def run_ssa_store_tests(compiler: Path, runner: Path | None = None) -> tuple[int, int]:
    compiler = Path(compiler).resolve()

    class SSAStoreTests(unittest.TestCase):
        def setUp(self):
            self.temp = tempfile.TemporaryDirectory(prefix="bwsl-ssa-store-")
            self.addCleanup(self.temp.cleanup)
            self.folder = Path(self.temp.name)

        def compile(self, code):
            source = self.folder / "store.bwsl"
            source.write_text(code, encoding="utf-8")
            target = self.folder / "output"
            result = subprocess.run(
                [str(compiler), str(source), "-all", "-validation", "strict", "-o", str(target)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return target

        def test_struct_field_write_preserves_live_scalar_and_vector(self):
            # The first declaration occupies r0 and needs SSA renaming. A
            # LOCAL_STORE has a dummy dest=0; treating that as a definition
            # replaces `color` with a value the store never produces.
            for kind in ("float", "float4"):
                with self.subTest(kind=kind):
                    self.compile(shader('''
                        KIND color = KIND(input.uv.y);
                        if (input.uv.x > 0.5) { color = KIND(input.uv.x); }
                        Payload payload;
                        payload.weight = input.uv.y;
                        output.color = float4(color * payload.weight);
                    '''.replace("KIND", kind), "struct Payload { float weight; }"))

        def test_struct_field_writes_in_loop_preserve_live_vector(self):
            self.compile(COMPUTE)

        @unittest.skipIf(runner is None, "native execution requires the Vulkan equivalence runner")
        def test_struct_field_writes_numeric_output(self):
            target = self.compile(COMPUTE)
            values = [(1., 2., 3., 4.), (-1., 2., -3., 4.),
                      (0., -2., 3., -4.), (2., 0., -1., .5)]
            source, destination = target / "input.bin", target / "output.bin"
            source.write_bytes(struct.pack("<16f", *(x for row in values for x in row)))
            result = subprocess.run(
                [str(runner.resolve()), "--input", str(source), "--input-binding", "0",
                 "--output", str(destination), "--output-size", "64", "--output-binding", "1",
                 "--set", "1", "--pass-spirv", str(target / "store.comp.spv"),
                 "--pass-groups", "1", "1", "1"],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            expected = []
            for row in values:
                color = list(row)
                for _ in range(3):
                    delta = 2. if color[0] > 0. else -1.
                    color = [x + delta for x in color]
                expected.extend(x * color[1] for x in color)
            self.assertEqual(struct.unpack("<16f", destination.read_bytes()), tuple(expected))

    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(SSAStoreTests))
    failures = len(result.failures) + len(result.errors)
    return result.testsRun - failures - len(result.skipped), failures


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=Path(__file__).resolve().parents[1] / "build/bwslc")
    parser.add_argument("--runner", type=Path)
    args = parser.parse_args()
    _, failures = run_ssa_store_tests(args.compiler, args.runner)
    raise SystemExit(bool(failures))
