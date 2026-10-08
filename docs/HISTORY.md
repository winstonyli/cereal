# Development log (newest rounds last)

The running log of the work, round by round: what was probed against gcc-13,
what was fixed and what stayed open.  The current summary is in
[STATUS.md](STATUS.md).

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
  return; }`) takes ~70-85 s of cereal CPU in -fsyntax-only (also at 46760f0);
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

- Round 4 (2026-10-02, after ee26f53):
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

- Round 5 (2026-10-02, after 8ad0073):
  * N_INDEX gets NF_CUT when `]` is missing: the subscript check is ordered
    after the syntax error (ORD_CUT), as for calls.  Golden index_cut.
  * volatile-return redeclarations (`void f(void); volatile void f() {}`):
    volatile_ret_only now goes through compat_gcc (prototype vs K&R), and a
    block-scope redeclaration warns twice, not three times.
  * An incomplete enum is compatible with no integer type
    (type_compatible): `extern enum E e; unsigned e;` conflicts.
  * type_print: a typedef of a pointer/array/function type nested inside a
    declarator is spelled out (gcc: 'int (*[])[10]', not 'IA10P[]');
    top-level keeps the name + aka.  Golden aka_derived, qual_enum_redecl.
  gcc.dg 3602 of 3875 identical (missing 906, extra 383); tests 816.
  Not feasible front-end-only: -Wstringop-overread in warn-strlen-no-nul.c,
  Wstringop-overflow-22, -overread-6 need -O2 constant propagation (gcc folds
  `strlen (a)` after propagating loop-free locals); skip.

- Round 6 (2026-10-02, after 7938799):
  * Struct member `const;` (no type, no declarator) is "ISO C forbids member
    declarations with no members": the check keys on Spec.default_int
    (finish_declspecs turns word NONE into INT, so the old test never fired).
  * `typedef float t;` over `extern int t;`: gcc keeps the variable bound, so
    a later `t v` is typed int.  checker: the failed typedef returns the old
    symbol; N_TYPEDEF_NAME resolving to an object uses its type.
  * N_DECLARED runs unquiet (ORD_EARLY) in a unit with syntax errors unless
    an N_INIT_DECL follows directly (a failed attribute list): gcc declares
    `a = <error>` before parsing the initializer, so redeclaration conflicts
    still show.  Goldens decl_before_init_err, typedef_over_var.
  gcc.dg 3607 of 3875 identical (missing 901, extra 380); tests 820.
  Remaining clusters are <= 3 files each (incompatible-pointer arg notes,
  -Wlarger-than, -Wdangling-else, shadow=compatible-local); `chk.h` not found
  is the missing gcc.c-torture dir of the sparse checkout (harness artifact).

- Round 7 (2026-10-02, after 59f196b):
  * type_composite: of a transparent union parameter and a member type the
    member type is kept, whichever came first (gcc) -> `f2 (&l)` warns after
    `f2 (U2); f2 (int *)`.  expr_loc: `__extension__ e` has e's location
    (call-argument warning column).  Goldens transparent_redecl.
    Open: gcc's "function types not truly compatible in ISO C" pedwarn (2x per
    such redeclaration under -pedantic) is not emitted.
  * Options: -Wshadow=local / =compatible-local / =global (alias of -Wshadow);
    -Wshadow enables local enables compatible-local; the param/local shadow
    warning picks its tag by type compatibility (shadow_id).  -Wparentheses
    enables -Wdangling-else.  Goldens shadow_levels, dangling_paren.
  * diag.c's id cache (idc_*) grew 128 -> 512 slots: adding two option names
    moved rodata and two hot ids collided (+1.2% check instructions); back to
    3.376G.
  gcc.dg 3614 of 3875 identical (missing 867, extra 363); tests 826.

- Round 8 (2026-10-02): -pedantic "function types not truly compatible in ISO C"
  (2x, [-Wpedantic]) when a compatible redeclaration pairs a transparent-union
  parameter with a non-union one (type_tu_mixed; gcc matches at file and block
  scope).  Goldens transparent_pedantic; transparent_redecl regenerated.
  gcc.dg 3615 of 3875 identical; tests 828.

- Round 8 (2026-10-02): -pedantic "function types not truly compatible in ISO C"
  (2x, [-Wpedantic]) when a compatible redeclaration pairs a transparent-union
  parameter with a non-union one (type_tu_mixed; gcc matches at file and block
  scope).  Goldens transparent_pedantic; transparent_redecl regenerated.
  gcc.dg 3615 of 3875 identical; tests 828.
  Builtins declared without a prototype: the too-few/too-many-arguments
  warnings carry a "declared here" note, and the argument-conversion notes
  point at the declaration (gcc), golden builtin_knr_notes.  Not done (one
  file, Wbuiltin-declaration-mismatch-3.c): -Wincompatible-pointer-types for a
  K&R-declared builtin used as a function pointer ("pointer to
  '__builtin_memset' with incompatible type", incl. gcc's double-space
  "int,  long" spelling) in init/assign/argument/conditional/return.
- Profile (callgrind, uvloop loop.c, -fsyntax-only, 2026-10-02): flat; top
  self cost pp_read_raw 4.9%, checker_unit 3.6%, parse ct 2.9%, then
  compute_lines 2.9%.  compute_lines now memchr's for newlines when the file
  has no CR: check 3.377G -> 3.289G (-2.6%), parse 1.703G -> 1.615G.  Left:
  strcmp is 2% spread over many callers (bt_find 0.6%, find_index_cached
  0.4%); no single hotspot remains.
- Round 9 (2026-10-02): -Wsign-compare false positives: nonneg() (gcc's
  tree_expr_nonnegative_p) now knows `*` of nonnegatives (signed overflow is
  undefined), `+` of two zero-extended operands narrower than the result,
  comma (right operand), simple assignment (right operand), and a statement
  expression's value.  Golden sign_compare_nonneg.  Known extra: `b + b + b`
  (uchar b) warns here, gcc folds it to b * 3 first.  gcc.dg 3620 of 3875
  identical; tests 832.
- Round 10 (2026-10-02): widened to c-c++-common (CEREAL_DGDIR=c-c++-common
  for bench/tools/par.py; gcc-13, CEREAL_DGOPTS=1): n=657, rejects-valid 9,
  accepts-invalid 19 (was 12/28).  Fixed: malloc(dealloc, pos) checks,
  statement-expression value behind a label, pedantic C99 Annex D check of
  UCNs in identifiers (src/ucn99.h from bench/tools/gen_ucn99.py).
  pp_read_raw (top self cost) is the bulk newline/char scan already; no
  further bulk-scan saving found.  Open: raw UTF-8 identifier validation +
  "stray in program", -Wbidi-chars, __builtin_has_attribute argument
  comparison, builtin_location constants, pr109884 (__float128), and the
  accepts-invalid list (attr-nocf-check-3, attr-simd-5, pr100785, pr20318,
  pr58346-2/3, pr68657-2/3, ...).  Tests 836; gcc.dg 3620/3875 identical.
- Round 11 (2026-10-02): __builtin_has_attribute compares attribute
  arguments (AName.arg: canonical ints/strings/idents; nonnull() covers every
  parameter, nonnull(N,..) is a list) and sees typedef-declared attributes
  (Checker.tdas: typedef type -> attribute set; a typedef keeps only its
  largest aligned; an array without its own aligned takes its element type\x27s,
  like gcc\x27s user-align bit).  c-c++-common rejects-valid 9 -> 5
  (builtin-has-attribute-2..7).  Golden has_attr_args.  Left: attr-copy
  (copy of packed bit-field offsetof), builtin_location, pr109884.
- Round 12 (2026-10-02): c-c++-common rejects-valid 0 (n=657: 468 identical,
  19 accepts-invalid); gcc.dg unchanged (3620/3875 identical, 0 rejects-valid).
  vector_size is a type property in __builtin_has_attribute; copy(packed)
  packs a field; __float128 base type and the six *q builtins (gnu=3);
  __builtin_FILE/FUNCTION are K_ADDR const char *, __builtin_LINE is an ICE
  (presumed_line scans back for #line); __builtin_constant_p is always an ICE
  (gcc -O0); pointer difference of equal address constants folds (same_addr_const).
  Goldens builtin_location, has_attr_args.  Tests 840; ASan+UBSan clean
  (840/0; leaks at exit are not tracked).  Open: step 2 cheap accepts-invalid
  list; -Wbidi-chars and UTF-8 "stray in program" (do at intern time).
- Round 13 (2026-10-02): c-c++-common accepts-invalid 19 -> 6 (identical
  468 -> 479; gcc.dg unchanged: 0 rejects-valid, 0 accepts-invalid).  New
  checks: pointer difference of an empty aggregate; returns_nonnull on a
  non-pointer function; address of a bit-field as an asm "m" operand;
  __builtin_clear_padding / __builtin_speculation_safe_value /
  __atomic_is_lock_free / __atomic_always_lock_free argument checks;
  scalar<->vector casts (size mismatch, non-integer scalars); vector_size on
  _Bool; __builtin_shufflevector (new e_shufflevector: vectors, element type,
  power-of-two count, constant indices < sum of both lengths; VEC_CONVERT(x)
  expression text); simd and zero_call_used_regs attribute arguments (arity
  via gnu_attr_argc, errors at the declared name via attrs_zcur_check).
  Goldens vec_cast, shufflevector, attr_args2, builtin_args2.  Tests 848;
  ASan+UBSan clean.  Left in c-c++-common: attr-nocf-check-3 (needs
  -fcf-protection), pr68657-2/3 (-Wlarger-than=N incl. pragma diagnostic),
  pr68833-2 (-Wmissing-format-attribute on __builtin_vprintf), pr79428-3
  (#pragma GCC pch_preprocess), unroll-5 (#pragma GCC unroll argument needs
  the macro-expanded expression; the pragma token is raw).  Not done:
  gcc.dg bitfld-12 (offsetof bit-field message: "attempt to take address of
  bit-field structure member", at the tag name), -Wbidi-chars and UTF-8
  "stray in program" (step 3).
- Round 14 (2026-10-02): -Wbidi-chars and UTF-8 "stray in program".
  gcc.dg identical 3618 -> 3620 (0 rejects-valid, 0 accepts-invalid);
  c-c++-common identical 479 -> 496 (accepts-invalid still 6).  Tests 864;
  ASan+UBSan clean (also bidi_fuzz); check icount 3.294G -> 3.303G.
  -Wbidi-chars[=none|unpaired|any|ucn,...] ports libcpp's bidi::vec (global
  stack of {pdf?, ucn?}; LRE/RLE/LRO/RLO push PDF contexts, LRI/RLI/FSI PDI
  contexts; LTR/RTL flagged only; closed at string/char end, each block-comment
  line, line comments containing 0xE2, identifiers containing UTF-8/UCN).
  Default unpaired; bare option = any; UCNs checked only with ucn.  The column
  of a diagnostic is gcc's display column (wcwidth tables, src/wcwidth.h).
  UTF-8 identifiers are validated against the C99 table under -pedantic and
  the C99|C++|C11 union otherwise (src/ucnx.h); an invalid character is a
  TK_OTHER token that the parser reports as "stray '\ooo' in program" /
  "stray 'c'" / "missing terminating c character" and skips; # and ## left in
  program text are stray too (gcc spells "##").  lexer_set_diag(L, d) sets
  hi8/bidi_live for lexer_init and the plan-segment lexers (the --cells path
  lexed UTF-8 on the fast path before); phase A clears bidi_live while diag
  is NULL.  Fixed a Round 13 regression: the argument-2 pointer check applied
  to every non-generic __atomic_* (test_and_set/clear take a memory order);
  now only *_lock_free (golden atomic_order).
  Goldens bidi_chars, bidi_any, bidi_ucn, bidi_any_ucn, bidi_off,
  stray_utf8, stray_ascii (each header-for-header equal to gcc-13).
  Tools: bench/tools/bidi_fuzz.py (differential: strings, char constants,
  comments, identifiers x 9 modes x pedantic; 0 diffs on 8 seeds),
  bidi_probe*.py, gen_wcwidth.py, gen_ucnx.py.  par.py now caches gcc's
  result (~/.cache/cereal-par, CEREAL_PARCACHE=0 off; clear it if ~/gccts
  headers change), CEREAL_JOBS (12), cereal timeout 10 s.
  Known gaps: the bidi stack does not carry across parallel chunks (a stray
  control left open is closed in a later chunk in gcc only); no bidi in
  skipped #if groups; \u{...} in identifiers and gcc's "delimited escape
  sequences" pedwarn; $ identifiers and numbers do not close contexts;
  UTF-8 and UCN spellings of one identifier are not unified; diagnostics are
  location-sorted, so lexer and parser messages can order differently from
  gcc.  (Parser cascade, larger-than, missing-format-attribute and bitfld-12 were
  open here; all done in Round 15.)
- Round 15 (2026-10-02): -Wlarger-than=N, -Wsuggest-attribute=format, the
  offsetof bit-field message, and gcc's parser->error cascade.  gcc.dg
  identical 3620 -> 3624 (0 rejects-valid, 0 accepts-invalid);
  c-c++-common 496 -> 501, accepts-invalid 6 -> 1.  Tests 880; ASan+UBSan
  clean; check icount 5.960G -> 5.978G (+0.3%, uvloop loop.c with
  -std=c99 -pedantic -I/usr/include/python3.13 -I.../libuv/include).
  -Wlarger-than=N[kB|KiB|MB..] warns at each object declaration (extern,
  tentative, locals; a parameter at the prototype and again at the
  definition); bad value = driver error; bare -Wlarger-than is unrecognized.
  -Wsuggest-attribute=format (alias -Wmissing-format-attribute): a v*printf
  /v*scanf call (builtin tables gained the v variants) whose format is not
  a literal, inside a function with a char * parameter and no format
  attribute of that kind; reported at gcc's input_location.  That location
  is the first token of the line of the call's closing ')' (last_tok + 1),
  which also fixes -Wformat-security / -Wformat-nonliteral on multi-line
  calls.  -Wformat=2 now enables -Wformat-nonliteral and -Wformat-security;
  -Wno-format disables the default-on -Wformat-security.
  offsetof of a bit-field: "attempt to take address of bit-field structure
  member" at the tag (or '{') of a struct/union/enum specifier in the type
  name, else at the first token of the line of the ',' after the type name;
  a later token that starts a line overrides the tag (input_location).
  Parser cascade: gcc's c_parser_declaration_or_fndef skips a declaration
  silently when parser->error is still set after its declaration specifiers,
  and file scope never clears the flag, so consecutive broken declarations
  report every other one.  declaration() does the same (p->err.live is no
  longer reset per external declaration); a parenthesised expression clears
  it only when skip_until_found really skips; unknown type name does not set
  it.  Goldens: larger_than, larger_than_pragma, suggest_format,
  offsetof_bitfield, err_cascade; parse goldens err_gimple, err_recovery,
  err_tokdesc regenerated (err_gimple/err_tokdesc now header-equal to gcc).
  Skip sites now follow c_parser_skip_until_found: calls, casts,
  sizeof (type), parenthesised expressions and brace initialisers skip with
  skip_until (no stop at ';', '{' or '}'; an unclosed bracket swallows the
  rest of the file silently, and EOF leaves the flag set); the attribute
  argument skip clears it; expected() is silent while the flag is set.
  Golden err_skip.  #pragma GCC unroll N: a literal/arithmetic argument that
  is a float or outside 0..65534 is an error at the argument (unroll_arg_bad
  in parse.c; names, sizeof and casts are left valid, so unroll-5's
  "#pragma GCC unroll j" is still missed).  #pragma GCC pch_preprocess after
  the first token is an error.  Goldens unroll_arg, pch_pragma.
  c-c++-common accepts-invalid 3 -> 1 (this round; only attr-nocf-check-3 remains: it
  needs the nocf_check type attribute, default-on in Ubuntu's gcc).
  Known gaps: the bidi stack is still per chunk with --cells/parallel
  (a stray control left open in code, e.g. U+202E outside strings, is not
  closed by a later string in another chunk: gcc warns, cereal does not;
  fixing it needs deferred closer events and a serial prefix-stack pass).
  The parser cascade flag is not affected (the parser is sequential).
  Other gaps: "int e = ) 2;" gcc says "expected ',' or ';' before numeric
  constant" (cereal: expected expression); a call with a syntax error in
  its arguments is not built, so gcc's follow-up semantic errors on it
  (initializer element is not constant, undeclared names) are missing;
  UTF-8 identifier spelling in "undeclared" messages (gnu mode prints the
  character, cereal \U000020ac).
- Round 16 (2026-10-02): -Wpacked (struct level: "packed attribute is
  unnecessary for X", re-lays the record out without packed via
  type_packed_unnecessary and compares every offset and the size; located
  by gcc input_location rule, see packed_unnecessary in cdecl.c) and the
  field level ("packed attribute is unnecessary for NAME" [-Wattributes] at
  the field name; not for unions, aligned fields or 1-aligned types).  Asm
  named-operand error now at cinput_loc.  gcc.dg identical stays 3624 (those
  tests lack -Wpacked); 0 rejects-valid, 0 accepts-invalid; c-c++-common
  accepts-invalid 1 (attr-nocf-check-3).  Tests 888; ASan+UBSan clean;
  icount 5.978G (unchanged).  Goldens packed_struct, packed_struct2,
  packed_field, asm_named_loc.  Remaining gcc.dg gaps are a long tail:
  stringop-overread, parentheses, c++-compat, attributes, pedantic.
- Round 17 (2026-10-02): c-c++-common 501 -> 512 -> 518 identical (gcc.dg
  unchanged at 3624; 0 rejects-valid, 0 accepts-invalid there).  Tests 900;
  ASan+UBSan clean; icount 5.988G (+0.16%).
  -Wmultistatement-macros (in -Wall): after an if/else/while/for/switch
  body (labels skipped, not a compound or a following ;), warn at the
  bodys
- Round 17 (2026-10-02): c-c++-common 501 -> 518 identical (gcc.dg unchanged
  at 3624; 0 rejects-valid, 0 accepts-invalid there).  Tests 900; ASan+UBSan
  clean; icount 5.988G (+0.16%).
  -Wmultistatement-macros (in -Wall): after an if/else/while/for/switch body
  (labels skipped; not a compound statement, not a following ';'), warn at the
  body's first token when it and the next token are from one macro expansion
  and the guard is not (cstmt.c multistatement).  gcc compares macro maps;
  cereal has no per-token macro id, so tok_macro() derives the defining
  #define from the spelled location (argument tokens are wildcards) and
  detects separate expansions of one macro by spelled locations going back.
  Known gap: the note chain for nested macros ("in expansion of macro" per
  level) is not produced, only the outermost level.
  -Wshift-overflow=2 (the 1 << 31 sign-bit case), and negative left operands
  ((-32768) << 17 warns at level 1: the sign-bit exemption is for
  non-negative operands only).  -Waddress-of-packed-member: comma operands
  (last operand), *&x and &*p folding with gcc's locations (pk_loc),
  __real__/__imag__, vector subscripts.
  Left: const variables folded under -O (gcc substitutes a const int with a
  constant initializer in c_fully_fold: Wshift-overflow-1/3/4 lines 59-61),
  -Wduplicated-branches (needs operand_equal_p with macro-location rules).
  Also in Round 17: "size 'N' of array 'x' exceeds maximum object size" now
  carries N when the byte size is representable in 64 bits (gcc omits it
  otherwise, e.g. a multi-dimensional overflow); aligned() errors: values >=
  2^63 print unsigned, "exceeds object file maximum" only when the attribute
  lands on a static-storage variable or function (attr_on_object), plain
  "exceeds maximum" on types/fields/params/auto variables, and the error is
  reported once per argument node (a struct specifier collected its
  attributes twice).  gcc.dg identical 3624 -> 3628, c-c++-common 520;
  tests 904; icount 5.988G.
  Deferred: the nocf_check function-type attribute (default under gcc-13
  -fcf-protection; needs attribute-carrying function types, compatibility,
  and type printing such as 'void (__attribute__((nocf_check)) *)(void)');
  affects 4 c-c++-common files (attr-nocf-check-1/2/3, pointer-to-fn1).
- Round 18 (2026-10-02): gcc.dg identical 3628 -> 3634, c-c++-common 521;
  tests 908.  Flexible arrays: assigning to one is "invalid use of flexible
  array member", an incomplete array object on the LHS is "'x' has an
  incomplete type"; a union containing a flexible-array struct is itself
  flagged RF_FLEXIBLE (type.c).  "called object is not a function" for a
  compound-literal or statement-expression callee sits at its opening brace
  (callee_err_loc).  Goldens: flex_assign, callee_loc.
- Round 19 (2026-10-02): -Wduplicated-branches (not in -Wall).  c-c++-common
  521 -> 529 identical, gcc.dg 3634; tests 912; ASan/UBSan clean; icount
  5.987G.  cstmt.c dup_expr is gcc's operand_equal_p(OEP_LEXICOGRAPHIC) on
  checker nodes: same shape/declarations, folded equal constants, same cast
  types, commutative + * & | ^ == != on arithmetic operands, and for nodes
  with a location the same defining macro (tok_macro).  Statements compared:
  expression statements, return, break/continue, nested if; blocks flattened,
  empty statements dropped, empty branches never warn; declarations, loops,
  switch, labels, compound literals are never equal.  A statement expression
  is equal only as a single statement.  `if`: reported at the token after
  `if` from cdecl_func_end (cstmt_dup_branches, pre-order by token), as gcc
  does at genericize.  `?:`: reported at the colon.  Arms without side effects
  are compared at parse time in e_cond (cstmt_cond_identical, immediate):
  arms already converted to the result type (equal constants and null values
  match; no nested ?: or statement expressions; a constant condition with
  constant arms is folded away; a __builtin_constant_p condition is skipped);
  arms with side effects are compared with the ifs.  File-scope ?: never.
  Known gaps: const variables folded under -O (Wduplicated-branches-1 line
  15), pointer arithmetic folding (p + 1 - 1 vs p), a few macro-expanded
  typeof null-pointer cases (c90-const-expr-7, c90-intconst-1), malloc(n1) vs
  malloc(n2) with equal arguments (Wstringop-overflow-59 line 20).
  Also: "called object ... is not a function" on a compound literal or
  statement expression callee sits at its opening brace.
- Round 20 (2026-10-02): "in expansion of macro" note chains.  Any checker
  diagnostic located in a macro replacement list now gets a note per macro it
  expanded through (check.c macro_notes, called from vrep and cnote), innermost
  first.  The chain is recovered from definitions (pp.c pp_macro_chain: the
  macro whose #define contains the spelled location, the outer macro named at
  the expansion point, nested names searched through mt_hist); the token is the
  one with that spelled location nearest the node (a window of 256 tokens
  first, then the whole unit).  Argument-origin tokens get no note (gcc prints
  none).  A note inside a macro uses the same invocation (cnote), except a note
  at the diagnostic's own location (previous declaration), which names an
  earlier use of the macro.  Tests 914; ASan/UBSan clean; gcc.dg 3634 and
  c-c++-common 529 identical (the harness ignores notes; the note-aware scan
  /tmp/notescan.sh: 172 -> 135 files differ, 96 of them over missing semantic
  warnings); icount 5.988G.  Golden: macro_chain.
  Known gaps: a previous-declaration note inside a macro prints no chain when
  the earlier use is not among the unit's tokens (gcc prints one; would need an
  expansion location on CSym); parse errors (vperr) still print a single
  outermost note; pasted names and argument tokens fall back to a single
  outermost note.
- Round 21 (2026-10-02): -O folding of const variables, vector shift counts.
  Under -O (any level but -O0; CheckOptions.opt_level) a read of a const,
  non-volatile integer variable with a constant initializer folds to that value
  inside a function, as gcc's decl_constant_value does in c_fully_fold: the
  initializer value is kept in CSym.val (flag CSF_CONST_VAL, cinit.c
  cinit_decl_done) and cexpr.c fold_const_var turns the identifier node into a
  K_FOLD/EF_CST constant when its parent uses the value (binary, ?:, cast,
  initializer, unary + - ~ !, right side of an assignment; never &, ++, --, the
  left side).  Fixes Wshift-overflow-1/3/4 and Wduplicated-branches-1 under -O.
  Vector shifts by a constant scalar count now warn "shift count is negative" /
  ">= width of vector element" (gcc.dg/vshift-7 under -O1).  Harness: par.py
  drops dg-options -O* unless CEREAL_DGO=1 (gcc then warns more than cereal
  does at -O: 3 gcc.dg + 1 c-c++-common files differ for that reason only:
  Wfree-nonheap-object-2, alias-9, Wstrict-aliasing-*).  Without -O: gcc.dg
  3635, c-c++-common 529 identical; tests 918; ASan/UBSan clean; icount 5.989G.  Goldens:
  const_fold_O, vec_shift_count.
  Known gaps: floats and enums-as-pointers are not folded; call arguments and
  return values keep the variable; a file-scope const is folded from the
  persistent symbol only if its val survives (cells path untested for -O).
- Round 22 (2026-10-02): attribute-list recovery (gcc.dg/pr67964) and macro
  chains on parse errors.  parse.c attribute(): after an attribute (name and
  optional arguments) gcc takes one more name, with its arguments, silently;
  then the list must end.  If ')' follows, the first ')' ends the attribute
  silently; otherwise "expected ')' before X" is reported at X.  Either way the
  parser skips past the first ')' (skip_past_rparen) and leaves the second to
  the declaration, which reports "expected ',' or ';' before ')' token"
  (probed over 12 shapes against gcc-13: /tmp/lt/at1..12.c).  vperr now emits
  the same per-macro note chain as the checker (Parser.macro_chain, set in
  main.c).  gcc.dg 3636, c-c++-common 529 identical; tests 922; ASan/UBSan
  clean.  Goldens: attr_list_recover, parse_macro_chain.
  Known gaps: the order of a parse error against a semantic error in the same
  function (gcc prints the parse error first in a function body); the
  previous-declaration note chain (see Round 20).
- Round 23 (2026-10-02): diagnostic order inside macro expansions; -O file-scope
  initializers.  Diagnostic.oloc is where a diagnostic merges with the
  parser's (diag_merge_from): its location, or for a replacement-list token the
  macro's invocation (the spelled location repeats in every use of a macro, so
  two uses of one macro in a function merged in the wrong order).  Within one
  invocation the order falls back to the spelled location.  Set in
  check.c macro_notes and parse.c vperr.  -O const folding also applies in a
  file-scope initializer (const int h = g + 1), as gcc's in_init, but not in
  an array bound or declarator.  Verified the cells path gives the same -O
  warnings.  Tests 924; ASan/UBSan clean; gcc.dg 3636, c-c++-common 529 identical (-O runs:
  3633 / 528, the rest gcc-only middle-end warnings).  Goldens:
  parse_sema_order, const_fold_O (file-scope cases).
  -O-only gcc warnings still missing: -Wstrict-aliasing (c-family
  strict_aliasing_warning, needs alias sets: gcc.dg/alias-9,
  Wstrict-aliasing-*; -Wall -O2 enables it, so it matters on real code),
  -Wfree-nonheap-object (middle end, not modelled).
- Round 24 (2026-10-02): -Wstrict-aliasing (levels 2 and 3).  alias.c models
  gcc alias sets as a canonical key per type (signedness, qualifiers,
  typedefs, arrays and enums folded; char types are set 0; every pointer chain
  ending in void is void *) plus a walk that asks whether one type holds the
  other.  type_alias_rel gives AL_SAME / AL_MAY / AL_DISJOINT, probed against
  gcc-13 over two 25-type matrices (/tmp/lt/mx.c, my.c): a struct with a char
  member conflicts with everything only as the *object*.  cexpr.c alias_base
  looks through parentheses and pointer casts for &decl, &a.b, &a[i], &p->m,
  __real/__imag or a decaying array (not &*p, &p[i]).  Level 3 (-Wall) warns
  at a dereference: *p, p->m, p[0] (not p[1]), at the cast (the [ for a
  subscript); level 2 warns at the cast, at its operand, with "might break"
  when the sets conflict, and the incomplete-target message.  Active at -O2,
  -O3, -Os, -Oz, -Ofast or -fstrict-aliasing; -fno-strict-aliasing wins.
  may_alias is kept on typedefs (TF_MAYALIAS) and records (RF_MAYALIAS).
  Tests 929; gcc.dg 3636 identical with -O (was 3633), c-c++-common 529.
  Goldens: strict_aliasing, strict_aliasing_2.  Gaps: level 1; constant-folded
  conditional operands (*(long *)(1 ? &x : &x)); a doubly parenthesised
  operand column at level 2; gcc underlines the whole cast operand.
- Round 25 (2026-10-02): -Waddress parity.  A weak declaration or weakref may
  be null, a weak *definition* cannot (CSF_WEAKREF, weak_maybe_null).  A
  pointer/array/function converted to _Bool (argument, assignment, return,
  initializer, cast) warns at gcc's input_location (cinput_loc).  Pointer sums
  print as gcc's folded tree: p + (sizetype)((long unsigned int)i * 4),
  (sizetype)(z * 4) for size_t, pc + (sizetype)i for char, -(sizetype)... for
  subtraction, ((sizetype)i + 1) * 4 when a constant is added, (int *)&arr and
  (int *)pa for array operands, pa + 8 for pa[1]; &p[0] and p + 0 fold away.
  addr_target names '*pa' for &pa[0][i] / &(*pa)[0] and __builtin_* functions.
  The macro test uses the comparison token (F (p) is silent, G (*p, i) != 0 and
  a macro-named function are not).  Tests 931; c-c++-common 533 (was 529),
  gcc.dg 3636.  Golden: address_ptr_plus.  Gaps: &&label truth values,
  &__real__/__imag__ x names, brace-initializer location.
- Round 26 (2026-10-02): -Wlogical-op (not in -Wall/-Wextra), cexpr.c
  logical_op_warn, called from e_logical before the -Waddress truth checks,
  located at the operator.  Three warnings, probed against gcc-13 (/tmp/lt/lo1..4.c):
  (1) "applied to non-boolean constant": right operand an integer constant
  other than 0/1 (enumerators, 1+1, sizeof count; a comma expression does
  not), left not constant and not written as a comparison, !, && or || (a
  parenthesised comparison counts as plain).  (2) "of equal expressions":
  both operands reduce to the same truth comparison (lg_form: x -> x != 0,
  !x -> x == 0, ! folded into the comparison, == 0 / != 0 / != NULL
  normalised, swapped comparisons equal, widening and same-width casts of a
  plain operand dropped) and opeq says the expressions match; the left operand
  is already a truth value but the right one must be a comparison, ! or
  integral (so p && p and fl && fl are silent, p && p != 0 is not).
  (3) "mutually exclusive tests is always false" / "collectively exhaustive
  tests is always true": both tests compare one expression with constants;
  interval sets in the expression type (iv_*) are intersected (&&) or
  complemented and intersected (||); a side that is trivially false/true
  alone is skipped.  Operators from macros are skipped, constants and
  names are not (gcc has no location for them).  Corpus scan (643 files):
  4 of 4 gcc warnings, no extras.  Tests 933; gcc.dg 3638, c-c++-common 536.
  Golden: logical_op.  Gaps: const/pure function calls as equal operands
  (pure (x) && pure (x)); a cast between signedness inside a range test
  ((unsigned) x < 3 && x > 5); -Wlogical-op is silent on &&/|| operands
  that are themselves && / ||.
- Round 27 (2026-10-02): -Wtype-limits: a constant on the left of a narrower
  unsigned operand (0ULL > p, p unsigned int) gets the "limited range of data
  type" message, not the "unsigned expression in < 0" one (gcc.dg/pr59846).
  opeq (-Wlogical-op, -Wtautological-compare) treats calls of const/pure
  functions with equal arguments as equal.  Tests 935; gcc.dg 3639,
  c-c++-common 536; ASan/UBSan clean.  Goldens: type_limits_left, logical_op.
  Remaining -Wlogical-op gap: signedness casts inside a range test.
- Round 28 (2026-10-03): -Waddress for &&label and &__real__/&__imag__ x.
  &&lab is checked as a truth value in if/while/do (cond_check in cstmt.c);
  the warning sits at the && column, and the "will always evaluate as true"
  form is given once per label per function (Checker.lbl_*), later uses get
  the comparison form.  &__real__ x is named "__real__ x".  Gap: pointer-index
  operands (__real__ *(p + (sizetype)...), gcc.dg/Waddress-3 lines 30-31).
  Tests 937; gcc.dg 3640, c-c++-common 536; ASan/UBSan clean.  Golden:
  address_label_real.
  Re-bucket of what is still missing (diags/files, gcc.dg + c-c++-common):
  -Wsizeof-pointer-memaccess 166/5, errors 161/79, -Wattributes 142/43,
  -Warray-bounds= 106/5, -Wstringop-overread 86/3, -Wparentheses 69/3,
  plain warnings 58/26, -Wc++-compat 54/14, -Wpedantic 44/18, -Wformat= 37/6,
  -Wpragmas 35/11, -Wcast-qual 34/4, -Wsign-compare 25/5, -Wtraditional 25/11.
  Survey -Wstringop-overread: all 86 are "argument missing terminating nul"
  in warn-strlen-no-nul.c (76, -O2), Wstringop-overread-6.c and
  Wstringop-overflow-22.c (5 each); needs -O2 const-array contents and
  index/range tracking (middle-end).  Low value; not planned.
- Round 29 (2026-10-03): -Wsizeof-pointer-memaccess (in -Wall; c-c++-common
  Wsizeof-pointer-memaccess1-3 SAME).  sizeof_memaccess (cexpr.c) runs from
  call_args and, for __builtin___*_chk (no call_args), from e_call.  Table
  MemAcc: strncmp/strncasecmp, strn{cpy,cat}/stpncpy, bcopy, memcpy, memmove,
  bcmp, memcmp, memset, bzero, memchr, snprintf, vsnprintf (not mempcpy;
  bcmp/bzero/bcopy only in gnu mode).  The sizeof operand must be a direct
  argument (parens ok).  Destination is checked first, then source; explicit
  casts on an argument block it.  "same expression" (opeq, &*p = p, string
  literals by spelling): "remove the addressof" for &x, "explicit length" for
  char-sized pointees or string functions, else "dereference it".  Otherwise
  "same pointer type" (not string functions, not void*), skipped when another
  argument points to the sizeof type (memcpy (&p, q, sizeof p)).
  strncpy (d, s, sizeof s) with an array s: "use the size of the destination",
  unless d is a known-size array/VLA, s is nonstring, or dest == src.
  Tests 939; gcc.dg 3641, c-c++-common 540.  Golden: sizeof_memaccess.
  bench/tools/cmp.sh and verify.sh (kept in the repo; WSL upgrades wipe /tmp)
  replace the old /tmp helpers; verify.sh pins CEREAL_GCC=gcc-13.
- Round 30 (2026-10-03): -Wparentheses: `!a & b` / `!a | b` (right operand not
  a comparison, && , || or !; lognot_bitop in cexpr.c; location is the `!`, or
  the operator for a constant operand) and `_Bool d = a = b` (e_assign).
  gcc.dg Wparentheses-3 and -11 SAME.  Tests 941; gcc.dg 3643, c-c++-common 540.
  Golden: parens_lognot_bitop.
  -Wattributes survey (108 missing in 37 files, 11 extra): a long tail with no
  dominant message; biggest are "attribute ignored" (15/7 files), "conflicts
  with previous" (11/2), "mismatch with mode" (10/1), "only applies to
  function types" (7/3).  Extras: "ignored on a declaration of a different
  kind than referenced symbol" (5), "attribute directive ignored" (3).
  Best first target: the 11 extras (wrong, not just absent).
- Round 31 (2026-10-03): -Wattributes extras fixed.  copy(X) source unwrap now
  looks through index and comma expressions (cexpr_asets too); copy of
  alloc_size/alloc_align onto a non-pointer function warns (a source that is
  itself non-pointer-returning has nothing to copy); copy(undeclared),
  copy("str"), copy(const) are errors as in gcc; the __const attribute name is
  "const" (no "directive ignored").  Golden: attr_copy_errs.  Tests 943;
  gcc.dg 3645, c-c++-common 540.  Known gap: 'const' attribute on a function
  returning void is not warned.
  bench/tools/san.sh: ASan+UBSan build in ~/cereal-san, run.sh + san.py over
  tests/check and tests/parse: 943 passed, 300 files, 0 findings (also covers
  sizeof-pointer-memaccess, parentheses, attributes work).
  -Warray-bounds= survey (c-c++-common only, 106 missing in 5 files, none in
  gcc.dg): Warray-bounds-7.c 60 (constant-string offsets, needs middle-end
  range tracking); builtin-offsetof-2.c 35, pr41935.c 8, builtin-offsetof.c 1
  ("index N denotes an offset greater than size of T" on __builtin_offsetof
  array indices, no -O needed: a front-end fold check); vector-subscript-3.c 2
  ("index value is out of bound", constant vector subscript).  Cheap target:
  the 46 offsetof + 2 vector diagnostics.
- Round 32 (2026-10-03): -Warray-bounds= front-end parts.  fold_offsetof_1:
  "index N denotes an offset greater than size of T" for constant offsetof
  array indices (one past the end is fine for the outermost reference only;
  no warning when every component back to the start is the last member of its
  struct or in a union -- the poor man's flexible array), at the type name's
  tag, or the statement start for a typedef name (offsetof_bf_loc);
  offsetof through a pointer member is "cannot apply 'offsetof' to a non
  constant address".  Constant vector subscripts: "index value is out of
  bound".  Option array-bounds= (-Wall).  Also "'pure'/'const' attribute on
  function returning 'void'" (not for pointers to function).  Goldens:
  array_bounds_offsetof, attr_pure_const_void.  Tests 947; gcc.dg 3645,
  c-c++-common 544.  Remaining array-bounds: Warray-bounds-7.c (60, constant
  strings, middle-end).  -Wstrict-aliasing level 1 and -Wlogical-op signedness
  casts: no diagnostics missing in either corpus, so no work needed.  Still
  unimplemented: "'const' attribute ignored" on typedefs and variables.
- Round 33 (2026-10-03): ASan/UBSan (san.sh) on the Round 32 tree: 947 passed,
  302 files, 0 findings.  error/warning attribute with a non-string message on
  a function: "'error' attribute ignored" (golden attr_error_nonstring).
  Tests 949; gcc.dg 3646, c-c++-common 544.
  -Wattributes missing now (gcc.dg + c-c++-common, diagnostics/files):
  alloc_align conflicts with previous designation 20/3; "'X' attribute
  ignored" 14/6 (each a different handler: 'used' on unnamed parameter
  declarators attr-nest.c, 'packed' on a typedef of an incomplete struct
  pack-test-3.c, const/pure on typedefs and variables); requested alignment
  '0' not a positive power of 2 11/4; access(mode) mismatch 10/1; fd_arg only
  applies to function types 9/4; format on non-variadic function 6/1.
- Round 34 (2026-10-03): -Wattributes batch.  aligned(0) warns "requested
  alignment '0' is not a positive power of 2" (once per attribute; a struct's
  are seen twice).  A redeclaration whose alloc_size / alloc_align differs
  from the oldest recorded one is ignored with "ignoring attribute 'X (a)'
  because it conflicts with previous 'X (b)'" (functions only; the typedef
  case in attr-alloc_size-13.c line 34 is still missing).  fd_arg /
  fd_arg_read / fd_arg_write use positional_arg.  "'X' attribute only applies
  to function types" for fd_arg*, nocf_check, warn_unused_result, alloc_*,
  access, format, nonnull, sentinel, returns_nonnull, assume_aligned,
  format_arg and the calling-convention attributes, on an object or typedef
  whose type is neither a function nor a pointer to one (attrs_fn_only).
  The attr_args golden was stale (gcc does print it for assume_aligned on an
  int).  Goldens: attr_aligned_zero, attr_alloc_redecl, attr_fn_only.
  Tests 955; gcc.dg 3647, c-c++-common 547.  Gaps seen: __builtin_has_attribute
  does not warn for aligned(0) or 'mode'; warn_unused_result on a void
  function typedef; typedef alloc_size conflicts.

- Round 35: gcc's "'format' attribute cannot be applied to a function that
  does not take variable arguments" for a first, non-static, no-prototype
  declaration of vprintf / vfprintf / vsprintf / vsnprintf / vscanf / vsscanf
  / vfscanf (built-in format attribute with first-arg 0), at the start of the
  declaration (cexpr_builtin_noproto_fmt).  The non-v built-ins keep
  -Wbuiltin-declaration-mismatch.  __builtin_has_attribute warns for
  aligned(0) (at the expression, i.e. the macro expansion point with
  -ftrack-macro-expansion=0) and for 'mode' ("not supported in
  '__builtin_has_attribute'", at the attribute token; result 0).
  builtin-has-attribute-4.c has no gcc-only diagnostics left.  Goldens:
  attr_fmt_noproto, attr_has_warn.

- Round 36: three fixes.  (1) A local typedef that is redeclared in its scope
  counts as used (gcc gives no -Wunused-local-typedefs), and a variable named
  in __builtin_has_attribute counts as used (a function does not: it keeps
  "declared 'static' but never defined").  c-c++-common/builtin-has-attribute-4.c
  is identical now.  (2) A call to a no-prototype built-in with too few
  arguments warns first and then skips the nonnull and format checks
  (check_builtin_function_arguments fails); builtins.c is identical.  (3)
  access attribute merging (append_access_attrs): per function, one accepted
  access attribute per pointer argument, the first wins; a later one for the
  same pointer is dropped with "attribute 'access(M, P[, S])' mismatch with
  mode 'M0'" / "positional argument 2 missing in previous designation" /
  "missing positional argument 2 provided in previous designation by
  argument S0" / "mismatched positional argument values S and S0", at the
  declarator name, with "previous declaration here" when an earlier
  declaration exists (acc_start / acc_replay / acc_add in cdecl.c; the replay
  reads the symbol's attribute chain lazily so the summary records no extra
  lookups).  Checked against gcc-13 on all 36x36 pairs, same declaration and
  redeclaration (no checked-in generator; see the goldens).  Goldens:
  local_typedef_redecl, builtin_few_nofmt, attr_access_redecl.  Tests 965;
  gcc.dg 3649, c-c++-common 552.  Gaps: the access attribute implied by VLA
  parameters (attr-access-2.c, Wvla-parameter-7.c: "conflicts with previous
  designation", "designating the bound of variable length array argument").

## Round 37 — "attribute ignored" by declaration context

- `attrs_ctx_check` (cdecl.c) replaces `attrs_fn_only`: a gcc-13-probed table gives, per context (typedef, file var,
  auto local, static local, parameter, field), the attributes that are "ignored" on non-function types, plus the
  function-only list (six names added, `format`). Names with an Attrs flag (noinline, used, weak, packed, alias,
  weakref, error, warning, cleanup) stay in `attrs_misapplied`.
- `nonstring` (objects: char array/pointer else "ignored on objects of type"; typedef: "does not apply to types"),
  `malloc` on non-functions/enumerators, `malloc` return-type warning skipped when a prior declaration's
  noreturn/const/pure excludes it, enumerator attributes, `visibility` on tag definitions.
- Golden: `attr_ignored_ctx`. Parity: tests 967, gcc.dg 3652 identical (223 differ), c-c++-common 554, san clean (312).
- Known gaps: `tls_model` without thread storage, `leaf` on function-pointer parameters, warning order when
  `attrs_misapplied` and the table both fire on one declaration, `packed` on typedef of incomplete struct,
  fallthrough, alloc_size/alloc_align in has_attribute type-names.

## Round 38 — access attribute implied by VLA parameters

- A prototype's `T a[n]` parameter implies `access(read_write, pos, n)` (`cparm_implied`, cparm.c); `[*]` implies a
  weak size-less entry that never warns. Implied entries are kept in the function's attribute chain as `~mode,p,s`
  and carry the bound parameter's location for the "designating the bound of variable length array argument N" note
  (old declaration's name if it names the parameter, else the current one).
- cdecl.c: `acc_kind`/`acc_diag`/`acc_add`/`acc_implied`. Explicit-vs-implied size mismatch reads "conflicts with
  previous designation by argument N"; a VLA bound found after the attribute names the attribute as `access (…)`.
  A non-definition redeclaration adds no implied entry of its own but repeats earlier conflicts (gcc re-merges).
- Golden `attr_access_vla`. All 256 old/new declaration-form combinations in a probe matrix match gcc on the access
  warnings. Parity: tests 969, gcc.dg 3653 identical (222 differ), c-c++-common 554, san clean (313).
- Known gap: after a declaration with an explicit `access` on a pointer/VLA parameter, gcc describes that parameter
  as an ordinary `char[]` in later -Wvla-parameter warnings (~64 matrix combinations); cereal does not.
- Goldens must use LF line endings (a CR in the `// flags:` line breaks run.sh).

