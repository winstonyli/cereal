---
status: draft
---

# A VeriFast-style prover and the evaluator: design pass

Question: can a VeriFast-style prover (symbolic execution over a heap, with contracts and loop
invariants) be integrated into the evaluator, and what is the end state? This is a design pass:
it decides shapes and lists spikes; nothing is built. Specs, tiers, proof witnesses and the
directive syntax stay in `SPECS.md` and are linked, not restated.

## 1. What exists (checked 2026-11-13)

| Piece | Where | What it gives a prover | What it lacks |
|---|---|---|---|
| Typed AST and a CFG per function (`label`/`branch`, `&&`/`||`/`?:` lowered; T8 built) | `tast.c`, `tcfg.c`, `TAST_T8_PLAN.md` | the input a symbolic executor wants; types and spans stay on each node, so UB sites remain expression nodes | no scope-end or lifetime-end marker; no loop-head or back-edge record (nothing for `#invariant` to attach to); initializers not lowered to stores; no `setjmp` or `returns_twice` edges; blocks carry no `SrcLoc`, only nodes do |
| Bit-vector expression DAG and an SMT-LIB2 pipe (`symx`) | `symx.c`, `symx.h` | QF_BV over 64-bit slots and 8-bit symbols, models for input bytes | no fresh-variable constructor except `sx_sym(offset)`; no push/pop (`sx_begin` forgets everything); no hash-consing (`CONCOLIC_DESIGN.md` 1.1); no array or heap theory; the SMT pipe needs POSIX fork (`CONCOLIC_DESIGN.md` 6) |
| Concrete interpreter, typed objects, UB traps | `eval.c` (2634 lines; the per-case body is `ev_node1`, about 370 lines), `eval_libc.c` | executes a call; 29 traps in `eval.c` and 21 in `eval_libc.c`; integer UB flags come from the shared `ia_binop` | traps are mostly state-dependent (uninitialized read, lifetime, freed block), not local to an edge; signed overflow traps only when `!v->o.wrap`; no alignment, effective-type, `restrict` or unsequenced traps, which `SPECS.md` sections 2 (T2) and 5 list |
| Concolic search and replayable seeds | `eval_run.c`, `CONCOLIC_DESIGN.md` | a solver model becomes a call (input bytes keyed by blob and offset) | pointers, layout, floats and aggregates are never symbolic (section 8 there) |
| Spec language, tiers, witness kernel (design only) | `SPECS.md` | the target the prover serves | no parser, no contract IR; `grep` finds `#requires` and `$old` only in docs |
| `annot = verifast` lexer field | `CONFIG_DESIGN.md` 3.7, 7.4, 8 (S5); `ROADMAP.md:217-228` | a comment channel | no design, no consumer |

