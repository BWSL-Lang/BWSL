# Fuzzing

`bwslc` has a libFuzzer harness that exercises the whole **lex → parse →
IR lowering → CFG/SSA → SPIR-V** pipeline in-process with ASan + UBSan.
Text-backend cross-compile (Metal/HLSL/GLSL/GLES) is covered by the
regression + equivalence suites, not by this harness. Crashes found here
become regression tests under `tests/fuzz_regressions/`.

## Quick start

```bash
# Build the fuzzer (Apple clang ships without libclang_rt.fuzzer on some
# Xcode versions; the Makefile auto-picks Homebrew LLVM when present).
make bwslc-fuzz

# Seed the corpus from the test suite.
mkdir -p fuzz/corpus fuzz/crashes
cp tests/*.bwsl tests/equivalence/*.bwsl fuzz/corpus/

# Fuzz for 10 minutes with the BWSL-keyword dictionary enabled.
./build/bwslc-fuzz \
  -max_total_time=600 -max_len=4096 -timeout=5 \
  -dict=fuzz/bwsl.dict \
  -artifact_prefix=fuzz/crashes/ \
  fuzz/corpus/
```

Crashes land in `fuzz/crashes/` as `crash-<hash>` files; hangs land as
`timeout-<hash>`. Reproduce either by running bwslc on them directly:

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
3. Delete the raw `fuzz/crashes/crash-*` or `fuzz/known_bugs/` artifact; the
   regression test is the canonical reproducer now.

## Known open bugs

`fuzz/known_bugs/` holds minimized reproducers for bugs the fuzzer has found
but nobody has fixed yet. Each file starts with a header comment explaining
what's wrong. They're *not* wired into `make test` — adding them there would
turn the suite red. Pick one, fix it, move the file to `tests/fuzz_regressions/`.

## The dictionary

`fuzz/bwsl.dict` lists BWSL keywords and common tokens. libFuzzer splices
these into mutations as atomic units, so instead of random byte-flipping
you get mutations like "inject the token `compute` here", which dramatically
increases the share of mutations that make it past the lexer. Measured
impact on a 3-minute run: coverage (features) doubled from ~4.8k to ~9.9k
compared to the harness without the dictionary.

## What's in version control

Tracked:

- `tools/bwslc_fuzz.cpp` — harness
- `Makefile` target `bwslc-fuzz`
- `fuzz/bwsl.dict` — libFuzzer dictionary
- `fuzz/known_bugs/*.bwsl` — unfixed reproducers, annotated
- `tests/fuzz_regressions/*.bwsl` — minimized reproducers for fixed bugs
- this README

Ignored (`.gitignore`):

- `fuzz/corpus/` — libFuzzer's grown mutation corpus (recreate with
  `cp tests/*.bwsl fuzz/corpus/`)
- `fuzz/crashes/` — raw crash / timeout artifacts before minimization
- `crash-*`, `timeout-*` — artifacts written without `-artifact_prefix`

## Scope

Current harness covers the in-process compile through SPIR-V emit.
**Not covered**: the text backends (Metal/HLSL/GLSL/GLES via SPIRV-Cross) and
differential fuzzing across backends. The equivalence suite already does
differential checking on curated compute shaders; extending the fuzzer to
generate valid programs and route them through the equivalence runner is a
natural next step for finding cross-backend miscompiles.
