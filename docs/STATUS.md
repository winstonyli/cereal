# Status

A short summary of where the project stands.  The round-by-round log is in
[HISTORY.md](HISTORY.md); design notes are in ARCHITECTURE.md, PARSER.md,
TYPES.md, PARALLEL.md, LSP.md and SPECS.md.

## What exists
- A C99 + GNU preprocessor (`-E`, byte-identical to `gcc -E` on the corpus,
  parallel for large files), with macro / conditional / include analyses
  (`cereal lint`).
- A hand-written C99 + GNU parser and a type checker that reports gcc-13
  diagnostics (`-fsyntax-only`, `-std=c99`, `-pedantic`, `-W...`).
- An index and LSP server over the same engine (`cereal index|query|lsp`).

## Numbers (gcc-13, `-std=c99 -pedantic -fsyntax-only`, last gate)
- gcc.dg: 3729 of 3744 files run give the same diagnostics as gcc (message,
  line and column); 15 differ.
- c-c++-common: 634 of 636 identical; 2 differ.
- gcc.dg/cpp (preprocessor tests, now in verify.sh): 240 of 266 identical; 26 differ. A baseline, not yet a target; see HISTORY Rounds 142, 146 to 150.
- 1417 golden tests pass; the sanitizer build is clean on 533 files.
- uvloop's `loop.c` checks in about 3.82 G instructions (callgrind).
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
```

`bench/tools/gate.sh` runs all of it (goldens, parity, sanitizers,
callgrind) in parallel; see its header for the expected numbers.

## Open differences (gcc.dg 15, c-c++-common 2)
Needs gcc's optimizer or middle end (out of reach for a front end):
invalid-call-1, pr56355-1, pr62090 (`-O2` nonnull),
large-size-array-6, pr88074-2, strlenopt-78, pr83844.

Darwin / Objective-C targets: darwin-cfstring-format-1, pr105522.

Front-end gaps, each a small separate cause:
- array-10: gcc adds "empty declaration" after a file-scope struct whose member
  has a variably modified type (rule not derived).
- Warray-parameter-11, multiple-overflow-warn-3, pr100547, pr100619 (VLA
  parameter type spelling), pr108375-2.
- A parameter of a function definition with `restrict` on a non-pointer is
  reported twice by gcc (once by us).

c-c++-common: dump-ada-spec-14, unroll-5 (needs an
enum-constant symbol kind in the parser).

Not counted as differences (same diagnostics, so the count ignores them):
binary-constants-1 (exit code) and init-bad-4 (message order).  HISTORY.md has the per-round detail.

## Working conventions
- Every fix leaves a golden in `tests/check` (`NAME.c` + `NAME.expected`,
  first line `// flags: ...` when needed), verified against gcc-13 first.
- Run long jobs at low priority and at most 12 of 16 threads.
- Add a dated round entry to HISTORY.md after each batch of changes.
- Work goes straight to `main`; milestones are tagged (`v0.1.0` is the first).
