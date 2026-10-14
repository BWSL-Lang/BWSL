#!/usr/bin/env python3
"""GPU benchmarks: hand-written GLSL ES against BWSL's generated code.

Every test case is a subdirectory <test>/ with
  <test>.json       inputs, render targets and comparison tolerance
  <test>.bwsl       the BWSL pipeline (one pass)
  <test>_hand.vert  hand-written GLSL ES 3.00 equivalent
  <test>_hand.frag

and runs on these backends:
  gl-hand    <test>_hand.vert/frag on OpenGL
  gl-bwsl    bwslc -gles (SPIR-V -> SPIRV-Cross) on OpenGL
  gl-direct  bwslc -gles-direct (IR -> GLSL ES) on OpenGL
  vk-bwsl    bwslc SPIR-V on Vulkan
  vk-hand    <test>_hand.vert/frag compiled with glslangValidator, on Vulkan
             (tells Vulkan-vs-GL cost apart from BWSL's code generation)

Frames are timed on the GPU (GL_TIME_ELAPSED queries, Vulkan timestamps) by
build/bench_runner, at 720p, 1080p and 4K. A test skips its remaining
resolutions once one resolution took longer than --skip-after seconds. The
first resolution also reads back every backend's output and compares it to
gl-hand's.

  python tests/benchmarks/run.py                  # everything
  python tests/benchmarks/run.py hsluv noise      # some tests
  python tests/benchmarks/run.py --backends gl-hand,vk-bwsl --resolutions 4k

See tests/benchmarks/README.md.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path

try:
    import numpy as np
except ImportError:
    sys.exit("tests/benchmarks/run.py needs numpy: python -m pip install numpy")

BENCH_DIR = Path(__file__).resolve().parent
ROOT = BENCH_DIR.parent.parent
EXE = ".exe" if os.name == "nt" else ""

RESOLUTIONS = {
    "720p": (1280, 720),
    "1080p": (1920, 1080),
    "1440p": (2560, 1440),
    "4k": (3840, 2160),
}
DEFAULT_RESOLUTIONS = ["720p", "1080p", "4k"]

BACKENDS = ["gl-hand", "gl-bwsl", "gl-direct", "vk-hand", "vk-bwsl"]
BACKEND_TITLES = {
    "gl-hand": "GL hand",
    "gl-bwsl": "GL bwsl",
    "gl-direct": "GL direct",
    "vk-hand": "VK hand",
    "vk-bwsl": "VK bwsl",
}

ATTRIBUTE_COMPONENTS = {"float": 1, "float2": 2, "float3": 3, "float4": 4}


# ============================================================================
# SPIR-V reflection (just enough to bind BWSL's Vulkan output)
# ============================================================================

SPV_OP_NAME = 5
SPV_OP_TYPE_STRUCT = 30
SPV_OP_TYPE_RUNTIME_ARRAY = 29
SPV_OP_TYPE_POINTER = 32
SPV_OP_VARIABLE = 59
SPV_OP_DECORATE = 71
SPV_DECORATION_ARRAY_STRIDE = 6
SPV_DECORATION_LOCATION = 30
SPV_DECORATION_BINDING = 33
SPV_DECORATION_DESCRIPTOR_SET = 34
SPV_STORAGE_CLASS_STORAGE_BUFFER = 12


@dataclass
class SpirvVariable:
    name: str = ""
    set: int = -1
    binding: int = -1
    storage_class: int = -1
    location: int = -1
    # For storage buffers holding one runtime array: its element stride.
    array_stride: int = 0


def reflect_spirv(path: Path) -> list[SpirvVariable]:
    words = np.fromfile(path, dtype="<u4")
    if len(words) < 5 or words[0] != 0x07230203:
        raise ValueError(f"{path} is not SPIR-V")
    names: dict[int, str] = {}
    decorations: dict[int, dict[int, int]] = {}
    pointers: dict[int, tuple[int, int]] = {}
    structs: dict[int, list[int]] = {}
    runtime_arrays: set[int] = set()
    variables: dict[int, tuple[int, int]] = {}
    i = 5
    while i < len(words):
        opcode = int(words[i]) & 0xFFFF
        count = int(words[i]) >> 16
        if count == 0:
            break
        operands = [int(w) for w in words[i + 1:i + count]]
        if opcode == SPV_OP_NAME:
            raw = struct.pack(f"<{len(operands) - 1}I", *operands[1:])
            names[operands[0]] = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
        elif opcode == SPV_OP_DECORATE and len(operands) >= 3:
            decorations.setdefault(operands[0], {})[operands[1]] = operands[2]
        elif opcode == SPV_OP_TYPE_POINTER:
            pointers[operands[0]] = (operands[1], operands[2])
        elif opcode == SPV_OP_TYPE_STRUCT:
            structs[operands[0]] = operands[1:]
        elif opcode == SPV_OP_TYPE_RUNTIME_ARRAY:
            runtime_arrays.add(operands[0])
        elif opcode == SPV_OP_VARIABLE:
            variables[operands[1]] = (operands[0], operands[2])
        i += count

    result = []
    for var_id, (type_id, storage_class) in variables.items():
        deco = decorations.get(var_id, {})
        v = SpirvVariable(
            name=names.get(var_id, ""),
            set=deco.get(SPV_DECORATION_DESCRIPTOR_SET, -1),
            binding=deco.get(SPV_DECORATION_BINDING, -1),
            storage_class=storage_class,
            location=deco.get(SPV_DECORATION_LOCATION, -1),
        )
        pointee = pointers.get(type_id, (0, 0))[1]
        members = structs.get(pointee, [])
        if len(members) == 1 and members[0] in runtime_arrays:
            v.array_stride = decorations.get(members[0], {}).get(SPV_DECORATION_ARRAY_STRIDE, 0)
        result.append(v)
    return result


# ============================================================================
# Inputs
# ============================================================================

def eval_expr(value, width: int, height: int) -> float:
    if isinstance(value, (int, float)):
        return float(value)
    env = {"width": width, "height": height, "aspect": width / height,
           "pi": math.pi, "sqrt": math.sqrt}
    return float(eval(str(value), {"__builtins__": {}}, env))


def texture_size(spec: dict, width: int, height: int) -> tuple[int, int]:
    size = spec.get("size", "target")
    if size == "target":
        return width, height
    if size == "half":
        return max(1, width // 2), max(1, height // 2)
    if size == "quarter":
        return max(1, width // 4), max(1, height // 4)
    return int(size[0]), int(size[1])


def generate_texture(spec: dict, w: int, h: int) -> np.ndarray:
    """RGBA float texels in [lo, hi], shape (h, w, 4), row 0 at v = 0."""
    rng = np.random.default_rng(spec.get("seed", 1))
    lo, hi = spec.get("range", [0.0, 1.0])
    kind = spec.get("gen", "smooth")
    if kind == "random":
        t = rng.random((h, w, 4))
    elif kind == "smooth":
        # A sum of random plane waves: smooth, tiles at any size, cheap.
        x = (np.arange(w) + 0.5) / w
        y = (np.arange(h) + 0.5) / h
        xx, yy = np.meshgrid(x, y)
        t = np.zeros((h, w, 4))
        waves = spec.get("waves", 6)
        detail = spec.get("detail", 8)
        for c in range(4):
            for _ in range(waves):
                fx, fy = rng.integers(-detail, detail + 1, size=2)
                phase = rng.random() * 2 * math.pi
                amp = rng.random()
                t[:, :, c] += amp * np.sin(2 * math.pi * (fx * xx + fy * yy) + phase)
            t[:, :, c] -= t[:, :, c].min()
            t[:, :, c] /= max(t[:, :, c].max(), 1e-6)
        if spec.get("noise", 0.0):
            t = (1 - spec["noise"]) * t + spec["noise"] * rng.random((h, w, 4))
    elif kind == "gradient":
        x = (np.arange(w) + 0.5) / w
        y = (np.arange(h) + 0.5) / h
        xx, yy = np.meshgrid(x, y)
        t = np.stack([xx, yy, 1 - xx, (xx + yy) * 0.5], axis=-1)
    else:
        raise ValueError(f"unknown texture generator {kind}")
    t = lo + (hi - lo) * t
    if "alpha" in spec:
        t[:, :, 3] = spec["alpha"]
    return t


def encode_texture(texels: np.ndarray, fmt: str) -> bytes:
    if fmt == "rgba8":
        return np.clip(np.rint(texels * 255.0), 0, 255).astype(np.uint8).tobytes()
    if fmt == "rgba16f":
        return texels.astype("<f2").tobytes()
    raise ValueError(f"unknown texture format {fmt}")


def mip_chain(texels: np.ndarray) -> list[np.ndarray]:
    """Level 0 and every smaller level down to 1x1, each a 2x2 box filter of
    the previous one (sizes halve, rounded down; odd edges are dropped)."""
    levels = [texels]
    while levels[-1].shape[0] > 1 or levels[-1].shape[1] > 1:
        t = levels[-1]
        h, w = t.shape[:2]
        if h > 1:
            t = (t[0:h // 2 * 2:2] + t[1:h // 2 * 2:2]) * 0.5
        if w > 1:
            t = (t[:, 0:w // 2 * 2:2] + t[:, 1:w // 2 * 2:2]) * 0.5
        levels.append(t)
    return levels


def generate_mesh(spec: dict) -> tuple[dict[str, np.ndarray], np.ndarray | None]:
    """Returns per-vertex attribute arrays and optional uint32 indices."""
    kind = spec.get("type", "fullscreen")
    if kind == "fullscreen":
        pos = np.array([[-1.0, -1.0], [3.0, -1.0], [-1.0, 3.0]])
        return {"position": pos, "uv": (pos + 1.0) * 0.5}, None
    if kind == "grid":
        cols, rows = int(spec["cols"]), int(spec["rows"])
        u = np.linspace(0.0, 1.0, cols)
        v = np.linspace(0.0, 1.0, rows)
        uu, vv = np.meshgrid(u, v)
        uv = np.stack([uu.ravel(), vv.ravel()], axis=-1)
        extent = spec.get("extent", 1.0)
        pos = np.stack([(uv[:, 0] * 2 - 1) * extent, (uv[:, 1] * 2 - 1) * extent,
                        np.zeros(len(uv))], axis=-1)
        rng = np.random.default_rng(spec.get("seed", 1))
        quads = np.arange((rows - 1) * cols).reshape(rows - 1, cols)[:, :-1].ravel()
        a, b, c, d = quads, quads + 1, quads + cols, quads + cols + 1
        indices = np.stack([a, b, c, b, d, c], axis=-1).ravel().astype(np.uint32)
        return {"position": pos, "uv": uv, "seed": rng.random((len(uv), 4))}, indices
    raise ValueError(f"unknown mesh type {kind}")


@dataclass
class Inputs:
    vbo: Path
    stride: int
    attributes: list[dict]          # name, type, offset
    ibo: Path | None
    pulled: dict[str, np.ndarray]   # attribute name -> (n, comps) float32
    ubos: list[tuple[str, Path]]
    textures: list[dict]            # spec + path, w, h


def build_inputs(bench: dict, width: int, height: int, out: Path) -> Inputs:
    out.mkdir(parents=True, exist_ok=True)
    arrays, indices = generate_mesh(bench.get("mesh", {}))
    attributes = []
    columns = []
    pulled = {}
    offset = 0
    for attr in bench["attributes"]:
        comps = ATTRIBUTE_COMPONENTS[attr["type"]]
        source = arrays[attr.get("source", attr["name"])]
        data = np.zeros((len(source), comps), dtype=np.float32)
        n = min(comps, source.shape[1])
        data[:, :n] = source[:, :n]
        if comps == 4 and source.shape[1] < 4:
            data[:, 3] = 1.0
        columns.append(data)
        pulled[attr["name"]] = data
        attributes.append({"name": attr["name"], "type": attr["type"], "offset": offset})
        offset += comps * 4
    vbo = out / "vertices.bin"
    np.concatenate(columns, axis=1).astype("<f4").tofile(vbo)
    ibo = None
    if indices is not None:
        ibo = out / "indices.bin"
        indices.astype("<u4").tofile(ibo)

    ubos = []
    for spec in bench.get("uniforms", []):
        values = [eval_expr(v, width, height) for v in spec.get("values", [])]
        if "random" in spec:
            r = spec["random"]
            rng = np.random.default_rng(r.get("seed", 1))
            lo, hi = r.get("range", [0.0, 1.0])
            values += list(lo + (hi - lo) * rng.random(int(r["count"])))
        while len(values) % 4:
            values.append(0.0)
        path = out / f"ubo_{spec['name']}.bin"
        np.asarray(values, dtype="<f4").tofile(path)
        ubos.append((spec["name"], path))

    textures = []
    for spec in bench.get("textures", []):
        w, h = texture_size(spec, width, height)
        fmt = spec.get("format", "rgba8")
        # Cached per spec and size; the generators are deterministic.
        key = zlib.crc32(json.dumps(spec, sort_keys=True).encode())
        path = out / f"tex_{spec['name']}_{w}x{h}_{key:08x}.bin"
        if not path.exists():
            texels = generate_texture(spec, w, h)
            levels = mip_chain(texels) if spec.get("mipmaps") else [texels]
            path.write_bytes(b"".join(encode_texture(t, fmt) for t in levels))
        textures.append({**spec, "path": path, "w": w, "h": h, "format": fmt})
    return Inputs(vbo, offset, attributes, ibo, pulled, ubos, textures)


# ============================================================================
# Compilation
# ============================================================================

@dataclass
class Program:
    """One backend's shaders plus how to bind the test's resources to them."""
    vert: Path
    frag: Path
    api: str
    gl_names: dict[str, str] = field(default_factory=dict)
    # Vulkan: resource name -> (set, binding); attribute name -> (set,
    # binding, array stride) of its vertex-pulling buffer.
    vk_bindings: dict[str, tuple[int, int]] = field(default_factory=dict)
    vk_pulled: dict[str, tuple[int, int, int]] = field(default_factory=dict)
    # Vulkan with real vertex inputs (vk-hand): attribute name -> location.
    vk_locations: dict[str, int] = field(default_factory=dict)
    # Set when the generated code is byte-identical to another backend's.
    same_as: str = ""


class CompileError(Exception):
    pass


def run_bwslc(bwslc: Path, source: Path, out: Path, flags: list[str]) -> None:
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    cmd = [str(bwslc), str(source), "-o", str(out), *flags]
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace")
    if proc.returncode != 0:
        raise CompileError((proc.stdout + proc.stderr).strip())


def single_stage_file(out: Path, suffix: str) -> Path:
    files = sorted(out.glob(f"*{suffix}"))
    if len(files) != 1:
        raise CompileError(f"expected one *{suffix} in {out}, found {len(files)}")
    return files[0]


def default_gl_names(bench: dict) -> dict[str, str]:
    """BWSL's GL naming, which the hand-written shaders follow too."""
    names = {a["name"]: f"a_{a['name']}" for a in bench["attributes"]}
    names.update({u["name"]: f"ub_{u['name']}" for u in bench.get("uniforms", [])})
    names.update({t["name"]: f"t_{t['name']}" for t in bench.get("textures", [])})
    return names


