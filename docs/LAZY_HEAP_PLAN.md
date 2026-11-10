---
status: draft
---

# Lazy blobs: free as a no-op on both sides — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (inline, three small tasks). Steps use checkbox syntax.

**Goal:** a sweep call that frees a lazy blob (or a pointer into one) runs on in the evaluator and agrees with gcc, so `ZSTD_freeCCtxParams` and its callers finish.

**Architecture:** `h_free` returns 0 for a pointer into a lazy object. On the gcc side `sw_load` takes the call's blobs from one arena and the driver links with `-Wl,--wrap=free`; `__wrap_free` ignores arena pointers and passes the rest to the real `free`.

**Spec:** `docs/LAZY_HEAP_DESIGN.md` (a1, chosen 2026-11-09). Spike: its §5.

## Global Constraints

- Exit bar as the user accepted it on `t1`: total edges up over base, with zstd up (spike: +42 total, +15 zstd; the spec's "zstd +20" is replaced by "total > +20, zstd up"; recorded in Task 3).
- No draw, hint, trap record, print or line-format change: sweep recordings and `CEREAL_LAZY_REPLAY` stay valid.
- gcc side: the function's own `malloc`/`free` pairs are untouched; `sweep_rt.h` is also included by `tests/asm_exec.py` (no `--wrap` there).
- Tests run in WSL (`wsl -e bash -lc`, tree `~/cereal-s1`, sync `bash ~/cereal-s1/.sync.sh`, `make -j4`); runs over a minute go through `run.ps1`.

## Review Focus

- Free of a pointer into the middle of a blob, a second free, and a read after the free: same bytes on both sides (fixture `lf_twice`).
- `free(NULL)` and the function's own malloc/free pair next to a lazy free: unchanged (fixture `lf_own`).
- `sw_out` frees the arena with the real `free` (arena range cleared first) and leaks nothing: run the fixture under valgrind once.
- `asm_exec.py` still passes (its thunks share `sweep_rt.h` but are not linked with `--wrap`).
- Library mode (`lib_oracle.sh`, forked children, `EVAL_SWEEP_LIB`) links through the same `eval_sweep.sh` line, so it gets the wrapper.

## End-state fit

Evaluator: `h_free` (`src/c/eval_libc.c`). gcc side: `SwG`/`sw_load`/`sw_out` and `__wrap_free` in `tests/sweep_rt.h`, `-Wl,--wrap=free` and the `drvmain.c` definition in `tests/eval_sweep.sh`. Replaces: the "malloc did not return" stop for lazy blobs and per-blob `calloc`/`free`. Deletes: nothing else. Parked afterwards: realloc of a lazy blob (spec a3).

### Task 1: evaluator — free of a lazy pointer does nothing

**Files:** Modify `src/c/eval_libc.c:486` (`h_free`); Create `tests/cfg_lazy/lazy_free.c`; Modify `tests/eval_lazy.sh` (append a section).

**Interfaces:** Consumes `Obj.lazy` (`eval_int.h:51`), `pid()`, `v->obj`. Produces: fixture functions `lf_run`, `lf_twice`, `lf_own` that Task 2 compares with gcc.

- [ ] **Step 1: fixture.** `tests/cfg_lazy/lazy_free.c` (declares `void *malloc(unsigned long); void free(void *);`, no includes):
  `struct LF { char *buf; int v; };`
  `int lf_run(struct LF *s)` reads `r = s->v`, `free(s->buf)`, `free(s)`, returns `r`.
  `int lf_twice(struct LF *s)` frees `s->buf`, frees it again, frees `s->buf + 1` only when `s->buf` is non-NULL (NULL + 1 is a bad pointer to gcc and not a trap here), then returns `s->v + (s->buf ? s->buf[0] : 0)` (read after free).
  `int lf_own(struct LF *s)` mallocs 8 bytes, stores `s->v`, reads it back, `free(NULL)`, frees the malloc'd block and `s->buf`, returns the read-back.
- [ ] **Step 2: test (red).** Append to `tests/eval_lazy.sh`, after the size-hint section, a loop over `lf_run lf_twice lf_own`: count `^call NAME .* =>` lines from `--eval-sweep=24,1234` on the fixture; fail with `eval_lazy: lazy_free.c: only $n of 24 calls of NAME finished (free of a lazy blob)` when `$n -lt 20`; then `echo "eval_lazy: lazy free ok"`. Run `sh tests/eval_lazy.sh ~/cereal-s1/cereal` in WSL. Expected: FAIL on `lf_run` with n = 0 ("free of a pointer that malloc did not return").
- [ ] **Step 3: implement.** At the top of `h_free` after `NEED(1)`: if `pid(a[0].x)` is nonzero, within `v->obj.len` and `v->obj.data[pid(a[0].x) - 1].lazy`, return 0 (comment: a lazy blob is never freed, `docs/LAZY_HEAP_DESIGN.md`).
- [ ] **Step 4: test (green).** Rebuild (sync + `make -j4`), rerun `eval_lazy.sh`. Expected: `eval_lazy: lazy free ok`, earlier sections still ok.
- [ ] **Step 5: commit** `git add src/c/eval_libc.c tests/cfg_lazy/lazy_free.c tests/eval_lazy.sh`; message "Lazy blobs: free of a lazy pointer is a no-op in the evaluator".

### Task 2: gcc side — arena and `__wrap_free`

**Files:** Modify `tests/sweep_rt.h` (`SwG`, `sw_load`, `sw_out`, a weak range); Modify `tests/eval_sweep.sh` (drvmain generation near line 114-134, link at 167); Test: `tests/eval_lazy.sh`.

**Interfaces:** Consumes Task 1's fixture. Produces: `sw_alo`, `sw_ahi` (weak `unsigned char *`, the live arena), `SwG.arena`, `__wrap_free` in `drvmain.c`.

- [ ] **Step 1: test (red).** In `tests/eval_lazy.sh` extend the stop-fixtures style loop: `sh "$D/eval_sweep.sh" "$C" "$D/cfg_lazy/lazy_free.c"` must exit 0 (message `eval_lazy: lazy_free.c differs from gcc`). Run. Expected: FAIL, gcc side aborts on `free` of an interior pointer ("the gcc build ran badly").
- [ ] **Step 2: `sweep_rt.h`.** Add `__attribute__((weak)) unsigned char *sw_alo, *sw_ahi;` beside `sw_fn`, field `unsigned char *arena` in `SwG`. In `sw_load` replace the per-blob `calloc` by one `calloc` of the sum of `(size + SW_SLACK + 16 + 15) & ~15` per blob, `b[i]` consecutive in it, `sw_alo`/`sw_ahi` set to its bounds. In `sw_out` replace the per-blob `free` by `sw_alo = sw_ahi = 0; free(g->arena);`. Update the header comment (blobs share an arena; a free of an arena pointer is ignored by the wrapper).
- [ ] **Step 3: `eval_sweep.sh`.** In the awk END block that writes `drvmain.c`, after its `#include rt` line, print into `drvmain.c`: `void __real_free(void *);` and `void __wrap_free(void *p) { if ((unsigned char *)p >= sw_alo && (unsigned char *)p < sw_ahi) return; __real_free(p); }`. Add `-Wl,--wrap=free` to the final `gcc -o "$W/drv"` link (line 167).
- [ ] **Step 4: green + regressions.** `eval_lazy.sh` fully; `tests/eval_sweep.sh` over the same small files the gate uses (`docs/S1_PLAN.md` "Closing a slice"); `python3 tests/asm_exec.py` as the repo does (grep its caller); valgrind once on `drv` for `lazy_free.c` (no leaks, no invalid free). Expected: all pass.
- [ ] **Step 5: library mode.** `bench/tools/lib_oracle.sh ~/cereal-s1/cereal zstd 8`: expected "calls finished by the evaluator" ≥ the base run, `gcc crashed` not higher.
- [ ] **Step 6: commit** `git add tests/sweep_rt.h tests/eval_sweep.sh tests/eval_lazy.sh`; message "Sweep runtime: blobs from one arena, free of an arena pointer ignored".

### Task 3: measure and close

**Files:** Modify `docs/LAZY_HEAP_DESIGN.md` (status, exit bar, §6 result), `PARKED.md` row 248, `docs/runs.md` (wrapper rows).

- [ ] **Step 1: sweeps.** `run.ps1 -Kind bench -Name heap-s1234 -Detach` and `-Name heap-s7` with `LINK=1` (and `SEED=7`), `sweep_meas.sh … 24`, watcher per RUNS.md. Expected vs size-share runs: seed 1234 edges ≥ 6583, seed 7 ≥ 6526 and zstd untaken not above base 1146 / seed-7 base.
- [ ] **Step 2: replay and gate.** `CEREAL_LAZY_REPLAY=1` on the seed-1234 recordings; `wsl -e bash /home/winstonyli/rungate.sh` as `-Kind gate`. Expected: replay equal, gate green (2405 passed or more).
- [ ] **Step 3: docs.** Spec: `status: done`, exit bar line rewritten to the bar used, §6 result with run ids; `PARKED.md` row 248: heap free landed, realloc of a lazy blob remains (trigger: 9 realloc edges, `luaD_reallocstack`); grep docs for "malloc did not return" / old row wording and fix. This plan: `status: done`.
- [ ] **Step 4: commit** (docs and runs rows). No push.

## Self-review

Spec coverage: §2.1 Task 1; §2.2 Task 2; §2.3 `lf_twice`; §2.4 Task 2 step 4 (byte-equal); §4 fixture/sweep/replay/gate/exit Task 1-3. Cannot-cover list stays in the spec. Types: `sw_alo`/`sw_ahi`/`arena` named the same in Tasks 2 and 3. Residual risk: `-Wl,--wrap=free` also redirects `free` calls inside the linked library objects in lib mode (intended: their frees of arena pointers are ignored too).
