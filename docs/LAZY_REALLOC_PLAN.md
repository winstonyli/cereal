---
status: done
---

# Lazy blobs: realloc grows in place on both sides — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (inline, three small tasks). Steps use checkbox syntax.

**Goal:** a sweep call that reallocs a lazy blob (or a pointer into one) runs on and agrees with gcc.

**Architecture:** `h_realloc` grows a lazy blob with `ev_access` and returns the same pointer; the driver links `-Wl,--wrap=realloc` and `__wrap_realloc` returns arena pointers unchanged. A shared `lazy_ptr` helper replaces the test inlined in `h_free`.

**Spec:** `docs/LAZY_REALLOC_DESIGN.md` (x1, chosen 2026-11-09; spike §5: +18 edges). Follows `docs/LAZY_HEAP_PLAN.md` (same arena, same wrapper style).

## Global Constraints

- No draw, hint, trap record, print or line-format change: recorded sweeps and `CEREAL_LAZY_REPLAY` stay valid.
- Size zero stays `UNSUP`; a grow past `EV_LAZY_CAP` traps (from `ev_access`); `realloc(NULL, n)` and the function's own blocks are unchanged.
- `tests/asm_exec.py` includes `sweep_rt.h` and links without `--wrap`: `sweep_rt.h` is not touched by this plan.
- Tests run in WSL (`wsl -e bash -lc`, tree `~/cereal-s1`, `bash ~/cereal-s1/.sync.sh`, `make -j4`); runs over a minute go through `run.ps1` (RUNS.md). Do not push or tag.
- Exit bar: spike gave +18 at seed 1234; the built change keeps edges taken at seed 1234 at or above 6601 and at seed 7 at or above 6541; otherwise report and ask (do not revert silently).

## Review Focus

- Realloc of an interior lazy pointer (`s->buf + 1`), and of the same blob twice: same bytes on both sides (fixture `lr_twice`).
- Read of the old part after the grow, write into the grown tail, read of the tail: same bytes (fixture `lr_grow`).
- `realloc(NULL, n)` and realloc of the function's own malloc next to a lazy realloc: unchanged (fixture `lr_own`).
- Shrinking realloc of a lazy blob: `ev_access` must not shrink or trap; the blob keeps its size (fixture `lr_grow` shrink call).
- Library mode (`lib_oracle.sh`) links through the same `eval_sweep.sh` line, so it gets the wrapper.

## End-state fit

Evaluator: `lazy_ptr`, `h_free`, `h_realloc` in `src/c/eval_libc.c`. gcc side: `__wrap_realloc` and `-Wl,--wrap=realloc` in `tests/eval_sweep.sh`. Replaces: the "malloc did not return" stop for lazy realloc; the lazy test inlined in `h_free`. Deletes: nothing else. Parked afterwards: the moved-pointer branches (spec "Cannot cover").

### Task 1: evaluator

**Files:** Modify `src/c/eval_libc.c` (`h_free` ~486, `h_realloc` ~498); Create `tests/cfg_lazy/lazy_realloc.c`; Modify `tests/eval_lazy.sh` (append after line 123).

**Interfaces:** Produces `static bool lazy_ptr(Ev *v, uint64_t p)` (true when `pid(p)` is nonzero, within `v->obj.len`, and that object is `lazy`); fixture functions `lr_grow`, `lr_twice`, `lr_own` that Task 2 compares with gcc.

- [ ] **Step 1: fixture.** `tests/cfg_lazy/lazy_realloc.c` (declares `void *malloc(unsigned long); void *realloc(void *, unsigned long);`, no includes), `struct LR { char *buf; int v; };`:
  `int lr_grow(struct LR *s)`: `r = s->v; s->buf = realloc(s->buf, 256);` if `s->buf`, `s->buf[200] = 7; r += s->buf[200]`; then a shrinking `s->buf = realloc(s->buf, 8)` and `r += s->buf[0]` (both under `if (s->buf)`); return `r`.
  `int lr_twice(struct LR *s)`: reallocs `s->buf` to 64, again to 128, and, when `s->buf` is non-NULL, `realloc(s->buf + 1, 160)`; returns `s->v + (s->buf ? s->buf[0] : 0)`.
  `int lr_own(struct LR *s)`: `int *p = realloc(0, 8); p[0] = s->v; p = realloc(p, 64);` returns `p[0] + (s->buf ? 1 : 0)`; `s->buf = realloc(s->buf, 32)` too.