def test_file(test_dir: Path, suffix: str) -> Path:
    """A test's files are named after its directory: <test><suffix>."""
    return test_dir / f"{test_dir.name}{suffix}"


def find_glslang() -> Path | None:
    found = shutil.which("glslangValidator")
    if found:
        return Path(found)
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        for sub in ("Bin", "bin"):
            p = Path(sdk) / sub / f"glslangValidator{EXE}"
            if p.exists():
                return p
    return None


def compile_hand_for_vulkan(test_dir: Path, bench: dict, out: Path) -> Program:
    """<test>_hand.vert/frag -> SPIR-V. GLSL ES 3.00 isn't a Vulkan dialect, so
    the sources become 3.10 ES with explicit set 0 bindings in <test>.json order
    (uniforms, then textures); in/out locations follow declaration order."""
    glslang = find_glslang()
    if glslang is None:
        raise CompileError("glslangValidator not found (install the Vulkan SDK)")
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    bindings = {}
    for name in ([u["name"] for u in bench.get("uniforms", [])] +
                 [t["name"] for t in bench.get("textures", [])]):
        bindings[name] = len(bindings)
    stem = f"{test_dir.name}_hand"
    prog = Program(out / f"{stem}.vert.spv", out / f"{stem}.frag.spv", "vulkan")
    for stage in ("vert", "frag"):
        src = test_file(test_dir, f"_hand.{stage}").read_text()
        src = re.sub(r"#version\s+300\s+es", "#version 310 es", src, count=1)
        for name, binding in bindings.items():
            src = re.sub(rf"layout\s*\(\s*std140\s*\)\s*uniform\s+ub_{name}\b",
                         f"layout(std140, set = 0, binding = {binding}) uniform ub_{name}", src)
            src = re.sub(rf"(?m)^(\s*)uniform(\s+\w+)?\s+sampler2D\s+t_{name}\s*;",
                         rf"\1layout(set = 0, binding = {binding}) uniform\2 sampler2D t_{name};",
                         src)
        path = out / f"{stem}.{stage}"
        path.write_text(src)
        proc = subprocess.run([str(glslang), "-V", "--auto-map-locations", str(path), "-o",
                               str(out / f"{stem}.{stage}.spv")],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            raise CompileError((proc.stdout + proc.stderr).strip())
    for name, binding in bindings.items():
        prog.vk_bindings[name] = (0, binding)
    for v in reflect_spirv(prog.vert):
        if v.name.startswith("a_") and v.location >= 0:
            prog.vk_locations[v.name[2:]] = v.location
    return prog


def compile_programs(bwslc: Path, test_dir: Path, bench: dict, work: Path,
                     backends: list[str]) -> dict[str, Program | CompileError]:
    source = test_file(test_dir, ".bwsl")
    programs: dict[str, Program | CompileError] = {}
    gl_names = default_gl_names(bench)

    if "gl-hand" in backends:
        programs["gl-hand"] = Program(test_file(test_dir, "_hand.vert"),
                                      test_file(test_dir, "_hand.frag"),
                                      "gl", gl_names)

    if "gl-bwsl" in backends:
        try:
            out = work / "gles"
            run_bwslc(bwslc, source, out, ["-gles", "-bindings"])
            names = dict(gl_names)
            bindings = json.loads(single_stage_file(out, ".bindings.json").read_text())
            for r in bindings.get("resources", []):
                gl_name = r.get("glName")
                if not gl_name and r.get("combinedSamplerUniforms"):
                    gl_name = r["combinedSamplerUniforms"][0]["name"]
                if gl_name:
                    names[r["name"]] = gl_name
            programs["gl-bwsl"] = Program(single_stage_file(out, ".vert"),
                                          single_stage_file(out, ".frag"), "gl", names)
        except CompileError as e:
            programs["gl-bwsl"] = e

    if "gl-direct" in backends:
        try:
            out = work / "gles-direct"
            run_bwslc(bwslc, source, out, ["-gles-direct"])
            prog = Program(single_stage_file(out, ".vert"), single_stage_file(out, ".frag"),
                           "gl", gl_names)
            # -gles-direct falls back to SPIRV-Cross for some shaders.
            other = programs.get("gl-bwsl")
            if isinstance(other, Program):
                stages = [s for s in ("vert", "frag")
                          if getattr(prog, s).read_bytes() == getattr(other, s).read_bytes()]
                if stages:
                    prog.same_as = "gl-bwsl " + "+".join(stages)
            programs["gl-direct"] = prog
        except CompileError as e:
            programs["gl-direct"] = e

    if "vk-hand" in backends:
        try:
            programs["vk-hand"] = compile_hand_for_vulkan(test_dir, bench, work / "vk-hand")
        except CompileError as e:
            programs["vk-hand"] = e

    if "vk-bwsl" in backends:
        try:
            out = work / "spirv"
            run_bwslc(bwslc, source, out, ["-spv"])
            prog = Program(single_stage_file(out, ".vert.spv"),
                           single_stage_file(out, ".frag.spv"), "vulkan")
            resource_names = ([u["name"] for u in bench.get("uniforms", [])] +
                              [t["name"] for t in bench.get("textures", [])])
            pulled_buffers = []
            for stage in (prog.vert, prog.frag):
                for v in reflect_spirv(stage):
                    if v.binding < 0:
                        continue
                    for name in resource_names:
                        if v.name in (f"bwsl_ub_{name}", f"t_{name}"):
                            prog.vk_bindings[name] = (v.set, v.binding)
                    if (stage == prog.vert and v.array_stride and not v.name and
                            v.storage_class == SPV_STORAGE_CLASS_STORAGE_BUFFER):
                        pulled_buffers.append(v)
            missing = [n for n in resource_names if n not in prog.vk_bindings]
            if missing:
                raise CompileError(f"no SPIR-V binding found for {', '.join(missing)}")
            # Vertex pulling: one storage buffer per attribute of the pass,
            # bindings in attribute declaration order. Attributes the shader
            # never reads keep their binding but have no buffer.
            for v in pulled_buffers:
                if v.binding >= len(bench["attributes"]):
                    raise CompileError(f"unexpected vertex-pulling binding {v.binding}")
                attr = bench["attributes"][v.binding]["name"]
                prog.vk_pulled[attr] = (v.set, v.binding, v.array_stride)
            programs["vk-bwsl"] = prog
        except (CompileError, ValueError) as e:
            programs["vk-bwsl"] = e if isinstance(e, CompileError) else CompileError(str(e))
    return programs


# ============================================================================
# Running
# ============================================================================

@dataclass
class RunResult:
    samples: list[float] = field(default_factory=list)
    device: str = ""
    driver: str = ""
    error: str = ""
    readback: Path | None = None

    @property
    def median(self) -> float:
        return float(np.median(self.samples)) if self.samples else math.nan

    @property
    def minimum(self) -> float:
        return float(np.min(self.samples)) if self.samples else math.nan


def runner_args(prog: Program, bench: dict, inputs: Inputs, width: int, height: int,
                work: Path, args) -> list[str]:
    cmd = ["--api", prog.api, "--vert", str(prog.vert), "--frag", str(prog.frag),
           "--width", str(width), "--height", str(height),
           "--targets", str(bench.get("targets", 1)),
           "--format", bench.get("format", "rgba8"),
           "--vbo", str(inputs.vbo), str(inputs.stride),
           "--frames", str(args.frames), "--warmup", str(args.warmup),
           "--budget", str(args.budget)]
    if bench.get("depth"):
        cmd.append("--depth")
    if inputs.ibo:
        cmd += ["--ibo", str(inputs.ibo)]
    for attr in inputs.attributes:
        gl_name = prog.gl_names.get(attr["name"], "-") if prog.api == "gl" else "-"
        location = prog.vk_locations.get(attr["name"], -1)
        cmd += ["--attr", gl_name, attr["type"], str(attr["offset"]), str(location)]
    if prog.api == "vulkan":
        for name, (set_, binding, stride) in prog.vk_pulled.items():
            data = inputs.pulled[name]
            comps = stride // 4
            padded = np.zeros((len(data), comps), dtype="<f4")
            padded[:, :data.shape[1]] = data
            path = work / f"pulled_{name}.bin"
            padded.tofile(path)
            cmd += ["--ssbo", str(path), str(set_), str(binding)]
    for name, path in inputs.ubos:
        if prog.api == "gl":
            cmd += ["--ubo", str(path), prog.gl_names.get(name, "-"), "-1", "-1"]
        else:
            set_, binding = prog.vk_bindings.get(name, (-1, -1))
            cmd += ["--ubo", str(path), "-", str(set_), str(binding)]
    for tex in inputs.textures:
        gl_name, set_, binding = "-", -1, -1
        if prog.api == "gl":
            gl_name = prog.gl_names.get(tex["name"], "-")
        else:
            set_, binding = prog.vk_bindings.get(tex["name"], (-1, -1))
        cmd += ["--tex", str(tex["path"]), str(tex["w"]), str(tex["h"]), tex["format"],
                "trilinear" if tex.get("mipmaps") else tex.get("filter", "linear"),
                tex.get("wrap", "clamp"), gl_name,
                str(set_), str(binding)]
    return cmd


def run_backend(runner: Path, cmd: list[str], readback: Path | None) -> RunResult:
    if readback:
        cmd = cmd + ["--readback", str(readback)]
    proc = subprocess.run([str(runner), *cmd], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        msg = (proc.stderr or proc.stdout).strip()
        return RunResult(error=msg or f"bench_runner exited with {proc.returncode}")
    try:
        data = json.loads(proc.stdout.strip().splitlines()[-1])
    except (json.JSONDecodeError, IndexError):
        return RunResult(error=f"unreadable bench_runner output: {proc.stdout[:200]}")
    return RunResult(samples=data["samples_ms"], device=data["device"],
                     driver=data["driver"], readback=readback)


# ============================================================================
# Verification
# ============================================================================

@dataclass
class Comparison:
    max_diff: float
    mismatch_fraction: float
    ok: bool
    images: list[Path] = field(default_factory=list)


def load_readback(path: Path, width: int, height: int, targets: int) -> np.ndarray:
    data = np.fromfile(path, dtype="<f4")
    return data.reshape(targets, height, width, 4)


def write_png(path: Path, rgba: np.ndarray) -> None:
    """rgba: (h, w, 4) floats in [0, 1], row 0 at the bottom (GL order)."""
    img = np.clip(np.rint(rgba[::-1] * 255.0), 0, 255).astype(np.uint8)
    h, w, _ = img.shape
    raw = b"".join(b"\0" + img[y].tobytes() for y in range(h))

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    path.write_bytes(png)


def compare(ref: np.ndarray, out: np.ndarray, bench: dict, image_dir: Path,
            backend: str, save_images: bool) -> Comparison:
    tolerance = bench.get("tolerance", 2.0 / 255.0)
    allowed = bench.get("max_mismatch", 0.0)
    both_nan = np.isnan(ref) & np.isnan(out)
    diff = np.where(both_nan, 0.0, np.abs(ref.astype(np.float64) - out))
    diff = np.nan_to_num(diff, nan=np.inf)
    max_diff = float(diff.max())
    fraction = float(np.count_nonzero(diff > tolerance)) / diff.size
    result = Comparison(max_diff, fraction, fraction <= allowed)
    if save_images or not result.ok:
        image_dir.mkdir(parents=True, exist_ok=True)
        for t in range(out.shape[0]):
            opaque = lambda img: np.concatenate([img[..., :3], np.ones_like(img[..., :1])], -1)
            p = image_dir / f"{backend}_target{t}.png"
            write_png(p, opaque(out[t]))
            result.images.append(p)
            if not result.ok:
                d = image_dir / f"{backend}_target{t}_diff.png"
                heat = np.clip(diff[t].max(axis=-1, keepdims=True) / max(tolerance, 1e-6) * 0.25,
                               0, 1)
                write_png(d, np.concatenate([heat, heat * 0, heat * 0, np.ones_like(heat)], -1))
                result.images.append(d)
    return result


# ============================================================================
# Report
# ============================================================================

def fmt_ms(value: float) -> str:
    return "-" if math.isnan(value) else f"{value:.3f}"


def ratio_bases(backend: str) -> list[str]:
    """Backends a timing is compared against, first one that ran wins."""
    if backend == "gl-hand":
        return []
    if backend == "vk-hand" or backend.startswith("gl-"):
        return ["gl-hand"]
    return ["vk-hand", "gl-hand"]


def print_table(header: list[str], rows: list[list[str]], right: list[bool]) -> None:
    """Markdown table padded so its columns also line up in a terminal."""
    widths = [max(len(row[i]) for row in [header, *rows]) for i in range(len(header))]
    widths = [max(w, 3) for w in widths]

    def line(cells: list[str]) -> str:
        padded = [c.rjust(w) if r else c.ljust(w) for c, w, r in zip(cells, widths, right)]
        return "| " + " | ".join(padded) + " |"

    print(line(header))
    print("| " + " | ".join("-" * (w - 1) + ":" if r else "-" * w
                            for w, r in zip(widths, right)) + " |")
    for row in rows:
        print(line(row))


def print_report(results: dict, comparisons: dict, backends: list[str], meta: dict) -> None:
    print()
    print("## BWSL GPU benchmarks")
    print()
    for key in ("gl", "vulkan"):
        if key in meta["devices"]:
            device, driver = meta["devices"][key]
            print(f"- {'OpenGL' if key == 'gl' else 'Vulkan'}: {device} ({driver})")
    print(f"- bwslc: {meta['bwslc']}")
    print(f"- GPU ms per frame, median of up to {meta['frames']} timed frames "
          f"(x{meta['rounds']} rounds)")
    print("- Ratios compare against the hand-written shader on the same API (VK hand "
          "against GL hand, i.e. the API's own cost)")
    gl_dev = meta["devices"].get("gl", ("", ""))[0]
    vk_dev = meta["devices"].get("vulkan", ("", ""))[0]
    if gl_dev and vk_dev and vk_dev.split("/")[0] not in gl_dev:
        print(f"- WARNING: GL and Vulkan ran on different GPUs")
    print()

    header = ["Test", "Resolution"] + [BACKEND_TITLES[b] for b in backends]
    rows = []
    for test, by_res in results.items():
        for res, by_backend in by_res.items():
            cells = []
            for b in backends:
                r = by_backend.get(b)
                base_ms = math.nan
                for base in ratio_bases(b):
                    other = by_backend.get(base)
                    if other and not other.error:
                        base_ms = other.median
                        break
                if r is None:
                    cells.append("-")
                elif r.error:
                    cells.append("error")
                elif math.isnan(base_ms):
                    cells.append(fmt_ms(r.median))
                else:
                    cells.append(f"{fmt_ms(r.median)} ({r.median / base_ms:.2f}x)")
            rows.append([test, res, *cells])
        if not by_res:
            rows.append([test, "skipped", *("-" for _ in backends)])
    print_table(header, rows, [False, False] + [True] * len(backends))
    for (backend, other), tests in meta["same_as"].items():
        other_backend, stages = other.split(" ")
        print()
        print(f"{BACKEND_TITLES[backend]} generated the same {stages} code as "
              f"{BACKEND_TITLES[other_backend]} (fell back to SPIRV-Cross) for: "
              f"{', '.join(tests)}")

    if comparisons:
        print()
        print(f"Output vs {BACKEND_TITLES[meta['baseline']]} "
              f"(max abs difference; share of channel values over the tolerance):")
        print()
        others = [b for b in backends if b != meta["baseline"]]
        rows = []
        for test, by_backend in comparisons.items():
            cells = []
            for b in others:
                c = by_backend.get(b)
                if c is None:
                    cells.append("-")
                elif c.ok:
                    cells.append(f"ok ({c.max_diff:.2g})")
                else:
                    cells.append(f"**MISMATCH** {c.max_diff:.3g}, {c.mismatch_fraction:.2%}")
            rows.append([test, *cells])
        print_table(["Test", *(BACKEND_TITLES[b] for b in others)], rows,
                    [False] * (len(others) + 1))

    errors = [(t, res, b, r.error) for t, by_res in results.items()
              for res, by_b in by_res.items() for b, r in by_b.items() if r.error]
    errors += meta["compile_errors"]
    if errors:
        print()
        print("Errors:")
        seen = set()
        for test, res, backend, msg in errors:
            key = (test, backend, msg)
            if key in seen:
                continue
            seen.add(key)
            first = msg.strip().splitlines()[:12]
            print(f"- {test} / {backend}" + (f" / {res}" if res else "") + ":")
            for line in first:
                print(f"    {line}")


# ============================================================================
# Main
# ============================================================================

def bwslc_description(bwslc: Path) -> str:
    try:
        commit = subprocess.run(["git", "-C", str(ROOT), "describe", "--always", "--dirty"],
                                capture_output=True, text=True).stdout.strip()
    except OSError:
        commit = ""
    built = time.strftime("%Y-%m-%d %H:%M", time.localtime(bwslc.stat().st_mtime))
    return f"{bwslc.relative_to(ROOT) if bwslc.is_relative_to(ROOT) else bwslc}, " \
           f"built {built}" + (f", source {commit}" if commit else "")


def ensure_runner(runner: Path, build: bool) -> None:
    source = ROOT / "src" / "bench_runner.cpp"
    fresh = runner.exists() and runner.stat().st_mtime >= source.stat().st_mtime
    if fresh or not build:
        if not runner.exists():
            sys.exit(f"{runner} not found; build it with `build.bat bench_runner` "
                     f"(Windows) or `make bench_runner`")
        return
    print(f"Building {runner.name}...", flush=True)
    if os.name == "nt":
        cmd = ["cmd", "/c", str(ROOT / "build.bat"), "bench_runner"]
    else:
        cmd = ["make", "-C", str(ROOT), "bench_runner"]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    if proc.returncode != 0 or not runner.exists():
        sys.exit(f"building bench_runner failed:\n{proc.stdout}{proc.stderr}")


def discover_tests() -> list[Path]:
    return sorted(p for p in BENCH_DIR.iterdir() if test_file(p, ".json").is_file())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tests", nargs="*", help="test names (default: all)")
    ap.add_argument("--list", action="store_true", help="list tests and exit")
    ap.add_argument("--backends", default=",".join(BACKENDS),
                    help=f"comma-separated subset of {','.join(BACKENDS)}")
    ap.add_argument("--resolutions", default=",".join(DEFAULT_RESOLUTIONS),
                    help=f"comma-separated subset of {','.join(RESOLUTIONS)} or WxH")
    ap.add_argument("--frames", type=int, default=100, help="timed frames per run")
    ap.add_argument("--warmup", type=int, default=10, help="untimed frames per run")
    ap.add_argument("--budget", type=float, default=1.5,
                    help="seconds of timed frames per run before stopping early")
    ap.add_argument("--rounds", type=int, default=1,
                    help="repeat every backend this many times, interleaved")
    ap.add_argument("--skip-after", type=float, default=10.0,
                    help="skip a test's larger resolutions once one took longer (s)")
    ap.add_argument("--no-verify", action="store_true", help="don't compare outputs")
    ap.add_argument("--save-images", action="store_true",
                    help="write every backend's output as PNG, not just mismatches")
    ap.add_argument("--bwslc", type=Path, default=ROOT / "build" / f"bwslc{EXE}")
    ap.add_argument("--runner", type=Path, default=ROOT / "build" / f"bench_runner{EXE}")
    ap.add_argument("--no-build", action="store_true",
                    help="don't rebuild bench_runner when its source is newer")
    ap.add_argument("--work", type=Path, default=ROOT / "build" / "benchmarks",
                    help="directory for generated shaders, inputs and images")
    ap.add_argument("--json", type=Path, help="also write all results as JSON")
    args = ap.parse_args()

    tests = discover_tests()
    if args.list:
        for t in tests:
            desc = json.loads(test_file(t, ".json").read_text()).get("description", "")
            print(f"{t.name:20} {desc}")
        return 0
    if args.tests:
        unknown = set(args.tests) - {t.name for t in tests}
        if unknown:
            sys.exit(f"unknown tests: {', '.join(sorted(unknown))}")
        tests = [t for t in tests if t.name in args.tests]

    backends = [b.strip() for b in args.backends.split(",") if b.strip()]
    for b in backends:
        if b not in BACKENDS:
            sys.exit(f"unknown backend {b}")
    resolutions = []
    for r in args.resolutions.split(","):
        r = r.strip().lower()
        if r in RESOLUTIONS:
            resolutions.append((r, RESOLUTIONS[r]))
        elif "x" in r:
            w, h = r.split("x")
            resolutions.append((r, (int(w), int(h))))
        else:
            sys.exit(f"unknown resolution {r}")
    if not args.bwslc.exists():
        sys.exit(f"{args.bwslc} not found; build bwslc first")
    ensure_runner(args.runner, not args.no_build)

    baseline = backends[0] if "gl-hand" not in backends else "gl-hand"
    meta = {"devices": {}, "bwslc": bwslc_description(args.bwslc), "frames": args.frames,
            "rounds": args.rounds, "baseline": baseline, "compile_errors": [],
            "same_as": {}}
    results: dict[str, dict[str, dict[str, RunResult]]] = {}
    comparisons: dict[str, dict[str, Comparison]] = {}

    for test_dir in tests:
        name = test_dir.name
        bench = json.loads(test_file(test_dir, ".json").read_text())
        work = args.work / name
        work.mkdir(parents=True, exist_ok=True)
        print(f"[{name}] compiling", flush=True)
        programs = compile_programs(args.bwslc, test_dir, bench, work, backends)
        for b, p in programs.items():
            if isinstance(p, CompileError):
                meta["compile_errors"].append((name, "", b, f"compile failed:\n{p}"))
            elif p.same_as:
                meta["same_as"].setdefault((b, p.same_as), []).append(name)
        runnable = {b: p for b, p in programs.items() if isinstance(p, Program)}
        results[name] = {}

        for index, (res_name, (width, height)) in enumerate(resolutions):
            label = f"{width}x{height}"
            inputs = build_inputs(bench, width, height, work / "inputs" / label)
            verify = index == 0 and not args.no_verify
            started = time.monotonic()
            by_backend: dict[str, RunResult] = {}
            for round_ in range(args.rounds):
                for backend in backends:
                    prog = runnable.get(backend)
                    if prog is None:
                        continue
                    readback = (work / f"readback_{backend}.bin"
                                if verify and round_ == 0 else None)
                    cmd = runner_args(prog, bench, inputs, width, height, work, args)
                    r = run_backend(args.runner, cmd, readback)
                    prev = by_backend.get(backend)
                    if prev is None:
                        by_backend[backend] = r
                    elif not prev.error:
                        if r.error:
                            prev.error = r.error
                        else:
                            prev.samples += r.samples
                    if not r.error:
                        meta["devices"].setdefault(prog.api, (r.device, r.driver))
            elapsed = time.monotonic() - started
            results[name][label] = by_backend
            summary = ", ".join(f"{b} {fmt_ms(r.median) if not r.error else 'error'}"
                                for b, r in by_backend.items())
            print(f"[{name}] {label}: {summary} ({elapsed:.1f}s)", flush=True)

            if verify:
                comparisons[name] = {}
                ref = by_backend.get(baseline)
                if ref and not ref.error and ref.readback:
                    targets = bench.get("targets", 1)
                    ref_img = load_readback(ref.readback, width, height, targets)
                    image_dir = work / "images"
                    if args.save_images:
                        compare(ref_img, ref_img, bench, image_dir, baseline, True)
                    for backend, r in by_backend.items():
                        if backend == baseline or r.error or not r.readback:
                            continue
                        img = load_readback(r.readback, width, height, targets)
                        c = compare(ref_img, img, bench, image_dir, backend, args.save_images)
                        comparisons[name][backend] = c
                        if not c.ok:
                            print(f"[{name}] {backend} output differs from {baseline}: "
                                  f"max {c.max_diff:.4g}, {c.mismatch_fraction:.3%} over "
                                  f"tolerance; images in {image_dir}", flush=True)

            if elapsed > args.skip_after and index + 1 < len(resolutions):
                skipped = ", ".join(r for r, _ in resolutions[index + 1:])
                print(f"[{name}] took {elapsed:.1f}s > {args.skip_after:g}s, "
                      f"skipping {skipped}", flush=True)
                break

    print_report(results, comparisons, backends, meta)

    if args.json:
        doc = {
            "meta": {k: v for k, v in meta.items() if k != "compile_errors"},
            "compile_errors": [{"test": t, "backend": b, "error": e}
                               for t, _, b, e in meta["compile_errors"]],
            "results": {t: {res: {b: {"median_ms": r.median, "min_ms": r.minimum,
                                      "frames": len(r.samples), "error": r.error,
                                      "samples_ms": r.samples}
                                  for b, r in by_b.items()}
                            for res, by_b in by_res.items()}
                        for t, by_res in results.items()},
            "verification": {t: {b: {"max_diff": c.max_diff,
                                     "mismatch_fraction": c.mismatch_fraction,
                                     "ok": c.ok} for b, c in by_b.items()}
                             for t, by_b in comparisons.items()},
        }
        args.json.write_text(json.dumps(doc, indent=2))

    failed = any(not c.ok for by_b in comparisons.values() for c in by_b.values())
    failed = failed or bool(meta["compile_errors"]) or any(
        r.error for by_res in results.values() for by_b in by_res.values()
        for r in by_b.values())
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
