"""A uniform struct's HLSL cbuffer layout matches its std140 offsets.

HLSL packs a scalar or vector into what is left of the previous 16-byte row,
and packs the next member into the unused end of an array, a matrix or a
struct; std140 does neither. The HLSL output pads uniform structs so that
DXC places every member at its std140 offset. DXC's assembly listing of the
compiled shader gives the offsets it chose.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

DXC = os.environ.get("BWSL_DXC", "dxc")

SHADER = """
pipeline UniformPacking {
    struct Inner { float k; float2 m; };
    struct Tail { float t; };
    struct Params {
        float a;
        float3 b;
        float[3] w;
        float c;
        mat3 m3;
        float d;
        mat2 m2;
        float e;
        mat3[2] ms;
        float f;
        Inner inner;
        float g;
        float2[2] v;
        float2 h;
        Tail tail;
        float i;
    };
    resources {
        params: Params
        output: buffer<float>
    }
    pass "Main" {
        use resources { params, output }
        compute "Main" [1, 1, 1] {
            Params p = resources.params;
            resources.output[0] = p.a + p.b.y + p.w[2] + p.c + p.m3[1].y + p.d + p.m2[1].x + p.e
                + p.ms[1][2].z + p.f + p.inner.m.y + p.g + p.v[1].y + p.h.x + p.tail.t + p.i;
        }
    }
}
"""

# std140 offsets, in the order DXC lists them: a nested struct's members
# come before the struct member itself.
STD140_OFFSETS = [
    ("a", 0), ("b", 16), ("w", 32), ("c", 80), ("m3", 96), ("d", 144),
    ("m2", 160), ("e", 192), ("ms", 208), ("f", 304),
    ("k", 320), ("m", 328), ("inner", 320), ("g", 336),
    ("v", 352), ("h", 384), ("t", 400), ("tail", 400), ("i", 416),
]

LISTED_MEMBER = re.compile(r"^;\s+(?:\}|[\w ]+?)\s+(\w+)(?:\[\d+\])*;\s+; Offset:\s+(\d+)")


def run_hlsl_cbuffer_tests(compiler: Path) -> tuple[int, int]:
    compiler = Path(compiler).resolve()

    class HLSLCbufferTests(unittest.TestCase):
        def setUp(self):
            if shutil.which(DXC) is None:
                self.skipTest("dxc is not on PATH")
            self.temp = tempfile.TemporaryDirectory(prefix="bwsl-hlsl-cbuffer-")
            self.addCleanup(self.temp.cleanup)
            self.folder = Path(self.temp.name)

        def test_uniform_struct_members_sit_at_std140_offsets(self):
            source = self.folder / "packing.bwsl"
            source.write_text(SHADER, encoding="utf-8")
            result = subprocess.run(
                [str(compiler), str(source), "-hlsl", "-o", str(self.folder)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            listing = self.folder / "listing.txt"
            result = subprocess.run(
                [DXC, "-T", "cs_6_0", "-E", "main", str(self.folder / "packing.comp.hlsl"),
                 "-Fc", str(listing)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            lines = listing.read_text(encoding="utf-8").splitlines()
            start = next(i for i, line in enumerate(lines) if line.startswith("; cbuffer ub_params"))
            members = []
            for line in lines[start:]:
                match = LISTED_MEMBER.match(line)
                if match and not match.group(1).startswith("bwsl_pad"):
                    members.append((match.group(1), int(match.group(2))))
                if line.startswith("; }"):
                    break
            # The struct, then the cbuffer, close the listing.
            self.assertEqual(members[:-2], STD140_OFFSETS)

    result = unittest.TextTestRunner(verbosity=1).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(HLSLCbufferTests))
    failures = len(result.failures) + len(result.errors)
    return result.testsRun - failures - len(result.skipped), failures


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build" / "bwslc")
    args = parser.parse_args()
    _, failures = run_hlsl_cbuffer_tests(args.compiler)
    raise SystemExit(bool(failures))
