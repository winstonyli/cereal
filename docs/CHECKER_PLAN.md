# cereal P2 checker — shared plan for subagents

Repo /home/user/cereal (C99 toolchain "cereal"), branch
`claude/c99-macro-analysis-qltej1`.  **Subagents never commit or push**; the
coordinator does.  Only edit the files your task owns.  User preferences:
compile speed matters (huge generated files), do things right, no
overengineering concerns.  Read docs/PARSER.md "Type checking (P2)" first.

Build: `make` (Makefile globs src/c/*.c; flags `-std=c99 -pedantic -Wall
-Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes
-Wno-unused-parameter`, must be warning-free).  `make test` runs
tests/run.sh (332 pass before P2; keep them passing).  Oracle: gcc 13
(`gcc -std=c99 -pedantic -fsyntax-only x.c`); compare message text AND
line:col.  Scratchpad (probes, gcc sources c-decl.cc / c-typeck.cc /
c-parser.cc, probe files r1.c..r5.c):
/tmp/claude-0/-home-user-cereal/a48a873c-a603-5af0-908d-b55cd1842cce/scratchpad
c-decl.cc landmarks: shadow_tag_warned 4832, grokparm 5972, push_parm_decl
6168, grokdeclarator 6545, grokparms 8195, get_parm_info 8324,
declspecs_add_type 11453, add_scspec 12338, finish_declspecs 12540.
Port gcc's logic and wording faithfully (read the gcc source for each rule).
Older design discussion: transcript /root/.claude/projects/-home-user-cereal/a48a873c-a603-5af0-908d-b55cd1842cce.jsonl
(grep it only if something here is unclear).

## Existing files (read them)
- src/c/ast.h, parse.c (parser, post-order node array), type.h/type.c (type
  table), target.h/target.c, scope.h, lit.h/lit.c (literals), diag.h/diag.c,
  src/main.c (or wherever main is: grep `parse_one`).
- src/c/check.h: public API (CheckOptions, checker_new/unit/finish/free).
- src/c/check_int.h: internal API shared by check.c / cdecl.c / cexpr.c.
- src/c/cexpr_int.h: helpers shared by the expression checker units (cexpr.c core,
  ccall.c calls/builtins/atomics/tgmath, cformat.c printf/scanf + strlen, cconv.c
  -Wconversion family); node/type accessors are static inline there.
  Prototypes there are the contract.  If you must add one, add it in the
  section for your file; do not change others' signatures without saying so
  in your report.
- src/c/check.c: core (diag wrappers, symbols/scopes, visit loop,
  per-node arrays, predeclared typedefs, tentative defs at finish).
  visit(): SCOPE/SCOPE_END handled in check.c; `cexpr_is_expr(tag)` nodes →
  cexpr_node; everything else → cdecl_node.
- To be written: src/c/cexpr.c, src/c/cdecl.c.

## AST (post-order)
Children end at i-1; node's first descendant cfirst(i) = i+1-size.
- SPECS children: STORAGE, QUAL, FUNCSPEC, TYPESPEC, TYPEDEF_NAME (only if no
  type yet), STRUCT/ENUM, TYPEOF/ALIGNAS (child TYPE_NAME or expr),
  ATOMIC_TYPE (child TYPE_NAME), ATTRIBUTE.  `__extension__` skipped.
- STRUCT: `attrs TAG? attrs OPEN(aux 0 struct/1 union) (PRAGMA |
  STATIC_ASSERT | MEMBER_DECL)* attrs`, NF_BODY when it has a body.
- ENUM: OPEN(aux 2) ENUMERATOR*(tok=name; children attrs, expr?); constant
  in scope after its ENUMERATOR.
- MEMBER_DECL: SPECS MEMBER*.  MEMBER: declarator? width-expr (NF_BITFIELD)
  ATTRIBUTE*.
- PTR: (QUAL|ATTRIBUTE)* declarator? LAST.  ARRAY(tok `[`): declarator?
  FIRST (attrs from a parenthesized declarator may precede), QUAL*,
  ATTRIBUTE*, size expr?; NF_STATIC, NF_STAR.  FUNC(tok `(`): declarator?
  first, SCOPE, (PARAM|KR_IDENT)*, SCOPE_END last; NF_VARIADIC, NF_KR.
- PARAM: SPECS declarator? ATTRIBUTE*; name in scope right after PARAM.
- INIT_DECL(tok=name): declarator attrs ASM_LABEL? attrs DECLARED(tok=name)
  initializer?.  Has initializer iff DECLARED != INIT_DECL-1.
- DECL: tok = first token, NF_EXTENSION.
- FUNC_DEF: SPECS declarator DECLARED[NF_BODY] SCOPE[NF_PARAMS] K&R-DECL*
  COMPOUND(BODY leaf, items, no scope end) SCOPE_END.
- TYPE_NAME: SPECS declarator?.
- Parser predeclares typedefs `__builtin_va_list`, `__int128_t`,
  `__uint128_t` (checker predeclares them too, in check.c).
- Only file-scope implicit int parses (`x;`, `f() {}`).
- IF/SWITCH/WHILE/DO/FOR scopes wrap the whole statement; COMPOUND =
  SCOPE…SCOPE_END.
- Tokens PTok{t, exp}; ctok_loc uses exp if set.  TF_BOL=1: first token on a
  line.  Error recovery can truncate nodes (NF_ERROR) — never crash on
  malformed trees.

## Walk design
- Specs: pushed at SPECS (parse into Spec, check.c `c->specs`), popped by the
  consumer (DECL, FUNC_DEF, PARAM, TYPE_NAME, MEMBER_DECL) with
  `while (specs.len && last.node >= cfirst(i))`.
- Declarator types resolved at the consumer: walk root declarator → NAME
  (PTR → last child; ARRAY/FUNC → first declarator-tag child), applying
  derivations root-first (gcc grokdeclarator order).  FUNC params come from
  PARAM children's ty/cv.
- Proto-scope bindings saved at SCOPE_END (cv/cb of the SCOPE_END node) and
  rebound at SCOPE[NF_PARAMS] via the FUNC adjacent to the name in the
  FUNC_DEF's declarator (check.c scope_open/close + cdecl_body_scope).
- K&R: KR_IDENT → lsym with CSF_KR_PENDING; DECL whose parent is FUNC_DEF
  declares params; at BODY remaining default to int and prototype
  comparison runs.
- Records created at OPEN (definition) or STRUCT (reference: xref lookup in
  all scopes, else declare in current scope); `struct S;` alone → this-level
  declaration.
- Tentative definitions checked in checker_finish (cdecl_finish_object).

## Diagnostics infrastructure
- cerror / cwarn / cpedwarn(c, loc, id, fmt,...) / cpedantic(c, loc,
  fmt,...) / cnote; suppressed when c->quiet (unit had syntax errors).
- cinput_loc(c, node): gcc's input_location = first TF_BOL token of the line
  of the parser's lookahead.  Used by add_scspec errors, "file-scope
  declaration of 'fr' specifies 'register'", "data definition…", float
  overflow (col 1).
- New diag options (diag.c table `{name, group, level, on, help}`):
  implicit-int (on), multichar (on), overflow (on), pointer-arith (group
  "pedantic", off → -pedantic enables), old-style-declaration (off),
  ignored-qualifiers (off).  Add more as needed (same pattern), matching
  gcc's -W names and default states under `-std=c99`.
- Warnings in system headers suppressed.

## gcc declspec rules (declspecs_add_type / add_scspec / finish_declspecs)
- add_type errors at the offending specifier's token.  Modifier/word
  conflict "both 'M' and 'W' in declaration specifiers":
  long vs auto_type, void, int_n, bool, char, float, floatN, decimal; before
  that long_long → "'long long long' is too long for GCC", long+double →
  "both 'long long' and 'double'", short → "both 'long' and 'short'".
  short vs auto_type, void, int_n, bool, char, float, double, floatN,
  decimal; before: long → "both 'long' and 'short'".  signed/unsigned vs
  auto_type, void, bool, float, double, floatN, decimal; before: "both
  'signed' and 'unsigned'".  complex vs auto_type, void, bool, decimal.
  dupe → "duplicate 'X'".
- Adding a word: "two or more data types in declaration specifiers" if a word
  or whole type already set; modifier checks order long, short, signed,
  unsigned, complex; int_n → "both '__int128' and 'long'" (reversed) +
  pedantic "ISO C does not support '__int128' types" unless spelling ends
  `__`; double checks long_long first; decimal: both long_long and long
  checks; char conflicts only with long/short; int with nothing; floatN
  pedantic "ISO C does not support the '_FloatN' type".  Word not set on
  conflict.
- Non-keyword (typedef/struct/typeof): error if any type/word/modifier seen.
- finish: modifiers only → int; complex alone → double complex + pedantic
  "ISO C does not support plain 'complex' meaning 'double complex'" at
  complex loc; nothing → int (default_int); complex+integer → pedantic "ISO C
  does not support complex integer types".
- add_scspec (error() at input_location): -Wold-style-declaration "'X' is
  not at beginning of declaration" if non_sc_seen; dup inline/_Noreturn ok;
  thread with auto/register/typedef → "'X' used with 'auto'"; pedantic "ISO
  C99 does not support '_Thread_local'"; "'__thread' before 'extern'" /
  "'static'"; "duplicate '_Thread_local' or '__thread'" / "duplicate 'X'";
  "multiple storage classes in declaration specifiers"; class other than
  extern/static with thread → "'__thread' used with 'X'", clear thread.
- shadow_tag_warned (empty decl; here = decl first token): see gcc 4832.
  Record/enum: restrict → "invalid use of 'restrict'"; unnamed → pedwarn
  "unnamed struct/union that defines no instances"; non-tagdef with sc →
  "empty declaration with storage class specifier does not redeclare tag",
  quals → "…with type qualifier…", alignas → "…with '_Alignas'…"; enum ref →
  pedantic "empty declaration of 'enum' type does not redeclare tag"; else
  this-level lookup/create.  Other → pedwarn "useless type name in empty
  declaration".  Then "'inline' in empty declaration", "'_Noreturn' in empty
  declaration"; file scope "'auto' in file-scope empty declaration" /
  "'register' in…"; if not warned: "useless storage class specifier in
  empty declaration", "useless '__thread' in empty declaration", "useless
  type qualifier in empty declaration", "useless '_Alignas' in empty
  declaration".  "data definition has no type or storage class" pedwarn at
  here when no specifiers.
- grokfield anonymous member (loc = `;` after specs, cfind_semi): untagged
  record → ok + pedantic "ISO C99 doesn't support unnamed structs/unions";
  else pedwarn "declaration does not declare anything".  "struct has no
  named members" / "struct has no members" (pedantic) at tag loc.
- grokparms/get_parm_info: lone unnamed void qualified/register → "'void' as
  only parameter may not be qualified"; with ellipsis → "'void' must be the
  only parameter"; other unnamed void → same, once, at param loc.  Tags
  declared in params → warning "'struct Q' declared inside parameter list
  will not be visible outside of this definition or declaration" (or
  "anonymous struct…"; anonymous unions exempt) at tag loc.  Definition
  incomplete param → "parameter N ('x') has incomplete type" / "parameter N
  has incomplete type".  Declaration void param → warning "parameter N
  ('x') has void type".  K&R ident list outside definition → pedwarn
  (input_location) "parameter names (without types) in function
  declaration".  "'[*]' not allowed in other than function prototype scope".
- grokdeclarator / diagnose_mismatched_decls: port from gcc (redeclaration
  messages, "previous definition/declaration of 'x' with type 'T'" notes,
  array/function derivation errors, storage-class errors, alignas errors,
  void variable, PARM adjustments).  Enumerators and params count as
  "previous definition"; an enumerator's type is the enum type (C99: int
  for expressions).

## Expected gcc output samples (r1.c..r5.c in scratchpad; regenerate with
`gcc -std=c99 -pedantic -fsyntax-only rN.c`)
conflicting types ("conflicting types for 'x'; have 'long int'" + note);
"redefinition of 'struct S'" + note "originally defined here"; "'S' defined
as wrong kind of tag"; "duplicate member 'm'"; bit-field errors (width
exceeds type, negative width, invalid type, zero width for named, "'g' is
narrower than values of its type", "bit-field 'i' width not an integer
constant"); "'e' initialized and declared 'extern'"; enumerator range /
non-constant / uintmax_t / forward-ref enum; _Static_assert (pedantic "ISO
C99 does not support '_Static_assert'", "static assertion failed:
\"fail\"", "expression in static assertion is not constant"); sizeof (void
[-Wpointer-arith], incomplete type, bit-field), "cannot take address of
bit-field 'f'"; "variably modified 'arr2' at file scope"; "size of array
'arr4' has non-integer type"; "size of array 'arr5' exceeds maximum object
size '9223372036854775807'"; literal diagnostics (multichar, -Woverflow
conversion, float range at col 1, "floating constant truncated to zero");
end-of-TU ("storage size of 'x4' isn't known", "register name not specified
for 'fr'", "nested function 'f12' declared but never defined"); K&R ("type
of 'b' defaults to 'int'" at function name, "declaration for parameter 'c'
but no such parameter", prototype mismatch notes, "argument 'a' doesn't
match prototype" + "prototype declaration"); "function definition declared
'register'/'typedef'/'__thread'" errors, 'auto' warning; linkage
("redeclaration of 'q' with no linkage", "non-static declaration of 'x'
follows static declaration", "redefinition of parameter 'x'").

## Phases (each committed separately by the coordinator)
- P2a declarations, types, layout, ICE (sizeof/_Alignof/_Static_assert,
  enumerators, array sizes, bit-field widths), --dump-types, layout parity.
- P2b expressions and conversions (all C99 6.5 constraints; usual
  arithmetic conversions; lvalues; assignment compatibility warnings, gcc
  wording; implicit function declarations).
- P2c constant expressions (arithmetic/address constants) and initializers
  (designators, braces, string init, excess elements, non-constant static
  init).
- P2d statements (labels, goto, switch/case duplicates/ranges, break/
  continue context, return checks, VLA jumps).
- P2e signature/body summaries with 64-bit structural digests, read sets
  for P3 (see docs/PARSER.md).
- P2f parity sweep (gcc.dg list scratchpad gccdg.json, corpus), speed
  (check <= parse time; corpus parse+check well under gcc's 6.3 s),
  memory.

## Wiring (main.c)
- `check` mode; `-fsyntax-only` runs parse + checker.  checker_new with
  CheckOptions{target, gnu = o->pp.gnu_mode, pedantic = tu.diag.pedantic,
  pedantic_errors, dump}.  After each unit: checker_unit(c, &u, errors
  increased during that unit); then checker_finish, checker_free.
- `--dump-types` (to stdout), `--target=NAME` (target_find); usage text.

## Tests
tests/check/*.c with expected diagnostics goldens (same style as
tests/parse), wired in tests/run.sh; a layout-parity generator (random
structs → gcc-computed `_Static_assert(sizeof/offsetof…)` files that cereal
must accept); docs/TYPES.md describing the checker.

## Wiring done (round 1)
- diag.c: many gcc options added (group "c"); `int diag_option_state(DiagEngine*, id)` → 1 enabled, 0 disabled by flag, -1 off by default (for gcc pedwarns that are tagged only when the option is explicitly on).
- Use id "pedantic" for plain -Wpedantic pedwarns; "" for untagged gcc diagnostics (excess initializers, "comparison between pointer and integer", "data definition has no type…").
- main.c: `check` mode, -fsyntax-only = parse+check, --dump-types (per-unit output stream), --target=NAME.
- tests/check/*.c goldens: `cereal -fsyntax-only -std=c99 -pedantic [// flags: ...] X.c`, stderr+stdout vs X.expected; also rerun with --cells. tests/gen_layout.py layout parity is wired in run.sh (LAYOUT_N seeds).

## Status after P2a (commits dd07fc7, 42f3871)
P2a done: check.c, cdecl.c (assembled single file, edit directly), cexpr.c, docs/TYPES.md (read it), tests/check goldens (`X.c` + `X.expected`; only add cases whose diagnostic headers match gcc exactly). make test 396 pass.
Known gaps: initializer checks, call args, assignment compat, statement checks (labels/switch/return/VLA jumps), -Wunused-value, -Wparentheses, -Wsign-compare etc., 0b constants under c99 (gcc pedwarns), "did you mean" sc_dist is slow on error path.
P2b/c/d run concurrently: several agents edit the tree at once. Use small Edit calls (never rewrite whole shared files: check_int.h, check.c, cdecl.c, cexpr.c); if a build error appears in code you don't own, wait briefly and retry — another agent is mid-edit. Don't commit.
