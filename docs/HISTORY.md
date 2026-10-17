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

## Round 157

cpp parity, item 1 of six: "unterminated argument list invoking macro"
location. gcc reports it at `pfile->cur_token[-1]`: the end-of-line EOF token
(after a trailing comment) when the EOF ends a directive line, otherwise the
start of the last token the lexer produced, never the macro name. `PP` now
records `last_lex_loc/last_lex_end` in `pp_read_raw`; `collect_args` picks the
lexer end, the directive end (`eol_after`), or that last lexed token by where
the EOF surfaced. New golden `misc_236`. `macsyntx.c` and `macsyntx2.c` now
match (cpp 242/266); `directiv.c` still differs on form feed and vertical tab
(item 5). Gate: 1426 goldens, san 534/0, callgrind 3.829G (+0.3%, two stores
on the token-read path).

## Round 158

cpp parity, item 2 of six: `-Wall` "the left/right operand of OP changes
sign when promoted" in `#if` (libcpp `check_promotion`). It fires for
`* / % + - < > <= >=` and the arms of `?:`, not `== !=`, shifts or bit
operations; the negative operand of a signed/unsigned pair is reported, and
skipped subexpressions warn too. `Val` now carries `loc`, where libcpp puts
an operand: its token, the operator for a reduced result, the inner operand
for parentheses. New untagged option `sign-promo-in-if` (`DO_ALL`; gcc prints
no `[-W...]`, and `-Wno-sign-promo` does not touch it). Golden `misc_237`
matches gcc-13 exactly. `Wsignprom.c` now matches (cpp 243/266). Gate: 1428
goldens, san 535/0, callgrind 3.812G.

## Round 159

cpp parity, items 3 to 6 of the six (cpp 249/266):

- `__has_attribute` / `__has_builtin` read their operand the way gcc's
  `c_common_has_attribute` does (`query_ident`): a lone identifier, each error
  at the last token read ("requires an identifier" at the `(` or the bad
  token, "missing ')'" or, for `__has_builtin`, "expected ')' after" with a
  skip to the `)`), a nested `__has_*()` counting as a number. "missing '('"
  now lands on the offending token. A stray top-level `)` in `#if` is
  "missing '(' in expression". Golden `misc_238`.
- `#pragma GCC poison`: a non-identifier gives "invalid #pragma GCC poison
  directive" and stops; `#define`/`#undef`/`#ifdef`/`#ifndef` of a poisoned
  name do nothing (libcpp's `lex_macro_node` returns NULL), so `#define`
  after the pragma no longer revives the macro.
- `-pedantic` form feed / vertical tab in a directive line
  (`warn_directive_ws`): scanned in code only, not strings, comments or a
  splice's trailing blanks.
- `_Pragma` diagnostics for `system_header` and `dependency` sit on the
  `_Pragma` line at the token's column in the destringized text
  (`scratch_col`); the `dependency` message now reads `expects "FILENAME" or
  <FILENAME>` (the `<FILENAME>` form itself is still not accepted).
  Golden `misc_239`.

Gate: 1432 goldens, san 537/0, callgrind 3.812G. Still open: caret ranges
(`^~~~`) differ on these pedwarns; parity compares headers only.

## Round 160

cpp parity, the remaining quick ones (cpp 255/266):

- Unterminated `'`/`"` in a directive line are diagnosed by `read_line` as
  gcc lexes them (pedwarn `invalid-pp-token`, error under `-pedantic-errors`),
  token by token. `#define` and `#include` flush in gcc's order
  (`unterm_from`, `flush_unterminated`); `#include "x\""` lexes the header
  raw. `#pragma message` warns once on a directive line (the `_Pragma` path
  still warns itself). Golden `misc_243`.
- `#undef` of always-warn builtins (`__LINE__`, `__STDC__`, `__COUNTER__`,
  `__has_*`, ...) is untagged at the name; other builtins keep
  `-Wbuiltin-macro-redefined` at a line-only location.
- `#if`: a leftover `:` is "':' without preceding '?'".
- `#line` overflow uses libcpp's two-step wrap check ("line number out of
  range"); an EOF diagnostic on the last line is placed on the next line.
  Goldens `misc_241`, `misc_242`.
- Number-literal errors are reported at the spelling location with "in
  expansion of macro" notes (`classify_num`, shared `add_macro_notes`).
  Golden `misc_240`.

Gate: 1442 goldens, san 542/0, callgrind 3.818G, -O 3729/3744.

Parked (open): `gnu99-scope-1` (the `::` token is CPP_SCOPE in gnu modes and
c2x; too invasive); `#warning` ordering against the
unterminated warning; the `<FILENAME>` form of `#pragma dependency`; caret
ranges on pedwarns; parser-wording files (19990413-1, 20000419-1,
20020927-1, spacing2, pr66415-2); `pragma_message.c` quote style in notes
and two missing macro-expansion notes.

Addendum (same round): skipped `#if 0` groups now lex the rest of each
directive line (gcc's `skip_rest_of_line`), so `#define z '` there warns.
Golden `misc_244`; no parity file changes (callgrind +0.2%). Survey of the
five parser-wording files, all still open: 19990413-1 (`else` with no `if`
should say "'else' without a previous 'if'"); 20000419-1 and 20020927-1
(one cause: a string made by `#x` is placed at the `#x` in the macro body,
gcc places it at the end of the invocation); spacing2 ("expected identifier
before '.' token", and `bar` undeclared at file scope is not reported);
pr66415-2 (format argument column).

Round 161 (cpp 258/266):

- A string made by `#x` takes the location of the last token the lexer
  produced (libcpp's `cur_token[-1]`: the `)` ending the outermost
  invocation), not the `#x` in the macro body. Fixes 20000419-1, 20020927-1.
- `else` with no `if`: "'else' without a previous 'if'" (19990413-1).
- A missing member or designator name reads "expected identifier before ..."
  (spacing2's wording; its file-scope `'bar' undeclared` error is still not
  reported). Golden `misc_245`.

Gate: 1444 goldens, san 543/0, callgrind 3.818G, -O 3729/3744. Still open:
pr66415-2 (format argument column), spacing2's undeclared `bar`, and the
parked list in Round 160.

Addendum (Round 161): an undeclared name that precedes the unit's first
syntax error is reported even though the unit is otherwise quiet (gcc
diagnoses it when it parses the name). `Checker.first_err_tok` is the last
token of the first flagged subtree, computed in `checker_unit`. spacing2 now
matches (cpp 259/266); golden `misc_246`. The one remaining header diff in
pr66415-2: after a `#24` line directive gcc puts the format warning at the
string's start (col 52), not at the `%d` (col 71); without the directive both
agree, so it is a gcc line-map quirk, parked. Gate: 1446 goldens, san 544/0,
callgrind 3.818G, -O 3729/3744.

Round 162 (cpp 262/266, gcc.dg 3731/3744):

- `_Float128` literals at the maximum no longer warn "exceeds range": the host
  long double (x87) overflows slightly below _Float128's maximum
  (`f128_decimal_fits`). pr88074-2.
- Removed `-Wunbalanced-push-pop-macro` and its warning: gcc 13 has neither
  (it is gcc 14). pragma-pop_macro-1.
- `#pragma STDC FLOAT_CONST_DECIMAL64`: pedantic "ISO C does not support" and
  "malformed ... ignored" [-Wpragmas].
- `#if` character constants: `\u`/`\U` escapes get libcpp's "outside the UCS
  codespace" pedwarn, "not a valid universal character" and "incomplete
  universal character name" (`ucn_diags`). ucs.
- "function called through a non-compatible type" also fires through nested
  pointer casts of a function designator. invalid-call-1.
- `vector_size`: "number of vector components N not a power of two" (was
  silently accepted), checked before "exceeds". Golden `misc_247`.

Still open: pr100547 (the location of the vector_size error is gcc's
input_location, which for an operator expression is not simply the start of
the last operand: 4:3, 8:3 and 10:4 in a probe, 34:5 in the test). Gate: 1448
goldens, san 545/0, callgrind 3.823G.

Round 163 (cpp 263/266, gcc.dg 3732/3744):

- `else` without a previous `if` is one error and an empty statement (no
  cascade). direct2. Golden `misc_248`: under `-ftrack-macro-expansion=0`
  a stray `#` from a macro is reported at the expansion point.
- A struct whose member is variably modified, declared at file scope or in
  the declaration list before a function body, gets gcc's "empty
  declaration" pedwarn at column 1 (`Checker.in_head` marks the region
  between a definition's declarator and its body; function bodies do not
  warn). array-10. Golden `misc_249`.
- The omitted-parameter-name pedantic is located at the parameter's first
  token (`array_parameter` golden regenerated: col 10, as gcc).

Gate: 1452 goldens, san 547/0.

Round 163 triage (parked): c-c++-common/unroll-5 needs `#pragma GCC unroll j`
(a variable) rejected; the parser judges the argument textually and its scope
cannot tell an enumerator from a variable (both SYM_ORDINARY), so it needs the
checker's constant evaluator on the pragma operand, a larger change. The
other lines already match (a caret range on 20000000000 is the parked caret
work). dump-ada-spec-14 is gcc's "packed layout" warning under
`-fdump-ada-spec`, a flag cereal does not implement.

Round 164 (cpp 264/266):

- "ISO C99 requires at least one argument for the ..." is not given for a
  macro defined in a system header (libcpp: macro->syshdr). sysmac3.
- An error inside a declarator's parameter list that parsed on (`bar(a, )`)
  no longer silences the declaration's own diagnostics ("data definition has
  no type", implicit int): gcc gives them after the error. The parser flags
  the N_FUNC node NF_ERROR, `Checker.first_err_params` says the first error
  was one, and N_DECLARED un-quiets in that case. Golden `misc_250`.

Still open in cpp: gnu99-scope-1, pr66415-2 (both parked). Gate: 1454
goldens, san 547/0.

Round 164 addendum (gcc.dg 3733/3744):

- An index in the initializer of an unsized array that makes the array larger
  than the largest object (`[0x8000000000000000] = ...`) is "size of array is
  too large" at the closing brace, not "exceeds array bounds" at the index
  (`CCtx.toolarge`, reported in `finalize_root`). large-size-array-6, golden
  `misc_251`.
- Parked: multiple-overflow-warn-3. gcc prints the overflow in
  `(1 << 31) - 1` as `-2147483648 - 1` at the start of the statement (the
  signed shift into the sign bit yields an overflow-flagged constant that is
  printed folded and loses its location); other nested constants print no
  expression at all and already match. A one-off for sign-bit shifts.

Round 164 addendum 2 (gcc.dg 3735/3744):

- More built-in library functions carry gcc's nonnull: the v*printf/v*scanf
  family and the _chk variants of str{cpy,cat,ncat}, stpcpy, (v)sprintf,
  (v)snprintf, (v)printf, (v)fprintf (probed from gcc; the mem*_chk ones do
  not). pr62090. Golden `misc_252`.
- Parameter declarators no longer give up past 16 array dimensions / 32
  levels (`parm_of` sizes its buffers from the parameter's subtree), so
  -Warray-parameter / -Wvla-parameter work on pr100619's 1100-dimension
  types. Golden `misc_253` (60 dimensions). (`cparm_typedef` was
  capped at 16 dimensions until addendum 4.)
- Parked: Warray-parameter-11. gcc folds `!__builtin_copysign(~2, 3)`,
  `(int)__builtin_fabs(-4.0)` and `(intptr_t)((char*)&sp->a[1] - (char*)sp)`
  to constants in an array bound (so "ISO C forbids zero-size array" and a
  plain array mismatch), while cereal calls them non-constant (VLA). Needs a
  value evaluator for built-in math calls and same-object address
  differences, not just the K_FOLD classification.

Round 164 addendum 3 (gcc.dg 3736/3744):

- `strlen` of a flexible array member of a const object no longer warns
  "offset 0 outside bounds of constant string" (its extent was taken as 0).
  The aggregate-initializer path now sizes it to the object's end; the
  plain const-member path gives up (a false negative: gcc still reports
  `strlen(ay.a)` without a nul and `ax.a + 9`). strlenopt-78, golden
  `misc_254`.

Round 164 addendum 4: `cparm_typedef` sizes its dimension list from the
declarator's subtree (no 16-dimension cap), golden `misc_255`. Warray-parameter-11
re-surveyed and stays parked: the pointer difference
`(char*)&sp->a[1] - (char*)sp` needs a constant kind for "pointer variable
plus offset" (K_ADDR's base is an object's address, so reusing it would make
`int *p = sp;` look constant), and the test needs the built-in math fold
too, so neither alone fixes it.

Round 165 (corpus spot-check, parity unchanged): `bench/tools/corp.sh DIR` compares
cereal and gcc-13 diagnostics file by file under `-std=gnu99 -Wall -Wextra` on
real code (lupa's Lua sources, libuv). It found two false positives, both
fixed and pinned (goldens `misc_256`, `misc_257`):
- `-Waddress` "will never be NULL" (`lgc.c` `markobject`): gcc stays silent
  when the operand being tested begins inside a macro expansion
  (`if (ID (&p->b))`, `if (ADDR (p))`), and warns again behind a caller's
  parenthesis, `!`, `!=` or the right side of `&&`. Applied in
  `cexpr_truth_warn` when the warning location is the operand's own start.
- `-Wmisleading-indentation` (libuv `tree.h` `RB_*`): the guard, body and next
  statement all inside one outer macro whose body and next statement come from
  a nested macro defined above the guard are not reported (`misleading`).
Parked: a guard inside macro V whose body is a nested macro and whose next
statement is an argument token (`if (o) COL (o) = 1; t->c = 2;`) still gets
cereal's `-Wmultistatement-macros`; gcc is silent, but the obvious guard
(`!kn`) regresses `macro_chain` and `multistatement`, so the rule is unknown.

Round 165 addendum: macro names are spelling candidates (gcc's
`lookup_name_fuzzy` includes user macros). `pp_macro_names` (pp.c) lists the
non-builtin macros live at the preprocessor's current point, and `fuzzy_name`
(cexpr.c) offers them through `CheckOptions.macro_names`. A sequential run
reads `tu.pp`; a `--cells` run reads the open cell cursor's `PP`, which is at
that cell's version. The end-of-file state would suggest macros defined later
and miss ones `#undef`'d later. Golden `misc_258` pins both (the goldens run
in both modes). uvloop's libuv `src/unix` now matches gcc-13 on all 50 files
(`CPU_SETSIZE`, `IOV_MAX`, `CPU_ISSET`, `sched_getaffinity` suggestions).

Round 165 addendum 2 (corpus sweep: every lupa Lua/LuaJIT directory and libuv
`src`, `src/unix` now match gcc-13 file for file): `-Wunused-value` for
`c && (foo (), 0)` as a statement. gcc's `warn_if_unused_value` lets a comma
whose last operand is constant pass (LuaJIT's `incr_top`); any other `&&`/`||`
statement still warns at the operator, even with a call on the right (the
operand becomes `!= 0`). Golden `misc_259`. Parked: libuv's `posix-poll.c`
(a file that does not compile on Linux) gets gcc's "parameter 'l' set but not
used" at line 78, which cereal omits; not pursued.

Round 165 refactors (no behaviour change; goldens, parity and the sanitizer
build unchanged, callgrind 3.837 G to 3.842 G):
- Macro provenance: `ptok_in_macro` / `ptok_loc` (ast.h) replace the copied
  `from_macro` and six open-coded `exp ? exp : loc`; `diag_expansion_notes`
  (diag.c) replaces the twin note builders in check.c and parse.c. The flag
  based `tok_from_macro` (cstmt_warn.c) stays separate on purpose: it is true
  for an argument token whose `exp` equals its location, the `exp` based one
  is not.
- One `strip_paren` (check_int.h; three copies before) and `cast_operand`
  (cexpr_int.h) for the cast-peeling loops. Sites with their own conditions
  (lg_strip, the `+ 0` folds) keep their loops.
- `quiet_lift` / `quiet_lift_before` / `quiet_restore` (check_int.h) name the
  "gcc reports this while parsing, so a unit with a syntax error still gets
  it" pattern at four sites. The `N_DECLARED` site mixes in diagnostic order
  and `first_err_params` and keeps its own code.
Not done (surveyed): splitting `grok` (about 740 lines), `declared_visit`,
`assign_check`, and moving the struct/enum and function-definition code out of
`cdecl.c` into their own files. Low risk to move, no motive yet.

`cdecl.c` split survey, step 1 (no behaviour change; goldens, parity and the
sanitizer unchanged, callgrind 3.8416 G). Counting what each candidate region
needs from the rest of `cdecl.c` (static helpers defined outside, used
inside): merge/redeclaration 5, records/enums 14, function definitions 28 (plus
`GDecl`). So function definitions is the costliest split, not the cheapest.
Done: the type predicates `tkind`, `is_err`, `is_arr`, `is_void`, `is_func`,
`is_prototype` now live once in check_int.h (cexpr_int.h, cinit.c and cdecl.c
each had a copy); `sym_public/external/defined`, `ref_file_scope`,
`extern_inline`, `sname`, `in_extension` moved to cdecl_int.h. Next, if
wanted: merge -> `cmerge.c` (about 8 exports), then records -> `crecord.c`
(about 14). Function definitions stays parked.

`cdecl.c` split, step 2: the duplicate-declaration code (`merge_decls`,
`duplicate_decls`, `diagnose_mismatched`, the inline/weak/optimize attribute
merging, `func_err_param`; 821 lines) moved to `cmerge.c`. Six functions are
exported through cdecl_int.h under the `cdecl_` prefix (`duplicate_decls`,
`inline_given`, `inline_follows`, `locate_old_decl`, `weak_apply`,
`typedef_under`); no types moved. `cdecl.c` is 5754 lines. No behaviour
change: parity unchanged, 1473 goldens (the self-check walks `src/c`, so a new
file adds one), sanitizer 0 findings, callgrind 3.8415 G.

`cdecl.c` split, step 3: the struct/union/enum/member code (tags,
finish_struct, enumerators, members, `_Static_assert`, `#pragma pack`; 1476
lines) moved to `crecord.c`; `cdecl.c` is now 4235 lines. Eleven
functions leave through cdecl_int.h (the eight visitors, `flex_struct`,
`struct_semis`, `typedef_tag_clash`) and eight come in (`grok`, `pushdecl`,
`pending_xref`, `find_spec`, `pop_specs`, `shadow_tag`, `dep_spec_use`,
`scan_end`), all `cdecl_`-prefixed. `GDecl`, `min_prec` and
`is_incomplete_array` moved to cdecl_int.h. No behaviour change: parity
unchanged, 1474 goldens, sanitizer 0 findings, callgrind 3.8422 G (+0.02%,
the calls now cross a translation unit). The function-definition split stays
parked: it would need about 28 imports for 13% of the file.

Round 166: `-Wunused-but-set-parameter` with an erroneous subscript. libuv's
`posix-poll.c` (written for a loop struct this Linux build lacks) gave gcc
"parameter 'l' set but not used" where cereal gave nothing: gcc's
`build_array_ref` returns at once when either operand is erroneous, before
the index is marked read, so a plain-identifier operand (parentheses allowed)
stays unread. `read_scan` now skips such operands (`err_index_mark`, cdecl.c);
compound operands (`i + k`) and other operators still read as before. Checked
against gcc-13 on `a.c`-style cases (assignment target, `i[err]`, nested
subscripts, undeclared base). Golden `misc_260`. `posix-poll.c` now matches
gcc-13, so the parked item is closed; the whole libuv unix directory is again
identical file for file. Also cleared the build's nine warnings (`bn` and
`k` shadows, missing initializers in `SlRes` and the `FieldIn` va_list tables);
callgrind is unchanged at 3.8422 G.

Round 167: the parked `-Wmultistatement-macros` case is closed, and the
guess behind it is gone. gcc compares macro maps (which expansion of which
macro holds a token); an argument token belongs to the macro it was
substituted into. cereal had no such identity and derived one from spelled
locations, with argument tokens as wildcards, so `if (o) COL (o) = 1; t->c =
2;` inside a macro V (body from the nested COL, next token an argument of V)
warned where gcc is silent. The `!kn` guard could not tell this from the
cases that must warn.
General mechanism: the preprocessor already knows, when it hands a token out,
which macro context it came from; `pp_next` now records that context's
invocation location (`out_mloc`), the token sources pass it on, and `PTok`
carries it as `mloc`. `multistatement` compares `mloc` (with the expansion
point) instead of the spelled-location derivation and its "locations never go
back" check, both deleted. `tok_macro` stays for `-Wduplicated-branches`,
which needs the definition, not the expansion. Golden `misc_261` (six
shapes, checked against gcc-13). Parity unchanged; callgrind 3.8397 G
(-0.07%, the derivation was costlier than the field); libuv `src` and
`src/unix` still match gcc-13 file for file. Not tried: moving
`-Wmisleading-indentation`'s macro handling onto `mloc` (it works from
spelled positions, not macro identity).

Round 168: three notions of "token from a macro" are now one. Audit of the
macro-provenance helpers (about 60 sites): the parser flag behind
`tfrom_macro`, `ptok_in_macro` (`exp != loc`) and cstmt_warn.c's
`tok_from_macro` agreed on every gated case; they differed only for an
argument substituted into a macro, which `exp != loc` called "not in a
macro" when the argument was pre-expanded in place. All three are now
`PTok.mloc != 0` (`ptok_in_macro`); `tok_from_macro` is deleted and its five
uses call `tfrom_macro`. Parity, 1478 goldens, sanitizer files and libuv
unchanged; callgrind 3.8396 G. Left alone: about ten sites that build a
presentation location by hand (`exp ? exp : loc`: ast.c, ccall.c, cconv.c,
cexpr.c, cformat.c, check.c, cinit.c, cstmt_warn.c); only some equal
`ptok_loc` (others fall back to a different location), so folding them is a
separate per-site step.

Round 169: `__builtin_`-only built-ins are typed when called. The lupa sweep
found luajit `lj_tab.c:294` (and `ljamalg.c`) with "parameter 'bins' set but
not used" from Round 166's `err_index_mark`: in `bins[(k > 2 ?
__builtin_clz (k - 1) : 0)]++` the `gnu = 2` built-ins (clz, ctz, bswap32,
...) were never typed as functions, so cereal saw an erroneous index and
skipped the base; gcc types the call `int`. Same gap hid every
-Wint-conversion and friends on their results. Fix, one mechanism: the
identifier path now types a `gnu = 2` entry as its table signature when it is
the callee (any other use is still rejected by `reject_builtin`); `bt_type`
parses the unsigned/short/`_Bool`/`__int128 unsigned` names; the table's
`gnu = 2` rows are regenerated by `bench/tools/merge_builtin_pfx.py` (126,
from gen_builtin_pfx.py, keeping rows whose types parse; the overflow family,
speculation_safe_value_{1,2,4,16} and va_start/end/copy are skipped as typed
elsewhere or special forms). A row with no parameters (`int|`) is typed
unprototyped, since those are the built-ins gcc checks by hand
(`__builtin_classify_type`, `__atomic_is_lock_free`): typing them with an empty
prototype first cost 5 gcc.dg and 2 c-c++-common files ("too many arguments").
Golden `misc_262`; parity unchanged; luajit 20/21 and libuv match gcc-13 file
for file; callgrind 3.8518 G (+0.3%, the larger table). Not covered: rows with
`_Float*`, `_Decimal*`, `__bf16` and function-pointer types stay out.

Round 170: the two hand-built `exp ? exp : loc` sites in parse.c (`tok_loc`,
`eof_input_loc`) call `ptok_loc`. The other `.exp` sites fall back to something else
(`expr_loc`, `first_loc`, the unchanged location, system-header-only), so they stay.
`bt_type` also parses `_Float16/32/64/32x/64x/128`, `__bf16` and `_Decimal32/64/128`,
so 26 more `gnu = 2` rows are typed (152 in all; no testsuite file differed, but
`int *p = __builtin_huge_valf64 ()` now gets gcc's error, in `misc_262`). Only
`__builtin_apply`'s function-pointer parameter stays out. Callgrind 3.8630 G.
Corpora matching gcc-13 file for file: zstandard (18 files; `zstd.c` is the
amalgamation), jemalloc `src` (63), freetype `src/base` (39), zlib-ng (25).

Round 171: two constant folds gcc does and cereal did not, both found by
`Warray-parameter-11.c` (an array parameter's bound that gcc folds to a
constant was a VLA bound to cereal: `-Wvla-parameter` instead of
`-Warray-parameter`, and no zero-size `-Wpedantic` warning).
* `ptr_diff` folds `A - B` when both are a plain non-volatile pointer variable
  plus a constant offset (`sym_ptr`, through casts, `&`, `->`, `.`, constant
  `[]`, array decay), same variable: `(char *) &sp->a[1] - (char *) sp` is 4.
  The existing K_ADDR case (a symbol's address) is the other half of the same
  idea; offsets from a pointer variable's value were the gap.
* `fold_math_builtin` (ccall.c): `__builtin_fabs` and `__builtin_copysign`
  (also f, l) of constants are K_FLOAT. Only these, because they are exact
  and need no libm (the build links without it); floor, ceil, fmin, ... would
  want a decision about -lm first.
Golden `misc_263`. gcc.dg 3737/3744.

Round 172: `::` is one token in the GNU modes. libcpp lexes CPP_SCOPE when the
language allows it (gnu89 to gnu17, C2X; not the strict ISO modes), so
`CONCAT (::, >)` pastes onto it and a stray one is named by its spelling.
`LexOptions.scope` (set by `-std=gnu99`), `P_COLONCOLON`, the fast and slow
lexer paths; the attribute-scope code (`[[ns::name]]`) tests the token
instead of two adjacent colons (the TF_SPACE check goes with it), and the asm
section loop treats `::` as an empty section followed by the next (libcpp
users do the same: `asm ("" :: "r" (x))`). Golden `misc_264`. gcc.dg/cpp
265/266. Known: cereal's note for a paste error inside a macro body says "in
expansion of macro" at the use, gcc says "in definition of macro" at the
body; the message and position of the error itself match.
Parked: `#pragma GCC unroll j` with a non-constant `j` (c-c++-common/unroll-5.c)
needs the argument judged with symbol information. The parser's judge
(`unroll_arg_bad`) is textual and leaves names alone; making it right means
checking the pragma in the checker, with the real expression evaluator, not
adding a name case to the text scanner.