- [ ] **Step 2: test (red).** Append to `tests/eval_lazy.sh`: a loop over `lr_grow lr_twice lr_own` requiring `grep -ac "^call NAME .* =>"` of `--eval-sweep=24,1234` to be 24, message `eval_lazy: lazy_realloc.c: only $n of 24 calls of NAME finished (realloc of a lazy blob)`, then `echo "eval_lazy: lazy realloc ok"`. Run `sh tests/eval_lazy.sh ~/cereal-s1/cereal`. Expected: FAIL on `lr_grow` with n below 24 (stop "realloc of a pointer that malloc did not return").
- [ ] **Step 3: implement** `lazy_ptr` above `h_free` (above is its only caller set: `h_free`, `h_realloc`); `h_free` uses it. In `h_realloc` after the NULL case: if `lazy_ptr`, then size zero → `UNSUP(v, "realloc to size zero")`, return 0; else `ev_access(v, a[0].x, a[1].x, "realloc")` (return 0 on failure), return `a[0].x`. Comment: a lazy blob stays where it is and grows in place (`docs/LAZY_REALLOC_DESIGN.md`). If `ev_access` with a smaller size than the blob's does not leave the blob alone, guard with the pointer's current extent (check by the shrink call in the fixture).
- [ ] **Step 4: test (green).** Sync, `make -j4`, rerun `eval_lazy.sh`. Expected: `eval_lazy: lazy realloc ok`, earlier sections ok.
- [ ] **Step 5: commit** `src/c/eval_libc.c tests/cfg_lazy/lazy_realloc.c tests/eval_lazy.sh`: "Lazy blobs: realloc of a lazy pointer grows in place in the evaluator".

### Task 2: gcc side

**Files:** Modify `tests/eval_sweep.sh` (awk END block near line 117, link at 168); Test `tests/eval_lazy.sh`.

**Interfaces:** Consumes `sw_alo`/`sw_ahi` and the Task 1 fixture. Produces `__wrap_realloc` in `drvmain.c`.

- [ ] **Step 1: test (red).** Append to `tests/eval_lazy.sh`: `sh "$D/eval_sweep.sh" "$C" "$D/cfg_lazy/lazy_realloc.c"` must exit 0 (`eval_lazy: lazy_realloc.c differs from gcc`), then `echo "eval_lazy: lazy realloc agrees with gcc"`. Expected: FAIL (gcc aborts on realloc of an interior pointer).
- [ ] **Step 2: implement.** In the awk string at line 117 add, in one `\n`-escaped line (no literal newlines inside the awk string): `void *__real_realloc(void *, unsigned long);\nvoid *__wrap_realloc(void *p, unsigned long n)\n{\n    if ((unsigned char *)p >= sw_alo && (unsigned char *)p < sw_ahi)\n        return p;\n    return __real_realloc(p, n);\n}`. Add `-Wl,--wrap=realloc` to the link at line 168. (Shrink/zero: size zero never reaches gcc for a lazy blob because the evaluator stops first.)
- [ ] **Step 3: green + regressions.** `eval_lazy.sh` fully; the `cfg_exec`, `cfg_link`, `cfg_state` sweeps and `asm_exec.py` as `tests/run.sh` 13e-13f call them; valgrind once on `drv` for `lazy_realloc.c` (no leaks, no invalid free). Expected: all pass.
- [ ] **Step 4: library mode.** `bench/tools/lib_oracle.sh ~/cereal-s1/cereal zstd 8`: 0 differ, 0 gcc crashed, finished calls not below the free-change run (45321).
- [ ] **Step 5: commit** `tests/eval_sweep.sh tests/eval_lazy.sh`: "Sweep driver: realloc of an arena pointer returns it unchanged".

### Task 3: measure and close

**Files:** Modify `docs/LAZY_REALLOC_DESIGN.md` (status done, §6 result), `PARKED.md` row 248, `docs/runs.md`.

- [ ] **Step 1: sweeps.** `run.ps1 -Kind bench -Name realloc-s1234 -Detach` and `-Name realloc-s7` (`LINK=1`, `SEED=7`), `sweep_meas.sh … 24`, watcher per RUNS.md (`grep -Eq '"end": +null'`). Expected: seed 1234 ≥ 6601, seed 7 ≥ 6541.
- [ ] **Step 2: replay and gate.** `CEREAL_LAZY_REPLAY=1` on the seed-1234 recordings; `-Kind gate -Command "wsl -e bash /home/winstonyli/rungate.sh"`. Expected: replay equal, gate ≥ 2405 passed.
- [ ] **Step 3: docs.** Spec `status: done` and §6 with run ids; `PARKED.md` row 248: realloc landed, numbers, remaining (moved-pointer branches, `luaD_reallocstack` subtraction stop); grep docs for "malloc did not return", "a3" and the old row wording. This plan `status: done`.
- [ ] **Step 4: commit** docs and runs rows. No push.

## Self-review

Spec coverage: §2.1 Task 1, §2.2 Task 2, §2.3 byte-equality via fixture agreement; §4 spike (done), fixture, seeds, replay, `lib_oracle`, gate: Tasks 1-3. Names (`lazy_ptr`, `sw_alo`, `sw_ahi`, `lr_*`) match across tasks. Residual risk: `--wrap=realloc` also redirects library objects' reallocs in lib mode (intended); seed 7 may repeat the zstd loss seen with free.
