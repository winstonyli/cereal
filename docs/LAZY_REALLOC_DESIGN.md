---
status: draft
---

# Lazy blobs that a function reallocs

Goal: a sweep call that passes a lazy blob (or a pointer into one) to `realloc` runs on instead of stopping, and the gcc side agrees.
Follows `LAZY_HEAP_DESIGN.md` (free as a no-op, built 2026-11-09), which listed this as a3 and expected a line-format change; this
spec finds a shape that needs none.
Mechanism: `h_realloc` (`src/c/eval_libc.c:496`), `ev_copy_n` (`src/c/eval.c:446`); gcc side `tests/sweep_rt.h`, `drvmain.c` in `tests/eval_sweep.sh`.

## 1. Facts

- Untaken edges behind the stop, seed 1234 after the free change (run `2026-11-09-heap-s1234`, `out.log` line 11): **9** ("realloc of a
  pointer that malloc did not return", via `exp2RK`, `readutf8esc`, `close_func`: `luaM_realloc_` callers). Before the free change 21
  (seed 7 base). The per-link NULL spike made `luaD_reallocstack` reach this stop in 29-41 of 88 runs, but that spike is not built
  (`LAZY_NULL_HINT_DESIGN.md` §5, `PARKED.md` row 248), and `luaD_reallocstack` itself still stops first on a subtraction (15 edges, line 14).
  So the measured value today is 9 edges plus what runs after them.
- `h_realloc` accepts only a `heap` object at offset 0; it copies `b`, `init` and `sh` by `memcpy`, then kills the old object. The same three
  copies are what `ev_copy_n` does (plus the access checks, the lazy-source rules and `ev_lazy_zero` for untouched lazy bytes).
- Output: the call line carries the final bytes of every *blob* (`lazy_call1`, `eval_run.c` ~1223-1247); a block the function mallocs is not
  a blob, is not compared, and a pointer to it stored in a blob is a "zero cell" (zeroed on both sides, `eval_run.c` 1235-1243). This is
  how the function's own malloc results already compare.
- gcc side: `realloc` of a pointer into the arena (every lazy pointer starts `LHEAD` = 128 bytes in) is an invalid realloc and aborts.
  `LAZY_HEAP_DESIGN.md` §2 already routes `free` through `--wrap=free` with `sw_alo`/`sw_ahi`.

## 2. Design (b1: realloc of a lazy pointer copies into a plain heap block; the blob stays)

1. **Evaluator:** in `h_realloc`, after the `NULL` case and before `heap_obj` (which traps on a lazy pointer), a pointer into a lazy object: size zero is unsupported as today (the existing `UNSUP`), else `id = ev_new_obj(v, n)`,
   `heap = true`, `keep = min(n, size - poff)` (0 when `poff >= size`), `ev_copy_n(v, mkptr(id, 0), a[0].x, keep)`; the old blob is not
   killed (as for `free`); return `mkptr(id, 0)`. Not-lazy objects take the existing path. Deletion pass: replace the three raw `memcpy`
   there by the same `ev_copy_n`, so both branches share one copy (only if the sweep, `cfg_exec` and the gate stay equal; otherwise the
   lazy branch keeps its own call).
2. **gcc side:** `sw_load` publishes the blob table (`sw_bp`, `sw_bs`, `sw_bn`: the `g->b`, `g->size`, `g->n` it has, weak globals like
   `sw_alo`), `sw_out` clears them. `drvmain.c` gets
   `__wrap_realloc(p, n)`: if `p` lies in blob `i` (`b[i] <= p < b[i] + size[i] + SW_SLACK`), return a `malloc(n)` block holding
   `min(n, b[i] + size[i] - p)` bytes copied from `p` (none if `p` is past the end), else `__real_realloc(p, n)`; the link adds
   `-Wl,--wrap=realloc`.
3. **What both sides do afterwards:** the old blob keeps its bytes (a read through the old pointer agrees); the new block is the
   function's own (freed by its own `free`, or leaked, as for any malloc). Undrawn lazy bytes copy as the zeros both sides hold
   (`ev_lazy_zero`; the arena is zero-filled), so a typed read of a copied field reads zero on both. No draw, hint, trap record, print or
   line-format change; recorded replays stay valid.

**Cannot cover.** The final contents of the moved block are not compared (only the old blob's, unchanged, and what the function stores
elsewhere); a lazy blob whose `size` grows after the realloc (a string read extends it, `eval_libc.c:63`): the evaluator copied
`size - poff` at the time and gcc copies the final size, which differs only if the function reads the copy past the old size; a
realloc to size zero (stays unsupported); a pointer stored in the copy that was an undrawn field.

## 3. Decide

**Decide (shape)**

- x1 **Copy into a plain block, the blob stays** (recommended): items 1-3 above.
    - Pro: no format change, no replay change; about 8 evaluator lines and a short `__wrap_realloc`; reuses the free change's arena.
    - Con: the moved block's contents are not compared; value is the 9 edges and what follows them.
- x2 **A replacement blob in the output** (the old a3): the realloc'd block becomes a new blob in the call line (`size[n]` and a
  replacement entry), compared byte for byte.
    - Pro: compares the moved block too.
    - Con: a line-format change (recorded sweeps, replay, `sw_out`, the solver's blob numbering), blob-count growth against `LMAXBL`.
- x3 **Park**: leave the 9 edges.
    - Pro: no work, no risk to the shared test runtime.
    - Con: the 9 edges and anything past them stay lost.

Searched: a trap-free "return NULL" (realloc fails) for a lazy pointer (cheap, but every caller then takes its out-of-memory path, which
is the opposite of the coverage wanted, and gcc would have to return NULL too); growing the blob in place (the blob table is fixed
at load on the gcc side); treating `realloc(p, n)` as `free(p)` plus `malloc(n)` with no copy (changes what the function reads
back, and gcc would copy nothing while a real realloc copies). Removed from the first draft: the replacement-blob output (x2 stays
listed), the per-blob freed state.

## 4. End-state fit, checks and exit

- **Where:** `h_realloc` (`eval_libc.c`); `sw_load`/`sw_out` and the table in `sweep_rt.h`; `__wrap_realloc` and `-Wl,--wrap=realloc` in
  `eval_sweep.sh`. **Replaces:** the "malloc did not return" stop for lazy pointers; the raw copies in `h_realloc` if the unification
  holds. **Deletes:** nothing else.
- **Spike first** (throwaway, evaluator side only, `h_realloc` lazy branch of item 1): seed 1234 sweep, Lua + zstd linked, 24 calls
  (`sweep_meas.sh`, `LINK=1`) against `2026-11-09-heap-s1234`: edges up by more than 9 or the "realloc ... did not return" line gone. If
  edges rise by 5 or fewer, record it in `PARKED.md` row 248 and stop. The gcc side is not needed to size the gain.
- Fixture `tests/cfg_lazy/lazy_realloc.c`: a function that `realloc`s a drawn pointer to a larger size, writes the new tail, reads a field
  copied from the old block, and returns it; `eval_lazy.sh` (all 24 calls finish) and `eval_sweep.sh` (agrees with gcc).
- Sweep seeds 1234 and 7, replay (`CEREAL_LAZY_REPLAY=1`), `lib_oracle.sh zstd 8` (0 differ, 0 gcc crashed), the gate.
