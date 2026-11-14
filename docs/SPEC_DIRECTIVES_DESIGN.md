---
status: draft
---

# Spec lines in the front end (stage 3a of `PROVER_DESIGN.md`)

Drafted 2026-11-14, revised after review rounds 1 and 2 (below). Decided 2026-11-14 ("as rec"): A a1, B b1, C c1, D d1. Directive syntax and the spec language are `SPECS.md` sections 3 and 6 and are linked, not restated. This document is how the parser, checker and typed AST carry a contract (`requires`, `ensures`, `invariant`) so that the prover (stage 3b, `src/c/prove.c`) reads it as typed nodes in function scope. Decided in `PROVER_DESIGN.md` (b1, 2026-11-14): the checker's own parser reads contract expressions.

## 1. What exists (code read 2026-11-14; "review" = checked by round 1's reviewer, not by hand)

- An unknown directive is an error (`pp.c:2666-2686`), so the bare `#requires` spelling cannot appear in any valid input; no test or source uses it. `#pragma cereal ...` is the portable spelling `SPECS.md` already gives (line 104). `pp_do_pragma` (`ppexpand.c:1149`) re-spells a pragma's raw tokens verbatim as one `TK_PRAGMA` token (`:1374-1383`), expands macros only for `#pragma message` (`:1330`), and an unknown pragma falls through to `DW_UNKNOWN_PRAGMAS` ("ignoring '#pragma ...'", in `-Wall`, `diag_opts.def:12`).
- A `TK_PRAGMA` token is one plan item (`plan.h:30,45`), hashed by the cell cache by its raw text (`cell.c:259`), printed by `-E` as `#` plus its text (`ppout.c:169-175`), and replayed from `plan->pragmas` (`pp.c:3014-3027`). A `_Pragma` operand is lexed from scratch space by `destringize` (`ppexpand.c:1412-1437`).
- The parser turns pragmas into `N_PRAGMA` leaves (`parse.c:1016-1075`, `:3505`); `skip_prag` (`:98-110`) makes them invisible inside a declaration. Each top-level pragma becomes its own unit and its tokens are dropped (`parse_next`, `:3493-3508`). `loop_pragma` (`:1019`, `:2927`) makes a pragma attach to the next `for`/`while`/`do` and errors otherwise (`:1066`). The parser has no preprocessor and no scratch cursor: its tokens come from `pp_source`, `cell_source` or a `TokCursor` (`frontend.c:13-22,100,121`) (review).
- Expression nodes are typed in tree order with scope opened and closed by `N_SCOPE`/`N_SCOPE_END`, which are empty marker nodes, not parents (`parse.c:748`; review, `check.c:604-640`). Nodes under an `N_FUNC_DEF` before its body are read as declaration material (`tast.c:2530`, `cdecl.c:2292,3564`, `cstmt.c:1605,1637`), so a spec expression must not sit there (review). `tast.c:1686` lowers `N_PRAGMA` to a nop; `cstmt_warn.c:233` lists it as not resetting fallthrough state; `_Static_assert` is the precedent for a checker-typed expression node that lowering skips (`tast.c:1449,1721`) (review). `El` begins at `tast_int.h:60`.
- `$` lexes inside identifiers (`driver.c:23`), `==>` lexes as `==` then `>` (survey), and a directive in an inactive `#if 0` group is ignored without a diagnostic (`pp.c:2616-2619`, review).

## 2. Decisions

### Decide A: the spelling and the carrier

Searched: tried a token run with its own plan item (the bare `#requires` carried as start token, expanded tokens and end marker) and a single pragma token; tried reading specs from the source text without the preprocessor, which loses `#if` and macros altogether. Removed: the plan item and the three new directive keywords (a1 needs neither; `pp.c:748-752` interns directives by position and drops the last two in strict mode, which would have broken new keywords).

