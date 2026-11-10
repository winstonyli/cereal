---
status: done
---

# Lazy blobs that a function frees (and reallocs)

Goal: a sweep call that passes a lazy blob to `free` runs on instead of stopping, and the gcc side agrees.
Before 2026-11-09 free stopped (and realloc still stops) with "free/realloc of a pointer that malloc did not return" (`heap_obj`, `src/c/eval_libc.c:469-483`,
trap text at 477; `h_free` 486, `h_realloc` 496).
Mechanism: `h_free` in `eval_libc.c`; the gcc side `tests/sweep_rt.h` (`sw_load`, `sw_out`) and the driver link in `tests/eval_sweep.sh`.

## 1. Facts

- Untaken edges behind these traps, seed 7, Lua + zstd linked, 24 calls, base `3b5a09e` (run `2026-11-09-seed7-base-link24`,
  `out.log` lines 15, 16, 22): free 117 (`ZSTD_freeCCtxParams` and callers) + 2 (`utf8esc`), realloc 21 (`luaM_realloc_` and
  callers), of 2888 untaken (1753 Lua + 1135 zstd). An upper bound: an edge behind a freed blob is reached only if the rest of the
  function runs. The larger walls are `luaV_execute` 471 (write through a null pointer) and `HUF_*` 81 (address as an integer).
- `heap_obj` accepts only `Obj.heap` objects at offset 0. A lazy blob has `heap` false and every pointer to one starts
  `LHEAD` (128) bytes in, so `free(p)` fails twice.
- The offset of a pointer to a lazy blob is drawn for only some pointers: a cell's only on the share path (`lazy_fill`
  965-970; a new blob always gets `LHEAD`, 979-980), a parameter's only when it aliases an earlier parameter (1151-1176), and
  `lazy_off` draws it as three byte keys (781-783), not one. A hint that moves a freed blob to offset 0 would need three
  entries (all of `hint[3]`) and a new draw on the new-blob and plain-parameter paths (a change of the draw stream and of every
  recorded replay), and offset 0 is outside the solver's domain (`dom`, 793). This is why the offset route is dropped (Decide a2).
- gcc side: `sw_load` makes each blob `calloc(size + SW_SLACK + 16)` and links cells to `g->b[i] + off`; `sw_out` copies every
  blob's `size[i]` bytes and then `free(g->b[i])`. The driver `#include`s the function's source and `sweep_rt.h` into one
  translation unit (`eval_sweep.sh:78-82`; the plain object is separate); `--wrap=free` applies to the undefined symbol, so it
  reaches the function's calls, but also `sw_load`/`sw_out`, which would call `__real_free`. `sweep_rt.h` is included by every
  driver, so the wrapper is weak or defined once.
- `ev_kill_obj` frees `Obj.b`, `init` and `sh`; the call-line output code reads them (`memcpy(out+n, o->b, o->size)`,
  ~eval_run.c:1228-1245), so a killed lazy blob would crash it. A no-op free keeps the blob alive and avoids that code.

## 2. Design (a1: free on a lazy blob does nothing, on both sides)

1. **Evaluator:** in `h_free`, a pointer whose object is lazy (`Obj.lazy`, any offset, including `LHEAD`) returns 0: no kill,
   no trap, no hint. A non-lazy pointer is checked as today (heap, offset 0, live).
2. **gcc side:** `sw_load` takes the call's blobs from one arena (one `calloc`, blobs consecutive, `SW_SLACK + 16` apart, as now),
   and the driver links with `-Wl,--wrap=free`: `__wrap_free(p)` returns if `p` lies in the current call's arena range, else
   `__real_free(p)`. `sw_out` and `sw_load` free the arena and the tables with `__real_free`. The function's own `malloc`/`free`
   pairs are untouched (their pointers are outside the arena).
3. **What both sides do afterwards:** a freed blob keeps its bytes. A read after the free returns the old bytes on both sides
   (undefined in C, defined here), so the line is the same on both; a second free is a no-op on both (the evaluator would
   otherwise trap "already freed"); a free of a pointer into the middle of a blob is a no-op on both (glibc would abort).
4. **Check that must hold:** the evaluator prints no extra stop for a call that frees and then returns, and `plain`/`drv`
   outputs are byte-equal on the fixture (Checks).

**Cannot cover.** `realloc` of a lazy blob (built later: `docs/LAZY_REALLOC_DESIGN.md`); a free reached through a drawn function pointer
(`customFree` drawn non-NULL: the call goes to that function, as today); use-after-free and double-free detection on lazy blobs
(lost by design: a sweep compares the evaluator with gcc, it does not hunt for undefined behaviour); a blob whose bytes
the function overwrites after freeing it (UB in gcc, defined as the same bytes here, so the comparison is still valid).

## 3. Decide

**Decide (scope and shape)**

- a1 **No-op free on both sides** (recommended): items 1-4 above.
    - Pro: about 119 untaken edges behind the trap; no draw, hint, trap record, print or format change; about five lines in `h_free`
      and a short wrapper plus an arena in `sweep_rt.h`.
    - Con: leaves the 21 realloc edges and `luaD_reallocstack`; use-after-free and double free on a lazy blob no longer stop.