Round 173: two diagnostic placements.
* Overflow in a binary expression with a folded binary operand
  (`(1 << 31) - 1`): gcc folds that operand to a bare constant without a
  location, so it prints the value (`'-2147483648 - 1'`) and reports at the
  start of the current line. `fold_flush` does the same when an operand is a
  number or a folded binary, at least one the latter; a folded cast keeps its
  location and stays as before (`misc_79`).
* Format substring columns: gcc rereads the literal from its presumed line
  (after `#line` or a linemarker). Where that text is missing (other presumed
  file, line past EOF) or is not the literal, the warning sits at the
  literal's start. `fmt_decode` clears `exact` the same way
  (gcc.dg/cpp/pr66415-2.c). Golden `misc_265`. gcc.dg 3738/3744, cpp 266/266.
Parked: gcc.dg/pr100547.c (vector_size error position). cereal reports
`number of vector components` at the attribute (`vs_loc`); gcc reports at the
start of the right operand of the size expression's outer binary operator
(`P * sizeof(short)` at 34:5), also across lines, for `big * 2`, `2 * big`,
`big + 0`; but `(1 * big)` or `(((big)))` on one line report at the opening
parenthesis and a bare literal at itself. No single rule found yet (probing
with ~/ps/v1.c, v2.c shapes); needs gcc's folded-constant location rules.

Round 174: the note after a paste error. libcpp names the macro by where the
lhs came from: an lhs that is a parameter sits at the use, so the note is
"in definition of macro" at the parameter in the body (it replaces the
Round 172 "known" difference); an lhs written in the body keeps "in
expansion of macro" at the macro name. `paste` (ppexpand.c) tests
`op[-1].flags & TF_PARAM`. Golden `misc_266` (identical to gcc, valid
follow-on tokens so no parse errors). Not done: gcc also prints "in
definition of macro" for a *parse* error at a token that came from a
parameter (`G(.)` -> "expected expression before '.'" at the argument, note
at the body parameter); cereal prints "in expansion of macro" at the use.
Notes are not compared by verify.sh; parked.

Round 175: a goto into the scope of a pointer to a typedef'd VLA. gcc declares
a nameless declaration for each pointer whose target is variably modified
only when the VLA is written in that declarator (`int (*p)[a]`); through a
typedef name (`A *p`, `B p`) the typedef's own declaration already counts, so
no extra error. `cstmt.c` stops the nameless-declaration walk at a typedef
(`pr108375-2.c` duplicate error). Golden `misc_267`. gcc.dg 3739/3744.
Left as is, on purpose: `darwin-cfstring-format-1.c` and `pr105522.c` are
`target *-*-darwin*` tests; gcc-13 on Linux has no CFString format kind or
`__builtin___CFStringMakeConstantString`, so the "expected" diagnostics are
that build's errors about a feature that does not exist for this target, and
matching them means emulating missing target support. `dump-ada-spec-14.c`
needs `-fdump-ada-spec` (gcc's Ada binding dumper warns "packed layout" while
dumping); implementing the dumper to reproduce one warning is out of scope.
`pr56355-1.c` (-Wstrict-overflow) and `pr83844.c` (-Wif-not-aligned with a
symbolic offset) depend on the middle end's folding; parked.

Stepped back on macro notes (design, nothing built). Probe `n1.c` against
gcc-13 gives one rule for the whole family: the notes for a token come from
its chain of macro expansions, innermost first. A token written in the body
of M gets "in expansion of macro M" at M's use. A token that M's parameter
substituted (argument text, or text from an enclosing macro's body) gets
"in definition of macro M" at the parameter's position in M's body instead,
and the chain goes on with the enclosing expansions only (`K(.)` through
`W` to `G`: definition of G, expansion of W at its place in K's body,
expansion of K). cereal has three mechanisms for this: `pp_macro_chain`
(searches definitions for the spelled location, fails for argument tokens,
then falls back to the identifier at the use), `pp_add_expansion_notes`
(live context stack, preprocessor errors only) and the Round 174 paste
special case. One mechanism covers them all: a per-token provenance, as
libcpp keeps (macro, parameter position or body position, parent), instead
of recomputing it from locations. It would cost a field on `PTok` (callgrind
and memory sensitive, since every token carries it) and touches only note
lines, which verify.sh does not compare. Parked until a corpus or golden
needs the notes; the Round 174 special case stays until then.

Round 176: `__VA_OPT__` (ROADMAP A1). Before this, `-E` left the token in the
output in every mode. Semantics probed against gcc-13 (libcpp, P1042R1):
`__VA_OPT__ ( content )` in a variadic macro yields the content, with
parameters substituted, when the variable arguments *expand* to at least one
token (`F(p,EMPTY)` counts as absent), and a placemarker otherwise, so
`x ## __VA_OPT__(..)`, `__VA_OPT__(..) ## y` and `#__VA_OPT__(..)` (string of
the substituted content, `""` when absent) work through the existing `##`
and `#` paths. `subst()` handles it inline (`vaopt_fill` substitutes the
content through a recursive `subst` on a sub-macro view of the body). Define-time
checks match libcpp: unterminated, not followed by `(`, nested, `##` at either
end of the parentheses (errors with the token's range); outside a variadic
body, or in a GNU named-variadic macro, a pedwarn "can only appear in the
expansion of a C++20 variadic macro" (the named form still expands, as in
gcc); under `-std=c99 -pedantic` every use warns "not available until C2X"
(no range on the warnings, as in gcc). Found on the way and fixed at the root
in the same place: an empty argument's spacing was dropped (`f(x y)` with
empty `y` printed `f(1)`, gcc prints `f(1 )`), and a first token after empty
arguments took the macro name's spacing instead of its own (`J()` with body
`x z` gives ` z`). Tests: `tests/pp/va_opt.c` (token diff vs gcc-13, both
modes) and `tests/pp/err_va_opt.c` (recovery). Left as is: after an empty
expansion at the end of a line, the next line's first token gets a leading
space in `-E` (also for `Q()` with `#define Q(x) x`; predates this round).

Round 177: shared build-option handling (ROADMAP A2). The command line used to
stop at the first option it did not model (`-c`, `-fPIC`, `-MD`, `-x`, `-lm`
were all fatal), while the language server kept its own skip list in
`config.c`. Both now call `option_ignored()` in `driver.c`, one table
(`ignored_opts`): silent (link, codegen, `-g*`, `-W[la],`, `-march=`, value
options such as `-Xlinker`), dependency output (`-MD -MMD -MP -MG -MF -MT -MQ`:
consumed, with a note that no `.d` file is written), and options that may
change diagnostics (`-ffreestanding -fno-builtin -funsigned-char -fwrapv
-fno-common -fpack-struct -fms-extensions -fshort-wchar -m32 -mx32
-idirafter -imacros -isysroot --sysroot`: consumed, one note each on stderr,
never silent). `-m64` and `-fsigned-char` are silent because the host is
x86-64 where `char` is signed. `-x c`/`none` pass; any other language is an
error (exit 1). Not implemented, only reported: `-m32` (the predefined
macros and headers are the host's, so selecting the i386 target alone would
give a mixed state), `-idirafter`, `-imacros`, `-isysroot`. The language
server sets `Options.lenient` (no notes, no `-x` error) and still skips `-o`
and `-E` itself. Test: section 13 of `tests/run.sh` (each silent option
leaves the diagnostics of an erroneous file unchanged, each reported one is
reported, `-x c++`/`-x c-header` fail). Not done: a harness that runs the
list through gcc-13 `-fsyntax-only` too (the gcc side only needs the options
to be valid, which the build lines already are).

Round 178: `-std=c11`, `c17` and their `gnu` and alias forms (ROADMAP A3).
`Options.std_year` (1999, 2011, 2017) beside `gnu_mode`; one table
(`std_names`) maps all gcc-13 spellings (`c1x`, `iso9899:2011`, `c18`, ...).
`gcc-13 -dM -E` across the six standards differs from c99 only in
`__STDC_VERSION__` (201112L, 201710L), `__STDC_UTF_16__`/`_32__` (every C11
or later mode, and gnu99) and, for gnu modes, `__STRICT_ANSI__`, `linux`,
`unix`; those are all it takes. Strict C11 and C17 have trigraphs, `u`/`U`/`u8`
literals and no `::`. `cped11` (the "ISO C99 does not support ..." pedwarns)
is silent from C11 on. The LSP's `-std` hack (map other values to gnu99) is
gone: `Options.lenient` ignores an unknown `-std=` there, the CLI stays fatal
(`c2x`/`gnu2x`/`c23`, `c++*`). The parity harness (`par.py`) now runs the gcc
tests with any of these `-std=`.
Parity, old and new together: the gcc.dg files run grew from 3744 to 3910
(166 C11/C17 tests that were skipped), identical 3739 -> 3866, differ
5 -> 44; c-c++-common 634/636 unchanged; gcc.dg/cpp 266/266 -> 281/285. No old
file changed. The 39 + 4 new differences are the next work, grouped by cause
(counts are diagnostics, not files): `#elifdef`/`#elifndef` in the GNU modes
(gcc accepts them with "before C2X is a GCC extension" under -pedantic, 4
files); the C11 atomic member and `_Atomic` initialisation checks
(c11-atomic-1/3/4); `_Alignas` in specifier-qualifier lists and in `sizeof`
(c11-align-5/8); the incomplete-pointer comparison (c11-compare-incomplete-1/2,
it is a pedwarn only before C11 and under -Wc99-c11-compat); the empty
initializer-braces warning option (`-Wc11-c2x-compat` in C11, not
`-Wpedantic`); `_Static_assert` without a message in C11 (c11-static-assert-7/8:
"ISO C11 does not support omitting the string"); qualified void/volatile
return types (qual-return-5/6/8); enumerator range, `[[]]` attributes, compound
literal storage class and `...` without a named argument before C2X
(cereal says -Wpedantic; gcc is silent in C11 mode for some of them); typedef
redefinition with a function type (c11-typedef-1); lvalue-9; and a handful of
single files. Tests: `tests/ppstd/std_macros.c` (token diff vs gcc-13 under
nine spellings), goldens `std_c11` and `std_gnu17`, section 13 of `run.sh`.
Found on the way: `std_year` first held 99/11/17 and `>= 11` was true for C99
(every pedwarn vanished in C99 mode); the encoding is now the real year.

Round 179: `#elifdef` / `#elifndef` in the GNU modes (first group of the Round 178
triage). The directives exist only when `gnu_mode` (interned at start-up, so
in the ISO modes they stay unknown directives, as in gcc-13); `do_elif_else`
takes the directive id and evaluates the `#ifdef` / `#ifndef` form (bad or
missing macro name, poisoned name, extra tokens) with the real directive name
in "without #if" and "after #else". libcpp's pedwarn ("#elifdef before C2X is
a GCC extension", only under -pedantic, an error under -pedantic-errors) is
not emitted per directive but when the directive decides: at the end of the
line (past a trailing comment, or at the first extra token) when the test is
true, at the directive name when an earlier group was taken and this group was
active, and never when the test is false or the group was already skipped.
Tests: `tests/ppstd/elifdef.c` (token diff under gnu99/gnu11/gnu17) and the
golden `check/elifdef_pedantic`; gnu11-elifdef-1..4 now match gcc-13.

Round 180: `_Alignas` and `_Atomic` parity (second group of the Round 178
triage; c11-align-5/8, c11-atomic-1/3/4 now match gcc-13). `_Alignas` is
accepted in a type name only where gcc parses it (sizeof, _Alignof, cast) and
then reported ("alignment specified for type name in ..."); elsewhere
`specs()` leaves it, giving gcc's "expected specifier-qualifier-list" and
"expected ',' / ')'" errors, and the failed type name is skipped as an error
node. Atomic: member access warns ("accessing a member 'x' of an atomic
structure"), `++`/`--` on an atomic pointer reports the pointer-sum
diagnostics, `_Atomic void *` is not `void *` in `?:`, compound-assignment
operand messages drop `_Atomic`, parameters keep `_Atomic` in function types
(K&R conflicts via `kr_promote`), and an atomic char array from a string
literal is rejected. Goldens: alignas_type_name, atomic_member_access,
atomic_ptr_arith, atomic_param_init. gcc.dg identical 3866 -> 3869.
Not done: other `TYPE_UNQUAL` uses on element types may also need to keep
`_Atomic`.

Round 181: C11/C2X compat-option parity (the remaining Round 178 sibling
groups plus the c11-* tail; gcc.dg identical 3869 -> 3893). One mechanism,
`cpedstd` (`cped11`/`cped2x` in check_int.h), models gcc's pedwarn_c11/c2x:
a pedwarn below the feature's standard year, an error under
-pedantic-errors, hidden by -Wno-OPT, and an explicitly given
-Wc99-c11-compat / -Wc11-c2x-compat fires in every mode (plain warning, tag
= the option). Everything is suppressed under `__extension__`. Users:
incomplete pointer comparison, `_Static_assert` (C11 form, omitted message),
empty initialiser braces, enumerator range / enum underlying type,
`[[]]`, compound-literal storage class, `...` without a named argument,
array qualifiers, decimal float, labels before declarations, `#warning`
and binary constants (libcpp variants). Also: DR#423 qualified function
return types (dropped in C11 except `_Atomic`), typedef redefinition with
VLA bounds (`type_vla_blind`), comma operands both converted to rvalues,
`__builtin_complex` constant initialisers, DR#444 `_Alignas` in compound
literal type names, `_Alignas(type)` validation, and an anonymous
struct/union member counting as named in C11+. Goldens: c11_compat_opts,
c11_compat_opts_ped, qual_return_c11, typedef_redef_vm, comma_incomplete,
complex_const_init, c11_anon_alignas. Not done: c11-noreturn-5,
c11-static-assert-3/4, c11-uni-string-2, c17-nullptr-2, array-10 (each a
one-off diagnostic, disproportionate).

Round 182: the Round 181 tail plus the gcc.dg one-offs (gcc.dg identical 3893
-> 3904, c-c++-common 634, cpp 285; tests 1611). Part A (each a shared
mechanism rather than a per-file patch): function specifiers (`inline`,
`_Noreturn`) are refused in struct members and type names like gcc's fspec_ok
(`specs(..., fspec_ok)`); `_Static_assert` reports an overflowed constant
(`cconst_overflow`) and is accepted as a for-init declaration and in K&R
declaration lists; string concatenation reports one error per differing
prefix piece (plus one when pieces 0 and 1 differ); `nullptr_t` is a builtin
typedef in every C mode (parser `builtin_types` + `predeclare`, replaced by the
first user typedef); `#elifdef`/`#elifndef` are in the directive "did you
mean" list and an exact name gives no hint (ISO modes); a function definition
missing its `{` merges its parse error at end of input (ORD_EOF) and the
parameter diagnostics of a body-less declaration keep `oloc` = end of input;
builtin-mismatch notes use `header_note_loc`.
-Wdesignated-init now follows process_init_element: the warning fires when an
element arrives at a designated_init struct level (before exhausted
brace-elided levels are popped), replacing the per-path `dwpath` bookkeeping.
Part B: pr59717: `cbuiltin_tab.h` regenerated against cc1 (+529 rows: lrint
llrint lround llround cabs carg cimag creal ilogb* fe* isw* tow* and the gnu-only
_FloatN families; sincos / aligned_alloc now name `<math.h>` / `<stdlib.h>`);
fenv_t / fexcept_t built-in parameters are void * until declared (like FILE *,
`bt_file_param` kinds 3-6); strftime / strfmon formats get the zero-length and
nul checks (`fmt_termonly` now holds the kind name; `format(strftime|strfmon)`
attributes are kinds 3/4); `cabs` no longer special-cased in warn_for_abs.
pr60195: reading an `_Atomic` lvalue is an atomic load, so `y;` is not
"statement with no effect". pr61077: -Wmain "'_Atomic'-qualified parameter
type" (anywhere along the pointer chain). pr61817-1: under
-ftrack-macro-expansion=0 `__LINE__` is the line of the outermost call's name
(`pp->inv_loc`, gcc's invocation_location); the parallel/cell workers now
inherit the unit's DiagEngine options through `diag_init_worker` (they had
lost `track0`). pr62024:
`__atomic_always_lock_free` / `__atomic_is_lock_free` have type _Bool, fold
(fold_builtin_atomic_always_lock_free on x86-64: sizes 1/2/4/8, alignment of
the pointee, no 16-byte cmpxchg) to K_FOLD, and are const calls; the
_Static_assert diagnostics use `value_loc` (the call start, the `?:` colon,
the first token for bare identifiers). pr63453: gnu_inline declarations
(attribute anywhere) are not "inline function declared but never defined".
ucnid-15-utf8: a stray-character error after an identifier is lexed as that
identifier's lookahead and so precedes the declaration's own diagnostics.
Goldens: kr_proto_eof, fspec_member, static_assert_for_ovf, kr_static_assert,
string_concat_prefix, nullptr_t_builtin, elifdef_hint_iso,
designated_init_arrival, builtin_tab_more, builtin_fenv_ptr, line_track0,
atomic_lock_free_fold, static_assert_loc, gnu_inline_nodef,
stray_after_ident. Not done (parked, docs/STATUS.md): see "Parked front-end
gaps".

Round 183: constant folding of integer built-ins (the Round 182 "noticed but not
fixed" pair). `fold_int_builtin` (ccall.c, after `fold_math_builtin`) folds
ffs/clz/ctz/clrsb/popcount/parity (suffix "", l, ll, imax), bswap16/32/64 and
abs/labs/llabs/imaxabs of a constant integer to K_ICE, as gcc's
fold-const-call does (clz/ctz of 0 stay calls). `__builtin_expect` and
`__builtin_strlen` of a constant are K_ICE too (they were K_FOLD/EF_CST and got
the -Wpedantic "not an integer constant expression"). Root cause of the
rejected popcount/clz/ctz/parity/bswap: `src/c/cbuiltin_pure.h` was a stale
subset of `bench/tools/gen_pure_builtins.py` output, so those calls carried
EF_SIDE; regenerated (564 lines), which also gives the missing "statement with
no effect" for them. Golden: builtin_fold_ice (silent under gcc-13 at c11
-pedantic-errors and c99 -pedantic). gcc.dg 3904, c-c++-common 634, cpp 285
identical (unchanged); tests/run.sh 1613 passed.

Round 184: the last gcc.dg parity diffs (gcc.dg identical 3904 -> 3908,
c-c++-common 634, cpp 285; tests 1621). Four were front-end gaps, each fixed
once in the shared place; two (Darwin-only tests that gcc-13 runs on Linux)
fell out of the first two:
- Built-in parameter types (`bt_type`, cexpr.c): a trailing ` const` after a
  pointer ("char * const*" in execv/execve/execvp) was parsed as an error type
  and every call got "passing argument 2 ... incompatible pointer type"
  (Wstringop-overflow-22). Golden builtin_const_ptr_param.
- The position of every `vector_size` diagnostic (pr100547): gcc applies the
  specifiers' attributes when the declarator is done, so the error sits at
  input_location with the parser's lookahead at the token after the
  declarator, i.e. the first token of that token's line (`iloc`). It was the
  attribute's own line. `attr_apply_type` now takes that location from its
  three callers (`vs_loc` is gone); the specifier path finds the declarator end
  with `declarator_end` in cspec.c. Probed against gcc-13 with `;`, `,`, `=`,
  `)` as the lookahead and with the declaration split over lines. Golden
  vector_size_loc.
- `format` / `format_arg` against the function type (new `format_attr_check`
  in cattr.c, through `positional_arg` which gained a mode): the format-string
  parameter must be a pointer to plain char ("refers to parameter type", an
  error; `unsigned char *`, `void *`, `CFStringRef`, `char * const*` all fail),
  the first-argument position must be 0 or the `...` ("does not refer to a
  variable argument list", "argument to be formatted is not '...'",
  "strftime formats cannot format arguments"), and `format_arg` must return a
  char pointer ("function does not return string type"). Position problems
  warn (-Wattributes) like alloc_size's. `format_kind` is shared with the
  attribute reader. `format_arg` arguments are now evaluated like the other
  expression attributes ("undeclared here"). This closes
  darwin-cfstring-format-1 (CFString is an unrecognized archetype on Linux, so
  `CFStringRef` parameters are errors). Golden format_attr_params.
- A call of an unknown `__builtin___X` name is an implicit declaration
  (the fortify `_chk` family and `__builtin___clear_cache` are the known ones);
  closes pr105522 (`__builtin___CFStringMakeConstantString`, with the
  int-to-pointer cast warnings). Other unknown `__builtin_*` names are still
  silent: the special built-ins are dispatched by name in `e_call` after the
  callee is visited, so telling them apart from unknown ones needs that list
  (parked; gcc also adds a "did you mean" hint). Golden builtin3_implicit.
Not fixable in a front end (STATUS "Open differences"): pr56355-1 and
pr83844 need fold-const.

Round 185: `#pragma GCC unroll NAME`. The text scanner (`unroll_arg_bad`) now
judges an identifier by the parser's scope: an enumerator (new `SYM_ENUMERATOR`
kind, declared where enumerators were `SYM_ORDINARY`) stays valid, any other
declared ordinary name (variable, function, `const int`) is not an integer
constant expression and gets gcc's error, as in gcc-13. An undeclared name
is still silent (gcc adds an "undeclared" error first). unroll-5 now matches;
golden unroll_name.

