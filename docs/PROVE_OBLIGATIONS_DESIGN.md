---
status: active
---

# Obligation generator and `--prove` (stage 3b of `PROVER_DESIGN.md`)

Drafted 2026-11-15, revised after review round 1 (two reviewers; findings in section 11). Decided 2026-11-15: t1 (user-written invariants only, no inference), u1 (a new module `src/c/prove_vc.c` over `Cfg`), v1 (one array per pointer parameter), d1 (three dedicated spec ops), e3 (cut points from layout order), y1 (consumer table first). Decisions are in section 9. This document is how a function's `Cfg` plus its `#pragma cereal` specs (stage 3a, `SPEC_DIRECTIVES_DESIGN.md`) become solver goals and a per-obligation report. Syntax and the spec language are `SPECS.md` sections 3 and 6; the memory model is `PROVER_DESIGN.md` decision A (a1); the solver layer is `prove_smt.h`. None of that is restated.

## 1. What exists (read 2026-11-15)

- `tast_cfg` (`tcfg.c:869`) builds one `Cfg` per function (`tcfg_int.h`): blocks of statement nodes (`st`), a terminator (`jmp`, `br`, `swtch`, `ijmp`, `ret`) and a value node. Expressions that hide control flow (`&&`, `||`, `?:`, statement expressions) stay inside their block. After the build it calls the consumers: `mark`, `eval_keep` (`--eval`), `x86_func` (asm), the dump and `check` (`--verify-cfg`).
- Specs are `TO_SPEC` statements (aux: requires/ensures/invariant, one expression kid). `tcfg` pushes them as ordinary block statements (`tcfg.c:417-420`); `eval.c:1966` and `cgx86.c:1955` skip them by case, so those consumers do receive the spec's kid nodes. Contracts lead the entry block; an invariant leads its loop's head block (`do`, `for(;;)`) or body block (`while`, `for`), per `tests/cfg/spec.expected`. `==>` is already `!a || b`.
- `return`, `$len(p)` and slices parse and check (`N_SPEC_RESULT`, `N_SPEC_LEN`, `N_SLICE`; `cexpr.c:411-413`) but tast does not lower them: the function is "unsupported" (`PARKED.md`, "Spec directives"; golden `tests/cfg/spec`). The checker requires a slice's parent to be a bare `==` or `!=` (`cspecform.c`), and nothing about the other operand.
- `TO_INTRIN` is not free-form: `intrin()` (`tast.c:75-80`) finds a row of `cintrin.h` by name, and the verifier (`tast.c:2147-2175`) and the dump index that row; `$result` and friends have none.
- A comparison node's `ty` is its result type (`cmp >= #1 #2 int` in the dump); the operand types are the kids'.
- `prove_smt.[ch]` has Bool, 8- and 64-bit bit-vectors, Int, byte arrays (64-bit index, 8-bit element), `forall`, substitution, the BV-to-Int rewrite (64-bit only, `prove_smt.c:100-103`) and `ps_valid` over one z3 pipe. It has no signed compare, subtraction, resize, element-typed arrays or model extraction.
- The S6/S7 spikes (`docs/spikes/prover/s6.py`) are the working reference: text-level over the dump, 64-bit values, hand-written specs, Houdini. This stage replaces the text parsing with the node graph, the hand-written specs with the pragmas, and drops Houdini (t1).

## 2. Scope: what a function must be for `--prove` to say anything

