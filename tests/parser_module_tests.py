"""Behavioral coverage for module discovery, watch invalidation and diagnostics."""
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

        def test_intrinsic_name_error_points_into_imported_module(self):
            module = self.base / "ReservedFunctions.bwsl"
            module.write_text("module ReservedFunctions {\n"
                              "    lerp :: (float x) -> float { return x; }\n}\n")
            shader = self.base / "shader.bwsl"
            shader.write_text("pipeline ReservedTest { import ReservedFunctions }\n")
            result = subprocess.run(
                [str(compiler), str(shader), "-check", "-errors-json"],
                cwd=self.base, text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            diagnostics = json.loads(result.stdout)["diagnostics"]
            error = next(d for d in diagnostics if "reserved for a built-in intrinsic" in d["message"])
            self.assertEqual(Path(error["file"]).resolve(), module.resolve())
            self.assertEqual((error["line"], error["column"]), (2, 5))
            self.assertEqual((error["endLine"], error["endColumn"]), (2, 9))
            self.assertIn("lerp ::", next(line["text"] for line in error["context"]
                                        if line["line"] == 2))

        def test_module_type_beats_foreign_short_name_constant(self):
            for name in ("T", "U", "V"):
                with self.subTest(name=name):
                    shader = self.base / f"type_{name}.bwsl"
                    shader.write_text(
                        f"module Constants {{ const float {name} = 5.0; }}\n"
                        f"module Types {{ struct {name} {{ float value; }} "
                        f"struct Outer {{ {name} item; }} "
                        "evaluate :: (Outer x) -> float { return x.item.value; } }\n"
                        "pipeline Test { import Types resources { result: buffer<float> } "
                        'pass "Main" { use resources { result } compute "Main" [1,1,1] { '
                        "Types::Outer x; x.item.value = 3.0; "
                        "resources.result[0] = Types::evaluate(x); } } }")
                    binaries = self.compile([shader], self.base / f"out_{name}",
                                            "-validation", "strict")
                    self.assertTrue(binaries)

        def test_transitive_modules_keep_local_type_qualifiers_in_their_source_file(self):
            # Each qualified field in Collision occupies the same line/column
            # as a different local declaration in Wrapper. Import order must
            # not qualify Wrapper's built-in or unqualified local types.
            (self.base / "Dependency.bwsl").write_text(
                "module Dependency { struct Box { float value; } "
                "base :: () -> float { return 1.0; } }\n")
            (self.base / "Collision.bwsl").write_text(
                "module Collision {\n"
                "import Dependency\n"
                "struct Fields {\n"
                "    Dependency::Box scalar;\n"
                "          Dependency::Box constant;\n"
                "    Dependency::Box array;\n"
                "    Dependency::Box custom;\n"
                "    Dependency::Box iterator;\n"
                "}\n}\n")
            wrapper = self.base / "Wrapper.bwsl"
            wrapper.write_text(
                "module Wrapper {\n"
                "import Dependency import Collision\n"
                "value :: () -> float {\n"
                "    float2 vector = float2(1.0);\n"
                "    const float amount = 2.0;\n"
                "    float[2] array = {3.0, 4.0};\n"
                "    Box local;\n"
                "    for (int index = 0; index < 1; index++) { local.value = amount; }\n"
                "    return vector.x + array[0] + local.value + Dependency::base();\n"
                "}\n"
                "struct Box { float value; }\n}\n")
            shader = self.base / "shader.bwsl"
            shader.write_text(
                "pipeline Test { import Wrapper\n"
                'pass "Main" {\n'
                "vertex { output.position = float4(Wrapper::value()); }\n"
                "fragment { output.color = float4(Wrapper::value()); }\n"
                "}\n}\n")
            for mode in ("-check", "-ast-json"):
                result = subprocess.run(
                    [str(compiler), str(shader), mode], cwd=self.base,
                    text=True, capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                if mode == "-ast-json":
                    data = json.loads(result.stdout)
                    def objects(value):
                        if isinstance(value, dict):
                            yield value
                            for child in value.values():
                                yield from objects(child)
                        elif isinstance(value, list):
                            for child in value:
                                yield from objects(child)
                    module = next(m for m in data["modules"] if m["name"] == "Wrapper")
                    locals = {n["name"]: n for n in objects(module)
                              if n.get("type") == "VARIABLE_DECL"}
                    expected = {"vector": "float2", "amount": "float", "array": "float",
                                "local": "Box", "index": "int"}
                    for name, type_name in expected.items():
                        node = locals[name]
                        self.assertEqual(node["declaredType"], type_name)
                        self.assertNotIn("typeQualifier", node)
                        self.assertEqual(Path(node["sourceFile"]).resolve(), wrapper.resolve())
                        self.assertFalse(any(r["from"] == node["id"] + "/type-qualifier"
                                             for r in data["referenceIndex"]["references"]))
            binaries = self.compile([shader], self.base / "compiled", "-gles",
                                    "-validation", "strict")
            self.assertEqual(len(binaries), 2)
            # Explicitly importing the transitive dependencies is only a
            # workaround; it must not change either shader's generated code.
            shader.write_text(shader.read_text().replace(
                "import Wrapper", "import Wrapper import Dependency import Collision"))
            explicit = self.compile([shader], self.base / "explicit", "-gles",
                                    "-validation", "strict")
            self.assertEqual(binaries, explicit)

        def test_same_module_constant_and_type_remain_duplicates(self):
            shader = self.base / "duplicate.bwsl"
            shader.write_text("module Types { const float V = 5.0; "
                              "struct V { float value; } }")
            result = subprocess.run(
                [str(compiler), str(shader), "-check"], cwd=self.base,
                text=True, capture_output=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Duplicate struct declaration", result.stdout + result.stderr)

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
