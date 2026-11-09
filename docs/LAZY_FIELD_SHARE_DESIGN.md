---
status: draft
---

# Lazy blobs: sharing by cell class

Goal: a sweep call that walks a linked structure (`for (up = L->openupval; up; up = up->u.open.next)
up->v.p = ...`) gets the pointer cells of the elements to agree after a few retries, not one retry per
element. Evidence and parking: `PARKED.md`, "Pointer structure of the sweep's generated state";
mechanism extended: `LAZY_BLOBS_DESIGN.md`, `lazy_redraw`, `lazy_draw`, `sweep_call` in `src/c/eval_run.c`.

## 1. Facts

Probe 2026-11-08, Lua, `--eval-only=luaD_reallocstack --eval-sweep=24,1234 --eval-link`; all 24 calls stop
on `subtraction of pointers into different objects`. A trap on a subtraction of two blobs makes
`lazy_redraw` force the later operand's pointer cell to share the earlier blob (outcome 1, target = the
earlier blob's key), one cell per retry, at most `LMAXHINT` = 4. The retries of one call, by path from `L`
(cell offsets with `LHEAD` 128 removed):

| retry | later operand | field |
|---|---|---|
| 1 | `L+48` | `stack` shares `stack_last` (`L+40`) |
| 2, 3 | `L+16`, `L+64` | `top`, `tbclist` |
| 4 | `L+56 -> +16` | the first upvalue's `v` |
| 5.. | `L+56 -> +24 -> +16`, `-> +24 -> +24 -> +16`, ... | the next upvalues' `v`, one retry each |

The stop is `relstack`/`correctstack` over the open-upvalue list; the `CallInfo` walk (`ci->func`,
`ci->top` through `previous`) follows it (an earlier `PARKED.md` sentence said `CallInfo` first; the probe
says upvalues). Raising `LMAXHINT` to 16 left all calls stopped: the list grew to 10 blobs and the hints
(one cell each) never caught up. The end of a list is also the call's single NULL draw (`nullat`: half the
calls have none, the rest one uniform in 0..31 pointer draws).

The wall is 536 of Lua's 9599 edges (5.6%). Any list walk over lazy blobs has this shape.

## 2. Design

**Cell class.** A pointer cell's class is (its offset in its blob, the offset of the cell that made its blob;
for a parameter's blob, the parameter's number). Both are in `LBlob` already (`poff`, `par`). The first
upvalue's `v` is class (16, 56); every later one is (16, 24), because their blobs are made by `next`.

**Hint by class.** A cell's draw key is today `lz_key(blob key, off)`; a class key is
`ck = lz_key(LZ_CLS, lz_key(off, creator poff))` (`LZ_CLS` is a new tag, 7 << 56; offsets are the stored
ones, `LHEAD` included, a parameter blob's `poff` is its parameter number, so classes of the two never
collide), and the class entries are the pair `lz_key(ck, LZ_OUT)`, `lz_key(ck, LZ_TGT)`; `lazy_redraw`
computes `ck` from `h->poff` and `bl[h->par].poff`, `lazy_fill` from `off` and `bl[bi].poff`. `lazy_redraw`'s share hint is written under the cell key as today and under the class key of
the later operand's creating cell (four entries, `hint[4]`; `hint_put`, `asked`, `nh` and the retry loop
are unchanged). `lazy_draw` takes an `alt` key (0: none): when the call's forced map has no entry for the cell key it
looks `alt` up, and the value used is logged under the cell key as always, so the call's log (`rec`), the
solver's `force`/`syms` and the gcc-side graph rebuild (`sw_load` replays the link table only) see
ordinary cell draws. Of the seven `lazy_draw` callers (`lazy_off`, `lazy_fn`, `lazy_byte`, the `LZ_OUT`
and `LZ_TGT` draws in `lazy_fill`, the scalar parameter bytes, the `LZ_ALIAS` draw) only the `LZ_OUT` and
`LZ_TGT` draws pass `alt`; the rest pass 0. Class entries live only in `sweep_call`'s `cur`; nothing copies
`cur` out (`rec`, the solver's `force`, the call line and replay are built from cell keys).

The Lua list then costs the five retries of the table, not one per upvalue: `stack`, `top`, `tbclist`,
class (16, 56), class (16, 24). The `CallInfo` walk adds its own classes. `LMAXHINT` rises from 4 to 8
(set by the probe; a retry reruns one call, measured in milliseconds).

**Rulings carried from review.**
- *The call's NULL.* A cell drawn at `d == z->nullat` does not take `alt` (the forced class entry would
  otherwise overwrite the one NULL and the list never ends); `ndraw++` still happens.
