---
status: draft
---

# Lazy blobs: sharing by element size

Goal: a sweep call that walks a structure through many pointer fields of one kind (Lua's `luaD_reallocstack`:
`L->stack`, `top`, `tbclist`, each open upvalue's `v`, `ci->func`, `ci->top`, all `StackValue *`) gets them to
agree after one or two retries, not one per field. Replaces the cell-class-by-offset hint of
`LAZY_FIELD_SHARE_DESIGN.md` (built in `ed6440c`; its measurements are the evidence below). Mechanism extended:
`LAZY_BLOBS_DESIGN.md`, `lazy_redraw`, `lazy_draw`, `lazy_fill`, `sweep_call` in `src/c/eval_run.c`.
Supersedes the typed-class draft of this spec (class = printed pointee type): a reviewer briefed to find fewer
concepts showed the trap already carries what that needed.

## 1. Facts

- The retry hint (`lazy_redraw`): a trap on a subtraction of two blobs forces the later operand's pointer cell
  to share the earlier blob, one hint per retry, at most `LMAXHINT`.
- Offset class (`ed6440c`: the hint also names the cell's class, its offset plus its creator's offset):
  fixture `lazy_list.c` (a six-element list) 17 -> 194 of 200 calls finish; Lua `LINK=1` N=24 seed 1234 edges
  6553 -> 6581 of 9599, seconds 1619 -> 2154, solver-first edges 2102 -> 2092; `luaD_reallocstack` still
  finishes none of 24 calls (one with `LMAXHINT` 24; `LMAXBL` 128 changes nothing). After the class, 507 edges
  stop on `lazy blob budget`, 18 on the subtraction.
- Trace of the first retries of 65 generator calls of `luaD_reallocstack` (2026-11-08, paths are pointer cell
  offsets from `L`, `LHEAD` 128 included): `/0/176` (`stack`), `/0/144` (`top`), `/0/192` (`tbclist`), then
  `/0/184/144` (first open upvalue's `v`), `/0/184/152/144` (the second's; later ones are covered by the
  class), then `CallInfo` fields. A call needs 10 to 14 distinct classes, one retry each. Every one of these
  cells is declared `StackValue *` (16 bytes; `TValue *`, the upvalue's other possible type, is 16 too).

## 2. Design

**Class = the pointee size the trap subtracted.** `same_object` gains the operand's `TypeId` (the node type of
operand 0, which `compare` already has as `ot`) and records `sub_sz = ev_tinfo(type).psz` beside `sub_a/sub_b`,
so the trap's size and a cell's `Ti.psz` are the same quantity by construction (0 for a `void *`, a function, an
incomplete or 4096-byte-or-larger pointee: no size entry, cell pair only). `TO_PTRDIFF`'s `t->val` is not used: it
is 1 for void and function pointees and has no 4096 cap. No type text, no hash, no `Ti` or `LBlob` field.
`lazy_call1` clears `sub_sz` with `sub_a`, `sub_b` and `pun_obj`.

**Hint.** `lazy_redraw` writes, beside the cell pair it writes today, one entry `lz_key(LZ_SZ, sz)` = the earlier
blob's key, when `sub_sz != 0` (tag `LZ_SZ` replaces `LZ_CLS`; the hint array is three entries for a pointer cell, one
for a parameter blob's `LZ_ALIAS` hint (no size entry), up to two for the `pun_obj` hints; `LEnt hint[4]` at
`lazy_redraw`, `lazy_call1`, `sweep_call` and the "at most four" comment shrink to 3). The entry means "share this blob"; it has no
OUT half. In `lazy_fill`, once per pointer cell with `Ti.psz != 0`: look up the entry for `psz` in `z->force`;
resolve its key to a blob index (none: no share); and if the draw is not the NULL draw, pass `lazy_draw` the
fallback values OUT = 1 and TGT = that key through `alt` (now a pointer to a pair of values, NULL for none, not a key; a key of 0 is never
"none"). `lazy_draw` uses them only when the cell has no forced entry of its own, and logs the value under the cell key. `lz_cls`, `LZ_CLS` and
the offset class are deleted. The size entry lives only in `sweep_call`'s `cur`; `rec`, seeds and replay hold
cell keys only.

**The share applies only if the target exists.** The first `StackValue *` cell of a rerun is read before the
blob the hint names is made (the one `stack_last` creates). The resolve-once step above is the guard: no blob with
the entry's key, no fallback, the cell draws as it would, and a dead key is never logged. (The offset class
could fall back to a random target; a size share that did would share `L` itself. This rule is also what keeps
`lazy_list.c`'s own `char *stack` cell, size 1 like the elements' `v`, from sharing before its blob exists, so
the fixture tests it.)

