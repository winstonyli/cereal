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
- gcc.dg: 3714 of 3744 files run give the same diagnostics as gcc (message,
  line and column); 30 differ.
- c-c++-common: 632 of 636 identical; 4 differ.
- 1307 golden tests pass; the sanitizer build is clean on 478 files.
- uvloop's `loop.c` checks in about 3.73 G instructions (callgrind).
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

## Open differences (representative)
- With `-ftrack-macro-expansion=0` gcc locates `#if` diagnostics from a macro
  body at the macro use (binary-constants-2/3).
- `#line 4294967295` overflow numbering (pr89410-1).
- Struct-size alignment error location (pr97164); `aligned(i)` errors in
  builtin-has-attribute.
- A tail of one- and two-diagnostic files with distinct causes: attribute
  conflict wording, `-Wmisleading-indentation`, `-Wcast-align`, padding and
  overflow notes.  HISTORY.md lists them per round.

## Working conventions
- Every fix leaves a golden in `tests/check` (`NAME.c` + `NAME.expected`,
  first line `// flags: ...` when needed), verified against gcc-13 first.
- Run long jobs at low priority and at most 12 of 16 threads.
- Add a dated round entry to HISTORY.md after each batch of changes.
- Work goes straight to `main`; milestones are tagged (`v0.1.0` is the first).
