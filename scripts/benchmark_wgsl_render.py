#!/usr/bin/env python3
"""Serve paired native BWSL/WGSL render pipelines for real browser benchmarks."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
from functools import partial
import hashlib
from http.server import ThreadingHTTPServer
import json
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
import re
import threading

from benchmark_wgsl_web import BenchmarkHandler, ROOT, TIMING, sha256


FIXTURES = ROOT / "tests/wgsl_render/benchmarks"
ASSETS = ROOT / "tools/wgsl-bench/render-web"
CASES = json.loads((FIXTURES / "corpus.json").read_text())
CHECKS = {"interfaces": {"label": "Stage interface regression", "targets": ["rgba8unorm", None, "rgba32uint"],
                          "topology": "triangle-list", "depth": True, "validation_only": True},
          "nested_continue": {"label": "Constant nested-loop continue regression", "targets": ["rgba8unorm"],
                              "topology": "triangle-list", "depth": False, "validation_only": True},
          "texture_array_layers": {"label": "Fractional texture-array layer regression", "targets": ["rgba8unorm"],
                                   "topology": "triangle-list", "depth": False, "validation_only": True}}
STAGES = {"vertex": "vert", "fragment": "frag"}


def reported_phases(log: str) -> dict:
    """Preserve stage boundaries so nested measurements are never added twice."""
    frontend = log.split("Per-shader breakdown:", 1)[0]
    summary = log.split("  Summary:", 1)[1] if "  Summary:" in log else ""
    stages = re.findall(r"^    [^\n]+\.(vert|frag):\n(.*?)(?=^    [^\n]+\.(?:vert|frag):|^  Summary:)",
                        log, re.MULTILINE | re.DOTALL)
    convert = lambda text: {key: float(value) * 1000 for key, value in TIMING.findall(text)}
    return {"frontend": convert(frontend), "stages": {stage: convert(text) for stage, text in stages},
            "summary": convert(summary)}


def compile_pipeline(compiler: Path, source: Path, directory: Path) -> tuple[dict, str]:
    command = [str(compiler), str(source), "-wgsl", "-bindings", "-validation", "off", "-timing", "-o", str(directory)]
    process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=30)
    if process.returncode:
        raise ValueError(f"Render compilation failed:\n{process.stdout}{process.stderr}")
    if list(directory.glob("*.spv")):
        raise ValueError("Direct render compilation unexpectedly emitted SPIR-V")
    result = {}
    for stage, suffix in STAGES.items():
        files = list(directory.glob(f"*.{suffix}.wgsl"))
        if len(files) != 1:
            raise ValueError(f"Expected one {stage} WGSL artifact")
        path = files[0]
        bindings = json.loads(Path(str(path) + ".bindings.json").read_text())
        if bindings.get("stage") != stage or bindings.get("entryPoint") != "main":
            raise ValueError("Invalid reflected render entry point")
        result[stage] = {"source": path.read_text(), "sha256": sha256(path), "interface": bindings}
    verify_linkage(result)
    return result, process.stdout + process.stderr


def verify_linkage(stages: dict):
    vertex_outputs = {item["location"]: item for item in stages["vertex"]["interface"]["outputs"] if item["location"] is not None}
    for item in stages["fragment"]["interface"]["inputs"]:
        if item["location"] is None:
            continue
        output = vertex_outputs.get(item["location"])
        if output is None or any(output[key] != item[key] for key in ("type", "interpolation")):
            raise ValueError(f"Vertex/fragment interface mismatch: {item}")
    resources = {}
    for stage in stages.values():
        for item in stage["interface"]["resources"]:
            key = (item["group"], item["binding"])
            abi = ("type", "elementType", "stride", "access", "byteSize", "arrayLength", "structTypeHash",
                   "viewDimension", "sampleType", "comparison", "defaultForTextureSlot")
            if key in resources and any(resources[key].get(field) != item.get(field) for field in abi):
                raise ValueError("Resource ABI differs between render stages")
            if item.get("access", "read") != "read" or item["group"] not in range(4):
                raise ValueError("This render corpus requires read-only resources in groups 0 through 3")
            resources[key] = item


def prepare(compiler: Path, directory: Path, selected: list[str], tools: dict, repetitions: int):
    directory.mkdir(parents=True, exist_ok=False)
    manifest = {
        "schema_version": 2, "kind": "render",
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "result_token": secrets.token_urlsafe(32),
        "compiler": {"path": str(compiler), "sha256": sha256(compiler)},
        "server_harness_sha256": sha256(Path(__file__)),
        "shared_server_sha256": sha256(ROOT / "scripts/benchmark_wgsl_web.py"),
        "corpus_sha256": sha256(FIXTURES / "corpus.json"),
        "git_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "git_status": subprocess.check_output(["git", "status", "--short"], cwd=ROOT, text=True),
        "reference_tools": {}, "cases": [],
    }
    for name in [*CHECKS, *selected]:
        specification = CHECKS.get(name, CASES.get(name))
        fixture_root = FIXTURES.parent if name in CHECKS else FIXTURES
        case_dir = directory / name
        case_dir.mkdir()
        source = fixture_root / f"{name}.bwsl"
        shutil.copy2(source, case_dir / "source.bwsl")
        stages, log = compile_pipeline(compiler, source, case_dir)
        (case_dir / "compile.log").write_text(log)
        reference = {}
        for stage, suffix in STAGES.items():
            shader = fixture_root / f"{name}.{suffix}.wgsl"
            shutil.copy2(shader, case_dir / f"handwritten.{suffix}.wgsl")
            reference[stage] = {"source": shader.read_text(), "sha256": sha256(shader)}
        case = {"name": name, **specification, "bwsl_source": source.read_text(),
                "bwsl_sha256": sha256(source), "variants": {"handwritten": reference, "bwsl": stages},
                "reference_validation": {}}
        for tool_name, executable in tools.items():
            manifest["reference_tools"][tool_name] = {"path": str(executable), "sha256": sha256(executable)}
            validation = {}
            for variant in ("handwritten", "bwsl"):
                validation[variant] = {}
                for stage, suffix in STAGES.items():
                    path = case_dir / (f"handwritten.{suffix}.wgsl" if variant == "handwritten" else f"{name}.{suffix}.wgsl")
                    process = subprocess.run([str(executable), "bench", str(path), "validate", str(repetitions), "5"],
                                             cwd=ROOT, capture_output=True, text=True, timeout=60)
                    if process.returncode:
                        raise ValueError(f"{tool_name} rejected {name}/{variant}/{stage}: {process.stderr}")
                    validation[variant][stage] = json.loads(process.stdout)
            case["reference_validation"][tool_name] = validation
        manifest["cases"].append(case)
    for asset in ASSETS.iterdir():
        if asset.is_file():
            shutil.copy2(asset, directory / asset.name)
    shutil.copy2(ROOT / "tools/wgsl-bench/web/statistics.mjs", directory / "statistics.mjs")
    manifest["harness_sha256"] = {
        path.name: sha256(path) for path in directory.iterdir() if path.suffix in {".mjs", ".html"}
    }
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


class CompilerWorker:
    """One preloaded source; each request builds fresh frontend/IR/arena state."""

    def __init__(self, executable: Path, source: Path):
        self.executable = executable
        stat = executable.stat()
        self.signature = (stat.st_size, stat.st_mtime_ns)
        self.lock = threading.Lock()
        self.process = subprocess.Popen([str(executable), "--serve", str(source)], cwd=ROOT,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, bufsize=1)
        if self.process.stdout.readline().strip() != '{"ready":true}':
            self.close()
            raise ValueError("In-process compiler worker did not start")

    def compile(self):
        with self.lock:
            stat = self.executable.stat()
            if self.signature != (stat.st_size, stat.st_mtime_ns):
                raise ValueError("In-process executable changed since preparation")
            self.process.stdin.write("compile\n")
            self.process.stdin.flush()
            response = self.process.stdout.readline()
            if not response:
                raise ValueError(f"In-process compiler failed: {self.process.stderr.read(4096)}")
            result = json.loads(response)
            stages = {}
            for output in result["outputs"]:
                bindings = json.loads(output["bindings"])
                stage = bindings["stage"]
                stages[stage] = {"source": output["source"], "interface": bindings,
                                 "sha256": hashlib.sha256(output["source"].encode()).hexdigest()}
            verify_linkage(stages)
            return stages, {"inprocess": result["phases_us"],
                            "summary": {"Fresh source in process": result["native_us"]}}

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        for pipe in (self.process.stdin, self.process.stdout, self.process.stderr):
            pipe.close()


class RenderHandler(BenchmarkHandler):
    def __init__(self, *args, workers=None, **kwargs):
        self.workers = workers or {}
        super().__init__(*args, **kwargs)

    def compile_source(self, request):
        if not isinstance(request, dict):
            raise ValueError("Expected a compile request")
        case = next((item for item in self.manifest["cases"] if item["name"] == request.get("case")), None)
        variant = request.get("variant")
        if case is None or variant not in {"handwritten", "bwsl"}:
            raise ValueError("Unknown render source")
        compiler = Path(self.manifest["compiler"]["path"])
        stat = compiler.stat()
        if variant == "bwsl" and (stat.st_size, stat.st_mtime_ns) != self.compiler_signature:
            raise ValueError("Compiler changed since preparation")
        start = time.perf_counter_ns()
        log = ""
        phases = None
        if variant == "bwsl" and case.get("bwsl_sha256") and not self.workers:
            if sha256(self.result_directory / case["name"] / "source.bwsl") != case["bwsl_sha256"]:
                raise ValueError("BWSL source snapshot changed since preparation")
        if variant == "bwsl" and self.workers:
            stages, phases = self.workers[case["name"]].compile()
        elif variant == "bwsl":
            with tempfile.TemporaryDirectory(prefix="render-compile-", dir=self.result_directory) as temporary:
                stages, log = compile_pipeline(compiler, self.result_directory / case["name"] / "source.bwsl", Path(temporary))
        else:
            stages = {}
            for stage, suffix in STAGES.items():
                path = self.result_directory / case["name"] / f"handwritten.{suffix}.wgsl"
                stages[stage] = {"source": path.read_text(), "sha256": sha256(path)}
        duration = (time.perf_counter_ns() - start) / 1000
        for stage in STAGES:
            if stages[stage]["sha256"] != case["variants"][variant][stage]["sha256"]:
                raise ValueError("Render source changed since preparation")
        # BWSL compile and WGSL emit have aggregate summary entries. Nested
        # per-stage labels retain the last occurrence and are not additive.
        phases = phases if phases is not None else reported_phases(log)
        payload = json.dumps({"stages": {stage: {key: data[key] for key in ("source", "sha256")}
                                           for stage, data in stages.items()}, "server_source_us": duration,
                              "bwsl_reported_phases_us": phases}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    suffix = ".exe" if sys.platform == "win32" else ""
    parser.add_argument("--compiler", type=Path, default=ROOT / f"build/bwslc{suffix}")
    parser.add_argument("--naga", type=Path, default=ROOT / f"build/wgsl-oracle/release/wgsl-oracle{suffix}")
    parser.add_argument("--tint", type=Path, default=ROOT / f"build/wgsl-tools/tint/bwsl-tint-bench{suffix}")
    parser.add_argument("--skip-reference-tools", action="store_true")
    parser.add_argument("--reference-runs", type=int, default=100)
    parser.add_argument("--case", action="append", choices=CASES)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--serve-existing", type=Path)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--inprocess", type=Path, help="Use a persistent source-preloaded native worker instead of CLI launches")
    args = parser.parse_args()
    if args.reference_runs < 1 or not 0 <= args.port <= 65535:
        parser.error("Use positive reference repetitions and port 0–65535")
    if args.output and args.serve_existing:
        parser.error("--output and --serve-existing are mutually exclusive")
    if args.serve_existing:
        directory = args.serve_existing.resolve()
        manifest = json.loads((directory / "manifest.json").read_text())
    else:
        compiler = args.compiler.resolve()
        if not compiler.is_file():
            parser.error("Compiler missing; run make build")
        tools = {} if args.skip_reference_tools else {"naga": args.naga.resolve(), "tint": args.tint.resolve()}
        for tool in tools.values():
            if not tool.is_file():
                parser.error(f"Reference tool missing: {tool}")
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        directory = (args.output or ROOT / "build/wgsl-render-benchmarks" / stamp).resolve()
        manifest = prepare(compiler, directory, args.case or list(CASES), tools, args.reference_runs)
    if not args.serve_existing:
        manifest["compilation_mode"] = "fresh-source-in-process" if args.inprocess else "fresh-cli"
        if args.inprocess:
            executable = args.inprocess.resolve()
            manifest["inprocess_compiler"] = {"path": str(executable), "sha256": sha256(executable)}
        (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Prepared render benchmark: {directory}", flush=True)
    if args.prepare_only:
        return
    compiler = Path(manifest["compiler"]["path"])
    if sha256(compiler) != manifest["compiler"]["sha256"]:
        parser.error("Compiler changed since preparation")
    stat = compiler.stat()
    workers = {}
    if manifest.get("compilation_mode") == "fresh-source-in-process":
        executable = Path(manifest["inprocess_compiler"]["path"])
        if sha256(executable) != manifest["inprocess_compiler"]["sha256"]:
            parser.error("In-process executable changed since preparation")
        try:
            for case in manifest["cases"]:
                if sha256(directory / case["name"] / "source.bwsl") != case["bwsl_sha256"]:
                    raise ValueError("BWSL source snapshot changed since preparation")
                worker = CompilerWorker(executable, directory / case["name"] / "source.bwsl")
                workers[case["name"]] = worker
                stages, _ = worker.compile()
                if any(stages[stage]["sha256"] != case["variants"]["bwsl"][stage]["sha256"] for stage in STAGES):
                    raise ValueError("In-process and CLI compilers disagree on generated WGSL")
        except Exception:
            for worker in workers.values():
                worker.close()
            raise
    handler = partial(RenderHandler, directory=str(directory), manifest=manifest,
                      compiler_signature=(stat.st_size, stat.st_mtime_ns), workers=workers)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    print(f"Open http://127.0.0.1:{server.server_port}/ in a WebGPU browser. Ctrl-C stops the server.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        for worker in workers.values():
            worker.close()


if __name__ == "__main__":
    main()
