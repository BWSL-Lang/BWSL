# Fuzzing handoff

State of the fuzz infrastructure as of 2026-04-18. Read this when resuming
fuzz work after a context reset.

## TL;DR

- Harness covers **lex → parse → IR lowering → CFG/SSA → SPIR-V emit**
  in-process with ASan + UBSan. Text-backend cross-compile (Metal/HLSL/GLSL/GLES)
  is *not* yet fuzzed.
- Last two runs: 1h → 479 artifacts (all parser bugs) → systemic fix applied
  → 5-minute re-run → **0 artifacts**. The parser is now robust.
- Next run should target **IR lowering + SSA + SPIR-V codegen** — that's
  where unfuzzed territory remains. See "Next run" below.

## What exists and is tracked

| Path | Purpose |
|---|---|
| `tools/bwslc_fuzz.cpp` | libFuzzer harness; full in-process pipeline per input |
| `Makefile` target `bwslc-fuzz` | Auto-picks Homebrew LLVM (Apple clang lacks libclang_rt.fuzzer); matches libc++ rpath |
| `fuzz/bwsl.dict` | 151-entry libFuzzer dictionary — BWSL keywords / intrinsics / operators |
| `fuzz/README.md` | Developer-facing usage docs |
| `fuzz/HANDOFF.md` | **this file** |
| `tests/fuzz_regressions/*.bwsl` | Minimized reproducers for fixed bugs; wired into `./tests/run_tests.sh` |

Ignored (`.gitignore`): `fuzz/corpus/`, `fuzz/crashes/`, `crash-*`, `timeout-*`.

## The two bug templates every fuzz run has hit

Both of these have been systematically fixed in the parser. When you find
new crashes, check whether they fit one of these before writing fresh logic —
it's probably the same pattern in a new location.

### Template A — Unchecked `std::sto*`

`std::stoi` / `std::stoul` / `std::stof` throw `std::invalid_argument` or
`std::out_of_range` on inputs the lexer accepts as `NUMBER` tokens (e.g.
`0x` with no hex digits, `1e99999`, integers overflowing u32).

**Fix pattern**: `SafeParseU32` / `SafeParseInt` / `SafeParseFloat` at the top
of `bwsl_parser_soa.cpp`, and `SafeStoi` / `SafeStof` at the top of
`bwsl_render_config.h`. All existing call sites use the helpers. If a new
one appears, route it through a helper.

### Template B — Parser loop that doesn't always advance

Any `while (!Check(RIGHT_BRACE) && !Check(EOF_TOKEN))` where the body's
error-recovery path (`Consume` failing, then `Synchronize`, then `continue`)
can leave `current` unchanged. Each iteration allocates an AST/arena node →
arena exhaustion → `printf("Allocation failed due to insufficient capacity")`
repeating forever.

**Fix pattern**: `Parser::ProgressGuard` RAII helper in `bwsl_parser_soa.h`.
Drop `ProgressGuard _pg_(this);` at the top of any such loop. Destructor
forces `Advance()` if a full iteration consumed zero tokens. Already applied
to all 23 known loops of this shape.

> If an arena allocation failure shows up in bwslc output it is **always an
> infinite loop**, not a real OOM. The compiler normally allocates a bounded
> amount per shader.

## How to run

```bash
# Build (uses Homebrew LLVM clang automatically).
make bwslc-fuzz

# Seed: tests/ and tests/equivalence/ give coverage of the full language.
# Also include fuzz_regressions so libFuzzer has fixed-bug starting points.
rm -rf fuzz/corpus fuzz/crashes
mkdir -p fuzz/corpus fuzz/crashes
cp tests/*.bwsl tests/equivalence/*.bwsl tests/fuzz_regressions/*.bwsl fuzz/corpus/

# 1-hour run. -fork=1 keeps going past crashes; ignore_* stops the session
# from terminating on the first timeout.
./build/bwslc-fuzz \
  -fork=1 -ignore_crashes=1 -ignore_timeouts=1 \
  -max_total_time=3600 -max_len=4096 -timeout=5 \
  -dict=fuzz/bwsl.dict \
  -artifact_prefix=fuzz/crashes/ \
  fuzz/corpus/ 2>&1 | tee fuzz/run.log
```

Afterwards:

```bash
# Classify artifacts
ls fuzz/crashes/crash-* | wc -l
ls fuzz/crashes/timeout-* | wc -l

# Bucket crashes by error signature (post-stof-fix binary)
for c in fuzz/crashes/crash-*; do
    ./build/bwslc "$c" 2>&1 | \
        grep -E "^Unhandled exception|AddressSanitizer|SEGV|runtime_error|terminate" | head -1
done | sort | uniq -c | sort -rn

# Bucket timeouts by dominant keyword
for c in fuzz/crashes/timeout-*; do
    for kw in variants struct enum pipeline pass for loop match eval import module; do
        if grep -qa "\b$kw\b" "$c" 2>/dev/null; then echo "$kw"; break; fi
    done
done | sort | uniq -c | sort -rn
```

