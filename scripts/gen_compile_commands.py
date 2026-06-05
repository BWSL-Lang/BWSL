#!/usr/bin/env python3
"""Generate compile_commands.json and IDE preamble headers for the BWSL project.

Actual TUs get their own entries. .inl shards get entries with:
  - A force-included preamble that provides the types they depend on
  - -DBWSL_CLANGD, which activates the namespace guards added to each shard

The guards (#ifdef BWSL_CLANGD / namespace X { / #endif) ensure the shard
compiles in the right namespace when built standalone by clangd, while the
real unity build (which never defines BWSL_CLANGD) is unaffected.

Preambles are written to build/ide/ (already gitignored via build/).
Run from the repository root:  python3 scripts/gen_compile_commands.py
"""

import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDE_DIR = os.path.join(ROOT, "build", "ide")

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

# Each group maps a directory of .inl files to its parent file.
# type "cpp"    -> extract #include lines from the .cpp before the first .inl
# type "header" -> strip the .inl #includes from the header, keep everything else
#                  (needed for ir_lowering where the struct definition is in the header)
INL_GROUPS = [
    {
        "dir": "phases/parser",
        "parent": "phases/parser/bwsl_parser_soa.cpp",
        "preamble": "parser_preamble.h",
        "type": "cpp",
    },
    {
        "dir": "phases/backends/spirv",
        "parent": "phases/backends/spirv/bwsl_spirv_backend.cpp",
        "preamble": "spirv_preamble.h",
        "type": "cpp",
    },
    {
        "dir": "phases/ir_lowering",
        "parent": "phases/ir_lowering/bwsl_ir_lowering.h",
        "preamble": "ir_lowering_preamble.h",
        "type": "header",
    },
]


def read_lines(rel_path):
    with open(os.path.join(ROOT, rel_path)) as f:
        return f.readlines()


def is_inl_include(line):
    return '#include' in line and '.inl"' in line


def make_cpp_preamble(parent_rel):
    """Return only the #include lines from a .cpp before the first .inl include.

    Deliberately excludes namespace openings so the preamble has no open scopes;
    the .inl file's own #ifdef BWSL_CLANGD guard handles namespace placement.
    """
    return ''.join(
        line for line in read_lines(parent_rel)
        if not is_inl_include(line) and line.strip().startswith('#include')
    )


def make_header_preamble(parent_rel):
    """Return the header with all .inl #include lines removed.

    For ir_lowering the struct definition lives in the header, so we need the
    full header content — just without the shard includes that would cause
    double-compilation.
    """
    return ''.join(
        line for line in read_lines(parent_rel) if not is_inl_include(line)
    )


def find_inl_files(directory):
    dir_path = os.path.join(ROOT, directory)
    return sorted(
        os.path.join(directory, name).replace('\\', '/')
        for name in os.listdir(dir_path)
        if name.endswith('.inl')
    )


def make_entry(rel_path, extra_args=None):
    include_args = [f"-I{d}" for d in INCLUDE_DIRS]
    args = [COMPILER] + BASE_FLAGS + include_args + (extra_args or []) + ["-c", rel_path]
    return {
        "directory": ROOT,
        "file": os.path.join(ROOT, rel_path),
        "arguments": args,
    }


def main():
    os.makedirs(IDE_DIR, exist_ok=True)

    entries = []

    # Actual translation units
    for src in sorted(TRANSLATION_UNITS):
        if os.path.exists(os.path.join(ROOT, src)):
            entries.append(make_entry(src))

    # .inl shards: compile standalone with a context preamble + BWSL_CLANGD
    for group in INL_GROUPS:
        if group["type"] == "cpp":
            content = make_cpp_preamble(group["parent"])
        else:
            content = make_header_preamble(group["parent"])

        preamble_abs = os.path.join(IDE_DIR, group["preamble"])
        with open(preamble_abs, "w") as f:
            f.write(content)

        for inl in find_inl_files(group["dir"]):
            entries.append(make_entry(inl, extra_args=[
                "-DBWSL_CLANGD",
                "-include", preamble_abs,
                "-x", "c++",
            ]))

    out_path = os.path.join(ROOT, "compile_commands.json")
    with open(out_path, "w") as f:
        json.dump(entries, f, indent=2)
        f.write("\n")

    tus = [e for e in entries if "-DBWSL_CLANGD" not in e["arguments"]]
    inls = [e for e in entries if "-DBWSL_CLANGD" in e["arguments"]]
    print(f"Wrote {len(entries)} entries to {out_path}")
    print(f"  {len(tus)} standalone TUs")
    print(f"  {len(inls)} .inl shards (preamble + -DBWSL_CLANGD)")
    for g in INL_GROUPS:
        print(f"    {g['dir']}  ←  {g['preamble']}")


if __name__ == "__main__":
    main()