- a1 **`#pragma cereal requires|ensures|invariant EXPR`, no macro expansion in the first cut** (recommended): the preprocessor emits the ordinary pragma token it already does; one change in `pp_do_pragma`: a `cereal` branch ahead of the unknown-pragma warning. The parser lexes the pragma's text itself (open: how, see risks). Plan items, cell hash, `-E`, parallel replay and the skeleton pass see nothing new. The bare `#requires` spelling and macro expansion inside specs are parked, trigger: the first contract that needs a macro (expansion must then emit macro-reference records so the index and `--check-graph` see it).
    - Pro: no preprocessor machinery changes; no gating option (nothing newly valid).
    - Con: no macros in specs yet; the spelling is `#pragma cereal` rather than the directive form `SPECS.md` calls its main syntax.
- a2 **The bare directives with macro expansion now**: `#requires`, `#ensures`, `#invariant` as directives, expanded, delivered as a token run.
    - Pro: the `SPECS.md` spelling, macros, real token locations.
    - Con: new plan item kind, cell hash, `-E` and parallel-replay changes, directive-table registration, expansion code with macro records, `-Wtraditional` handling; nothing in the S3 to S7 loops needs it.

### Decide B: how a spec is placed in the tree and reaches the prover

Searched: tried a new `N_SPEC` node with an `El.specs` side table keyed by function or loop node id, a `TO_SPEC` tree operation, and (round 1) reusing `TO_NOP` with a kid. Removed: `N_SPEC` (a spec is an `N_PRAGMA` with an aux kind and one expression child) and the side table as the default. Round 2 showed `TO_NOP` cannot carry specs: `lower` drops it (`tcfg.c:401-402`), so nothing reaches `CBlk.st`; a spec needs an op that `tcfg` emits.

- **Placement.** An `invariant` is a statement-level pragma before its loop (the `loop_pragma` pattern: an error unless a loop follows) whose text the parser keeps pending and parses after the loop's scope opens (`open_scope` at `parse.c:2930` comes after the pragma, and a `for` init variable lives in it, so a sibling expression could not name `i`); it becomes an extra trailing kid of the loop node (slice 2 checks the positional kid readers, `tcfg.c:441-470`). `requires` and `ensures` appear before a function definition; the parser keeps them pending at top level (instead of making each its own unit) and emits them as the first children of the body compound, after its `N_SCOPE`, when the body opens. The expression is parsed once, at that point; the parser needs scope only to tell typedef names apart, and the checker resolves identifiers in its own scope on visit. A pending spec whose next unit is not a function definition (a prototype, a variable, end of file) is an error ("contract not followed by a function definition"); contracts on prototypes wait for call support in 3b (parked when built).
- b1 **A `TO_SPEC` operation (kind in aux, one expression kid) that `tcfg` emits as a block statement** (recommended): contracts are the first statements of the entry block. An invariant is the loop node's trailing `TO_SPEC` kid, which `tcfg` emits as the first statement of the loop's back-edge target block: the head `h` for `while` (`fall(L,h)`, `tcfg.c:411`), the body block `x` for `do` (`h` there is the test block after the body, `tcfg.c:424-436`), and for `for` the head after the init has been placed (the body block when there is no test, `tcfg.c:468`). The prover finds specs by position in `Cfg`. A `TO_SPEC` needs an explicit case everywhere an unknown op is unsupported today: `tcfg.c:384-386,555` (`lunsup`), `cgx86.c:2011-2012` (`unsup`), `eval.c:2063-2070` (`UNSUP`); the verifier just breaks (`tast.c:2253`). `put_tree` prints it. Its kid is carried verbatim, never lowered through `lx`/`lcond`: `==>` and `&&`/`||` in a spec would otherwise split into blocks (`TO_LOGOR` is control flow, and `seen_tree` counts it, `tcfg.c:641-650`), so `seen_tree` and the other walkers must not descend into a spec's kid.
    - Pro: specs sit where the prover reads them; no side table to keep in sync.
    - Con: one new operation and a `tcfg.c` change (its loop lowering takes the preceding spec); walkers that recurse into all kids (`struct_stmts`, `seen_tree`, `reg_walk` at `tcfg.c:631-651,717`, `sconst` at `tast.c:1905`) and the verifier's used-exactly-once check (`tast.c:2390`) must be tested with specs present; slice 2 does that first.
