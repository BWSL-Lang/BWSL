#!/usr/bin/env python3
"""Prepare matched WGSL sources and serve a browser-only WebGPU benchmark."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
from functools import partial
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/wgsl_benchmarks"
ASSETS = ROOT / "tools/wgsl-bench/web"
CASES = json.loads((FIXTURES / "corpus.json").read_text())
TIMING = re.compile(r"^\s*([A-Za-z][A-Za-z0-9 +_-]*):\s*([0-9.]+)\s+ms\s*$", re.MULTILINE)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(compiler: Path, directory: Path, selected: list[str], unrolled_reference: bool,
            baseline_compiler: Path | None) -> dict:
    directory.mkdir(parents=True, exist_ok=False)
    manifest = {
        "schema_version": 1,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "result_token": secrets.token_urlsafe(32),
        "compiler": {"path": str(compiler), "sha256": sha256(compiler)},
        "reference_compiler": {"path": str(baseline_compiler), "sha256": sha256(baseline_compiler)} if baseline_compiler else None,
        "reference_label": "Previous BWSL" if baseline_compiler else "Handwritten WGSL",
        "harness_sha256": {asset.name: sha256(asset) for asset in ASSETS.iterdir() if asset.is_file()},
        "git_revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
        ).strip(),
        "server_harness_sha256": sha256(Path(__file__)),
        "corpus_sha256": sha256(FIXTURES / "corpus.json"),
        "path": "BWSL source -> native WGSL; both WGSL variants go directly to browser WebGPU",
        "baseline_style": "Explicitly unrolled diagnostic WGSL where available" if unrolled_reference else "Idiomatic handwritten WGSL",
        "cases": [],
    }
    if baseline_compiler:
        manifest["baseline_style"] = "Previous BWSL compiler versus current BWSL compiler"
    for name in selected:
        specification = CASES[name]
        case_dir = directory / name
        case_dir.mkdir()
        source = FIXTURES / f"{name}.bwsl"
        handwritten = FIXTURES / f"{name}.wgsl"
        unrolled = FIXTURES / f"{name}_unrolled.wgsl"
        if unrolled_reference and unrolled.is_file():
            handwritten = unrolled
        shutil.copy2(source, case_dir / "source.bwsl")
        shutil.copy2(handwritten, case_dir / "handwritten.wgsl")
        command = [str(compiler), str(source), "-wgsl", "-bindings", "-validation", "off", "-o", str(case_dir)]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        (case_dir / "compile.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f"WGSL compilation failed for {name}:\n{result.stdout}{result.stderr}")
        outputs = list(case_dir.glob("*.comp.wgsl"))
        if len(outputs) != 1 or list(case_dir.glob("*.spv")):
            raise RuntimeError(f"Expected one direct WGSL artifact and no SPIR-V for {name}")
        generated = outputs[0]
        bindings = json.loads(Path(str(generated) + ".bindings.json").read_text())
        expected_count = len(specification["strides"])
        resources = sorted(bindings["resources"], key=lambda resource: resource["binding"])
        if bindings["entryPoint"] != "main" or bindings["workgroupSize"] != [64, 1, 1]:
            raise RuntimeError(f"Unexpected entry point or workgroup size for {name}")
        if [resource["binding"] for resource in resources] != list(range(expected_count)):
            raise RuntimeError(f"Unexpected binding layout for {name}")
        for index, resource in enumerate(resources):
            access = "read_write" if index == expected_count - 1 else "read"
            stride = specification["strides"][index]
            if resource["group"] != 0 or resource["access"] != access or resource["stride"] != stride:
                raise RuntimeError(f"Unexpected WGSL buffer ABI for {name}: {resource}")
        if baseline_compiler:
            baseline_dir = case_dir / "baseline"
            baseline_dir.mkdir()
            baseline_command = [str(baseline_compiler), str(source), "-wgsl", "-bindings", "-validation", "off", "-o", str(baseline_dir)]
            baseline = subprocess.run(baseline_command, cwd=ROOT, capture_output=True, text=True)
            (baseline_dir / "compile.log").write_text(baseline.stdout + baseline.stderr)
            if baseline.returncode:
                raise RuntimeError(f"Baseline compiler failed for {name}: {baseline.stdout}{baseline.stderr}")
            baseline_outputs = list(baseline_dir.glob("*.comp.wgsl"))
            if len(baseline_outputs) != 1 or list(baseline_dir.glob("*.spv")):
                raise RuntimeError(f"Expected one direct baseline WGSL artifact for {name}")
            handwritten = baseline_outputs[0]
            baseline_bindings = json.loads(Path(str(handwritten) + ".bindings.json").read_text())
            abi_fields = ("entryPoint", "workgroupSize", "resources")
            if any(baseline_bindings.get(field) != bindings.get(field) for field in abi_fields):
                raise RuntimeError(f"Baseline compiler changed the benchmark buffer ABI for {name}")
            shutil.copy2(handwritten, case_dir / "handwritten.wgsl")
        manifest["cases"].append({
            "name": name,
            "label": specification["label"],
            "category": specification["category"],
            "bwsl_source": source.read_text(),
            "bwsl_sha256": sha256(source),
            "entry_point": bindings["entryPoint"],
            "workgroup_size": bindings["workgroupSize"],
            "resources": resources,
            "variants": {
                "handwritten": {"source": handwritten.read_text(), "sha256": sha256(handwritten)},
                "bwsl": {"source": generated.read_text(), "sha256": sha256(generated)},
            },
        })
    for asset in ASSETS.iterdir():
        if asset.is_file():
            shutil.copy2(asset, directory / asset.name)
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


class BenchmarkHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, directory: str, manifest: dict, compiler_signature: tuple,
                 reference_signature: tuple | None = None, **kwargs):
        self.result_directory = Path(directory)
        self.manifest = manifest
        self.result_token = manifest["result_token"]
        self.compiler_signature = compiler_signature
        self.reference_signature = reference_signature
        super().__init__(*args, directory=directory, **kwargs)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()

    def do_POST(self):
        if self.path not in {"/results", "/compile"} or self.headers.get("X-BWSL-Result-Token") != self.result_token:
            self.send_error(403)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 8 * 1024 * 1024:
                raise ValueError("Invalid result length")
            result = json.loads(self.rfile.read(length))
            if self.path == "/compile":
                self.compile_source(result)
                return
            if not isinstance(result, dict) or result.get("schema_version") not in {1, 2} or result.get("status") not in {"passed", "failed", "cancelled", "unavailable"}:
                raise ValueError("Invalid benchmark report")
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
            path = self.result_directory / f"results-{stamp}.json"
            with path.open("x") as output:
                json.dump(result, output, indent=2)
                output.write("\n")
        except (ValueError, TypeError, OSError, subprocess.TimeoutExpired) as error:
            self.send_error(400, str(error))
            return
        payload = json.dumps({"saved": path.name}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)
        print(f"Browser report: {path}", flush=True)

    def compile_source(self, request: dict):
        if not isinstance(request, dict):
            raise ValueError("Expected a compile request")
        fixture = next((case for case in self.manifest["cases"] if case["name"] == request.get("case")), None)
        variant = request.get("variant")
        if fixture is None or variant not in {"handwritten", "bwsl"}:
            raise ValueError("Unknown benchmark source")
        reference = self.manifest.get("reference_compiler")
        translate = variant == "bwsl" or reference is not None
        compiler = Path((reference if variant == "handwritten" and reference else self.manifest["compiler"])["path"])
        signature = self.reference_signature if variant == "handwritten" else self.compiler_signature
        if translate:
            compiler_stat = compiler.stat()
            if (compiler_stat.st_size, compiler_stat.st_mtime_ns) != signature:
                raise ValueError("Compiler changed since preparation; prepare a new benchmark")
        start = time.perf_counter_ns()
        phases = {}
        with tempfile.TemporaryDirectory(prefix="compile-", dir=self.result_directory) as temporary:
            if translate:
                source = self.result_directory / fixture["name"] / "source.bwsl"
                command = [str(compiler), str(source), "-wgsl", "-validation", "off", "-timing", "-o", temporary]
                process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=30)
                if process.returncode:
                    raise ValueError(f"BWSL compile failed: {process.stdout}{process.stderr}")
                outputs = list(Path(temporary).glob("*.comp.wgsl"))
                if len(outputs) != 1 or list(Path(temporary).glob("*.spv")):
                    raise ValueError("Expected direct WGSL compilation")
                text = outputs[0].read_text()
                phases = {key: float(value) * 1000 for key, value in TIMING.findall(process.stdout)}
            else:
                text = (self.result_directory / fixture["name"] / "handwritten.wgsl").read_text()
        elapsed_us = (time.perf_counter_ns() - start) / 1000
        digest = hashlib.sha256(text.encode()).hexdigest()
        if digest != fixture["variants"][variant]["sha256"]:
            raise ValueError("Shader changed since preparation; prepare a new benchmark")
        payload = json.dumps({
            "source": text, "sha256": digest, "server_source_us": elapsed_us,
            "bwsl_reported_phases_us": phases,
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    suffix = ".exe" if sys.platform == "win32" else ""
    parser.add_argument("--compiler", type=Path, default=ROOT / f"build/bwslc{suffix}")
    parser.add_argument("--output", type=Path, help="New directory for sources, page and browser reports")
    parser.add_argument("--serve-existing", type=Path, help="Serve a previously prepared benchmark directory")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--case", action="append", choices=CASES, help="Repeat to select kernels; default is the whole corpus")
    parser.add_argument("--unrolled-reference", action="store_true", help="Use explicit WGSL unrolling controls for Gaussian and GGX")
    parser.add_argument("--baseline-compiler", type=Path, help="Compare a previous BWSL compiler against the current one, including full compilation")
    parser.add_argument("--port", type=int, default=0, help="Local port; default chooses a free port")
    args = parser.parse_args()
    if args.baseline_compiler and args.unrolled_reference:
        parser.error("--baseline-compiler and --unrolled-reference are mutually exclusive")
    if args.baseline_compiler:
        args.baseline_compiler = args.baseline_compiler.resolve()
        if not args.baseline_compiler.is_file():
            parser.error(f"Missing baseline compiler: {args.baseline_compiler}")
    if not 0 <= args.port <= 65535:
        parser.error("--port must be between 0 and 65535")
    if args.serve_existing:
        if args.output:
            parser.error("--serve-existing and --output are mutually exclusive")
        directory = args.serve_existing.resolve()
        manifest = json.loads((directory / "manifest.json").read_text())
    else:
        compiler = args.compiler.resolve()
        if not compiler.is_file():
            parser.error(f"Compiler missing: {compiler}; run make build first")
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        directory = (args.output or ROOT / "build/wgsl-web" / stamp).resolve()
        manifest = prepare(compiler, directory, args.case or list(CASES), args.unrolled_reference, args.baseline_compiler)
    print(f"Prepared browser benchmark: {directory}", flush=True)
    if args.prepare_only:
        return
    compiler = Path(manifest["compiler"]["path"])
    if sha256(compiler) != manifest["compiler"]["sha256"]:
        parser.error("Compiler changed since preparation; prepare a new benchmark")
    compiler_stat = compiler.stat()
    reference_signature = None
    if manifest.get("reference_compiler"):
        reference = Path(manifest["reference_compiler"]["path"])
        if sha256(reference) != manifest["reference_compiler"]["sha256"]:
            parser.error("Baseline compiler changed since preparation; prepare a new benchmark")
        reference_stat = reference.stat()
        reference_signature = (reference_stat.st_size, reference_stat.st_mtime_ns)
    handler = partial(
        BenchmarkHandler, directory=str(directory), manifest=manifest,
        compiler_signature=(compiler_stat.st_size, compiler_stat.st_mtime_ns),
        reference_signature=reference_signature,
    )
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    print(f"Open http://127.0.0.1:{server.server_port}/ in a WebGPU browser. Press Ctrl-C to stop.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
