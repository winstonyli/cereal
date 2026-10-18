---
status: draft
---

# TAST stage T8: plan (lowering to a control-flow graph)

Follows `docs/TAST_T7_PLAN.md`. This is the old design stage T6
(`docs/TAST_DESIGN.md` sections 4 and 13), renumbered after T5 to T7 took
the other T-numbers; see the "Stage map as built". Facts are from the tree of
2026-10-18; anything unread is marked **verify**. Decisions in section 9
taken 2026-10-18 (form 1, T8a first, structured form kept); T8a approved.

## 1. Why, and what it must be

Every corpus function now elaborates (lua54 0 unsupported units, libuv 2
by decision). The structured TAST keeps `if/while/do/for/switch/goto` and the
expression forms with hidden control flow (`logand`, `logor`, `cond`, `elvis`,
`comma`, `post`, `stmtexpr`). Consumers need a graph:
- the planned back end (ROADMAP: typed-AST export, SysV classification, QBE):
  QBE input is basic blocks of three-address instructions;
- Track D dataflow warnings and symbolic execution/proving: a CFG per
  function.

The pass must be additive: the structured form stays the first stage and its
dump, verifier and goldens do not change (design section 4).

## 2. Output form: blocks of the existing trees

A lowered function is a list of **blocks**; each block is a run of existing
nodes in post-order (kids before parent, each used once) followed by exactly
one **terminator**. Inside a block the post-order node list already is
three-address code: each `#k` is a single-use value. So no new instruction
set; the lowering only (a) cuts the statement structure into blocks and
(b) removes the control flow hidden inside expressions.

New ops (all statements, all terminators):

| op | kids | meaning |
|---|---|---|
| `jmp B` | none | unconditional |
| `br B1 B2` | one value | nonzero: B1, else B2 |
| `swtch` | one integer value | `val` = default block; one `case v B` entry per case, listed after |
| `ijmp` | one pointer value | computed goto; the possible targets are every block whose label has its address taken (`labaddr`) |
| `ret` | zero or one value | return |

and one new leaf, **`reg N`** (a function-local virtual register, any scalar
type, stored many times, loaded where its value is needed). Each lowering
below that merges control flow uses it: it is T5's `tmp` for values instead
of addresses, and unlike `tmp` it may be stored more than once. `tmp`
(T5) and `complit` stay as they are.

A block has a number; blocks print as `block B` with their nodes and then the
terminator. `label L`, `goto L`, `igoto` disappear: a label becomes the block
it starts, `goto` a `jmp`, `igoto` an `ijmp`.

## 3. The lowerings

Statements (per-tag, but all are "emit blocks and edges"):
- `if c S1 S2`: `br c Bt Bf`; both arms `jmp Bj`.
- `while`/`do`/`for`: header block with `br`; `continue` and `break` are
  `jmp` to the step/header and exit blocks; the loop stack is the existing
  `break/continue` target discipline.
- `switch v body`: the body is a `block` whose items are `case n stmt` /
  `default stmt` wrapping their statement (checked on a dump of
  2026-10-18: `case 1 #7`, `block #8 #16 #17 #22`, `switch #3 #23`); each
  `case`/`default` starts a block, every non-terminated block falls through
  with a `jmp` to the next, `break` is `jmp` to the exit block, and
  `swtch` is the terminator after the controlling value. Duff-style nesting
  (a `case` inside an inner statement) works because blocks are cut by label,
  not by syntax. Checked 2026-10-18 on `tests/tast/duff.c` (Duff's device,
  `case` inside `if`/`for`/`do`): the structured dump already carries each
  nested `case` as a node at its depth, so T8a collects `case`/`default`
  by walking the whole switch body, not only its top-level items.
- `return e`: `ret e` (a `reg` is not needed).
- `local x init`, `exprstmt`, `nop`: unchanged, stay in the current block.

Expressions with hidden control flow (these split the current block and join
through a `reg`; value-less forms need no `reg`):
- `logand a b`: `br a Bb Bf`; `Bb`: evaluate `b`, store `reg := (b != 0)`,
  `jmp Bj`; `Bf`: `reg := 0`, `jmp Bj`; `Bj` loads `reg`. `logor` mirrors it.
- `cond c a b`: `br`, each arm stores `reg`, join loads it (void: no `reg`).
- `elvis a b`: store `a` to a `reg`; `br` on it; the false arm stores `b`.
- `comma a b`: no control flow, just `a` as an `exprstmt` then `b`: it only
  needs to be flattened into the block.
- `post (store ..)`: the store of `x++` returns the new value; the old one is
  the loaded value, saved to a `reg` before the store (the one node T1
  already kept out of the core).
- `stmtexpr`: lower its statements into the current block structure and
  yield the value of the last, via a `reg` if the statements contain a
  split. A `goto` into one is rejected by the checker (T3 plan).

A split inside an expression means everything evaluated before the split in
the same expression must already be in single-use values that survive it. In
post-order they would span blocks, which the "each `#k` single-use within a
block" rule forbids. **Rule:** at a split, every pending value (a node
already evaluated whose parent is not yet built) is spilled to a `reg` first.
This is the real work of the pass: it needs the evaluation order the
structured form already fixes, and a spill list at each split. **Verify** the
count on the corpus before fixing the design (section 6, spike).

