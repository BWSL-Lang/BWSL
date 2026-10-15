#!/usr/bin/env python3
"""Exercise the benchmark's source compilation and result protocol over HTTP."""

from functools import partial
import hashlib
from http.server import ThreadingHTTPServer
import importlib.util
import json
from pathlib import Path
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("benchmark_web", ROOT / "scripts/benchmark_wgsl_web.py")
WEB = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(WEB)


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.compiler = self.directory / "compiler"
        self.compiler.write_text("Compiler identity fixture")
        self.source = "@compute @workgroup_size(64) fn main() {}\n"
        digest = hashlib.sha256(self.source.encode()).hexdigest()
        (self.directory / "kernel").mkdir()
        (self.directory / "kernel/source.bwsl").write_text("Source snapshot")
        (self.directory / "kernel/handwritten.wgsl").write_text(self.source)
        self.manifest = {
            "result_token": "test-token",
            "compiler": {"path": str(self.compiler)},
            "cases": [{"name": "kernel", "variants": {
                name: {"sha256": digest} for name in ("handwritten", "bwsl")
            }}],
        }
        stat = self.compiler.stat()
        handler = partial(
            WEB.BenchmarkHandler, directory=str(self.directory), manifest=self.manifest,
            compiler_signature=(stat.st_size, stat.st_mtime_ns),
        )
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.01})
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temporary.cleanup()

    def post(self, path, body, token="test-token"):
        request = Request(
            f"http://127.0.0.1:{self.server.server_port}{path}",
            data=json.dumps(body).encode(),
            headers={"Content-Type": "application/json", "X-BWSL-Result-Token": token},
        )
        with urlopen(request, timeout=5) as response:
            return json.load(response)

    def assert_rejected(self, body, code=400, token="test-token"):
        with self.assertRaises(HTTPError) as failure:
            self.post("/compile", body, token)
        self.assertEqual(failure.exception.code, code)

    def test_handwritten_source_is_snapshotted_and_verified(self):
        result = self.post("/compile", {"case": "kernel", "variant": "handwritten"})
        self.assertEqual(result["source"], self.source)
        self.assertGreaterEqual(result["server_source_us"], 0)
        (self.directory / "kernel/handwritten.wgsl").write_text("Changed source")
        self.assert_rejected({"case": "kernel", "variant": "handwritten"})

    def test_bwsl_invocation_uses_fixed_arguments_and_records_phases(self):
        def compile_fixture(command, **kwargs):
            self.assertEqual(command[0], str(self.compiler))
            self.assertEqual(command[1], str(self.directory / "kernel/source.bwsl"))
            self.assertEqual(command[2:7], ["-wgsl", "-validation", "off", "-timing", "-o"])
            (Path(command[7]) / "kernel.comp.wgsl").write_text(self.source)
            return SimpleNamespace(returncode=0, stdout="  BWSL compile: 0.125 ms\n", stderr="")
        with patch.object(WEB.subprocess, "run", side_effect=compile_fixture):
            result = self.post("/compile", {"case": "kernel", "variant": "bwsl"})
        self.assertEqual(result["source"], self.source)
        self.assertEqual(result["bwsl_reported_phases_us"], {"BWSL compile": 125})
        self.assertEqual(list(self.directory.glob("compile-*")), [])

    def test_invalid_sources_tokens_and_changed_compiler_are_rejected(self):
        self.assert_rejected({"case": "kernel", "variant": "bwsl"}, 403, "wrong")
        self.assert_rejected({"case": "../kernel", "variant": "bwsl"})
        self.assert_rejected({"case": "kernel", "variant": "arbitrary"})
        self.assert_rejected([])
        self.compiler.write_text("Different compiler identity")
        self.assert_rejected({"case": "kernel", "variant": "bwsl"})

    def test_baseline_reference_runs_its_own_compiler_on_the_same_source(self):
        baseline = self.directory / "baseline-compiler"
        baseline.write_text("Baseline compiler fixture")
        stat = baseline.stat()
        self.manifest["reference_compiler"] = {"path": str(baseline)}
        self.server.RequestHandlerClass.keywords["reference_signature"] = (stat.st_size, stat.st_mtime_ns)
        def compile_fixture(command, **kwargs):
            self.assertEqual(command[0], str(baseline))
            self.assertEqual(command[1], str(self.directory / "kernel/source.bwsl"))
            (Path(command[-1]) / "kernel.comp.wgsl").write_text(self.source)
            return SimpleNamespace(returncode=0, stdout="", stderr="")
        with patch.object(WEB.subprocess, "run", side_effect=compile_fixture):
            result = self.post("/compile", {"case": "kernel", "variant": "handwritten"})
        self.assertEqual(result["source"], self.source)

    def test_reports_are_saved_without_overwriting_sources(self):
        report = {"schema_version": 1, "status": "passed", "cases": {}}
        first = self.post("/results", report)
        second = self.post("/results", report)
        self.assertNotEqual(first["saved"], second["saved"])
        self.assertEqual(json.loads((self.directory / first["saved"]).read_text()), report)
        self.assertEqual((self.directory / "kernel/handwritten.wgsl").read_text(), self.source)


if __name__ == "__main__":
    unittest.main()
