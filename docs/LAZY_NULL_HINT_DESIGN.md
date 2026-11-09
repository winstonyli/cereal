---
status: done
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

**The NULL draw becomes a hinted draw.** `lazy_call1` (line 1192, the only place `nullat` is set) today assigns
`z.nullat = rnd(rs) & 1 ? ~0u : rnd(rs) % 32` with no log entry; the second `rnd` runs only when the first bit is 0.
It keeps that PRNG use exactly (stream unchanged), then passes the result through `lazy_draw` under a new key
`lz_key(LZ_NUL, 0)` (`LZ_NUL` is `(uint64_t)8 << 56`; `lz_key` hashes the tag, so 8 fits); the forced value wins and
the value used is logged, so `rec`, seeds and replay carry it. `lazy_redraw` gains a branch, tested before the
`sub_a != 0` branch: a trap on a subtraction with exactly one NULL operand (`(sub_a == 0) != (sub_b == 0)`; the
trap is `same_object`, eval.c:630) while the call's NULL cell was reached (`z->nullat < z->ndraw`) writes one hint
entry, `lz_key(LZ_NUL, 0)` = the new index. `sub_a`/`sub_b` do not name the NULL cell, so the branch assumes the NULL
operand is the `nullat` cell (true for 21 of 30 traced; see Cannot cover).

**Where the NULL goes** (Decide below). Recommended: `z->ndraw`, the first pointer draw the walk had not reached at
the stop. The NULL then falls after everything the walk subtracted so far, and each further NULL-operand stop moves
it on (bounded by `LMAXHINT`).

**Rulings.** The existing NULL rule stays (`d == z->nullat` never takes a share or size share). One hint entry, so
`hint[3]` is enough (the size share uses at most 3 entries, the NULL hint is a separate retry); `asked` ends the
retries as for the other hints. Moving the NULL shifts the PRNG stream of every later cell, not just the moved one
(`lazy_fill` skips `rnd` only at the NULL cell, line 939), so the rerun can differ before it reaches the new index;
`ndraw` is only the first unreached draw of the stopped run. A retry applies only when the new index differs from
the current `nullat` (replay restarts `nh` at 0 and must not retry a call that stopped with the value already forced).

**Cannot cover.** NULL operands with no NULL draw (10 of 40); `lazy blob budget` stops (36); two-blob stops not
retried (12); calls whose NULL cell is forced by recorded OUT entries (solver reruns, `mut_gen`: the forced OUT=0 of
the old cell wins, so moving `nullat` does nothing there; the solver's own NULLs, D11a, also make NULL operands that
are not `nullat`); a moved NULL that lands on a dereferenced cell (a NULL-dereference stop replaces the old one); a
NULL cell whose draw returned false (`lazy_fill` budget paths), where `ndraw` is past anything a rerun reaches.

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

- **4 Hint the NULL cell's own OUT** (`z->nullck` = key of the cell drawn at `nullat`; hint `lz_key(nullck, LZ_OUT) = 2`,
  a new blob): the no-NULL effect with the existing hint kind.
    - Pro: no `LZ_NUL`, no index rule, consistent with forced OUT entries (solver reruns), one field in `Lazy`.
    - Con: same effect as option 3 (the list loses its end, stop moves to the budget); cannot move the NULL later, since a later cell's key does not exist yet.

Searched: a retry derived from the seed or retry counter (not logged, so replay and solver reruns would see another
`nullat`; making it logged is the same key again); statically dropping the NULL for pointer-subtracting functions
(loses every list end up front); naming the NULL operand's cell (`sub_a` is 0, so no blob names it; option 4 stores the
cell key at fill time instead). Removed: nothing from options 1–3; option 4 is the only one with fewer concepts.

## 4. Checks and exit

- **Where:** `lazy_call1`, `lazy_redraw`, `LZ_NUL` in `src/c/eval_run.c`; a fixture (a function subtracting two
  pointer fields where the NULL draw can land on either); `tests/eval_lazy.sh`. **Replaces/deletes:** nothing.
- **Spike first** (cheapest prototype, throwaway build, not committed): options 1, 2, 3 and 4 on the probe,
  `luaD_reallocstack` 24 calls seed 1234 `--eval-link` (today 1 of 24 finish). If no option finishes more than 1 of 24,
  the idea is decided against in `PARKED.md` row 248 and nothing is built.
- Fixture: finishes the calls whose NULL draw lands on a subtracted field (today they stop for good).
- Sweep: `sweep_meas.sh` seed 1234 `LINK=1` N=24: edges at least 6541 (size share), seconds not above 1896 by more than
  10%, solver-first edges not below 1968; replay (`CEREAL_LAZY_REPLAY=1`) and the gate green.
- Exit: if the sweep does not beat 6541 by more than the seed pair's noise, record it and revert to `22ac39a`.

## 5. Result (spike, 2026-11-09): decided against

Throwaway build of `cc9a666` with `CEREAL_SPIKE=1..4` (options 1-4; 0 = today), `luaD_reallocstack`, 24 calls, seed 1234,
`--eval-link` on the Lua 5.4 sources. Final stops of all generator runs (88 per mode):

| mode | finished | subtraction | blob budget | other traps |
|---|---|---|---|---|
| 0 today | 1 | 47 | 36 | 5 |
| 1 next unreached draw | 1 | 23 | 52 | 13 |
| 2 one draw later | 1 | 37 | 38 | 13 |
| 3 no NULL | 1 | 36 | 50 | 2 |
| 4 OUT hint on the NULL cell | 1 | 36 | 45 | 7 |

No option finishes more than 1 of 24 (the exit bar above). Option 1 removes half the subtraction stops, but the freed
calls run on into the budget or a NULL dereference; the lists have no end without the NULL. Nothing was built; the spike
tree and outputs were deleted. Parked: `PARKED.md` row 248.
