---
status: draft
---

# Spec lines in the front end (stage 3a of `PROVER_DESIGN.md`)

Drafted 2026-11-14, revised after review round 1 (below); the decisions are open. Directive syntax and the spec language are `SPECS.md` sections 3 and 6 and are linked, not restated. This document is how the parser, checker and typed AST carry a contract (`requires`, `ensures`, `invariant`) so that the prover (stage 3b, `src/c/prove.c`) reads it as typed nodes in function scope. Decided in `PROVER_DESIGN.md` (b1, 2026-11-14): the checker's own parser reads contract expressions.

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

- **Placement.** An `invariant` is an ordinary statement-level pragma, a sibling just before its loop (the `loop_pragma` pattern: an error unless a loop follows). `requires` and `ensures` appear before a function definition; the parser keeps them pending at top level (instead of making each its own unit) and emits them as the first children of the body compound, after its `N_SCOPE`, when the body opens. The expression is parsed once, at that point; the parser needs scope only to tell typedef names apart, and the checker resolves identifiers in its own scope on visit. A pending spec whose next unit is not a function definition (a prototype, a variable, end of file) is an error ("contract not followed by a function definition"); contracts on prototypes wait for call support in 3b (parked when built).
- b1 **A `TO_SPEC` operation (kind in aux, one expression kid) that `tcfg` emits as a block statement** (recommended): contracts are the first statements of the entry block. An invariant is a `TO_SPEC` placed by `tcfg` as the first statement of the loop's head block: for `while`/`do`, `lower` takes the immediately preceding spec sibling when it builds the head (`fall(L,h)`); for `for`, after the init and before the test. The prover finds specs by position in `Cfg`. `eval.c` and `cgx86.c` treat it as a nop (one case each; `cgx86.c` has no `TO_NOP` case today, so check what an unknown op does); `put_tree` prints it.
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
- Tast and cfg: `N_PRAGMA` with a child lowers to `TO_SPEC`; `tcfg.c` emits it (loop lowering takes a preceding invariant into the head block) and prints it; `eval.c` and `cgx86.c` skip it. One new operation, no table (b1).
- Replaces and deletes nothing. `SPECS.md` section 6's `#requires` form stays design (a2, parked).
- Guard: no class of drift is removed, so none is added.

## 4. Slices, each ending in a check that can fail

1. **Parser and checker**: the `cereal` pragma branch, the pending list, the lexer over pragma text (first step: a throwaway spike that lexes a pragma's text into tokens the parser can read, and whether diagnostics, `tok - 1`, `last_bol` (`check.c:700`) and scratch lifetime (`tokcur_open`, `toks.c:36-55`) survive; fallback a2), `N_PRAGMA` aux and child, `return`, `==>`, slices, `$len`, spec context, misplaced-spec diagnostics. Check: new goldens in `tests/parse` and `tests/check` (not `tests/pp`, whose cases are diffed against gcc `-E` and fed to the parallel and macro-graph loops, `tests/run.sh:66-77,172,245`): a spec naming a parameter, a local, an unknown name, `return` outside `ensures`, `a == > b` (stays an error), a loop invariant before a non-loop, a contract before a prototype, a spec under `-Wall` and `-pedantic`; every existing golden unchanged.
2. **Tast and cfg**: `TO_SPEC` lowering, `tcfg.c` emission and loop-head placement, `eval.c`/`cgx86.c` skip, `--dump-tast`/`--dump-cfg` printing; first run the kid walkers and the verifier's refs check with a spec present. Check: `tests/cfg` goldens for the S3 to S7 functions with contracts (contracts first in the entry block, invariant first in the loop head for `for`, `while`, `do`); `--verify-cfg` unchanged on all existing cases.

## 5. Risks

- The parser has no scratch source of its own to lex pragma text, but the checker already re-lexes `TK_PRAGMA` text with its own lexer and per-token locations (`cpragma_apply`, `cpragma.c:594-640`, reached via `cdecl_pragma_visit`, `crecord.c:1489`, `cdecl.c:4331`). Slice 1's spike decides between that lexer feeding the parser's expression grammar and parsing the expression checker-side (then the parser only keeps the pragma node and its position); a2 is the fallback. Pending specs cannot be held as nodes (`parser_next` resets `p->nodes.len` per unit, `parse.c:3494-3505`), so the list copies text and locations.
- Definition-level specs are held pending across `parse_next`, whose units drop their tokens; the pending list must copy text and locations.
- `==>` is joined from `==` and `>` with no space between; `a == > b` and a macro boundary (`EQ>` with `#define EQ ==`) cannot arise in a1 (no expansion), and the test list carries the first.
- Spec nodes are visited by every per-expression pass (warnings, symbol index, `--dump-symbols`); the spec context must silence the warnings, and whether specs are indexed is a slice 1 decision.
- A `cereal` pragma inside a declaration is invisible (`skip_prag`) rather than diagnosed; the misplacement diagnostics must hook where the pragma is seen.

## 6. Open

Decisions A to D (B's placement text is settled by review; b1 versus b2 is the choice). Parked until 3b needs them: contracts and calls on prototypes, `$old`, `$all`, `$valid`, `$init`, block form `#if $spec`, `#assert`, `#assume`, the bare `#requires` spelling with macros.

## 7. Review log

Round 1 (2026-11-14): a fact-check against the code found four blocking claims in the first draft (the plan-item carrier needed a macro expander that does not exist for pragmas, the parser has no scratch source, definition-level specs cannot fold into the next unit as written, `tests/pp` is gcc-diffed) and four decision changes (nodes under `N_FUNC_DEF` break positional readers, directive registration order, unknown-pragma warning, per-expression passes); a fewer-concepts search removed the `N_SPEC` node, the `El.specs` table, the preprocessor slice, the gating decision and the anchor. This revision applies all of them.

Round 2 (2026-11-14): one blocking finding, two should-fix. b1 was false: `lower` drops `TO_NOP` (`tcfg.c:401-402`), so a nop-with-kid never reaches a block, and a pragma sibling before a `for` sits in the preceding block, not a pre-header; b1 is now a `TO_SPEC` op emitted by `tcfg` with loop-head placement (and "`tcfg.c` unchanged" is withdrawn). The verifier's refs check and the recursive kid walkers are named as slice 2 tests. The checker already re-lexes pragma text (`cpragma_apply`), so the lexing spike now has a concrete candidate. Pragma-to-`==>`, `$len` and cell-hash claims confirmed. Round 3 pending.