## Round 39 — `__label__` runs, -Wduplicated-cond, tls_model/leaf contexts

- cstmt.c: "ISO C forbids label declarations" once per leading run of `__label__` declarations, at the last
  (`label-decl-4.c` matches). Golden `label_decl_run`. Gap: a `__label__` after a declaration/statement should be
  `error: expected expression before '__label__'`; cereal accepts it with a warning.
- `-Wduplicated-cond` (`cstmt_dup_cond`, cstmt.c): per else-if chain (head = an `if` that is not the else of
  another), each condition without side effects and not constant is compared (`dup_expr`) with the earlier ones;
  warns at the condition, note "previously used here" at the first match. Sorted by condition position
  (gcc warns while parsing). Golden `dup_cond`. Gap: gcc also matches conditions its folder canonicalises
  (`n+1 == 3` vs `n == 2`, `i<j` vs `j>i`); cereal compares structurally only.
- cdecl.c `attrs_ctx_check`: `tls_model` on objects (no thread storage → "ignored because 'v' does not have thread
  storage duration"; with it, bad argument → error) via the `AC_TLS` flag; `leaf` on any non-function (ignored, plus
  "no effect on unit local functions" when not public: static, param, field, typedef, auto local; `AC_PUB`);
  attributes after a declarator's `*` are now checked too (wrapper walks the N_PTR chain). Golden `attr_leaf_tls`.
