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
| `annot = verifast` lexer field | `CONFIG_DESIGN.md` 3.7, 7.4, 8 (S5); `ROADMAP.md:217-228` | a comment channel | dropped as a goal 2026-11-13 (Decide E); kept in those docs as a record |

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

### Decide A: the heap core (decided 2026-11-13: a1)

Searched: I tried deleting the chunk engine (object facts per allocation, with modes only for
parameter aliasing and `#modifies` for frames) and deleting the permission layer (all formulas,
`SPECS.md` M1). The first removes chunks, `consume`/`produce` and open/close, and fails only on
recursive and functional shapes, which are parked as M3 anyway. The second loses the linear-time
aliasing pass. `SPECS.md` calls M1 "far slower", but that cost is about aliasing, not provenance,
and was measured once (S3 result: M1 returned `unknown` on one obligation of one copy loop, which may be an artifact of my encoding). Removed: nothing yet; a1 removes the chunk concept.

- a1 **Object facts, modes for parameter aliasing, chunks parked** (recommended): bounds, initialization and liveness are facts per allocation over the evaluator's own object table; `in`/`out`/`inout` say which parameters may alias; `#modifies` is the frame. Chunk-style predicates (M3) are a later, separate addition.
    - Pro: fewest concepts; no heap-matching engine; fits the UB-elimination product. Con: no route to VeriFast-style functional proofs of lists and trees until M3; the "not slow" claim is measured only on one copy loop (S3 result below); S5 found no mode without a reading, but nullable pointers, nested reach and non-object `borrow` bounds are inexpressible (see Results).
- a2 **Chunk core, modes as a fragment**: one symbolic heap of chunks, the modes desugar to chunk shapes where they can, `raw`, `borrow` and the `$`-facts handled beside it, M3 adds user kinds.
    - Pro: the VeriFast-style end state with one engine. Con: builds `consume`/`produce` and matching first, and gives recursive shapes a home only by adding a second layer of user chunk kinds.
- a3 **Modes only, no heap model** (`SPECS.md` fork 1 as written): T2 as a dataflow pass.
    - Pro: smallest first build. Con: M3 arrives later as a second mechanism; and it cannot be combined with c1, whose single entailment engine replaces the separate T2 pass.

Coupling: c1 presupposes a1 or a2. With symbolic extents (`out[n]`, `p[lo:hi]`) neither a1 nor a2 keeps T2 as `SPECS.md` section 2's linear dataflow pass: the check is fuel-bounded entailment, cheap only when the facts are syntactically equal. Formulas only (M1) is not offered as an option; it is the measured baseline in S3 (`SPECS.md` section 4 calls it "far slower").

### Decide B: where the engine lives (decided 2026-11-13: b1)

Reaffirmed 2026-11-13 after the review: b1 stays. Considered and not taken: start with the prover's own rules over `symx` and park the shared table (less work now, but duplicated rules and the parity risk in section 6, and it contradicts the end state). Open: whether the table can hold the memory half (S2 did not show it).

The engine runs the CFG symbolically with havoc, forking, loop invariants and summaries.

Searched: I tried deleting the engine's own operation semantics by making the evaluator the prover
run symbolic-first (a concrete value is a constant-folded expression, so one rule per operation
serves both), with a mode flag that turns any concretizing site into a failure. That removes the
duplicated per-operation rules and the shadow cases; what remains is its hard half: pointer and
memory operations are not symbolic in the evaluator at all (only integers carry shadows), so
S2 read that half and found it needs a fork or havoc rule per operation (not built). I also tried translating the CFG to an external verifier (Why3,
Boogie), and abstract interpretation first with the prover only for what falls through.

- b1 **Own module over the CFG, with one operation-rule table shared with the evaluator** (recommended): a module takes `tcfg` blocks, owns path states and the heap, shares `symx` (extended) and the integer rule library (`ia_binop`), and moves the per-operation concrete and symbolic rules into one table the evaluator also reads.
    - Pro: soundness direction is explicit in the new module; the table is additive and can later be replaced by b2's constant folding, at the price of deleting a table that by then holds rules for every operation. Con: the table is a refactor of `ev_node1` (its own commit); its integer half is cheap, the pointer and memory half needs fork or havoc rules per operation (S2, by reading), and S2 did not show the shared table can hold them: that is open.