- b2 **A side table on `El`** (`{kind, expression node, anchor}`), `N_PRAGMA` lowered to a plain nop.
    - Pro: spec nodes are off every statement path, so no walker sees them.
    - Con: one more table, and an anchor mapping a loop node to its head block, which `Cfg` does not provide: that needs the same `tcfg.c` loop change plus a record.

### Decide C: first-cut scope

Function definitions (`requires`, `ensures`) and loops (`invariant`). Expression forms: C expressions, `return`, `==>`, `$len(p)`, and slices `p[lo:hi]` as comparison operands (`copy` needs `ensures dst[0:n] == src[0:n]`). `==>` lowers to `!a || b`, so it adds no operation; `$len`, `return` and slices lower to `intrin` nodes (`tast_int.h`), the existing builtin form. `$old`, `$valid`, `$init`, `$all`, `$any` wait (`$all` needs a binder in scope).

- c1 **That scope** (recommended). Pro: smallest set that states the S3 to S7 loops. Con: contracts using `$old` or `$all` wait.
- c2 **Include `$old` and `$all`**. Pro: closer to `SPECS.md`. Con: the binder and the pre-state snapshot are the two hard checker pieces and no spike needs them.

### Decide D: spec arithmetic

`SPECS.md` 3 says spec arithmetic is mathematical, not C's wrapping arithmetic.

- d1 **The checker types a spec as C; the consumer reads it mathematically** (recommended): the prover, knowing the node sits in a spec, reads `+`, `-`, `*` and comparisons as Int; checked-mode code generation would use overflow-detecting widening on the same nodes.
    - Pro: one set of typing rules; semantics live with each consumer.
    - Con: a spec's meaning is not visible in its node types.
- d2 **A spec mode in the checker** with an unbounded integer type. Pro: self-describing. Con: a new type and conversion rules for one consumer.

## 3. End-state fit