## 4. Verifier (`--verify-cfg`)

- Every block ends with exactly one terminator and has none inside.
- Every target block exists; `swtch` case values are distinct.
- Every block is reachable from the entry, or marked `unreachable` (code
  after a `return`, a label never jumped to); a function has at least one
  `ret` or an infinite loop.
- A `reg` is stored on every path to each of its loads (must-defined
  dataflow over the CFG); a `reg` has one type.
- The node rules of the structured verifier hold inside each block (kids
  precede, single use).
- Round trip: nothing for the structured verifier to change; the lowered
  form is a separate dump (`--dump-cfg`), so the structured goldens stay.

## 5. Check against execution (the oracle)

Structure alone does not show the lowering preserves meaning. A small
interpreter (Python, in `tests/`) runs the lowered form on functions over
integer scalars with locals, arithmetic, comparisons, `&&`/`||`/`?:`, loops,
`switch`, `goto` and calls to other such functions (no pointers, no
memory), and a driver compares its results with gcc-compiled code on random
arguments. A subset, but it covers every control-flow lowering. The
structured form is not touched by it. Pointer and memory operations are
covered by the structural verifier and the corpus gate only; say so in the
test header. **Verify** that a Python interpreter for the subset stays small
(under ~250 lines); if not, evaluate the lowered form with the existing
constant evaluator in `tests/tast_data.py` extended with a block walk.

## 6. Order, spikes, gates

0. Spike (throwaway): count, over lua54 and libuv, the splits inside
   expressions and the pending values at each (section 3). If pending
   values are rare, spill-everything is acceptable; if frequent, a smarter
   scheme (keep values in a `reg` only when needed) is a later step.
   **Result (2026-10-18, throwaway script, not kept):** lua54 2174 functions,
   722 with a split; 1778 splits (cond 774, logand 610, logor 392, stmtexpr
   2). Pending values at a split: 0 in 1236, 1 in 364, 2 in 114, 3 in 60, 5
   in 4; max 5. libuv 1546 functions, 247 with a split; 600 splits (stmtexpr
   203, logand 222, logor 144, cond 30): 0 in 538, 1 in 56, 2 in 4, 3 in 1;
   max 3. About 70% of splits have nothing pending; the maximum is 5.
   Spill-everything is acceptable: a `reg` count per split is small and no
   smarter scheme is needed. (Pending = expression node before the split
   whose expression parent comes after it; `stmtexpr` counted at its start.)
1. T8a: statements only (`if`, loops, `switch`, `break/continue/goto/label`,
   `return`), expressions unchanged; blocks and `jmp/br/swtch/ret`, the CFG
   verifier. Functions whose expressions contain no hidden control flow
   lower fully.
2. T8b: expression control flow (`logand/logor/cond/elvis/post/stmtexpr`)
   with `reg` and spills; `ijmp`.
3. T8c: the execution oracle, then the gate: suite, callgrind (the pass is
   behind `--dump-cfg`/an option, so the hot path is untouched), lua54 and
   libuv: every function lowers and verifies with 0 findings, count of
   unreachable blocks and of `reg`s reported.

## 7. What is out

- Optimization of any kind (no SSA, no copy propagation): `reg` is a mutable
  variable, QBE or a later pass builds SSA.
- Lowering initializers and `local` elems into stores, `va`, intrinsics and
  calls' ABI classification: later (the SysV classification is its own
  ROADMAP item).
- `setjmp`/`longjmp` and `returns_twice` edges: not modelled; noted for the
  dataflow consumers.
- VLA allocation (unsupported in the structured form already).

## 8. Parked by this plan

Nothing yet; the out list above becomes `PARKED.md` rows when a stage
closes.

## 9. Decisions needed

1. Form: **blocks of the existing trees plus `reg`** (recommended: smallest
   concept count, QBE-shaped, reuses every verifier rule) against full
   three-address code with explicit register operands (a second node
   language; larger, and wins only if a consumer cannot take trees).
2. Stage order: T8a statements first (recommended; useful for Track D
   dataflow immediately) against both at once.
3. Keep the structured form as the first stage (recommended, design section
   4) against replacing it: replacing deletes the structured goldens and
   the T1 to T7 verifiers' reason for existing, so no.

## 10. Review log

- **Round 1 (shape).** Caught: a block of post-order trees already is
  three-address code, so a new instruction language is not needed; the
  `reg` must be multiply-assignable, which `tmp` is not (T5 verifier rule
  "exactly one store").
- **Round 2 (hidden control flow).** Caught: a split in the middle of an
  expression breaks the single-use-within-a-block rule for values already
  computed; the plan now names the spill rule as the main cost and puts a
  spike before the design is fixed. `post`'s old value also needs a `reg`.
- **Round 3 (checks).** Caught: structural verification cannot show meaning
  is preserved; added the execution oracle and stated that it covers only the
  integer, memory-free subset.
- **Round 4.** Nothing material beyond the **verify** items (Duff nesting,
  interpreter size, the spike).