- *Cell entries win.* A cell with its own forced entry (a seed's log, the solver's override, a `LZ_ALIAS` or
  `pun_obj` hint) keeps it; a class entry fills only cells without one. Consequence: a solver rerun of a
  trapped seed, whose log holds an entry for every cell it drew, does not get the class hint for those
  cells. The cell half of the hint (written as today, overwriting the seed's entry) still fixes the cell
  that trapped, so a trapped seed loses nothing; only the other cells of the class go unhelped. (Writing the
  class entry alone would have lost the fix: the retry would see the cell's entry win, trap again and stop.)
  The check in section 4 measures the solver half separately.
- *Draw stream.* A class hint that turns a "new" cell into a share adds two `rnd` calls (`lazy_fill`
  draws target and offset only on outcome 1) and shifts later unforced draws of that rerun. The log holds
  everything the final run drew, so replay is unchanged; the sentence in `LAZY_BLOBS_SOLVER_PLAN.md`
  that draws do not shift under forcing is already untrue and is not made worse in kind.
- *Absent target.* A class entry's target blob key may not exist in the rerun; `lazy_fill` then keeps the
  random target it drew (`:942-948`), so the cell shares some blob, not the asked one, and `asked` ends the
  retries. Same as today.
- *One target per class.* `hint_put` overwrites the class's target when a later trap in the same class names
  a different earlier blob, which can undo an earlier fix and burn retries up to `LMAXHINT`. Accepted; the
  probe shows one target (`stack_last`) for the whole class.
- *Wrong class.* Two unrelated structs with a pointer at the same offset, made through a pointer at the same
  offset, share wrongly. Cost: coverage only (the graph is recorded and replayed).

**Cannot cover.** Lists walked through different link offsets in turn (doubly linked, alternating) cost one
retry per link offset. Element 1 and element 2 are two classes (two retries). The end of the list stays
with `nullat`.

## 3. Decide

**Decide (class key)**

- **1 Offset and creator offset** (recommended): about 10 lines; no types.
    - Pro: nothing new in `Ti`, `LBlob` or `Lazy`; per-call scope keeps `--eval-only` reproducing a call alone.
    - Con: element 1 and the rest are two classes; collisions across unrelated structs.
- **2 Declared pointee type and offset:** `Ti.pte`, `LBlob.ty`, a rule table beside `cur`.
    - Pro: one retry for the whole list; robust to two structs sharing an offset pair.
    - Con: three new concepts; parameter blobs carry no type (`SArg` has none for `SA_PTR`); a record
      parameter's blob type is not a pointee; a cast or `void *` cell gives type 0; TypeIds are per
      translation unit, so a sweep-wide table needs a printed key. Revisit if a measurement shows class
      collisions cost edges.
- **3 Offset alone:** one retry for the list, but every same-offset cell in the call shares. Rejected
  without a measurement.

Searched: a hint by the cell's draw path fails (the path grows along the list); inferring the share in
`lazy_fill` with no stop to learn from changes every draw and the goldens; a transitive hint along the
creator chain cannot name class (16, 24) before the list is walked. A sweep-wide rule table is not taken
(per-TU type ids, and calls would no longer reproduce alone). Removed: the type, the rule table and the
per-sweep scope of the first draft. Nothing else removable.

## 4. End-state fit and checks

- **Where:** `lazy_draw`/`lazy_fill`/`lazy_redraw` in `src/c/eval_run.c`; no other file.
- **Replaces:** nothing; the share hint gains a class pair beside its cell pair. **Deletes:** nothing; `hint_put`, `LZ_ALIAS`
  and the `pun_obj` hints stay (they name parameters and a blob, not a class). Retry accounting
  (`nh`, `LMAXHINT`) covers all hints.
- Probe: `--eval-only=luaD_reallocstack` finishes some of its 24 calls (today none); the same for the
  functions that walk `CallInfo` and `GCObject.next`.
- Sweep: `sweep_meas.sh` seed 1234, `LINK=1`, N=24: at least 6553 edges taken, the 536-edge stop smaller;
  a second seed; the solver half reported on its own.
- Replay: `CEREAL_LAZY_REPLAY=1` over `tests/eval_lazy.sh` and `tests/eval_sweep.sh` (each call re-run from
  its log prints the same line). A new fixture: a small C file whose function walks a three-element list
  checking a pointer subtraction per element, in `tests/eval_lazy.sh`'s style, that traps today.
- Gate: the suite and sweep goldens unchanged except where a draw changed; gcc comparison clean.
