#!/usr/bin/env python3
"""Check paired-stage compilation protocol, timing boundaries and linkage guards."""

from functools import partial
import hashlib
from http.server import ThreadingHTTPServer
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import benchmark_wgsl_render as RENDER

LOG = '''  Lexer + Parser: 0.100 ms
  Per-shader breakdown:
    demo.vert:
      IR lowering: 0.200 ms
      WGSL emit: 0.010 ms
    demo.frag:
      IR lowering: 0.300 ms
      WGSL emit: 0.020 ms
  Summary:
    BWSL compile: 0.630 ms
    WGSL emit: 0.030 ms
    Total: 1.500 ms
'''


class RenderProtocolTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.compiler = self.directory / "compiler"
        self.compiler.write_text("compiler fixture")
        (self.directory / "demo").mkdir()
        (self.directory / "demo/source.bwsl").write_text("BWSL source snapshot")
        self.stages = {}
        for stage, suffix in RENDER.STAGES.items():
            source = f"{stage} source snapshot"
            self.stages[stage] = {"source": source, "sha256": hashlib.sha256(source.encode()).hexdigest()}
            (self.directory / "demo" / f"handwritten.{suffix}.wgsl").write_text(source)
        self.manifest = {"result_token": "render-token", "compiler": {"path": str(self.compiler)},
                         "cases": [{"name": "demo", "variants": {name: self.stages for name in ("handwritten", "bwsl")}}]}
        stat = self.compiler.stat()
        handler = partial(RENDER.RenderHandler, directory=str(self.directory), manifest=self.manifest,
                          compiler_signature=(stat.st_size, stat.st_mtime_ns))
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.01})
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temporary.cleanup()

    def post(self, body, token="render-token", path="/compile"):
        request = Request(f"http://127.0.0.1:{self.server.server_port}{path}", data=json.dumps(body).encode(),
                          headers={"Content-Type": "application/json", "X-BWSL-Result-Token": token})
        with urlopen(request, timeout=5) as response:
            return json.load(response)

    def test_one_native_compilation_supplies_both_verified_stages(self):
        with patch.object(RENDER, "compile_pipeline", return_value=(self.stages, LOG)) as compiler:
            result = self.post({"case": "demo", "variant": "bwsl"})
            compiler.assert_called_once()
            self.assertEqual(compiler.call_args.args[0], self.compiler)
            self.assertEqual(compiler.call_args.args[1], self.directory / "demo/source.bwsl")
        self.assertEqual(set(result["stages"]), {"vertex", "fragment"})
        self.assertEqual(result["bwsl_reported_phases_us"]["stages"]["vert"]["IR lowering"], 200)
        self.assertEqual(result["bwsl_reported_phases_us"]["stages"]["frag"]["IR lowering"], 300)
        self.assertEqual(result["bwsl_reported_phases_us"]["summary"]["WGSL emit"], 30)
        self.assertEqual(list(self.directory.glob("render-compile-*")), [])

    def test_inprocess_requests_use_the_persistent_worker_and_retain_native_timing(self):
        worker = unittest.mock.Mock()
        phases = {"inprocess": {"parse": 12}, "summary": {"Fresh source in process": 100}}
        worker.compile.return_value = (self.stages, phases)
        stat = self.compiler.stat()
        self.server.RequestHandlerClass = partial(RENDER.RenderHandler, directory=str(self.directory),
            manifest=self.manifest, compiler_signature=(stat.st_size, stat.st_mtime_ns), workers={"demo": worker})
        with patch.object(RENDER, "compile_pipeline") as cli:
            result = self.post({"case": "demo", "variant": "bwsl"})
        cli.assert_not_called()
        worker.compile.assert_called_once_with()
        self.assertEqual(result["bwsl_reported_phases_us"], phases)
        self.assertEqual(result["stages"], self.stages)

    def test_both_reference_snapshots_are_checked_and_requests_are_scoped(self):
        result = self.post({"case": "demo", "variant": "handwritten"})
        self.assertEqual(result["stages"], self.stages)
        (self.directory / "demo/handwritten.frag.wgsl").write_text("changed")
        for body, token, status in [
            ({"case": "demo", "variant": "handwritten"}, "render-token", 400),
            ({"case": "../demo", "variant": "bwsl"}, "render-token", 400),
            ({"case": "demo", "variant": "bwsl"}, "wrong-token", 403),
        ]:
            with self.assertRaises(HTTPError) as failure:
                self.post(body, token)
            self.assertEqual(failure.exception.code, status)

    def test_render_report_schema_is_saved(self):
        report = {"schema_version": 2, "kind": "render", "status": "passed"}
        result = self.post(report, path="/results")
        self.assertEqual(json.loads((self.directory / result["saved"]).read_text()), report)

    def test_linkage_keeps_sparse_locations_and_rejects_conflicting_types(self):
        varying = {"location": 3, "type": "FLOAT2", "interpolation": "linear"}
        stages = {"vertex": {"interface": {"outputs": [varying], "resources": []}},
                  "fragment": {"interface": {"inputs": [dict(varying)], "resources": []}}}
        RENDER.verify_linkage(stages)
        stages["fragment"]["interface"]["inputs"][0]["type"] = "FLOAT4"
        with self.assertRaisesRegex(ValueError, "interface mismatch"):
            RENDER.verify_linkage(stages)
        stages["fragment"]["interface"]["inputs"][0] = {**varying, "location": 0}
        with self.assertRaisesRegex(ValueError, "interface mismatch"):
            RENDER.verify_linkage(stages)


if __name__ == "__main__":
    unittest.main()