- Preprocessor: one `cereal` branch in `pp_do_pragma`; nothing else changes (a1).
- Parser: a pending-spec list and a lexer over the pragma text; `N_PRAGMA` gains an aux and an optional expression child. The unroll pragma's hand reader stays; it is an older, different mechanism (a later fold is parked, not part of this plan).
- Checker: spec expressions are typed in scope in a spec context that does not warn about unused values or comparisons; `return`, `==>`, slices and `$len` are the only new forms. Whether spec names count as symbol references is decided in slice 1 (open).
- Tast and cfg: `N_PRAGMA` with a child lowers to `TO_SPEC`; `tcfg.c` emits it (a loop's trailing invariant kid goes first in its back-edge target block) and prints it; `eval.c`, `cgx86.c` and the unsupported-op defaults get an explicit skip case. The checker types the pragma's child on visit (`cdecl_pragma_visit` today runs `cpragma_apply` on a leaf; it must also type the child), and `st` at `tast.c:1686` lowers it as a spec kid, not a nop. One new operation, no table (b1).
- Replaces and deletes nothing. `SPECS.md` section 6's `#requires` form stays design (a2, parked).
- Guard: no class of drift is removed, so none is added.

## 4. Slices, each ending in a check that can fail

1. **Parser and checker**: the `cereal` pragma branch, the pending list, the lexer over pragma text (first step: a throwaway spike that lexes a pragma's text into tokens the parser can read, and whether diagnostics, `tok - 1`, `last_bol` (`check.c:700`) and scratch lifetime (`tokcur_open`, `toks.c:36-55`) survive; fallback a2), `N_PRAGMA` aux and child, `return`, `==>`, slices, `$len`, spec context, misplaced-spec diagnostics. Check: new goldens in `tests/parse` and `tests/check` (not `tests/pp`, whose cases are diffed against gcc `-E` and fed to the parallel and macro-graph loops, `tests/run.sh:66-77,172,245`): a spec naming a parameter, a local, a `for` init variable (an invariant that must pass), an unknown name, `return` outside `ensures`, `a == > b` (stays an error), a loop invariant before a non-loop, a contract before a prototype, a spec under `-Wall` and `-pedantic`; every existing golden unchanged.
2. **Tast and cfg**: `TO_SPEC` lowering, `tcfg.c` emission and loop-head placement, `eval.c`/`cgx86.c` skip, `--dump-tast`/`--dump-cfg` printing; first run the kid walkers and the verifier's refs check with a spec present. Check: `tests/cfg` goldens for the S3 to S7 functions with contracts (contracts first in the entry block, invariant first in the loop head for `for`, `while`, `do`); a golden with `==>` and `&&` inside a spec under `--verify-cfg` (no blocks split, no hidden-control-flow count change); `--verify-cfg` unchanged on all existing cases.

## 5. Risks

- The parser has no scratch source of its own to lex pragma text. The checker re-lexes `TK_PRAGMA` text (`cpragma_apply`, `cpragma.c:594-640`, via `cdecl_pragma_visit`, `crecord.c:1489`), but its static `lex()` (`cpragma.c:44-120`) serves fixed `pack`/`GCC` shapes: names, numbers, strings and single-character punctuation, no `==`, `<=`, `&&`, `->`, char constants or keywords, and it has no `cereal` branch. The slice 1 spike compares a `Tok`-producing re-lex of the pragma text against a2 (and against parsing checker-side); it does not assume reuse. Pending specs cannot be held as nodes (`parser_next` resets `p->nodes.len` per unit, `parse.c:3494-3505`, and definition-level specs cross units whose tokens are dropped), so the list copies text and locations.
- `==>` is joined from `==` and `>` with no space between; `a == > b` and a macro boundary (`EQ>` with `#define EQ ==`) cannot arise in a1 (no expansion), and the test list carries the first.
- Spec nodes are visited by every per-expression pass (warnings, symbol index, `--dump-symbols`); the spec context must silence the warnings, and whether specs are indexed is a slice 1 decision.
- A `cereal` pragma inside a declaration is invisible (`skip_prag`) rather than diagnosed; the misplacement diagnostics must hook where the pragma is seen.

## 6. Open

Decisions A to D are settled (a1, b1, c1, d1, 2026-11-14); Decide E (the loop invariant's carrier, below) is open and supersedes Decide B's invariant placement; b1 falls back to b2 only if slice 2's walker check fails. Parked until 3b needs them: contracts and calls on prototypes, `$old`, `$all`, `$valid`, `$init`, block form `#if $spec`, `#assert`, `#assume`, the bare `#requires` spelling with macros.

### Decide E: how a loop's invariant reaches `tcfg` (open, from round 4)

Contracts on functions are unaffected (they are first children of the body compound, which no positional loop reader sees).

- e1 **A parser-side list `{loop node id, spec node}`** (recommended): the invariant is parsed after the loop's scope opens and stored outside the loop's kids (the spec node is not emitted into the tree at all; the checker types it from the list while the loop's scope is open, which needs a hook where the checker leaves the loop header); `tcfg` looks the loop up and emits the `TO_SPEC`. No loop shape changes, so the verifier, `tast.c` loop cases and the `cstmt` readers stay untouched.
    - Pro: no positional reader changes; Con: a side table (the thing round 1 removed) and a checker hook for scope-time typing.
- e2 **An invariant kid in the loop node plus one shared kid accessor** that skips spec kids, with every positional reader (list above) moved onto it and the verifier's `NEED` counts relaxed.
    - Pro: specs stay in the tree, no table; Con: about eight reader sites change, each a regression risk, and the next positional reader must remember the accessor.
- e3 **The invariant as a statement inside the loop body's first position** (`for (...) { #pragma cereal invariant ... body }`): a spec statement is an ordinary body child, in scope of the init variables, emitted by `tcfg` where it stands (the loop's first body statement is the back-edge target's first statement).
    - Pro: no new table, no loop reader changes, scope is right; Con: the invariant sits inside the body (a braceless body needs braces), and it reads as a per-iteration assertion at the top of the body rather than at the head, which differs from the head when the test is false on exit (the prover must treat it as the invariant at the body entry and derive the exit fact from test plus invariant; that is the standard Floyd shape for `while` but the stated invariant must then hold at body entry, so one fewer fact is available after the loop).

## 7. Review log

Round 1 (2026-11-14): a fact-check against the code found four blocking claims in the first draft (the plan-item carrier needed a macro expander that does not exist for pragmas, the parser has no scratch source, definition-level specs cannot fold into the next unit as written, `tests/pp` is gcc-diffed) and four decision changes (nodes under `N_FUNC_DEF` break positional readers, directive registration order, unknown-pragma warning, per-expression passes); a fewer-concepts search removed the `N_SPEC` node, the `El.specs` table, the preprocessor slice, the gating decision and the anchor. This revision applies all of them.

Round 2 (2026-11-14): one blocking finding, two should-fix. b1 was false: `lower` drops `TO_NOP` (`tcfg.c:401-402`), so a nop-with-kid never reaches a block, and a pragma sibling before a `for` sits in the preceding block, not a pre-header; b1 is now a `TO_SPEC` op emitted by `tcfg` with loop-head placement (and "`tcfg.c` unchanged" is withdrawn). The verifier's refs check and the recursive kid walkers are named as slice 2 tests. The checker already re-lexes pragma text (`cpragma_apply`), so the lexing spike now has a concrete candidate. Pragma-to-`==>`, `$len` and cell-hash claims confirmed. 

Round 3 (2026-11-14, decisions settled): two blocking findings, four should-fix. Blocking: a sibling invariant before a `for` cannot name the loop's init variable (scope opens after the pragma), so the invariant is now a trailing kid of the loop node, parsed after the scope opens, which also deletes the "preceding sibling" rule; an unknown op is unsupported in `tcfg`, `cgx86` and `eval`, so each gets an explicit `TO_SPEC` case. Should-fix: `do` places the invariant in the body block, not the test block; a spec kid is carried verbatim so `==>`/`&&` do not split blocks; the pragma lexer is not reusable as is (spike compares options); the checker typing of a pragma child and `st` lowering are stated; two golden tests added. Fewer-concepts pass: nothing further to remove. Round 4 found round 3's fix unsound: loop kids are read by position from both ends, so a trailing invariant kid breaks `tast.c:1488-1491,1505-1508` ("malformed loop/for"), the verifier's exact `NEED(2)`/`NEED(4)` (`tast.c:2321-2332`), `cstmt_warn.c:1214` (`kids[k-1]` is the body) and `cstmt.c:1114-1118` (`kids[n-3]`); a leading kid shifts the front-index readers (`cdecl.c:3927-3931`, `cstmt.c:1035-1041`, `cexpr.c:417-421`). Also: `loop_pragma` keeps only a bitmask and `item_pragmas` consumes the token (`parse.c:1016-1075`), so a pending text field is needed and every loop case must clear it; the `leaf(p, N_PRAGMA, ...)` at `parse.c:1063` stays a childless nop. Back-edge blocks confirmed (`while`: after `fall(L,h)`, before `lcond`; `do`: before the body lowers; `for`: after `fall(L,h)`). Requires/ensures emission after `N_SCOPE` confirmed. The invariant carrier is reopened as Decide E (section 6); the "trailing kid" wording in Decide B is superseded by it.
