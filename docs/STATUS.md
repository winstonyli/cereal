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
   accepts-invalid 102, tests 525.  Missing: gcc's "'gnu' attribute ignored".
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

- Attribute-argument checks (2026-10-01):
  * `[[ns::name]]` parses in gnu modes only (the lexer has no `::` token):
    `gnu::`/`__gnu__::` is the GNU attribute, other scopes warn
    "'ns::name' scoped attribute directive ignored".
  * assume_aligned_check: return type must be a pointer, arguments integer
    constants, positive, power of 2, second in [0, first-1]; on a non-function
    declaration "only applies to function types".
  * strict_flex_check: error on a non-field, non-integer / out-of-0..3 argument,
    non-array field; `[[...]]` after a type specifier warns "does not apply to
    types".  Golden attr_args (-std=gnu99).
  gcc.dg: 3556 of 3875 identical; tests 770, corpus 0/0, ASAN/UBSAN 0.

- Packed pointers, -Wvla text, zero-length string init (2026-10-01):
  * `-Waddress-of-packed-member` (cexpr.c packed_ptr_check, c-warn.cc
    warn_for_address_or_pointer_of_packed_member): run after every pointer
    conversion (cexpr_assign_check wrapper, casts, cinit's compatible-type
    path via cexpr_packed_check).  "taking address of packed member of 'S'":
    `&x.m`, `&p->a[i]`, array-typed `p->arr`, when the pointee is more aligned
    than the member (a member is DECL_PACKED only if its record is packed AND
    its type is aligned > 1; char members never warn).  "converting a packed
    'S' pointer (alignment 1) to a 'T' pointer (alignment N)": a variable,
    parameter or call result of pointer-to-packed type, with a "defined here"
    note on named non-empty records.  Offsetof-style folded addresses are
    skipped.  Location: the expression for the first form; for the second
    gcc uses input_location, modelled by cdecl_iloc(last_tok+1) (tag
    references are iloc events).  TypeTable.any_packed is set but does not
    help the speed (uvloop includes packed headers); check cost +1.4%
    (3.296G -> 3.341G on uvloop/loop.c), accepted.
  * -Wvla: gcc 13 says "ISO C90 forbids variable length array 'x'" in every
    mode (it was "is used").  Identifiers with extended characters print as
    \UXXXXXXXX (cident_ucn, C locale), UCN- or UTF-8-spelled alike.
  * "initialization of a flexible array member" also for a string initializing
    a zero-length array ([0], braced or not, compound literals too).
  Goldens packed_ptr, vla_flex_init.  gcc.dg: 3572 of 3875 identical; tests
  774, corpus 0/0.  Known gaps: designated `.b = ""` on a flexible member
  gives gcc a second warning at the tag (c99-flex-array-7/typedef-7); `u8""`
  literals in -std=c99; `__mode__(\u00e9)` unknown-mode error.

- Trailing-token recovery, -Wtraditional ints, __int128 literals (2026-10-02):
  * "expected ',' or ';'" after a complete init-declarator no longer silences
    the checker (Parser.soft_errors is subtracted in main.c's had_errors):
    gcc has already processed the declarator, so undeclared identifiers,
    zero-size-array pedwarns and "initializer element is not constant" still
    follow.  Other syntax errors still quiet the whole unit.  Order stays
    parser-errors-first (parked).
  * -Wtraditional: "this decimal constant would be unsigned in ISO C90" and
    "traditional C rejects the \"u\" suffix" (not in system headers).
  * Unsuffixed decimals above LLONG_MAX are __int128: overflow-in-conversion
    to long long is pedantic-only, to int always; 128-bit values print unsigned.
  Goldens trad_int, trail_decl.  gcc.dg 3572/3875 identical (unchanged),
  tests 778, corpus 0/0, ASAN/UBSAN 0.  Open: Wconversion-complex-c99,
  overflow-warn-5, pr35635, init-bad-4 recovery, -Wstringop-overread.

- Diagnostic order, bit-and conversions, init recovery (2026-10-02):
  * Order: in a unit with a syntax error, main.c merges the parser's
    diagnostics with the checker's by location (diag_merge_from; each side
    keeps its own order), since gcc checks as it parses.  Order-only
    differences vs gcc: 110 -> 32 files (bench: compare g and c lists of
    dg_out.json in order, not as multisets).  Still off: K&R cascades, macro
    notes, `expected '}'` that gcc prints before later checker diagnostics.
  * `x & K` converted to a narrower integer: K with no bit inside the
    target width -> -Woverflow "changes value from '(int)p & 512' to '0'"
    (operands printed with the promotion cast, constant last); K beyond the
    width otherwise (also `|`, `^`) -> -Wconversion "changes the value of
    'K'".  `?:` arms stop at the first arm that warns.  Golden bitand_ovf.
    Open: `p & 256 & 512` (gcc silent); bit-field `?:` (gcc warns on the
    whole conditional, -Woverflow, pr35635).
  * A missing '}' in an initializer skips to the matching '}' like gcc's
    c_parser_skip_until_found and is a soft error (checker still runs):
    init-bad-4 now has gcc's diagnostics.  Golden init_brace_err.
  * Perf: cident() tested every identifier for UCN spelling on each call
    (+2.3% check); the flag is now Ident.ext, set at interning.  uvloop
    check 3.357G (parse 1.702G).