Round 186: ROADMAP B1, compiler diagnostics in the language server. `parse_one`
moved out of main.c into `frontend_run` (`src/c/frontend.[ch]`, options in
`FrontendOpts`; the CLI keeps a thin `parse_one` that prints and frees); it
now also stops at a set `pp.cancel` flag (no `checker_finish`, no further
units). The builder runs it as a second phase after the macro snapshot has
published: a fresh sequential TU over the same overlay, options from
`config_options_for` (so `-std` and the compile command's flags apply) with a
fatal missing include as on the command line, the unit's cancel flag, then
the TU is dropped. `u->building` stays set through the phase so `waitIdle`
covers it. Publication is under the server lock and only if `u->want` is
still the generation the overlay was captured at (stale and cancelled runs
publish nothing). `lsp_publish_diagnostics` takes the check TU and writes
the macro phase's diagnostics then the compiler's, dropping those with the
same range and message. Units whose sources total over 4 MiB are skipped
(`CEREAL_LSP_CHECK_MAX`): a dense single-initializer table costs 48 bytes of
parser tokens per source byte (194 MB at 4 MB, 420 MB at 9 MB), real code
about 4 bytes per byte. The second preprocessor pass is 30 to 45% of the
phase; small files need it (their cells are not kept), large files keep
cells but their tokens are not, so reuse is parked. Phase times: 33 KB 0.05 s,
2.2 MB zstd.c 0.34 s. Shutdown now cancels running builds. Golden
`tests/lsp/diag` (undeclared identifier, type error, two quick edits, a
missing include reported once); `tests/lsp/basic.expected` gained the
compiler errors of its broken edit. Parked: the check phase blocks the single
builder thread for other units; compiler diagnostics vanish between an edit's
macro publication and the check's; B2 (index) must keep the TU past the phase.

Round 187: ROADMAP B2 phase 1, the C symbol index (B2_DESIGN.md). New
`src/c/csymidx.[ch]`: a builder fed by about 20 hooks (`csx_*`: `cdecl_pushdecl`
now takes the name token; parameters' DEF upgrade in bodies and K&R lists;
tags in `cdecl_open_visit`, `xref_visit` and `cdecl_shadow_tag`; fields via a
new `FieldIn.tok` after `type_complete_record`; uses in `e_ident` (skipping
attribute arguments, `attr_takes_expr`), implicit declarations, typedef names,
member access, designators, offsetof; labels at `new_label`, definition, use
and `__label__`). The hooks are no-ops unless `CheckOptions.symidx` is set,
so `cereal check` and parity are unchanged. `csx_finish` resolves tentative
roles, classifies macro tokens against `#define` ranges, sorts, dedupes,
hashes files and builds the CSR; the TU need not outlive the check. Measured:
16 B per event as estimated, 25 B per decl before hover; zstd.c 1.37 MB,
cexpr.c 0.52 MB, main.c 0.20 MB; check time within noise. `cereal check
--dump-symbols` and `--verify-symbols` (every resolved identifier has an
event unless its line has a diagnostic; sorted, CSR, declaration events,
hashes). LSP: the check phase hands its `CIndex` to the snapshot (only if
not cancelled and still current), files changed since the snapshot are
marked stale; `definition` falls back from the macro index (now marking its
plain-identifier fallback `weak`) to C DEF events, else DECL; new
`declaration` (DECL, else DEF; `declarationProvider`). D1: both wait up to
1.5 s (`cond_timedwait`, new in thread.[ch]) while the newest edit's snapshot
or check is pending, then answer from what is there. D2: the LSP check no
longer stops at a missing include; diagnostics after the first one are
dropped. Tests: goldens `tests/symidx/` (plain and cells), `--verify-symbols`
over tests/check, tests/parse and the sources (run.sh section 14), LSP
sessions `tests/lsp/csym` (every kind, shadowing, forward declaration,
macro-vs-C, D1, D2) and `csym_nocheck` (new `env` step in lsp_session.py);
the other transcripts gained `declarationProvider`. The LSP sessions pass
under TSan. Deferred: `cereal query` C support, verify over bench/corpus,
hover strings (phase 3).

Round 188: `--verify-symbols` over the corpus (B2 phase 1 follow-up).
Ran `cereal --verify-symbols` with each file's own flags over the 170 translation
units of `bench/corpus.py` (Lua 5.1-5.5, lupa, libuv, uvloop incl. the Cython
loop.c, zstd) and every gcc.dg and c-c++-common test that passes par.py's
`dg-options` filter (4716 files with the corpus: 1.02 M events, 0 unindexed),
plus a second pass at `-std=gnu11` and a sweep of 16 k files from
gcc.c-torture and the gcc.dg subdirectories, with the diagnosed-line excuse
turned off. Misses found, 2 (both in clean code), both fixed: (1) labels in
a system header got a decl but no event (a `# n "f.h" 3` region): the label
decl is now made at its first event (`csx_label`, so none for system-presented
labels), case appended to `tests/symidx/stmts.c`; (2) the identifier list of a
declaration that is not a definition (`int f(a, b);`, `foo (one_arg);` at file
scope, `typeof(x);`) names nothing: new hook `csx_skip` at the "parameter names
(without types)" site marks the tokens as no entity, case in
`tests/symidx/decls.c`. Every other miss with the excuse off (about 40 names in
32 files) is on a line gcc rejects (dropped or duplicate members, designators
into them, labels outside functions, incomplete-type fields). The excuse is
counted now: the `symbols:` line ends with `N excused` (names left out for a
diagnosed line, a builtin or an error node; 18.6 k over the 4716 files).
`tests/symidx` is also run through `--verify-symbols` in run.sh section 14.

Round 189: an LSP crash on edits, fatal() per phase, and no diagnostics
flicker. (1) A session on uvloop's loop.c (8.6 MB) with five back-to-back
full-text edits died after 116 s (89 s system time) with "token buffer too
large". gdb on the builder: `build → par_run → pp_run_phase_a → pp_directive
→ do_if → pp_eval_if → expand_into → tokbuf_push → tokbuf_grow`. Root cause:
a cancel sets `PP.halted`, after which `pp_read_raw` returned a lexer EOF and
skipped barrier contexts, but `expand_into` (an `#if` expression, a macro
argument; phase A and phase-B workers alike) reads until `SRC_BARRIER`, so it
pushed EOFs, doubling the buffer to 2^29 tokens while paging. Only an edit
landing during such an expansion of a long, macro-dense build hits it. Fix:
a halted stream returns `SRC_BARRIER` while a barrier context is open
(`src/pp.c`). Stress (5 edits per round, 8 rounds, random delays): the old
binary died in 6 of 8 sessions (every delay of 150, 300 or 450 ms; back-to-back
edits coalesce), the fixed one passed all 24, and the final binary 12 of 12
on loop.c and 12 of 12 on zstd.c (which also has checks, up to 18 of them
cancelled per session). (2) `fatal()` inside a server
phase no longer exits: a thread-local `FatalTrap` (setjmp/longjmp, common.h)
around the builder's build and check, and around pool jobs when
`ThreadPool.trap_fatal` (only `cereal lsp` sets it; `group_wait` re-raises
the first job's message). The phase is dropped and logged (stderr and
`window/logMessage`), a failed build resets the unit's interner and cells,
partial state is leaked. A `fatal()` with a mutex held (counted in
`mutex_lock`) still exits. `CEREAL_FAULT=site[:N]` injects faults for tests
(`par-worker`, `lsp-check`, `pp-halt-in-if`). (3) The macro phase's
publication carries the previous compiler diagnostics that lie wholly before
the first edited line of their file (not all: later ones may have moved);
a failed check withdraws them. Chosen over keeping all for that reason.
publishDiagnostics carries the document `version` when the client sent one
(a didChange without one no longer invents version+1). (4) Units over the
check limit show one Information diagnostic at line 1 (the size and the
limit); the CLI is unchanged. Tests: `tests/lsp/carry`, `skip`, `fault`
(`.cereal` forces the parallel path), `fault_check`; a `waitall` step in
lsp_session.py; the other transcripts gained `version`; run.sh runs `-E`
with `pp-halt-in-if` under a 6 GB cap (fails on the old binary with the
out-of-memory fatal). Gates: run.sh 1650 passed; san.sh 0 failed and 0
findings over 600 files; verify.sh gcc.dg 3908, c-c++-common 635, cpp 285; the LSP
sessions (both modes) under TSan: 18 passed, 0 warnings. src/c untouched.

Round 190: ROADMAP B2 phase 2, references and documentHighlight for C names
(B2_DESIGN.md "Phase 2 results and corrections"). Phases 2 to 4 were first
re-estimated from phase 1's 2.6x overrun (~250, ~300, ~500 lines). New in
csymidx.[ch]: `cindex_select` (a set of decls' events for definition,
declaration, references or uses, in (file, offset) order, one per location,
a declaration preferred to a use there; stale files and, for highlight,
other files left out), `cindex_decls_at`, and `cindex_validate` and
`cindex_srcfile` moved from the server. `textDocument/references` answers C
names with the same merge as definition (a macro or macro parameter at the
cursor wins; a weak or empty macro answer goes to C) and honours
`includeDeclaration`; new `textDocument/documentHighlight`
(`documentHighlightProvider`) gives the requested file's events, Write for
declarations and definitions, Read for uses, and for macros the definitions
Write and every other reference in the file Read. Both wait for the newest edit's index like
definition (D1). Uses spelled in a `#define` body are left out (a correction
to design section 6); a name in a macro argument counts once at its
spelling. `cereal query` gains C support (`def`, `decl`, `refs`, new `uses`
and `highlight`) from a second, checking run when the macro answer is weak
or empty. Tests: `tests/lsp/csym_refs` (function, redeclarations, shadowing,
parameter with a macro-argument use, typedef, enumerator, `key` in two
structs, label, tag vs a variable of the same name, a redeclaration chain
whose body use is left out, includeDeclaration false, macros unchanged, D1:
fails if references do not wait), `tests/query/csym.cmd` (strong macro,
weak macro, plain C, body token with two entities); the other transcripts
gained `documentHighlightProvider`. About 290 lines added, 100 removed
(net +190), against ~250 re-estimated. Gates: run.sh 1653 passed, 0
failed; san.sh 1653 passed, 600 files, 0 findings; verify.sh gcc.dg 3908,
c-c++-common 635, cpp 285; the LSP sessions (both modes) under TSan: 20
passed, 0 warnings.

Round 191: `bench/tools/symcov.py` (`symcov.sh`) makes the Round 188 coverage run
durable: 4716 files, about 1.02 M events, 0 unindexed, also under `--gnu11`.
Its first run found five segfaults in `--verify-symbols`: two rows of
`cbuiltin_tab.h` had a signature without the `|` separator (`fegetround` was
`"int"`, `signbit` `"int"`), and `strchr(sig, '|') + 1` dereferenced NULL in
`cexpr_builtin_decl`. Rows now read `"int|"` and `"int|?"`; golden
`builtin_noarg_row` (also run under `--verify-symbols` by section 14). run.sh section 15 now
checks that every table signature contains `|` (fails on the old row).

Round 192: ROADMAP B2 phase 3, hover for C names (B2_DESIGN.md "Phase 3
results and corrections"). `dump_decl` became `cdecl_decl_line` (a StrBuf,
shared by `--dump-types` and the index; enumerators through
`crecord_enumconst_line`), with lines for parameters too; `tentative` is
printed only when no declaration initialized the object. The builder copies
each decl's text out while the checker's tables live: block-scope symbols at
unit end, labels (`label f:out`) at their first event, enums at completion
(new hook `csx_enum`: `enum E (underlying T)` and its enumerators),
persistent symbols, records (`type_dump_record`) and fields (`field x: T
(struct S, offset N[ bit B, width W])`) at finish. Texts are interned in one
hash set that becomes the CIndex string pool, cut at 1024 bytes and 16
members. `textDocument/hover` answers a C name as definition does (a strong
macro answer stands; a weak one adds "(also a macro name)"; distinct texts
of several entities, at most 5) and waits for the newest check (D1);
`cindex_hover` is shared with `cereal query hover`. `--dump-symbols` prints
each decl's text. Memory: 30-39 B per decl added (zstd.c 1.37 to 1.81 MB,
cexpr.c 0.53 to 0.68, main.c 0.21 to 0.34; equal texts shared: zstd.c's
583 KB of texts keep 429 KB); the worst case, a 4 MiB struct of 330,869
short members, 14.4 to 29.3 MB, within the ~30 MB budget, so no shorter
cap. Tests: `tests/lsp/csym_hover` (every kind, qualifiers and storage,
bit-field, designator, a 20-member struct cut, label, shadowing, macro
precedence, D1: fails without the wait), `tests/query/csym.cmd` hover
cases, `tests/symidx` goldens with the texts plus a 110-parameter
prototype cut at 1024 bytes. 349 lines added, 66 removed (net +283),
against ~300. Gates: run.sh 1658 passed, 0 failed; san.sh 1658 passed, 601
files, 0 findings; verify.sh gcc.dg 3908, c-c++-common 635, cpp 285;
symcov 4716 files, 0 unindexed; the LSP sessions (both modes) under TSan:
22 passed, 0 warnings. Noticed, not investigated: checking one struct is
quadratic in its member count, with or without the index (17.6 k members
0.15 s, 34 k 0.48 s, the 330,869-member file 20 to 58 s; 4 MiB of `int
vN;` takes 0.7 s).

Round 193: three follow-ups to Round 192.
(A) "Hover on `XXH_MIN` in zstd.c returns null" is not a bug: both its
`#define` (zstd.c:15290) and its use (15324) lie inside `#ifndef
XXH_NO_XXH3` (11943), and zstd.c defines `XXH_NO_XXH3` itself (8294), so
the macro is never defined; `def` answers nothing there either. The
suspected cause, a macro inside a nested macro's argument, answers
correctly (`A(MIN(a, b))`, `A(K)`, `B(MIN(a, K))` all hover to their
`#define`). `tests/query/lsp.cmd` pins both: LIMIT inside TWICE's argument
(through SQUARE) hovers to its `#define`; in an `#if 0` region a macro
visible there answers (SQUARE) and one whose `#define` is itself skipped
(new GONE) answers nothing.
(B) Hover texts read with one space after a parameter's comma: `hover_add`
(csymidx.c, every text the index stores goes through it) collapses the
two spaces `type_print` puts after a parameter ending in a word (gcc's
diagnostic spelling, which `--dump-types` keeps). The stored text is what
changes, so `tests/symidx/decls.expected` changes on its two prototypes
(`f`, the 110-parameter `many`, still cut at 1024 bytes);
`tests/query/csym.cmd` gains `int pair(int a, long b, unsigned c);`
(`func pair: int(int, long int, unsigned int)`). csym_hover's texts had
no such comma, so its golden is unchanged.
(C) The quadratic struct check was the duplicate-member check
(`dup_add`, crecord.c): every named member scanned the names seen so far
(73% of the instructions at 10 k members). It is now a stamp array by
ident (`Checker.mseen`, one generation `mgen` per record, cleared on
wraparound), so the check is linear; diagnostics and their order are
unchanged (byte-identical to the old binary on the new golden). callgrind:
10 k members 343 M instructions before, 40 k 361 M and 160 k 1.43 G after
(linear; with `--dump-symbols` 20 k 433 M, 40 k 868 M). Wall time (shared
machine, noisy), before / after: 17.6 k 0.45 / 0.15 s, 34 k 1.55 / 0.50 s,
80 k 9.4 / 0.78 s, 160 k 36.5 / 1.8 s, 330 k members 3.1 s after (8.3 s
with `--dump-symbols`). Golden `tests/check/dup_members_many` (4,000
generated members: a duplicate among them, one through an anonymous
member, and a plain member repeating a name of that anonymous member; a
record defined among the members and a later record reuse every name
without a diagnostic), matching gcc-13 up to locale quotes and carets.
Found, not fixed (same shape, an ident-set lookup per record, a per-record
name index would cover all three): member access and designators scan the
record's fields per use (`find_field` cexpr.c, `lookup_path` cinit.c):
20 k members each accessed once and designated once, 4.77 G instructions,
88% in these two; and under `-Wc++-compat` two loops are quadratic in a
struct of typedef-typed members (`cxx_typedef_in_struct` cdecl.c scans the
open fields per typedef use; struct_finish scans `tdseen` per member):
10 k members 0.54 s, 20 k 3.3 s. Gates: run.sh 1660 passed, 0 failed;
san.sh 1660 passed, 602 files, 0 findings; verify.sh gcc.dg 3908 of 3910,
c-c++-common 635 of 636, cpp 285 of 285; symcov 4716 files, 0 unindexed;
uvloop loop.c 3.89 G instructions (unchanged).

Round 194: ROADMAP B2 phase 4, rename for C names (B2_DESIGN.md "Phase 4
design addendum" and "Phase 4 results"); B2 is complete. The addendum,
written first and stress-tested in three rounds plus a final pass (A7),
replaces section 9's scope tree with verification by recomputation: new
`src/c/crename.c` (`c_rename`) checks the unit, plans the edits on that
run's index (`cindex_rename_plan` in csymidx.c: one event per place, all
in the main file), checks the unit again with the edits applied, and
accepts only if both runs give the same diagnostics (level, option,
place), decl kinds, linkages and names (the target renamed) and events
(same decl and flags at moved offsets). The refusal names the first
difference: a new or lost diagnostic, a token that also names another
entity, or a name that would refer to another declaration. Refused before
that: several entities at the place, predeclared, system or implicit
names, a use in a `#define` body or formed by `##` (a macro-argument use
is edited, D4), any use outside the main file (D3, until B5), a new name
that is not an identifier, is a keyword or has macro history, a missing
`#include`, and the old or new name in a skipped `#if` group within the
decl's scope extent (new `CIdxDecl.scope`/`CIndex.scopes`, the unit's
extent for block-scope names, shown by `--dump-symbols` as `scope L-L`).
The checker keeps static state, so the server runs the two checks on the
builder thread (`lsp_check_rename`, a `RenameJob` taken before the next
build) and refuses ("retry") unless the snapshot is the newest edit's
with its check done; prepareRename and rename wait for it (D1).
prepareRename refusals are now RequestFailed errors with the reason, for
macros too (`tests/lsp/proj` changed); null stays for no entity. The
identifier test moved to `cindex_is_identifier`. `cereal query
rename=NEW FILE:L:C main.c` prints the edits or `cannot rename: REASON`.
Tests: `tests/query/rename.cmd` (36 renames: 14 accepted, 22 refused,
every blocker and every kind of second-check failure),
`tests/lsp/csym_rename` (both modes, a rename right after an edit), the
symidx goldens with scope lines. 384 lines in crename.c, 390 added and 40
removed elsewhere, against ~350 re-estimated. cexpr.c rename by query
0.10 s (refs 0.05 s). Gates: run.sh 1664 passed, 0 failed; san.sh 1664
passed, 602 files, 0 findings; verify.sh gcc.dg 3908 of 3910,
c-c++-common 635 of 636, cpp 285 of 285; symcov 4716 files, 0 unindexed
(10 skipped, as before); the LSP sessions (both modes) under TSan: 24
passed, 0 warnings; uvloop loop.c 3.892 G instructions (HEAD 3.891 G).