- b2 **The evaluator run symbolic-first with havoc** (the unified end state; rejected for now by S2, see Results): a concrete value is a constant-folded expression; havoc replaces concretization; a "prover mode" makes any concretizing site fail to compile or run; the path-recording driver (`run_fn`) already forks.
    - Pro: one definition of every C operation, the shadow cases and the table both disappear. Con: pins and host libc functions (`eval_libc.c`) under-approximate and each needs a havoc or a contract; sweep speed may fall; the evaluator takes on both directions of approximation.
- b3 **External verifier backend** (Why3 or Boogie via a CFG translator; `SPECS.md` fork 12 already allows untrusted engines).
    - Pro: no engine to build. Con: C memory semantics and UB classes live in the translator, which is then the trusted base (`SPECS.md` 7b item 4); no shared counterexample path.
Also considered, not options: abstract interpretation first (`SPECS.md` T0/T1) is a stage inside c1's ladder, not an engine; bounded symbolic execution without havoc is a bug finder that the concolic sweep already approximates and proves nothing.

### Decide C: tiers as ladder or as separate passes (decided 2026-11-13: c1)

Searched: I tried deleting the four tiers (T0 syntactic, T1 abstract interpretation, T2 permission
check, T3 solver) as separate passes in favor of one `entails(facts, goal, fuel)`: T0 is the
constant folding the DAG needs anyway, T1 interval and congruence reasoning over the same facts,
T2 the object-fact check, T3 the SMT call. Removed: three passes and their interfaces.

- c1 **One entailment engine with a fuel ladder** (recommended): the parameter is fuel, which S3 found needs calibrating on a baseline goal and is z3-version specific (determinism holds per solver build), and the engine keeps scalar and quantified facts apart; summaries inferred by T1 and invariants supplied by users are both facts the same engine checks.
    - Pro: one interface, determinism from fuel (`SPECS.md` 7; 7b needs it only of the checker). Con: widening must emit an invariant and the same engine must check it; spike S3 ran it once on the copy loop (Results).
- c2 **Four passes as in `SPECS.md` section 2**.
    - Pro: each pass can be built and shipped alone. Con: four interfaces, and facts convert between them.

### Decide D: what the evaluator is for, and the witness (decided 2026-11-13: d1)

Searched: I tried deleting the replay seam (models reported as text) and deleting the shared
witness file (a model file beside a certificate file). The first loses the executable counterexample
`SPECS.md` section 2 promises and costs nothing to rebuild later; the second keeps two keying
schemes for artifacts that are keyed by the same goal identity. Removed: the second keying scheme.

- d1 **Replay is a bug-finding oracle; one witness file holds a model or a certificate** (recommended): a failed goal's model becomes a generated driver (allocations of the model's sizes, then the call; S4 follow-up: lazy-blob force keys cannot carry sizes) that the evaluator runs; a trap at the goal's site confirms a bug, and no trap leaves the goal unconfirmed (spurious, unreachable, too weak, or beyond the replay's size cap; S4 capped sizes at 64), never "holds" and never proof of spuriousness. Models and certificates share the stable goal key of `SPECS.md` 7b item 3 and one file. The prover never trusts the evaluator for a proof. Contracts also become runtime assertions in a sweep (needs the spec parser).
    - Pro: reuses `sweep_call` and seeds; one keying scheme. Con: the map covers input bytes (parameters, lazy blobs), not havoc symbols or heap shapes the generator never builds, so havoc needs symbol identities distinct from `SX_SYM` input offsets.
- d2 **Replay seam only, separate model and certificate files**: models keyed apart from certificates.
    - Pro: no change to the `SPECS.md` 7b file format. Con: two keying schemes for one goal identity.

Not pursued: observed ranges as invariant candidates (observed, not proved; a separate design if wanted).

### Decide E: annotation syntax (decided 2026-11-13: e1)

Searched: I tried deleting the annotation front end altogether (no `//@`, no `annot` field) and
deleting the directive syntax in favor of `//@`. The first removes `annot = verifast`, its overlay
atom and a lexer channel; the second drops macro expansion of specs and `#if $spec`, which
`SPECS.md` section 1 requires. Removed: the lexer annotation channel as a goal.