- Ordering round 2, -Wtraditional-conversion (2026-10-02):
  * Parser pedwarns and errors are merged with the checker's in every unit
    that has any parser diagnostic (strictly-earlier checker diagnostics go
    first); diagnostics of K&R parameter checks ("type of 'x' defaults to
    'int'", "declaration for parameter ... but no such parameter") are marked
    Diagnostic.late and sort after the parser's.  Order-only differences vs
    gcc: 110 -> 10 files.
  * choist(c, node, n0): a keyword extension warning (`_Static_assert`,
    `_Alignof`) that gcc emits at the keyword is moved in front of the
    diagnostics of its operand (Checker.dm records the diagnostic count at
    each node's visit).
  * -Wtraditional-conversion (cexpr.c trad_conv, called from assign_check
    for CONV_ARG): integer/floating/complex kind changes, "with different
    width", "as signed/unsigned", and the untagged "as 'float' rather than
    'double'"; not for __builtin_ callees or system headers; a constant
    argument is located at its macro expansion point.  Golden trad_conv.
  Remaining order-only files: 20020104-1, asm-8, init-bad-4, pr105853,
  redecl-21, spellcheck-inttypes, struct-semi-4, vla-stexp-8 and two more.

- Recovery locations and ordering round 3 (2026-10-02):
  * After "expected '=', ',', ';' ..." gcc skips to the end of the block or
    statement (consuming a `}`) for a later declarator (n > 0) or an unknown
    type name (`Specs.err`); the first declarator of a known type just returns
    (and gets the nested-function pedwarn).  Fixes the missing "expected
    declaration or statement at end of input" (pr98198, pr54355).
  * That EOF error sits at input_location: the first token of the last line,
    or the last "expected expression" token on that line (`Parser.expr_err_tok`).
  * "conflicting types for 'f'; have 'void()'" (implicit decl vs definition)
    is at the name; its note shows the new type.
  * diag_merge_from flags: `Diagnostic.eof` (unclosed-body error goes after
    all checker diags), `tie` (parser "no semicolon at end of struct": checker
    diags at the same loc first), `early` (checker diag before a parser one at
    the same loc: label-declaration pedwarn), `cut` (call with a missing ')'
    (`NF_CUT`): its format warnings follow that syntax error), `late` now also
    for unused-variable (scope close) and "struct has no named members".
    Braced-group pedwarn is hoisted before the scope-end diags.
  Goldens eof_expr_loc, decl_skip_brace, unk_type_skip, implicit_void_def,
  stexp_unused_ord, struct_semi_ord, call_cut_fmt.  gcc.dg: 3583+ of 3875
  identical; order-only differences 110 -> 3 (binary-constants-1, init-bad-4,
  one more); tests 798, corpus 0/0, ASAN/UBSAN 0; check 3.373G.  Known gap:
  the "'X' is defined in header" note of spellcheck-inttypes.

- Ordering unification (2026-10-02):
  * The five bool flags (late/early/cut on Diagnostic+DiagEngine, tie/eof on
    Diagnostic) are one `DiagOrd ord` (diag.h: ORD_NORMAL/LATE/CUT/EARLY/TIE/
    EOF); diag_merge_from asks `checker_first()`.  Set with
    `diag_ord(diag, ORD_X)` (returns the previous one; restore it) or
    `diag_mark_last(diag, ORD_X)` for a parser diagnostic just reported.
    Adding a new ordering quirk = a new enum value and one line there.
  * Parser: `nerrs` duplicated `errors` (removed); the include-chain dance of
    vperr/pwarn/c++-compat is `pvreport`.
  * Braced-group pedwarn is hoisted (choist_at) before the "statement with no
    effect" warnings of its body, else before the scope-end diagnostics.
  Still order-only: binary-constants-1, init-bad-4.  Not done: __builtin_trap /
  unreachable / prefetch / __clear_cache signatures for
  -Wbuiltin-declaration-mismatch (cbuiltin_tab.h only lists library built-ins
  reachable undeclared; needs a __builtin_-only flag).

- __builtin_-only builtins, error cascades (2026-10-02):
  * cbuiltin_tab.h: `gnu = 2` entries exist only as __builtin_NAME (112, from
    bench/tools/gen_builtin_pfx.py, restricted to types bt_type parses) plus
    `__clear_cache`; bt_find skips them for a plain spelling; they are not
    typed as functions by the identifier path and do not count as having a
    library fallback.  -Wbuiltin-declaration-mismatch now fires for
    `__builtin_trap (int)`, `__builtin_prefetch`, `__clear_cache`.  Open: a
    declarator with an error in its parameter list (`__builtin_exit (int,
    int[+])`) still gets no warning.
  * parser->error model: "expected expression" is silent while err_live
    (gcc's c_parser_error); err_live is cleared after every block item;
    for-statement `;` failures skip_until(';') like c_parser_skip_until_found.
  * A call whose argument list had a syntax error (NF_CUT) is checked after
    that error (ORD_CUT around call_args, not only format checks).
  Goldens builtin_pfx_decl, expr_err_cascade.  gcc.dg 3587 of 3875 identical;
  tests 802; ASAN/UBSAN 0.
  (Fixed 2026-10-02, see below.)  Old lead: gcc.dg/pr59992.c (one function of ~100k `if (p[n]) { foo##n (..);
  return; }`) takes ~70-85 s of cereal CPU in -fsyntax-only (also at a7b156c);
  bench/tools/par.py's 30 s timeout flags it when the machine is busy.  Likely
  something super-linear in the checker/parser per function body.

- pr59992 perf, parser error state (2026-10-02):
  * cbol_tok's memo now covers [BOL token, queried token], so backward queries
    on a long macro-expanded line no longer rescan to the line start
    (callgrind: 94% of a 10k-statement input).  pr59992.c 70-85 s -> 0.9 s;
    dg timeouts=0, so par.py needs no long-timeout copy.  check icount +0.1%.
  * Parser error state is one struct `p->err` {have, live, eof_stmt, last,
    expr_tok}; `errors`/`soft_errors` stay as cross-module counters (main.c).
  * `expect()` is silent while err.live (c_parser_error).  `expect_skip` =
    gcc's c_parser_skip_until_found for `]` in array declarators, designators
    and subscripts (not attributes: they have their own skip).  The K&R
    declaration-list loop rejects a leading __attribute__ (start_attr_ok
    false) -> "expected declaration specifiers before '__attribute__'".
    Trying expect_skip for `)` in expressions gained nothing and broke 4
    goldens; reverted.
  Goldens bracket_skip, err_live_expect, kr_attr_start.  gcc.dg 3590 of 3875
  identical; tests 808; ASAN/UBSAN 0.  Open: `int a[2, 3];` still declares `a`
  (gcc drops it: later "'a' undeclared" missing); `z[1, 2;` orders the
  subscript diagnostic before the syntax error; nofixed-point-1 needs
  gnu-mode `_Fract/_Accum/_Sat` keywords ("fixed-point types not supported for
  this target" + pedwarns); for-1's line numbers after `# 0` linemarkers.

- Round 4 (2026-10-02, after f202aa4):
  * A failed `]` in an array declarator sets DeclInfo.failed (gcc drops the
    declarator; later uses give "'a' undeclared").  bracket_skip golden covers it.
  * gnu-mode `_Fract/_Accum` ("fixed-point types not supported for this
    target" + pedwarn) and `_Sat` (pedwarn), typed as int; golden fixed_point_gnu.
  * diag_vreport: a pedwarn in a system header is dropped even when
    -pedantic-errors made it an error (gcc's own stdint.h `#include_next`);
    dg rejects-valid 5 -> 0.  No golden (needs a real system header; the dg
    run is the check).
  gcc.dg 3597 of 3875 identical (missing 942, extra 397); tests 810.
  Long tail: no diagnostic cluster spans more than ~8 files (largest missing:
  `conflicting types ... have` 4 files / 23 diags; `-Wstringop-overread` 86
  diags in 3 files).