The evaluator's symbolic side **under-approximates by design**: in `CONCOLIC_DESIGN.md` section 1,
item 2 ("Shadow values"), an operation not listed is a concretization ("sound for search, a lost
opportunity at worst"), and pins fix a pointer index, divisor or shift count. A proof needs the
opposite direction: an unknown operation must **havoc** (a fresh unconstrained value) and every
path must be covered or summarized.

## 2. The heap: what VeriFast adds over `SPECS.md`

VeriFast proves memory safety and functional correctness with a symbolic heap of chunks
(points-to, arrays, user predicates), `consume`/`produce` for contracts and open/close of
predicates. `SPECS.md` aims at UB elimination: its object model (section 5) already gives every
allocation a base, size, lifetime and effective type, and a pointer is `(provenance, offset)`, so
distinct allocations never alias by identity. The part chunks add over that is **recursive and
functional heap shape** (lists, trees, Lua's tables), which `SPECS.md` already defers (M3).

The permission modes of section 4 there do map to chunk shapes (`out[n]`: an uninitialized array
returned initialized; `in[n]`: a read-only array; `inout`: a points-to; `own`: a chunk transferred),
but not completely: `raw` (the default for unannotated pointers, the gradual-adoption case) has
no chunk reading, `borrow(x)` is a lifetime constraint (a region mechanism), and the `$`-facts and
slices (`$valid`, `$len`, `$init`, `p[lo:hi]`) are facts about extents, not chunks. So "modes are
sugar for chunks" is true only for a fragment; this pass does not rest on it.

## 3. Decisions

### Decide A: the heap core

Searched: I tried deleting the chunk engine (object facts per allocation, with modes only for
parameter aliasing and `#modifies` for frames) and deleting the permission layer (all formulas,
`SPECS.md` M1). The first removes chunks, `consume`/`produce` and open/close, and fails only on
recursive and functional shapes, which are parked as M3 anyway. The second loses the linear-time
aliasing pass. `SPECS.md` calls M1 "far slower", but that cost is about aliasing, not provenance,
and is unmeasured. Removed: nothing yet; a1 removes the chunk concept.

- a1 **Object facts, modes for parameter aliasing, chunks parked** (recommended): bounds, initialization and liveness are facts per allocation over the evaluator's own object table; `in`/`out`/`inout` say which parameters may alias; `#modifies` is the frame. Chunk-style predicates (M3) are a later, separate addition.
    - Pro: fewest concepts; no heap-matching engine; fits the UB-elimination product. Con: no route to VeriFast-style functional proofs of lists and trees until M3; the "not slow" claim is unmeasured (spike S3 includes a two-array case); S5 found no mode without a reading, but nullable pointers, nested reach and non-object `borrow` bounds are inexpressible (see Results).
- a2 **Chunk core, modes as a fragment**: one symbolic heap of chunks, the modes desugar to chunk shapes where they can, `raw`, `borrow` and the `$`-facts handled beside it, M3 adds user kinds.
    - Pro: the VeriFast-style end state with one engine. Con: builds `consume`/`produce` and matching first, and gives recursive shapes a home only by adding a second layer of user chunk kinds.
- a3 **Modes only, no heap model** (`SPECS.md` fork 1 as written): T2 as a dataflow pass.
    - Pro: smallest first build. Con: M3 arrives later as a second mechanism; and it cannot be combined with c1, whose single entailment engine replaces the separate T2 pass.

Coupling: c1 presupposes a1 or a2. With symbolic extents (`out[n]`, `p[lo:hi]`) neither a1 nor a2 keeps T2 as `SPECS.md` section 2's linear dataflow pass: the check is fuel-bounded entailment, cheap only when the facts are syntactically equal. Formulas only (M1) is not offered as an option; it is the measured baseline in S3 (`SPECS.md` section 4 calls it "far slower").

### Decide B: where the engine lives

The engine runs the CFG symbolically with havoc, forking, loop invariants and summaries.

Searched: I tried deleting the engine's own operation semantics by making the evaluator the prover
run symbolic-first (a concrete value is a constant-folded expression, so one rule per operation
serves both), with a mode flag that turns any concretizing site into a failure. That removes the
duplicated per-operation rules and the shadow cases; what remains is its hard half: pointer and
memory operations are not symbolic in the evaluator at all (only integers carry shadows), so
nothing yet measures that half. I also tried translating the CFG to an external verifier (Why3,
Boogie), and abstract interpretation first with the prover only for what falls through.

- b1 **Own module over the CFG, with one operation-rule table shared with the evaluator** (recommended): a module takes `tcfg` blocks, owns path states and the heap, shares `symx` (extended) and the integer rule library (`ia_binop`), and moves the per-operation concrete and symbolic rules into one table the evaluator also reads.
    - Pro: soundness direction is explicit in the new module; the table is additive and can later be replaced by b2's constant folding, at the price of deleting a table that by then holds rules for every operation. Con: the table is a refactor of `ev_node1` (its own commit); its integer half is cheap, the pointer and memory half is unmeasured (spike S2).
- b2 **The evaluator run symbolic-first with havoc** (the unified end state; rejected for now by S2, see Results): a concrete value is a constant-folded expression; havoc replaces concretization; a "prover mode" makes any concretizing site fail to compile or run; the path-recording driver (`run_fn`) already forks.
    - Pro: one definition of every C operation, the shadow cases and the table both disappear. Con: pins and host libc functions (`eval_libc.c`) under-approximate and each needs a havoc or a contract; sweep speed may fall; the evaluator takes on both directions of approximation.
- b3 **External verifier backend** (Why3 or Boogie via a CFG translator; `SPECS.md` fork 12 already allows untrusted engines).
    - Pro: no engine to build. Con: C memory semantics and UB classes live in the translator, which is then the trusted base (`SPECS.md` 7b item 4); no shared counterexample path.
Also considered, not options: abstract interpretation first (`SPECS.md` T0/T1) is a stage inside c1's ladder, not an engine; bounded symbolic execution without havoc is a bug finder that the concolic sweep already approximates and proves nothing.

### Decide C: tiers as ladder or as separate passes

Searched: I tried deleting the four tiers (T0 syntactic, T1 abstract interpretation, T2 permission
check, T3 solver) as separate passes in favor of one `entails(facts, goal, fuel)`: T0 is the
constant folding the DAG needs anyway, T1 interval and congruence reasoning over the same facts,
T2 the object-fact check, T3 the SMT call. Removed: three passes and their interfaces.

- c1 **One entailment engine with a fuel ladder** (recommended): the only parameter left is fuel; summaries inferred by T1 and invariants supplied by users are both facts the same engine checks.
    - Pro: one interface, determinism from fuel (`SPECS.md` 7; 7b needs it only of the checker). Con: widening must emit an invariant and the same engine must check it; spike S3 measures it.
- c2 **Four passes as in `SPECS.md` section 2**.
    - Pro: each pass can be built and shipped alone. Con: four interfaces, and facts convert between them.

### Decide D: what the evaluator is for, and the witness

Searched: I tried deleting the replay seam (models reported as text) and deleting the shared
witness file (a model file beside a certificate file). The first loses the executable counterexample
`SPECS.md` section 2 promises and costs nothing to rebuild later; the second keeps two keying
schemes for artifacts that are keyed by the same goal identity. Removed: the second keying scheme.

- d1 **Replay is a bug-finding oracle; one witness file holds a model or a certificate** (recommended): a failed goal's model maps through lazy-blob keys to a call the evaluator runs; a trap at the goal's site confirms a bug, and no trap classifies the goal spurious, unreachable or an invariant too weak, never "holds". Models and certificates share the stable goal key of `SPECS.md` 7b item 3 and one file. The prover never trusts the evaluator for a proof. Contracts also become runtime assertions in a sweep (needs the spec parser).
    - Pro: reuses `sweep_call` and seeds; one keying scheme. Con: the map covers input bytes (parameters, lazy blobs), not havoc symbols or heap shapes the generator never builds, so havoc needs symbol identities distinct from `SX_SYM` input offsets.
- d2 **Replay seam only, separate model and certificate files**: models keyed apart from certificates.
    - Pro: no change to the `SPECS.md` 7b file format. Con: two keying schemes for one goal identity.

Not pursued: observed ranges as invariant candidates (observed, not proved; a separate design if wanted).

### Decide E: annotation syntax

Searched: I tried deleting the annotation front end altogether (no `//@`, no `annot` field) and
deleting the directive syntax in favor of `//@`. The first removes `annot = verifast`, its overlay
atom and a lexer channel; the second drops macro expansion of specs and `#if $spec`, which
`SPECS.md` section 1 requires. Removed: the lexer annotation channel as a goal.

- e1 **`SPECS.md` directives only** (recommended): macro-expandable pp-directives and `#if $spec` blocks; `annot = verifast` is dropped as a goal (sites listed in section 4).
    - Pro: one spelling. Con: VeriFast sources cannot be read directly.
- e2 **Also support `//@` as a second front end** over the same contract IR.
    - Pro: reuses VeriFast annotations. Con: a second parser for a spec language whose semantics differ (predicates, lemmas, ghost commands); not pursued unless a corpus needs it.

## 4. End-state fit

| Change | Target | Replaces | Deletes |
|---|---|---|---|
| prover module (path states, object facts, loop invariants, summaries, obligations) | new `src/c/prove*.c` over `tcfg` | nothing | nothing |
| operation-rule table (concrete and symbolic rule per C operation, integer half backed by `ia_binop`) | one table read by `ev_node1` and the prover | the inline `*sh` shadow cases of `ev_node1` | those cases (`CONCOLIC_DESIGN.md` section 1, item 2) once the table drives them |
| `symx` extensions (fresh symbols, backtracking, hash-consing) | `symx.c` | nothing | nothing |
| `tcfg` gaps (loop heads and back edges, scope ends, block spans) | `tcfg.c` | nothing | nothing |
| directive parser and contract IR (prerequisite of every spike that uses a contract) | per `SPECS.md` section 6 | the `annot = verifast` goal | its text in `CONFIG_DESIGN.md` 3.7, 7.4, 8 (S5) and `ROADMAP.md:217-228`; whether the `VERIFAST` overlay atom survives is open (`#if $spec` covers its use) |
| counterexample replay | an `eval_run.c` entry that takes a model | nothing | nothing |

## 5. Spikes (each falsifiable, throwaway)

Each has a kill criterion; a missed one is a result, written up and the design revised.

- S1 **Obligations from existing stops**: classify the 50 `TRAP` sites and the non-`TRAP` stops (`CONCOLIC_DESIGN.md` section 8's list: unevaluated intrinsics, inline asm, missing callees) as node-local or state-dependent (needs the heap model); list the UB classes `SPECS.md` names that nothing covers. Kill: if fewer than 60% are node-local, the obligation generator is a heap-model client, not a per-node pass, and a1's "facts per allocation" must carry them.
- S2 **Operation table, both halves**: integer subset first (check unchanged results on the eval test list), then loads, stores, `TO_PTRADD`, casts and calls. Kill: if more than a third of the memory and pointer rules need a havoc form with no constant-folding reading, b2 is rejected and b1 stays.
- S3 **Tiny prover**: `int` and two array parameters (a copy loop), `requires`/`ensures`, one invariant, the single entailment engine with fuel, object facts (a1) against a formulas-only baseline (M1). Kill: a1 more than 2x slower than M1 on the two-array case, or widening failing to emit a checkable invariant on the copy loop, rejects a1 or c1 respectively.
- S4 **Replay**: take failed S3 goals (a deliberately wrong bound and a deliberately weak invariant) and run their models through `sweep_call`. Kill: the wrong-bound goal must trap at the goal's site; if it does not, the model-to-call map is not usable and d1 shrinks to d2's text. The weak-invariant goal must be classified spurious, never reported as a bug.
- S5 **Desugar the modes**: `out`/`in`/`inout`/`own`/`raw` to parameter-aliasing facts (a1) and to chunk shapes (a2) on three functions. Kill: if more than one of the five modes has no a1 reading, a1 is not the surface `SPECS.md` section 4 promises.

### Results (2026-11-13)

- **S1 ran** (Sonnet agent, table of 99 rows kept in the session scratchpad, not in the repo; counts re-checked by `cut | uniq -c` on that table and `grep -c "TRAP("`). Of the 50 `TRAP` sites, 18 are node-local, 28 state-dependent, 4 not UB (host limits, `abort`); all 49 non-`TRAP` stops (`UNSUP`, fuel, exit) are not UB. Node-local share among the 46 UB sites: 39% (57% if the 8 printf-format sites count, 65% with function-pointer and label derefs). **The kill criterion (under 60%) is met**: the obligation generator is a heap-model client, not a per-node pass. This fits a1: every bounds, lifetime, initialization and free check is one object-table lookup, so "facts per allocation" has a direct reading. No trap or stop exists for alignment, effective type, `restrict`, unsequenced side effects, arithmetic past one-past, sub-object bounds, indeterminate pointers, `va_arg` mismatch, VLA size or data races; the evaluator's traps under-cover `SPECS.md`'s list, so they cannot be the obligation list. Node-local sites: integer rules (division, shifts, signed overflow, `abs` of the minimum; overflow and shift-of-negative trap only when `!v->o.wrap`), float-to-int range, `clz`/`ctz` of zero, `__builtin_unreachable`, null deref and call, pointer compare or difference across provenances, `memcpy` overlap.
- **S2 partly run, by reading, not building.** (1) The integer half is already a table in miniature: `bin_op` (C operator to `SX_*`) and `op_sh` ([eval.c](../src/c/eval.c) near 1650-1690) beside the concrete `int_arith`; what b2 would need for integers is a concrete evaluator for `symx` nodes, and `symx.c` has none. (2) Of the 31 cases of `ev_node1`, 13 set a shadow (`TO_ADDR`, `BINOP`, `CAST`, `CMP`, `COMMA`, `FIELD`, `LOAD`, `POST`, `PTRADD`, `PTRDIFF`, `RFIELD`, `STORE`, `UNOP`); the memory and pointer cases (`LOAD`, `STORE`, `POST`, `FIELD`, `RFIELD`, `PTRADD`, `PTRDIFF`, `DEREF`, `ADDR`) either concretize a symbolic address through one of six pin kinds (`PK_LOAD`, `STORE`, `FIELD`, `INDEX`, `SHDIV`, `GOTO`) or have no symbolic pointer at all (object ids are concrete). Under b2 each of those needs a fork or havoc rule, which is the whole memory half, so the kill criterion (more than a third of memory and pointer rules with no constant-folding reading) is met: **b2 is rejected for now and b1 stays.** The table itself was not built or run against the eval tests; that, and the count of floats and host calls needing havoc, remain open.
- **S5 ran, on paper** (three real functions: zstd `ZSTD_copy8`, Lua 5.4 `luaM_free_` in `lmem.c`, `lua_tolstring` in `lapi.c`; corpus copies under `~/corpus`, no code run). Literal kill criterion (more than one of the five modes with no a1 reading) is **not met**: each mode has an a1 reading where it applies (`out[8]` and `in[8]` for `ZSTD_copy8`: allocation live, size at least 8, `dst` uninitialized on entry and initialized after, `src` initialized, the two allocations distinct; `own[osize]` for `luaM_free_`'s `block`: liveness ends; `raw` for the call through `g->frealloc`, a function pointer with no contract). The exercise found three gaps the modes cannot express, and they bite a2 as well: (1) **nullable pointers**: `block` is `own` only when `osize != 0` (`luaM_free_`'s assert ties them), and `len` in `lua_tolstring` is `out` or NULL; modes have no "or NULL" form (a chunk reading needs a disjunction). (2) **Nested reach**: `luaM_free_` reaches `g` through `L`'s `l_G` and updates `g->GCdebt`, and `lua_tolstring` may reallocate the stack through `luaC_checkGC`; `inout` is "one object" (`SPECS.md` section 4), so facts about the graph behind `L` (that `g` is distinct from `block`, what the call may modify) have no a1 form and fall to `raw`; a2 would need a user predicate for `lua_State` (M3). (3) **`borrow` tied to non-object state**: the string `lua_tolstring` returns stays valid while the value sits in its stack slot, not for as long as `L` lives, so `borrow(L)` is the wrong bound. Consequence (inference, three functions): a1 covers leaf utilities like zstd's; Lua-style code over a state graph needs a per-type invariant (which object a pointer field refers to, and that it is distinct from what the callee is given), under either a1 or a2. That is the same missing piece as the lazy-blob constructor parked in `PARKED.md` (row 254), so one type-invariant concept would serve both; not designed.

## 6. Risks

- The havoc rule per operation is the whole soundness story; the table needs a parity test per entry (concrete rule against symbolic rule on concrete inputs).
- `symx` is 415 lines of QF_BV; T3 as a compile-time default needs the extensions above and a Windows-native solver path.
- The prover core is thousands of lines (inference); this pass does not estimate it beyond the spikes.
- The goal-to-witness hand-off (`SPECS.md` 7b) and the VC-generator trust (7b item 4) are untouched; d1 fixes only the key scheme.
