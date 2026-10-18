---
status: done
---

# TAST stage T6: plan (computed goto)

Follows `docs/TAST_T5_PLAN.md`. Facts are from the tree of 2026-10-18; anything
unread is marked **verify**. Built 2026-10-18 (section 8).

## 1. The problem and how big it is

GNU computed goto is two constructs: `&&label` (checker `N_ADDR_LABEL`, type
`void *`, `ck` K_ADDR, `cexpr.c:5779`) and `goto *expr;` (`N_GOTO_EXPR`,
`parse.c:2986`). The pass handles neither: `&&label` is "expression
ADDR_LABEL"; the statement **verify**: its reason when it is hit (the pass has
no `N_GOTO_EXPR` case at `tast.c:1438`, so it falls to the default refusal).

Corpus: lua54 `lvm.c` only (the `luaV_execute` dispatch: a function-local
`static const void *const disptab[] = { &&L_OP_MOVE, ... }` in `ljumptab.h`
and `vmdispatch(x)` = `goto *disptab[x]`). It is the last lua54 unsupported
unit; libuv has none. The reason to do it is that the last lua54 unit is
also the hot interpreter loop, and the one place a static initializer holds a
label address.

## 2. Design: two ops, no new concept

- **`labaddr L`**: a leaf value, type `void *`, `val` = the label identifier
  (as `goto` keeps it). It is a link-time constant (`sconst` true), so it can
  sit in a static initializer. Printed `labaddr L void *`.
- **`igoto`**: a statement, one kid, a pointer value (`goto *p;`). Printed
  `igoto #k`. Allowed wherever `goto` is.

Nothing else changes: `goto *disptab[x]` is `igoto (load (deref (ptradd
disptab x)))` over existing nodes. Casts of `&&L` (`(long)&&L`) are ordinary
`cast` over `labaddr`.

Considered and not done:
- Modelling labels as symbols/`addr`: `sym` means "a declared entity" (T5
  section 2) and a label has no type or storage; a leaf op is smaller.
- A control-flow graph for `igoto` targets: the pass has no CFG; the
  verifier only needs that each `labaddr` names a label of the same function,
  the same rule as `goto`.

## 3. Verifier

- `labaddr`: no kids, type `void *`, its label exists in the unit
  (`labels` is collected from all nodes before any is checked,
  `tast.c:1721-1725`, so a label defined below its use is fine, which
  `luaV_execute` needs).
- `igoto`: statement position, one kid, a pointer-typed value.
- `sconst`: `labaddr` is constant; `velems` already uses `sconst` for static
  elements, so `static void *t[] = {&&a,&&b}` passes without a new rule.
- **Verify** an `labaddr` outside a function cannot reach the pass (the
  checker errors: "referenced outside of any function").

## 4. Data oracle

`tests/tast_data.py` compares gcc object bytes and relocations with the
evaluated dump. A label address in gcc's object is a relocation against
`.text` plus an offset the pass cannot know. Plan: the oracle treats a
reloc to `.text` as "any `labaddr`" at that offset (count and positions match,
the addend is not compared); say so in the oracle's header. **Verify** this
is what `gcc -c` emits for `lvm.c` (local labels may be resolved by the
assembler into section-symbol relocations, or by a `-fno-pic` absolute).

## 5. Order, checks, gates

1. `labaddr`, `igoto`: ops, printing, verifier, `sconst`.
2. `e_...` cases for `N_ADDR_LABEL` and `N_GOTO_EXPR` in `tast.c`.
3. Tests `tests/tast/cgoto.c`: a dispatch table in a static local, a
   `goto *p` in a loop, `&&L` in an automatic initializer and a cast, a
   label defined after its address is taken, `goto *` of a conditional
   expression.
4. Gates: suite; coverage via `run.ps1` (lua54 2 -> 0, libuv unchanged);
   data oracle (section 4); gate with callgrind.

## 6. Risks

- The checker may already treat a function containing `&&L` specially
  (`cstmt.c:1023`, `:1255`, `check_int.h:649`): **verify** none of it
  affects what the pass reads (reachability or "label used" marks).
- The dump is the structured form; the order of labels in a function is
  not a promise about layout.

## 7. Review log

- **Round 1 (claims against the tree).** Caught: the pass has no
  `N_GOTO_EXPR` case (the refusal reason is unverified); `ljumptab.h`'s
  table is a static local, so section 3's `sconst` rule is load-bearing, not
  incidental.
- **Round 2 (verifier and oracle).** Caught: forward label references need
  the label list built before checking uses; the oracle cannot compare a
  label address, so the plan states what it does compare instead of
  claiming byte equality.
- **Round 3.** Resolved the forward-label question against `tast.c:1721`;
  the `N_GOTO_EXPR` refusal reason stays unread (lua54 stops at
  `ADDR_LABEL` first). Still open, to be settled while building: the
  `.text` relocation form (section 4) and the checker's label marks
  (section 6).

## 8. Progress (2026-10-18)

Done as planned: `labaddr` (leaf, `sconst`) and `igoto`; `tests/tast/cgoto.c`.
Suite 2266/0. Coverage `tast-cgoto-coverage`: lua54 unsupported 0, libuv 3,
verify findings 0.

- Section 4 not built as planned: the data oracle skips an object holding a
  `labaddr` (`lvm.c`, `onelua.c` `disptab`) instead of comparing positions;
  lua54 107 objects 0 failures, libuv 32 / 0. Parked in `PARKED.md`.
- Section 6 (checker label marks) was not needed: the pass reads only the
  label identifier.
- Not checked: the `goto *expr` refusal reason before this change.
