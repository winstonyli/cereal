# Status

A short summary of where the project stands.  The round-by-round log is in
[HISTORY.md](HISTORY.md); design notes are in ARCHITECTURE.md, PARSER.md,
TYPES.md, PARALLEL.md, LSP.md and SPECS.md; the plan for what comes next is
in [ROADMAP.md](ROADMAP.md).

## What exists
- A C99 + GNU preprocessor (`-E`, byte-identical to `gcc -E` on the corpus,
  parallel for large files), with macro / conditional / include analyses
  (`cereal lint`).
- A hand-written C99 + GNU parser and a type checker that reports gcc-13
  diagnostics (`-fsyntax-only`, `-std=c99`, `-pedantic`, `-W...`).
- An index and LSP server over the same engine (`cereal index|query|lsp`).

## Numbers (gcc-13, `-std=c99 -pedantic -fsyntax-only`, last gate)
- gcc.dg: 3869 of 3910 files run give the same diagnostics as gcc (message,
  line and column); 41 differ (5 old, 36 in the C11/C17 tests added by -std=c11).
- c-c++-common: 634 of 636 identical; 2 differ.
- gcc.dg/cpp (preprocessor tests, now in verify.sh): 285 of 285 identical. A baseline, not yet a target; see HISTORY Rounds 142, 146 to 150, 157 to 164.
- 1567 golden tests pass (the count grows by one per new source file: the dogfood and parse cases walk src/); the sanitizer build is clean on 567 files.
- uvloop's `loop.c` checks in about 3.85 G instructions (callgrind).
"Files run" are the files whose `dg-options` and selectors cereal models;
the rest are skipped, not counted as passes.

## How to check it
The gcc testsuite is not vendored.  Make a sparse checkout and run the gate
(Linux or WSL):

```
git clone --filter=blob:none --no-checkout --depth 1 --branch releases/gcc-13.3.0 \
    https://github.com/gcc-mirror/gcc ~/gccts
git -C ~/gccts sparse-checkout set gcc/testsuite/gcc.dg gcc/testsuite/c-c++-common
git -C ~/gccts checkout
make -j6 && sh tests/run.sh          # goldens, fuzzers
CEREAL_GCC=gcc-13 bench/tools/verify.sh   # parity vs gcc-13
bench/tools/cmp.sh gcc.dg/FILE.c     # diff one file against gcc-13
bench/tools/corp.sh DIR [-IDIR...]  # per-file cereal vs gcc-13 on any C sources
```

`bench/tools/gate.sh` runs all of it (goldens, parity, sanitizers,
callgrind) in parallel; see its header for the expected numbers.

## Open differences (gcc.dg 41, c-c++-common 2)
Needs gcc's optimizer or middle end (out of reach for a front end):
pr56355-1 (-Wstrict-overflow), pr83844 (alignment of a VLA-offset member).

Darwin / Objective-C targets (the expected output comes from a target gcc
does not model on Linux): darwin-cfstring-format-1, pr105522.

Parked front-end gaps (reasons in HISTORY Rounds 172 to 175):
- pr100547: the position of the `vector_size` error; no single rule found.
- unroll-5: `#pragma GCC unroll j` with a non-constant `j` needs the argument
  judged by the checker, not the text scanner.
- dump-ada-spec-14: needs `-fdump-ada-spec` (gcc's Ada dumper).

Not scored by verify.sh: the macro notes ("in definition of macro" for a token
a parameter substituted, the full expansion chain). The design for fixing
them with a per-token provenance is in HISTORY (Round 175 and after).

Not counted as differences (same diagnostics, so the count ignores them):
binary-constants-1 (exit code) and init-bad-4 (message order).  HISTORY.md has the per-round detail.

## Working conventions
- Every fix leaves a golden in `tests/check` (`NAME.c` + `NAME.expected`,
  first line `// flags: ...` when needed), verified against gcc-13 first.
- Run long jobs at low priority and at most 12 of 16 threads.
- Add a dated round entry to HISTORY.md after each batch of changes.
- Work goes straight to `main`; milestones are tagged (`v0.1.0` is the first).
