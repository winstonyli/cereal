# Status and handoff (end of the P2 session)

Branch `claude/c99-macro-analysis-qltej1`; P2a-P2f committed (last: P2e-P2f).
`make test` 508 pass.  Architecture: docs/PARSER.md, docs/TYPES.md (checker,
summaries, gaps), docs/SPECS.md (spec/proof design, unchanged this session),
docs/CHECKER_PLAN.md (the brief the checker subagents worked from: AST
layouts, gcc rules ported, walk design).

## Results (gcc 13, `-std=c99 -pedantic -fsyntax-only`)
- Real-code corpus (170 files): 0 rejects-valid, 0 accepts-invalid.
- gcc.dg (6217): 77 rejects-valid, 104 accepts-invalid; 47% of files with any
  diagnostic have identical message+line:col.
- Speed: check ~1.6x (lua54.c) to ~2x (loop.c) parse time; goal check <= parse
  not met.  Summaries add ~4% to the check pass.  RSS check vs parse: 13/11 MB
  (lua54), 28/24 MB (loop.c).
- ASAN/UBSAN clean on corpus and gcc.dg except a 300 s timeout on pr59992.c;
  not rerun over tests/ after the last edits.

## Next work
1. Mutated-corpus verdict parity (bench/tools/mut.py; path to corpus.py fixed,
   never successfully run).
2. gcc.dg gaps: parser "expected X before Y" wording; "excess elements in
   struct initializer"; "jump into scope of identifier with variably
   modified type"; attribute-ignored/argument-count warnings; __builtin_*
   argument checks; 8 "initializer element is not constant"; g == g folding.
3. Speed: get check <= parse (profile cexpr.c/cdecl.c/check.c).
4. Known diffs: restrict misuse reported once (gcc twice); `enum S;` for an
   existing struct tag lacks the pedwarn; "empty enum is invalid";
   -Wmisleading-indentation exotic layouts; "In function" context lines and
   caret widths (renderer); -Waddress at lgc.c:78 (lupa lua52) and one extra
   -Wsign-compare at lua54.c:46655.
5. P3 (incremental units, early cutoff) consumes csum.h: checker_summary,
   summary_valid, checker_set_unit_key; see "Summaries and read sets" in
   docs/TYPES.md.  Read sets do not cover once-per-TU diagnostic notes or the
   pack stack beyond its digest.

## Workflow notes
- Harness: bench/tools/{par,hd,san,mut}.py (parity, headers, sanitizers,
  mutants); gcc sources used as the port reference (c-decl.cc, c-typeck.cc,
  c-parser.cc) were in the session scratchpad and are not in the repo: fetch
  gcc 13.3 from upstream to regenerate.
- cdecl.c is a single assembled source; edit it directly.
- Preferences: succinct replies; compile speed matters; do things right
  regardless of upfront cost; no PR unless asked; commit trailers as in git
  log.