Minimize a crash to its smallest reproducer:

```bash
./build/bwslc-fuzz -minimize_crash=1 -runs=100000 -timeout=5 \
    -exact_artifact_path=/tmp/min_crash \
    fuzz/crashes/crash-<hash>
```

Promote a minimized reproducer into `tests/fuzz_regressions/<descriptive_name>.bwsl`
with a header comment pointing at the fix. Run `./tests/run_tests.sh` to
verify — the runner enforces exit ∈ {0, 1} in ≤ 5s.

## Next run: IR lowering + codegen coverage

The current harness already reaches these stages, but the *parser* was the
soft target. With parser bugs fixed, mutations that now get past parsing
will start landing in lowering / SSA / SPIR-V codegen. Expect a different
bug shape.

### What to watch for

Codegen bugs typically surface as:

- SPIR-V validation failures printed by `bwslc`'s internal validator —
  observe them via `Error: SPIR-V validation failed: ...` followed by
  `OpCode result type mismatch`, `Id is 0`, `Expected operand to be...` etc.
- ASan reports from within `bwsl_spirv_backend.cpp` / `bwsl_ir_lowering.h`
  / `bwsl_ssa.cpp` — stack trace will name `SPIRVBuilder::...` or
  `IRLowering::...`.
- Asserts in `bwsl_ir_analysis.cpp` (used for workgroup size, storage flags,
  capability detection).
- New infinite-loop families if any loop in lowering or SSA lacks an
  analogue of the parser's `ProgressGuard`. (Grep for `while (true)` and
  long `for`/`while` inside lowering passes if hangs appear.)

### Classifier additions for codegen-stage bugs

Extend the bucketing above with these signatures:

```bash
# Crash bucketing — codegen-layer signatures
grep -E "SPIR-V validation failed|OpCode|OpLoad|OpStore|OpAccessChain|\
IRLowering|SSAConstructor|SPIRVBuilder|analysis\.cpp|assert" -o
```

When codegen hangs appear, the classifier can also check for IR opcodes
in bwslc output: `JUMP` / `BRANCH` loops without merge blocks, or SSA
conversions on degenerate CFGs.

### Likely families to hit

Based on what the compiler's architecture looks like:

1. **OpAccessChain mismatches** — struct / array / matrix access on
   pathological IR (already seen: compute SSBO missing struct-member index,
   dynamic matrix column as OpUndef). Fuzz will likely find more.
2. **SPIR-V type mismatches** at OpSelect / OpFAdd / etc. where the backend
   assumes matching types but the IR delivers mixed.
3. **CFG assertions** when branch targets are out of range or loops are
   irreducible.
4. **Integer bucket mistakes** — FSign vs SSign, SAbs vs FAbs, SDiv vs UDiv,
   S/URem — we already fixed one (`sign(int)`); there are more.
5. **Uninitialized register reads** — IR register used before defined.

### Expected fix patterns

For SPIR-V backend bugs, most fixes are small — type-dispatch on operand
register type (see the `sign` / `inverse` fixes) or adding a missing case
in the big switch in `bwsl_spirv_backend.cpp`. Scope creep risk: don't
refactor the backend while fixing bugs; just plug each hole.

## Running totals (for orientation at next resumption)

| | |
|---|---|
| Regression tests | 153 pass |
| Golden files | 50 matched across 4 backends |
| Cross-backend validators | Metal 309 / HLSL 311 / GLSL 307 / GLES 291 — all 0 fail |
| Equivalence tests | 54 pass cross-backend (native SPIR-V ≡ HLSL→SPIR-V ≡ GLSL→SPIR-V) |
| Fuzz regressions (fixed bugs) | 5 files under `tests/fuzz_regressions/` |
| Compile time on a typical shader | Sub-ms native, 1–4 ms in WASM browser build |

## One-liner to pick back up

```bash
make bwslc-fuzz && \
  rm -rf fuzz/corpus fuzz/crashes && mkdir -p fuzz/corpus fuzz/crashes && \
  cp tests/*.bwsl tests/equivalence/*.bwsl tests/fuzz_regressions/*.bwsl fuzz/corpus/ && \
  ./build/bwslc-fuzz -fork=1 -ignore_crashes=1 -ignore_timeouts=1 \
    -max_total_time=3600 -max_len=4096 -timeout=5 \
    -dict=fuzz/bwsl.dict -artifact_prefix=fuzz/crashes/ \
    fuzz/corpus/ 2>&1 | tee fuzz/run.log
```

When it finishes, bucket artifacts (see script above), minimize the top 3-5
unique bugs, fix them (following the established patterns), add regression
tests, re-run. Repeat until a long run produces zero new artifacts.
