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
- gcc.dg: 3908 of 3910 files run give the same diagnostics as gcc (message,
  line and column); 2 differ.
- c-c++-common: 635 of 636 identical; 1 differs.
- gcc.dg/cpp (preprocessor tests, now in verify.sh): 285 of 285 identical. A baseline, not yet a target; see HISTORY Rounds 142, 146 to 150, 157 to 164.
- 1658 golden tests pass (the count grows by one per new source file: the dogfood and parse cases walk src/); the sanitizer build is clean on 601 files.
- uvloop's `loop.c` checks in about 3.89 G instructions (callgrind).
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
bench/tools/symcov.sh [--gnu11]      # --verify-symbols over corpus + dg tests: 0 unindexed
bench/tools/cmp.sh gcc.dg/FILE.c     # diff one file against gcc-13
bench/tools/corp.sh DIR [-IDIR...]  # per-file cereal vs gcc-13 on any C sources
```

`bench/tools/gate.sh` runs all of it (goldens, parity, sanitizers,
callgrind) in parallel; see its header for the expected numbers.

## Open differences (gcc.dg 2, c-c++-common 1)
Needs gcc's optimizer or middle end (out of reach for a front end):
pr56355-1 (-Wstrict-overflow=4 at -O2: the warning comes from fold-const's
tree_expr_nonnegative_warnv_p on `abs (i * i)`), pr83844 (alignment of a
VLA-offset member: stor-layout folds the byte offset `n * 4 + 8` to
`((sizetype)(n) + 2) * 4` and prints that tree).
c-c++-common: dump-ada-spec-14 (gcc's Ada dumper).


Round 185, latest verify (gcc-13): gcc.dg 3908 of 3910 identical, c-c++-common 635 of 636,
cpp 285 of 285; `tests/run.sh` 1621 passed.  Remaining gcc.dg diffs: pr56355-1,
pr83844 (above).
Noticed but not fixed (each needs its own mechanism, none is in gcc.dg):
- `char * const _Atomic c` parameter of main prints as `char * _Atomic`
  (gcc `char * _Atomic const`): the parameter type loses const.
- Redeclaration note for `double cabs(int)` after <complex.h> prints
  `complex double` at 112:23 without the include chain; gcc `_Complex double`
  at 112:1 with it.
- Const fenv/fexcept pointer parameters (nonconst pointer for a const builtin
  parameter) not probed.
- _FloatN builtin rows only checked through the implicit-call harness
  (bench bt_gen); `-std=c2x` and constant-expression visibility unchecked.

Parked front-end gaps (reasons in HISTORY Rounds 172 to 175 and 184):
- `vector_size` errors are reported once per declaration; gcc reports once per
  declarator (`typedef int __attribute__((vector_size(0))) V, W;` gives two),
  and inside a one-line `struct { ... }` it uses the `{` as the position.
- `format(printf, f, 2)` with an undeclared name in the attribute: gcc prints
  `'f' undeclared here (not in a function)` (cereal does so for `format_arg`
  and the other expression attributes, but not for `format`, whose first
  argument is an identifier).
- pr56355-1, pr83844: fold-const results, see Open differences.
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