- Known gaps unchanged otherwise: warning order when `attrs_misapplied` and the table both fire, `packed` on typedef
  of incomplete struct, fallthrough, `-Wvla-parameter` "ordinary char[]" quirk.
- `duplicated-cond` is registered off by default (not in -Wall); a side-effecting condition resets the chain's
  comparisons (Wduplicated-cond-1..4 match). Parity: tests 975, gcc.dg 3655 identical (220 differ),
  c-c++-common 555 (102), san clean (316).

## Round 40 — duplicated-cond folding, VLA-typedef parameters

- `-Wstringop-overread` triaged and NOT started: the 86 missing diagnostics are in 3 files. `warn-strlen-no-nul.c`
  (76) needs the -O2 strlen pass (variable offsets into arrays); `Wstringop-overread-6.c` and `-overflow-22.c` need
  front-end folding of `strlen`/`copysign` etc. on constant arrays (gcc folds `__builtin_strlen(arr)` to a constant
  at parse time, which also shifts -Wparentheses columns). Prerequisite for any of them: a builtin constant folder.
- `cstmt_dup_cond`: conditions now compare after gcc's fold (`dup_cmp_of`/`dup_cond_same`): operands of a comparison
  with the constant on the right, `>`/`>=` turned into `<`/`<=`, `< C` into `<= C-1`, `x ± K cmp C` into `x cmp C∓K`
  (signed, or ==/!=), `-x cmp C` mirrored, `!` pushed into the comparison (not for floats), `v` as `v != 0`.
  The warning column is the condition's first token for a non-boolean binary expression (gcc wraps it in `!= 0`).
  Probe of 40 pairs matches gcc. Goldens `dup_cond_fold`. Not folded by gcc either: `a&&b` vs `b&&a`, `i*2` vs `i<<1`.
- cparm.c: a parameter whose type is a typedef of a VLA is "not described" (`unk`) instead of looking like a
  pointer; removes five false -Wvla-parameter warnings in Wvla-parameter-4.c. Parameters whose adjusted types differ
  in kind (conflicting redeclaration, an error already) are not compared (pr105635.c). Golden `vla_param_typedef`.
- Still open in Wvla-parameter-4.c: the typedef'd VLA bound text for "mismatched bound 'n'" (VLA types carry no bound
  expression), and `int (*[*])[3]` {aka …} type printing for arrays of pointers to arrays. Warray-parameter-11.c
  needs constant folding of `!__builtin_copysign(~2, 3)`.
- `-Wvla-parameter` "ordinary char[]" quirk after explicit `access` (64 matrix combinations, no testsuite file hits
  it): not done. Observed model: with an explicit access attribute on the argument in either declaration, gcc
  describes the older declaration's pointer/VLA/[*] parameter as `T[]`; needs the attribute presence plumbed into
  cparm_compare (attributes are processed after the merge).
- Parity: tests 979, gcc.dg 3656 identical (219 differ), c-c++-common 555 (102), san clean (318).

## Round 41 — typedef VLA bounds, `__label__` placement
- cparm.c `cparm_typedef` (called from `declared_visit` for typedef declarators) records a VLA typedef's bound
  expressions in `c->tdvla`; `parm_of` resolves parameters of such typedefs (including typedefs of typedefs) from it,
  so Wvla-parameter-4.c's "mismatched bound" warnings now match gcc. Golden `vla_param_typedef_bounds`.
- Still open there: type printing `int (*[*])[3]` {aka `int (*[])[3]`} (cereal prints `int (*)[3][*]`, 3 lines);
  gcc warns "int[2][n][3] declared as a variable length array" when the older declaration is a typedef with a
  constant first bound (gcc quirk, not modelled).
- parse.c `lbl_ok`: `__label__` is accepted only before the first declaration/statement of a block; later it is
  `error: expected expression before '__label__'`. Golden `label_decl_late`.
- Parity unchanged: tests 983, gcc.dg 3656 identical (219 differ), c-c++-common 555 (102), san clean (320).

### Round 41b — misleading-indentation, nonnull on function pointers
- cstmt.c `misleading`: macro tokens are positioned at their expansion point (no longer skipped); a `goto` body
  uses the `goto` keyword column; an empty body `;` on the guard line warns when the next statement is indented past
  the guard line. Golden `misleading_goto_empty`.
- Still open in Wmisleading-indentation.c: empty macro invocations between body and next statement (gcc stays quiet;
  cereal has no "preceded by empty expansion" token flag), `#if`-skipped regions, `if (...);` followed by a `{`
  block, statement-expression bodies, `else if (b)` chains at 1154.
- `nonnull` on a function-pointer object: file-scope attribute stored on the symbol, block-scope attribute on the
  binding (`Bind.nn`), so a later unrelated local redeclaration does not inherit it (matches gcc's xfail cases in
  Wnonnull-6.c). Golden `nonnull_fnptr`. Open: built-in nonnull via `__builtin_va_arg_pack` (pr62090.c),
  builtin-arith-overflow-1.c arg 3.
- Parity: tests 987, gcc.dg 3657 identical (218 differ), c-c++-common 555 (102), san clean (322).

