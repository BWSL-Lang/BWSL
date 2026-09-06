#!/usr/bin/env python3
"""Provision a pinned DXC locally from the verified official LunarG SDK core."""
import argparse
import hashlib
from pathlib import Path
import shlex
import subprocess
import sys

SDK_VERSION = "1.4.357.1"
DXC_VERSION = "1.9.0.5399"
URL = f"https://sdk.lunarg.com/sdk/download/{SDK_VERSION}/mac/vulkansdk-macos-{SDK_VERSION}.zip"
SHA256 = "cf23e604e6b8b82c18eaba329f5623ea5a309d949a98d49a61a161d576c769a5"
ROOT = Path(__file__).resolve().parent.parent


def provision(destination: Path) -> Path:
    if sys.platform != "darwin":
        raise RuntimeError("This provisioning script is for macOS only")
    destination = destination.resolve()
    dxc = destination / "macOS/bin/dxc"
    if not dxc.exists():
        cache = destination.parent / f"vulkan-sdk-{SDK_VERSION}-installer"
        cache.mkdir(parents=True, exist_ok=True)
        archive = cache / f"vulkansdk-macos-{SDK_VERSION}.zip"
        if not archive.exists():
            subprocess.run(["curl", "--fail", "--location", "--retry", "2", "--max-time", "300",
                            "--output", str(archive), URL], check=True)
        with archive.open("rb") as stream:
            checksum = hashlib.sha256()
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                checksum.update(block)
            digest = checksum.hexdigest()
        if digest != SHA256:
            raise RuntimeError(f"SDK checksum mismatch: {archive}; remove it and retry")
        subprocess.run(["unzip", "-q", "-o", str(archive), "-d", str(cache)], check=True)
        installer = cache / f"vulkansdk-macOS-{SDK_VERSION}.app/Contents/MacOS/vulkansdk-macOS-{SDK_VERSION}"
        # Only core files are installed locally. In particular, the optional
        # com.lunarg.vulkan.usr system-wide component is never selected.
        with (cache / "install.log").open("w") as log:
            subprocess.run([str(installer), "--root", str(destination), "--accept-licenses",
                            "--default-answer", "--confirm-command", "install", "com.lunarg.vulkan.core"],
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    version = subprocess.run([str(dxc), "--version"], check=True, text=True, capture_output=True)
    if DXC_VERSION not in version.stdout + version.stderr:
        raise RuntimeError(f"Expected DXC {DXC_VERSION}, got {version.stdout}{version.stderr}")
    print((version.stdout + version.stderr).strip())
    print(f"export BWSL_DXC={shlex.quote(str(dxc))}")
    return dxc


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / "build/toolchains" / f"dxc-{DXC_VERSION}")
    args = parser.parse_args()
    try:
        provision(args.output_dir)
    except (RuntimeError, subprocess.SubprocessError, OSError) as error:
        parser.exit(1, f"DXC provisioning failed: {error}\n")