- a2 **Offset-0 free that kills the blob**: the hint moves the freed blob's incoming pointer to offset 0, `heap_obj` accepts a lazy blob
  there, `h_free` kills it, the output prints zeros for it, the wrapper marks it freed.
    - Pro: keeps use-after-free and double-free detection.
    - Con: a 3-entry hint (all of `hint[3]`), a new offset draw on the new-blob and parameter paths (every replay changes), a new trap
      record in `Ev`, a freed-blob print branch (the output code crashes on a killed blob otherwise), a per-blob freed table on the gcc side;
      conflicts with the solver's domain.
- a3 **a1 plus realloc**: also `__wrap_realloc` and a result format that carries a replacement blob.
    - Pro: finishes the realloc callers (21 edges, `luaD_reallocstack`).
    - Con: changes the line format (`size[n]` per blob and the output of a replaced blob), `sw_out`, replay; reads of old undrawn bytes after the
      move stop as uninitialized. Needs its own spec after a1.
- a4 **Park**: record the measurement in `PARKED.md` and stop.
    - Pro: no work.
    - Con: the 119 edges stay lost.

Searched: lazy blobs with `heap = true` at creation (then `free(p)` succeeds on interior pointers gcc aborts on; a1 makes that
agree instead); blobs starting at the malloc start so `b[i] + LHEAD` is a valid free (the `LHEAD` bytes before the pointer must be
owned memory, so the wrapper is still needed, and only frees at `LHEAD` itself would pass: fewer than a1); all parameter blobs at
offset 0 (cannot cover cell pointers such as `ctx->field`, and drops the room before the pointer). Removed from the first draft:
the offset hint, the `lazy_redraw` branch, the trap record, the freed print rule and the zero fill (a1 needs none of them).

## 4. End-state fit, checks and exit

- **Where:** `h_free` (`eval_libc.c`); `sw_load`/`sw_out` and `__wrap_free` (`tests/sweep_rt.h`); `-Wl,--wrap=free` on the driver
  link in `tests/eval_sweep.sh` (and the library oracle link, `lib_oracle.sh`, not yet read). **Replaces:** the "malloc did not return"
  stop for lazy blobs. **Deletes:** nothing.
- **Spike first** (throwaway, evaluator side only): the `h_free` early return on `ZSTD_freeCCtxParams` (24 calls, seed 1234,
  `--eval-link`; today every call that frees stops): finished calls and zstd edges. The gcc side is not needed to size the gain,
  but is needed before the change can land.
- Fixture `tests/cfg_lazy/lazy_free.c`: a function that frees its parameter and a pointer field's target, then returns a field read
  before the free; `eval_sweep.sh` (gcc comparison) must pass and `eval_lazy.sh` must see the calls finish.
- Sweep: `sweep_meas.sh` seeds 1234 and 7: Lua edges at least the size-share numbers (6541 / 6526), zstd edges up; replay
  (`CEREAL_LAZY_REPLAY=1`) and the gate green.
- Exit (as written): if the zstd edges do not rise by more than 20 over base, revert and record it in `PARKED.md` row 248. Kept anyway on 2026-11-09 by the user (§6).

## 5. Spike result (2026-11-09, evaluator side only)

`h_free` returns early for a lazy object (throwaway build of `b520f83`). Probe `ZSTD_freeCCtxParams`, 24 calls, seed 1234,
`--eval-link`: finished 88 of 88 runs, base 0 (all 88 stopped on "free of a pointer that malloc did not return"). Full sweep, seed
1234, Lua + zstd linked, 24 calls (`2026-11-09-heapspike-link24` against the size-share run `2026-11-09-sizeshare-link24`, same
generator code): edges taken 6583 (+42), solver-first 1987 (+19), untaken zstd 1146 -> 1131 (-15), Lua 1719 -> 1685 (-34),
"function never finished" Lua 237 -> 116, zstd 737 -> 706; seconds 2659 against 1896 (machine at 80-95% CPU from other work; not a
timing). Realloc stops left: 9 edges. The exit bar above (zstd edges up by more than 20) is not met by zstd alone (+15); the total is +42.
The gcc side is not built; the spike tree is deleted.

## 6. Result (2026-11-09, built: `h_free` no-op, sweep arena and `--wrap=free`)

Plan `docs/LAZY_HEAP_PLAN.md`; fixture `tests/cfg_lazy/lazy_free.c` (all three functions finish and agree with gcc). Lua + zstd linked, 24 calls,
against the size-share runs (same generator): seed 1234 edges 6541 -> 6583 (+42; zstd untaken 1146 -> 1131, Lua 1719 -> 1685; run
`2026-11-09-heap-s1234`, 2156 s); seed 7 6526 -> 6541 (+15; zstd untaken 1151 -> 1168, Lua 1758 -> 1722; run `2026-11-09-heap-s7`, 2109 s).
Replay equal; gate 2405 passed; `lib_oracle.sh zstd 8`: 45321 calls finished, 0 differ, 0 gcc crashed. The exit bar (zstd +20) is met by neither seed;
the user kept the change for the Lua gain, which holds on both seeds (-34, -36), and the "function never finished" count roughly halving. The zstd
loss at seed 7 is unexplained (no third seed). Realloc of a lazy blob: built 2026-11-10 (`docs/LAZY_REALLOC_DESIGN.md`).