- e1 **`SPECS.md` directives only** (recommended): macro-expandable pp-directives and `#if $spec` blocks; `annot = verifast` is dropped as a goal (sites listed in section 4).
    - Pro: one spelling. Con: VeriFast sources cannot be read directly.
- e2 **Also support `//@` as a second front end** over the same contract IR.
    - Pro: reuses VeriFast annotations. Con: a second parser for a spec language whose semantics differ (predicates, lemmas, ghost commands); not pursued unless a corpus needs it.

### Decide F: first integration (decided 2026-11-13: f1)

Searched: I tried deleting the first-surface question by integrating everywhere at once; that makes a prover bug an editor lie (language server) or a miscompile (code generation) before the soundness story exists. Removed: nothing; the order is the decision.

- f1 **Command line first** (chosen): a `--prove` mode that reports per-obligation status and replays failures through the evaluator (decide D). No editor feedback and no check elision yet.
    - Pro: smallest surface; exercises the replay seam. Con: no visible payoff for editor users.
- f2 **Language server first**: obligation status as diagnostics from a background phase.
    - Pro: the visible payoff. Con: needs background-phase and cancellation work in the server first.
- f3 **Code generation first**: elide runtime checks for proved obligations.
    - Pro: a measurable payoff. Con: a prover bug becomes a miscompile.

Where the prover also fits later, in order: language-server diagnostics (a third source beside the macro and compiler phases), cache keys on the unit summaries and read sets (`csum.h`) by function-contract hash, a prover test list beside `tests/eval.sh` with "the evaluator traps where a proof fails" as the oracle, open obligations in `cereal audit`, and check elision in `cgx86.c` last. It runs beside the evaluator, not inside the sweep loop (the rejected b2).

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

