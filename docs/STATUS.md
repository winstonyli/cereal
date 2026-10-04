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

- Round 5 (2026-10-02, after 55d2e3a):
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

- Round 6 (2026-10-02, after 37fe051):
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

- Round 7 (2026-10-02, after 3450e7c):
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
