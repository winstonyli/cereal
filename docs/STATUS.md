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
1. Mutated-corpus verdict parity: ran 2026-09-30 (bench/tools/mut.py, 170
   units x 30 mutants, seed 7, gcc 15.2): 5004 agree, 33 gcc-only semantic
   rejects, 3 "failures", all `char x[4]` -> `char x[]` inside glibc's
   pthread unions: cereal says "flexible array member in union", gcc >= 14
   accepts it (extension).  Confirmed a gcc-version difference: gcc 13.3
   c-decl.cc:9242 errors unconditionally, so cereal matches the reference.  lua52/lua53 lua.c are skipped (need libreadline-dev).
   Next: larger N/other seeds; a gcc 13 binary would remove the -fpermissive
   and flexible-array-union noise.
   gcc.dg baseline 2026-09-30 (gcc-13 13.4, `par.py $PWD/cereal dg out.json`
   with CEREAL_GCC=gcc-13, ~/gccts sparse 13.3.0 checkout, absolute cereal
   path): n=6176, rejects-valid 82, accepts-invalid 103, crash 0, timeouts 1,
   both-reject 939 of which 347 have identical headers.  Most common wording
   gaps among both-reject files: "expected X or Y before string constant"
   (gcc) vs "expected X before 'tok'" (cereal) and gcc's multi-token
   expectation lists; "before 'tok' token" suffix; "expected declaration
   specifiers" vs "expected declaration"; "unknown type name" where cereal
   says "expected ...".
   After the parser wording work (token descriptions, unknown type name,
   file-scope stray tokens): identical headers 347 -> 373 of 939, verdicts
   unchanged, corpus still 0/0.  Not ported: the tag hint ("use struct
   keyword to refer to the type") and "did you mean" (need tag scope and
   fuzzy lookup); gcc's K&R-parameter mode after an earlier error makes
   later file-scope declarations report "expected declaration specifiers"
   (about 80 cascade pairs in gcc.dg, e.g. c2x-constexpr-1.c): not emulated.
   gcc puts "expected ';'" at the end of the previous token in some cases
   (`foo void *v;` 1:4 vs cereal 1:5).
   Better metric (par.py now prints it): gcc errors reproduced exactly in
   both-reject files: 6935/10063 after token descriptions, 7079 after unknown
   type name, 7525 (74.8%) after the declarator-list expectations ("expected
   ',' or ';'", "'=', ',', ';', 'asm' or '__attribute__'", missing ';' at
   the end of the previous token).  Identical-header files: 379/939.
   After c_parser_require's location rule (a missing ')' ']' ';' ',' ':'
   goes at the end of the previous token unless an error is pending,
   Parser.err_live) and gcc's struct-member terminator lists: 7929/10063
   (78.8%), identical-header files 382/939.  Biggest known levers left:
   (a) tokens from macro expansions: gcc reports at the spelling location
   inside the macro definition (20020103-1.c:22:7), cereal at the expansion
   point (37:3) -- about 500 gcc-only/cereal-only pairs; (b) "expected X"
   before statements/expressions not yet compared; (c) 'undeclared ...; did
   you mean' (fuzzy lookup), "assignment of read-only location" quoting,
   "cannot initialize array of ... from a string literal" wording.
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
  c-parser.cc) were in the session scratchpad and are not in the repo: run
  `bench/tools/fetch_gcc_refs.sh [DIR]` (downloads gcc-13.3.0 c-decl.cc,
  c-parser.cc, c-typeck.cc, c-lex.cc, libcpp/charset.cc, libcpp/expr.cc from
  github.com/gcc-mirror/gcc tag releases/gcc-13.3.0).  Line landmarks cited
  in docs/CHECKER_PLAN.md (c-decl.cc: shadow_tag_warned 4832, grokparm 5972,
  grokdeclarator 6545, grokparms 8195, get_parm_info 8324, declspecs_add_type
  11453, add_scspec 12338, finish_declspecs 12540) refer to that version.
- Windows host: no make/gcc; build and test in WSL Ubuntu (default shell is
  fish: wrap commands in `bash -lc`, or run a script).  Work from WSL's own
  filesystem (cp -r to ~): under /mnt/c four tests/lsp sessions time out
  (504/508); on ext4 508/508.  The repo has .gitattributes (eol=lf); a
  Windows checkout with core.autocrlf=true breaks tools/probe-host.sh.
- Corpus: `sh bench/fetch_corpus.sh ~/corpus`; needs python3-dev headers:
  `CEREAL_PYINC=/usr/include/python3.13 python3 bench/tools/mut.py ./cereal
  ~/corpus N SEED`.  mut.py passes -fpermissive to gcc (gcc >= 14 made
  implicit-int/function-declaration/pointer mismatches errors; gcc 13, the
  reference, warns) and caps its pool at 3/4 of the cores.
- cdecl.c is a single assembled source; edit it directly.
- Preferences: succinct replies; compile speed matters; do things right
  regardless of upfront cost; no PR unless asked; commit trailers as in git
  log.