**Rulings carried over.**
- The call's NULL draw (`nullat`) never takes `alt`; `ndraw++` still happens.
- Cell entries win. A cell with its own forced entry (the seed's log, a solver rerun, `LZ_ALIAS`, `pun_obj`)
  keeps it, so a solver rerun of a trapped seed, whose log holds an entry for every cell it drew, gets the cell
  fix for the trapped cell only. The size half is not seen by those cells (limitation, measured via the solver
  half of the sweep).
- A cell retargeted after its entry was logged keeps its logged target; if that blob no longer exists the
  `lazy_fill` rule keeps the random target (as today).
- One target per size: a later trap on the same size overwrites it (bounded by `LMAXHINT`); two types of one
  size (`TValue *`, `StackValue *`, any 16-byte struct) share one entry. A wrong share costs coverage only (the
  graph is recorded and replayed).
- A forced share shifts later unforced draws of that rerun (true of every forced draw).

**Cannot cover.** Subtractions between cells of different pointee sizes (a `char *` against a `TValue *`, which
is a different-object trap anyway); a list whose end is a NULL the call does not draw (`nullat`: half of the
calls have none, the walk stops at `LMAXBL`: the 507 edges); a pointer reached only through a cast; a comparison whose operand 0 is a `void *` (`sub_sz` 0: cell pair only).

## 3. Decide

**Decide (what keys the share)**

- **1 Pointee size from the trap** (recommended): the design above. Subsumes the offset class: one retry covers
  `stack`, `top`, `tbclist`, the upvalues and the `CallInfo` fields of one size.
    - Pro: no type text, no hash, no `LBlob` or `Ti` field; deletes `lz_cls`; works across `--eval-link` files.
    - Con: same-size unrelated types share (coverage cost only); `char *` (size 1) is the likeliest collision.
- **2 Pointee type text:** the earlier draft, a printed canonical type hashed to a key.
    - Pro: finer than size.
    - Con: adds `Ti.pte`, `LBlob.cls`, a print and hash per cell to compare with the trap's type.
- **3 Keep the offset class** with `LMAXHINT` raised.
    - Pro: nothing to build.
    - Con: 10 to 14 classes in Lua against a cap of 8; the trace says it cannot reach the wall.

Searched: the size entry as an OUT/TGT pair (removed: one value suffices, it always means "share"); dropping
the cell pair (kept: a solver rerun logs every cell, so only the cell key reaches the trapped cell); a size
computed in `lazy_redraw` from the creating cell (rejected: no `psz` on `LBlob`); a class from the cell's path fails (the path grows along a list); a class from the type of the *blob*
has no type on parameter blobs; inferring the share in `lazy_fill` with no stop to learn from changes every draw
and the goldens; the type text, `Ti.pte`, `LBlob.cls`, and the random-target fallback were each tried and
removed by the size key. Nothing else removable.

## 4. End-state fit and checks

- **Where:** `same_object` and the pointer-subtraction/compare trap sites record `sub_sz` (`eval.c`, `eval_int.h`);
  `lazy_fill`/`lazy_redraw`/`lazy_draw` in `src/c/eval_run.c`; fixture and `tests/eval_lazy.sh`.
- **Replaces:** the offset class. **Deletes:** `lz_cls`, `LZ_CLS`. `hint_put`, `LZ_ALIAS` and the `pun_obj`
  hints stay. No new field in `Ti` or `LBlob`.
- Probe: `--eval-only=luaD_reallocstack --eval-sweep=24,1234 --eval-link` finishes some calls (none today).
- Fixtures: `lazy_list.c` still finishes at least 180 of 200 (update its header comment and the `eval_lazy.sh` message, which describe the offset class; its `stack` and `v` cells are both size 1, so it also tests the target-exists rule); a new fixture with several fields of one pointee
  size at different offsets of one struct, subtracted against another field, finishes in one retry where the
  offset class needs one per field.
- Sweep: `sweep_meas.sh` seeds 1234 and 7, `LINK=1`, N=24: edges at least the base (6553; seed 7 needs a
  `~/cereal-base` run `base-link24-s7`), seconds within +10% of 1619, solver half no lower.
- Replay: `CEREAL_LAZY_REPLAY=1` over `tests/eval_lazy.sh` and `tests/eval_sweep.sh`; gate green; sweep `call` lines before and after diffed
  over `tests/cfg_lazy/lazy_*.c` and `tests/eval/*.c` (only calls that stopped on a subtraction may change; there are no goldens).
- Exit: if the sweep gain is under the noise of the seed pair or time rises more than 10%, record the result in
  `PARKED.md` row 248 and undo the size-change commits (`git checkout c2bbf16 -- src tests`: the offset-class state, where the fixture passes).
- Docs: `PARKED.md` row 248 is corrected (`e1d0bc7`); when this lands or is shelved, add the size-share result to it.
