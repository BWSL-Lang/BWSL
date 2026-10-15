#!/usr/bin/env python3
"""Build the pinned standalone Tint CLI and in-process timing driver locally."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import subprocess


ROOT = Path(__file__).resolve().parents[2]
REVISION = "af01c4ef653550c1e9bf6c04065e7b4f054ea1e8"
REMOTE = "https://dawn.googlesource.com/dawn"


def run(command: list[str]) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, check=True)


def prepare_checkout(checkout: Path) -> None:
    checkout.mkdir(parents=True, exist_ok=True)
    if not (checkout / ".git").exists():
        run(["git", "init", str(checkout)])
        run(["git", "-C", str(checkout), "remote", "add", "origin", REMOTE])
    revision = subprocess.run(
        ["git", "-C", str(checkout), "rev-parse", "--verify", "HEAD"],
        capture_output=True, text=True, check=False,
    )
    actual = revision.stdout.strip() if revision.returncode == 0 else ""
    if actual != REVISION:
        if actual:
            raise SystemExit(f"Refusing to overwrite checkout {actual}; expected {REVISION}")
        run(["git", "-C", str(checkout), "fetch", "--depth", "1", "origin", REVISION])
        run(["git", "-C", str(checkout), "checkout", "--detach", "FETCH_HEAD"])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT / "build/wgsl-tools")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    checkout = args.root.resolve() / "dawn"
    build = args.root.resolve() / "tint"
    prepare_checkout(checkout)

    definitions = {
        "CMAKE_BUILD_TYPE": "Release",
        "DAWN_SOURCE_DIR": str(checkout),
        "BWSL_TINT_REVISION": REVISION,
        "DAWN_FETCH_DEPENDENCIES": "ON",
        "TINT_BUILD_SPV_READER": "ON",
        "TINT_BUILD_SPV_WRITER": "ON",
        "TINT_BUILD_WGSL_READER": "ON",
        "TINT_BUILD_WGSL_WRITER": "ON",
    }
    for option in (
        "DAWN_BUILD_SAMPLES", "DAWN_BUILD_TESTS", "DAWN_BUILD_PROTOBUF",
        "DAWN_ENABLE_METAL", "DAWN_ENABLE_NULL", "DAWN_ENABLE_VULKAN",
        "DAWN_ENABLE_D3D11", "DAWN_ENABLE_D3D12", "DAWN_ENABLE_DESKTOP_GL",
        "DAWN_ENABLE_OPENGLES", "DAWN_USE_GLFW", "TINT_BUILD_TESTS",
        "TINT_BUILD_MSL_WRITER", "TINT_BUILD_HLSL_WRITER", "TINT_BUILD_GLSL_WRITER",
        "TINT_BUILD_GLSL_VALIDATOR", "TINT_BUILD_IR_BINARY",
    ):
        definitions[option] = "OFF"
    command = ["cmake", "-S", str(ROOT / "tools/wgsl-bench"), "-B", str(build), "-G", "Ninja"]
    command.extend(f"-D{name}={value}" for name, value in definitions.items())
    run(command)
    run(["cmake", "--build", str(build), "--target", "bwsl-tint-bench", "tint_cmd_tint_cmd",
         "--parallel", str(args.jobs)])
    metadata = {
        "repository": REMOTE,
        "revision": REVISION,
        "cmake_definitions": definitions,
        "configure_command": command,
        "cmake_cache": str(build / "CMakeCache.txt"),
        "platform": platform.platform(),
    }
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line.startswith("CMAKE_") and ":" in line and "=" in line:
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    metadata["compiler_configuration"] = {
        key: value for key, value in cache.items()
        if key.startswith("CMAKE_CXX_") or key.startswith("CMAKE_OSX_")
        or key in ("CMAKE_BUILD_TYPE", "CMAKE_GENERATOR")
    }
    compiler = cache.get("CMAKE_CXX_COMPILER")
    if compiler:
        version = subprocess.run([compiler, "--version"], capture_output=True, text=True, check=False)
        metadata["compiler_version"] = version.stdout.strip()
    (build / "build-metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"Tint benchmark: {build / 'bwsl-tint-bench'}")
    print(f"Tint CLI: {build / 'dawn/tint'}")


if __name__ == "__main__":
    main()
