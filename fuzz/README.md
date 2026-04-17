# Fuzzing

`bwslc` has a libFuzzer harness that exercises the lex+parse pipeline with
ASan + UBSan. Crashes found here become regression tests under
`tests/fuzz_regressions/`.

## Quick start

```bash
# Build the fuzzer (Apple clang ships without libclang_rt.fuzzer on some
# Xcode versions; the Makefile auto-picks Homebrew LLVM when present).
make bwslc-fuzz

# Seed the corpus from the test suite.
mkdir -p fuzz/corpus fuzz/crashes
cp tests/*.bwsl tests/equivalence/*.bwsl fuzz/corpus/

# Fuzz for 3 minutes.
./build/bwslc-fuzz \
  -max_total_time=180 -max_len=4096 -timeout=5 \
  -artifact_prefix=fuzz/crashes/ \
  fuzz/corpus/
```

Crashes land in `fuzz/crashes/` as `crash-<hash>` files. Reproduce any of them
by running the compiler on them directly:

```bash
./build/bwslc fuzz/crashes/crash-<hash>
```

## Minimizing a crash

libFuzzer can shrink a crashing input to the smallest byte sequence that still
triggers the bug:

```bash
./build/bwslc-fuzz -minimize_crash=1 -runs=10000 -timeout=5 \
  -exact_artifact_path=/tmp/min_crash \
  fuzz/crashes/crash-<hash>
cat /tmp/min_crash
```

## After fixing a bug

1. Hand-craft a small named reproducer under
   `tests/fuzz_regressions/<descriptive_name>.bwsl`, with a comment pointing
   at the fix.
2. Verify `./tests/run_tests.sh` passes it. The runner checks that bwslc
   exits with code 0 or 1 within 5 seconds — anything else (signal death,
   timeout, non-zero exit) counts as a regression.
3. Delete the raw `fuzz/crashes/crash-*` artifact; the regression test is the
   canonical reproducer now.

## What's in version control

Tracked:

- `tools/bwslc_fuzz.cpp` — harness
- `Makefile` target `bwslc-fuzz`
- `tests/fuzz_regressions/*.bwsl` — minimized reproducers for bugs fixed so far
- this README

Ignored (`.gitignore`):

- `fuzz/corpus/` — libFuzzer's grown mutation corpus (recreate with
  `cp tests/*.bwsl fuzz/corpus/`)
- `fuzz/crashes/` — raw crash artifacts before minimization
- `crash-*` — crashes written without `-artifact_prefix`

## Scope

The harness currently exercises **lex + parse only**. The full pipeline
(IR lowering → SSA → SPIR-V → cross-compile) is covered by the equivalence
suite and regression tests, but not by libFuzzer. Extending the harness
through codegen is a worthwhile next step — a grammar-based generator that
produces syntactically valid programs would then hit the lowering paths
where miscompiles tend to hide.
