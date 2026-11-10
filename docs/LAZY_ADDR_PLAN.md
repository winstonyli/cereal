---
status: draft
---

# Lazy blobs: a pointer read as an integer — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (inline, three tasks). Steps use checkbox syntax.

**Goal:** a sweep call that converts a lazy-blob pointer to an integer runs on and agrees with gcc.

**Architecture:** both sides place lazy blob `k` at `SW_BASE + k * SW_STRIDE` (`src/c/sw_addr.h`). The evaluator's `convert` returns that address for a lazy pointer, and `ev_access` stops (`UNSUP`) on any pointer value in that range, however it got there (cast, union, memory); the gcc driver `mmap`s the range.

**Spec:** `docs/LAZY_ADDR_DESIGN.md` (choices l1 = p1 + i1, chosen 2026-11-10). Follows `docs/LAZY_REALLOC_PLAN.md` (same wrappers, same fixture style).

## Global Constraints

- No draw, hint, trap-record or call-line change: recorded sweeps and `CEREAL_LAZY_REPLAY` stay valid.
- `SW_BASE` = `0x400000000000` (pid field 16384, 47 bits: mmap-able, below the PIE image at `0x55..`; replaces the spec's `0x200000000`, whose pid field 2 would name a real object: ruling from the plan review), `SW_STRIDE` = `0x1000000` (16 MiB), at most `LMAXBL` = 32 blobs (512 MiB virtual, `MAP_NORESERVE`).
- Only pointers into lazy objects get a model address; every other pointer-to-integer conversion keeps its `UNSUP`.
- `tests/asm_exec.py` includes `sweep_rt.h` and links with plain `gcc`: it must still build.
- Tests run in WSL (`wsl -e bash -lc`, tree `~/cereal-s1`, `bash ~/cereal-s1/.sync.sh`, `make -j4`); runs over a minute go through `run.ps1` (RUNS.md). Do not push or tag.
- Exit bar: seed 1234 edges taken >= 6601 + 60 = 6661, seed 7 >= 6575 + 60 = 6635; otherwise report and ask (no silent revert). Spike upper bound was +129 / +113 with an unsound model.

## Review Focus

- Two lazy pointers from different blobs subtracted as integers (`la_diff`): equal on both sides.
- `p & 63`, `p >> 4` stored in a field (`la_align`, `la_hash`): same bytes on both sides.
- An integer cast back to a pointer, or stored and reloaded as a pointer (`la_back`, `la_pun`), then dereferenced: the call stops (`UNSUP`) in `ev_access`; it never accesses a real object. A NULL `a` still finishes (the NULL draw), so the test compares against a control (`la_ctl`) instead of expecting 0.
- A negative offset into a lazy blob (`p[-1]`): signed arithmetic, same integer on both sides, no `UNSUP`.
- A pointer into a non-lazy object (the function's own `malloc`) converted to an integer: still `UNSUP`.
- A second call in one non-fork driver process: `munmap` frees the range (the next `mmap` at `SW_BASE` succeeds).
- Library mode (`lib_oracle.sh`, `SW_FORK`, `ulimit -S -v`): the 512 MiB range fits.

## End-state fit

Evaluator: `convert` and `ev_access` in `src/c/eval.c`, `Obj.lazy` in `eval_int.h`, `lazy_new` in `eval_run.c`; model constants in new `src/c/sw_addr.h`; gcc side `sw_load`/`sw_out` in `tests/sweep_rt.h`. Replaces: `UNSUP "address as an integer"` for lazy pointers; the `calloc` arena and its per-blob padding arithmetic; `Obj.lazy` as a bool. Deletes: nothing else. Parked afterwards: map-back of integers to pointers (i2), addresses of non-lazy objects.

### Task 1: evaluator

**Files:** Create `src/c/sw_addr.h`, `tests/cfg_lazy/lazy_addr.c`; Modify `src/c/eval_int.h` (Obj, ~51), `src/c/eval_run.c` (`lazy_new` ~851), `src/c/eval.c` (`convert` ~541, `ev_access` ~184), `tests/eval_lazy.sh` (append); the field comment at `eval_int.h:51` changes with the type. Also add `src/c/sw_addr.h` to whatever `.sync.sh` copies, if it lists files.

**Interfaces:** Produces `SW_BASE`, `SW_STRIDE`, `SW_ADDR(k, off)` (macros in `sw_addr.h`), `Obj.lazy` as `uint32_t` (blob index + 1, 0 = not lazy); fixture functions `la_align`, `la_diff`, `la_hash`, `la_ctl`, `la_back`, `la_pun`, `la_own` that Task 2 compares with gcc.

- [ ] **Step 1: fixture** `tests/cfg_lazy/lazy_addr.c` (no includes), `struct LA { char *a; char *b; unsigned long v; };`:
  `unsigned long la_align(struct LA *s)` returns `(unsigned long)s->a & 63`;
  `unsigned long la_diff(struct LA *s)`: `if (!s->a || !s->b) return 0;` then `(unsigned long)s->b - (unsigned long)s->a` (usually two blobs, sometimes one);
  `unsigned long la_hash(struct LA *s)` stores `s->v = (unsigned long)s->a >> 4` and returns `s->v + 1`;
  `int la_ctl(struct LA *s)`: `if (!s->a) return 0; return *s->a;` (control);
  `int la_back(struct LA *s)`: `if (!s->a) return 0; return *(char *)((unsigned long)s->a & ~63ul);`;
  `int la_pun(struct LA *s)`: `if (!s->a) return 0; s->v = (unsigned long)s->a; return **(char **)&s->v;`;
  `unsigned long la_own(struct LA *s)`: `char *p = malloc(8); return (unsigned long)p;` (declare `void *malloc(unsigned long);`).
- [ ] **Step 2: test (red).** Append to `tests/eval_lazy.sh`: for `la_align la_diff la_hash`, `grep -ac "^call NAME .* =>"` of `--eval-sweep=24,1234` must be 24 (message `eval_lazy: lazy_addr.c: only $n of 24 calls of NAME finished (address as an integer)`); `la_ctl` must finish 24; `la_back` and `la_pun` must each finish fewer than `la_ctl` (message `eval_lazy: lazy_addr.c: NAME ran past a model address used as a pointer`); `la_own` must finish 0; then `echo "eval_lazy: lazy address ok"`. Run `sh tests/eval_lazy.sh ~/cereal-s1/cereal`. Expected: FAIL on `la_align` with n below 24 (stop "address as an integer").
- [ ] **Step 3: implement.** `sw_addr.h`: `#define SW_BASE 0x400000000000ull`, `#define SW_STRIDE 0x1000000ull`, `#define SW_END (SW_BASE + 32 * SW_STRIDE)`, `#define SW_ADDR(k, off) (SW_BASE + (unsigned long long)(k) * SW_STRIDE + (unsigned long long)(long long)(off))` (plain types, no `<stdint.h>`; the offset is signed), one comment pointing to the spec. `Obj.lazy` becomes `uint32_t`; `lazy_new` sets `= (uint32_t)z->bl.len` (after `vec_push`, index + 1). `convert`: a pointer into an object with `lazy != 0` gives `SW_ADDR(lazy - 1, poff(a))` (`poff` is signed: do not cast it unsigned); other objects keep the `UNSUP`. The one stop for the way back is in `ev_access`: before the `pid(p) == 0` test, `p >= SW_BASE && p < SW_END` raises `UNSUP "a model address used as a pointer"` and returns NULL (this replaces the earlier idea of a check in `convert`, and covers casts, unions and memory round trips). The integer carries no shadow. Check that `ev_new_obj` zero-initializes `lazy` for a reused slot; add the reset if not.
- [ ] **Step 4: test (green).** Sync, `make -j4`, rerun `eval_lazy.sh`. Expected: `eval_lazy: lazy address ok`, earlier sections ok. `la_own` (a pointer into the function's own block) still stops.
- [ ] **Step 5: commit** the files above: "Lazy blobs: a pointer into a lazy blob converts to a model address in the evaluator".

### Task 2: gcc side

**Files:** Modify `tests/sweep_rt.h` (`sw_load` 49, `sw_out` 115, includes, comment 20), Test `tests/eval_lazy.sh`.

**Interfaces:** Consumes `sw_addr.h` and the Task 1 fixture. Produces blob `i` at `SW_ADDR(i, 0)` in `g->b[i]`.

- [ ] **Step 1: test (red).** Append to `tests/eval_lazy.sh`: `sh "$D/eval_sweep.sh" "$C" "$D/cfg_lazy/lazy_addr.c"` must exit 0 (`eval_lazy: lazy_addr.c differs from gcc`), then `echo "eval_lazy: lazy address agrees with gcc"`. Expected: FAIL (`la_align` differs: the arena's alignment is `calloc`'s).
- [ ] **Step 2: implement.** `#include "../src/c/sw_addr.h"` (`sweep_rt.h` is included by absolute path from `tests/`, so the relative include resolves; no `-I`) and `<sys/mman.h>`; `sw_load`: `mmap((void *)SW_BASE, n * SW_STRIDE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0)`, `abort()` unless it returns exactly `SW_BASE`; `g->arena` is that range, `g->b[i] = g->arena + i * SW_STRIDE`; `sw_alo`/`sw_ahi` = the range; the arena padding sum is deleted; `SW_SLACK` stays for `sw_tag`. `sw_out`: `munmap(g->arena, g->n * SW_STRIDE)` where `free(g->arena)` was. Update the comment at line 20.
- [ ] **Step 3: green + regressions.** `eval_lazy.sh` fully; the `cfg_exec`, `cfg_link`, `cfg_state` sweeps and `asm_exec.py` as `tests/run.sh` 13e-13f call them; a calls file with 3 calls in one non-fork process (the `eval_sweep.sh` default) proves the `munmap`. Expected: all pass.
- [ ] **Step 4: library mode.** `bench/tools/lib_oracle.sh ~/cereal-s1/cereal zstd 8`: 0 differ, 0 gcc crashed, finished calls not below 45321 (the realloc run).
- [ ] **Step 5: commit** `tests/sweep_rt.h tests/eval_lazy.sh` (and `eval_sweep.sh` if touched): "Sweep driver: lazy blobs at the model addresses".

### Task 3: measure and close

**Files:** Modify `docs/LAZY_ADDR_DESIGN.md` (status done, §6 result), `PARKED.md` row 248, `docs/runs.md`.

- [ ] **Step 1: sweeps.** `run.ps1 -Kind bench -Name addr-s1234 -Detach` and `-Name addr-s7` (`LINK=1`, `SEED=7`), `sweep_meas.sh ... 24`, watcher per RUNS.md. Expected: seed 1234 >= 6661, seed 7 >= 6635, and the sweep's own agreement check shows 0 differ. A new difference is a finding: record it, do not mask it.
- [ ] **Step 2: replay and gate.** `CEREAL_LAZY_REPLAY=1` on the seed-1234 recordings; `-Kind gate -Command "wsl -e bash /home/winstonyli/rungate.sh"`. Expected: replay draws equal, gate >= 2406 passed.
- [ ] **Step 3: docs.** Spec and this plan `status: done` with §6 result and run ids; `PARKED.md` row 248: address model landed, numbers, remaining (i2 map-back, non-lazy addresses, moved-pointer branches, `luaD_reallocstack` subtraction stop); grep docs for "address as an integer" and the old wording.
- [ ] **Step 4: commit** docs and runs rows. No push.

## Self-review

Spec coverage: §2.1 Task 1 header, §2.2 Task 1, §2.3 Task 2, §2.4 Task 1 (`la_back`), §2.5 constraints; §4 fixture, bars, `lib_oracle`, replay, `asm_exec`, second-call test: Tasks 1-3. Names (`SW_ADDR`, `Obj.lazy`, `la_*`) match across tasks. Verified in review: the call line lists blobs in `Lazy.bl` order (`eval_run.c:1205-1268`, no reordering). Residual risk: calls that now run may differ from gcc for reasons outside the model (an integer used as an index or loop bound; gcc SIGSEGV vs evaluator trap): the first sweep shows them, and they are findings.