### Round 42 — -Wcast-qual
- cexpr.c `cast_qual` (gcc's handle_warn_cast_qual) called from e_cast for pointer→pointer casts: discarded target
  qualifiers, qualifiers added to function types (`__attribute__((const))` / `noreturn`), and the "all intermediate
  pointers must be const" check, only when the types are otherwise the same (same depth, compatible base).
- Function types now keep their qualifiers on a plain declaration (`const fnt f;`) and print them as
  `__attribute__((const)) int (*)(int)` (type.c); the "makes … qualified function pointer" text uses the same names.
- Goldens `cast_qual_ptrs`, `cast_qual_func`, `cast_qual_basic` (gcc testsuite files).
- Parity: tests 993, gcc.dg 3659 identical (216 differ), c-c++-common 556 (101), san clean (325).

### Round 43 — -Wformat forms
- `format_arg(N)` attribute stored (`CSym.fmtarg`, `Attrs.fmtarg`); the format expression is resolved through
  `?:` arms (a constant condition folds to one arm; identical literal arms are checked once, like gcc's fold) and
  calls of format_arg functions (`fmt_leaves`). Golden `format_arg_cond` (Wformat-pr104148.c).
- A parenthesized literal reports at its parenthesis; casts, `"str" + N` and `&"str"[N]` of a literal are checked
  (`fmt_check` takes a skip count; columns stay those of the unshifted string, as gcc does), a wide literal gives
  "format is a wide character string". Golden `format_literal_forms`.
- `-Wformat-signedness` (new option): %d/%i against an unsigned and %u/%o/%x/%X against a signed argument; unsigned
  types narrower than int are exempt; `%hhu`/`%hu` take any int; `%c` wants a signed int. Golden `format_signedness`.
- Open: a format read through a `const char[]` initializer (gcc checks `const char fmt[] = "%d"; printf(fmt, 1.0)`
  and warns "format string is not an array of type 'char'" for `const unsigned char[]`) — symbols keep no
  initializer reference; unterminated-format-string (Wstringop-overflow-22.c); Darwin CFString/NSString formats.
- Parity: tests 999, gcc.dg 3659 identical (216 differ), c-c++-common 558 (99), san clean (328).

### Round 44 — misleading-indentation source gap
- cstmt.c `gap_scan`: the source text between the body's last token and the next token is scanned (comments and
  white space skipped). A preprocessing directive there (`#if`, `#ifdef`, …) silences the warning; the text of a macro
  invocation that expanded to nothing stands in for the next statement's position (no preprocessor flag needed).
  Empty `;` bodies on a line of their own count as properly indented; one behind a comment does not.
  Goldens `misleading_gap`, `misleading_goto_empty`.
- Still differing in Wmisleading-indentation.c (4 diagnostics): `for` produced by a macro (704), `else if (b);`
  followed by a `{` at the `else` column (804), the statement-expression `while`/`if` (924), `} else if` with an
  over-indented body (1154).
- Parity: tests 1001, gcc.dg 3659 identical (216 differ), c-c++-common 558 (99), san clean (329).

### Round 45 — const-array formats, typedef pointer-array printing
- `cexpr_note_strinit` (called from `cinit_decl_done`) records the decoded bytes of a string initializer of a
  const `char`/`signed char`/`unsigned char` array (`CSym.strinit` → `Checker.strinits`). `check_format_literal`
  reads a format given as such an array (also through casts, parentheses, `+ N`): checked like the literal, columns
  inexact (the argument's location; `fmt + N` reports mismatches at the `+`, other warnings at `fmt`);
  a non-`char` array gives "format string is not an array of type 'char'". Golden `format_const_array`
  (Wformat-pr84258.c). Not done: writable arrays (gcc does not read them), strlen folding / `-Wstringop-overread`.
- cparm.c: an array of pointers to a typedef'd array (`IA3 *x[*]`) now goes through the level printer
  (`int (*[*])[3]`) and gets gcc's ` {aka 'int (*[])[3]'}` when it has `[*]` bounds. `pstr` returns the quoted
  type (the `'%s'` in `cmp_param` messages became `%s`). Wvla-parameter-4.c is identical. Golden
  `vla_param_typedef_ptrs`. Still differing: `IA3 x[*]` vs `IA3 x[n]` (gcc's first-bound quirk), `IA3 *(*x)[*]`.
- Parity: tests 1005, gcc.dg 3660 identical (215 differ), c-c++-common 559 (98), san clean (331).

### Round 46 — -Wsign-compare: promoted bitwise complement
- cexpr.c `bitnot_operand`/`bitnot_cmp`: in a comparison, `~E` (E unsigned, narrower than the type the operand is
  compared in, W; casts set W) against a constant whose bits p..W-1 are not all ones ("with constant", or "is always
  nonzero" for 0) or against an unsigned operand narrower than W ("with unsigned"). Reported at the operator,
  replaces the ordinary sign-compare/type-limits check. `cexpr_truth_warn` gives the "always nonzero" form for `~E` in
  truth contexts (`if`, `!`, `&&`, `?:`, casts and conversions to `_Bool`). Golden `sign_compare_bitnot`.
- Open: `b = c ? ~c : 0` (gcc folds the bool conversion into the arms). The survey's other sign-compare misses are
  vector-compare-4.c (12, vector element signedness) and pr35430.c (complex int). `-Warray-bounds` has no misses left
  in the gcc.dg/c-c++-common/-O lists (the "60" in the earlier note was stale).
- Parity: tests 1007, gcc.dg 3662 identical (213 differ), c-c++-common 560 (97), san clean (332).

### Round 47 — vector sign-compare; survey of float-conversion / stringop-overread
- Comparing two integer vectors whose element signedness differs warns "comparison between types 'v4qi' {aka ..}
  and 'uv4qi' {aka ..}" (-Wsign-compare, at the operator). Golden `vector_compare_sign`; vector-compare-4.c identical.
- Survey, nothing implemented: all 40 missing `-Wfloat-conversion` diagnostics are constant *complex* values
  (Wconversion-complex-{c99,gnu}.c; need complex constant folding and `(_Complex double){..}` printing, which would
  also fix missing -Wconversion/-Woverflow lines in the same files). All 86 `-Wstringop-overread` are strlen/strcspn
  "argument missing terminating nul" at -O2 (warn-strlen-no-nul.c alone is 76: arrays, struct members, `?:` arms,
  odd locations); the const-array initializer table (Round 45) is the base for it.
- Parity: tests 1009, gcc.dg 3662 identical (213 differ), c-c++-common 561 (96), san clean (333).

### Round 48 — -Wpedantic: shift in initializers
- A shift whose count >= width (or whose result overflows) makes an initializer a non-ICE constant, so `-pedantic`
  warns "initializer element is not a constant expression" (cexpr.c `e_shift` marks it K_FOLD; negative count does not).
- `ctok_loc` reports tokens of predefined macros (`<built-in>`, `<command line>`) at the use site (check.c
  `cbuiltin_loc_`), so `__INT_MAX__ << 2` is no longer dropped as a system-header location. Side effect: a user
  `-D` macro is also reported at the use (gcc: at the -D location); no verify regression seen.
- Golden `shift_init_pedantic`; Wshift-overflow-1..4.c identical.
- Parity: tests 1011, gcc.dg 3662 identical (213 differ), c-c++-common 565 (92), san clean (334).

### Round 49 — parenthesized string in a compound literal
- "array initialized from parenthesized string constant" inside a compound literal is reported at the literal's
  `(` (cinit.c, close path); other init diagnostics there keep the `{`. Golden `paren_string_complit`;
  init-string-1.c identical.
- Open (found while triaging -Wpedantic, not done): attribute-only statements. `__attribute__((used));` should give
  "empty declaration" (location = first token on the line, gcc quirk), after a `case`/`default` label also the
  label-declaration pedwarn, and fallthrough misuse gives -Wattributes ("not followed by", "specified with a
  parameter", "specified multiple times", "not preceding"); cereal's N_ATTR_STMT is never checked
  (attr-fallthrough-2.c, Wimplicit-fallthrough-20.c).
- Parity: tests 1013, gcc.dg 3664 identical (211 differ), c-c++-common 565 (92), san clean (335).

### Round 50 — attribute-only statements (fallthrough family)
- cdecl.c `attr_stmt_visit` checks N_ATTR_STMT (`__attribute__((..));`): no fallthrough in the list gives
  "empty declaration"; with fallthrough: "specified multiple times", "specified with a parameter", and
  "'X' attribute ignored" for the others. Located at the first token of the line (gcc quirk, `line_start_loc`).
  After `case`/`default` the label-declaration pedwarn comes first; after a named label the attributes belong to
  the label (no warning). `[[..]]` statements are skipped.
- "'fallthrough' attribute not followed by ';'": parse.c `fallthrough_not_followed` (when what follows is not a
  declaration start; block items, statements, label bodies) and cdecl.c `attrs_ctx_check1` (leading list of a
  block-scope declaration). `label_body` now sends a non-`;`-terminated `__attribute__` list to `declaration`.
- Golden `attr_stmt_fallthrough`; attr-fallthrough-2.c and Wimplicit-fallthrough-20.c identical.
- Open: fallthrough's arguments are not evaluated (`fallthrough(a)` should say `'a' undeclared`); file-scope
  `__attribute__((used));` should give "empty declaration"; `m: __attribute__((fallthrough));` at the end of a
  block should say "'fallthrough' attribute ignored" (label attribute).
- Parity: tests 1015, gcc.dg 3664 identical (211 differ), c-c++-common 568 (89), san clean (336).

### Round 51 — -Wattributes argument checks; fallthrough leftovers; constant-folding survey
- `__builtin_has_attribute(x, alloc_size/alloc_align (args))`: "ignored on a function returning 'void'" and
  "ignoring attribute 'X (a)' because it conflicts with previous 'X (b)'" (cexpr.c `e_has_attr`;
  cdecl.c `cdecl_alloc_conflict`, `cdecl_aset_first_arg`); reported at the first token of the line.
  builtin-has-attribute-3.c identical.
- `nonnull`/`alloc_size` arguments: "argument [N ]is invalid" for an erroneous argument, "argument [N ]has type 'T'"
  for a non-integer; `vector_size(foo)` with a non-constant gives the error; `patchable_function_entry` argument
  checks ("is not an integer constant", "exceeds 65535"), located at the first token of the declarator's line
  (`patchable_loc`). attributes-1.c, pr89946.c, patchable_function_entry-error-3.c identical.
- Fallthrough leftovers: `fallthrough(args)` arguments are evaluated; file scope `__attribute__((x));` gives
  "empty declaration" and `fallthrough` there "attribute at top level"; `m: __attribute__((fallthrough));` gives
  "attribute ignored". Goldens `attr_args_misc`, `attr_stmt_fallthrough` (extended).
- Not done (small): UCN/keyword attribute names print raw (`'__int128'`, ucnid-13*.c), access-attribute implicit
  mode mismatch extras (uninit-37.c), other attributes' arguments are not evaluated (`used(b)` should say
  `'b' undeclared`; needs the per-attribute identifier-argument list).
- Parity: tests 1017, gcc.dg 3664 identical (211 differ), c-c++-common 572 (85), san clean (337).

#### Survey: generalizing constant folding (nothing implemented)
Current model: `ck` kinds K_ICE/K_FOLD (integers in `cv`, uint64), K_FLOAT (`fv`, host `long double`), K_ADDR
(symbol base + offset). Missing diagnostics that trace to a folding gap, by size:
1. Strings/arrays as constants (~145): `-Wstringop-overread` 86 (strlen/strcspn/... on arrays without a nul; needs
   the const-array table of Round 45 plus builtin string evaluation, struct members, `?:` arms) and
   `-Warray-bounds` "outside bounds of constant string" 60 (Warray-bounds-7.c: a K_ADDR whose base is a string
   literal, with the offset checked against its length). One generalization serves both: a "known bytes" value
   (literal or const-initialized array + offset) attached to pointer-valued expressions.
2. Complex constants (~60): `-Wfloat-conversion` 20, `-Wconversion` 14, `-Woverflow` ~8 (Wconversion-complex-*.c,
   overflow-warn-8.c, c99-const-expr-7.c), plus pr35430.c. Needs a K_COMPLEX (re, im in `fv`), folding of
   `__builtin_complex`, `+ - * /`, casts and `__real__/__imag__`, and printing `(_Complex double){re, im}`. The
   `-Wdouble-promotion` complex misses (14) are type-only, no folding.
3. Not folding after all, despite appearing in the lists: `-Wint-in-bool-context` (`<<`), `-Wparentheses`
   omitted-middle-operand, `-Wsizeof-array-div`, `-Wmemset-transposed-args` need flags/other rules; check each
   file's dg-options before assuming a folding cause.
4. Representation limits: `cv` is 64-bit, so `__int128` constants cannot be held (only 1 corpus diagnostic
   depends on it, pr105186.c is a naming issue); `fv` is x87 `long double`, so `_Float128` arithmetic and exact
   float/double rounding of intermediate values are approximations. Neither shows up in the missing lists today.
Suggested order: (2) complex is self-contained and mostly local to cexpr.c; (1) is larger but covers ~145
diagnostics and builds on the strinit table.

### Round 52 — complex constant folding (survey item 2)
- Complex constants carry `EF_CPLXCST` (ck stays K_NONE): parts in `fv[cv]`, `fv[cv+1]`, rounded to the component type
  (integer parts exact in long double). Producers: imaginary literals, `__builtin_complex` (now typed, with gcc's three
  errors and "cannot take address"), `+ - * /` (float division by the textbook formula, integer `/` untracked),
  unary `- + ~`, casts/usual conversions (`conv_const`), `__real__`/`__imag__` (fold to K_FLOAT / K_FOLD).
- Consumers: `-Wconversion`/`-Wfloat-conversion`/`-Wsign-conversion` print `(_Complex T){re, im}` (`cplx_conv_warn`);
  `-Woverflow` for complex -> integer (float parts saturate; integer parts only overflow a signed target);
  initializer constness (`const_varlike`); `-Wsign-compare` for complex integers (pr35430).
- Also: `real_cst_str` prints zero as `0.0`/`-0.0`; a folded real or complex binary expression passed as an argument is
  located at its operator (gcc), not at the argument start.
- Golden `cplx_const` (needs the runner's implicit `-pedantic`; generate with `-std=c99 -pedantic`).
- Parity: tests 1019, gcc.dg 3667 (208), c-c++-common 572 (85), san 338 clean.
- Found, not done: `-Wdouble-promotion` is not implemented at all (3 corpus files; Wdouble-promotion.c ~22 diagnostics);
  `Wc90-c99-compat-2.c` (cereal prints 1 diagnostic vs 19: unrelated to folding); c99-const-expr-7.c (file-scope
  "overflow in constant expression").
- Step 2 of the survey (known bytes): gcc's `c_strlen` runs in the front end, so `Warray-bounds-7.c` ("offset 'N'
  outside bounds of constant string", for `strlen` of a zero-length/flexible member of a const object, offsets truncated
  to int) needs a new strlen-folding helper; cereal has none (strinits only serve format checks).

## Round 53: strlen constant folding (c_strlen diagnostics)

- `check_strlen`/`sl_resolve` (cexpr.c, before `check_format_literal`): argument of `strlen`/`__builtin_strlen` resolved through parens, casts, `?:` (location = the ':'), `x±K`, `&x[K]`, `&*x`, literals, const char arrays with string initializer (`strinits`), and `OBJ.member` of a const global (bounds only, no bytes).
- Emits "offset 'N' outside bounds of constant string [-Warray-bounds=]" (N int-truncated) and "'strlen' argument missing terminating nul [-Wstringop-overread]" (new default-on option in diag.c), each with a note at the object.
- Golden: tests/check/strlen_const.c. `Warray-bounds-7.c` now identical. Parity: gcc.dg 3666 identical (209 differ), c-c++-common 573 (84), san clean (339 files).
- Not done: `warn-strlen-no-nul.c` (needs initializer bytes for 2D arrays and nested struct members via cinit's pending sets; some cases are -O2 middle-end); `&obj.m[K]` (gcc offsets from the whole object, skipped); bare `strlen(d);` lacks "statement with no effect".
- Build noise: san.sh's -O2 build shows two -Wformat-truncation warnings in `real_cst_str` (cexpr.c ~3053, 3063); harmless, could size the buffer.

## Round 54: -Wdouble-promotion, decimal/binary float mixing error

- `double_promo` (cexpr.c, before `conv_operand`): float / complex float -> double only (gcc does not warn for long double). Hooks: `e_arith` and `e_compare` (location = operator, "to match other operand of binary expression"), `e_cond` (location = ':', "to match other result of conditional"), `call_args` (unprototyped/implicit/variadic args, location = argument, "when passing argument to function"). Suppressed in unevaluated operands via `inhibited`. New option `double-promotion` in diag.c.
- `dec_mix`: "cannot mix operands of decimal floating and other floating|complex types" for arithmetic, comparison, `?:`, compound assignment. Located at gcc's input_location (`cinput_loc` of the token after the expression), not the operator.
- Goldens: tests/check/double_promo.c, dec_mix.c. `Wdouble-promotion.c`, `dfp/pr79515.c` identical.
- Parity: gcc.dg 3667 identical (208 differ), c-c++-common 573 (84), san clean (341 files).
- Survey: `-Wc90-c99-compat` is not implemented at all (the earlier "1 diagnostic" was something else). 9 corpus files, ~110 diagnostics, but only -2 and -9 are gnu99; about 15 distinct checks across parser, decl and preprocessor.

## Round 55: constant-expression overflow leftovers (c99-const-expr-7.c)

- Explicit float->int cast of an out-of-range or NaN constant sets `EF_OVERFLOW` (conv_const), so `int b = (int) DBL_MAX;` gets "overflow in constant expression".
- `0 << -1` / `0 >> -1` stay constants to initializers (pedwarn "initializer element is not a constant expression"); a nonzero left operand with a negative count is still a hard error (golden shift_init_pedantic).
- Overflow in a braced-initializer element and in a designator array index (`[0 * (INT_MAX + 1)] = 0`) is reported at gcc's input_location (`cinput_loc` of the token after the expression), not the expression.
- Golden: tests/check/const_expr_ovf.c. `c99-const-expr-7.c` identical. Parity: gcc.dg 3668 identical (207 differ), c-c++-common 573 (84), san clean (342 files).
- Survey, strlen remainder (`warn-strlen-no-nul.c`): 8 of 76 warnings matched, 68 missing. By shape: ~40 nested `ba[i].a[j].a|b` member arrays (+ `&...[K]`, `+ K`, `+ v`), ~22 2D `b[i][j]` rows, ~6 `s.b` members, the rest `?:` mixes of these. All need initializer bytes keyed by member/element path: record string-literal leaves in cinit (PEnt PV_STR) for const objects, resolve `.m`/`[K]` chains in `sl_resolve`. About 150 lines; no false positives today. gcc quirks to respect: `&obj.m[K]` offsets from the whole object; non-constant outer indices (`b[i3]`) are not diagnosed under -fsyntax-only.
- Survey, `Wc90-c99-compat-2.c`: -Wc90-c99-compat is entirely unimplemented (9 corpus files, ~110 diagnostics, only -2 and -9 are gnu99 so only ~27 diagnostics are reachable). About 15 checks across the lexer/parser/decl/preprocessor: bool, complex, long long, flexible array members, bit-field type, duplicate qualifiers, variadic macros, enumerator trailing comma, [*] declarators, static/qualifiers in array parameters, compound literals, designators, mixed declarations, subscripting non-lvalue arrays, empty macro arguments, VLAs.

## Round 56 — `-Wc90-c99-compat`

- New `c90-c99-compat` option (implies `-Wlong-long`); `cc90`/`cc90_id` in check.c implement gcc's `pedwarn_c90` rule (own option on, or compat on and own option not explicitly off; tag = own option else compat).
- Covered: VLA, long long, mixed declarations, `_Complex`/`__builtin_complex`, `_Bool`, `[*]`, `static`/qualifiers in array params, flexible array members, bit-field types, enum trailing comma, duplicate qualifiers, compound literals, subscripting non-lvalue arrays, designators, `for` initial declarations; pp: anonymous variadic macros (untagged), empty macro arguments (tagged).
- Goldens: `c90_compat`, `c90_compat_pp` (split because `--cells` emits pp diagnostics ahead of checker ones — pre-existing ordering difference).
- `Wc90-c99-compat-2.c` and `-9.c` identical to gcc. Other 7 tests need unsupported `-std` values.
- Loose end: gcc's "C++ style comments are incompatible with C90" (cpp warning, once per file) is not implemented (lexer has no hook).
- Parity: suite 1031/0; gcc.dg 3671 identical (204 differ); c-c++-common 573 (84); -O 3671 (204); san 343 files, findings 0.

## Round 57 — refactors from the diagnostic-pattern scan
- `tloc` is one inline in `check_int.h` (was 3 copies); `expr_loc` is one function in `cexpr.c` (the cstmt.c copy had diverged: union of both rules, incl. `__extension__` and `&&label`).
- `sq_eq` (sequence-point) now delegates to `opeq` (operand_equal_p); it was a second copy. Parity unchanged (3671/573/3671), suite 1031/0, san 344/0.
- Deliberately not done: a tag-and-gate resolver (`cc90_id` and `cped11` have different rules; the table would not remove duplication); merging `dup_expr` (statement-level, folded floats, `?:`) into `opeq`.

## Round 58 — cexpr.c split (refactor, no behavior change)
- `cexpr.c` 13.9k -> 9.6k lines. New: `cexpr_int.h` (node/type accessors as `static inline`, `BTab`, `Conv`, prototypes of the functions that now cross units), `cformat.c` (printf/scanf, strlen folding, string reads), `ccall.c` (e_call/e_index/e_member, builtin signatures, nonnull/restrict/atomic/tgmath), `cconv.c` (-Wconversion family, -Wdouble-promotion, decimal mix).
- The old "-Wformat" banner in cexpr.c really covered format + calls; the split follows the actual contents.
- Left in cexpr.c: -Waddress-of-packed-member, -Wstrict-aliasing, logical-op, sizeof checks etc. (each needs 40+ shared statics for 400-750 lines: not worth the header).
- Gates: suite 1034/0, parity unchanged (3671/573/3671), san 344/0; check icount on uvloop/loop.c 3.578G -> 3.568G (-0.3%; inlined accessors offset the cross-unit calls).
- Tooling used (scratch, not kept): a function mover that de-statics crossing functions and appends prototypes; if redoing another cut, expect forward `static` prototypes and shared typedefs to need hand moves.

## Round 59 — pragma validation (`src/c/cpragma.c`)

- New `cpragma.c` re-lexes each `#pragma` token (with per-token locations) and mirrors gcc 13's `handle_pragma_*`: `pack` (push/pop with ids, `pop` of an unknown id warns *and* pops the top, alignment ∈ {0,1,2,4,8,16}, junk, malformed forms), `GCC diagnostic` (kind/option validation, `fatal` no longer accepted, unknown / other-language / not-a-warning options, "did you mean"), `redefine_extname`, `scalar_storage_order`. All tagged `-Wpragmas`.
- `pack_stack` now holds `[align, idhash]` pairs (csum digests every entry, so unchanged).
- Option tables: `src/gcc_pragma_opts.h` generated by `bench/tools/gen_pragma_opts.py` (probes gcc-13); `gcc_opts.[ch]` now owns `gcc_wopt_*` (moved from `driver.c`) plus `gcc_pragma_opt`.
- Golden: `tests/check/pragma_valid.c` (output equals gcc-13's).
- Gate: suite 1041/0; gcc.dg 3680 identical (195 differ); c-c++-common 574 (83); -O 3680 (195); san 345 / 0; icount 3.567G.
- Not done: `#pragma GCC optimize` bad-option warning plus its per-function `attribute 'optimize'` repeat (`Wpragmas-1.c`) needs a probed table of valid optimize options.

## Round 60 — unused-value rules

- `e_call` ends with `call_pure`: a call to a `const`/`pure` function (attribute, or a library built-in listed in `src/c/cbuiltin_pure.h`, generated by `bench/tools/gen_pure_builtins.py` from gcc-13 probes with non-constant args) with side-effect-free args has no `EF_SIDE`, so a bare `strlen(d);` / `memchr(...)` gives "statement with no effect" (also as comma operands).
- `e_comma` runs `unused_value` (now exported) on a left operand that has side effects (gcc's `emit_side_effect_warnings`): `(bar (), 1, bar ())` → "right-hand operand of comma expression has no effect"; `f () + f (), f ()` → "value computed is not used".
- A `for` increment warns at the expression's own location (init stays at `for`).
- Golden `tests/check/unused_pure.c`.
- Gate: suite 1043/0; gcc.dg 3685 identical (190 differ); c-c++-common 575 (82); -O 3685 (190); san 346 / 0; icount 3.582G (+0.4%, the per-call attribute lookup).
- Not done: plain-name library calls are treated as built-ins whenever declared (gcc also requires a compatible declaration / no `-fno-builtin`).

## Round 61 — `-Wtraditional`

- Rules (all probed against gcc-13): numeric suffix (`u`/`i`/`j` on integers, any suffix on floats), unary plus, `\a`/`\x` meaning change, `\e`/`\E` non-ISO pedwarn (no tag), string-constant concatenation, `long`/`unsigned long` switch, non-static after static (functions and extern objects, with old-decl note), ISO C style function definitions (`CSF_PROTO_DEF`, emitted after `pushdecl`), automatic aggregate initialization (declarations and function-scope compound literals), union initialization (non-designated, non-zero, incl. `{0}` converted to float and `{0.0}`).
- Both init warnings share one location: the token after the last `struct`/`union` keyword before the declarator if on the initializer's line, else the first token of that line (`ctrad_tag`/`ctrad_loc`/`ctrad_decl_loc`).
- `vrep` suppresses `traditional` under `__extension__` (gcc's `disable_extension_diagnostics`).
- Goldens: `tests/check/traditional_1..6.c`.
- Gate: suite 1055/0; gcc.dg 3696 identical (179 differ); c-c++-common 575 (82); -O 3696 (179); san 352 / 0; icount 3.596G (+0.4%, the pure-flag lookup from Round 60 plus these checks).
- Not done: gnu89-only `wtr-*` tests (cereal rejects `-std=gnu89`); all `-Wc++-compat` rules.

## Round 62 — optimize pragma, pure-call cost

- `#pragma GCC optimize ("opt", ...)`: an option outside the Optimization set gives "bad option '%s' to pragma 'optimize'" [-Wpragmas] at the `GCC` token. Table `src/gcc_optimize_opts.h` generated by `bench/tools/gen_optimize_opts.py` (probes gcc-13; `n=` may take a value, `n==` needs one; `-O*` always accepted).
- `push_options` / `pop_options` / `reset_options` keep the list of bad options (`Checker.opt_bad`, digest `opt_dig` is part of `pack_digest`). Every function declaration/definition repeats each one as "bad option '%s' to attribute 'optimize'" [-Wattributes] at `cinput_loc` of the terminating `;`/`{`; an explicit `__attribute__((optimize("...")))` warns once likewise. `Wpragmas-1.c` now matches.
- `call_pure`: argument side effects are tested first, the attribute lookup is skipped for symbols without attributes, and the built-in name search is prefiltered by first character. icount 3.590G (Round 60 baseline 3.582G, was 3.596G).
- Golden `tests/check/optimize_bad.c`.
- Gate: suite 1057/0; gcc.dg 3697 identical (178 differ); c-c++-common 575 (82); -O 3697 (178); san 353 / 0.
- Not done: the order gcc repeats several bad options in (not pragma order, apparently hash order); strings holding several space-separated options; `-O` argument errors (`-O2=1`, `"-Os -fno-lto"`); a bad option merged into a function that also has an explicit attribute.

## Round 63 — `-Wc++-compat` rules

- Duplicate file-scope object definition ("duplicate declaration of 'x' is invalid in C++", + previous-declaration note) when neither declaration is `extern`.
- Uninitialized `const` object ("uninitialized 'const x'") and uninitialized const members (one warning + "'m' should be initialized" note per const field, nested records included), for every non-extern object without initializer.
- Empty struct/union ("has size 0 in C, size 1 in C++"), after the pedantic "has no members".
- Typedef/tag clash in one scope (`typedef_tag_clash`, from `new_tag` and `pushdecl`), with "originally defined here".
- Typedef names used inside struct bodies: "C++ lookup of 'x' would return a field, not a type" (an open struct has a field of that name; `Checker.fields`) and, at the struct's end, "using 'x' as both field and typedef name" (`Checker.tdseen`, `RecDef.first_td`).
- Non-local variable with an anonymous struct/union/enum type (a typedef-named one is fine).
- Types/enumerators defined inside a struct and used outside it (`RF_IN_STRUCT`, `Enum.in_struct`, `CSF_IN_STRUCT`).
- "defining a type in a compound literal" also for a first-reference tag (`cxx_defining_cast`).
- Enum-conversion messages print a bit-field source as `unsigned char:3` (`cmp_tstr`); `orig_type` of a comma expression is its right operand's.
- "request for implicit conversion" uses the macro expansion point (system-header macros).
- Goldens `tests/check/cxx_compat_1..7.c`.
- Gate: suite 1071/0; gcc.dg 3710 identical (165 differ); c-c++-common 575 (82); -O 3710 (165); san 360 / 0; icount 3.593G.
- Not done: `builtin-has-attribute.c` (other diffs in that file), the `-Wc++-compat` rows that remain are in files whose other diagnostics differ.

## Round 64 — `-Wlogical-not-parentheses` parity, `-Wint-in-bool-context`

- `-Wlogical-not-parentheses`: `!(constant)` is no longer a double negation, and a tree of `| ^ & ~`/int casts over boolean leaves (`boolish_bits`) suppresses the warning (pr49706.c, Wlogical-not-parentheses-3.c).
- `-Wint-in-bool-context` (new, `int_bool_warn` in `src/c/cexpr.c`, called from `cexpr_truth_warn`, so every truth-value context is covered: if/while/for/do, `!`, `&&`, `||`, `?:` condition, `_Bool` casts/assignments):
  - `*` (any arithmetic type, constant-folded results skipped), signed-promoted-lhs `<<`, and `?:` with integer-constant arms (not both 0/1; "always true" when both nonzero), located at the colon.
  - Passes through parens, unary `+`/`-`, comma right operand and non-narrowing arithmetic casts. Not implemented: gcc's location-less `cc1:` warning for narrowing casts.
- Goldens `tests/check/int_bool_1.c`, `int_bool_2.c` (both SAME against gcc-13 via `cmp.sh`).
- Baselines: suite 1075/0; gcc.dg 3711 identical (164 differ); c-c++-common 580 (77); -O 3711 (164); san 362 files / 0 findings; icount 3.598G (+0.14%).
- Gate note: a single `to` (timeout) entry for gcc.dg/binary-constants-1.c broke `diagstat.py` in one run (load); rerunning par.py fixed it.

## Round 65 — c-c++-common single-rule gaps

- New: `-Wmemset-transposed-args`, `-Wmemset-elt-size` (`memset_args`, called beside `sizeof_memaccess`), `-Wsizeof-array-div` (array branch of `sizeof_div`), `-Wdate-time` (`builtin_token` in `src/ppexpand.c`), omitted-middle-operand `-Wparentheses` for `?:`.
- Fixed: `-Wbool-operation` on truth-valued operands (`bool_operand`); `-Wlogical-not-parentheses` zero-rhs exemption only for `==`/`!=`; `-Wunused-but-set-variable` (statement-expression value is a use, comma left operands, `__real__`/`__imag__` stores, block-scope `extern`); attributes among pointer qualifiers apply to the declaration (`decl_attrs`); noreturn/naked functions skip "no return statement"; `-Wswitch` skips `unused` enumerators; `-Wtautological-compare` skips constant indices anywhere in the access chain and now covers function names.
- Gotcha: a `-W` option missing from `src/diag.c` is silently enabled by default; register new options there (DO_ALL for -Wall members).
- Golden `tests/check/misc_64.c` (SAME against gcc-13).
- Baselines: suite 1077/0; gcc.dg 3711 (164 differ); c-c++-common 598 (59); -O 3711 (164); san 363 files / 0 findings; icount 3.610G (+0.3%).

## Round 66 — gcc.dg/c-c++-common single-rule triage (continued)
- String-literal comparison (`-Waddress`): null pointer constants cast to pointer types exempt (`null_valued`).
- `-Wswitch-enum` out-of-enum `case` warns without `-Wswitch`; type name omitted when the switch expression is a cast.
- `-Wfloat-equal` for `==`/`!=` and truth-value contexts (float, complex twice).
- `-Wunsuffixed-float-constants` (registered in `diag.c`).
- `-Wenum-compare` sees enumerators through `orig_type`.
- PR c/67730: `exp_if_system()` maps a system-header macro location (NULL) to its expansion point for conversion diagnostics (`conv_diag`) and `'return' with a value` in void functions.
- Golden: `tests/check/misc_66.c`. Gate: 1079 pass, san 364/0, gcc.dg differ 154 (was 164), c-c++-common 58 (was 59).
- Not fixed: location-less `cc1:` narrowing-cast warning; `a == __builtin_inf()` float-equal.
- Remaining gcc.dg candidates: discarded-qualifiers noreturn fn-pointer (assign-warn-1/2, pr56724-2), pr53037-* (if-not-aligned), sso-11/13, spec-barrier-3 (builtin return type), Warray-parameter-10, Wbuiltin-declaration-mismatch-*, warn-strlen-no-nul, pr82167, pr68412-2, pr81779, pr23165, pr62090, unused-3.

## Round 67 — gcc.dg tail (continued)
- `noreturn` on a pointer-to-function declaration/typedef qualifies the function type (`-Wdiscarded-qualifiers` "qualified function pointer from unqualified"): assign-warn-1/2, pr56724-2.
- `-Wsizeof-array-argument` sees through `*&x` (pr82167).
- `__builtin_speculation_safe_value` returns the first argument's type (spec-barrier-3).
- Golden: `tests/check/misc_67.c`. Gate: 1081 pass, san 365/0, gcc.dg differ 149 (was 154), c-c++-common 58.
- Optimize-pragma gaps (repeat order, combined strings): no corpus file exercises them; left open, low value.

## Round 68 — strlen over rows, -Wif-not-aligned / -Wpacked-not-aligned, bidi survey
- `-Wstringop-overread` on `strlen` of a row of a const 2-D char array (`b[3]`, `&b[3][1]`, `?:` of them): `StrInit` now records the byte image of `const char t[][N] = {"..",..}` (`note_strinit_rows`); `SlRes.base` offsets the row. One warning per call; the `?:` location uses `cexpr_colon_loc`. warn-strlen-no-nul 68 -> 47 missing.
  Still open there: struct/array-of-struct member images (`s.b`, `ba[0].a[0].b`; needs a byte image of struct initializers) and gcc's optimizer constant propagation of locals (`b[i3]`).
- `-Wif-not-aligned` (default on) and `-Wpacked-not-aligned`: `Attrs.wina_al`, `FieldIn.wina`, side tables `wina_td`/`wina_rec` (typedef / record warn_if_not_aligned), `wina_check` in `struct_finish`. Struct-level "alignment N of 'struct T' is less than M" first (at the closing brace when it starts its line, else the tag), then per-field "offset". Bit-field with a wina type is an error. pr53037-1..4 SAME; golden attr_wina.expected regenerated (it recorded the gap), new golden misc_68.
- Gate: 1083 pass, san 366/0, gcc.dg differ 145 (was 149), c-c++-common 58.
- Survey of the parked cross-chunk bidi fix: of the 8 differing Wbidi-chars files only a minority is chunking. 24/25 (42 diags) are `\u{...}` / `\N{...}` escapes in C (gcc: "delimited escape sequences are only valid in C++23", "named universal character escapes ..."); 4/5/6 (~21) are UCNs inside identifiers (`b\u202a` lexed as `b` + stray; "unknown type name 'b'"); 11 is the UCN spelling in an error (`\U0000202c` vs `\u202C`); 6 has one location difference. So the escape/identifier UCN work is worth more than the cross-chunk stack.

## Round 69 — parse recovery after declarator errors, UCN spelling

- File scope: after "expected '=', ',', ';', 'asm' or '__attribute__' before X", gcc does not skip; it re-parses from X (so `int a b c;` also reports "unknown type name 'b'", `b c d;` stays silent after it). `parse.c`: no `sync_top` there at file scope; the unknown-type error keeps a pending error and bypasses the one-error-per-place dedup (error_at, not c_parser_error).
- Parser "before 'ident'" messages spell extended identifiers as `\UXXXXXXXX` (`cident_ucn`), like the checker (C-locale model).
- Bidi warnings inside literals precede checker diagnostics on the same token (`oloc` set to the literal start in `bidi_ctx`).
- `u'\U00064321'` is two UTF-16 units: "too long for its type". Unknown `mode(x)` errors (lowercase / extended names only; upper-case modes are not validated). Attribute names and asm `%[\u00c3]` operands use the `\U` spelling; asm templates decode UCNs to UTF-8.
- Golden `misc_69`. Gate: 1085 pass, san 367/0, gcc.dg differ 139, c-c++-common 55.
- Gotcha: including `c/check_int.h` in `parse.c` silently changed parser behaviour; declare `cident_ucn` locally.
- Not done: libcpp UCN validation (`not a valid universal character`, incomplete, codespace), `\u{}`/`\N{}`, UCNs in identifiers per `_cpp_valid_ucn` (rules recorded in Round 68 research).

## Round 70 — UCN validation, `\u{}`/`\N{}`, faster gate

- Literals: `\u`/`\U` values validated like `_cpp_valid_ucn` ("is not a valid universal character": < 0xA0 except $ @ `, surrogates, >= 0x80000000); `\u{..}`/`\U{..}`/`\N{NAME}` (bidi names only) parsed, with the C++23 pedwarns after the closing brace; one report per token (`ucn_seen`, keyed by token location).
- Identifiers: UCN validity checked at every `-std=c99` (not only `-pedantic`); bidi `\N{NAME}` recognised in strings/comments and as a stray `\` token (gcc's repeated `forms_identifier_p` lexing, so the plural message matches).
- Lookahead heuristic: a literal's bidi warning is placed at the previous token's start (gcc lexes it as lookahead) — Wbidi-chars-12/13 now SAME.
- Wbidi-chars-4/5/11/12/13/25 SAME. Left: 24 (pp-number "invalid suffix" in recovery), 6 ("unpaired" location 78:50 vs 79:4). gnu99 `\u{}` in identifiers not done.
- Golden `misc_70`. Cells-mode gotcha: lexer diagnostics are emitted before the unit's checker ones, so a golden with a lexer error after checker diagnostics differs between plain and `--cells`; keep lexer errors first in the file.
- Gate speed: `tests/run.sh` shards the parse and check golden loops (`par_goldens`, `GOLDEN_JOBS`, default 4); `bench/tools/gate.sh` overlaps run/verify/san/callgrind within 12 threads (`SAN_JOBS`, `SAN_PY_JOBS`, `CEREAL_JOBS`): 3m01 wall (was >10 min serial). Callgrind uvloop 3.631G. Inner loop: `sh tests/run.sh` alone.
- Gate: 1087 pass, san 368/0, gcc.dg differ 139, c-c++-common 54.

## Round 71 — struct byte image for strlen, `unused` typedefs, labelled empty bodies

- `warn-strlen-no-nul` SAME (was 47 diagnostics short): a const struct / array-of-structs object with a positional brace initializer gets a byte image (`si_fill`, `StrInit.agg`; char-array members copied from string literals, the rest zero; designators and unions bail). `sl_obj` resolves `obj.m`, `obj[K].m`, `&obj.m[K]` (constant K only: gcc cannot place `&obj.m[var]`) to image offsets; `SlRes.mem` marks the `&obj.m[K]` path, which skips the array-bounds check. Struct images are ignored by the format-string and whole-array paths.
- Known gap: `&ba[0].a[1].a[v0]` (variable index through an array of structs) warns in gcc, not here.
- `unused-3`: a variable of a typedef declared `unused` is not warned about (TREE_USED of the type) via `Spec.attrs.unused`.
- `pr23165`: `if (c) label: ;` / `else label: ;` / `do` — labels are looked through before the -Wempty-body test (c_parser_if_body).
- Goldens `misc_71`, `misc_72`. Gate: 1091 pass, san 370/0, gcc.dg differ 136, c-c++-common 54, callgrind 3.632G.
- Remaining gcc.dg tail, triaged: `Warray-parameter-10` (a block-scope extern's prototype must not carry into a later file-scope `void gia ();`), `pr68412-2` (self-comparison of statement expressions), `pr62090` (gnu_inline sprintf nonnull), `pr81779` (needs -std=c90), `Warray-parameter-11`, `Wbuiltin-declaration-mismatch-3`.

### Round 71 addendum — cells-mode order of lexer diagnostics

- From cells the whole file is lexed before the first unit, so lexer/preprocessor diagnostics used to print before every checker diagnostic. `main.c` now holds them back and releases each with the unit whose tokens reach it (location < the lookahead token, else < the end of the unit's last token), ahead of that unit's parser diagnostics; leftovers are appended after the last unit. This removes the "keep lexer errors first" golden workaround noted in Round 70: `misc_70` is back in natural file order and identical plain / `--cells`.
- Approximation: release is by location, so diagnostics from included files compare by global `SrcLoc`, not lexing time. fuzz_cells / lsp transcripts pass.
- Gate: 1091 pass, san 370/0, gcc.dg differ 136, c-c++-common 54, callgrind 3.631G.

## Round 72 — block-scope extern prototypes, numbers lexed but never read

- `Warray-parameter-10` SAME (PR c/102759): a file-scope `void f ();` after only block-scope declarations of f does not inherit their prototype (`pushdecl`: the entity's type becomes the unprototyped one).
- `Wbidi-chars-24` SAME: gcc classifies a pp-number when it lexes it, so a malformed one the parser never reads is still diagnosed. `classify_num` (parse.c) reports "invalid suffix / digit" for the token an error names as current (before that error) and for tokens skipped in recovery (`skip_tok` in sync_stmt / skip_until / sync_top); `lit_report` drops an identical error already at that location. Errors only, with the x86_64 classifier (messages are target independent).
- Golden `misc_73`. Gate: 1093 pass, san 371/0, gcc.dg differ 135, c-c++-common 53, callgrind 3.632G.
- Not pursued: variable-index `&obj.m[v]` through nested structs / arrays of structs. gcc-13 warns for `&cc.x.b[v0]` and `&sa[0].b[v0]` but not `&s.b[v0]`, `&sa[1].b[v0]`, `&dd.y[0].b[v0]` (all const objects of the same shape): no rule visible from the source forms — it depends on how the address folds. Left as is.

## Round 73 — c-c++-common triage (enumerators, overflow builtins)
- Deprecated/unavailable enumerators now carry `CSF_DEPRECATED`/`CSF_UNAVAILABLE` + message (`enumerator_visit`), so uses warn (`attributes-enum-1` SAME). Golden `misc_74`.
- `-Wnonnull` for a null 3rd argument of generic `__builtin_{add,sub,mul}_overflow` (`builtin_nonnull` mask 4). `builtin-arith-overflow-1` SAME bar a cmp.sh `-Wall` artifact. Golden `misc_75`.
- Gate: 1097 goldens pass; san 373/0; gcc.dg differ 135; c-c++-common differ 52 (was 53); callgrind 3.633G.
- Still open from triage: `builtin-has-attribute` (wording of lines 33-35, missing errors 42-44); `pr84999` needs `typeof`.
- Not started: gcc.dg tail (`nofixed-point-2`, `diag-aka-5a/5b`, `overflow-warn-8`, `init-excess-2`); cells-mode release heuristic with includes.
- `__builtin_has_attribute` (Round 73b): "expected identifier" wording; `unknown attribute 'X'` error (`cdecl_attr_known`); non-power-of-2 `aligned(N)` error; `alloc_size`/`alloc_align` on a non-function operand warns. Golden `misc_76`. Gate: 1099 pass, san 374/0, dg 135, c-c++-common 52, callgrind 3.633G.
- Still open in `builtin-has-attribute`: `aligned(i)` non-constant error (line 56) and the array-element alignment error location inside a type-name (gcc reports at statement start, we at the size expr).

## Round 74 — gcc.dg tail
- Fixed-point literals (`0r`, `0.5hr`, `1ulk`, ...): `-pedantic` "fixed-point constants are a GCC extension" then "fixed-point types not supported for this target" (`LIT_FIXED`, `fixed_lit`); bare `_Sat` errors ("'_Sat' is used without ..."). `nofixed-point-2` SAME. Golden `misc_77`.
- aka: a system-header typedef of a vector type keeps its name (`aka_atomic`); `diag-aka-5a/5b` still differ only on `__attribute__((transaction_unsafe))` in function-pointer types (not modelled). Golden `misc_78` (via `<xmmintrin.h>`).
- Overflow warnings issued at c_fully_fold time (operand not yet constant, e.g. through a `(double)` cast) print the expression: `expression '1 + 2147483647' of type ...` for binary nodes (`fold_flush`). `overflow-warn-8` SAME.
- `pedwarn_init`/`warning_init` (`iped`, `iwarn`, designated-init): a token spelled in a system header macro (NULL) is reported at the macro's expansion point (`wloc`), so it is not dropped. `init-excess-2` SAME. Golden `misc_79`.
- Gate: 1105 goldens pass; san 377/0; gcc.dg differ 132 (3743 identical); c-c++-common differ 52; callgrind 3.634G.

## Round 74b — cells mode with #include
- Multi-file `--cells` probe (`misc_80`, `misc_81`) found two bugs, both fixed:
  1. The cells hold-back/release compared SrcLocs numerically, but an included file's locs are numbered after the main file's. `main.c: pre_before` now orders a header's diagnostics by where its outermost #include sits (inc_chain), falling back to file load order inside a header.
  2. "In file included from" was missing for diagnostics reported by the parser/checker in cells mode (no live pp). `SrcFile.inc_loc` is now set in `push_file`, and `diag_vreport` derives the chain from the diagnostic's own location (scratch/virtual locations still use the pp's live chain).
- Note: the pp front end's option tags differ from gcc's (`[-Wextra-tokens]`, `[-Wpp-warning-directive]` on `#warning`, which gcc-13.4 words "before C2X is a GCC extension" under -pedantic); the pp goldens bake this in, so misc_80/81 use `#undef X junk`.
- Gate: 1109 pass, san 379/0, dg 132, c-c++-common 52, callgrind 3.634G.

## Round 75 — pp wording, empty declarations
- verify compares (line, col, level, message) without option tags, so the pp's own tags (`[-Wextra-tokens]`, `[-Wpp-warning-directive]`) are invisible there; the *wording* was not: `#warning before C2X is a GCC extension`, `#assert/#unassert is a GCC extension` (under -pedantic; else the -Wdeprecated one), `#include_next is a GCC extension` (also in the primary file, before the "in primary source file" warning). Golden `misc_83`.
- `shadow_tag`: a declaration with no type specifier whose only content is a useless storage class / qualifier / `_Alignas` / `__thread` also gets "empty declaration". `declspec-4` SAME. Golden `misc_82`.
- Tried and dropped: `X Y(Z);` with unknown `X` should still give "parameter names (without types)"; the checker is quiet for units with a parser error, so it needs that policy changed (`pr14963`).
- Gate: 1113 pass, san 381/0, dg 131, c-c++-common 52, callgrind 3.633G.
- Open one-diff gcc.dg files worth a look: `20050209-1` (parser recovery after `return 1);`), `for-1`, `attr-copy-3/5`, `Wbuiltin-declaration-mismatch-8/16`, `pr15698-1/6`, `parm-impl-decl-2`.

## Round 76 — function-type attributes in the type (nocf_check, transaction_unsafe)
- `TF_NOCF`/`TF_TXUNSAFE` (type.h bits 64/128) on function `TypeEnt`s. `declared_visit`/`funcdef_declared` read the attributes from the declspecs (functions only) and the declarator tree (`fn_attr_walk`, through N_PTR/N_FUNC/N_ARRAY) and rebuild the first function type under the pointers (`fn_attr_type`, keeps typedef'd return/params).
- `type_print` shows `__attribute__((nocf_check|transaction_unsafe)) ` before the `*` of a pointer to such a function. `type_compatible` compares only `TF_NOCF` (gcc ignores transaction_unsafe); `type_composite` and the incomplete-return rebuild carry both.
- Closes gcc.dg `diag-aka-5a/5b` (golden `misc_84`). Gate: 1115 goldens, san 382/0, gcc.dg differ 129, c-c++-common 50, callgrind uvloop ~3.643G.
- Open: `attr-nocf-check-1/3` need the "'nocf_check' attribute ignored. Use '-fcf-protection' option to enable it" warning when `-fcf-protection=none` (option not modelled).

## Round 77 — -fcf-protection and the nocf_check ignored warning
- Driver: `-fcf-protection[=full|branch|return|check|none]`, `-fno-cf-protection` set `cf_nobranch` (none/return) -> `Checker.opt.cf_nobranch`. With it, `nocf_check` warns "'nocf_check' attribute ignored. Use '-fcf-protection' option to enable it" (at the declaration's first specifier token; for a parameter at the function's first token) and is left out of the type (`nocf_ignored` in cdecl.c). Default stays "enabled" like Ubuntu gcc-13, where a `nocf_check` declaration followed by a plain definition is "conflicting types" (matched).
- `attr-nocf-check-1/3` are SAME under their `-fcf-protection=none` and under the default. Goldens `misc_85` (none) and `misc_86` (full). Gate: 1119 goldens, san 384/0, gcc.dg differ 129, c-c++-common 50, callgrind ~3.647G.
- Note: `verify.sh` does not read `dg-additional-options`, so those two files were never in the differ list; their real-flag behaviour is only checked by cmp.sh.

## Round 78 — small c-c++-common files
- Unknown attribute `__int128__` prints as `'__int128'` (gcc keeps the keyword spelling). `-Wunused-value`: complex arithmetic whose operands differ in type (`x + 1`, `d - x`, `fz + x`, `x / d`; not `d / x`, not constant-vs-constant complex) says "value computed is not used" (`mixed_complex`, cstmt.c; PR c/97748). `int printf;` -> "built-in function 'printf' declared as non-function" (`cexpr_builtin_nonfn`). `va_arg` first-argument error is located at the type name. `-Wswitch-bool` also fires for a bool switch with a default after labels covering both 0 and 1. `sentinel` attribute: "only applies to variadic functions", "requested position is not an integer constant / less than zero". Golden `misc_87`.
- Gate: 1121 goldens, san 385/0, c-c++-common differ 50 -> 43; callgrind ~3.647G.
- Looked at and left: `pr70756` ("array with unspecified bounds" vs "flexible array member" needs a flex-member bit on array types), `pr97164` (struct-size alignment error located at the tag), `pr51628-8/9/16` (packed-member warning column differs for call arguments and casts: gcc uses the `->`/cast token).

## Round 79 — "parameter names (without types)" in an erroring unit
- `grokparms` now reports the K&R-list pedwarn even when the unit is quiet (a parser error such as `unknown type name 'X'`), ordered after the parser's diagnostics (`ORD_LATE`): gcc emits it from the declarator parse itself. Closes gcc.dg `pr14963`; golden `misc_88`. A small whitelist exception to the quiet policy; the policy itself is unchanged.
- Gate: 1123 goldens, san 386/0, gcc.dg differ 126, c-c++-common 43, callgrind ~3.647G.

## Round 80 — -Waddress-of-packed-member locations
- `packed_ptr_check_x` (cexpr.c) takes a mode. Context (assignment/init/argument) looks through a cast and reports a member whose record/field is packed at the cast's `(`; the cast itself reports only a member whose *type* is packed (at the `&`); a parenthesized cast operand (`(T*)(&p.i)`) is located by the cast site as before. A bare member passed as a call argument is located at its `->`/`.` (not through `?:`). `pk_member` now returns which clause matched. Goldens: `misc_89` (probe of all forms against gcc).
- Closes c-c++-common `pr51628-8/9/16`. Gate: 1125 goldens, san 387/0, gcc.dg differ 126, c-c++-common 42, callgrind ~3.654G.
- Note: a one-off "1 failed" appeared in the gate's san step under load and did not reproduce (san.sh and run.sh clean on rerun); keep an eye on the `--cells -fparallel-chunk=1` runs.

## Round 80 — -Waddress-of-packed-member locations
- `packed_ptr_check_x` (cexpr.c) takes a mode. Context (assignment/init/argument) looks through a cast and reports a member whose record/field is packed at the cast's `(`; the cast itself reports only a member whose *type* is packed (at the `&`); a parenthesized cast operand (`(T*)(&p.i)`) is located by the cast site as before. A bare member passed as a call argument is located at its `->`/`.` (not through `?:`). `pk_member` now returns which clause matched. Golden `misc_89` (a probe of all forms against gcc).
- Closes c-c++-common `pr51628-8/9/16`. Gate: 1125 goldens, san 387/0, gcc.dg differ 126, c-c++-common 42, callgrind ~3.654G.
- Note: a one-off "1 failed" appeared in the gate's san step under load and did not reproduce (san.sh and run.sh clean on rerun); watch the `--cells -fparallel-chunk=1` runs.

## Round 81 — verify.sh honours dg-additional-options
- `bench/tools/par.py` (CEREAL_DGOPTS=1) now merges every selector-free `dg-additional-options` into the test's flags (a selector skips the file, as for dg-options); `-fcf-protection*` is an accepted option. Files whose options cereal lacks are skipped, so the denominators shrink: gcc.dg 3875 -> 3744 files run, c-c++-common 657 -> 636; differ 126 / 41 (was 126 / 42). Baseline for the gate: gcc.dg 126, c-c++-common 41 (gate.sh header updated).

## Round 82 — flexible array member vs unspecified bounds
- A struct's `[]` member gets `type_array_flex` (`TF_INCOMPLETE|TF_FLEX`; TF_FLEX shares bit 64 with TF_NOCF, which only applies to function types). The canonical form and `type_composite` keep the bit. `incomplete_error` says "invalid use of flexible array member" only for those; any other incomplete array (`int (*A)[]`, typedef'd `T[]`) says "invalid use of array with unspecified bounds", as gcc does (it keys on TYPE_DOMAIN). Closes c-c++-common `pr70756`; golden `misc_90`.
- Gate: 1127 goldens, san 388/0, gcc.dg differ 126, c-c++-common 40, callgrind ~3.654G.
- Fix-up (same round): a typedef'd `T[]` struct member is also a flexible member for gcc (finish_struct), so the last field of a struct is rebuilt with `type_array_flex` (loses the typedef name). `misc_90` now matches gcc line for line (the first commit of this round had a golden with one wrong line). Gate: 1127 goldens, san 388/0, gcc.dg differ 124, c-c++-common 40, callgrind ~3.654G.

## Round 83 (re-baseline, flake check, portfolio tidy)
- Open lists regenerated (verify.sh): gcc.dg 124 differ of 3744 (40 one-diff,
  41 two-diff, 43 with 3+); c-c++-common 40 of 636 (12 / 14 / 14).  The
  one/two-diff files are mostly distinct small causes (locations, attribute
  conflicts, wording of range/overflow notes), not a shared bucket.
- `--cells -fparallel-chunk=1` golden flake: not reproduced in 7 full
  `tests/run.sh` runs (1 idle, 6 with four busy loops at nice 19); all 1127
  pass.  Treat the single earlier failure as unexplained until it recurs.
- README parser status refreshed; stray empty `null` file removed.  No
  LICENSE added yet (owner to choose).

## Round 84 (conflict markers)
- A parser "expected ..." error at a token that starts a version-control
  conflict marker (7 x '<', '>' or '=' at line start) now reads "version
  control conflict marker in file", as gcc's c_parser_error does
  (parse.c `conflict_marker`, in `vperr`).  Golden misc_91.  c-c++-common
  differ 40 -> 37.  Gate: 1129 goldens, san 389/0, callgrind 3.654G.
- Open cluster: scalar_storage_order is unimplemented (sso-2/3/11/13,
  Wscalar-storage-order; ~25 diagnostic lines across gcc.dg and
  c-c++-common).  asm-qual-3: gcc errors on 'asm' qualifiers in C99 data
  declarations ("expected identifier or '(' before string constant").

## Round 85 (scalar_storage_order)
- Reverse-order records (RF_SSO) now give gcc's four checks (cexpr.c
  `sso_*`, cdecl.c record finish): `&` of a scalar member/element of reverse
  storage is an error; `&` or decay of a reverse array (elements wider than a
  byte, not records) warns (`sso_decay` at the end of `cexpr_node`);
  pointer conversions between reverse and non-reverse pointees warn before
  the incompatible-pointer warning, except for built-in callees' arguments
  and results of allocators (malloc attribute / alloca; PR c/100920); a
  union member of the other storage order warns "type punning toggles".
  Goldens misc_92..97 (all match gcc).  gcc.dg differ 124 -> 120.
  Gate: 1141 goldens, san 395/0, callgrind 3.666G (+0.3%).

## Round 86 (asm qualifiers)
- Top-level `asm` takes no qualifiers (gcc: "expected '(' before 'volatile'"
  etc.); in a statement `const`/`restrict` give "'X' is not a valid 'asm'
  qualifier" and parsing goes on.  Golden misc_98.  gcc.dg differ 120 -> 118.
- pr89410-1 stays open: after `#line 4294967295` gcc numbers the next line
  -1 (a `#warning` there reads ":-1:2") and words the range warning without
  "(C99 6.10.4p3) [-Wpedantic]"; cereal gives the real line and both
  pedantic notes.

## Round 87 (packed-cast location, binary constants)
- Packed-member check: a cast that changes the pointer type reports at the
  operand (cast site, whether or not the operand is parenthesized); a cast
  that leaves the type alone is looked through by the conversion that
  receives it and reported at the cast (`noop_ptr_cast`).  Goldens misc_100,
  101.  c-c++-common differ 37 -> 35.
- `#if 0b11` under -pedantic-errors is an error (was a warning).  Golden
  misc_99.
- Open: with -ftrack-macro-expansion=0 gcc locates a pp-expression
  diagnostic from a macro body at the macro use (binary-constants-2/3);
  `pp_eval_if` works on already-expanded tokens, so the use location is not
  at hand there.

## Round 88 (built-ins as function pointers)
- gcc names a built-in used as a function pointer value: "from pointer to
  '__builtin_X' with incompatible type ..." (init, assignment), "returning
  pointer to '__builtin_X' of type ..." (return), "pointer type mismatch
  between ... of 'a' and 'b' in conditional expression" (both operands);
  arguments stay plain.  It applies to `__builtin_X` itself (also through
  `&`) and to a library built-in redeclared without a prototype, which keeps
  the built-in's own type (so `void *memset();` is not compatible with a
  `memcpy` function pointer).  A prototyped redeclaration is an ordinary
  function.  `fnref_builtin` (cexpr.c) and `ccall_builtin_ref` (ccall.c);
  cinit.c's `digest` uses the built-in's type, otherwise the compatible-type
  shortcut hid the mismatch.  Golden misc_102.  gcc.dg differ 118 -> 116
  (Wbuiltin-declaration-mismatch-3, pr59630).
- Gate: 1151 pass, san 400/0, gcc.dg differ 116, c-c++-common 35,
  callgrind 3.690G (+0.3%).
- Mirror reminder: `rsync --delete` of src/tests/bench from `~/cereal-t`
  overwrites edits made only in the repo; edit those trees in `~/cereal-t`.

## Round 89 (statement-expression callee location, -Wextra tag)
- A call of a statement expression that is not a function is located (as
  gcc does) at the lone expression or `break`/`continue` inside it
  (`callee_err_loc`); with several statements or a declaration it stays at
  the opening brace.  gcc's location for a parenthesized value is the
  paren, for a call the callee.  pr35742.
- "ordered comparison of pointer with null pointer / integer zero" carries
  `[-Wextra]` (they are warned through the -Wextra umbrella itself).
  ordered-comparison-4.  Golden misc_103.
- Open: for an implicitly declared function `({ f(); })()` gcc locates at
  `f`, cereal at the `(`.
- Gate: 1153 pass, san 401/0, gcc.dg differ 114, c-c++-common 35, callgrind
  3.690G.

## Round 90 (attribute diagnostics)
- access: modes are not compared when a VLA bound implies one (uninit-37).
- noipa acts as noinline: "inline function given attribute 'noinline'" on
  an inline definition or declaration, and the always_inline/gnu_inline
  conflict messages name noinline and drop the right attribute by order
  (attr-noipa).
- 'packed' after a reference to a tag (`typedef struct p __attribute__
  ((packed)) t;`, enums too) warns "'packed' attribute ignored" at the tag
  name (pack-test-3).  Golden misc_104.
- Gate: 1155 pass, 0 sanitizer findings,
  gcc.dg differ 111, c-c++-common 35, callgrind 3.693G.

## Round 91 (attribute group, rest)
- Attributes after a '*' in a parameter declarator, nested ones included,
  belong to the parameter: `void f (int (*__attribute__((used))) (void))`
  warns 'used' ignored (attr-nest).
- copy of a variable copies its common/nocommon, so the exclusion applies
  (note at the referenced variable); copy of a thread variable's tls_model
  onto one without thread storage warns (attr-copy-3, attr-copy-5).
  `copy_target` is shared by both checks.
- alloc_size/alloc_align on a function typedef are checked as on a
  function, including a conflicting redeclaration (attr-alloc_size-13).
- optimize option strings are normalized as gcc does (`no-x` -> -fno-x,
  `x` -> -fx, `2`/`Ofast` -> -O...); "no-lto" is a bad option
  (Wattributes-4).  A first version missed Ofast/O2 and added eight false
  warnings; the gate caught it.  Golden misc_105.
- Open: `int (U *q) (void)` as a parameter, gcc says 'used' does not apply
  to types.
- Gate: 1157 pass, san 403/0, gcc.dg differ 106, c-c++-common 35, callgrind
  3.697G.

## Round 92 (implicit-declaration callee location)
- A statement expression whose lone value is a call of an implicitly
  declared function (maybe parenthesized) is located at its brace, not at
  the call (`implicit_call` in `callee_err_loc`).  Closes the Round 89
  open item.  Golden misc_106.
- Gate: 1159 pass, san 404/0, gcc.dg differ 106, c-c++-common 35, callgrind
  3.697G.

## Round 93 ("used but never defined", block-scope extern inline)
- New `CSF_CUSED` (gcc's C_DECL_USED): a function named only in an
  unevaluated sizeof, alignof or typeof is not "used" for "'f' used but
  never defined"; a VLA sizeof or variably modified typeof evaluates its
  operand, so it counts (gcc's pop_maybe_used; only the outermost wrapper
  decides, a small simplification of its nesting rule).  CSF_USED is
  unchanged, so unused-function warnings behave as before.
  c99-static-1, gnu99-static-1.
- A block-scope `extern inline` function declaration now merges with the
  file-scope function (it is not DECL_EXTERNAL until defined, so pushdecl
  skipped the merge and left a second, undefined symbol).  inline-40/42.
  Golden misc_107.
- Gate: 1161 pass, san 405/0, gcc.dg differ 102, c-c++-common 35, callgrind
  3.701G.

## Round 94 (read-only locations, -Wwrite-strings, comma array sizes)
- An element of a string literal assigned to or incremented warns
  "assignment of read-only location '"foo"[0]'" (no option tag), at the
  literal or a prefix operator (lvalue-5).  Concatenated pieces print
  as written (`"a" "b"`), gcc prints `"ab"`; not done.
- A subscript of a pointer by a conditional with no constant arm prints
  lowered, `*(p + (sizetype)(c ? a : b))` (pr45079).  With a constant arm
  gcc pushes the conversion into the arms; still printed as `p[...]`.
- -Wwrite-strings is implemented: string literals are `const char[N]`
  (new diag row, off by default).  Gives the const-discarding warnings for
  initializations, and `&""` pointer-to-array diagnostics
  (Wwrite-strings-1).  Goldens misc_108, misc_109.
- A comma operator outside a sizeof operand makes an array size not an
  integer constant, so the array is a VLA at block scope (array-5, part).
- Open: compatibility of VLA types ignores constant dimensions
  (`int (*)[4][n+1]` vs `int (*)[6][m]` should be incompatible; type_vla
  keeps no sizes); pr83415 prints a vector subscript as
  `((const short int[8])y)[i]`; Warray-parameter-11 needs gcc's folding of
  builtin calls and address differences.
- Gate: 1165 pass, san 407/0, gcc.dg differ 98, c-c++-common 35, callgrind
  3.703G.

## Round 95: parameter-list recovery

- A parameter that cannot start a declaration is diagnosed, then the list
  continues after a following comma with the error state cleared, as gcc's
  c_parser_parms_list_declarator does (lvalue-11, lvalue-3, qual-assign-7).
  Golden misc_110.
- Open, separate recovery differences: 20050209-1 (`return 1); }`: gcc
  skips with skip_until_found, which stops at the unmatched ')' and then
  reports "expected statement"; switching end_stmt to expect_skip alone
  regressed misc_76 and did not produce it), for-1 (extra error at end of
  input).
- Gate: 1167 pass, san 408/0, gcc.dg differ 95, c-c++-common 35, callgrind
  3.703G.

## Round 96: VLA dimension sizes

- A variable length array type keeps a constant dimension (TF_SIZED, count in
  `n`) when it is an array of variably modified type, and the spelling of a
  variable one (`TypeEnt.extra`, an interned string from the declarator's
  size expression).  Compatibility compares known counts, composite keeps
  them, and the type printer writes `[4][n + 1]` instead of `[*][*]`
  (array-5 line 27, C99 6.7.5.2p9).  Golden misc_111.
- Open: pr83415 vector-subscript spelling; Warray-parameter-11 folding.
- Gate: 1169 pass, san 409/0, gcc.dg differ 94, c-c++-common 35, callgrind
  3.704G.

## Round 97: statement recovery

- A missing ';' skips as gcc's c_parser_skip_until_found does and stops at an
  unmatched ')' or ']' (clearing the one-error-per-place guard); a ')' or
  ']' that starts a statement is "expected statement".  A parenthesised
  condition skips to its ')' after a failed expression, and
  __builtin_has_attribute skips past its ')' after a bad attribute
  (20050209-1, 20031222-1 kept).  Golden misc_112.
- Open: for-1 (extra "expected declaration or statement at end of input").
- Gate: 1171 pass, san 410/0, gcc.dg differ 93, c-c++-common 35, callgrind
  3.704G.

## Round 98: #line in diagnostics

- Diagnostics ignored `#line` and linemarkers (they showed the physical
  file and line).  Each SrcFile now keeps its #line history (`linemap`);
  the location line, the "In file included from" chain and the source
  snippet use the presumed name and line.  Presumed line 0 prints no
  position (`q.c: error: ...`); the snippet is read at the presumed line
  and shown only when the presumed file is the file itself and the line
  exists, as gcc does.  for-1 and one more gcc.dg file now match.
  Golden misc_113; builtin_location regenerated (its `#line 20` moved every
  line after it).
- "style of line directive is a GCC extension" stays untagged: gcc 13 does
  not print `[-Wpedantic]` (newer gcc does, so the local `gcc` is not the
  reference here).
- Open: pp diagnostics are printed before parse errors of earlier lines;
  one linemap per file, so a file included twice with different #line
  histories uses the newest entries.
- Gate: 1173 pass, san 411/0, gcc.dg differ 91, c-c++-common 35, callgrind
  3.704G.

## Round 99: declaration recovery

- A `({` outside a function body is an error at the `(` from the parser
  (gcc: `!building_stmt_list_p`); the group is skipped through `}` and `)`,
  and the node is an erroneous expression, so the array bound is not
  "assumed to have one element" (20041014-1, pr84721).
- `typeof` after a type specifier ends the specifiers (`int typeof;` is
  "expected identifier or '(' before 'typeof'"; no-asm-3).
- Attributes between the declarator and the `{` of a function definition:
  "attributes should be specified before the declarator in a function
  definition" at the declaration start, body skipped (pr60915).
  Golden misc_114.
- Gate: 1175 pass, san 412/0, gcc.dg differ 87, c-c++-common 35, callgrind
  3.704G.

## Round 100: diagnostic order of ?:

- The omitted-middle-operand warnings (`-Wpedantic`, `-Wparentheses`) are
  reported by gcc's parser before it reads the third operand; they now
  precede the third operand's own warnings (Wduplicated-branches-9,
  pr70144-1).  Golden misc_115.
- The verify "differ" counts ignore diagnostic order, so they did not move
  (gcc.dg 87, c-c++-common 35).  Order-only differences left: binary-constants-1,
  init-bad-4 (parse error before the initializer's warnings), builtins.c
  (built-in prototype warning before the 'format' attribute one),
  pr89888 (case range warnings).
- Gate: 1177 pass, san 413/0, callgrind 3.704G.

## Round 101: #if expression errors

- A string in an #if expression is "token "..." is not valid in
  preprocessor expressions" (with the token text).  A value wanted where an
  operator or the end came follows gcc's _cpp_parse_expr: "operator 'X' has
  no right operand" (X the operator before), "missing expression between
  '(' and ')'", "missing '(' / ')' in expression", "operator 'X' has no left
  operand"; "'?' without following ':'" is at the current token.  Checked
  against gcc-13 (`gcc-13` exists in WSL; plain `gcc` is 15).  Golden misc_116.
- Open: a bare `#if` reports at column 2 (gcc: after the directive name);
  `u8"x"` in #if lexes as an identifier and a string in -std=c99.
- Gate: 1179 pass, san 414/0, gcc.dg 87 / c-c++-common 35 differ.

## Round 102: flexible-array strings, char subscripts, missing include

- A string for a flexible array member that is stored out of order (a
  designator that skips the earlier fields: `{ .b = "x" }`) is digested again
  when gcc outputs the pending elements, so it gets a second "initialization
  of a flexible array member" at the declaration's last struct tag, with the
  "(near initialization for 'd')" note (c99-flex-array-7, -typedef-7).
- `-Wchar-subscripts` is located at the `[`; a missing include reads
  "NAME: No such file or directory" (strerror's wording).  Golden misc_117.
- Open: after "flexible array member in a struct with no named members" cereal
  drops the field, so a later `.b = ""` errors; gcc keeps the erroneous field
  and stays silent.  The note after an out-of-order `.b = "x", .a = 1` names
  `e1`, gcc `e1.a`.
- Gate: 1181 pass, san 415/0, gcc.dg 83 / c-c++-common 35 differ, callgrind 3.704G.

## Round 103: erroneous flexible member, typeof side effects, bare #if

- A flexible array member in a struct with no named members is kept (gcc
  keeps the erroneous member), so `q->b` and designators on it no longer add
  "no member named"; the member's type is erroneous, so initializers for it
  are silent too (golden misc_119).
- A cast to a typeof whose type is not variably modified drops the side
  effects of the typeof operand (gnu99-const-expr-3/4).
- A bare `#if` / `#elif` is reported at the end of the line, after blanks
  and comments (gcc's EOF token).  `u8"x"` in #if already matched.
- Golden misc_118.  Gate: 1185 pass, san 417/0, gcc.dg 81 / c-c++-common 35.

## Round 104: empty declarations, __auto_type, _Imaginary

- `__auto_type` misuse follows c_parser_declaration_or_fndef: "requires a
  plain identifier as declarator", "requires an initialized data
  declaration", "may only be used with a single declarator" are errors at the
  declaration's first token, and the rest of the declaration is skipped;
  "used with a bit-field initializer" is checked at the initializer;
  "'__auto_type' in empty declaration" replaces "useless type name".
- A qualifier on a tag of the wrong kind (`const struct u3;` with a union
  u3) is a reference, so it gets "does not redeclare tag" plus "defined as
  wrong kind of tag", not "useless type qualifier".
- `_Imaginary` is a reserved word that cannot be a specifier: "expected
  identifier or '(' before '_Imaginary'".
- A block-scope declaration whose declarator failed to parse (`double ) z;`)
  no longer also reports "useless type name in empty declaration".
- Golden misc_120 (auto-type-2, c99-tag-3, c99-complex-3 now identical).
- Gate: 1187 pass, san 418/0, gcc.dg 78 / c-c++-common 35, callgrind 3.706G.

## Round 105: statement recovery after a stray bracket

- A `)` or `]` where a statement should start is reported ("expected statement") and consumed, and the block goes on with the next item (`) y;` then reports `y` undeclared), as gcc's compound-statement loop does.  Golden misc_121.
- Gate: 1189 pass, san 419/0, gcc.dg 78 / c-c++-common 35 (no file changed).

## Round 106: old-style definitions against prototypes and built-ins

- An enum as wide as int is not promoted (c_type_promotes_to), so an
  old-style `void f(x) enum e2 x;` after `void f(enum e1);` is "argument 'x'
  doesn't match prototype" (enum-compat-1).
- An old-style definition of a library built-in (`char *strchr(a) const char
  *a;`) is checked against the built-in's prototype: warnings "number of
  arguments doesn't match built-in prototype" (at the name) and "argument
  'a' doesn't match built-in prototype" (pr15698-1/-6).
- A built-in with a FILE * parameter takes the first FILE type declared; a
  later declaration with another struct warns "mismatch in argument N type"
  under -Wextra (Wbuiltin-declaration-mismatch-8).
- The "'X' is declared in header" note is at the declaration (gcc-13), not at
  the include position, and names the __builtin_ spelling when used.
  Goldens attr_fmt_noproto, builtin_decl_mismatch regenerated (notes only).
  Golden misc_122.
- Open: (-12's `__clear_cache` is fixed in Round 107.)
- Gate: 1191 pass, san 420/0, gcc.dg 75 / c-c++-common 35, callgrind 3.706G.

## Round 107: declarations with a bad parameter list

- A syntax error inside an array bound in a parameter no longer drops the
  declaration (gcc keeps it with a `<type-error>` parameter; redeclaration
  notes print `int(int,  <type-error>)`): `Parser.bound_errors` separates
  those errors from a failed declarator.
- The built-in mismatch warning (and header note) is now issued for such a
  declaration even though its unit has an error, after the parser's error
  (ORD_LATE), as gcc does (Wbuiltin-declaration-mismatch-16). Golden misc_123.
- Gate: 1193 pass, san 421/0, gcc.dg 74 / c-c++-common 35, callgrind 3.706G.
- `__clear_cache` is a built-in only without -std=c99 (gnu = 1 in
  cbuiltin_tab.h), so a c99 redeclaration is silent (Wbuiltin-declaration-
  mismatch-12). Golden misc_124. Gate now: 1195 pass, san 422/0, gcc.dg 73.

## Round 108: duplicate pointer qualifiers

- -Wduplicate-decl-specifier now also covers qualifiers after '*' and inside
  array parameter brackets (`char *restrict restrict`, `a[const const 3]`),
  at the second qualifier, silent when either comes from a macro
  (quals_of_warn). Wduplicate-decl-specifier.c now matches. Golden misc_125.
- Gate: 1197 pass, san 423/0, gcc.dg 72 / c-c++-common 35, callgrind 3.705G.

## Round 109: four one-line differences

- -Wc++-compat: `extern const T x = v;` is not "initialized and declared
  'extern'" (Wc++-compat).
- Built-ins with a struct tm * parameter (strftime) fix their own type apart
  from FILE * (`bt_fileptr[2]`): Wbuiltin-declaration-mismatch-11.
- __builtin_choose_expr with an overflowing constant condition: "overflow in
  constant expression" at the line's first token (gnu99-const-expr-2).
- A bit-field of a forward-referenced enum is always "narrower than values of
  its type" (pr14475).
  Golden misc_126. Gate: 1199 pass, san 424/0, gcc.dg 69 / c-c++-common 35,
  callgrind 3.705G.

## Round 110: typedef notes and four small gcc.dg files

- A redefinition note names an object's qualified typedef type by the typedef
  alone (`CI {aka const int}`, `type_q_decl`); inside a pointer the
  qualifier stays. Golden misc_127.
- -Wdeclaration-after-statement on a declaration starting with a system
  header's macro (`bool`) is reported at the expansion (pr81779).
- `&&label` outside a function: the -Wpedantic warning comes before the error
  (parm-impl-decl-2).
- A function initialized like a variable after it was defined also gets
  "invalid initializer" at the initializer (pr64766). Open: the
  "prototype follows non-prototype definition" warning is still missing when
  such a redeclaration is initialized. Golden misc_128.
- Gate: 1203 pass, san 426/0, gcc.dg 66 / c-c++-common 35, callgrind 3.705G.

## Round 111: visibility conflicts, prototype after an old-style definition

- A redeclaration with a different explicit visibility warns "redeclaration
  of 'X' with different visibility (old visibility preserved)" with the
  previous-declaration note (visibility-7). It compares the oldest
  `visibility` in the symbol's attribute set with the new declaration's;
  `Checker.vis_old` keeps the symbol as it was before merge_decls.
- `prototype follows non-prototype definition` also for a `()` definition
  (the check required parameters), and the "invalid initializer" of an
  initialized redeclaration of a defined function now comes after that
  merge's diagnostics. Goldens misc_129, misc_130.
- Not attempted (need the optimizer or symbolic offsets): invalid-call-1 and
  pr62090 (-O2 inlining), pr83844 (-Wif-not-aligned on a VLA struct offset).
  gcc also prints a bare "previous definition" note after a prototype whose
  parameter is erroneous (proto-1); notes are not compared by par.py.
- Gate: 1207 pass, san 428/0, gcc.dg 65 / c-c++-common 35, callgrind 3.706G.

## Round 112: five single-line gcc.dg differences

- va_arg of a function type: "is a function type" (pr105149).
- `*&&label` is not "dereferencing 'void *' pointer" (comp-goto-4).
- A conflicting redeclaration of an array with `= {}` leaves the old
  declaration: no "zero or negative size array" (pr94726;
  `Checker.redecl_failed`).
- After "variable-sized object may not be initialized", the rest of the
  initializer gives no "excess elements" (pr93577-1; `CCtx.varerr`).
- `volatile` written on a function declaration agrees with an earlier
  `noreturn` (gcc's noreturn is a volatile function type; noreturn-5).
  Golden misc_131.
- By design, not changed: a missing #include is an error and the unit goes on
  (gcc: fatal, stops; only -E stops, `fatal_missing_include`), so pr89434 and
  files like it differ in severity.
- Gate: 1209 pass, san 429/0, gcc.dg 60 / c-c++-common 35, callgrind 3.706G.

## Round 113: bit-field type names, vector subscripts, incomplete enum casts

- Pointer-to-integer assignment to a bit-field names the bit-field type
  (`signed char:4`; pr70174); `bf_tstr` is shared with -Wsign-compare.
- A signed bit-field assigned `c ? K : x` with the true arm out of range gets
  gcc's "overflow in conversion" naming the whole conditional as written
  (`cexpr_str_plain`; pr35635).
- A vector subscript prints as gcc's array view,
  `((const short int[8])y)[i]` (pr83415).
- `(enum T) x` with `enum T` incomplete: "conversion to incomplete type" at
  the tag, no sizeof error (pr101171; `cast_tag_loc`).
- Not changed: pr100547 (gcc places the vector-components error at the last
  sizeof of a long product, at the closing paren otherwise; cereal uses the
  typedef start for both).
- Golden misc_132. Gate: 1211 pass, san 429/0, gcc.dg 56 / c-c++-common 35,
  callgrind 3.708G.

## Round 114: float to bit-field conversions

- A real value assigned to a bit-field names the bit-field type
  (`unsigned char:3`) in the -Wfloat-conversion / may-change-value messages,
  and an out-of-range constant gives gcc's -Woverflow saturated to the field's
  width (9.5 to 7). `float_to_int_bits` takes the width; `conv_arith` sets
  `uc_bw` for real sources too. Golden misc_133.
- Gate: 1213 pass, san 431/0, gcc.dg 56 / c-c++-common 35, callgrind 3.708G.

## Round 115: four more gcc.dg files

- `extern const T x = v` is exempt from "initialized and declared 'extern'"
  only under -Wc++-compat (Wno-c++-compat.c).
- A function designator under a cast prints as its address,
  `(long int (*)(int))&foo` (call-diag-1).
- The element type of a declared array keeps its typedef name for
  initializer messages (`'A' has no member named 'D'`; c99-init-2).
- "ISO C forbids forward parameter declarations" sits at the first token of
  the line holding the ';' (pr68533).
- Not changed: concat2 (a string of 100000 pieces: `Node.aux` is 16 bits and
  saturates at 65535; widening it costs memory on every node).
- Golden misc_134. Gate: 1215 pass, san 432/0, gcc.dg 52 / c-c++-common 35,
  callgrind 3.708G.

## Round 116: old-style merge, system-header markers, -O null pointer constants

- A prototype after an old-style definition that was merged into an earlier
  declaration (`void f(); void f(){}`) is not checked for "declares more
  arguments": gcc keeps no actual argument types then (pr89211;
  `CSym.olddef_merged`).
- `diag.c` suppressed warnings only for files in system directories; it now
  also honours `# N "file" 3` markers (`srcmgr_is_system`), so conversion
  warnings in such regions are dropped (wtr-int-type-1).
- Under -O, an expression reading a const object folded to its value is no
  null pointer constant (`constvar_in`; c99-const-expr-3), and arithmetic on
  floating constants, `(int)(0.0+0.0)`, is no integer constant expression
  (the binary result no longer carries `EF_REALCST`): gcc warns about
  variably modified / non-constant bit-field widths and enumerators there.
- Golden misc_135. Gate: 1217 pass, san 433/0, gcc.dg 48 / c-c++-common 35,
  callgrind 3.710G.

## Round 117: string pieces derived from tokens

- `Node.aux` (16 bits) saturated at 65535 string pieces: concat2 printed the
  wrong length and `last_tok` computed wrong token offsets. A string node's
  pieces are now the run of string tokens from its first (`node_pieces` in
  check_int.h); `N_STRING` no longer stores them. The AST dump still shows
  `#N`.
- The rustc-`Span`-style alternative (a `0xFFFF` sentinel and a per-unit
  spill table) is written up in docs/DESIGN-NOTES.md.
- Golden misc_136. Gate: 1219 pass, san 434/0, gcc.dg 47 / c-c++-common 35,
  callgrind 3.711G.

## Round 118: typedef aligned(1), statement-expression self-comparison

- `-Waddress-of-packed-member` asked the canonical type for its alignment, so
  a typedef with `aligned(1)` was ignored; `pk_align` now asks the typedef
  (misaligned-expand-3).
- `opeq` (`-Wtautological-compare`) unwraps a statement expression that is a
  single expression statement (`stmt_expr_single` in cstmt.c):
  `({ i; }) == ({ i; })` warns as gcc does (pr68412-2).
- Golden misc_137. Gate: 1221 pass, san 435/0, gcc.dg 45 / c-c++-common 35,
  callgrind 3.710G.

## Round 119: register arrays, block-scope extern of a static, large alignments

- A `register` array decaying to a pointer is an error (`address of register
  variable`) unless it is the operand of `sizeof`, `_Alignof`, `typeof`,
  `&` or `[]`; `register struct S c __asm__(..)` with a volatile member
  (at any depth) is an error (reg-vol-struct-1).
- A block-scope `extern` of a file-scope *static* now saves the outer binding's
  type and gives itself the composite, as the non-static path did: after the
  block the outer type is the old one again (redecl-3).
- `-Woverlength-strings` skips asm strings (template, constraints, clobbers,
  label).
- Alignments of 64 KiB and up were truncated to 16 bits (`CSym`, `Field`,
  `FieldIn`); they are 32 bits now, and a typedef's `aligned` is stored as
  log2 + 1 in its 16-bit slot. `__alignof__(function)` gives the declared
  alignment (1 if none) without the pedantic warning, as gcc does for a
  function designator (attr-aligned).
- Goldens misc_138, misc_139. Gate: 1225 pass, san 437/0, gcc.dg 41 /
  c-c++-common 35, callgrind 3.712G.

## Round 120: records past 2^63 bytes

- Record layout (type.c) computes in 128-bit bits (`lbits`) and stores the byte
  size saturated at 2^64-1, so a record over `INT64_MAX` bytes is no longer a
  wrapped small one. `type %s is too large` is reported at the tag (pr42611).
- `-Wlarger-than=` is on by default with the largest valid object as the limit
  (gcc warns for a local `struct S s;` of 2^63+4 bytes); `-Wno-larger-than` and
  `-Wlarger-than=N` behave as before.
- Golden misc_140. Gate: 1227 pass, san 438/0, gcc.dg 40 / c-c++-common 35,
  callgrind 3.717G.

## Round 121: c-c++-common rejects-valid and accepts-invalid

- A qualifier on a function declared through a typedef (`volatile ft vg;`) is a
  flag of the declaration, not of its type: a later `int vg(void);` no longer
  conflicts, and `volatile` makes the function noreturn (pr20000).
- `__builtin_has_attribute` of a comma expression whose value is an array sees
  the decayed pointer in C (no typedef attributes), and its operand is
  unevaluated, so a comma inside it keeps the array size an integer constant
  expression (builtin-has-attribute-7).
- `-Wno-attributes=` is validated like gcc: a comma list of `ns::attr` or
  `ns::`, identifier characters only, not all underscores (Wno-attributes-3).
- Goldens misc_141, misc_142. Gate: 1231 pass, san 440/0, gcc.dg 40 /
  c-c++-common 33, callgrind 3.718G.

## Round 122: asm constraint rules

- Checked against gcc-13 for every printable character in input, output and
  matching positions: an input constraint rejects a space or tab
  (`invalid punctuation`), an output one accepts any punctuation; a digit or
  `[name]` in an input must name an output (`matching constraint references
  invalid operand number`, `undefined named operand`, `missing close brace for
  named operand`); both are errors in an output (`matching constraint not
  valid in output operand`). Multi-digit numbers are decimal.
- Golden misc_143. Gate: 1233 pass, san 441/0, gcc.dg 40 / c-c++-common 33,
  callgrind 3.718G.

## Round 123: gcc.dg and c-c++-common leftovers

- Over-warnings removed: `-Wxor-used-as-pow` past 64 bits, `-Wconversion`
  for shifts (left operand only), `/`, `%` and bool-valued operands, unscoped
  `[[name]]` dropped once, `-Wunused-local-typedefs` for attribute uses.
- Added: `-Wcast-align=strict`, `-Wcast-function-type` (gcc's
  c_safe_function_type_cast_p), `nonstring` misapplication warnings,
  `__builtin_has_attribute` as a `typeof` for `-Wc++-compat`, `&__real__ p[i]`
  in `-Waddress`, `1.0f128x` is an error at input_location (no `_Float128x` on
  x86), and the missing-`#include` note on an unknown type name
  (`size_t`, `wchar_t`, `ptrdiff_t`), with no spelling suggestion. The header
  table and first-note state now live in the diag engine, shared by parser
  and checker.
- Goldens misc_144 to misc_153. Gate: 1253 pass, san 451/0, gcc.dg 36 /
  c-c++-common 22, callgrind 3.728G.

## Round 124: c-c++-common leftovers

- `-Wpadded`: "padding struct to align 'X'" at the field (named `({anonymous})`
  for an unnamed member) and "padding struct size to alignment boundary with N
  bytes" at gcc's finish_struct input_location (the token after `struct`
  when the lookahead is on the same line, else the first token of the
  lookahead's line). That location rule is shared with `-Wpacked`.
- `-Wrestrict` compares each restrict parameter with every other pointer
  argument, variadic ones too (not for built-ins), a restrict pair once.
- `-Wattributes`: `malloc (dealloc)` on an inline function (needs `-O`:
  `co.optimize`; at -O0 it is "given attribute 'noinline'") and an inline
  deallocator (any with `-O`, else only `always_inline`); an `optimize`
  attribute following a definition without one.
- `__builtin_has_attribute`: non-constant `aligned (i)` error and array
  element alignment error at input_location.
- `-Wlarger-than-N` is `-Wlarger-than=N`.
- Open: `-Wnormalized` (needs NFC tables), `#pragma GCC unroll j` with a
  non-constant name (the parser cannot tell enumerators from objects),
  `-Wmisleading-indentation`, `Wbidi-chars-6`, `conflict-markers-11`.
- Goldens misc_154 to misc_160. Gate: 1267 pass, san 458/0, gcc.dg 36 /
  c-c++-common 18, callgrind 3.734G.

## Round 125: unknown pragmas, K&R parameter labels, conflict markers
- Unknown pragmas print gcc's no-column form (`file:LINE: warning: ignoring
  '#pragma T1 T2' [-Wunknown-pragmas]`, first two tokens) with an empty caret
  line (`DiagEngine.nocol_next`); golden misc_161.
- `&&label` in a K&R parameter declaration: "referenced outside of any
  function" (`Checker.kr_decls`, set from the declarator to the body), no
  use_label; no "type of 'x' defaults to int" for a parameter already
  reported undeclared. Golden misc_162.
- Conflict markers count only in column 1 (misc_163).
- Builtin deallocators (free, realloc, __builtin_*) imply no noinline.
- -Wrestrict: the built-in flag was true for every prototyped function
  (`builtin_decl_ok` alone), so variadic arguments were skipped; now also
  requires `bt_for_decl`.
- Tried and dropped: spelling-location comparison for -Wmisleading-indentation
  inside one macro expansion (fixes test04, regresses DLSYM_OPT and FOR_EACH).
- Open: -Wnormalized, #pragma GCC unroll j, -Wmisleading-indentation (macro
  cases), Wbidi-chars-6, builtin-has-attribute typedef redefinition after an
  erroneous array size, -Wrestrict "arguments 3, 4" grouping.
- Goldens misc_161 to misc_163. Gate: 1273 pass, san 461/0, gcc.dg 34 /
  c-c++-common 14, callgrind 3.732G.

## Round 126: restrict grouping, has_attribute recovery, macro indentation
- -Wrestrict: one warning per restrict parameter listing every argument it
  aliases ("arguments 3, 4"); an argument already named is not reported
  again (misc_164).
- A non-constant `aligned (i)` in `__builtin_has_attribute` goes on as 0, as
  gcc does, so a later typedef redefinition is still diagnosed (misc_165).
- -Wmisleading-indentation: guard and next statement from one macro
  expansion compare their spelled (definition) positions, skipping the
  empty-expansion gap scan; argument tokens are left out (misc_166).
- Open: nested macros (Wmisleading-indentation-5 test05 needs the position
  of the inner macro invocation inside the outer body; the preprocessor
  keeps only the outermost expansion point), and 804:8, 924:15, 1154:10 in
  Wmisleading-indentation.c.
- Goldens misc_164 to misc_166. Gate: 1279 pass, san 464/0, gcc.dg 34 /
  c-c++-common 13, callgrind 3.732G (an eager line_start_loc in check_restrict cost +1.6% until made lazy).

## Round 127: -Wmisleading-indentation rules (c-c++-common 11)
- A guard whose body lines up with the guard token itself (`{ for (...)` with
  the body under the `for`, or `else if` with the body under the `if`) does
  not warn, except for `else` (gcc warns there).
- An empty body followed by a `{` on a later line warns when the block is at
  the guard line's own column (other statements need a deeper column).
- Guard and next statement from one macro expansion where the next token is
  spelled in a nested macro: the column is that of the nested macro's
  invocation inside the outer body (the first text after the last body
  token), the note stays at the nested spelling (Wmisleading-indentation-5).
- Goldens misc_167 to misc_172. Gate: 1291 pass, san 470/0, gcc.dg 34 /
  c-c++-common 11, callgrind 3.732G. All Wmisleading-indentation files now
  match gcc.

## Round 128: K&R identifier lists, bidi location in spliced lines (gcc.dg 30, c-c++-common 9)
- `params()`: a parameter list is an identifier list unless the second token
  is a name, `*`, `[` or `(`; a non-name after a comma gives gcc's
  "expected ')'" and only `)` gives "expected identifier" (misc_173). The
  `t (a,);` recovery (gcc goes on with extra warnings) is still open.
- -Wbidi-chars "unpaired" in a literal or identifier that continues across a
  backslash-newline is placed like gcc: on the logical line's first physical
  line, at the byte offset in the spliced text (`Diagnostic.vcol`, shown as a
  display column). Wbidi-chars-6 now matches (misc_174).
- Harness: the sparse checkout in `~/gccts` gained
  `gcc/testsuite/gcc.c-torture/execute/builtins` (chk.h), so attr-alloc_size
  and builtin-stringop-chk-2 are real tests and match. par.py's gcc cache is
  keyed on the command, not on included headers: run with `CEREAL_PARCACHE=0`
  after changing the checkout.
- gate.sh now logs per-stage seconds to `/tmp/gate.time` (run 71, verify 80,
  callgrind 83, san 172: san is the long pole, about 3 min total). A gate
  started with nohup/setsid from a one-shot `wsl -e` dies when that session
  exits; hold it with a foreground `wsl -e` run instead.
- Gate: 1295 pass, san 472/0, gcc.dg 30 / c-c++-common 9, callgrind 3.732G.

## Round 129: small c-c++-common clusters (c-c++-common 6)
- K&R `t (a,);`: the "expected identifier" error no longer fails the
  declarator; "parameter names (without types)" follows at the `)` (misc_175).
- "size of array element is not a multiple of its alignment" is placed at
  input_location (`iloc(ltok)`), not the member's first token (misc_176,
  pr97164).
- Named variadic macros under -pedantic: "ISO C does not permit named
  variadic macros [-Wvariadic-macros]" (own option; -pedantic-errors makes it
  an error) (misc_177, substring-location-PR-87721).
- `constructor`/`destructor`: arguments are expressions (an undeclared name
  is an error and silences the priority error), 0 or 1 arguments, and no
  priority check after "wrong number of arguments" (misc_178, pr59280).
- Open in c-c++-common: attr-opt-1, builtin-convertvector-1, dump-ada-spec-14,
  pr68833-3 (-Wnormalized), unroll-5.
- Gate: 1303 pass, san 476/0, gcc.dg 30 / c-c++-common 6, callgrind 3.732G.

## Round 130: __builtin_convertvector (c-c++-common 5)
- c_build_vec_convert's three errors: first argument not an integer/float
  vector (at the builtin), second not a vector type (at the type), element
  counts differ (at the builtin).
- Parse recovery as gcc's: after any failure skip to the closing `)` and
  build an error node (no implicit-int warnings, no "expected statement").
  `type_name` now says "expected specifier-qualifier-list" like
  c_parser_type_name (misc_179).
- Not done: va_arg, offsetof and __builtin_types_compatible_p have the same
  recovery gap (spurious implicit-int / "expected statement").
- Gate: 1305 pass, san 477/0, gcc.dg 30 / c-c++-common 5, callgrind 3.731G.

## Round 131: optimize attribute (c-c++-common 4)
- `optimize("a,b")` strings may be comma or space separated; each option is
  checked (was: a string with a space was skipped, a comma list was one bad
  option).
- "optimization attribute ... follows definition but the attribute doesn't
  match" now also fires when the definition has a different set: the options
  are compared as an unordered set, except that an -O level makes the order
  significant (gcc applies -O only to flags not yet set). An emulation of
  gcc's node comparison, checked on attr-opt-1 only (misc_180).
- Parked: unroll-5 (`#pragma GCC unroll j`) needs the parser to tell a
  variable from an enum constant (both are SYM_ORDINARY), or the check moved
  into the checker.
- Open in c-c++-common: dump-ada-spec-14 (packed layout), pr68833-3
  (-Wnormalized), unroll-5, and one file not yet identified (par lists the
  differing files; the gate counts 4).
- Gate: 1307 pass, san 478/0, gcc.dg 30 / c-c++-common 4, callgrind 3.736G.

## Round 132: builtin recovery, enumerator attributes (c-c++-common 3)
- `__builtin_va_arg` recovers like convertvector (skip to the `)`);
  `offsetof` skips to the `)` after a bad type name or missing comma and says
  "expected identifier" for a bad member; `__builtin_types_compatible_p`
  leaves the `)` of a failed type name behind, as gcc does (the statement
  parser then reports it). A type name with a syntax error leaves no nodes,
  so the checker adds no implicit-int warning (`type_name_ok`, misc_181).
- An unknown attribute on an enumerator was reported twice (the deprecated
  scan and enumerator_attrs both collected it): the scan is now quiet
  (misc_182, attributes-enum-2).
- Open in c-c++-common (3): dump-ada-spec-14 (`-fdump-ada-spec` warns
  "packed layout" on a packed struct), pr68833-3 (-Wnormalized), unroll-5
  (see Round 131).
- Gate: 1311 pass, san 480/0, gcc.dg 30 / c-c++-common 3, callgrind 3.736G.

## Round 133: gcc.dg triage (30 to 28)
- `-pedantic-errors` makes the parser pedwarns errors (`pwarn`): the
  struct-semicolon pedwarn (misc_184, struct-semi-3).
- `__imag__` of a non-complex int whose operand has a side effect is not
  constant (hard error, as gcc); an imaginary literal cast directly to an
  integer is an ICE (misc_183, gnu99-const-expr-1).
- Gate: 1315 pass, san 482/0, gcc.dg 28 / c-c++-common 3, callgrind 3.736G.

## Round 134: gcc.dg triage (28 to 24)
- A nested enum redefinition is diagnosed even when a later syntax error
  (an empty inner enum) mutes the unit, and ends the being-defined state, so
  the next redefinition is a "redeclaration" (misc_185, enum-redef-1).
- Under `-ftrack-macro-expansion=0` a binary constant that comes from a macro
  in `#if` is reported at the expansion point (`PP.if_exp`; misc_186/187,
  binary-constants-2/-3).
- `#line` out of range: the plain gcc message (an error under
  -pedantic-errors), and diagnostics print the presumed line as a signed int
  (`-1`; misc_188, pr89410-1).
- Gate: 1323 pass, san 486/0, gcc.dg 24 / c-c++-common 3, callgrind 3.738G.

## Round 135: gcc.dg triage (24 to 21)
- `[*]` in a function body is an error at the array, and gcc drops the
  declaration (no "array size missing" follow-up): misc_189, vla-6.
- A function definition's own `aligned (N)` (in its specifiers) now sets the
  symbol's alignment, so `__alignof__ (f)` sees it: misc_190, attr-aligned-2.
- `restrict` on a non-pointer variable or field is reported twice, as gcc
  qualifies the type twice: misc_191, c99-restrict-1. Known gap: a parameter
  of a function *definition* is also reported twice by gcc (we say once).
- Not fixable here: invalid-call-1 and pr56355-1 need `-O2` middle-end
  warnings; init-bad-4 differs only in diagnostic order (a parse error
  printed after checker errors); array-10 needs gcc's "empty declaration"
  after a variably modified struct member at file scope (rule not derived).
- Gate: 1329 pass, san 489/0, gcc.dg 21 / c-c++-common 3, callgrind 3.738G.

## Round 136: linemarker nesting (gcc.dg 21 to 20)
- A GNU linemarker with flag 2 (return to the includer) in a file that no
  marker entered is ignored with "file "X" linemarker ignored due to
  incorrect nesting", located at the end of the line (`IncludeFrame.
  marker_depth`; misc_192, pr69650).
- STATUS.md: the open differences are now grouped (optimizer-only, Darwin,
  front-end gaps) and the counts match diagstat (binary-constants-1 and
  init-bad-4 differ only in exit code / order, so are not counted).
- Looked at and left: Warray-parameter-11 (needs folding of `!copysign (...)`
  and address differences in array bounds), multiple-overflow-warn-3 (gcc
  prints the folded `-2147483648 - 1`), pr100547 (error location),
  sequence-pt-pr17880 (`return a++ - a--` column).
- Gate: 1331 pass, san 490/0, gcc.dg 20 / c-c++-common 3, callgrind 3.738G.

## Round 137: -Wnormalized (c-c++-common 3 to 2)
- Identifiers (and pp-numbers) with an extended character run libcpp's
  quick check: `norm_check` in lex.c, data in `src/ucnorm.h` generated by
  `bench/tools/gen_nfc.py` from libcpp/ucnid.h (ranges of {combining class,
  kind} and the (c, previous starter) pairs of `check_nfc`; Unicode licence
  kept). Levels `-Wnormalized=nfkc|nfc|id|none`, bare = nfc (the default),
  `-Wno-normalized`, `-Werror=normalized`. The message respells the token
  with `\U%08x` (numbers as written).
- Check: `python3 -P bench/tools/fuzz_nfc.py ./cereal 25 < /dev/null`
  compares the warnings with gcc-13 on random identifiers at every level
  (0 mismatches); goldens misc_193..197 (pr68833-3 now matches).
- (Closed in Round 139: a token made by `##` was not checked; gcc warns at
  column 1 of the current line.) The goldens run under `-std=c99 -pedantic`,
  where Hangul jamo are invalid, so those paths are covered by the fuzz only.
- Gate: 1341 pass, san 495/0, gcc.dg 20 / c-c++-common 2, callgrind 3.733G.

## Round 138: identifier spelling in messages (gcc.dg 20 to 19)
- A name written with UCNs is interned as its UTF-8 text (`ucn_canon` in
  lex.c; the token length is that of the interned text), so `\u03C0`,
  `\U000003c0` and `π` are one identifier. Before, UTF-8 and UCN spellings
  were different names, and `\U` forms were interned under a text shorter
  than the token length (a read past the end when the text was re-read).
- `cident_ucn` no longer truncates at 240 bytes, and `fuzzy_name` no longer
  uses its rotating buffers: the caller's `name` was overwritten by the
  candidates (an undeclared long name was reported as another one). A
  suggestion is printed as UTF-8, as gcc does (`did you mean 'π'?`).
- "unknown type name" spells an extended name as `\U%08x` (misc_198;
  ucnid-15-utf8).
- (Fixed in Round 141.) A token that is an identifier with a UCN has the length of its
  interned UTF-8 text, so an end-of-token location (e.g. "expected ';' before")
  after such a name is off by the difference.
- Gate: 1343 pass, san 496/0, gcc.dg 19 / c-c++-common 2, callgrind 3.732G.

## Round 139: -Wnormalized for ## results
- A token made by `##` that is an identifier or pp-number with an extended
  character is checked too (`lex_norm_check`, called from `paste` in
  ppexpand.c). libcpp reports it at column 1 of the line being read, i.e. the
  line of the invocation's `)` (`PP.paste_loc`); misc_199, ucnid-3.
- Gate: 1345 pass, san 497/0, gcc.dg 19 / c-c++-common 2, callgrind 3.732G.

## Round 140: reparse after "expected ... before X" at file scope
- At file scope gcc reparses the token after an "expected ... before X" error,
  even following "unknown type name" (`foo bar baz qux;` gives a second
  "unknown type name 'baz'"). Block scope still skips (unk_type_skip).
  parse.c now syncs only when `!top` (`s.err && !top`); misc_200, normalize-2.
- Gate: 1347 pass, san 498/0, gcc.dg 19 / c-c++-common 2, callgrind 3.732G.

## Round 141: end of a UCN-written identifier
- `tok_raw_len` now also re-lexes TF_UCN tokens, and `perr_after_prev` uses it,
  so "expected ';' before" after `\u03c0` lands at the source column
  (misc_201). Removed a stray `typedef` on `struct Lexer` in lex.h.
- Known gap: after `void f(void) { int \u00c1x = 1 }` gcc leaves `f` open
  (nested-function warnings follow); cereal closes it.
- Gate: 1349 pass, san 499/0, gcc.dg 19 / c-c++-common 2, callgrind 3.732G.

## Round 142: gcc.dg/cpp in the parity run
- `CEREAL_DGDIR=gcc.dg/cpp` already worked in par.py; verify.sh now runs it
  (266 of 474 files; the rest need dg-options par.py skips). Baseline: 144
  identical, 122 differ. This is the biggest open parity area.
- Main clusters (gcc-only messages unless noted): `-pedantic-errors` does not
  turn pp pedwarns into errors ("ISO C99 requires whitespace after the macro
  name", also 1 column off: gcc 5:9, cereal 5:10); `assertions are a GCC
  extension` / `#assert|#unassert is a GCC extension` (cereal prints
  `[-Wpedantic]` and a different deprecated-assertions text); "this use of
  defined may not be portable"; `-Wtraditional` pp warnings; "embedding a
  directive within macro arguments"; "astr/cat redefined" (cereal-only,
  56x, probably a shared header); rejects-valid: Wtrigraphs-2, backslash,
  import1/2, include4, pr33415, vararg2; accepts-invalid: escape-1, macspace2.
- The default par cache (`~/.cache/cereal-par`) gave 23 gcc.dg differences
  where `CEREAL_PARCACHE=0` gives 19; run parity with the cache off (the gate
  does).
- gate.sh header corrected to dg 19 / c-c++-common 2 (it still said 30 / 3).

## Round 143: unrecognized format function types
- `format(T, ...)` with T not printf/scanf/strftime/strfmon (and gnu_ forms),
  gcc_diag/cdiag/cxxdiag/tdiag or asm_fprintf warns "'T' is an unrecognized
  format function type"; NSString gives "is only allowed in Objective-C
  dialects" (-Wformat=), at the first token of the attribute's line
  (misc_202; warn-nsstring).
- Known gap: for a struct member gcc reports at the tag name, cereal at the
  line start.
- Gate: 1351 pass, san 500/0, gcc.dg 18 / c-c++-common 2, callgrind 3.732G.

## Round 144: preprocessor pedwarns, macro parameter lists, small recoveries
- `pp_pedwarn` (libcpp CPP_DL_PEDWARN, no option): a warning, an error under
  -pedantic-errors. Used for "ISO C99 requires whitespace after the macro
  name" (now at the name, as gcc), "__VA_ARGS__ can only appear in the
  expansion of a C99 variadic macro" (was an error under -pedantic) and
  "ISO C99 requires at least one argument for the "..."" (misc_203).
- `#define` parameter lists follow libcpp's parse_params: its five messages
  (`expected parameter name, found "X"`, `expected ',' or ')', found "Y"`,
  `... before end of line` at the end of the line including a trailing
  comment, `expected ')' after "..."`), `__VA_ARGS__` as a parameter name is a
  pedwarn, a duplicate stops the list.
- "unterminated argument list" is reported after the last token read (gcc),
  with the macro name kept as the range; lsp/basic.json updated.
- A missing ';' before a declaration at file scope no longer skips the next
  declaration (`foo` / `int x = a;` reports 'a'); misc_204.
- -Wsequence-point on a `return` expression is checked like an assignment
  (gcc reports the first operator, not the last); misc_204.
- Known gaps: `__VA_ARGS__` in plain text or as a #define name does not
  warn; a pp error that gcc prints before a later parse error comes out after
  it (unterminated argument list at EOF); "missing terminating" quote inside a
  #define line is silent; multiple-overflow-warn-3 (gcc spells folded
  operands, '-2147483648 - 1', at the statement start); array-10 (empty
  declaration after an erroneous struct).
- Gate: 1355 pass, san 502/0, gcc.dg 17 / c-c++-common 2, callgrind 3.732G.

## Round 145: missing-nul string functions, format-type warnings
- `strspn`/`strcspn` (both arguments) and `strlen` report "argument missing
  terminating nul" with the call location; a parenthesised binary argument is
  located at its operator. `__builtin_strlen` of a constant string or const
  char array is folded (plain `strlen` is not an integer constant in gcc);
  misc_205, misc_206.
- `printf(unt)` with a const char array without a NUL: "unterminated format
  string" (-Wformat=), also with a constant offset (misc_207).
- `format` attribute: NSString and unknown function types warn at the
  start of the attribute line; strftime/strfmon/gcc_* types are known
  (misc_202).
- Remaining: Wstringop-overflow-22 (`%s` with a non-nul-terminated array is
  not diagnosed), array-10, multiple-overflow-warn-3 (see R144).
- Gate: 1361 pass, san 505/0, gcc.dg 16 / c-c++-common 2, callgrind 3.736G.

## Round 146: const char arrays from brace lists, libcpp message parity
- A `const char t[] = { 'a', ... }` of constants is read like a string
  initializer (-Wstringop-overread, "unterminated format string"); the format
  is unterminated when its last byte is not a nul, whether or not an earlier
  byte is (misc_208). `__builtin_strfmon` checks only for the missing nul
  (its conversions are not parsed). Closes Wstringop-overflow-22.
- Preprocessor diagnostics follow libcpp's option tags (misc_209, misc_210):
  pedwarns of directive handling (#include_next, #assert, #warning before C2X,
  extra tokens, "#include_next in primary source file", integer overflow in
  #if) print no `[-W...]`; #warning is `-Wcpp`; extra tokens after
  #else/#endif is `-Wendif-labels`; pp pedwarns are hidden in system headers.
- "this use of "defined" may not be portable" replaces the old wording and is
  reported at the last source token read (gcc's cur_token[-1]); `"defined"
  cannot be used as a macro name` uses double quotes.
- Assertions in #if: `assertions are a GCC extension` under -pedantic, the
  -Wdeprecated warning otherwise.
- #if: the comma pedwarn and integer-overflow warnings sit at the token that
  ends the operand (the end of the line, comment included, for the last
  one); the comma is an error under -pedantic-errors; left shift overflows
  when shifting back loses the value (libcpp num_lshift).
- `'##' cannot appear at either end` and `'#' is not followed by a macro
  parameter` are at the last token before the body (name, or the ')').
- "ISO C99 requires at least one argument" also covers named variadics.
- Known gaps: `#if 1 #foo(bar)` evaluates the assertion (gcc: missing binary
  operator before "#"); gnu89 "empty macro arguments" pedantic warning.
- Gate: 1367 pass, san 508/0, gcc.dg 15 / c-c++-common 2 / cpp 91, callgrind 3.746G.

## Round 147

gcc.dg/cpp rejects-valid and accepts-invalid are now 0 (cpp 197 identical, 69 differ; was 175 / 91).

- Trigraphs: `-std=c99` enables them silently; `-Wtrigraphs` (or -Wall) warns
  "converted"; gnu modes ignore them and warn "ignored, use -trigraphs".
  `-trigraphs` sets a separate flag so a later `-std=gnu99` turns them off.
  A pre-scan (`scan_line_notes`, run from `lexer_init`) emits trigraph and
  "backslash and newline separated by space" warnings, tracking
  code/comment/string state and gcc's per-trigraph column shift.
  `\` + spaces + newline is a splice (tab/FF/VT are not, empirically).
- `#import` (once-only, GCC extension messages), UTF-8 BOM skip,
  `#include <x.h` with a macro `>` (include4), vararg2 (a macro whose only
  parameter is variadic counts as omitted in GNU modes).
- Escape diagnostics (`lit_escape_diags`, shared by #if char constants and the
  C front end): `\x` without digits, hex/octal out of range, unknown escapes,
  `\e` and `\(` etc. under -pedantic, delimited `\x{}` / `\o{}` forms.
- `-Wtraditional` preprocessor warnings: directive indentation, `#elif`,
  `U` suffix and unary plus in #if, function-like macro used without
  arguments, stringified macro argument; skipped for system-header macros.
- Goldens misc_211..218.
- Known gaps: `#if 1 #foo(bar)`; gnu89 "empty macro arguments"; pp-vs-parser
  message ordering (sysmac1/3); pre-scan diagnostics come up front in file
  order, not interleaved; `'\x{'` adds an extra multichar warning; escape
  warnings not issued for tokens the parser rejects (`u'\x10000'` in c99);
  missing "in expansion of macro" note for stray `\` inside macros.
- Perf: the pre-scan is SIMD-skipped (`next_note_byte`) and gated by
  `has_line_note`; first cut cost +7% on uvloop, now +1.4%.
- Gate: 1383 pass, san 516/0, gcc.dg 15 / c-c++-common 2 / cpp 69, callgrind 3.800G.

## Round 148

gcc.dg/cpp 197 -> 233 of 266 identical (33 differ); gcc.dg 15 and c-c++-common 2 unchanged.

- Directive messages: `lex_macro_node` ("no macro name given", "macro names
  must be identifiers"), `do_line` empty/non-number forms, `__VA_ARGS__`
  pedwarns, `#include` header names lexed raw to the first `>` with extra
  tokens macro-expanded before warning, `check_eol` as an error under
  `-pedantic-errors`.
- `paste` error at the lhs definition location with an "in expansion of
  macro" note (expansion point under `-ftrack-macro-expansion=0`);
  `stringify_arg` "invalid string literal, ignoring final '\'" at the rparen.
- "unterminated #if/#ifdef/#ifndef/#elif/#else" as a nocol error with the
  last directive's name; nested conditionals in skipped groups are tracked.
- Charconst "too long for its type" vs multichar; octal "invalid digit" names
  the max digit; splice blanks are space/tab/FF/VT (`splice_blank`).
- "at end of input" location follows the last BOL token read even when its
  macro vanishes (`pp->last_bol`, `CellSource.last_bol`, `p->last_line`).
- Line notes (trigraph / backslash-blank) are now lazy: `scan_line_notes`
  records them, `lexer_flush_notes` reports each once the lexer reaches its
  position, interleaving with other diagnostics in file order like libcpp.
  A NULL-diag lexer (parallel phase A) leaves them pending; the workers
  report. Golden misc_232.
- Goldens misc_219..232. Gate: 1411 pass, san 530/0, callgrind 3.810G.
- Known gaps: escape/multichar warnings not issued for tokens the parser
  rejects (`u'\x10000'`, `U'x41'` in c99); missing include is "error" not
  "fatal error" outside -E; `__has_attribute`/`__has_builtin` error forms;
  "backslash-newline at end of file" warning not implemented; file ending in
  `#line N` empty-TU line; `#if 1 ??= 2`; one-token parser lookahead can
  order a parser error after a following line's note; pp-vs-parser message
  ordering; gnu89 "empty macro arguments".
- verify.sh's gcc-side cache (`~/.cache/cereal-par`) can hold stale gcc
  results if the testsuite was momentarily missing; delete entries matching
  "No such file" and rerun.

## Round 149

gcc.dg/cpp 233 -> 240 of 266 identical (26 differ); gcc.dg 15, c-c++-common 2.

- "backslash-newline at end of file" (pedwarn): a file ending in a splice is
  noted once, at the first splice of the last logical line (`'e'` line note;
  the prefilter in `scan_line_notes` also checks the file tail).
- Line notes flush by raw position (`LineNote.pos`) and after comments, so
  a note inside a line is reported only when the lexer reaches it. Golden misc_232/233.
- Missing `#include` is a fatal error ("compilation terminated.") in
  `-fsyntax-only`/`check` too (`parse` still continues, for the editor
  features); `diag_flush` stops at the first DL_FATAL. Golden misc_234.
- "invalid preprocessing directive #x; did you mean #y?" (`directive_hint`:
  Damerau-Levenshtein, gcc's cutoff, dtable order).
- `#line` number is 32-bit with wrap detection like libcpp; a wrapped value
  always warns "line number out of range".
- `#error`/`#warning` text is cleaned like libcpp (trigraphs under
  -trigraphs, splices removed); token raw end skips splices/trigraphs.
- Gate: 1415 pass, san 532/0, callgrind 3.812G.
- Still open: escape/multichar warnings for tokens the parser rejects;
  one-token parser lookahead ordering; `__has_attribute` error forms;
  pragma diagnostic columns (system_header, dependency); undef2 builtin
  names; poison; Wsignprom; remaining cpp list in verify output.

## Round 150

Lexing diagnostics for tokens the parser rejects (gate: cpp still 240/266,
gcc.dg 15, c-c++-common 2; 1417 pass, san 532/0, callgrind 3.818G).

- `classify_num` also classifies character constants (`classify_char`): escape
  diagnostics (`lit_escape_diags`), "multi-character character constant" and
  "too long for its type", when an error names the token or recovery skips it
  (`u'ab'`, `U'x41'` in c99, where `u`/`U` are plain identifiers).
- gcc lexes the token after an identifier to look for a postfix operator, so
  `parse_primary` classifies a following char constant or number right away
  and sets the diagnostic's merge key (`oloc`) just before the identifier:
  the warning precedes "'u' undeclared", as in gcc. Golden misc_235.
- Still open: the general one-token-lookahead ordering for other token
  pairs (only identifier + literal is handled); the rest of the cpp list.

## Round 151

Duplication cleanup from the clone scan (no behaviour change intended; 1417
goldens, san 533/0, callgrind 3.816G, parity unchanged: cpp 240/266).

- `tpunct`, `first_tok`, `last_tok`, `first_loc`, `after_tok` live once in
  `check_int.h` (were copied in cdecl/cinit/cstmt/cparm/cexpr_int.h). The shared
  `first_tok` handles `N_ADDR_LABEL`; the old static copies did not.
- `cparm.c`: the loop in the parameter printer calls `put_arr`.
- `ppexpr.c` decodes char escapes with `lit_char_one` (was `lit_one`, now
  exported from `c/lit.h`); its own switch and `hexval` are gone. `#if` now
  also accepts `\u{..}` and `\N{..}` like the lexer.
- Looked at and left: `cdecl.c` repeats are attribute-walk skeletons and a
  case dispatch with different bodies; `cexpr.c:683` is a data table;
  `index.c` covering_refs/range span walks differ in clamping and the point
  argument; `lsp/pos.c` only shares a 9-line `hexval`.
- Checked, not a bug: `u'x'` / `U'x'` in `#if` is rejected under -std=c99 and accepted
  under gnu99, same as gcc.

## Round 152

`cattr.c` split out of `cdecl.c` (pure move: attributes and attribute names,
cdecl.c 10,3xx -> 7,341 lines, cattr.c 2,828). Gate: 1418 goldens, san 533/0,
callgrind 3.812G, parity unchanged.

- `c/cdecl_int.h` holds what both files read: the node/token readers
  (`ntag`, `tokp`, `tstr`, `Kids`, ...), the `AC_*`/`AttrState` types,
  prototypes of the attribute entry points, and the per-declaration globals
  cdecl.c sets and cattr.c reads (`alloc_name`, `alloc_via_ptr`, `ctx_vname`,
  `imp_l/imp_n/imp_name`).
- `iloc`, `iloc_event`, `is_rec` and `find_child` are no longer static.
- Next candidates (not started): cspec (cdecl.c add_scspec..specs_visit),
  cwarn_expr (cexpr.c), cprint (cexpr.c), cstruct.

## Round 153

`cspec.c` split out of `cdecl.c` (pure move: `add_scspec` .. `specs_visit`, 719
lines; cdecl.c now 6,6xx). Gate: 1419 goldens, san 533/0, callgrind 3.812G,
parity unchanged. `specs_visit` and `cxx_typedef_in_struct` are exported via
`cdecl_int.h`.

## Round 154

`cwarn_expr.c` split out of `cexpr.c` (pure move: -Wlogical-op through
-Wsizeof-pointer-div, 2,464 lines; cexpr.c now 8,4xx). Gate: 1420 goldens,
san 533/0, callgrind 3.814G, parity unchanged. 31 helpers are exported through
`cexpr_int.h`, plus `no_int_bool`, `cst_parts` and the `PW` macro.
Remaining candidates: cprint (cexpr.c 996-1610), sequence-point (cexpr.c
tail), asm statements, cstruct/cstmt_warn/parse_diag.

## Round 155

Three more pure moves out of `cexpr.c` (now 6,705 lines): `cprint.c` (%E
expression printing, 624 lines), `cseqpt.c` (-Wsequence-point, 609) and
`casm.c` (asm statements, 477). Gate: 1423 goldens, san 533/0, callgrind
3.819G (was 3.814G: code layout only), parity unchanged. Only
`sq_for_cond` and `sq_check` needed exporting. Largest files now: cdecl.c
6,620, cexpr.c 6,705, parse.c 3,393, cstmt.c 3,064, cwarn_expr.c 2,466.

## Round 156

`cstmt_warn.c` split out of `cstmt.c` (pure move: -Wunused-value,
-Wmisleading-indentation, -Wduplicated-branches/-cond and the statement walk
they share; 1,279 lines; cstmt.c now 1,6xx). New `cstmt_int.h` holds the
`CStmt` state types and the exports. Gate: 1424 goldens, san 533/0, parity
unchanged. Callgrind first read 3.828G: `tg`, `strip_paren` and
`node_err` had become cross-file calls on a hot path; making them
`static inline` in `cstmt_int.h` brought it back to 3.817G. Lesson for later
splits: export tiny hot accessors as `static inline`, not as functions.
