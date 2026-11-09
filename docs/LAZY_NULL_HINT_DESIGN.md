---
status: draft
---

# Lazy blobs: moving the call's NULL on a NULL-operand trap

Goal: a sweep call that stops on a pointer subtraction with a NULL operand gets one more chance with its one NULL
draw (`z.nullat`) somewhere harmless, so the walk of `luaD_reallocstack` goes on past the cell the NULL spoiled.
Built on `LAZY_SIZE_SHARE_DESIGN.md` (built in `836573e`, kept: it does what it was meant to, see its plan's Result).
Mechanism: `lazy_call1`, `lazy_redraw`, `sweep_call` in `src/c/eval_run.c`.

## 1. Facts

Trace 2026-11-09 (throwaway build of `836573e`, `luaD_reallocstack`, 24 calls, 88 generator runs, seed 1234, plan's
Result section): the size hint takes, and the final stops are

| final stop | runs | of which the call has a NULL draw |
|---|---|---|
| `lazy blob budget` | 36 | 14 |
| subtraction with a NULL operand (`sub_a` or `sub_b` is 0; `lazy_redraw` names no blob) | 40 | 30 |
| subtraction of two blobs, not retried | 12 | 2 |

In 21 of the 30 NULL-operand runs with a NULL draw, the last cell drawn is the NULL one (`d == nullat`, `r = 0`). A
call has one NULL draw (`z.nullat`, drawn in `lazy_call1`: half the calls none, else a uniform pointer-draw
index 0..31), so the NULL operand there is that draw landing on a cell the walk subtracts. Ten NULL-operand runs
have no NULL draw: a zero-filled field, a separate cause this spec does not touch.

## 2. Design

**The NULL draw becomes a hinted draw.** `lazy_call1` draws `nullat` from the PRNG as today (both numbers, always,
so the stream is unchanged) and passes it through `lazy_draw` under a new key `lz_key(LZ_NUL, 0)` (`LZ_NUL` is
`(uint64_t)8 << 56`, with `~0u` for none); the forced value wins and the value used is logged, so `rec`, seeds,
solver reruns and replay hold it with no further change. `lazy_redraw` gains a branch: a trap on a subtraction with
exactly one NULL operand (`sub_a` or `sub_b` is 0, not both) while the call has a NULL draw that was reached
(`z->nullat < z->ndraw`) writes one hint entry, `lz_key(LZ_NUL, 0)` = the new index. No cell identity is needed: the
call has one NULL.

**Where the NULL goes** (Decide below). Recommended: `z->ndraw`, the first pointer draw the walk had not reached at
the stop. The NULL then falls after everything the walk subtracted so far, and each further NULL-operand stop moves
it on (bounded by `LMAXHINT`).

**Rulings.** The existing NULL rule stays (`d == z->nullat` never takes a share or size share). One hint entry, so
`hint[3]` is enough; `asked` ends the retries as for the other hints. A moved NULL changes the later draws of the rerun
(true of every forced draw).

**Cannot cover.** NULL operands with no NULL draw (10 of 40); `lazy blob budget` stops (36: lists with no NULL or a
NULL that did not end them); two-blob stops that were not retried (12); a NULL subtracted from a cell whose NULL
draw comes later than the walk reaches within `LMAXHINT` retries.

## 3. Decide

**Decide (where the NULL goes)**

- **1 The next unreached draw** (recommended, `nullat = ndraw`): one value, derived from the stop.
    - Pro: advances through the walk, never lands on a cell already read; ends a list after the cells so far.
    - Con: the next cell may be subtracted too; one retry per such cell.
- **2 One draw later** (`nullat + 1`): the smallest move.
    - Pro: simplest rule.
    - Con: the NULL stays among cells already read when the stop was late, so it can be the same cell's neighbour; costs a retry per step.
- **3 No NULL** (`~0u`): remove the draw.
    - Pro: nothing can be NULL; one retry.
    - Con: the lists lose their only end, moving the stop to `lazy blob budget`.

Searched: the identity of the NULL cell (not needed: one NULL per call, identified by its draw index); naming the
NULL operand's cell in the hint (`sub_a` is 0, so no blob names it); a per-cell NULL flag in the forced map (the draw
is already one value). Nothing else removable.

## 4. Checks and exit

- **Where:** `lazy_call1`, `lazy_redraw`, `LZ_NUL` in `src/c/eval_run.c`; a fixture (a function subtracting two
  pointer fields where the NULL draw can land on either); `tests/eval_lazy.sh`. **Replaces/deletes:** nothing.
- **Spike first** (cheapest prototype, throwaway build, not committed): options 1, 2 and 3 on the probe,
  `luaD_reallocstack` 24 calls seed 1234 `--eval-link` (today 1 of 24 finish). If no option finishes more than 1 of 24,
  the idea is decided against in `PARKED.md` row 248 and nothing is built.
- Fixture: finishes the calls whose NULL draw lands on a subtracted field (today they stop for good).
- Sweep: `sweep_meas.sh` seed 1234 `LINK=1` N=24: edges at least 6541 (size share), seconds not above 1896 by more than
  10%, solver-first edges not below 1968; replay (`CEREAL_LAZY_REPLAY=1`) and the gate green.
- Exit: if the sweep does not beat 6541 by more than the seed pair's noise, record it and revert to `22ac39a`.
