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
   Later the same day (8 commits on top): aligned/_Alignas wording; wide
   string arrays by compatibility; block-scope `extern` of an incomplete
   array merged with the file-scope declaration; unknown-type-name tag hint
   and did-you-mean (typedefs + type keywords; Parser.tags, src/c/fuzzy.c
   shared with the checker); gcc's K&R-mode cascade: a file-scope function
   declarator not followed by `= , ; asm __attribute__` starts a definition
   whose parameter declarations run to the next `{` (Parser.kr_params: no
   definitions there, plain "expected '=', ..." errors, "expected
   declaration specifiers" + gcc's skip), an error inside a parameter list
   drops the declarator (DeclInfo.failed), a failed declarator list entry is
   not declared, the fabricated unknown type name node carries NF_ERROR, and
   function definitions with syntax errors are now checked (check.c:
   quiet only for other units).  gcc.dg exact 7929 -> 8772 of 10063
   (87.2%; cereal-only errors 846 -> 1020: precision 90%, recall 87%, F1
   unchanged by the last step), identical headers 392/940, corpus 0/0.
   Then `__GIMPLE` (skip the body) and `[[ns::x]]` before C2X (skip to the
   closing `]` like gcc, so later lists in a sequence are diagnosed): exact
   8853 of 10064, identical headers 446/940, rejects-valid 81,
   accepts-invalid 102, tests 525.  Then "'X' attribute ignored" for
   unknown namespace-less `[[X]]` (incl. `gnu` before C2X; cdecl.c
   std_attr_unknown), emitted with the `[[]]` pedwarn even in units with
   syntax errors (parser diagnostics bypass quiet mode), and cinput_loc now
   uses the expansion point for macro tokens: exact 8862.  Still missing
   (~150 warnings): attribute-ignored for known standard names on the wrong
   entity (deprecated/maybe_unused/fallthrough/nodiscard), GNU-style
   `__attribute__((x))` ignored/conflict/mismatch warnings (noinline, used,
   weak ... in attr-invalid.c), and `sizeof ([[]] int)` should be a syntax
   error.
   Then: `[[` is not a type-name start after `(` (sizeof/cast); an erroneous
   primary silences its postfix tail and the closing `)` (Parser.hush/hushed,
   gcc's parser->error); unknown `[[x(...)]]` arguments are skipped as
   balanced tokens; syntax errors are reported at the token's spelling
   location (macro body tokens at the definition, spell_loc): exact 8986 of
   10064, identical headers 448/940, tests 527, corpus 0/0.  A global "no
   error while err_live" rule was tried and is worse (8821): gcc's
   parser->error is reset at more places than sync_stmt/sync_top.
   Attribute semantic warnings (wrong-entity std attributes, GNU conflict/
   mode/argument checks, ~100 files) are mostly tests run with -std=c2x or
   per-attribute handlers; par.py runs everything at -std=c99, so these are
   low value until the harness honours each test's dg-options.
   par.py CEREAL_DGOPTS=1 honours each test's dg-options and keeps only
   C99 tests with options cereal knows (cereal is C99-only: -std=c11/c2x
   tests are out of scope): n=2934, rejects-valid 56, accepts-invalid 92,
   exact 5486 of 6051 (90.7%), identical headers 283/546 -- the honest
   baseline from now on.  Parser errors on macro-body tokens now carry
   "in expansion of macro 'X'" (outermost macro only; nested levels and
   gcc's order of parser errors vs checker warnings are not reproduced).
   Then `&a == &b` of distinct non-weak objects folds; library calls gcc
   folds (atan, nan, sin, ...) with constant arguments are pedwarn-only
   initializers (cinit.c foldable_libcall): rejects-valid 53.  u/U/u8 string and char
   prefixes under -std=gnu99 now lexed (rejects-valid 45, exact 5524 of 6051).
   GNU forward parameter declarations
   (`f(int n; int a[n])`: NF_FWD/NF_SEMI on N_PARAM, CSF_FWD, "just a forward
   declaration"; cdecl.c fwd_params) done: rejects-valid 43, exact 5531.
   accepts-invalid triage (92 -> 86, exact 5531 -> 5561): "empty enum is
   invalid", -Werror=implicit (umbrellas with =; promoted warnings print
   [-Werror=X] like gcc), -Wdeclaration-after-statement (cstmt.c
   decl_after_stmt, gcc's last_stmt), "ISO C forbids label declarations",
   "integer constant is so large that it is unsigned" as a pedwarn (pp and
   C), "ISO C forbids an empty translation unit" (main.c; diag.c prints
   end-of-file locations as `file:N:` without column or snippet).  The
   remaining accepts-invalid are a long tail: ~35 attribute/builtin
   argument checks (attr-*, builtin-*, atomic-*), ~15 asm operand checks, ~10
   vector operations, "size of array element is not a multiple of its
   alignment" (pr36093, pr43783), block-scope `extern` of an incomplete array
   composite (redecl-7/14/18), "invalid use of void expression" in asm
   operands, jump into VM scope (pr108375-1), compound-literal pedwarns in
   static initializers (c99-const-expr-11/14, gnu99-const-expr-3/4), #pragma
   message wording.
   Folding (cinit.c const_varlike): `"str"[i]`, same-variable pointer difference
   `p - (p - 1)`, `?:` of constants; `__TIMESTAMP__` (file mtime); label-address
   pedwarn at the input location; initializer of an incomplete struct digested.
   Metrics: dg-options rejects-valid 39, accepts-invalid 86, exact 5563 of 6059
   ("str"[i] folds only for an in-range, non-overflowed index; 1["bar"] too);
   default gcc.dg 70/101, exact 8993 of 10067; corpus 0/0; tests 540.
   Constant-expression pedwarns (cinit.c const_class: integer K_FOLD with
   EF_INTOPS, chosen ?: arm, casts to VM types whose bound has a call/
   assignment/++/comma), "overflow in constant expression" as a pedwarn for
   enumerators/bit-fields/case labels, DR031 `2 || 1/0` is an ICE; asm
   statement checks (cexpr.c cexpr_asm: constraints, named operands,
   lvalue/read-only outputs, register `m` operands, void/incomplete
   operands; parse.c duplicate qualifiers).  dg-options: rejects-valid 39,
   accepts-invalid 57, exact 5857 of 6290; default gcc.dg 70/79, exact 9228;
   corpus 0/0; tests 553.  Overflow-in-conversion warnings for call arguments are reported at the argument's first token (at the macro use when that token is from a system header), as gcc does.  Built-in argument checks (cexpr.c builtin_args_ok, called from e_call for undeclared `__builtin_*` callees): counts for constant_p, va_start, alloca_with_align(_and_max), assume_aligned, fpclassify, is*, signbit, *_overflow(_p); kinds of the arguments (non-floating-point, non-const, integral/pointer-to-integral), alloca_with_align's alignment.  `deprecated`/`unavailable` uses (cexpr.c cdep_use; CSF_DEPRECATED/CSF_UNAVAILABLE + CSym.dep_msg) are diagnosed for variables, functions, typedefs, fields (Field.dep/dmsg) and struct/union/enum tags (Record/Enum.dep/dmsg; read straight from the tables so a name-only use is not a layout read) at gcc's input_location (line start; gcc also moves it to a `struct S` tag token it just parsed, which cereal does not model).  Block-scope extern declarations keep gcc's per-binding types (Bind.ty = c_binding.u.type; pushdecl/outer_bindings/bind_this_type in cdecl.c; e_ident reads cbind_type) and the 'completed incompatibly with implicit initialization' check (CSF_INNER_COMP).  Known gap: UCN operand names in asm are not compared.   Remaining rejects-valid (dg-options baseline): builtin-tgmath, `copysign`
   in array-parameter bounds, #include_next pedwarn from cereal's gcc-15
   header dirs under -pedantic-errors (environment, 5 files).
   Output order: parser errors of a unit print before its checker
   diagnostics, gcc interleaves them in emission order; not changed (no
   metric sees it, and a correct merge needs emission points in the checker).
   Not done: did-you-mean for macro names (42 gcc.dg cases; needs the
   macro table at the token's version in the checker, with read-set
   consequences for P3), "expected ... at end of input" location (gcc
   prints `N:` with no column at the line after the last), read-only
   location expression text (43), sizeof of a member whose type is a
   typedef (gcc drops the typedef name).
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