A function is analysed only if every statement and expression in it is in the subset below; otherwise the report has one line `unsupported: <why>` for the function (the same discipline as `tcfg`'s `why`) and no goals. Never a partial analysis that skips a statement.

- Parameters: integers of 8 to 64 bits (signed or unsigned, `_Bool`), and pointers to such integers. Each pointer parameter is its own allocation (v1). **A parameter is never assigned**, so a spec and the code see its entry value.
- Locals: integer scalars of those types, no address taken, assigned by `=`, `++`, `--`, compound assignment of `+ - *`. No pointer locals.
- Expressions: `+ - *`, comparisons, `! && ||`, casts between integer types, constants, `?:` of pure operands, and `p[i]` / `*(p + i)` where the pointer is a parameter plus an integer, consumed directly by a load or store. Anything else (`/ % << >> & | ^ ~`, floats, calls, structs, `&x`, `p - q`, a pointer that is stored, compared or passed, function pointers, `switch`, `goto` outside a loop's own jumps) is unsupported with the node's op named. None of those is needed by the copy loop; each is a separate rule.
- Control: `if`, `while`, `do`, `for`, `break`, `continue`, `return`.
- Specs: C expressions over parameters and locals in the subset above, `return` (an `ensures` only), `$len(p)` of a pointer parameter, and a slice equality `p[lo:hi] == q[lo2:hi2]` (or `!=`; the other operand must itself be a slice, else unsupported). No load `p[i]` in a spec (a spec's loads have no uniform meaning under `!`, `||` and `==>`; slices carry their own bounds, section 3). Spec `+` and `-` are allowed only on operands of at most 32 bits, at most three operators deep, and are computed exactly (each operand widened to a signed 64-bit value first, so an unsigned `a - b` may be negative; three 33-bit sums cannot overflow 64 bits); spec `*` and any 64-bit operand are unsupported (d1 says a spec reads arithmetic as Int, which needs stage 4's no-wrap goals). A pointer-valued `return p` in the code is accepted and its value unconstrained: `return` in an `ensures` is integer-only. Slices over different element types are unsupported. Comparisons in a spec follow the C conversions, as in code.

## 3. Translation (`prove_vc.c`)

**Values.** Integers are bit-vectors of their own C width (8, 16, 32, 64; `_Bool` 8), arithmetic wraps. Signedness is read from the operand kids' types, not from the comparison's result type: `bvslt`/`bvsle` for signed, `bvult`/`bvule` for unsigned, after the checker's conversions (already explicit `TO_CAST` nodes, so a mixed `int < unsigned long` compares at 64 bits after a sign extension). A pointer is just its offset in elements (a bit-vector of 64 bits); the allocation is known at translation time (the parameter it walks from), so there is no tag. Booleans are Bool terms, converted at `br`, `!`, `&&`, `||`.

**State.** `env`: local symbol ref to value; `mem`: per allocation, an array term indexed by element holding element-width bit-vectors; `facts`: the path condition. A load is one select and a store one store. `$len(p)` is a fresh 64-bit variable `len_p` (elements), a free input.

**Bounds goals.** A load or store at `(a, idx)` yields `idx <u len_a`, with `idx` extended to 64 bits from its own signedness first, so a negative index is out of bounds. Once checked, the access is assumed in bounds for the rest of its path (the report's line for a later goal is "given the earlier accesses"), which stops one missing fact from failing every later goal. This is all of t1's code-side goals; no other UB class gets a goal (signed overflow wraps and the result holds provided none occurs, section 5). An uninitialised local is a fresh variable.

**Spec goals.** `requires`: assumed at entry. `ensures`: a goal at each `ret`, with `return` bound to the returned value. `invariant`: several of one kind are one conjunction; per loop head h, a goal on each edge into h from outside the loop (established) and on each back edge (preserved), and an assumption in the havoced head state. A loop with no invariant has the invariant `true`, so its bounds goals usually fail (t1: a missing invariant is a user error, not inferred). An invariant means "holds at the head", so it is evaluated in the head's state (a snapshot taken at the head block's first statement), not at its textual position, where the condition's side effects may already have run. At a head, hypotheses are `requires`, the invariants, and every path fact whose variables the loop does not assign.

**Slice equality** `a[lo:hi] == b[lo2:hi2]` is `B && E`, with `B = lo <=u hi && hi <=u len_a && lo2 <=u hi2 && hi2 <=u len_b && hi - lo == hi2 - lo2` and `E = forall k <u hi - lo: a[lo + k] == b[lo2 + k]`, every bound first extended to 64 bits from its own signedness (a negative `int` bound becomes huge and fails `B`: the checker inserts no cast, `e_slice` only requires an integer). `==` is `B && E`; `!=` is `B && !E` (not `!(B && E)`, which an out-of-range slice would satisfy). Out of range is therefore false for `==` and `!=` alike, with no polarity rule. This is the only quantifier the generator emits.

**Loops.** Decision e3: no loop construct is read. A cut point is the target of a retreating edge (an edge to a block whose `pos` is at most the source's), which every cycle contains; an edge into a cut point c from c's strongly connected component is a back edge, any other an entry edge; a spec belongs to the cut point with the smallest component containing the spec's block. At a cut point the state is replaced by a fresh one: every local assigned in the loop gets a fresh variable and the array of every allocation stored to in the loop gets a fresh array variable. Acyclic regions between cut points (entry, heads, `ret`) are executed forward; a `br` forks the state with its condition on each side, and the goals of one source position are the conjunction over paths of `path => goal`, reported once. A load or store in the right operand of `&&`, `||` or `?:` carries the left operand's value (or its negation) as a hypothesis. The path cap is 256 per region (counted at forking `br`s); beyond it the function is unsupported (joins with `ite` are parked).

**Solver.** One `ps_valid` per goal, hypotheses = `requires`, path, head invariants. Built 2026-11-15: the `int *` copy loop was the risk from S6 and it is real in cost, not in kind: z3 proves the "invariant preserved" goal over 4-byte elements, but needs 5.5 s and 50M resource units against 0.1 s for bytes, so `RLIMIT` is 100M and each query starts from `(reset)` (incremental mode after a push answered `unknown` on such queries); the instantiation tier stays parked (PARKED.md). The Int tier does not apply to values narrower than 64 bits (`ps_to_int` reads 64-bit only), so a nonlinear bound on `int` indices is `unknown` here; stage 4 extends `ps_to_int` to the resize nodes (parked, with the `fill2` shape as its check).

## 4. Lowering of `return`, `$len` and slices (tast; first slice)

Decision d: three new tast operations, `TO_RESULT` (typed as `El.ret`), `TO_LEN` (one pointer kid, typed as the checker typed `$len`) and `TO_SLICEEQ` (six kids `p lo hi q lo2 hi2`, Bool-typed; `!=` becomes `!` of it), each with verifier and dump cases. They appear only under a `TO_SPEC`; `tcfg` carries them verbatim, `eval` and `cgx86` never read a spec's kid (they skip `TO_SPEC` by case), and the verifier checks they occur nowhere else. A slice whose other operand is not a slice is `tunsup` ("a slice is compared with a slice"). `PARKED.md`'s row is deleted when this lands.

## 5. Report

`cereal check --prove f.c` prints, per function, to stdout:

```
copy: assuming pointer parameters are distinct allocations; proved provided no signed arithmetic overflows
  f.c:5:5   proved        bounds read src[i]
  f.c:5:5   proved        bounds write dst[i]
  f.c:4:5   proved        invariant holds on entry
  f.c:4:5   proved        invariant preserved
  f.c:8:1   proved        ensures dst[0:n] == src[0:n]
copy: 5 proved, 0 not proved, 0 unsupported
```

Status: `proved`; `not proved (sat)` (a counterexample exists in the abstraction: a missing invariant or a real bug, not told apart until stage 5's replay); `not proved (unknown)` (timeout or solver `unknown`); `unsupported: why` (function level). Exit status 1 if any obligation is not proved or any function is unsupported, 0 otherwise, so the mutation tests can be goldens. Models are not printed.

## 6. Checks

New tests in `tests/prove/` (need z3; run by `tests/run.sh` only when z3 is on `PATH`, like `selftest prove`). Goldens:

(Built 2026-11-15 as slice 4a, see section 7; items 5 to 7 are partly built: `mixed_sign` is the sign case, `straight` and `unsup_ternary` stand in for the rest, the remaining shapes are parked.)

All of 1 to 4 use `size_t i, n` (unsigned: a signed `i` admits negatives under a bare `i <= n`; item 5 covers signed).
1. `copy_ok`: `char *copy(char *dst, const char *src, size_t n)` with `requires n <= $len(dst) && n <= $len(src)`, `invariant i <= n && dst[0:i] == src[0:i]`, `ensures dst[0:n] == src[0:n]`, `return dst`: all proved. Also a variant over `int *` (4-byte elements), where the risk in section 3 shows if it is real.
2. `copy_nodstlen`: `requires` without `n <= $len(dst)`: the write bound is `not proved (sat)`; because a checked access is assumed afterwards, "invariant preserved" and `ensures` stay `proved` only if they do not need the missing fact: the golden records what the engine says, and the expected set (write bound not proved; invariant-preserved and ensures not proved too, since `dst[0:i+1]` needs `i + 1 <= len_dst`) is checked by hand first.
3. `copy_noinv`: no invariant: the bounds goals and the `ensures` are not proved.
4. `copy_weak`: `invariant i <= n` only: the `ensures` is not proved, the bounds `proved`.
5. `mixed_sign`: an `int i` against `unsigned long n` loop with `invariant 0 <= i && i <= n` (a sign extension decides a bound), a `signed char` index (negative means out of bounds), and a slice with an `int` bound that can be negative.
6. `loops`: `while`, `do`, `for(;;)` and a `continue` (two back edges) each with an invariant: heads found, goals present.
7. `unsup_*`: one function per excluded construct (`/`, a call, `&x`, `p - q`, a pointer local, an assigned parameter, a load in a spec, 64-bit spec arithmetic) with its reason line, and exit status 1.
8. `tests/cfg/spec_result`: golden for section 4 (the old "unsupported" golden is regenerated), and a `--verify-cfg` plus x86 run on a function with all three new ops (no consumer walks a spec's kids).
9. `prove_selftest` cases for each new term op (section 7).

## 7. Slices (each ends in a check that can fail)

1. **Lowering** (section 4): `tast.c`, `tast_int.h` (three ops), verifier and dump cases, `tcfg.c` unchanged beyond carrying them. Check: item 8 above; `--verify-cfg` passes on all of `tests/cfg`.
2. **Consumer table** (decision y1, its own commit, before slice 4): `tast_cfg` builds the `Cfg`, then runs each enabled consumer from a static list. Check: the full suite and the gate, unchanged.
3. **Solver ops** (built 2026-11-15; `PS_SIGNED` flag in `k`, array element width in the sort's `w`) (`prove_smt`): `PS_BVSUB`, `PS_BVSLT`, `PS_BVSLE`, one `PS_RESIZE` (k: target width, a sign flag), widths 8, 16, 32 and 64, and arrays whose element width is a parameter of the sort. `ps_to_int` returns 0 for them. Check: `selftest prove` cases, including a sign-extension mutant and a 32-bit wrap.
4. **Generator and `--prove`** (`prove_vc.c`, `main.c`, `check.h`, the `tast_cfg` call per decision y): sections 3 and 5. Check: the `tests/prove` goldens.
   **4a built 2026-11-15** (`prove_vc.c`, `--prove`, `CHK_PROVE_ONLY` in `check.h` hides the lowering banner and unit headers for a prove-only run). Deviations from sections 3 and 5, decided while building: goals are reported per spec (one `invariant`/`ensures` pragma is one goal, not one conjunction); a cut point havocs every local and every stored array, not only those assigned in the loop; loops are natural loops of retreating edges (an irreducible graph is unsupported); `?:` and comparisons used as values are unsupported (a `?:` lowers to a store to a reg: parked); path facts holding before a loop are not carried into its region (only `requires` and the loop's invariants are), so an invariant must restate what the body needs. Goldens: `tests/prove/*.expected` (stdout, stderr and `exit N`), run by `tests/run.sh` when z3 is on `PATH`.

## 8. End-state fit

| Change | Target | Replaces | Deletes |
|---|---|---|---|
| spec ops lowered | `tast.c`, `tast_int.h` | the "unsupported: expression SPEC_RESULT" path | the `PARKED.md` "Spec directives" row; the golden's unsupported text |
| cut points and loop bodies (e3) | `prove_vc.c`, from layout order and strongly connected components | the "loop heads and back edges" part of `PROVER_DESIGN.md`'s tcfg-gaps row (amended) | nothing |
| consumer table (y1) | `tcfg.c` (`tast_cfg`) | the four inline consumer branches (`mark` stays inline: it always runs) | those branches |
| obligation generator | new `src/c/prove_vc.c`, `Cfg` in, `Ps` goals and a report out | `docs/spikes/prover/s6.py`'s generator (kept as the spike record, with a pointer) | nothing |
| solver ops | `prove_smt.[ch]` | nothing | nothing |
| `--prove` flag | `main.c`, `check.h` (`opt.prove`), one consumer in `tast_cfg` | nothing | nothing |
| tests | `tests/prove/`, `tests/run.sh` | nothing | nothing |

Tripwire: `tast_cfg` already has four optional consumers hung off it as inline branches (`eval_keep`, `x86_func`, dump, `check`), plus `mark`; `--prove` would be a fifth (built as table `cons[]` in `tcfg.c`, 2026-11-15). Decision y.

## 9. Decisions

**Decide d (spec ops), decided 2026-11-15: d1 (three dedicated ops).** Searched: tried reading the three nodes as `TO_INTRIN` rows (no new ops) and as a prover-side side table keyed by the checker's nodes; the first fails the verifier, the second loses the tast/checker separation the prover already relies on.
- d1 **Three dedicated ops** (recommended): `TO_RESULT`, `TO_LEN`, `TO_SLICEEQ`, never past `tcfg`.
    - Pro: nothing for the back end to learn; the verifier can say they occur only under a spec; Con: three tags and verifier/dump cases.
- d2 **Rows in `cintrin.h`**: `$result`, `$len`, `$slice`.
    - Pro: no new tags; Con: back-end rows that no back end handles, and the verifier's "intrinsic row" logic must learn exceptions.

**Decide e (loop heads), decided 2026-11-15: e3.** Survey: What `tcfg.c:438-489` builds: `while`: head h (test), body x, back edges from the body end and every `continue` to h; spec leads x, not h. `do`: body x is the head (and holds the spec), test block h branches back to x, `continue` goes to h. `for`: h (test) or x (no test), a step block y that `continue` targets, one back edge y (or the body end) to h; `for(;;)` has h = x. So the head is not always the spec's block, and a loop has several back edges. Every placed block has a unique `pos` (layout order, `tcfg_int.h`), and a `goto` can add back edges the constructs do not know.
Searched: tried deleting the records (dominators, as S7), deleting the prover's body computation (tcfg records the body), and deleting the notion of "loop" down to the cycle structure of the graph (e3).
- e3 **Cut points from layout order, bodies from strongly connected components** (new, recommended): a retreating edge is an edge from a block to one with `pos` at most its own; every cycle contains one (positions are linear), so their targets are a sound cut set. Edges into a cut point c from inside c's strongly connected component (blocks reachable from c that reach c) are back edges (preserved), the others entry edges (established); c's component is exactly what its loop assigns, so it is also the havoc set, and a spec's owner is the cut point with the smallest component containing the spec's block. No change to `tcfg`, no dominators, no records; one SCC routine (about 40 lines, `src/mgraph.c` has a Tarjan but over a different graph, so not reused).
    - Pro: nothing added to `tcfg` or its dump; handles `goto`-made loops soundly (their cut points just have no spec, so the invariant is `true`); the nested-loop mis-cut S7 hit cannot arise (an edge into an inner head from the outer body is outside the inner component, so an entry edge); Con: the prover owns graph analysis beside `tcfg`'s structured knowledge, and a reader must see that "retreating in layout" is a proof device, not a source construct; an irreducible graph gets an over-large havoc set (sound, weak).
- e1 **`tcfg` records head and back-edge sources**: the constructs already know them.
    - Pro: the prover reads declared loops; `PROVER_DESIGN.md` section 4 lists it as a tcfg gap; Con: a record per construct (head, spec block, step, continue sources: four fields to keep in step with `lower`), a dump format and goldens, the body set still has to be computed by the prover, and `goto` loops are unsupported.
- e2 **The prover computes dominators** (as `s6.py`).
    - Pro: textbook natural loops; Con: a second notion of loop to keep consistent with the layout, and loops without a dominating head are not found.

Chosen e3: the loop-records slice and row are gone, and `PROVER_DESIGN.md`'s tcfg-gaps row says "loop heads: derived by the prover from layout order".

**Decide y (`tast_cfg` consumers), decided 2026-11-15: y1 (consumer table first, its own commit).** Searched: tried leaving the four inline `if`s and adding a fifth, and a consumer table.
- y1 **Consumer table first, as its own commit** (recommended): `tast_cfg` builds the `Cfg`, then calls each enabled consumer from a small static list; `--prove` is one entry.
    - Pro: the tripwire's rule (split before the third of a shape); Con: touches `eval_keep` and `x86_func`'s call sites, which are verified by the full suite and the gate.
- y2 **One more `if`.**
    - Pro: smallest diff; Con: a sixth special case in a dispatcher, against the tripwire.

My calls (change on a word): distinctness of pointer parameters is an assumption printed on the report's first line, not a goal and not new syntax (a `$disjoint` spec waits for stage 6's object table); signed overflow is assumed absent (printed likewise; a no-overflow goal class is parked with the integer rules); arrays are element-typed (no byte view, no `sizeof` factor); values keep their C width.

## 10. Parked by this design

(Added to `PARKED.md` in the commit that lands each slice.)

- Path joins (state merging with `ite`): trigger a function over 256 paths in a region.
- Pointer parameters that alias: trigger stage 6 (the object table) or a function whose proof depends on distinctness.
- 64-bit spec arithmetic; `ps_to_int` over narrower widths and resize nodes: trigger stage 4's Int tier (`fill2` with `int`).
- Signed-overflow and one-past-pointer goals: trigger the integer and pointer rules (`SPECS.md` UB list).
- Division, shifts, bit-wise operations, pointer locals and walks (`pcopy`), calls, loads in specs: each waits for its own rule (calls: contracts at call sites).
- Counterexample models and replay: stage 5. Inference (Houdini T1-T5): stage 4.

## 11. Review log

- Round 1 (2026-11-15, two Sonnet reviewers: soundness against the code, and a shape search for fewer concepts). Caught and fixed: `TO_INTRIN` is not free-form (verifier and dump index `cintrin.h` rows); the claim that `TO_SPEC` is dropped before eval and cgx86 was stale (they skip it by case); a comparison's `ty` is its result type, so signedness comes from the kids; "exact 64-bit spec arithmetic" was false for unsigned narrow types; slice bounds had no polarity rule and spec loads no uniform meaning (bounds are now conjuncts, loads are out of scope); the scope rules contradicted each other (`p - q`, pointer locals, assigned parameters); report gaps (unsupported function exit status, per-position goal dedup, the path cap counted per `br`); `ps_to_int`'s 64-bit-only reading makes the Int tier unreachable for narrower types (stated as a limit, parked); the solver ladder risk (stated, with the check that decides it). Adopted from the shape search: element-typed arrays (deletes byte extract, multi-byte loads, `sizeof` factor and the `2^40` fact), static allocation on pointer values (no tag), native widths (no normalisation rule), one `TO_SLICEEQ` instead of a `$slice` type, loop records in `tcfg` (as decision e), one "not proved" status with a reason. Not adopted: backward weakest-precondition (needs a spike on `copy` and `fillnz` first; recorded here, not parked), folding the lowering slice into the generator slice (a different file and golden that `--verify-cfg` can fail alone).
- Round 2 (one Sonnet reviewer). Caught and fixed: a checked access is now assumed in bounds afterwards (without it `copy_nodstlen`'s expected set was wrong); spec arithmetic exactness (`*` and chains out; `+ -` on at most 32 bits, three deep); slice bound widths (extended from their own signedness) and a mutant test; slices over different element types unsupported; `!=` as `B && !E` (was vacuously true out of range); the tests now state their types (`size_t i, n` where bounds must prove); pointer-valued `return p` accepted; head hypotheses keep path facts the loop does not assign; uninitialised locals are fresh variables; the invariant is read from a head-start snapshot; the report wording for signed overflow. Nothing material found on `&&`/`||` guards, `ensures` at several `ret`s, or the eval/cgx86 skip of `TO_SPEC`.
- Round 3: not run. The round 2 fixes are local edits; a third review is the user's call, or happens after their answers to section 9 change the shape.