Round 195: inactive-code hover, and member lookup without quadratic scans.
(A) Hover inside a skipped `#if` group answered null when nothing else
applied, which reads as a bug. New `index_inactive_note` (index.c) finds
the group in `Index.inactive` (the ranges inactiveRegions publishes) and
the innermost `Index.blocks` entry around it, and spells its directive
from the source ('#' or "%:", blanks, the keyword): `inactive code
(skipped by #ifdef at lsp.c:25)` (no place for an unterminated
conditional). No new tracking. Both `lsp_hover` (only where it used to
answer null) and `cereal query hover` (only where the macro and C
indexes gave nothing) use it, so a macro named in the group (SQUARE in
`#if 0`) still answers with its `#define`. Definition and references
stay empty there: they answer locations, and there are none. Tests:
`tests/query/lsp.cmd` (lsp.c gains an `#ifdef`/`#else` with an `#if 1`
nested in the skipped group: the note names the `#ifdef` for both; the
active group and the `#else` line get none; GONE in `#if 0` now gets the
note), `tests/lsp/basic` hovers the skipped `#else` group (both modes).
(B) One per-record member-name index serves member access and
designators. `cfield_slot` (cexpr.c) gives the member of a record that a
name selects, the named member first, else the first anonymous struct or
union member holding it at any depth (find_field's order), as an index.
Records of at most 16 members are searched in place (no cost for small
records); a larger one is indexed on its first lookup into one Checker
hash keyed (first field << 32 | name), which a clone (`RF_SSO`, same
fields) shares; a marker key per record holds the lowest record index
of its anonymous members. `find_field` and `lookup_path` (cinit.c) are
now loops over `cfield_slot`, one step per anonymous level, and their two
recursive scans are gone. lookup_path used field order (an anonymous
member before a direct one won); after struct_finish clears a duplicate's
name the two orders agree except past dup_nested's depth of 16. The
summary read sets (csum) are kept exactly: the index is built from the
table without reads, and a lookup in a record whose anonymous members
are older than the unit reports the reads a search in place makes (those
searched before the hit, all for a miss), checked by turning that off
(the new golden then loses two reads). The `-Wc++-compat` scans use the
by-ident arrays instead: `grow_idents` became `cgrow_idents` and sizes
`mseen` and new `fopen` with `top` and `ext` (`dup_add`'s own growth and
`nmseen` are gone); struct_finish marks the body's typedef names in a
new `mseen` generation and tests each member once (`mseen_next` shared
with the duplicate check); `cxx_typedef_in_struct` asks
`crecord_open_member`: `fopen[name]` is the first open member so named
(fields index + 1), valid while that `c->fields` entry still names it;
bodies only push (`push_member`) and truncate, so nothing is cleared.
Diagnostics byte-identical to the old binary on the generated inputs and
the new goldens. callgrind, 20 k members each accessed and designated
once: 4.776 G before, 0.585 G after (40 k 1.165 G, linear);
`-Wc++-compat` with 20 k `T mN;` members 3.81 G before, 0.213 G after
(wall here 0.08 / 0.29 s before at 10 k / 20 k, 0.01 / 0.03 s after).
Tests: `tests/check/member_index_many` (8,000+ generated typedef-typed
members under `-std=c11 -Wc++-compat`: access, designators, nested and
const anonymous members, a direct-then-anonymous and an
anonymous-then-direct duplicate, misses with a hint, the field/typedef
warnings in a small record, a large one and a record nested in it, and a
later record reusing a stale `fopen` entry), matching gcc-13 except two
differences noted in STATUS (the field/typedef warning's order against
the duplicate errors; the read-only message for a const anonymous
member); `tests/summary/member_reads` (read sets for direct, first and
second anonymous, missing and designated members, both modes). 285 lines
added and 99 removed in src. Gates: run.sh 1668 passed, 0 failed;
san.sh 1668 passed, 603 files, 0 findings; verify.sh gcc.dg 3908 of
3910, c-c++-common 635 of 636, cpp 285 of 285; symcov 4716 files, 0
unindexed (10 skipped); the LSP sessions (both modes) under TSan: 24
passed, 0 warnings; uvloop loop.c 3.884 G instructions (HEAD 3.893 G).

Round 196: the two gcc-13 differences Round 195 noted.
(A) The read-only member wording. gcc's readonly_error says "member 'q'
in read-only object" when the object holding the member (the
COMPONENT_REF's operand) is const, else "read-only member 'q'". An
anonymous member is its own COMPONENT_REF whose type takes the outer
qualifiers, so `p->q` with `const struct { int q; };` in the record
counts as a read-only object, as does a member of a non-const anonymous
struct inside a const one; `struct { const int r; };` stays "read-only
member". `readonly_check` (cexpr.c) only looked at the base's type; it
now adds the anonymous members' qualifiers along the path (`find_field`
already collects them). Same rule for =, compound assignment, ++, --,
parenthesized and nested access (checked against gcc-13). asm outputs had
their own copy of the wording (casm.c `asm_operand`), which never said
"in read-only object" and missed a record with a const member (`*pc`
where `struct C` holds a `const int`: gcc "read-only location '*pc' used
as 'asm' output", cereal nothing); it now calls `readonly_check` with
use 3 (a table of the two message shapes; no string-literal warning for
asm, as in gcc), and its copy is gone.
(B) `-Wc++-compat` "using 'T' as both field and typedef name" (and
"empty struct has size 0") come from gcc's warn_cxx_compat_finish_struct,
the last step of finish_struct: after the duplicate-member errors (which
clear the duplicate's name, so `T x; int T; int T;` warns once), after
-Wpadded and "union cannot be made transparent". struct_finish ran it
before the flexible-array checks; it now runs last. It must still see
the erroneous members gcc keeps in its field list (`int T[];` not at the
end, `int T : 0`), which struct_finish compacted in place: when a member
is dropped, the kept ones are now copied past the record's entries in
`c->fields` (truncated at the end as before) and the originals stay for
the check. Checked against gcc-13: duplicate before and after the field,
nested record (its own duplicates and warning first), a duplicate named
T, typedef not used in the body (no warning), anonymous member holding a
field T (no warning), -Wpadded, transparent_union, erroneous members.
Noticed, not fixed (STATUS): a record past 2^64 bytes, which gcc wraps
silently. Tests: `tests/check/readonly_member_anon`,
`tests/check/cxx_compat_field_typedef_order` (both identical to gcc-13's
headers), `tests/check/member_index_many.expected` now gcc's order and
wording. 57 lines added and 47 removed in src. Gates (gate.sh): run.sh
1672 passed, 0 failed; san.sh 1672 passed, 605 files, 0 findings;
verify.sh gcc.dg 3908 of 3910, c-c++-common 635 of 636, cpp 285 of 285
(unchanged); uvloop loop.c 3.885 G instructions.

Round 197: ROADMAP B3, typeDefinition, document symbols, signature help,
semantic tokens and completion for C names (B3_DESIGN.md, written and
stress-tested in four rounds before this one; results in its section 11).
Index (csymidx.c/h): `CIdxDecl.type` (the typedef, struct, union or enum
the decl's type names, peeled to the first typedef or tag; a typedef's
is one step down) and `.parent` (a field's outermost record through
anonymous members, an enumerator's enum), flags `readonly` (const after
stripping arrays) and `static`; nameless decls for anonymous records and
enums (no events, `<anonymous>` in `--dump-symbols`); a scope tree
(`CIdxScope.parent`: roots are external declarations, children the
checker's blocks via new `csx_scope` hooks in check.c's scope_open and
scope_close), `decl.scope` the innermost scope (definition parameters
move to the body; labels take the root); every file-scope system
declaration gets its decl at finish, used or not, so completion and
signature help by name reach `strlen` before its first use. New queries
`cindex_lower`, `cindex_types`, `cindex_scope_at`, `cindex_scope_root`
(crename.c's inactive-use test now uses it) and `cindex_visible` (the
names in scope at a place, innermost first, sorted). `--verify-symbols`
checks scope order, nesting and overlap and the new fields' ranges; its
summary counts scopes (symcov.py's pattern follows). Server: the request
table gained a `cidx` column, and every request reading the C index
waits for the newest check (D1) through it; `typeDefinitionProvider`; the
token legend is built from one list in features.c. Features:
typeDefinition (DEF of the type decls, else DECL); documentSymbol (the
macros, then file-level declarations with the external declaration as
range, members under the entry of their record or enum: a definition of
the type first, else the first entry of it; members never claim);
signatureHelp (a function-like macro at that version, else the C entity
at the callee or the visible one by name, its parameter list read from
the declaration text, through a typedef for function pointers, `...`
capping activeParameter); semantic tokens (function, variable,
parameter, type, enumMember, property, struct, enum; declaration,
readonly, static; not for `#define` body, pasted, system or label
events; the kept whole-file result is keyed by the index being there);
completion (macros, then `cindex_visible` minus names a visible macro
hides, the hover text as detail). `cereal query type` and the C names in
`cereal query visible`. Found while testing: an enumerator claimed its
nameless enum's other enumerators in document symbols (members no longer
claim), and a file including itself (gcc.dg/range-test-1.c) gave blocks
outside their parent (now dropped when they do not overlap it). Tests:
`tests/lsp/csym_b3` (both modes), `tests/query/b3.cmd`,
`tests/symidx/b3.c` and `selfinc.c`; every LSP transcript gained
`typeDefinitionProvider`, `basic` gained C document symbols and tokens,
`tests/query/lsp` C names in `visible`. Memory: zstd.c's index 1.83 to
2.52 MB, the dense 4 MiB files 29.97 to 32.6 MB and 23.3 to 31.2 MB
(both over the ~30 MB budget, accepted); build time unchanged. 1,056
lines added and 161 removed in src, against ~550 estimated. Parked
(B3_DESIGN.md section 9): member, tag, label and keyword completion;
semantic-token refresh after a check that ends past the wait; carrying
the previous index across edits; signature help through an expression
callee; blocks written inside a macro argument. Gates (gate.sh): run.sh
1679 passed, 0 failed; san.sh 1679 passed, 605 files, 0 findings;
verify.sh gcc.dg 3908 of 3910, c-c++-common 635 of 636, cpp 285 of 285
(unchanged); symcov 4716 files, 0 unindexed (10 skipped); the LSP
sessions (both modes) under TSan: 26 passed, 0 warnings; uvloop loop.c
3.888 G instructions (HEAD 3.885 G).

Round 198: ROADMAP B3 carry-over, step 1 of B3_DESIGN.md 12.11 (the index
side only; server, features and diagnostics are steps 2 to 4). csymidx.c/h:
`CIndex` gained an atomic `refs` (`cindex_free` drops one, `cindex_ref`
takes one), a unique `serial` and `core`, the original that owns decls
and strings; `CIdxFile` gained `edited`, `dmg_begin`, `dmg_end`; the CSR
code of `csx_finish` moved into `build_csr`. `cindex_text_edit` (common
prefix and suffix of two texts, widened to whole identifiers),
`cindex_carry(from, sm_old, sm_new)` (copies files, events, CSR and
scopes; shifts offsets past each file's edit, drops events overlapping
it, maps scopes through the one monotone map, shares decls and strings
with the core; a stale or missing file stays or becomes stale; no change
returns `from` with a reference; the damage is the hull along a chain),
`cindex_damaged`, `cindex_touched`; `cindex_verify` skips its "every decl
has a declaration event" rule for a carried index; `cindex_bytes` counts
only what an index owns. main.c: `cereal check --verify-carry=OLD[@PATH]`
indexes the input with OLD's text standing in for it (or for PATH),
indexes it again, carries the first to the second and compares events
and scopes outside the damage (`cindex_verify_carry`); the exit status
reflects only structural problems, differences are printed.
`CEREAL_CARRY_STATS=1` prints sizes and the carry time. Tests:
`tests/symidx/carry` (six goldens: comment inserted at the top, an
identifier extended, a statement typed, a `}` deleted, a typedef changed
to a variable, an edit in a header) with a loop in tests/run.sh.
Measured (B3_DESIGN.md 12.13): the copy is 62%, 38% and 21% of the
index of zstd.c, the `vN` file and the `fN` file, in 1.1, 4.4 and 2.6 ms;
the claim that carrying costs +31 MB at the 4 MiB limit is too low as
built: the copy keeps the whole original alive, so index plus copy is
43.1 MB (`vN`) and 35.8 MB (`fN`), +28% and +16% of the peak RSS of a
check of the same file; a lighter core is an open choice. A line
inserted that begins like the next declaration widens that declaration's
root scope (a limit, documented). About 570 lines added and 20 removed in
src (csymidx.c +409/-20, main.c +114, csymidx.h +43), against ~220
estimated for the step, which put the comparison tool in. Gates
(gate.sh): run.sh 1687 passed, 0 failed; san.sh 1687 passed, 605 files,
0 findings; verify.sh gcc.dg 3908 of 3910, c-c++-common 635 of 636, cpp
285 of 285 (unchanged); symcov 4716 files, 0 unindexed (10 skipped);
uvloop loop.c 3.889 G instructions. Not run: the LSP sessions under TSan
beyond what san.sh runs (no server code changed).

Round 199: ROADMAP B3 carry-over, steps 2 and 3 of B3_DESIGN.md 12.11
(server, features, refresh; results in 12.14). server.c: the builder
carries the previous snapshot's index over the edit at install
(`cindex_carry`, only for a snapshot that will be checked; it reads
`u->snap` and its index without the lock, being their only writer) into
`Snapshot.cidx` with `cidx_carried`; a check's publish replaces it under
the lock and frees the carried one after the unlock (requests hold their
own reference); a failed check, not superseded, drops it; a superseded one
leaves it for the next carry. The request table's `cidx` boolean became a
wait policy: `CP_NONE`, `CP_ANY` (completion, signatureHelp, highlight,
documentSymbol: wait for the newest edit's snapshot, for the check only
when the snapshot has no index), `CP_POS` (hover, definition, declaration,
typeDefinition: also when the position is in the carried index's damage),
`CP_WAIT` (references, prepareRename, rename: always), `CP_TOK` (tokens:
`CP_ANY` for a client with `workspace.semanticTokens.refreshSupport`, else
`CP_WAIT`). After the 1.5 s deadline every request answers from the
carried index (references included; rename refuses, "retry", since
`c_fresh` excludes a carried index). `Unit.tok_stale` is set by a token
answer without the snapshot's own index; the publish of a check's index
sends `workspace/semanticTokens/refresh` (id `cereal-refresh-N`, under
`S.m` like `publishDiagnostics`; the client's response is already skipped).
Test-only `cereal/holdChecks {"hold": bool}` makes the builder wait before
a check. features.c: the whole-file token cache is keyed by the index
serial; `STok.from_c` and a macro token wins a tie with a carried C token;
completion from a carried index is `isIncomplete: true`; hover adds
"(rechecking: its declaration was edited)" (a lost DECL/DEF event, or one
on a line the damage touches, the damage's end taken as exclusive).
`CEREAL_LSP_STATS` logs the carry per install and `carried` per request.
Tests: new `tests/lsp/carry_cidx` (held checks; edit 1 makes a variable
`long` and types a declaration and a call; hover/definition below it
moved, hover above it unmarked, the marker on the edited variable,
completion incomplete, signature help by name, tokens, outline, hover on
the typed name and prepareRename each waiting out 1.5 s, a second edit
while held, then the release: the refresh recorded, a token delta, fresh
answers); `csym_b3` gained a token request after an edit with no
`refreshSupport` and the new `absent` step of lsp_session.py (no refresh
is sent); `fault_check` shows the carried index going with a failed
check (disabling the drop fails it); the D1 notes of csym, csym_hover and
csym_refs changed, their answers did not; lsp_session.py's `wait` accepts
a server request without params. `tests/lsp_stress.py` (200 edits each
followed at once by tokens, completion and hover on zstd.c, checks not
held) ran with empty logs under TSan (9.7 min; only 1 refresh, since few
checks end between edits at TSan speed, so also on features.c, 1,850
lines: 62 refreshes, 20 s) and ASan/UBSan (2.6 min, 64 refreshes).
Measured on zstd.c in the server: peak RSS during the check 42.8 MB
without carry, 45.8 MB with (+3.0 MB, +7%; 3 runs each). Noticed, not
caused: `tests/lsp/csym_sys` with `--flags` leaves its `.cereal` behind
(the harness restores the file its own write step created); removed by
hand. About +240/-54 lines in src (server.c +182/-42, features.c +49/-8,
lsp.h +9/-4), against ~185 estimated for the steps. Gates: run.sh 1689
passed, 0 failed; san.sh 1689 passed, 605 files, 0 findings; verify.sh
gcc.dg 3908 of 3910, c-c++-common 635 of 636, cpp 285 of 285 (unchanged;
run before the last features.c edit, the hover line test, which no gate
but the sessions exercises); symcov 4716 files, 0 unindexed (10
skipped); uvloop loop.c 3.886 G instructions; the LSP sessions (both
modes) under TSan: 30 passed, 0 warnings. Not done: step 4 (diagnostics
through the same edit, 12.8).

Round 200: ROADMAP B3 carry-over, step 4 of B3_DESIGN.md 12.11 (compiler
diagnostics through the same text edit; results in 12.15). `CDiag` stores
file offsets (range, severity, code, message, notes with path and offset,
and `carry`: the check read the snapshot's text of every file it names)
instead of rendered JSON. `cdiags_carry` (features.c; `carry_cdiags` and
`first_changed_line` are deleted from server.c, with the "no file changed"
rule for notes in other files) computes each file's edit with
`cindex_text_edit` (now with `cindex_edit_map` exported), drops a
diagnostic whose range or any note meets the edit's span (inclusive) or
whose file is gone, and shifts the others; `lsp_publish_diagnostics`
renders carried ones against the new snapshot's line table and keys them
there (one renderer, `put_diag`, for macro, fresh and carried diagnostics,
built from `cdiag_make`). Effect: errors below an edit stay visible,
moved, while typing. Tests: `tests/lsp/carry` changed (see 12.15 for every
line), `fault_check` (same workspace; the macro-phase publication now also
shows the lower error, still withdrawn at the failed check), and two new
sessions: `carry_tie` (the tie rule: a `#define foo` typed above `int foo;`
with checks held; fails with the rule disabled) and `carry_hdr` (an edit of
an included header only, checks held: definition and hover through the
carried index, the error in the includer kept and its note into the header
moved). ASan found a use-after-free in the first version of `cdiags_carry`
(the edit cache kept the path of a diagnostic copy that was then freed);
fixed by keying the cache on the source diagnostic's strings. Gates:
run.sh 1693 passed, 0 failed (1689 + 2 sessions x 2 modes); san.sh 1693
passed, 605 files, 0 findings; verify.sh (gcc-13) gcc.dg 3908 of 3910,
c-c++-common 635 of 636, cpp 285 of 285; symcov 4716 files, 0 unindexed
(10 skipped); uvloop loop.c 3.886 G instructions; all 17 LSP sessions (both
modes) under TSan: 34 passed, 0 warnings; `lsp_stress.py` on features.c
(200 edits, 66 refreshes) clean under TSan (74 s) and ASan/UBSan. About
+228/-134 lines in src (features.c +197/-52, server.c +1/-70, lsp.h
+22/-7, csymidx.c/h +8/-5), against ~80 added and ~60 removed estimated.

Round 201: server memory. Measuring the carry (B3_DESIGN 12.9) showed
`cereal lsp` RSS climbing over successive edits of a dense 4 MiB file.
Two causes, found on a throwaway copy with ASan/LeakSanitizer and
mallinfo2: (1) a real leak, `Checker.dm` (`grow_nodes`, one size_t per
node) was never freed in `check.c`; a unit with one huge declaration
(a 307k-member struct, about 1.5 M nodes) leaked about 12 MB per check;
fixed with `free(c->dm)` (flat in-use heap afterwards, clean LSan);
(2) glibc retention: in-use heap stays flat while the arena keeps
227 MB of free memory; bounded (plateau after 4 to 7 edits), and
`MALLOC_TRIM_THRESHOLD_=0` cut steady RSS 55% (dense), 77% (fixed
struct), 40% (zstd.c); its speed cost is not measured, not adopted. The
carried index is not chained (`core` always points to the original).
True carry cost at the check peak, with vs without carry: +50 to 70 MB
(dense `int vN;`), +35 to 45 MB (307k-member struct), +4 MB (zstd.c);
the 36 to 43 MB estimate of 12.9 was low because the carried index keeps
the whole original alive. Parked: share only decls and strings so the old
event arrays can be freed (`cindex_touched` reads `core->by_decl`).

Round 202: malloc tuning for `cereal lsp`, measured, not adopted. Setting
`MALLOC_TRIM_THRESHOLD_=0` (or any trim threshold, 16 to 128 MiB, which
turns off glibc's dynamic mmap threshold; `MALLOC_MMAP_THRESHOLD_=1048576`
alone does the same) cuts the server's steady RSS after edits by 45% (dense
4 MiB `int vN;` file, 306 to 168 MB), 62% (307k-member struct, 402 to 154
MB) and 29% (zstd.c, 48 to 34 MB), and the peak by 20 to 24%, but makes the
per-edit check 10 to 20% slower on the two 4 MiB files (dense 0.85 to
0.99 s, struct 0.69 to 0.90 s; run-to-run noise about 10%, 6 rotated runs
per variant, 3 files, 8 variants). Nothing measurable on zstd.c, a small
file, or the CLI (best of 7). The RSS gain and the slowdown come together:
no threshold keeps the gain within 5% of the base check time. Decision:
leave glibc defaults. A user who prefers memory over latency on huge
generated files can start the server with `MALLOC_TRIM_THRESHOLD_=0`.
Untested: explicit `malloc_trim()` after a check, `M_TOP_PAD`, arena tuning.

Round 203: explicit `malloc_trim(0)` for `cereal lsp`, measured on a copy,
not adopted. Called in the builder after the check frees (variant 1) or
after 300 ms of quiet (variant 2, trim only if no work is queued), it cuts
steady RSS 1 s after idle by 70% (dense 4 MiB file, 290 to 86 MB), 75%
(307k-member struct, 366 to 90 MB) and 39% (zstd.c, 46 to 28 MB), more than
the trim-threshold settings of Round 202, and lowers the peak a little.
Each call takes 2 to 90 ms. But the pages must be faulted back in, so the
next check is 5 to 15% slower (paired ratios, 6 to 8 rotated runs per
variant, noise floor 5 to 8%); variant 1 also delays the next build by up
to 90 ms. Variant 2 costs nothing during a typing burst (the 300 ms of
quiet never occurs) and pays the re-fault once on the first check after a
pause: the only candidate left, a latency-versus-memory call. Not adopted.
Survey (no code): slimming the carried index. Only `cindex_touched` reads
data from `core`; a separate refcounted block for decls and strings plus a
1-bit-per-decl "lost a DECL/DEF event" bitmap would let the old index's
events, CSR and scopes be freed at install (about 30 net lines in
csymidx.[ch]; saves about 12 MB (dense), 6 MB (struct), 1.6 MB (zstd.c) of
the carry cost, 3 to 8% of the 4 MiB-limit peak). The 19 to 23 MB of shared
decls and strings is untouched. Parked as a cleanup, not a memory fix.

Round 204: ROADMAP A4 slice 1, `cereal check --compile-commands DB [FILES]`
(A4_DESIGN.md D1 to D5, D9). Foundations: `Options.cwd` becomes `SrcMgr.cwd`;
`SrcFile.path` is the absolute key and `name` the normalized spelling as
first written (diagnostics print `name`; quote includes join the includer's
`name`; the LSP display sites use `path`); `par_worth_it` stats the
cwd-resolved path. Option errors are counted messages: `Options.msg`
(default stderr, NULL silent), `opt_error`, `bad_options`; the six
`fatal()` sites in option parsing are gone. Default `-std=gnu17`
(`options_init`); run.sh section 1 passes `-std=c99` to cereal as it
already did to gcc, and `parse/gnu.expected` changed in four lines (gnu17
lexes `::` as one token in `asm goto`, gcc-13 accepts the file); verify.sh,
par.py, corp.sh, cmp.sh and the callgrind line already pass `-std`;
perf_check.py now does. New `src/compdb.[ch]`: `shell_split`, `compdb_load`,
`add_flag` (moved from config.c), `entry_is_c` and `entry_options`, shared
by the replay and the LSP (`config.c` shrank by about 150 lines). The
replay (`main.c`): phase 1 on the main thread picks entries (FILES by
absolute path or directory; none matching is exit 2), classifies C by
`-x`/suffix, parses options into a memory stream, fingerprints
(directory plus every diagnostic-affecting word) and drops duplicates;
phase 2 builds each entry's `Options` on the worker (`run_jobs`, shared
with the other modes); output is committed in database order, with
`Entering directory` lines on stderr and option messages aggregated
(`(N entries, first F)`). Exit 1 for errors in a checked file, 3 for an
entry not checked (unknown option, unsupported `-std`, unreadable file),
else 0; 2 for usage; `-j` defaults to the core count; a replayed `-MD`
writes no `.d` file. Unknown clang/other flags make an entry "not checked";
`.h` entries are skipped as not C.
Found by the TSan gate and fixed here: the checker's per-check mutable
globals (cattr.c `alloc_*`, `acc_*`, `imp_*`, `ctx_vname`, `zero_warn_*`;
cconv `uc_*`; cformat `fmt_*`; cinit `g_pedw`, `digest_bitfield`; cparm
`aka_print`; cprint `pcond_plain`; cwarn_expr `no_int_bool`; three lazy
caches in diag.c `option_state`) raced when several files were checked at
once. That was already true of `cereal check a.c b.c` at `-j>1` before this
round; they are `__thread` now (the LSP builder is one thread, so unchanged).
`cfg_gen_next` is atomic; the replay stamps one `__DATE__`/`__TIME__` for
all entries so workers never call `localtime`.
Tests: `tests/ccdb` (17 cases: arguments/command forms, relative `file`
and `directory`, `-I` against the entry directory, duplicates, `.S`/`.cpp`/
`.h` not C, `-x c` on `.cpp`, unknown option and `-std=c2x`, missing file,
FILES as file/directory/two/nothing, `-Werror`, cereal's `-Wall` after the
entry's flags, `-MD -MF` writing nothing, `-o` and `lsp` refused, bad and
missing database), run.sh section 16 at `-j1` and `-j12` byte-identical.
`bench/tools/mkccdb.sh` (database without a build system) and `ccdb.sh`
(`ccdb.py`: gcc-13 per entry in its directory vs the replay, normalized as
corp.sh does, plus the verdict). Gates: run.sh 1715 passed (STATUS said 1693 at
Round 200; this round adds 18 checks: the 17 cases and
the `.d` check); san 1715 passed, 605 files, 0 findings; verify gcc.dg
3908 of 3910, c-c++-common 635 of 636, cpp 285 of 285 (unchanged); symcov
4716 files, 0 unindexed (10 skipped); uvloop loop.c 3.870 G instructions
(3.886 G before, -0.4%); all 17 LSP sessions (both modes) under TSan: 34
passed, 0 warnings; the 17 replay cases at `-j12` and replays of Lua 5.4
(33 entries), libuv unix+common (41), cereal's own sources (62) and
tests/check (597) at `-j8` under TSan: 0 warnings. Real replays against
gcc-13 per entry: Lua 33/33 and libuv 41/41 identical (libuv: 2 files with
errors, the same as gcc's), cereal's sources 62/62 identical; with
`-Wall -Wextra` 11 of them differ, all cereal-only `statement with no
effect` (37) and `suggest parentheses around assignment used as truth
value` (21) lines, identical with the HEAD binary run directly (not a
replay effect; noticed, not fixed). Lines: src +597/-336 in tracked files
plus 259 new (compdb.[ch]) (main.c +399, driver.c +220/-; compare est.
370, realistic 550 to 750. Not done: slice 2 (`@file`), slice 3 (`-M`).

Round 205: the cereal-only warnings of Round 204, two classes, both fixed
at their shared source. Reproduced all 11 files (`mkccdb.sh` over cereal's
62 sources with `-std=gnu17 -Wall -Wextra -I.`, `ccdb.sh`): 21 `suggest
parentheses around assignment used as truth value` and 37 `statement with
no effect` lines, and nothing else.
(1) `e_assign` (cexpr.c) warned for every `_Bool = a = b`. gcc warns where
the inner assignment is converted to a truth value, i.e. when its type is
not `_Bool` (`b = i = j`, `b = p = q`, `b = d = 1.0` warn; `*r = *m = false`,
`c = b = i`, a `_Bool` bit-field target do not; probed on small reductions).
The condition now also requires the inner assignment's type not to be
`_Bool`. (2) `call_pure` (ccall.c) matched a call to any declared function
against `cbuiltin_pure`, a table of `__builtin_NAME` suffixes, by plain
name, so cereal's own `static bool expect(Parser *, Punct)` was the pure
`__builtin_expect` and every unused call was `statement with no effect`.
A plain name now has to be a library built-in (`ccall_is_builtin`: the same
rule `bt_find` applies everywhere else); the prefixed spelling is as before.
Goldens `tests/check/bool_chain_assign` and `call_user_builtin_name` (warning
lines checked against gcc-13 first; cereal prints no "In function" lines and
shorter carets, as for all goldens). The replay over cereal's sources with
`-Wall -Wextra`: 62 of 62 identical to gcc-13.
Files: src/c/cexpr.c (+3/-1), src/c/ccall.c (+5), the two goldens.

Round 206: ROADMAP A4 slice 2, `@file` response files (A4_DESIGN.md section
5, results under "Slice 2"). `shell_split` became `split_args(a, s, mode,
&argv)` with `SPLIT_SHELL` and `SPLIT_GCC` (libiberty buildargv: a
backslash escapes the next character everywhere, quotes only group, an
unterminated quote runs to the end, every token is an argument); new
`argv_expand(a, dir, &argc, from, argv, &err)` reads, splits and splices,
rescanning for nested files, names relative to `dir` (the entry's directory)
or the process directory, never to the naming file; the 2000th expansion is
`too many @-files encountered`, a directory is `@-file refers to a
directory`, and a missing or unreadable file is `cannot read response file
'F'`: an error (documented divergence from gcc's literal fallback, Q4).
Hooks: `main` expands its argv first (so a file may hold the mode word;
exit 1 on error) and `entry_options` expands an entry's arguments into the
new `Options.rsp` arena (freed by `options_free`; options point into the
strings), the error counting as a bad option, so a replayed entry is "not
checked". Bug caught by the LSP sessions: an entry without arguments
(`none`) must not be expanded. Probes (gcc-13): a `''`-only file is one
empty argument, a trailing `\` too, 1999 expansions pass, `-D @f` expands
`@f`; gcc 15 keeps backslashes inside quotes, so the differential uses
`$RSPCC` (default gcc-13). Tests: run.sh 13b (18 `tests/rsp/*.rsp` against
gcc-13 on stdout and exit class, four error cases, the mode word from a
file), `tests/ccdb/rsp` (nested file in `src/rsp`, quoted flag, relative to
the entry directory) and `rsp_bad` (missing, directory, self-recursive,
wrong directory; exit 3). Lines: src about +125/-45 (compdb.c +95,
compdb.h +20, main.c +14, driver 2), est. 75, realistic 110 to 150.
Not done: `-x` inside a response file is not seen by the entry's language
check. Gates for rounds 205 and 206 together: run.sh 1744 passed; san 1744
passed, 607 files, 0 findings; verify gcc.dg 3908 of 3910, c-c++-common 635
of 636, cpp 285 of 285 (unchanged); symcov 4716 files, 0 unindexed; uvloop
loop.c 3.881 G (3.870 G before, +0.28%); all 17 LSP sessions (both modes)
under TSan: 34 passed, 0 warnings.

Round 207: A4 slice 2 gap closed, and the A4 section 6.6 probes (A4_DESIGN.md
sections 4.3, 6.1, 6.6, 6.9, 6.10). Task 1: `entry_is_c` read the entry's
own words, so `-x c++` inside an `@file` left the entry checked as C. The
expansion now has one place, `entry_expand` (compdb.c: `argv_expand` of the
entry's arguments against its directory, the unexpanded arguments on
failure); `entry_options` and `entry_is_c` both read its result (the latter
expands into a scratch arena, because the classification runs before the
entry's `Options` exist). A failed expansion classifies on the words as
written and the error is still reported only for entries that are C, as
before. The LSP never called `entry_is_c`. Golden `tests/ccdb/rsp_lang`
(`-x c++` from a file makes a `.c` entry "not C"; `-x c` from a file makes a
`.cpp` entry checked) with `src/xcxx.rsp`, `src/xc.rsp`. Files: src/compdb.c
(+37/-18), src/compdb.h (comment), the golden (3 files, 9 lines) and the two
rsp files. Task 2 (docs only, no cereal code kept): the probes of 6.6 and the
phase A question, against gcc 13.4.0. Findings: the `-M`/`-MM`/`-MD`/`-MMD`
style is the strongest present in the order MD < MMD < M < MM whatever the
command-line order (so `-MD -MMD` is `-MMD`); the `-MQ` "last first" is a
rotation, `-MQ` targets rotated left by #`-MT` mod #`-MQ` after the `-MT`
ones (in order when there is no `-MT`), so the accepted divergence of 6.9 is
dropped and slice 3 copies the rule; `-imacros` files come before the
preinclude, not after `-include` (6.1 had it wrong); `-Wfatal-errors`
suppresses the `.d` (and `-M`'s rule on `#error`); a missing `-include` or
`-imacros` is a fatal `<command-line>` error, listed as spelled under `-MG`
(also under `-MM -MG`); `#include` of a directory is "No such file" (the
search goes on past directories), listed as spelled under `-MG`; `#line`
changes no include directory. Phase A, by an instrumented copy
(`~/cereal-mp`, deleted): the include/enter/exit event streams of
`-fparallel=on` and `off` are identical on uvloop `loop.c`, lupa, zstd,
four cereal files and a guard/once/include_next/computed/`#if` test, and the
first-entry file order equals gcc-13 `-M` on 8 files up to 352 headers; no
assumption of section 6 or D7 changes. Added requirements: `dir_index` is
required (paths arrive normalized), the writer must belong to the TU whose
result is used (a divergence or `PAR_FALLBACK` leaves an abandoned phase A
TU that saw the whole stream), replayed `file_enter` events have no `via`.
Gates: `GOLDEN_JOBS=4 sh tests/run.sh` 1745 passed, 0 failed (1744 + the new
golden); san.sh 1745 passed, 607 files, 0 findings. Not run: verify.sh,
callgrind, TSan (config.c untouched).

Round 208: A4 slice 3, the `-M` family (A4_DESIGN.md sections 4, 6, 9). One
preprocessor listener (new src/deps.c, 297 lines: records each `#include`
by gcc's (directory object, name) key, so the same path reached through a
search directory and through an includer's directory is listed twice, as
gcc does) and one writer run from `finish()`; `-M -MM -MD -MMD -MF -MT -MQ
-MP -MG` all work, including `-MD` naming, the `-MQ` rotation, 72-column
wrapping, `-MF -`, the link-mode `a-STEM.d` name, and "no `.d` once a
diagnostic ended the TU". The listener is attached in `tu_begin` unless the
PP replays a plan; D5 holds (replay entries clear `deps`). New: `IncludeEvent`
`dir_index` and `system`; `PPOptions.missing_ok`; `Options.deps`, `.stage`
(src/driver.[ch], pp.[ch], main.c, compdb.c, tools/probe-host.sh writes
`host_preinclude`). Option errors are gcc's ("to generate dependencies you
must specify either '-M' or '-MM'", `-MG` needs `-M`/`-MM`). Found by
differential testing, not in the probes: pure `-MM` (without `-MD`) skips a
missing system-ish header silently with exit 0 (`-M` and `-MM -MD` fail);
gcc creates an empty `-o` file for `-M -MD -o f`; `-M` truncates `-o`.
Tests: tests/deps (100 cases, gcc-13 goldens, driven by tests/deps_case.sh),
run.sh section 17 (goldens; `-fparallel=on` against `off` for `-M` on
tests/pp, tests/lint, src and for the `-MD` file with a `__COUNTER__`
divergence), section 13 no longer lists the -M options, one ccdb case
(`-M --compile-commands` is a usage error). bench/tools/deps.sh: no
argument runs the cases through gcc-13 and cereal (100 cases, 0 differ);
with DIR it runs `-M`, `-MM` and `-E -MD -MP -MF` per C file: libuv 849 runs (375
byte-identical, 474 same after normalising the compiler's include
directory, 0 differ), lua51 105, lua54 120, luajit21 231, zstandard 54,
cereal's src 189: 1548 runs, 0 differ. Gates: run.sh 1895 passed; gate.sh
(run, verify, san, cg) rc 0, san 1895 passed, 607 files, 0 findings, gcc.dg
3908 of 3910, c-c++-common 635 of 636, cpp 285 of 285 (unchanged), callgrind
3.899 G. Not done: `-imacros` is not implemented (so its missing-file case
is untested); deps.sh has no compile-commands mode; the four `missing-*`
verify.sh tests and the `par.py` change of the design are not done.
Extensions: `-MD`/`-MMD` with index/query and `-M`/`-MM` with
`--compile-commands` are usage errors (exit 2).

Round 209: A4 follow-ups, `-imacros` and the `missing-*` tests (A4_DESIGN.md
6.11). `-imacros FILE` shares the `-include` mechanism: `tu_begin` writes
`#__include_macros "FILE"` (a directive only the `<command line>` file may
use) next to `#include "FILE"`; the include frame (and the plan frame, for
the parallel path) carries `quiet`, and `pp_next` drops the tokens of a
quiet file and of what it includes before macro expansion, as libcpp's
`cpp_scan_nooutput` does (a `__COUNTER__` there is not consumed). The
driver now emits `-D`/`-U`/`-imacros` in command-line order and then every
`-include`, gcc's order; this also repairs `-include F -DQ` (F saw no `Q`).
A missing `-include`/`-imacros` file prints gcc's position-less
`<command-line>: fatal error:`. The `-M` list puts what `-imacros` entered
(`DepEnt.pre`, `IncludeEvent.macros_only`) before the preinclude, also for
`-MG` misses. `bench/tools/par.py` accepts `-MD`/`-MMD` and runs those tests
in `$TMPDIR/par-md`: gcc.dg/cpp is 289 files (was 285), 289 identical.
Sources: src/pp.c, pp.h, plan.h, driver.c, driver.h, deps.c, compdb.c
(`-imacros` is a path option in entries), main.c (usage), about +45 lines net.
Tests: 9 tests/deps cases (`imacros*`, gcc-13 goldens via DEPS_UPDATE, none of
the 100 old goldens changed), six differential run.sh cases on
tests/ppstd/imacros.c against gcc. Gates: run.sh 1910 passed (was 1895);
deps.sh 109 cases, 0 differ; gate.sh rc 0: san 1910 passed, 607 files, 0
findings; gcc.dg 3908 of 3910, c-c++-common 635 of 636, cpp 289 of 289;
callgrind 3.881 G (3.899 G in Round 208). Not done: deps.sh compile-commands
mode (over the 40-line budget); `-E` prints no line-0 prefix before the
fatal missing-file error; warnings inside `-include`/`-imacros` files are not
shown (the `<command line>` file is system); `-imacros ${srcdir}/...` tests of
c-c++-common stay skipped by par.py.

Round 210: `<command-line>` is an ordinary virtual file (A4_DESIGN.md 6.3, 6.11).
Cause: `pp_enter_main` set `system_header` on the command-line buffer, and
`pp_find_include` loads a quote-include as `SF_SYSTEM` when the includer has
that bit, so every `-include`/`-imacros` file was a system file: its warnings
were suppressed and `-E` printed `# 1 "F" 3`. The mark had no other purpose
(removing it changed no golden, no deps case, no verify file). One mechanism
now: diagnostics in a virtual file print `FILE: ` with no position
(`diag.c print_loc_line`, also the `nocol_next` the missing-include path set),
the include trace through it prints `In file included from <command-line>:`
with no line, and the file is named `<command-line>` (was `<command line>`,
printed through a special case); `cbuiltin_loc_` tests `SF_VIRTUAL`. The
redefinition warnings of the generated directive lines are real gcc warnings
(`-DX=1 -DX=2`, `-D__GNUC__=4`); a sweep of 25 flag sets showed none
spurious. Goldens: tests/check/cmdline_{include,error,missing,macro_error}
(+3 headers in cmdline_inc/). `bench/tools/par.py` substitutes `${srcdir}`
(the testsuite directory, as dejagnu; the brief said the test's directory,
but `${srcdir}/c-c++-common/F.h` needs the parent) and takes `-imacros FILE`:
pr57653 and pr57653-2 run, identical. Sources: src/pp.c, pp.h, srcmgr.h,
diag.c, c/check.c (src +12/-15, par.py +6/-5). Gates: run.sh 1918 passed (was
1910, +8 from the 4 new goldens and their cells runs); deps.sh 109 cases, 0
differ; gate.sh rc 0: san 1918 passed, 611 files, 0 findings; gcc.dg 3908 of
3910, c-c++-common 637 of 638 (was 635 of 636), cpp 289 of 289; callgrind
3.882 G (3.881 G). Not done: gcc spells a file found through the command-line
buffer `./F` (`././F` for `-include ./F`), cereal prints the normalised `F`
as it does everywhere; no `FILE: In function` context lines (nowhere);
the "previous definition" note for a redefined host predefine says
`<command-line>`, gcc `<built-in>`; `-U__FILE__` gives no "undefining"
warning; with `--cells` a malformed `-D` error is ordered after an error in
an `-include` file.

## Round 211: BOM on the mmap path

A file of 64 KiB or more that starts with a UTF-8 BOM failed with
`unknown type name '\U0000feffint'` (gcc accepts it): only the `read` path in
`srcmgr_load` stripped the BOM. The strip now runs after both paths (the
mmap mapping is MAP_PRIVATE and writable, so the `memmove` is a private copy).
The overlay path (LSP buffers) still keeps a BOM; C_DESIGN section 2 covers
it. Test: tests/run.sh section 18 (small and 70000-byte-padded BOM files).
Gates: run.sh 1920 passed; gate.sh rc 0: san 1920 passed, 611 files, 0
findings; gcc.dg 3908 of 3910, c-c++-common 637 of 638, cpp 289 of 289;
callgrind 3.882 G. C_DESIGN decisions Q1-Q10 taken as recommended (user:
"all yes"); Q7 resolved by this round, not bundled into C1.

## Round 212: ROADMAP C1, fix-its in the diagnostic engine

C_DESIGN.md slice C1. A `Diagnostic` now carries `VEC(FixIt) fixits`
({begin, end, text, owner, applicability SAFE|MAYBE|INFO}; owner 0 is the
diagnostic, k a note) and a per-owner `nofix` poison; the old `fixit` string
is deleted and its three lint producers (cond-typo INFO, hygiene parameter
parentheses SAFE, header-guard SAFE) moved to `fix_tok_replace`. All fix-its
enter through `diag_fixit` (user file, one line, newline only as the last
character of a column-1 insertion, touching hints merged, a refused hint
poisons its owner), with token helpers that refuse macro-origin tokens
(`fixtok.c` for `Tok`, `c/fixptok.c` for `PTok`). Producers in the checker
and parser: header notes (`cheader_note`, once per header, at the line after
the last `#include` before the use in the file of the use), did-you-mean for
identifiers, implicit functions, members, designated members, unknown type
names and `#elsif`, `.` to `->` and the pointer-to-pointer `(*` `)` pair,
and expected `) ] ; , :` (after the previous token, `;` MAYBE, the rest
INFO) and `(` (before the token found). Cells carry the fix-its in memory
(`CellFix`, `CN_*` note flags; no format, so no version); `par.c diag_copy`
deep-copies them. Output: `-fdiagnostics-parseable-fixits`, a JSON `fixits`
array on diagnostics and notes, `-fdiagnostics-generate-patch` and the edit
builder `fixset.c` (sort, dedupe, same-point order, touching is not overlap,
an overlapping later group skipped whole), shared with `cereal fix
--edits=FILE --diff` (`fixmode.c`; the rest of `fix` is C3). Probes P11 to
P13, P18, P19 (C_DESIGN section 13) changed four design points, listed in
section 8 "C1 as built". Tests: eight `tests/check/fixit-*.c` goldens, six
`tests/fix` builder cases (run.sh section 19), BOM/CRLF/64 KiB columns
against gcc-13 (section 20), fix-its on in the parallel and `-j` comparisons;
`bench/tools/fixits.sh` (differential) and `par.py CEREAL_FIXITS=1` with a
`verify.sh` line. Differential over 7042 gcc.dg and c-c++-common files: 0
differ, 31 differ in the diagnostic itself (listed in C_DESIGN); patches
over the 198-file survey: 0 differ. `verify.sh`: gcc fix-its identical in
102 of 103 files (spellcheck-inttypes: `PRIx8` not in `std_names`). Source
+1284/-128, tests +369, bench +135 lines. Gates: run.sh 1989 passed (was
1920 + 69 new); gate.sh: san 1989 passed, 619 files, 0 findings; gcc.dg 3908
of 3910, c-c++-common 637 of 638, cpp 289 of 289 (unchanged); callgrind
3.895 G (3.882 G); the LSP sessions (both modes) under TSan: 34 passed, 0
reports (`server.c` only frees the new vector), and the new goldens at `-j8`
under TSan: 0 reports. Not done: the eight goldens that put "is declared in
header" notes at the use (gcc: first per header at `include_loc`, with a
fix-it) are unchanged; see C_DESIGN "Known gaps". Scratch lived in
`~/c1cf`, not `/tmp` (the WSL tmpfs is wiped when the distro idles).

## Round 213: ROADMAP C2, checker and cereal-only fix-its

Part 1. The "'f' is declared in header" notes (`ccall.c`, builtin mismatch)
now use `cheader_note`: first note per header at `include_loc` with the
`#include` fix-it, as gcc. Four goldens changed (`attr_fmt_noproto`,
`builtin_decl_mismatch`, `misc_122`, `misc_123`), each checked against
gcc-13's note positions. Parity gaps closed: `INTPTR_MAX`/`UINTPTR_MAX` and
the `PRI*`/`SCN*` macros after a string literal (the `<inttypes.h>` hint in
`expected()`/`expect()`), and the `case` colon message (`expected ':' or
'...'`). Skipped: attribute/asm operand `)`/`]` fix-its, function
did-you-mean for implicit declarations, `__FILE_`/`__LINE_`/`__builtin_`
did-you-mean, the C89 "include <h> or provide a declaration" note; each
needs its own mechanism.

Part 2. Slice C2 as scoped in C_DESIGN: missing braces (SAFE; gcc's push and
pop model replicated, with two root-cause fixes in `cinit.c`: `zeroinit`
after conversion, float truncation in `post_zero`), logical-not note, xor as
pow, "use 'struct' keyword", format specifiers (MAYBE), hygiene body parens,
assignment parens, strict-prototypes `(void)`. `diag_fixit` refuses columns
above 4096 as gcc does; the logical-not note is skipped for constant
operands as gcc does. `verify.py` learns `// expect-fixit: L:C-L:C "text"
APP`. Tests: nine `tests/check/fixit-*.c` goldens, `tests/lint/hygiene_fixit.c`.
`fixits.sh`: `FIXIT_COVER` widened, `FIXIT_SKIP` removed. Differential over
7042 files: 0 differ and 31 diag before; 2 differ and 32 diag after (the
two zero-size-array missing-braces files; added diag files gnu2x-attrs-2,
pr65066, store_merging_28 are pre-existing gaps newly covered; closed:
missing-symbol-2, spellcheck-inttypes). Source +346/-44 in 14 files, tests
+50/-17 plus 160 lines of new sources and 212 of expected output, bench
+6/-7. Gates: run.sh 2014 passed (was 1989); gate.sh: san 2014 passed, 628
files, 0 findings; gcc.dg 3908 of 3910, c-c++-common 637 of 638, cpp 289 of
289 (unchanged); callgrind 3.899 G (3.895 G). `fixits.sh` with no argument:
17 cases, 0 differ. TSan LSP not run (lsp/ and config.c untouched). Not
done: header-guard and cond-typo INFO fix-its, `%1$d` format fix-its.

## Round 214: ROADMAP C3, `cereal fix`; C2 leftovers

Part 1. `cond-typo` and `header-guard` fix-its (`include.c`): cond-typo is
INFO; header-guard is SAFE, MAYBE when the misspelt name is tested anywhere
else in the unit (`tested_anywhere`). Goldens `tests/lint/guard_fixit.c`
(with `inc/guard_safe.h`, `inc/guard_maybe.h`) and `cond_fixit.c`;
`verify.py` takes an optional `FILE` prefix in `expect-fixit` for a fix-it in
a header.

Part 2. Slice C3 as scoped in C_DESIGN (section 8 "C3 as built" has the
structure and the deviations). `cereal fix` is a mode: a `TuFn` per unit
(`fixtu.c`: check, lint analyses, `skipped` listener, `FixResult`), a merge
over all units by real path (D5, D6/Q10, Q3), a three-phase gate with
bisection and demotion (strict, parentheses with the AST digest of the new
`c/cdigest.c`, lenient), re-runs with the overlay (crename's method), atomic
write keeping BOM and line ends (`file_write_atomic`), up to 8 passes and a
probe pass, exit 0/1/2/3/4. Shared plumbing moved to `jobs.h` (`TuFn` and
`TuJob` gained `res`, `run_jobs` a `dirs` flag, `mode_compdb` split into
`compdb_plan`). `DiagNote.hdr` lets the gate see that an include insertion
also removes the builtin-declaration-mismatch warning that names the same
header. Root-cause fixes found by the new gates: gcc's no-op format fix-its
(`%u` for `%u`) made `fix` loop to the pass limit, so collect drops groups
that change nothing; a note's fix-it overlapping its own diagnostic's (xor:
`0x2` against `1 <<`) was reported as a conflict, so it is dropped as the
alternative it is; `qsort` on an empty vector's NULL tripped UBSan, so the
file uses `sortn`; the summary counted a file once per pass, now once.

Tests: thirteen `tests/fix/NAME.sh` cases with goldens (run.sh section 21,
each at `-j1` and `-j12`, which must agree): safe, maybe, gate, passes,
crlf, bom, files (read-only, hard link, symlink, outside root), unwritable,
changed (a file saved mid-run), diff, only, compdb (shared header fixed once,
D5 disagreement), corpus (every fix-it input, `--unsafe`, twice).
`bench/tools/fixgate.sh [DIR]`: second run a no-op, no new diagnostics, gcc-13
still accepts, same `.text` for SAFE (not compared when an `#include` was
inserted: `scanf` becomes `__isoc99_scanf`; a stringized macro argument
changes data only): 24 cases of the tests, 620 files of `tests/check`, 1469
files (every third) of gcc.dg, 0 failed, 111 of them with edits. A 400-file
gcc.dg run gives identical files and output at `-j1` and `-j12`; the fix mode
under TSan at `-j2/4/8/12` (150 gcc.dg files) and the compdb, maybe, safe and
files cases at `-j4/12`: 0 reports (`~/cereal-tsan`, removed). Gates: run.sh
2047 passed (was 2014); gate.sh: san 2047 passed, 628 files, 0 findings;
gcc.dg 3908 of 3910, c-c++-common 637 of 638, cpp 289 of 289 (unchanged);
callgrind 3.895 G (3.899 G); `fixits.sh`: 17 cases, 0 differ. Source +1564/-83
in 13 files plus 505 lines in 5 new ones, tests +252 lines of cases and 530
of goldens, bench +159. Not done: a final combined re-verification of the
gated phases, a conflict case from real diagnostics, C4. Scratch lived in
`~/c3s` (removed).

## Round 215: ROADMAP C4, LSP code actions; C3 nits

Two C3 nits. `cereal fix --only=ID` with an id that is not the id of a
diagnostic with a fix-it (`--only=nothing` included) is a usage error, exit 2,
naming the id (`FIXABLE_IDS` in `fixmode.c`; `tests/fix/only.sh` catches
drift). An unwritable file prints its error once per file per run, not once
per pass (`Totals.failed`); goldens in `tests/fix`.

C4 (C_DESIGN section 7, "C4 as built"). `codeActionProvider` (`quickfix`,
`source.fixAll.cereal`) is advertised only when the client sends
`codeActionLiteralSupport`; `documentChanges` (with the document version) or
`changes` follows the client's `workspaceEdit` capability. The request has the
new policy `CP_SNAP`: it answers from the snapshot only if `snap->gen` is the
wanted generation, else `[]`. Check-phase fix-its are stored on `CDiag`
(`CFix`), set from the checker's `Diagnostic` after the diagnostic is placed;
`cdiags_carry` maps each fix through `carry_range`, and a group that cannot be
carried is dropped while its diagnostic stays. Lint-phase actions come from
the same groups, validated by the new shared `diag_fix_group` (also used by
`fixtu.c` collect, so the CLI and the server agree on what a group is).
SAFE groups are preferred; MAYBE and INFO are offered without preference.
Fix-all runs on the builder thread through the generalized `BuilderJob`
(rename became one), calling `fix_unit_edits`, which is `cereal fix`'s
collect, select, gate and sub-build for one TU over the editor buffers
(`FixResult.base`, `Pass.base`: an overlay lookup instead of disk), one pass,
with the same gate. Results are cached per snapshot and document.
`tests/lsp_session.py` got a `fixall` step: it applies the returned edit and
checks the text against `cereal fix` run on a copy.

Deviations (also in C_DESIGN): fix-all is one pass, not the CLI's loop;
`fatal_missing_include=false` in the server run; the first fix-all request
blocks the protocol thread for about one builder latency; a hook
`cereal/holdChecks "builds"` was needed (a held check does not make a
snapshot stale); a fix-all is offered when the document has a SAFE group even
on lines without a diagnostic; the gate's re-runs in the server are serial
with their diagnostics discarded (stdout is the protocol).

Tests: four sessions, `fixit_actions`, `fixit_carry`, `fixit_stale`,
`fixit_nolit` (plain and with forced parallel flags; 8 goldens). Gates:
run.sh 2055 passed (was 2047); gate.sh: san 2055 passed, 628 files, 0
findings; gcc.dg 3908 of 3910, c-c++-common 637 of 638, cpp 289 of 289
(unchanged); `fixits.sh` 17 cases, 0 differ; `fixgate.sh` 24 cases, 0 failed.
TSan: all 21 LSP sessions, plain and `-fparallel=on -fparallel-chunk=1
-fparallel-threads=3`: 42 passed, 0 reports; `lsp_stress.py` 200 edits on
zstd.c, and a scratch variant adding codeAction and fix-all requests under
edits on `fixit/main.c`: 0 reports (`~/cereal-tsan`, removed). Source
+843/-68 in 9 files; tests about 2500 lines (sessions, goldens, sources) plus
75 changed.

Noticed, not explained: callgrind on uvloop `loop.c` is 3.916 G against 3.898
G for a build of the previous commit with the same flags (+0.5%; round 214
had 3.895 G). The difference is stable across copies of the binary; the
`diag.c` change is only an added function and the check path calls none of
the new code. `__strcmp_avx2` accounts for 9 M of it (350 k more calls through
the PLT). Not chased further. Scratch lived in `~/c4s` (removed).

## Round 216: the Round 215 callgrind rise; C4 follow-ups

Callgrind (found, fixed). The +0.5% on uvloop `loop.c` was not the new code:
callgrind of 943a38e (3.8949 G) and d5a587f (3.9157 G), built the same way,
differ by 20.8 M, all of it in four functions of `diag.c`/libc: `__strcmp_avx2`
+10.1 M, `name_matches` +6.0 M, `__strlen_avx2` +2.4 M, `find_index_cached`
+1.7 M. `find_index_cached` (the option lookup of `diag_level_for`, 1,155,595
calls in both builds) keys a 512-entry direct-mapped cache on the address of
the id string, hashed as `(addr >> 3) * 0x9E3779B1u >> 7 & 511`: the low bits
of a product depend only on the low bits of the address, and which of the
~190 literals collide depends on where the linker puts them. The new code
moved the literals; misses went from 105 k to 279 k, and each miss scans the
option table with `name_matches` (strcmp, strlen). The call sites were
identical. Fix: the high bits of a 64-bit multiplicative hash (one line, plus
a static assert that the table has the 512 entries it takes 9 bits for).
uvloop `loop.c`: 3.9157 G -> 3.8852 G (-0.78%), 0.25% below 943a38e's 3.8949 G.
A 2048-entry table with the same hash measured 3.8867 G, so it was not kept.
The cache remains layout-dependent in principle (any new literal can still
collide); the high-bit hash makes that a random draw of a small effect
rather than a systematic one.

C4 follow-ups (C_DESIGN "C4 follow-ups (Round 216)"). (a) The LSP fix-all
now loops until stable like `cereal fix`: `fix_unit_edits` repeats the
one-unit pass (now `unit_pass`) up to the new `FIX_MAX_PASSES` (8, also the
CLI's `--max-passes` default), each pass over the buffers with the
document's text replaced by the previous result (`Layer`). After one pass
the edits are returned as they are; after more they become one edit from the
first changed byte to the last, since later passes' edits are relative to
text the client does not have. (b) The first fix-all request no longer holds
up the protocol thread. Finding: the brief said to do it the way rename is
answered, but rename waits on the protocol thread too (`run_on_builder`);
nothing was asynchronous. New mechanism: `lsp_fix_all` on the protocol
thread only sets `Req.need_builder` when nothing is cached; `handle_request`
then hands the request (message body and arenas, snapshot and index
references) to the builder as a `DeferredReq` and returns true so the main
loop does not free it; the builder runs it after a `BuilderJob`, before the
next build, runs the code action again with `Req.on_builder` (fix-all now
computed and cached) and writes the response. The document having changed
since the request answers `[]`. Responses may come out of order. A deferred
request pending at exit is dropped. `cereal/holdChecks` also holds a fix-all
at its start.

Tests: `fixit_actions` got `fixit/passes.c` (`p.x + q.x`, two passes) and a
`fixall` step (equal to `cereal fix`; fails on d5a587f); new session
`fixall_async` (a fix-all held, a hover and a quick fix answered, the fix-all
after the release, the second one from the cache; fails on d5a587f);
`lsp_session.py` steps `defer`, `await`, `unanswered`. Gates: run.sh 2057
passed (was 2055); gate.sh: run 2057, san 2057 passed, 628 files, 0
findings; gcc.dg 3908 of 3910, c-c++-common 637 of 638, cpp 289 of 289
(unchanged); callgrind 3.8852 G. TSan: all 22 LSP sessions, plain and with
`-fparallel=on -fparallel-chunk=1 -fparallel-threads=3`: 44 passed, 0
reports; `lsp_stress.py` 200 edits on zstd.c (12 min), and the scratch
variant adding `codeAction` and fix-all requests after each edit on
`fixit/main.c`: 0 reports (`~/cereal-tsan`, removed). Source +237/-20 in 5
files (diag.c, fixmode.c, fixmode.h, lsp.h, server.c); tests about +45
lines changed, `fixall_async` 23 + 279 lines.

Not done: composing a multi-pass fix-all into granular edits (`map_off`
could); the pass limit is not reported to the client; rename still blocks
the protocol thread (the same deferral would serve it). Scratch lived in
`~/c5s` (removed).

## Round 217: rename uses the deferral

The C rename's checks (`lsp_check_rename`) no longer wait on the protocol
thread. `c_rename_edits` sets `Req.need_builder` and writes nothing when it is
not on the builder (after the existing "index not ready; retry" refusal, so a
stale or unchecked snapshot is still refused at once, RequestFailed -32803);
`handle_request` defers it as a `DeferredReq` (new field `kind`), and
`deferred_run` runs `lsp_rename` on the builder and answers. `lsp_check_rename`
is now a plain call of `rename_run`. The `cereal/holdChecks` hold moved from
`lsp_fix_all` to the start of `deferred_run`, so it holds a deferred rename and
a fix-all alike (for a code action the stale test now comes after the hold).
Deleted: `run_on_builder`, `BuilderJob`, `Server.bjob` and the builder loop's
`bjob` branch; no other caller existed (grep over src/ and tests/). prepareRename
never used the builder (`cindex_rename_plan` reads the index) and is unchanged;
macro renames are unchanged (index only).

Semantics. The reply is the same edit as before. Stale input at arrival gives
what it gave before (the "retry" error). New, a consequence of the protocol
thread being free: a document change while the rename waits or runs gives a
ContentModified (-32801) error, "the document changed during the rename", not
edits for the old text (a fix-all answers `[]`; for a rename the error is the
LSP convention and a client can retry). Before, no edit could arrive during
the wait, so the case did not exist. Out-of-order replies; a deferred request
pending at exit is dropped (existing `deferred_free` at exit).

Tests: new session `csym_rename_async` (a C rename held by `holdChecks` while a
hover and a prepareRename are answered; the edit after the release; a second
rename held across a `didChange` gives -32801; a rename on the new snapshot).
The existing rename sessions (`csym_rename`, `csym_b3`, ...) are unchanged and
pass. Gates: run.sh 2059 passed (was 2057: +1 session x 2 modes); gate.sh:
run 2059, san 2059 passed, 628 files, 0 findings; verify gcc.dg 3908 of 3910,
c-c++-common 637 of 638, cpp 289 of 289 (unchanged); callgrind 3.8833 G
(3.8852 G). TSan (`~/cereal-tsan`, removed): all 23 LSP sessions, plain and with
`-fparallel=on -fparallel-chunk=1 -fparallel-threads=3`: 46 passed, 0 reports;
`lsp_stress.py` 200 edits on zstd.c: 0 reports; a scratch variant (a rename of
a static function, then an edit at once, per iteration; 40 edits on zstd.c): 164
ok, 35 ContentModified, 5 refusals, 0 reports. Source +62/-69 in 3 files
(server.c, features.c, lsp.h); tests +178 lines (28 + 150).

Not done: the rename's -32801 can be hit often by an editor that types while
the rename check runs (35 of 40 in the stress, which edits at once; a person
would rarely); the client retries. A cancel (`$/cancelRequest`) of a deferred
request is not handled (as for the fix-all). Scratch lived in `~/c6s` (removed).

## Round 218: B4a, C call hierarchy

Slice B4a of B4_DESIGN.md, as designed (all D and Q decisions as recommended).
`CIX_CALL` (64) marks a REF of a function that is the callee of a call
(`call_callee` in cexpr_int.h; the identifier and implicit-declaration hooks in
cexpr.c; the member hook in ccall.c); dedupe ORs it, the dump prints ` call`,
`--verify-carry` compares it, and `--verify-symbols` reports
`unindexed 'NAME' (call)` for an identifier callee without a flagged event
(a scratch run with the flag removed from the identifier hook gave 25 reports).
A callee spelled in a `#define` body gets an `xcall` (functions only, a side
array, carried and verified) at the invocation. `csymidx.c` gained `cindex_site`,
`cindex_calls_to`, `cindex_calls_in` (caller by root scope, 5.1). features.c:
prepare/incoming/outgoing for C functions and file-level variables (a strong
macro first), grouped by the other function; items carry `data` {c,name,doc};
follow-ups resolve by position and name, then `data.c`, then name (5.5);
`request_uri` prefers `data.doc` (E4); `req_loc_in`. Policies: prepare `CP_POS`,
incoming/outgoing `CP_WAIT`. `cereal query callers|callees` answers C names.

Found: the xcall for `ID(CALL_LEAF(2))` is at `ID` (confirmed by the dump); the
call bit of the name just before an inserted `(` differs between a carry and a
fresh index (`carry/calls_adj`, 2 differences; 4.2 expected none);
`tests/query/calls.expected` last line changed (`no callers` for a C variable).
UBSan found `qsort(NULL, 0, ...)` in `cindex_calls_in`/`call_rows_out` (empty
result); guarded.

Tests: `tests/symidx/calls.c` (dump; plain and cells identical; 0 unindexed),
`carry/calls_{in,adj,out,mac}`, `tests/query/ccalls.cmd` (17 queries and the
field-role check), scripted session `tests/lsp/ccalls` (incl. unopened-header
items with hand-written `data`, a stripped item, stale items after an edit).
Gates: run.sh 2068 passed (was 2059: +9); gate.sh: run 2068, san 2068 passed,
628 files, 0 findings; verify unchanged (gcc.dg 3908 of 3910, c-c++-common
637 of 638, cpp 289 of 289); callgrind 3.8856 G (3.8833 G, +0.06%); symcov
`unindexed 0` over 4718 files (1,146,276 events); TSan (`~/cereal-tsan`,
removed) all 24 LSP sessions, plain and with `-fparallel=on -fparallel-chunk=1
-fparallel-threads=3`: 48 clean; `lsp_stress.py` 200 edits on zstd.c: 0 reports.
Source +690/-62 in 9 files; tests ~300 hand-written lines plus goldens (1,013-line
LSP transcript).

Not done: B4b and B4c. The stress script sends no call hierarchy requests
(none of the TSan sessions race one with an edit); a call in a file-level
`_Static_assert` has no caller item (parked in B4_DESIGN 15).


## Round 219: B4b, parameter-name inlay hints; clangd_diff; call-hierarchy stress

Slice B4b of B4_DESIGN.md, as designed (D1-D6, Q1-Q7 as recommended).
`textDocument/inlayHint` answers parameter hints (`NAME:`, kind 2,
`paddingRight`) for the `CIX_CALL` events spelled in the file's own text.
Parameter names come from the index (definition, then declarations, then a
typedef); the text rule applies to `CIX_SYSTEM` declarations only. Arguments
are re-lexed from the snapshot's text (`call_args`). `text_params` moved to
csymidx.c as `csig_params` with its struct `CSig` (spans added), and
signature help reads it from there (goldens unchanged). Suppression: the
argument equals the name (outer parentheses and leading underscores stripped
on both sides), `/*name*/` or `/*name=*/`, the one-parameter setter,
unnamed parameters, `...`, no prototype. Refresh generalised from
`tok_stale` to the per-unit bits `views_stale` (`VIEW_TOKENS`, `VIEW_HINTS`)
and `send_refresh(bits)`; policy `CP_VIEW`. The capability is advertised
only for a client that sends `textDocument.inlayHint` and without
`initializationOptions.inlayHints: false`. `--verify-symbols` compares the
text rule with the index and prints `N param misses` (symcov carries it;
`--misses` lists them). The first version of the rule had 21 misses on the
run.sh trees and 3 on gcc.dg (`typeof` family, a UCN in a span); 0 now.

`bench/tools/clangd_diff.py` (+ `.sh`) with `bench/clangd_diff/` and three
corpus files compares hints and call hierarchy with clangd 20.1.8: 6 files,
294 differences, 0 unexpected. Found: clangd strips the leading underscores
of the argument too (B4_DESIGN 6.5 had it wrong; fixed in the code and the
golden); cereal lists callee edges for system-header macros (`assert`,
`errno`: C6) that clangd omits; a function defined by a macro invocation has
no outgoing edge and its body xcalls are attributed to file scope (G1, a
B4a gap, parked in B4_DESIGN 15).

`tests/lsp_stress.py --calls --hints` races call hierarchy and hint requests
against edits.

Tests: `tests/lsp/hints` (three sessions: with the capability, without it,
with `inlayHints` false; the whole-file hints cover every row of 6.5; a
carried answer and its two refreshes). Gates: run.sh 2074 passed (was
2068: +6); gate.sh: run 2074, san 2074 passed, 628 files, 0 findings; verify
unchanged (gcc.dg 3908 of 3910, c-c++-common 637 of 638, cpp 289 of 289);
callgrind 3.8832 G (3.8856 G, -0.06%); symcov `unindexed 0, param misses 0`
over 4718 files (1,146,276 events). TSan (`~/cereal-tsan`, removed): all 27
LSP sessions, plain and with `-fparallel=on -fparallel-chunk=1
-fparallel-threads=3`: 54 clean; `lsp_stress.py` 200 edits on zstd.c: 0
reports (238 edits sent, 0 error responses); with `--calls --hints` on zstd.c
and on `tests/lsp/ccalls/main.c`: 0 reports, 0 error responses.
Source +745/-169 in 6 files; hand-written tests ~170 lines (47 session JSON,
59 + 7 of C, 46 net in the stress script) plus 513 lines of goldens;
clangd_diff 318 lines plus a 130-line corpus.

Not done: B4c (workspace/symbol). The G1 gap above.

## Round 220: B4c, workspace/symbol over open units; B4 complete

Slice B4c of B4_DESIGN.md, as designed (Q2 open units only, Q6 no system
headers, E6 its own handler that never waits and never starts a check).
`workspaceSymbolProvider` is always advertised. `workspace/symbol` takes a
reference on the snapshot and index of every unit with an open document (under
the lock, each unit once), then builds the candidates outside it:
`SymbolInformation[]` of the document symbols' selection (now
`c_symbol_event`, shared) plus user macros, one entry per decl per unit at its
definition, `containerName` the record or enum of a field or enumerator,
deduplicated by (path, offset, name), ranked (exact, exact ignoring case,
prefix, fewer segment jumps; C before macros; shorter; name; place), the first
100. The matcher is a small DP over the name.

clangd_diff.py gained workspace/symbol (13 queries) and found that the matcher
as designed disagreed with clangd's sets: it now treats the capital before a
lower-case letter after a capital as a segment start (`str` finds `TString`)
and lets a character skip no word (`lf` no longer finds `luaC_fix`); B4_DESIGN
19. New and widened intended differences W2 (header symbols clangd's index
lacks), W3 (struct 5 and enumerator 10 kinds as well as macro 15), W1, W4.
Result: 6 files, ~1,700 differences, 0 unexpected over three runs (clangd's
answers vary until its index settles; the script now waits for the
diagnostics and repeats each query until it is stable).

`tests/lsp/wsym` (one session): two units sharing a header, the matcher cases,
containerName, system symbols absent, a closed document, a held first check
(macros only, no wait) and the 100 limit. `tests/lsp_stress.py --symbols`.
Answered in the design doc, not implemented: G1 (the root scope of a function
defined by a macro invocation is one byte wide, so its name token lies outside
it; widen the root at `csx_unit_end`, ~10 lines) and C6 (kept as intended).

Gates: run.sh 2076 passed (was 2074: +2); gate.sh: san 2076 passed, 628 files,
0 findings; verify unchanged; callgrind 3.8814 G (3.8832 G); symcov unchanged.
TSan: 56 session runs clean, `lsp_stress.py --symbols` (also with `--calls
--hints`) 0 reports, 0 error responses. Source +276/-9; hand-written tests ~87
lines plus 2,773 of goldens.

Not done: G1; B5.

## Round 221: G1 closed, workspace/symbol matcher probed against clangd

**G1.** The root scope of an external declaration spanned `tok_begin` of its
first token to `tok_end` of its last; a macro-expanded token sits at the
invocation's first character with end `exp + 1`, so `DEFINE_FN(made)` had a
one-byte root and `made`'s DEF (the argument token) lay outside it. `caller_of`
rule 1 and `cindex_calls_in` both scan events inside the root, so `made` had no
outgoing edge and its body calls were attributed to file scope. Fixed once, in
the shared place: `widen_root` (csymidx.c, ~25 lines) at the start of
`csx_unit_end` extends the root's end over every event of the unit that starts
inside it. Golden `tests/query/gmacro` (`made` and `other` call `leaf`;
`leaf`'s callers list both). clangd_diff: the two G1 lines are gone and
`classify_call` lost its G1 class.

**Digit rule probed.** Against clangd 20.1.8 (`-nostdinc`) over 95,758
query/name pairs: digits are lower case for clangd, so there is no segment
start at a letter-digit change (`u8` finds nothing in `utf8_decode`). clangd
has two matchers, FuzzyMatcher for the open file's symbols and Dex trigram
tokens plus FuzzyMatcher for included headers. `ws_match` (features.c) is
rewritten as a port of both (the old DP is deleted); `lsp_workspace_symbols`
takes the open documents' paths to choose. Open-file symbols and macros agree
exactly; header symbols differ in 3 pairs (queries with two underscores:
`L__`, `l__`, `_ct`), left as a residual. The rank 3 + jumps now counts
non-adjacent matches over the new matcher (a few results change order). New:
`tests/lsp/wsdigits` (+2 goldens), `bench/clangd_diff/wsdigits.{c,h}` with
`EXTRA` queries in clangd_diff.py. B4_DESIGN 7/15/18/19, LSP.md updated.

**C3 26 to 10 (investigated):** clangd variance, not the fix. The runs before
the fix and the final run after it print 26; two runs in between printed 10.
The 16 entries that come and go are all in libuv core.c (`uv_close`'s 14
calls through `uv__*_close` and the two `*sockopt` calls) and all "cereal
only": clangd's answer for core.c varies with how far its index has settled,
the same effect as the 29 to 8 of Round 220.

Gates: run.sh 2079 passed (+3: gmacro, wsdigits x2); gate.sh: san 2079 passed,
628 files, 0 findings; verify unchanged (gcc.dg 3908 of 3910, c-c++-common 637
of 638, cpp 289 of 289); callgrind 3.8810 G (3.8814 G); symcov unchanged
(`unindexed 0, param misses 0`, 4718 files, 1,146,276 events); clangd_diff
7 files, 1958 differences, 0 unexpected. TSan (`~/cereal-tsan`, removed): 58 session runs clean (29 sessions, plain and
parallel+cells); `lsp_stress.py --symbols --calls --hints` 200 edits on zstd.c
(239 edits sent) and on `tests/lsp/ccalls/main.c` (242): 0 reports, 0 error
responses.

Round 222: B5a, the index record format (B5_DESIGN.md sections 4, 5, 14.2,
15). User decisions recorded: A1 and Q1-Q6 accepted as recommended. New:
`cereal index-summary [--root D] [--compdb P] (--cache DIR | --dump) FILE` and
`--check FILE...` (`src/lsp/indexsum.c`); `src/c/cidxrec.{h,c}` (records and
manifests: sorted self-contained native-endian sections, a content-addressed
record per user file, one manifest per TU, a validating reader, atomic 0600
writes under `<cache>/index/<projid>/{rec,tu}`); `cindex_key` and
`cindex_each_call` in `csymidx.c`; `config_options_in` (arena variant, also
returns the `.cereal` and response files); `SrcFile.mtime_ns`;
`Options.rsp_files`. D5 prerequisites: `localtime_r` in `pp_options_finish`,
the stale "static state" comments, `bench/tools/globals.sh` with a 31-symbol
allowlist as gate stage 0. Tests: `tests/index` (dump goldens), a determinism
check and a corruption check (truncated, flipped, version, build id) in
run.sh section 22. Measured 5.5: 30-53 MB per 1000 TUs of bytes (46 MB on
cereal's own src), under the 80 MB stop rule; records byte-identical across
runs on lua54, libuv and cereal src. Deviations are listed in B5_DESIGN 15.

Gates: run.sh 2103 passed (2079 before); gate.sh: san 2103 passed, 628 files, 0
findings; verify unchanged (gcc.dg 3908 of 3910, c-c++-common 637 of 638,
cpp 289 of 289); callgrind 3.8797 G (3.8810 G); symcov unchanged (4718 files,
1,146,276 events, unindexed 0, param misses 0). TSan not run (no threading
touched).

Round 223: B5 sizes at scale (measure only, no src change).
`bench/tools/genproj.py OUT [TUS] [SEED]` generates a 1,000-TU C project
(50 shared headers, 4 `-D` sets). `index-summary --cache` over all of it,
8 processes: 10.5 s, 2,200 files, 17.25 MB (23.8 MB in 4 KiB blocks), under
the 80 MB stop rule; records and the second run are byte-identical.
Manifests are 66% of the cache (84% of that is system-header FILS entries),
over the 25% interning trigger; directory-prefix interning would save 3.55
MB (20.6% of the cache) and leave manifests at 57%. Numbers in B5_DESIGN 5.5.

Round 224: B5 system-header sharing spike (measure and throwaway prototype, no
src change in this repo). Analytic pass over the real manifests of the
generated project and lua54/libuv/cereal-src corpora: sharing the system-header
part of the manifests saves 50-55% of the generated cache (exact-set 53.8%,
per-header pool 49.8%, chunked sets 54.6%, cache-wide table 52.5-54.4%,
directory interning only 20.6%). Chosen: content-defined chunks of the sorted
system list (about 16 files, cut where hash(path) % 16 = 0), one immutable
content-addressed file each, manifest holding digests. Prototype (WSL copy,
about 85 lines of format and 35 of dump): 17.25 MB to 7.86 MB (-54.4%), 23.8 MB
to 14.9 MB in blocks, +42 files, dump output unchanged, records and chunks
byte-identical across fresh caches, a changed or touched system header seen by
a stat check. Hash-less system entries save 0.9 MB unshared and nothing shared;
rejected. Numbers and the proposed 5.2/6/7 edits in B5_DESIGN 5.5; not adopted.

Round 225: B5b-1 and B5b-2, the project index store and scheduler
(B5_DESIGN.md sections 6-9, 13). B5b-1: format 2 with chunked system headers,
the store (flock writer, GEN, GC, eviction, memory-only root), `PState`
(staleness NEW/FILE/MISS/LOST, racy-stat rule), the single job function
`index_unit`, `index-summary --stale/--entry/--budget-mb/--check`. B5b-2
(`src/lsp/project.[ch]`): one scheduler thread owns the state and takes events
from a mailbox; jobs run on an ipool in process or as a child
(`index-summary --entry`, posix_spawn, deadline, SIGKILL); order by open
document, then recently stale, then compile database order; edits cancel jobs
and hold new ones for `quietMs`; failure backoff in memory; GEN-polling
readers with lock takeover; compile DB reload with retired configs kept;
workDoneProgress; settings from `initializationOptions.index` and
`CEREAL_INDEX_*`. Test-only requests `cereal/indexStats|indexWait|indexHold`;
`tests/proj_test.py` (8 scenarios, run.sh 22c). The child's `RLIMIT_AS` had to
include the 4 GiB `SrcMgr` reservation. Deviations in B5_DESIGN 15.
Gates: run.sh 2137 passed; gate.sh: san 2137 passed, 628 files, 0 findings;
verify unchanged; callgrind 3.8857 G (3.8797 G); globals 32 allowed (+1
`lsp/project P`); symcov unchanged (4718 files, 1,146,276 events); clangd_diff 7
files, 2264 differences, 0 unexpected. TSan and builder latency: B5_DESIGN 15.

Round 226: B5b-3, the name list and project-wide workspace/symbol
(B5_DESIGN.md 10.1-10.2, 11.1). `src/lsp/projidx.[ch]`: a ref-counted,
RwLock-guarded `ProjIndex` of per-record name lists, refreshed by the
scheduler wherever a manifest is reloaded. `lsp_workspace_symbols` now
searches the open units and the project through one candidate path and one
matcher; project names are placed at query time by file hash (as recorded,
mapped through the edit for an open document, else dropped with a sweep hint),
and a declaration is dropped for its definition. New `off_to_pos`,
`lockRetrySeconds`, `proj_test.py` scenarios `wsym` and `takeover`. 1,001-TU
project: index 8.5 s, restart check 0.09 s, workspace/symbol p95 4 ms,
`ProjIndex` 1.2 MB, builder p50 within 5% with indexing running (noisy host).
Stop rules did not fire. B5b total about 5,660 lines (4,800 waived).
Gates: run.sh 2142 passed; san 2142, 628 files, 0 findings; callgrind 3.8848
G; globals 32 allowed; symcov unchanged; clangd_diff 7 files, 2186
differences, 0 unexpected; TSan 0 reports. Numbers and deviations 16-20 in
B5_DESIGN 15.

Round 227: B5b stop rules re-measured (no src change) on a quieter host and a
real 1,804-TU project (libuv, Lua 5.1-5.5, LuaJIT, zstd, cereal; 213,903
names): index 23 s, `ProjIndex` 6.1 MB, RssAnon 68 MB, restart check 0.15 s,
builder p50 within 5% with indexing on; workspace/symbol p95 112 ms on a 22-query
mix (rule 3 fires on `ZSTD_*` queries with thousands of candidates, p50 19 ms);
the scheduler spins a core once `sweepSeconds` expires during a long run
(confirmed, no visible latency effect). B5_DESIGN 15.

Round 228: the B5b review, "fix all" (B5_DESIGN.md 15.1). All 15 findings were
real. Scheduler: the sweep deadline no longer spins a core; one per-unit
route and next-attempt record; `finish()` keyed by job identity; a miss right
after a successful job backs off; `indexWait` is quiescent for the reader or
writer role. Records: `#include_next` misses carry the start directory; one
loadable-file predicate; a corrupt or missing record is rejected online and
the unit re-indexed (one path); config files are snapshotted before the
options are built. Store: GC every 100 manifest writes and eviction every 10
minutes, not per burst; one `walk()`; the `USE` file is held shared by every
opener and exclusive by the evictor; project id includes the build id; index
writes skip fsync. Readers reload only changed manifests. `LspConfig` is
ref-counted (no retired list; an equal reload is dropped; a partial compdb
keeps manifests). Children die with their parent (PDEATHSIG, alarm); private
roots are pid-named and swept. workspace/symbol: dedupe once, after
placement; `ws_drop_decls` over a table; rule 3 fixed at its root (the
position lookup rescanned the file for each result; now a per-file line
table): 1,770-TU harness (`bench/tools/wsym_bench.py`) p95 85.5 to 17.2 ms.
Deleted `lsp_config`, `pstate_drop_manifest`, `store_bytes`, `is_file`,
`pstate_check_records`, `off_to_pos`, three `u64_cmp` copies and more. Not
applied: collapsing "edits first" to one timer (cannot keep the guarantee).
Gates: run.sh 2153 passed; san 2153, 628 files, 0 findings; verify unchanged;
callgrind 3.8863 G; globals 32 (30 after Round 233); symcov unchanged; clangd_diff 0 unexpected;
TSan (58 LSP session runs plain and -fparallel with indexing on, lsp_stress --symbols --calls --hints 200 edits (236 sent) with indexing active, 16 proj_test scenarios): 0 reports.

Round 229: B5c, references, call hierarchy and definition across TUs
(B5_DESIGN.md 10.3-10.4, 11.2-11.4, 14.6, 15.2). New `src/lsp/projq.[ch]`: one
position source per query (`PqFiles`: snapshot text, else the open document,
else disk, with B5b's per-file `LineTable`), the query-time mapping of a record
place into the client's text, a place type with sort and dedupe (the open unit
wins), and the project's entries by key; `projidx` gained a key map (`pi_find`).
references, documentHighlight, typeDefinition, definition, declaration,
prepareCallHierarchy and incoming/outgoingCalls use it (item data has `key`);
workspace/symbol's private copy of the placement code is deleted. Open-unit
reference results are now sorted by (path, offset) (csym golden regenerated).
Not built: the 64-file `TextCache` (files are read once per query; p95 3 ms
without it). `clangd_diff.py` has a project mode (lua54, libuv; clangd's own
index is not deterministic, so the union of three runs): 0 unexplained misses in
3,425 clangd results; the one large difference is that clangd lists invocations
of macros whose body uses a function (13%; B5_DESIGN Q7). `wsym_bench.py` times
references, definition and incomingCalls: on the 1,770-TU project references p95
3.1 ms (stop rule 300 ms), workspace/symbol p95 17.6 ms. Stop rules did not
fire. `lsp_stress.py --refs`; `proj_test.py` scenario `xref`.
Gates: run.sh 2157 passed; san 2157, 628 files, 0 findings; callgrind 3.8886 G
(3.8863 G); globals 32 allowed; symcov unchanged; clangd_diff 7 files, 0
unexpected; TSan (58 session runs, `lsp_stress --symbols --calls --hints --refs`
200 edits (239 sent) with indexing active, 17 proj_test scenarios): 0 reports.

Round 230: B5d, rename across TUs (B5_DESIGN.md 11.5, 15.3; Q7 resolved: keep,
parked). `crename` takes one edit list per file (given by the project or planned
from the unit) and checks a unit over an overlay of every edited file it reads;
`cereal query rename=` and the LSP share `c_rename`. One predicate,
`cindex_rename_blocker`, refuses a use in a `#define` body or at a macro
invocation (R4/R7), for the open unit's events and every project record event,
because macro invocation sites stay unlisted (Q7, an intended difference from
clangd, P2). R1 (`pq_ambiguous`), R3 (unplaced entries), R2/R5 (`project_scope`
on the scheduler) and R6 (the per-unit gate on the index pool, submissions
paused) complete the refusals; a project rename runs on a worker thread and
replies `documentChanges` or `changes`; a file that changes meanwhile gives
ContentModified. `bench/tools/rename_libuv.py` renames 441 external libuv
functions: 382 edits, 54 refused for a use in an inactive `#if` group (A3.9), 5
for a macro body, 0 other (12.2% if A3.9 is counted as false; stop rule not
fired on the design reading, reported). Renamed decls: only the asked decl is
renamed, a second decl at an edit is refused (golden `rename.expected` kept).
`proj_test.py` scenario `rename`; `lsp_stress.py --rename`.
Gates: run.sh 2158 passed; san 2158, 628 files, 0 findings; callgrind 3.8845 G;
globals 32 allowed; symcov unchanged; wsym_bench workspace/symbol p95 21.9 ms;
clangd_diff project mode 0 unexplained misses in 3,400 clangd results; TSan (58
session runs, `lsp_stress --symbols --calls --refs --rename` 200 edits on a
40-TU project, 18 proj_test scenarios): 0 reports.

Round 231: B5d follow-ups (user decisions). Renames touching inactive `#if`
groups stay refused (stop-rule measure: the design reading, 0% of 441 libuv
functions; 12.2% strict). R2 stays as written, but its message now names each
blocking unit with its state ("only partly indexed: N errors in the check") and
suggests `cereal.index.paths`; `proj_test.py rename` gained an erroneous-unit
case. ROADMAP Track D has a new item, "macro-aware project index and
refactoring", collecting the parked pieces (macro invocation sites, the
uninvoked-macro gap, inactive groups, macro rename policy; shared prerequisite a
record format bump); B5_DESIGN 18 lists them.

Round 232: review fixes for B5c/B5d (B5_DESIGN 15.4). One rename path: every
C rename runs in `project_rename` (locals planned from the unit's events and
gated on the open unit alone); deleted `lsp_check_rename`, `RenameJob`,
`rename_run`, the `wide` parameter of `cindex_rename_plan` and the unit-only
write path. Fixed: doubled call `fromRanges` (F1), external link-time clashes
and externals without a definition (R8, R9; F2), unity builds (F3), the R1
cost of prepareRename (F4), opaque tag keys across units (tag anchor is the
first declaration; `CREC_FORMAT` 4; F5), the outgoing call item at the
prototype (F6), caps (20000 entries, R10), a hash table for `pq_file`, and
copy-out-then-place under the read lock (F8), the pause now spanning plan to
write, the fault hook replaced by `cereal/holdChecks` `gate`, the `Run` leak
on a fatal in the gate, `pq_where` on the line table (F9). F7 discarded:
macro-formed declarations never reach the records (parked, B5_DESIGN 18).
Unifications: `EV_CALL` scheduler events, `rpc_begin/end/log/progress`,
`pq_loc_cmp/order`; `DocSet`/`WsDocText` kept. `lsp_stress.py` now roots at the
compile database, waits for the index and counts renames answered with edits.
Gates: run.sh 2158 passed; san 2158, 0 findings; callgrind 3.8830 G; globals
32; symcov unchanged; clangd_diff 0 unexplained; wsym_bench p95 21.2 ms;
rename_libuv 441 renames, 0.0% false (design), 3m59s; TSan 0 reports (stress:
6 project renames with edits). One unreproduced `proj_test` rename failure in
the first TSan pass (0 of 11 reruns).

Round 233: the unreproduced `proj_test` rename failure. Found under TSan with
load (a slow check): a request waits 1.5 s for the open unit's check, so
`proj_test` raced it (`rename` refused as "not ready; retry", `xref` saw an
unmapped edit); under load 3 of 8 TSan batches failed, plain runs never. Test
fix, not a server change: `didOpen` waits (`cereal/waitIdle`), the `xref` edit
holds the check (`cereal/holdChecks`), and a new `rename` case pins the bounded
wait (refused with "retry", then goes through). A real data race fell out of the
same runs: `pure_first_char` (ccall.c) filled a lazy table from several checking
threads; now a stateless binary search over the sorted `cbuiltin_pure`, and the
two `c/ccall` globals.allow rows are gone (32 -> 30 allowed globals).
Gates: run.sh 2158 passed; globals 30; proj_test 18/18 plain. The TSan
re-run: 8 of 8 plain-load `proj_test` runs under TSan passed (20 min, one run at a time), 0 reports. The hunt itself ran about 5 hours at about 12 busy threads
and starved other sessions; stopped by the user. Long loaded runs: bound the
time and threads up front.

Round 234: `$/cancelRequest`. The protocol thread answers every request before it
reads the next message, so only deferred requests can be cancelled: a
cross-TU rename (its own thread, now listed in `S.active`) and a queued code
action. Each `DeferredReq` has an atomic `cancel`; the notification matches the
id's source text under `S.m`. A cancelled request answers -32800: a queued one
before it starts, a rename by skipping the gate's units not yet begun and by
checking the flag before the edits are written (nothing is written). Not
cancellable: a fix-all already running on the builder, and one unit's check once
started. Removal from `S.active` happens before the free, under the same lock.
`proj_test.py rename` has the case (cancel after the checks, a stale id, the
same rename succeeds). Gates: run.sh 2158 passed; globals 30; proj_test 18/18
plain; TSan: 9 of 9 `proj_test` runs (20 min, one at a time), 0 reports.

Round 235: `lsp_stress.py --rename` now follows half its renames with a
`$/cancelRequest` (and sometimes a stale id) and counts RequestCancelled. Under
TSan (40-TU generated project, 200 edits, all request flags, indexing on) it
found a data race that predates Round 234: `index_find_file` normalized its
argument into `ix->arena`, the snapshot's shared arena, so a rename worker and
the protocol thread (completion, definition) raced on that arena. Fixed at the
root: the lookup normalizes into a local arena and only reads the index. After
the fix 2 seeds ran with 0 reports; run.sh 2158 passed, proj_test 18/18. Noted:
the stress logged 0 cancelled renames (most of its renames are refused before
the worker starts, or finish first), so the cancel path is covered by
`proj_test` only.

Round 236: gate at dbcffcc (Rounds 233 to 235): globals 30, run.sh 2158 passed,
san 2158 and 0 findings (628 files), verify gcc.dg 3908/3910, c-c++-common
637/638, cpp 289/289, callgrind 3.8830 G. Idle memory with the project index
(the parked idle-trim item, Round 203): a generated 1,000-TU project, 4 index
threads, all jobs in process: after the index is built the server holds RSS 52 MB
(anon 40, file 11), peak 54 MB, unchanged after 2 s idle and after
`workspace/symbol` queries; `MALLOC_TRIM_THRESHOLD_=0` gives 47 MB (-10%). The
in-process jobs free their memory, so the index adds no idle cost worth a trim;
the Round 203 finding (only huge generated files) stands. Not measured: the
1,804-TU real corpus (ProjIndex 6.1 MB there), peak under `renameMaxTUs` checks.

Round 237: the 3-pair Dex residual of Round 221, closed. Probed against clangd
20.1.8 with a small header (queries on `LUA_INTEGER_FMT`, `SSL_CTX_new`, `a_b_c`,
`my_long_name_here`, `__builtin_x`, ...): clangd cuts a query into trigrams over
its non-separator characters (the old port used raw windows of three, so `L__`
and `_ct` found nothing). `ws_dex` now filters the separators first: three or
more characters are windows, one or two are the unigram or bigram (`L__` is
`l`, `_ct` is `ct`, `a__` is `a`), none lets every name through. This also fixes
`a__` (a_b_c), which the corpus pairs did not contain. Not reproduced: a query
that starts with a separator and has one more character (`_b_`, `__b`): clangd
answers `__builtin_x` only, cereal adds `Foo_Bar_Baz` and `a_b_c` (a superset;
the index apparently keys that query on the separator too). `tests/lsp/wsdigits`
pins `L__` and `_ct`. Gates: run.sh 2158; clangd_diff 0 unexpected; wsym_bench
p95 23.2 ms (was 21.2); the open-file matcher agreed on all probed queries.

Round 238: survey (no code), configurations per `#if` group (ROADMAP Track D).
Existing pieces: the skeleton pass (every directive of a file, active or not),
`cond.c` (each condition as a formula over `defined(X)`, integer and opaque
atoms, decided by enumeration; no model of mutually exclusive atoms), `Options`
(std year, -D/-U, fingerprint; one per compile-database entry), the B5 manifests
(E5: differing header records are kept as variants). Counted over libuv, Lua
(7 trees), zstd and cereal's src (7,231 `#if`-family directives): 1,208 (17%)
test a predefined or platform atom, 834 of them only such atoms. By axis: OS
(`_WIN32` 391, `__APPLE__` 122, `__linux__` 97, the BSDs, `_AIX`, `__sun`) is
the biggest, then compiler family (`__GNUC__` 206, `_MSC_VER` 111, `__clang__`
67), `__cplusplus` 101, the C standard only 46 (`__STDC_VERSION__`) and 4
(`__STRICT_ANSI__`), architecture/ABI about 80. So the axis that matters here is
OS and compiler, not the standard. Limits found: (1) atoms of one family are
mutually exclusive (one OS, one compiler) and `cond.c` treats them as
independent; (2) a non-host OS or compiler needs its own system headers and
predefines, which a Linux host lacks, so such a group parses only with missing
includes tolerated (lenient mode) or with a supplied sysroot; (3) architecture
and ABI atoms imply type sizes, and the checker's layout model is x86-64 SysV
only, so a target is more than macros; (4) a verification view needs an
annotation channel (comments lexed) as well as a macro.

Round 239: design doc `docs/CONFIG_DESIGN.md` (no code): presence conditions
over initial atoms (I, threshold, HAS, OPAQUE), family DAG with cross-axis
constraints, macro definitions as a fold per inclusion instance (concrete and
symbolic modes, exactness flags), ROBDD solver with block-contiguous order and
a deviated-flag cost DP, per-unit PC families, Local PCs as a query-independent
predicate DAG in `CREC_FORMAT` 5, virtual compile entries from a pure `cover`,
realizability classes, rename through inactive groups only when every
occurrence is exact, satisfiable and active in a passing `REALIZED` unit.
Seven adversarial Sonnet review rounds (section 12): round 1 six blocking
(Reach in stored PCs, circular fold, initial state, false exclusive families,
unreachable stop rule, guard rule), round 2 three (position of evaluation,
concrete versus symbolic, rename on uncovered groups), rounds 3 to 7 no
blocking except one identity omission (round 6). Next: the S0 spikes (OS versus
feature axis of the 54 refusals, BDD size and fold cost), before any code.

Round 240: S0a (no code in `src/`): classified libuv's 54 A3.9 refusals by
atom axis, 20 OS only, 26 OS plus guard or derived, 8 with no OS atom, so at
least 46 of 54 (85%) sit on the OS axis; recorded in `docs/CONFIG_DESIGN.md`
section 13.

Round 241: `docs/CONFIG_DESIGN.md` 3.8, preconditions as the general explanation
(user: "this group needs these preconditions to be active"). Requirements typed
by the Configuration field that changes them (config, language, target,
environment, opaque), two-plus phases (overlay, then environment, then target
relaxation), the language field a one-hot block of std rows, realizability
derived from the unmet kinds, S4 re-scoped from rename edits to typed lists.
Three more Sonnet review rounds (8 to 10; 2, 1 and 1 blocking), stopped by
judgment; S0b must exercise every phase.

Round 242: S0e (throwaway script): every inactive `#if` group per refused libuv
function, typed by atom kind: 46 of 54 target-kind, 1 config-like, 8 unresolved
by a textual evaluator; groups per function median 1, max 7. The S4(c)
denominator is therefore at most 9 of 54, so rename-edit gains are small on
this corpus and typed precondition lists are the S4 deliverable.

Round 243: S0b/S0c v1 (throwaway Python ROBDD plus symbolic fold over the quoted
include closure): libuv 62 units, 30,839 groups, max 44 nodes per group, 2.7 s;
Lua 5.4 max 79 nodes, 4.6 s; zstd max 126. Size and time are not the risk;
caveats were overstated exactness (system-header macros were free atoms), no
threshold encoding, families or cost DP. S4 wording changed to typed lists.

Round 244: S0b/S0c v2 (throwaway Python; results in `docs/CONFIG_DESIGN.md`
section 13): system headers followed, threshold atoms, OS and compiler family
constraints, block-contiguous order, phased cost DP. Two blow-ups found and
mitigated: following system headers deeper than 1 level (unlimited overflowed
3M nodes after about 1,000 files) and conjoining the constraints into every
group (max nodes per group 504 to 166 on libuv, 10,113 to 179 on Lua once applied
per group at query time). Honest exactness 92% libuv, 78% Lua, 76% zstd;
17% to 26% of groups are unsatisfiable under the constraints; 67% to 83% of
inactive groups have an overlay cube. Decision 1 (BDD) stands with depth-1
system headers and per-group constraints; ranks 2 to 3 and `cover` untested.

Round 245: S0b/S0c v2c (throwaway Python; `docs/CONFIG_DESIGN.md` section 13):
ranks 2 to 3 and `cover` prototyped (rank-1 equals the DP cost once the variable
order is complete before the final fold); fold profile (38% of the table is
live; `stddef.h` re-inclusions dominate zstd); system headers folded
host-concretely at any depth removes the depth cap and the size problem
(libuv max 119 nodes per group, exact 99%; Lua 86%; zstd 85%, fold 1.6 s).
`cover`'s 4-overlay budget covers 91% of libuv's phase-1 groups but 46% of Lua's.
Design 3.4 and decision 1 updated.

Round 246: decisions from the v2c data (`docs/CONFIG_DESIGN.md` section 13):
`cover` overlay budget per entry 4 to 8 (configurable, uncovered share
reported; coverage curve libuv 100% / Lua 69% / zstd 100% at 8), and exactness
restated as an assumption set per unit (skipped computed or unresolved includes
contribute `not cur`) instead of per-group inexactness, since 83% of Lua's
exact groups follow a computed include.

Round 247: the "other" inexact bucket broken down (`docs/CONFIG_DESIGN.md`
section 13): unary minus, character constants, macro-alias comparisons and
two-atom comparisons, all fixable by three evaluator rules; opaque atoms are
named per directive.

Round 249: S1 Slice 0 (`docs/S1_PLAN.md`, draft 5): `tests/lint/cond_limits.c`
pins today's behavior at every `cond.c` limit (arithmetic, variable-free, ternary,
`#elifdef`, `__has_include`, 17 variables, 70 nesting, 34 branches, 2^18, wide
domain); `bench/bench.py --lint` times lint (baseline 100.7 ms total in
`bench/BASELINE.md`). Full suite 2165 passed. No library code changed.

Round 250: S1a (`docs/S1_PLAN.md`): `src/analysis/bdd.[ch]` (ROBDD, node cap, `cereal selftest bdd`)
and `cond.c` rewritten on it; the enumeration solver and its limits (16 variables, 63
conjuncts, 64 frames, 2^18, `dom[24]`, 32 priors) are deleted. Value atoms are
thresholds `GE(X,c)` with monotonic and undefined-is-0 clauses added per query;
arithmetic and two-macro comparisons are opaque atoms keyed by text. Differential
lint against the old binary on tests, cereal sources and 400 corpus files: only
`cond_limits.c` (five dead branches the limits hid, now expected) and six corpus
diagnostics removed, all false positives from arithmetic over unknown macros.
Suite 2169 passed; lint timing 87.5 ms (baseline 100.7).

Round 251: S1b skeleton kinds (`docs/S1_PLAN.md`): `#elifdef`/`#elifndef` are chain
branches in the skeleton and lint (they were `SK_OTHER`, so ignored), `#pragma once` has
its own kind, `include_next` is a flag on `SK_INCLUDE`. Resolved include edges parked
to S1c (no consumer yet). Suite 2169 passed.

Round 252: S1c step 0 (`docs/S1_PLAN.md`): the condition evaluator moves out of `cond.c` into
`src/analysis/pcexpr.[ch]` with an environment interface (is X defined now; which values can it
hold under which conditions), so the S1c fold and lint share one evaluator; the default environment
is the start of the unit and lint is unchanged. Value comparison is now over the environment's
entries plus the undefined-is-0 case (the prototype's model), so only the monotonic threshold
clause remains in `pc_sat`. `pp_resolve_include` (the include search from an explicit includer)
shares `find_include` with the preprocessor, and `PP.seq_main` records the macro version at which
the main file starts (the initial valuation). Suite 2172 passed.

Round 253: S1c (`docs/S1_PLAN.md`): `src/analysis/pc.[ch]`, the symbolic fold of the include
closure: one BDD per `#if`/`#elif`/`#else` group over the initial state of the macros, with
define/undef tracked per macro (`PcMacro`), `#pragma once` per file, and computed/unresolved
includes skipped, recorded and marked as taint on later groups. Atoms of one macro (or opaque
text) share a block of 32 consecutive BDD levels, blocks in order of first use, so no first pass
is needed. Macro values are entries (`known` number, or `expr` evaluated at the use point, depth
cap 8, or an opaque keyed atom). System headers follow the real run: a PP listener
(`pc_trace_attach`) records the taken group instances (key: hash of the include chain mixed with
the group's `kw_loc`), so `-fparallel=off` is required for the hook. Oracle: a group's value
under the host valuation (`pc_host_atom`, macros live at `seq_main`) against whether the real
preprocessor took it, counted by [exact][tainted]. Hidden hook `CEREAL_PCDUMP=<words>`
(`size`, `oracle`, `skipped`, `time`) prints `pc:` lines on stderr; `bench/pc_oracle.py`
aggregates per corpus family. Fixtures `tests/pc/*.c` with `.pc` goldens (run.sh section 24).
Corpus results: exact groups agree 100% (libuv 18426 groups; lua52-55, lupa, uvloop, zstd);
exactness 87-100% (libuv 99, lua52 97, lua53 89, lua54 92, lua55 88, lupa 98, uvloop 100, zstd
87), so the S0 bound of 90% is narrowly missed on lua53, lua55 and zstd (opaque: `__has_*`,
function-like macro calls, arithmetic on macros; opaque atoms are host-false, 45-100% agree).
Fold time 8-24 ms mean, 41 ms max (bound 1 s); largest group BDD 160 nodes (bound 20,000); no
unit over the cap. Suite 2184 passed.

Round 254: S1c exactness survey (`docs/S1_PLAN.md`). Found and fixed a parser bug in
`pcexpr.c`: a function-like call (`__has_attribute(x)`, `FOO(1)`) ended the call scan on its
first token, so every `#if` containing a call failed to parse and the whole directive became
one opaque text atom (`calls.c` fixture). Calls are now single opaque leaves and the rest of the
condition stays exact; lint gains the dead `#elif __has_include(..)` repeat (`cond_limits.c`),
no new diagnostics on the corpora. Exactness percentages are unchanged because a leaf still counts
as opaque. What is left (zstd: 146 opaque groups, 94 with feature tests, 36 two-macro
comparisons, 15 arithmetic) is feature tests and compiler-version arithmetic
(`__GNUC__*100+__GNUC_MINOR__ >= 403`): functions of the compiler, i.e. the family table of
S1d, not of the initial macro thresholds. Suite 2182 passed.

Round 255: S1c function-like macro calls (`docs/S1_PLAN.md`). A `#define NAME(a, b) body`
without `#`, `##` or variadics is kept as `PcFn` (parameters, body tokens); a call in an `#if` is
instantiated by textual argument substitution (C's own order, no added parentheses), re-parsed and
evaluated where it is used (depth cap 8). One definition: its value; several (or an outside
definition): whether it is nonzero; otherwise one opaque atom keyed by the call text. A call whose
macro the fold never saw defined is now `V_OPQ` (its truth is the same atom; a comparison with it
is opaque instead of treating the call as 0/1); only `__has_*` other than the dated
`c_attribute`/`cpp_attribute` stay boolean. Fixture `tests/pc/fnmacro.c`. Corpus exactness is
unchanged (the survey below says why); lint output on 400 corpus files is identical. Two lua54
groups (`FIGS`, via `l_floatatt(n) (DBL_##n)`) turned from accidentally exact to opaque. Suite
2183 passed.
Survey (no change made): counting feature tests and compiler/limits arithmetic (`__has_*`,
`__GNUC__*100+__GNUC_MINOR__`, `INT_MAX`, ...) as family atoms would give exactness lua53 96.8,
lua54 92.3, lua55 90.8, zstd 95.8, all others above 97 (from 88.8, 91.7, 88.5, 87.2). The
remaining opaque atoms are mostly enumerations of the unit's own constants, e.g.
`LUA_INT_TYPE == LUA_INT_LONG` (about 3000 atoms) and `LUAI_ISNINT(x)@..` bodies (3542): both
sides are macros whose values the fold knows, so a macro-vs-macro comparison can be expanded over
the entries of the known side. That, not a gate change, is the next lever.

Round 256: S1c value layer rewritten as guarded cases (`src/analysis/pcexpr.c`). An integer
expression is a list of cases, a value under a BDD condition (disjoint, covering): a number, the
initial value of a macro plus a number (`N_INIT`, the existing `GE` thresholds shifted), or an
opaque text keyed by what it computes (`num_desc`). Every operator is a product over the cases of
its operands (`pair_op`); equal values merge; more than 16 cases collapse to one opaque text.
Macro entries (known, expression body, function-like result, initial value, undefined-is-0), calls
with several definitions, `?:` and `defined` are all cases. Deleted: `V_CONST/V_BOOL/V_VAL/V_OPQ`,
`num_cmp`, `body_cmp`, `cmp_bool`, `binop`, and the 0/1-only combination in `ev_call`. Gains:
macro against macro (`LUA_INT_TYPE == LUA_INT_LONG`), arithmetic on known values (`LEVEL > 7`
now exact), `X+k` against a constant. Still opaque: two unrelated initial values compared or
multiplied (`EXT == OTHER`, `__GNUC__*100+__GNUC_MINOR__`; S1d's family values make those
constants), `##`/`#`/variadic macros. Opaque keys changed form (`(b25 v:A v:B)`; arithmetic is
`(bOP x y)`), so `tests/pc/exprdef.pc` was regenerated (one group now exact, correct) and
`tests/pc/cases.c` added. Oracle: exact groups agree 100% on all corpora; exactness libuv 100,
lua52 99, lua53 91, lua54 94, lua55 91, lupa 99, uvloop 100, zstd 89 (was 99/97/89/92/88/98/100/87);
fold 10-27 ms mean, 42 ms max; largest group BDD 160 nodes. Lint output on 400 corpus files
identical. Suite 2184 passed.

Round 257: S1d-1, the query layer (`src/analysis/pcq.[ch]`), on a `PcUnit` with no family table yet.
`PcValuation` gives each initial atom a value or "free" (no pin). `pc_holds`, `pc_exact`,
`pc_preconditions` (cost = macros whose assignment differs from V, a DP over the BDD with a
per-block deviated flag; opaque atoms never cost, free atoms never deviate; best-first paths
expanded to prime cubes, K up to 3), `pc_cube_render` (requirements typed config/opaque, status
met/unmet/unknown) and `pc_diff` (macros of the support that decide a flip between two
valuations). `pc_sat` applies the monotone clauses via `pc_monotone`. `cereal selftest pcq`: 400
random rounds against brute force (supports up to 14 variables) for holds, cost, ordering and diff.
`CEREAL_PCDUMP=why` prints up to three cheapest preconditions under each group the host leaves off
(`tests/pc/why.c`). Not yet: family table, EMPTY/HAS atoms, target/environment/language kinds,
phases 2a/2b, `via`. Suite 2189 passed.

Round 258: host values for opaque atoms, and `##`/variadic function-like macros (S1c leftovers).
(1) `pc_key_eval` (`pcexpr.c`) evaluates an opaque atom's key (`v:X`, `d:X`, numbers,
`(bOP x y)`, `(uOP x)`, `(? a b c)`) under the host: `v:X` is the host macro's number or 0 when
undefined. A key naming a call (`c:...`, `__has_include(...)`) or a non-numeric macro is "unknown":
the oracle prints `oracle=unknown`, counts it as `undecided` and no longer calls it BAD (before,
every opaque atom was host-false, so opaque agreement was partly luck). `pc_oracle.py` reports the
undecided count. Opaque groups now really agree: 27/27 libuv, 66/66 lua52, 1124/1124 lua53 (+3
unknown), 896/899 lua54, 1329/1331 lua55 (the 5 BAD are all after a skipped include); zstd 27/27
(+103 unknown, all `__has_*`). (2) `PcFn` gains `va` (`...`/`__VA_ARGS__`, the rest of the call as
one span) and `call_inst` pastes `a ## b` into one identifier (identifier with identifier or
number; placemarker for an empty argument); `#` stays opaque. `tests/pc/paste.c` (+ `.flags` for the
host-valued opaque case). lua exactness unchanged: the remaining `FIGS` opacity is the skipped
`luaconf.h` (taint), not `##`. Suite 2190 passed.

Round 259: S1d-2 family table (first cut), the rank-1 gate, conditional taint, unsigned `#if` arithmetic.
(1) `pcfam.[ch]`: two one-hot axes of leaves, OS (linux, android, windows, apple, other) and compiler
(gcc, clang, msvc, clang-cl, other), as atoms `PA_LEAF` of one block per axis. Per macro (`__linux__`,
`__unix__`, `__ELF__`, `__ANDROID__`, `_WIN32`, `__APPLE__`, `__MACH__`, `__GNUC__`, `__clang__`,
`_MSC_VER`) the leaves that must define it and those that must not; the others leave it free, so
the table says only what is certain and "other" stays a residual. MSVC and clang-cl require the Windows leaf.
`pc_constrain` (monotone thresholds + `pc_family`) replaces `pc_monotone` in `sat` and the queries; the
clauses are added per query over the support. The host leaf is read from the host's predefined
macros. Costs: a leaf block is a `target` change (weight 1000), a table macro follows its leaf
(weight 0, hidden from the list when a leaf must change), everything else weighs 1; `PcCube` gains
`target`, requirements `PQ_TARGET`; leaf literals render as `os = windows`. Not yet: integer-valued
members and concrete versions per leaf (so zstd's `__GNUC__*100+__GNUC_MINOR__` and `__has_*` stay
opaque; exactness is unchanged), `HAS`/`EMPTY`, language rows, overlay-flippable bit, phases 2a/2b
as separate passes (the weights give phase 1 vs 2b; `environment` has no atoms yet), `via`,
`enable`/`cover`.
(2) Rank-1 gate: `pc_check_rank1` / `pc_cost_brute` enumerate every assignment of the constrained
support (up to 20 variables) and compare the least weighted cost with the DP's first cube;
`selftest pcq` uses the same function (now with leaves, 16 variables, at least 100 compared and 20 with
leaves), `CEREAL_PCDUMP=check` runs it on every satisfiable group, `pc_oracle.py` aggregates. Corpora:
0 disagreements in about 56,000 groups checked (libuv 12196, lua52 6870, lua53 9969, lua54 9673, lua55
8660, lupa 2278, uvloop 5994, zstd 747); 5,000 groups too large to enumerate.
(3) Taint is a condition, not a flag: a skipped include taints the groups after it under the
condition at which it is reached (`PcGroup.taint` is a BDD; the oracle evaluates it under the host).
The only skipped include in the lua units is `#include LUA_USER_H` under `#if defined(LUA_USER_H)`, so
the lua groups are now untainted under the host. This exposed five real disagreements, all the
same bug: `#if` arithmetic ignored unsignedness (`__LONG_MAX__ * 2UL + 1UL`, `>> 31`) and a shift
by 63 was treated as 0. `Num`/`PcNode` carry an unsigned bit (suffix `u`, or a value above
INTMAX_MAX), `fold` follows the usual arithmetic conversions (comparison gives signed 0/1; a shift takes
its left operand's type) and the key evaluator does too. Oracle: 100% agreement on exact groups and 100%
on decidable opaque groups on every corpus (3-103 per corpus are host-unknown, the `__has_*` calls).
Fixtures `family`, `unsigned`; suite 2195 passed.

Round 260: S1d-3 integer-valued family members, `HAS` atoms, the data-model axis.
(1) `PA_HAS`: a feature-test call (`__has_attribute(x)`, `__has_builtin`, `__has_include`, ...) is an
exact atom keyed by the call's canonical text. It is pinned like `PA_TEXT` (`pc_atom_pinned`: queries
never flip it, the DP gives it cost 0) but is not counted as opaque; the requirement kind is `PQ_ENV`
("env "). The host oracle has no value for it (`oracle=unknown`, counted as `exact-undecided`). The
dated `__has_c_attribute` / `__has_cpp_attribute` values are known nonzero, not 1. The table says
only `__has_*` is defined under clang and clang-cl, never under MSVC.
(2) A third axis `dm` (lp64, ilp32, llp64): `_WIN64`, `__LP64__`, `_LP64`; lp64 implies not Windows,
llp64 implies Windows. A touched dm leaf forces the OS axis, which forces the compiler axis.
(3) Valued rows (`Row.val[leaf]`, `pc_fam_values`): `__clang_major__`, `__GNUC__` (clang: 4),
`__GNUC_MINOR__` (2), `__GNUC_PATCHLEVEL__` (1), `__CHAR_BIT__`, the limits and `__SIZEOF_*__`
(uniform, or per data model: `__LONG_MAX__`, `__SIZEOF_LONG__`, `__SIZEOF_POINTER__`, ...). A value
that is the same on every leaf is one unconditional entry. Guarded values gain scale: `N_INIT` is
`s*X + n`, and `N_VER` is `s*X + Y + n` with Y's table range <= s (`__GNUC_MINOR__` < 100), so
`__GNUC__*100 + __GNUC_MINOR__ >= 403` becomes `GE(X,5) | (GE(X,4) & GE(Y,3))`. Radix shifts
(`<< 16`) work the same way. Fixtures `has`, `versions`.
(4) The why-dedupe keys on the unmet requirements only, so cubes that differ only in derived
requirements no longer print twice (`has.pc` now asks for `cc = clang`, not `os = android`).
Corpora: 0 BAD, rank-1 0 bad. zstd 89% -> 98% exact, lua 91-94% -> 97-100%. Left opaque: byte order
(`__BYTE_ORDER__ == __ORDER_*__`), user-macro comparisons, residual `__has_builtin`. Suite 2197 passed.

Round 261: S1d-4 the language axes.
`pcfam.c` gains two one-hot axes beside OS, compiler and data model: `std` (c89, c99, c11, c17, c23)
and `mode` (gnu, iso). `__STDC_VERSION__` is valued per std leaf (undefined in c89), `__STRICT_ANSI__`
is the iso leaf. `PcAtom.target` becomes `PcClass cls` (config, language, target); a language leaf
weighs 100 (config 1, target 1000), a cube carries `lang` beside `target` and `cost`
(`pc_cube_weight` is the one scalar the DP, the ranking, the brute-force gate and the selftest share),
requirements of the class print as `language std = c23`, and a derived macro is hidden when any
leaf must change. The host leaf of `std` is read from the value of `__STDC_VERSION__`. `pc_family`
no longer adds the "MSVC builds for Windows" clause unless a compiler leaf is in play. Fixture `lang`.
Not done: bare `linux`/`unix` under gnu modes (a cross-axis row), the extension set, C94. Corpora:
0 BAD, rank-1 0 bad, exactness unchanged. Suite 2198 passed.

Round 262: S1e hover and `cereal/configExplain`.
(1) The trace is per-Analysis (`PcTrace`, listeners with `ctx = PcTrace*`), so it works under
`-fparallel=on` too (phase A runs every directive on the calling thread); the LSP keeps the folded
`PcUnit` in the `Snapshot` (`analysis_keep_pc`/`analysis_take_pc`). (2) `pc_explain` finds the
innermost group at a position (ORing the instances of a header included more than once) and
`pc_explain_render` writes the hover: status, directive, short condition, and the ranked "To compile
it" alternatives with kind labels (target, language, environment, unknown) and a `via` label naming
where the unit defines the macro. "Other" leaves are never offered. Hover on a `#if/#elif/#else` or
in skipped text uses it; `PcReq` gains `id` and `residual`. (3) `cereal/configExplain` returns the same
as data: `{directive, line, onDirective, status (compiled|skipped|never), exact, tainted, condition,
why: [[{kind, clause, unknown}]]}`, or null. Queries mutate the snapshot's BDD manager, so they run
one at a time (requests are single-threaded; no mutex). Goldens `tests/lsp/pcexplain.json` (both
parallel modes), `basic.expected` refreshed. Known quirks: include-guard atoms (`defined(CFG_H)`)
show up in answers; alternatives 2-3 can be redundant. Suite 2200 passed.

Round 263: byte order, guard labels, selftest, alternatives.
(1) `pc_req_via` (pc.h) is the one `via` label for the hover and for `cereal/configExplain` (new `via`
field per requirement): "the unit defines `X` at f:N", or, for the guard of a header that defines it,
"the guard of f: it makes the unit skip that header" (so `defined(CFG_H)` reads as what it is).
(2) A sixth axis `endian` (little, big), a target: `__BYTE_ORDER__` is valued per leaf (1234, 4321),
`__ORDER_{LITTLE,BIG,PDP}_ENDIAN__` are gcc/clang constants, big-endian excludes Windows, the host leaf
is read from `__BYTE_ORDER__`. `__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__` is now exact. Fixture `endian`.
(3) `pcq selftest` draws macros `__STDC_VERSION__` and `__BYTE_ORDER__` and a leaf for every axis
(`pc_leaf_axis`), so the language and byte-order blocks meet the brute-force gate too.
(4) `pc_preconditions` drops an alternative whose unmet requirements include all those of a cheaper one
(`std = c11; defined(USE_X)` after `defined(USE_X)`); the next distinct cubes take their place.
Corpora: rank-1 0 bad everywhere. Suite 2201 passed.

Round 264: bare linux/unix, enable, cover, capability.
(1) `linux`/`unix` are now family rows (os linux-ish, and mode gnu: they are not defined under `-std=c99`),
via a second axis per row (`axis2/must2/never2`); the other OS macros (`_AIX`, `__sun`, ...) are
`OTHER_OS` rows defined only by the "other" os leaf, so an overlay can set them. zstd rank-1: 691 ok, 158
too large, 0 bad. (2) S2 library: `pc_enable` (pcq.h) turns a cube into command-line words (`-DX=v`, `-UX`,
`-std=`; cereal reads c99/c11/c17 and gnu99/11/17), `pc_enable_group` takes the first of the three cheapest
cubes that an overlay can do (no target, env, opaque, unknown or residual requirement, and no macro the
unit defines itself: a test before the definition is a guard or default, and `-DX` would change which
code is read; `ZSTD_DEPS_COMMON` is such a block guard that `guard_covers_file` missed). `pc_cover` is a
greedy set cover of those overlays over the groups that are off, exact and enable-able (at most max,
ties by first candidate). Dump words `enable`, `cover`; fixture `tests/pc/enable`. Oracle
`bench/pc_enable.py [--cover]` re-runs cereal with the words and checks the preprocessor takes the group.
Corpora: enable 0 still off (libuv 263/1 unverified, uvloop 7/1, rest active); cover 0 failed
(lua: 2,000+ claims held; lupa 2136 groups by 8 overlays, uvloop 5718 by 8; zstd 125 by 8). Merging: see Round 265. (3) `initialize` advertises `experimental["cereal/configExplain"]`; 30
session goldens gained the key. Parked: the (file, hash) fold cache. The fold is 33-57 ms on the largest
corpus units, and its key needs the hash of every included file, known only after preprocessing; a hit
would save the fold alone (a lint of uvloop/loop.c takes ~130 ms without the fold). Suite 2202 passed.

Round 265: `cover` merges overlays.
`pc_cover` now runs the greedy pick to exhaustion, then folds overlays that agree (no macro or `-std`
set two ways, at most 16 words) into one while every group of both still holds under the union
(`merge_items`), sorts by size and keeps the first max (8). Corpora (oracle `pc_enable.py --cover`, 0
failed): lua53 819 of 819 eligible groups by 71 overlays (was 634 by 272), lua54 772 by 70, lua55 951 by
69, lua52 722 by 119, libuv 774 by 231, zstd 241 of 241 by 8, lupa 2234 and uvloop 5825 by 8. Fixture
`enable` gains a clashing `-DFEATURE_B` case. Suite 2202 passed.

Round 266: fold cost.
Callgrind on a unit of 107 system headers (fold only): 39% re-lexing skeletons (`skel_get`, one per file
the fold follows; inherent while the fold reads directives itself), 22% `macro_state`, 11.5% `strcmp`
(the family table scanned linearly, 44k times), 17.5% `arena_printf` (the `name@loc` label of an
unmodeled `#define`). Now `row_of` is a hash index built on first use (race-safe by a pointer CAS) and
the label is built by hand: 17.5M to 12.5M instructions (-28%). Suite 2202 passed, rank-1 0 bad. Whole
corpus, fold ms over lint ms: libuv 62%, lua 61-87%, lupa 40%, uvloop 30%, zstd 18% (before: 68%,
67-115%, 36%, 26%, 18%; timings are noisy). The fold is still of the order of a lint, so a server that
wants overlays gates it (`--cover`).

Round 267: decision (no code): the fold replays events for system headers.
Survey: system headers are 72-100% of the files the fold reads (libuv 94%, lua 73%, zstd 100%) and the
fold re-lexes and re-walks each, though the preprocessor already did. Planned: record the listener's
define/undef/include/include_miss/cond events per include-chain hash in the trace and replay them
(`do_define`/`do_undef` under the include site's condition) instead of `fold_file` on a `conc` header.
Decided: groups inside system headers are kept for the groups the run took (from `cond` events) and
dropped for the ones it skipped (it never reports them): hover/explain on a skipped system-header
conditional loses its group; `enable`/`cover` never used them. Not started. Gate before it becomes the
default: old and new fold side by side over the corpora with identical user-file group conditions; the
suite unchanged except the system-header group records. Open: the `Macro` body adapter for
`pc_const_value`/`pc_parse`/`fn_macro`, taint/skipped counters from the include events, event memory.

Round 268: the fold replays the preprocessor's events for system headers.
(Round 267's decision about system-header groups was moot: `record` already skipped groups inside a
system header, so nothing is lost.) The trace (`PcTrace`) now also records, while inside a system header
entered from outside one, the `define`, `undef` and not-found `include` events in order (`PcEv`), as one
run per outermost include keyed by its chain hash (`PcRun`). `do_include` replays that run
(`replay_run`: `do_define` from the `Macro`'s body with the same constant/expression/function-macro
rules, `do_undef`, unresolved-include counters and taint) instead of `fold_file` on the header. A header
the run did not enter (guard or `#pragma once` skip, or an include in a group the host does not take)
still folds from its skeleton. Gate (a throwaway A/B script, since deleted): the 56,000 corpus groups
had identical conditions up to BDD variable order, and the totals (skipped, unresolved) matched; the
suite and the oracles are unchanged (rank-1 0 bad; enable/cover 0 failed). Cost: 107-header unit
12.7M to 1.5M instructions (-88%), libuv unix/fs.c 67.6M to 31.9M (-53%; the rest is BDD work on the
user files and skeletons of the headers it falls back on), whole-corpus fold time about -25% to -40%
(noisy). Known difference: a `#include MACRO` inside a system header, which the old walk skipped
(taint), is followed through the run's events; no corpus group changed.

Round 269: measurements, no code.
(1) The fold is not in the `index-summary` child (it never attaches the trace); with the replay a unit's
fold is 240 ms for libuv's 34 units and 110-180 ms per lua version (best of 3, sequential), against an
index job of 1,093 ms (libuv) and 530 ms (lua54): about 20-25%, above the 10% that would let the sidecar
be written always. So `--cover` stays gated by `cereal.index.configs`. (2) Tried skipping the fold of a
guard-covered system header the run did not enter (nothing in it can be taken, so no skeleton needed):
the suite and oracles were unchanged and the fold time did not move (libuv 240 to 281 ms, noise), so
it was reverted. The fallback skeleton folds are not the remaining cost; BDD work on the user files is.

Round 270: BDD cost of the fold, negative result.
Callgrind of libuv unix/fs.c (31.9M instructions in the fold): `do_define` 48% (of which `macro_state`
creating a macro's state 20%: `pc_atom` 11%, `xcalloc` 10%), `bdd_ite`/`bdd_and`/`mk` about 40%
inclusive, `skel_get` 20%, `qsort` 9%. Tried making a macro's initial state lazy (no `defined(X)` atom
or family values until it is read or changed under a condition; a define or undef under TRUE needs
neither). Goldens, oracles and cover were unchanged, but the fold took 32.3M instructions (31.9M
before) and the same wall time, so it was reverted: most defines in the corpus units are not under
TRUE, or are read soon after. Also tried: the taken set as a hash set instead of sort + bsearch: 31.9M to 31.3M (-2%), reverted. The
`qsort` share is 2,700 calls from elsewhere in the fold (not the taken list; not traced further).
Not tried: arena-allocating `PcMacro` and a BDD variable order that follows first use. The fold stays at about
7 ms per unit (libuv) with the replay.

Round 271: the 2,700 `qsort` calls and a per-call calloc, fixed.
Traced: `bdd_vars` (via `pc_sat`/`pc_constrain`/`pc_monotone`/`pc_family`) sorted the variable list and, like
`bdd_count`, `xcalloc`ed a visited array of the whole node table on every call (the `xcalloc` and `memset`
share of Round 270). Both now share a generation-stamped mark array in `BddMgr` (`walk_marks`), no clearing
per walk. libuv unix/fs.c fold, sequential best of 6-8: 7.6 ms to 6.7 ms (-12%). Suite 2202 passed,
rank-1 0 bad, cover 0 failed. The sort stays (callers rely on sorted variables).
Callgrind after the fix (fs.c fold): 31.9M to 28.2M instructions (-12%). The profile is now flat: `mk` 9.7%
self, `bdd_ite` 13%, `skel_get` 21% inclusive (real lexing of user files and fallback skeletons), `macro_state`'s
`xcalloc` 3.4% (3,497 calls). Not worth doing: an arena for `PcMacro` (at most 3%) and a first-use variable
order (speculative, touches every golden's atom order). The fold's cost work stops here.

Round 272: virtual units (S2), the scheduler slice.
`cereal.index.configs` (ratio, default 0) and `cereal.index.maxOverlays` (8). Child: `index-summary
--cover N` attaches only the fold (`analysis_attach_pc`, not the checks), writes `ovl/<tukey>.ovl`
(always, even empty, so that a manifest implies its sidecar); `--overlay WORDS` (with `--entry`)
indexes the entry with the words after its own flags (`config_options_overlay`; the later -D/-U/-std
wins; the options fingerprint is sequence-sensitive, so the tukey differs). Scheduler: `PTu` gets
`overlay`/`parent`; `PS_COVER` (fresh, no sidecar, configs on) re-runs a real unit once;
`pstate_allocate` reconciles the virtual units from the sidecars. First version allocated in
completion order and a restart re-ran 2 of 6 virtual units (a different set), so it now waits until
every real unit is fresh or backing off and serves the budget in main-path order; at startup with a
warm cache it allocates in `pstate_set_config`, zero jobs. Test: `proj_test.py configs` (budget
respected, names added, restart without a job, off again). Suite 2203 passed. Rename ignores virtual
units for now (see CONFIG_DESIGN 6).

Round 273: `cereal.index.configs` measured (throwaway driver over `cereal lsp`, cold cache, 3 threads, `maxOverlays` 8,
single runs, load ~1; the driver was deleted).
| project | configs | units | time | names | cache |
| libuv (34 real) | 0 | 34 | 0.4 s | 4,969 | 1.1 MB |
| | 0.5 | 51 | 1.2 s | 7,006 (+41%) | 1.5 MB |
| | 2 | 102 | 1.7 s | 8,616 (+73%) | 1.9 MB |
| | 6 | 238 | 3.0 s | 10,333 (+108%) | 2.7 MB |
| lua54 (33 real) | 0 | 33 | 0.2 s | 18,281 | 2.0 MB |
| | 0.5 | 49 | 1.7 s | 20,589 (+13%) | 2.6 MB |
| | 2 | 99 | 1.9 s | 22,805 (+25%) | 3.4 MB |
| | 6 | 103 | 1.8 s | 22,934 | 3.4 MB (the sidecars run out at 70 overlays) |
The first 0.5 per real unit buys most of the names (libuv +2,037 for 17 units; the next 51 units +1,610, the
next 136 +1,717). Cost: a full index is 3-8x slower on these small projects (the fold in every real job, plus
the virtual jobs), seconds in absolute terms; the cache grows 36-70%. Recommended on-value 0.5 (as the design
proposed); the default stays 0 until the S2 gate (wsym p95 and builder p50 not regressed with configs on) is
measured, which this round did not do.

Round 274: realizability of cover overlays, negative result. Hypothesis: the 25-34% of overlays that give PARTIAL
virtual units are platform-identity macros, so classing every `__`-prefixed macro outside the family table as
`PC_TARGET` (so `enable` skips it) would filter them. Tried in `pc_atom`: suite and both oracles stayed green
once family-table macros were excluded, but lua54 still gave 39 of 106 overlays PARTIAL (37%, was 34%). Bisecting
lapi.c's first PARTIAL overlay: the cause is `-DLUA_BUILD_AS_DLL` (selects `__declspec(dllexport)`, 674 errors on
its own); no static name rule finds it (it is a project macro whose value is MSVC syntax). Reverted. Measured by a
throwaway driver (deleted). The usable signal is the result itself: a virtual unit whose manifest is PARTIAL.
Proposal, not built: treat a PARTIAL virtual unit as dropped (names not merged, budget passed to the next sidecar
line); costs one wasted job per bad overlay, no heuristics. Also: rename already refuses names that occur in an
inactive `#if` group of a user file (`crename.c inactive_use`), so a gate over virtual units adds nothing; the
real feature is relaxing that refusal where the group is covered by a fresh virtual unit (design item, CONFIG_DESIGN 6).

Round 275: PARTIAL virtual units dropped; the pc_enable scratch; the S2 gate measured.
(1) `pstate_allocate` does not want an overlay whose manifest says PARTIAL, FAILED or OVER_LIMIT (`vc_bad`, a probe
of the manifest if the unit is not loaded): the budget goes to the next sidecar line. A bad virtual unit that
finishes sets a flag; once every unit is settled the scheduler allocates again (once, not per unit: a first version
re-allocated per bad finish and republished each time). Test `proj_test.py badoverlay` (every source gets an
`#ifdef PROJ_BAD` group that does not parse; fails with the skip disabled).
(2) wsym_bench (1770 units) with configs 0.5 did not finish in 500 s (10 s with configs off). Cause: `pc_preconditions`
allocated and zeroed a table of the whole BDD (about 1 MB x 4 on LuaJIT) per group (`memo`) and per literal of every
candidate cube (`prime`'s `ok` memset); 21 s for one LuaJIT unit's cover, 90% in memset. Replaced by one `BddScratch`
(generation-tagged slots, never cleared; `bdd.h`) that also serves the graph walks (it replaces `mark`/`stamp`). The
The bench now finishes (below); a profile mid-run is flat BDD work.
(3) S2 gate, 1770-unit wsym project, 3 threads, WSL, machine otherwise idle, single runs:
| configs | index | units | names | wsym p50/p95 | references p95 |
| 0 | 10.1 s | 1770 | 216,466 | 14.6 / 19.3 ms | 2.6 ms |
| 0.5 | 157.5 s | 2655 | 268,316 | 18.3 / 25.9 ms | 2.7 ms |
Builder edit-to-refresh (300 units, 100 edits 0.5 s apart, `quietMs` 100; throwaway driver): p50 configs 0.5 =
32.1/32.8/31.6 ms against configs 0 = 32.4/30.6/40.7 ms (two earlier runs read about 180 ms for both settings, right
after other heavy jobs on the machine; discarded). So the builder is not regressed. wsym p95 rises 34% (more names;
limit 100 ms). The cost of the on-value is index time (15x on this project), not latency. Default stays 0.

Round 276: the rename gate uses virtual units.
`c_rename` with `CRename.inact` set records the uses of the old or new name in inactive `#if` groups of user
files (path, offset, the refusal text) and the files its index has, instead of refusing. `lsp_rename_gate`
(`resolve_inactive`) then accepts a use when another unit that passed (real or virtual) reads that file and does not
have the use inactive; otherwise it refuses with the same text as before. `do_scope` appends the fresh, sound
virtual units that list the edited files (after the rename_max check, so they do not count against R5); `gate_job`
checks them with `config_options_overlay`; their failures never fail the rename, they just cover nothing. The
position match is by file path and offset, so it relies on every unit reading the same text of a file. Conservative
where unsure: a file with no index entry in the covering unit counts as unread. Test `proj_test.py renamecfg`
(h01.h gets an `#ifdef PROJ_ALT` group naming the function: refused with configs off, goes through with them on and
renames the use in the group). Suite 2205 passed.

Round 277: where the cost of `configs` goes (S2 gate follow-up).
Index time of the 1770-unit project (wsym_bench, 3 threads): configs 0: 10.1 s; 0.001 (fold and `cover` in every real
job, one virtual unit): 161.5 s; 0.1: 163.2 s; 0.5: 157.5 s. So the cost is the real jobs' fold plus `cover`, not the
virtual jobs. Per unit (index-summary --cover 8, 8 parallel, 906 cpu-s in all): LuaJIT (450 units) takes about 4.7 s
each, 74% of the total; the fold is 0.2-0.25 s of that. `pc_cover` spends its time in `pc_enable_group`: of 1121
candidate groups of lj_state.c, 946 fail and 875 of those because the cheapest cube needs a macro the unit defines
itself (a guard or default; `unit_enable` rejected it after the cubes were enumerated and primed). Fix: a
`PcValuation.fixed` hook (target atoms and macros the unit defines cannot flip), which makes `dp` skip such paths, so
the search finds only cubes `pc_enable` can use. lj_state.c: 3.0 s to 1.3 s; all units 906 to 666 cpu-s; index 157.5 s
to 116.2 s with configs 0.5 (names 288,676, wsym p95 26.0 ms). pc_enable eligible groups: zstd 241 to 251 (cubes behind
a cheaper rejected one are now found), lupa and uvloop unchanged. Rank-1 oracle 0 bad. Negative: growing the ite cache
with the node table (1.30 to 1.66 s, slower); not kept. What is left in a LuaJIT unit is BDD node creation (`mk`, 40%):
the table is 4M slots for a largest group of 2208 nodes, i.e. dead intermediate nodes (no GC), mostly from `pc_constrain`
per group.
Rename on libuv with configs (Round 276 follow-up): `rename_libuv.py`, 441 external functions: configs 0 and 0.5 both
382 renamed, 54 refused for an inactive group, 5 for a macro, 0 false refusals. The 54 are other platforms' branches
(target atoms), which no overlay can turn on; see CONFIG_DESIGN 6.

Round 278: `pc_cover` cost, second cut (callgrind on lj_state.c, 7.45 to 3.58 G instructions; wsym index at configs 0.5: 116.2 to 98.8 s, p95 24.4 ms; suite 2205 passed).
The profile put the cost in `prime` (via `pc_preconditions`), not in node creation: the enumeration collected 64 paths,
primed each (500K `implied` walks of a 3.8M-slot table for one unit, cubes of about 38 literals) and then returned 3.
The loop now stops at `4k + 4` paths (`PATH_CAP` deleted): priming only reorders the paths a little. Evidence: pc_enable
`--cover` output is identical to the 64-path run on the whole corpus, and rank-1 against enumeration is 0 bad; but the
LuaJIT lj_state.c overlay text differs, so the cap is a heuristic, not exact. `implied` returns false at once below the
cube's last variable (exact; no measurable gain). `pc_constrain` is cached (256 entries by BDD, `PcCtx.cons`): `pc_sat`
and `pc_preconditions` constrain the same group twice (5.6% of instructions). `pc_monotone` conjoins only the next
lower threshold per atom (exact, 2%). Negative: building the family constraint before ANDing f (small intermediates
first) was 1% slower; reverted. Left: `pc_constrain` is 30% (the ANDs of f with the family axes), `pc_holds` 7%.
Round 278, continued: per-unit check of the cap: 60 LuaJIT 2.1 units, `--cover 8`, 4k+4 against 64 paths: 4459 against
4390 overlay words in all, and 56 units differ in text (tie-breaks); the corpus cover counts (pc_enable) are identical.
`bdd_restrict` (new, one memoized walk): `pc_preconditions` restricts the group's condition to the host's values of the
atoms the valuation calls fixed before the family constraint is ANDed in, so the constraint (30% of the cost) sees a
smaller f and touches fewer axes. lj_state.c 3.58 to 2.94 G instructions; its overlay text is unchanged; rank-1 oracle
0 bad; pc_enable `--cover` identical; suite 2205 passed.
