---
status: draft
---

# Lazy blobs that a function reallocs

Goal: a sweep call that passes a lazy blob (or a pointer into one) to `realloc` runs on instead of stopping, and the gcc side agrees.
Follows `LAZY_HEAP_DESIGN.md` (free as a no-op, built 2026-11-09), which listed this as a3 and expected a line-format change; this
spec finds a shape that needs none.
Mechanism: `h_realloc` (`src/c/eval_libc.c:498`), `ev_access` (`src/c/eval.c:220`, grows a lazy blob); gcc side `__wrap_free` next to it.

## 1. Facts

- Untaken edges behind the stop, seed 1234 after the free change (run `2026-11-09-heap-s1234`, `out.log` line 11): **9** ("realloc of a
  pointer that malloc did not return", via `exp2RK`, `readutf8esc`, `close_func`: `luaM_realloc_` callers). Before the free change 21
  (seed 7 base). The per-link NULL spike made `luaD_reallocstack` reach this stop in 29-41 of 88 runs, but that spike is not built
  (`LAZY_NULL_HINT_DESIGN.md` §5, `PARKED.md` row 248), and `luaD_reallocstack` itself still stops first on a subtraction (15 edges, line 14).
  So the measured value today is 9 edges plus what runs after them.
- `h_realloc` checks `NULL`, then `heap_obj` (which traps on a lazy pointer: it needs `heap` and offset 0), then size zero (`UNSUP`), then
  `ev_new_obj(v, n, false)` and raw `memcpy` of `b`, `init`, `sh`, then kills the old object.
- A lazy blob grows on any access past its end (`ev_access`, `eval.c:220`, up to `EV_LAZY_CAP`), and the call line carries the blob's *final*
  size (`lazy_call1`, `eval_run.c`), so the gcc arena (`LAZY_HEAP_DESIGN.md` §2) is already as large as the blob ever got.
- gcc side: `realloc` of a pointer into the arena (every lazy pointer starts `LHEAD` = 128 bytes in) is an invalid realloc and aborts.
  `--wrap=free` with `sw_alo`/`sw_ahi` already exists (`tests/sweep_rt.h`, `drvmain.c` in `tests/eval_sweep.sh`).

## 2. Design (b0: realloc of a lazy pointer grows the blob in place and returns the same pointer)

1. **Evaluator:** in `h_realloc`, after the `NULL` case and before `heap_obj`, a pointer into a lazy object: size zero is `UNSUP` as today;
   else `ev_access(v, a[0].x, a[1].x, "realloc")` (grows the blob to `poff + n`, traps at the cap), return `a[0].x`. The old object is
   not killed. The lazy test is the one `h_free` has: one `lazy_ptr(v, p)` helper used by both.
2. **gcc side:** `__wrap_realloc(p, n)` in `drvmain.c`: `if (p in [sw_alo, sw_ahi)) return p;` else `__real_realloc(p, n)`; the link adds
   `-Wl,--wrap=realloc`. The blob is already as large as the evaluator made it, so nothing moves and nothing is copied.
3. **What both sides do afterwards:** the blob keeps its bytes and its place; bytes the function writes into the grown tail are in the
   blob and are compared like any other. No draw, hint, trap record, print or line-format change; recorded replays stay valid.

**Cannot cover.** The "realloc moved the block" path (`p2 != p` fixups, `luaM_realloc_` callers' relocation branches) is never taken on
either side; a grow past `EV_LAZY_CAP` traps; size zero stays unsupported; a function that relies on the old pointer being dead after a
moved realloc (undefined to keep using, not in the corpus); a read of the grown tail draws a value like any untouched lazy byte (C leaves it indeterminate), and the call line
carries the drawn bytes as initial bytes, so gcc reads the same.

## 3. Decide

**Decide (shape)**

- x1 **Grow in place** (recommended): items 1-3 above.
    - Pro: fewest concepts (no new block, no blob table, no copy, one lazy helper shared with `free`); compares the grown bytes; no format change.
    - Con: the moved-pointer branches stay untaken; the gcc wrapper never returns a moved pointer.
- x2 **Copy into a plain heap block, the blob stays**: `ev_new_obj` + `ev_copy_n` of `min(n, size - poff)` bytes; gcc `__wrap_realloc` mallocs and copies from the
  blob found through a published table (`sw_bp`/`sw_bs`/`sw_bn`).
    - Pro: takes the moved-pointer branches.
    - Con: the table; `ev_copy_n` pins the copied bytes' zeros in the solver (`ev_lazy_zero`: `lazy_zero` for 16 bytes or fewer) and marks the source
      blob's `init`, so it is not "no draw"; it stops (UNSUP) when the copy cuts a drawn pointer or starts unaligned over one; the moved block is not compared;
      a blob grown after the realloc is copied at its then-size on one side and its final size on the other.
- x3 **A replacement blob in the output** (the old a3): the moved block becomes a new blob in the call line, compared byte for byte.
    - Pro: moved-pointer branches and a compared block.
    - Con: a line-format change (recorded sweeps, replay, `sw_out`, blob numbering, `LMAXBL`).
- x4 **Park**: leave the 9 edges.
    - Pro: no work, no risk to the shared test runtime.
    - Con: the 9 edges and anything past them stay lost.

Searched: one shared mechanism for free and realloc (found: both become "a lazy blob stays where it is", one predicate on each side;
`h_free` and `h_realloc` share `lazy_ptr`, `__wrap_free` and `__wrap_realloc` share the arena test); a trap-free "return NULL" (every caller takes
its out-of-memory path: the opposite of the coverage wanted); `free` plus `malloc` with no copy (changes what the function reads back). Removed
from the first draft (b1, now x2): the plain-block copy as the default, the blob table, the grow-after-realloc caveat. The reviewer found
the in-place shape; x2 and x3 stay listed.

## 4. End-state fit, checks and exit

- **Where:** `h_realloc` and a shared `lazy_ptr` helper in `eval_libc.c`; `__wrap_realloc` and `-Wl,--wrap=realloc` in `eval_sweep.sh`.
  **Replaces:** the "malloc did not return" stop for lazy pointers; the lazy test inlined in `h_free`. **Deletes:** nothing else.
- **Spike first** (throwaway, evaluator side only, item 1): seed 1234 sweep, Lua + zstd linked, 24 calls (`sweep_meas.sh`, `LINK=1`)
  against `2026-11-09-heap-s1234`: the "realloc ... did not return" line gone and edges up. If edges rise by 5 or fewer, record it in
  `PARKED.md` row 248 and stop. The gcc side is not needed to size the gain.
- Fixture `tests/cfg_lazy/lazy_realloc.c`: a function that `realloc`s a drawn pointer to a larger size, writes the new tail, reads a
  field of the old part, and returns it; `eval_lazy.sh` (all 24 calls finish) and `eval_sweep.sh` (agrees with gcc).
- Sweep seeds 1234 and 7, replay (`CEREAL_LAZY_REPLAY=1`), `lib_oracle.sh zstd 8` (0 differ, 0 gcc crashed), the gate.

## 5. Spike result (2026-11-09, evaluator side only, x1)

`h_realloc` grows a lazy blob in place with `ev_access` and returns the same pointer (throwaway build of the tree at the spec's commit, tree
deleted). Seed 1234, Lua + zstd linked, 24 calls, run `2026-11-09-reallocspike` against `2026-11-09-heap-s1234` (same generator): edges taken
6583 -> 6601 (+18, bar: more than 5); the "realloc of a pointer that malloc did not return" line is gone (it named 9 edges); zstd untaken
1131 -> 1108 (-23), Lua unchanged at 1685 (the 9 edges were in zstd's callers as well, not only Lua's); the subtraction stop of
`luaD_reallocstack` 15 -> 12 edges; solver-first 1987 -> 1986; seconds 2156 -> 2251 (machine load from other work; not a timing). The gcc side
(`__wrap_realloc`, `-Wl,--wrap=realloc`), the fixture, seed 7, replay and the gate are not done.