- S1 **Obligations from existing stops**: classify the 50 `TRAP` sites and the non-`TRAP` stops (`CONCOLIC_DESIGN.md` section 10.1's census: unevaluated intrinsics, inline asm, missing callees) as node-local or state-dependent (needs the heap model); list the UB classes `SPECS.md` names that nothing covers. Kill: if fewer than 60% are node-local, the obligation generator is a heap-model client, not a per-node pass, and a1's "facts per allocation" must carry them.
- S2 **Operation table, both halves**: integer subset first (check unchanged results on the eval test list), then loads, stores, `TO_PTRADD`, casts and calls. Kill: if more than a third of the memory and pointer rules need a havoc form with no constant-folding reading, b2 is rejected and b1 stays.
- S3 **Tiny prover**: `int` and two array parameters (a copy loop), `requires`/`ensures`, one invariant, the single entailment engine with fuel, object facts (a1) against a formulas-only baseline (M1). Kill: a1 more than 2x slower than M1 on the two-array case, or widening failing to emit a checkable invariant on the copy loop, rejects a1 or c1 respectively.
- S4 **Replay**: take failed S3 goals (a deliberately wrong bound and a deliberately weak invariant) and run their models through `sweep_call`. Kill: the wrong-bound goal must trap at the goal's site; if it does not, the model-to-call map is not usable and d1 shrinks to d2's text. The weak-invariant goal must be classified spurious, never reported as a bug.
- S5 **Desugar the modes**: `out`/`in`/`inout`/`own`/`raw` to parameter-aliasing facts (a1) and to chunk shapes (a2) on three functions. Kill: if more than one of the five modes has no a1 reading, a1 is not the surface `SPECS.md` section 4 promises.

### Results (2026-11-13)

- **S1 ran** (Sonnet agent, table of 99 rows kept in the session scratchpad, not in the repo; counts re-checked by `cut | uniq -c` on that table and `grep -c "TRAP("`). Of the 50 `TRAP` sites, 18 are node-local, 28 state-dependent, 4 not UB (host limits, `abort`); all 49 non-`TRAP` stops (`UNSUP`, fuel, exit) are not UB. Node-local share among the 46 UB sites: 39% (57% if the 8 printf-format sites count, 65% with function-pointer and label derefs). **The kill criterion (under 60%) is met on the strict classification only** (one agent's labelling, 39%; 57% and 65% under the looser ones, and the 99-row table is not in the repo, so it cannot be rechecked): the obligation generator is a heap-model client, not a per-node pass. This fits a1: every bounds, lifetime, initialization and free check is one object-table lookup, so "facts per allocation" has a direct reading. No trap or stop exists for alignment, effective type, `restrict`, unsequenced side effects, arithmetic past one-past, sub-object bounds, indeterminate pointers, `va_arg` mismatch, VLA size or data races; the evaluator's traps under-cover `SPECS.md`'s list, so they cannot be the obligation list. Node-local sites: integer rules (division, shifts, signed overflow, `abs` of the minimum; overflow and shift-of-negative trap only when `!v->o.wrap`), float-to-int range, `clz`/`ctz` of zero, `__builtin_unreachable`, null deref and call, pointer compare or difference across provenances, `memcpy` overlap.
- **S2 partly run, by reading, not building.** (1) The integer half is already a table in miniature: `bin_op` (C operator to `SX_*`) and `op_sh` ([eval.c](../src/c/eval.c) near 1650-1690) beside the concrete `int_arith`; what b2 would need for integers is a concrete evaluator for `symx` nodes, and `symx.c` has none. (2) Of the 31 cases of `ev_node1`, 13 set a shadow (`TO_ADDR`, `BINOP`, `CAST`, `CMP`, `COMMA`, `FIELD`, `LOAD`, `POST`, `PTRADD`, `PTRDIFF`, `RFIELD`, `STORE`, `UNOP`); the memory and pointer cases (`LOAD`, `STORE`, `POST`, `FIELD`, `RFIELD`, `PTRADD`, `PTRDIFF`, `DEREF`, `ADDR`) either concretize a symbolic address through one of six pin kinds (`PK_LOAD`, `STORE`, `FIELD`, `INDEX`, `SHDIV`, `GOTO`) or have no symbolic pointer at all (object ids are concrete). Under b2 each of those needs a fork or havoc rule, which is the whole memory half, so the kill criterion (more than a third of memory and pointer rules with no constant-folding reading) is met: **b2 is rejected for now and b1 stays.** The table itself was not built or run against the eval tests; that, and the count of floats and host calls needing havoc, remain open.
- **S5 ran, on paper** (three real functions: zstd `ZSTD_copy8`, Lua 5.4 `luaM_free_` in `lmem.c`, `lua_tolstring` in `lapi.c`; corpus copies under `~/corpus`, no code run). Literal kill criterion (more than one of the five modes with no a1 reading) is **not met**: each mode has an a1 reading where it applies (`out[8]` and `in[8]` for `ZSTD_copy8`: allocation live, size at least 8, `dst` uninitialized on entry and initialized after, `src` initialized, the two allocations distinct; `own[osize]` for `luaM_free_`'s `block`: liveness ends; `raw` for the call through `g->frealloc`, a function pointer with no contract). The exercise found three gaps the modes cannot express, and they bite a2 as well: (1) **nullable pointers**: `block` is `own` only when `osize != 0` (`luaM_free_`'s assert ties them), and `len` in `lua_tolstring` is `out` or NULL; modes have no "or NULL" form (a chunk reading needs a disjunction). (2) **Nested reach**: `luaM_free_` reaches `g` through `L`'s `l_G` and updates `g->GCdebt`, and `lua_tolstring` may reallocate the stack through `luaC_checkGC`; `inout` is "one object" (`SPECS.md` section 4), so facts about the graph behind `L` (that `g` is distinct from `block`, what the call may modify) have no a1 form and fall to `raw`; a2 would need a user predicate for `lua_State` (M3). (3) **`borrow` tied to non-object state**: the string `lua_tolstring` returns stays valid while the value sits in its stack slot, not for as long as `L` lives, so `borrow(L)` is the wrong bound. Consequence (inference, three functions): a1 covers leaf utilities like zstd's; Lua-style code over a state graph needs a per-type invariant (which object a pointer field refers to, and that it is distinct from what the callee is given), under either a1 or a2. That is the same missing piece as the lazy-blob constructor parked in `PARKED.md` (row 254), so one type-invariant concept would serve both; not designed.
- **S3 partly run** (throwaway Python script over `cereal check --dump-cfg` of the copy loop `for (i = 0; i < n; i++) dst[i] = src[i];` with `n <= szD`, `n <= szS`, invariant `i <= n && forall k < i: D[k] == S[k]` written by hand; z3 4.13.3, one process per goal; script kept in the session scratchpad, not the repo). **a1 (one array per allocation) proves all five obligations (entry, read bound, write bound, invariant preserved, postcondition) in 0.25-0.4 s total; M1 (one array, base addresses, disjointness as a hypothesis) proves four and returns `unknown` on "invariant preserved"** (0.45 s). The a1-over-2x-slower kill is not met (a1 is no slower and M1 fails outright); this is one function and one invariant, and M1 is my own encoding (one array, base addresses), so "M1 fails" may be an artifact of it; the result shows a1 adequate on the copy loop, not that a1 is faster. Mutations behave: dropping the `n <= szD` fact makes the write bound fail, and an invariant with `<=` in place of `<` makes preservation fail. Two findings for c1: (1) `rlimit` counts are far larger than the ladder assumed: a three-line 64-bit `bvult` goal costs about 45,000, so a ladder starting at 1000 proves nothing but the trivial entry goal; fuel must be calibrated on a baseline goal (inference from one query). (2) **Slicing pays**: dropping the quantified hypotheses from goals that do not mention a quantifier proves both bounds in 0.03 s against 0.09 s, and the quantified invariant must be kept as separate conjuncts for this to work, so a prover keeps scalar and quantified facts apart. **Widening (Houdini) ran**: six candidates built from the loop condition `i < n` and the postcondition (`i<=n`, `i<n`, `i==0`, `forall k<i`, `forall k<=i`, `forall k<n`, the last three by replacing the bound), pruned by entry and preservation checks in two rounds and 12 queries (0.26 s); the survivors `i<=n` and `forall k<i` prove the postcondition on a1. **The c1 kill criterion (widening fails to emit a checkable invariant on the copy loop) is not met**; the candidate templates were chosen with the answer in view (the sample is one loop), so this shows the engine can check and prune candidates, not that generation finds invariants in general. On M1 the same pruning leaves only `i<=n` and the postcondition fails, in line with its failed preservation proof.
- **S4 ran, by hand** (a hand-written driver, not the `sweep_call` force log). The wrong-bound goal (write bound without `n <= szD`) is `sat`; z3's default model uses sizes near 2^64, which no call can allocate, so replay needs a size cap on the query (`< 64` on every size and index); with it the model is `n=33, szD=1, szS=48` (loop-head index 32). Run as `char d[1], s[48]; copy(d, s, 33)` under `--eval-sweep`, the evaluator traps `write out of bounds (offset 1, n 1, size 1)` at the same write (the sweep runs `drv_bad` as a no-argument function; `--eval-sweep=N,seed`, not a replay of generated inputs); the same call with `d[48]` does not trap. **The kill criterion (the wrong-bound goal must trap at the goal's site) is met**, with two limits: the trap comes at index 1, the first violation, not at the model's index 32 (the loop-head state is a havoc, so a model fixes sizes, not the path), and the replay was a driver written by hand from the model, so the model-to-call map through `sweep_call` is unbuilt. **Follow-up (same day): that map cannot be built for sizes.** `sweep_call` draws parameter blobs lazily and a blob's size is the extent the call has touched (`LAZY_BLOBS_DESIGN.md`), so a size is not an input: 90 sweep calls of `copy` (`--eval-sweep=60,7`, the unsized parameters) gave 0 traps, with `n` free. Out-of-bounds on a parameter is unobservable through the force log; a replay needs allocations of the model's sizes, as in the hand driver, which means a generated driver (local arrays of the model's sizes, then the call) or a sweep that allocates from a contract's `out[n]` extents (needs the spec parser). **Driver generation then ran end to end** (same day, in the throwaway script, driver text templated for `copy`): from the capped z3 model of each failed bounds goal it wrote `malloc` of the model's sizes and the call, and the evaluator trapped at the matching access each time: dropping `n <= szD` gave `n=33, szD=1, szS=48` and `trap write out of bounds (offset 1, n 1, size 1)`; dropping `n <= szS` gave `n=33, szD=48, szS=1` and `trap read out of bounds (offset 1, n 1, size 1)`; the unmutated facts proved both bounds, so no driver was made. Two mutations on one loop; the generator is not general (the call shape and the model's variable names are hard-coded). The weak-invariant goal is not a UB site: its model (all sizes 0, memory arrays unprinted) has nothing to trap on, so replay finds no trap and the goal is classified spurious, as required; invariant goals need a different replay (check the invariant on the concrete run) or stay "unknown".

- **S6 ran** (gate spike; `docs/spikes/prover/`, script `s6.py`, which replaces the S3 script: a general verification-condition generator over the CFG dump with cut points at back-edge targets, a1 memory, and Houdini over templates fixed before the run). Four functions: `copy`, a bounded `blen` (`i < n && s[i] != 0`, read-only, returns `i`), `rev` (two indices, `j = n - i`) and `fillnz` (an `if` in the body). **All four prove** (bounds on every access, invariant, postcondition) in 0.3-0.6 s with 7-12 queries, and replay of every dropped-requires mutant found a bounds trap at the matching access (7 of 7; `blen` needed a non-zero fill, because the z3 model does not carry array contents and a zero buffer stops the loop at index 0, so drivers are tried with two fills). The gate (two or more of the three new loops failing) is **not met**, but two findings change the engine. (1) **z3 alone does not finish the inductive step of `blen`**: `forall k < i: s[k] != 0` and `s[i] != 0` imply the same for `i + 1`, and it gave no answer in over 120 s with no `rlimit`, though the same shape passes for `copy`. A quantifier-free tier fixes it: skolemize the goal's `forall`, replace each hypothesis `forall` by its instances at the skolem and every 64-bit constant (sound, instances are consequences), and fall back to the full query last. So c1's ladder is slice, instantiate, full, not fuel alone. (2) **`rev` needs a template the first run did not have**: T1 (`v <= b`) and T2 (postcondition with its bound replaced by the loop variable) leave only `i <= n`; adding T3 (`x + y == c` for two assigned locals and a scalar parameter, `T3=1`) gives `i + j == n` and a proof. T3 was added after seeing the failure, so the claim is that three generic templates cover these four loops, not that three were predicted. Limits: hand-written specs (`requires`, `ensures` as lambdas), 64-bit arithmetic with no 32-bit wrap, pointer provenance read syntactically from `p[i]`, no calls, and `fillnz`'s `if` is the only branching in a body (nested loops and the pointer walk: S7 below).
- **S7 ran** (same script, `p1.c` pointer walk, `p2.c` nested loop; script now finds natural loops by dominators, since treating every edge to a head as a back edge mis-cut the nested case). **Pointer walk `pcopy`** (`e = s + n; while (s < e) *d++ = *s++;`): **proves** in 0.45 s with a fourth generic template T4 (`x == y` for two loop-assigned locals, `T4=1`; again added after the failure); mutants replay (dropping `n <= szD` traps on the write at offset 1, dropping `n <= szS` on the read). **Nested loop `fill2`** (`d[i * c + j] = 0`, requires `r*c <= szD`): the loop invariants are found (`i <= r`; `j <= c` and the enclosing condition `i < r`, template T5 `T5=1`) and **the write bound now proves** (1.8 s, 13 queries) once an integer tier is added (below; `INT=0` switches it off and it fails again, so the encoding is what decides it); dropping the `r*c <= szD` or the `r <= 2^31` requires fact fails the bound again. Before that tier the bound was not proved: the goal `i*c + j < szD` is 64-bit nonlinear `bvmul`, and z3 alone gave no answer in 100 s (checked on the bare goal, no other facts). The same goal in unbounded integer arithmetic, with the range facts `r, c <= 2^31` (which make the products fit in 62 bits, so the two encodings agree), is `unsat` in 0.6 s. So the obstacle is the encoding, not the invariant. The integer tier (`int_tier` in `s6.py`) runs after the bit-vector tiers fail, on goals without `forall`: it rewrites `bvadd`, `bvmul` and the comparisons to Int and first proves that nothing wraps, from the mul-free hypotheses only (adds in bit-vectors, linear and fast; every mul, and every add over a mul, as an Int value below 2^64, one query each). Two sharp edges: that no-wrap proof reads the mul-free hypotheses' adds as non-wrapping only after proving it in bit-vectors, and Int-only queries needed a 25 times larger `rlimit` (400,000 gave `unknown` on a goal that takes 0.4 s with none), the same lesson as the 64-bit fuel finding. Only the quantifier-free goal shape is covered; a `forall` over products is not tried. Consequence for the solver layer (stage 2): emit products of range-bounded values in integer arithmetic guarded by a no-wrap side condition (or a lemma tier), and pick bit-vector or integer per term from the bounds. Also added in the engine: a facts-free tier first (nonlinear `requires` facts such as `r*c <= szD` make every query expensive, including ones that do not need them).

## 6. Risks

- The havoc rule per operation is the whole soundness story; the table needs a parity test per entry (concrete rule against symbolic rule on concrete inputs).
- `symx` is 415 lines of QF_BV; T3 as a compile-time default needs the extensions above and a Windows-native solver path.
- The prover core is thousands of lines (inference); this pass does not estimate it beyond the spikes.
- The goal-to-witness hand-off (`SPECS.md` 7b) and the VC-generator trust (7b item 4) are untouched; d1 fixes only the key scheme.

## 7. Build order after the spikes (proposed 2026-11-13, not decided)

Searched: tried dropping stages 1 and 5 (build the prover over the evaluator's table at once; build without replay). Stage 1's gate exists because every spike ran on one copy loop; replay stays because it is d1's soundness story. Removed: nothing.

What the spikes settle: a1 is enough for the copy loop (S3); the engine can check and prune candidates (S3); replay confirms a bounds bug when given real allocations (S4, a generated driver, not the sweep); obligations are a heap-model client (S1); modes read as a1 facts for leaf utilities only (S5). What they leave open: the memory half of b1's shared table (S2), whether Houdini candidates and a1 survive a second and third loop, and that `symx` has neither arrays nor quantifiers (415 lines of QF_BV), which S3 got from z3 directly.

Stages, each ending in a check that can fail:

1. **Gate spike S6** (ran 2026-11-13, gate not met: all four loops prove and replay; see Results; S7 then covered the pointer walk (proves) and a nested loop (proves, with an integer tier for the nonlinear bound).
2. **Solver layer** (built 2026-11-13, `src/c/prove_smt.[ch]`, check `cereal selftest prove`, run by `tests/run.sh` when z3 is on `PATH`): a prover-side sorted term DAG (Bool, BV64/BV8, Int, byte Array, `forall`), SMT-LIB printing, substitution, the BV-to-Int rewrite that returns its no-wrap obligations, and `ps_valid` / `ps_valid_int` over one `z3 -in -smt2` pipe (push/pop per query, variables declared once). Decided 2026-11-13: b1 (own DAG; `symx`'s DAG untouched), d1 (one pipe), c1 (bit-vectors first, then the Int tier with z3's no-wrap proof; calibrate "variable times variable goes to Int first" in stage 4). Deviation from the plan above: `symx` gained `sx_raw`, a raw round trip on its pipe, so the two clients share one pipe implementation instead of a second copy. Moved to stage 3: turning `tcfg` blocks into terms is the obligation generator itself, so the stage 2 check is the selftest (the copy bound and its dropped-fact mutant, `fill2`'s nonlinear bound through the Int tier with two mutants, the `x < x + 1` wrap case that the Int tier must refuse, a quantifier instance, array store/select), not regeneration of `s6.py`'s obligations. Not built yet: models from `sat` answers (stage 5 needs them for replay), a hash-consed DAG (terms print as trees).
3. **Obligation generator and `--prove`** (f1): back-edge loop heads derived from the CFG (no marker), bounds goals only, directives from `SPECS.md` (`requires`, `ensures`, `invariant`, extents). Check: the copy loop and its two mutations, per-obligation status.
4. **Engine**: slice, then the quantifier-free instantiation tier, then the full query, fuel calibrated on a baseline goal; candidate pruning with T1-T3. Check: S6's four functions and their seven mutants.
5. **Replay**: the generated driver (S4 follow-up). Check: every `sat` bounds goal of the test list traps at its site; a goal with no trap is reported "unconfirmed".
6. **Gate for b1's table**: before sharing rules with the evaluator, a spike that builds the memory half for three operations (`LOAD`, `STORE`, `PTRADD`) with parity tests. Failing keeps the prover's own rules and parks the table.

Not in this order (parked or later): lists and trees (M3), language-server diagnostics, cache keys, check elision, a type-invariant concept (`PARKED.md`, chunk-heap row).
