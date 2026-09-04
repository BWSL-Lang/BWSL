"""Behavioral coverage for cached submodule discovery and watch invalidation."""
from __future__ import annotations

import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import tempfile
import threading
import unittest


PARENT = "module IndexParent { base :: () -> float { return 1.0; } }\n"
EXTENSION = """submodule IndexExtra extends IndexParent {
    value :: () -> float { return base() + OFFSET; }
}
"""
SHADER = """pipeline IndexTest {
    import IndexParent
    resources { output: buffer<float> }
    pass "Compute" {
        use resources { output }
        compute "Main" [1, 1, 1] {
            resources.output[0] = IndexParent::value();
        }
    }
}
"""


def run_parser_module_tests(compiler: Path) -> tuple[int, int]:
    class ModuleIndexTests(unittest.TestCase):
        def setUp(self):
            self.temp = tempfile.TemporaryDirectory(prefix="bwsl-module-index-")
            self.addCleanup(self.temp.cleanup)
            self.base = Path(self.temp.name)

        def compile(self, inputs, output, *options):
            result = subprocess.run(
                [str(compiler), *map(str, inputs), "-spv", "-validation", "off",
                 "-o", str(output), *map(str, options)],
                cwd=self.base, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout)
            return {p.name: p.read_bytes() for p in output.rglob("*.spv")}

        def test_batch_preserves_search_scope_and_alias_deduplication(self):
            inputs = []
            expected = {}
            for scope, offset in (("left", "2.0"), ("right", "9.0")):
                directory = self.base / scope
                directory.mkdir()
                (directory / "IndexParent.bwsl").write_text(PARENT)
                (directory / "Extra.bwsl").write_text(EXTENSION.replace("OFFSET", offset))
                # The scanner must honor comments, including misleading headers.
                (directory / "unrelated.bwsl").write_text(
                    "/* submodule Fake extends IndexParent { invalid } */\n")
                for n in range(2):
                    shader = directory / f"{scope}_{n}.bwsl"
                    shader.write_text(SHADER)
                    inputs.append(shader)
                    output = self.base / f"individual_{scope}_{n}"
                    expected.update(self.compile([shader], output))
            actual = self.compile(inputs, self.base / "batch")
            self.assertEqual(actual, expected)
            self.assertTrue(actual)
            self.assertNotEqual(actual["left_0.comp.spv"], actual["right_0.comp.spv"])
            # Two spellings of one root must not register the extension twice.
            alias = self.compile(inputs[:2], self.base / "aliases",
                                 "-modules", self.base / "left",
                                 "-modules", str(self.base / "left") + "/.")
            self.assertEqual(alias, {k: v for k, v in expected.items() if k.startswith("left_")})
            if os.name != "nt":
                link = self.base / "linked_root"
                link.symlink_to(self.base / "left", target_is_directory=True)
                (self.base / "left" / "linked_extra.bwsl").symlink_to(
                    self.base / "left" / "Extra.bwsl")
                linked = self.compile(inputs[:2], self.base / "symlinks", "-modules", link)
                self.assertEqual(linked, alias)

        def test_watch_discovers_new_roots_and_invalidates_negative_files(self):
            shaders = self.base / "shaders"
            shaders.mkdir()
            parent_dir = self.base / "parents"
            parent_dir.mkdir()
            (parent_dir / "IndexParent.bwsl").write_text(PARENT)
            shader = shaders / "job.bwsl"
            shader.write_text(SHADER)
            candidate = shaders / "candidate.bwsl"
            candidate.write_text("// No submodule yet.\n")
            output = self.base / "output"
            proc = subprocess.Popen(
                [str(compiler), str(shader), "-modules", str(parent_dir),
                 "-spv", "-validation", "off", "-o", str(output),
                 "-errors-json", "-watch", "-watch-interval", "50"],
                cwd=self.base, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL)
            documents = queue.Queue()

            def read_documents():
                pending = []
                for line in proc.stdout:
                    pending.append(line)
                    if line.rstrip("\n") == "}":
                        try:
                            documents.put(json.loads("".join(pending)))
                        except json.JSONDecodeError:
                            documents.put({"invalid": "".join(pending)})
                        pending.clear()

            reader = threading.Thread(target=read_documents, daemon=True)
            reader.start()

            def stop():
                if proc.poll() is None:
                    if os.name == "nt":
                        proc.terminate()
                    else:
                        proc.send_signal(signal.SIGINT)
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                reader.join(timeout=2)
                proc.stdout.close()

            self.addCleanup(stop)

            def event(success):
                try:
                    doc = documents.get(timeout=10)
                except queue.Empty:
                    self.fail("Watch mode did not emit a rebuild event")
                self.assertEqual(doc.get("success"), success, doc)
                self.assertEqual(doc.get("fileCount"), 1, doc)
                return doc

            event(False)  # Parent is known; its extension is initially absent.
            implicit_root = self.base / "modules"
            implicit_root.mkdir()
            extension = implicit_root / "extra.bwsl"
            extension.write_text(EXTENSION.replace("OFFSET", "2.0"))
            event(True)
            original = (output / "job.comp.spv").read_bytes()
            extension.write_text(EXTENSION.replace("OFFSET", "7.0"))
            event(True)
            self.assertNotEqual(original, (output / "job.comp.spv").read_bytes())
            extension.unlink()
            event(False)
            candidate.write_text(EXTENSION.replace("OFFSET", "2.0"))
            event(True)
            self.assertEqual(original, (output / "job.comp.spv").read_bytes())
            candidate.write_text("// This used to be a submodule.\n")
            event(False)
            candidate.write_text(EXTENSION.replace("OFFSET", "2.0"))
            event(True)
            self.assertEqual(original, (output / "job.comp.spv").read_bytes())

    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(ModuleIndexTests))
    failed = len(result.failures) + len(result.errors)
    return result.testsRun - failed, failed


if __name__ == "__main__":
    import sys
    root = Path(__file__).resolve().parent.parent
    compiler = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/bwslc"
    _, failed = run_parser_module_tests(compiler)
    raise SystemExit(bool(failed))
