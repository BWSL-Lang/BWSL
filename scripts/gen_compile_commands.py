#!/usr/bin/env python3
"""Generate compile_commands.json for the BWSL project.

Run from the repository root:  python3 scripts/gen_compile_commands.py
"""

import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

INCLUDE_DIRS = [
    ".",
    "core",
    "core/middleware",
    "phases/lexing",
    "phases/parser",
    "phases/evaluation",
    "phases/ir_generation",
    "phases/ir_lowering",
    "phases/control_flow",
    "phases/ssa",
    "phases/backends/spirv",
    "phases/backends/gles",
    "vendor/SPIRV-Cross",
]

BASE_FLAGS = [
    "-std=c++20",
    "-Wall",
    "-Wextra",
    "-DUSE_SPIRV_CROSS_LIB",
]

COMPILER = "clang++"

TRANSLATION_UNITS = [
    "tools/bwslc.cpp",
    "tools/spirv_cross_wrapper.cpp",
    "tools/bwslc_fuzz.cpp",
    "tools/equiv_runner.cpp",
]


def find_inl_files():
    results = []
    for search_dir in ("phases",):
        for dirpath, _, filenames in os.walk(os.path.join(ROOT, search_dir)):
            for name in filenames:
                if name.endswith(".inl"):
                    rel = os.path.relpath(os.path.join(dirpath, name), ROOT)
                    results.append(rel.replace("\\", "/"))
    return sorted(results)


def make_entry(rel_path, extra_args=None):
    include_args = [f"-I{d}" for d in INCLUDE_DIRS]
    args = [COMPILER] + BASE_FLAGS + include_args + (extra_args or []) + ["-c", rel_path]
    return {
        "directory": ROOT,
        "file": os.path.join(ROOT, rel_path),
        "arguments": args,
    }


def main():
    entries = []

    for src in sorted(TRANSLATION_UNITS):
        if os.path.exists(os.path.join(ROOT, src)):
            entries.append(make_entry(src))

    for inl in find_inl_files():
        entries.append(make_entry(inl, extra_args=["-x", "c++"]))

    out_path = os.path.join(ROOT, "compile_commands.json")
    with open(out_path, "w") as f:
        json.dump(entries, f, indent=2)
        f.write("\n")

    print(f"Wrote {len(entries)} entries to {out_path}")


if __name__ == "__main__":
    main()
