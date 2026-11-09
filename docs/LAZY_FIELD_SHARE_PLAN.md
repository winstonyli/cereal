---
status: active
---

# Lazy blobs: sharing by cell class — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (inline, small plan). Steps use checkbox syntax.

**Goal:** one retry of a sweep call fixes every pointer cell of the same class, so a list walk over lazy blobs finishes after a few retries, not one per element.

**Architecture:** the share hint that `lazy_redraw` writes gains a class key beside the later operand's cell key; `lazy_draw` consults a class entry when the call's forced map has no entry for the cell. All in `src/c/eval_run.c`.

**Spec:** `docs/LAZY_FIELD_SHARE_DESIGN.md` (option 1, approved 2026-11-08). Nothing here is built until the user approves this plan.

Builds and gates in WSL: `bash ~/cereal-s1/.sync.sh` (syncs and rebuilds `~/cereal-s1/cereal`), tests as `wsl -e bash -lc 'cd ~/cereal-s1 && sh tests/eval_lazy.sh ./cereal'`, the gate as `~/rungate.sh`, long runs through `run.ps1` (RUNS.md). Baseline: committed HEAD, Lua LINK=1 N=24 seed 1234 = 6553 of 9599 edges (run `2026-11-08-fieldsmall-link24`).

## Global Constraints

- `LZ_CLS` is `(uint64_t)7 << 56`; class key `ck = lz_key(LZ_CLS, lz_key(off, creator_poff))`, entries `lz_key(ck, LZ_OUT)` and `lz_key(ck, LZ_TGT)`; `LMAXHINT` 8.
- Class entries live only in `sweep_call`'s `cur`; `rec`, the solver's `force`/`syms`, the call line and replay see cell keys only (`cur` is local to `sweep_call`: `eval_run.c:1304-1335`; the solver builds its own `force` from a seed's log, `:1436-1449`).
- The share hint is both pairs: the cell's own entries (as today, so a solver rerun whose seed log already holds the cell still gets fixed: `hint_put` overwrites them) and the class entries (new, for the cells without an entry). `lazy_redraw` returns up to four entries (`hint[4]`); the `pun_obj` branch still returns at most two.
- A cell drawn at `d == z->nullat` never takes `alt`; a cell with its own forced entry keeps it.
- Only the `LZ_OUT` and `LZ_TGT` draws in `lazy_fill` pass `alt`; every other `lazy_draw` caller passes 0.
- The PRNG is advanced exactly as today in an unforced run, so a call that never traps is unchanged. (A forced share draws two more numbers than a new blob, so a retried call's later draws shift; true today too.) A call that trapped and now finishes (more retries, `LMAXHINT` 8) changes, so the goldens are regenerated and the diff read: every changed line must be a call that stopped before.

## End-state fit

- **Where:** `lazy_draw`, `lazy_fill`, `lazy_redraw`, `LMAXHINT` in `src/c/eval_run.c`; new fixture `tests/cfg_lazy/lazy_list.c`; its block in `tests/eval_lazy.sh`.
- **Replaces:** nothing; the share hint gains a class pair beside its cell pair. **Deletes:** nothing (`hint_put`, `LZ_ALIAS`, the `pun_obj` hints stay: they name parameters and a blob, not a class).
- **Docs after:** spec `status: done`; `PARKED.md` row 248 (wall now stated correctly, linked to the spec, new numbers); `LAZY_BLOBS_DESIGN.md` mention of the retry hint if it describes the cell key (grep, Task 3).

## Review Focus

- Replay: a call's log from a class-hinted run, rerun forced by that log alone, prints the same line (`CEREAL_LAZY_REPLAY=1`).
- The call's one NULL: a list must still end at `nullat`, not at a class-forced share (fixture finishes calls, not just sweeps them).
- A class hint must not change a call that never traps (goldens unchanged).
- Solver reruns of trapped seeds: their log holds a cell entry per drawn cell, which beats a class entry; the cell half of the hint must still fix them (no loss versus baseline in the solver's own edges, reported separately).
- Calls without a NULL (half of all: `nullat` is `~0u`, `eval_run.c:1165`) never end a pointer list and stop at the blob budget (`LMAXBL`); the fixture's expected gain is the non-NULL-ended calls only, so the fixture walks a fixed number of elements.

---

### Task 1: Fixture and failing check

**Files:**
- Create: `tests/cfg_lazy/lazy_list.c`
- Modify: `tests/eval_lazy.sh` (a block after the "parameter alias" block)

**Interfaces:** Produces the function `ll_run(struct LS *s)`; the check reads lines `call ll_run ... =>`.

- [ ] **Step 1: Write the fixture.** `lazy_list.c`, shaped as Lua's `luaD_reallocstack`: `struct LU { char *v; struct LU *next; }; struct LS { char *stack; struct LU *open; };` and `long ll_run(struct LS *s)`: load `char *base = s->stack;` first (as Lua reads `L->stack` before the walk, so the first retry hits the same cell), then a fixed six elements, `for (i = 0, u = s->open; i < 6; i++, u = u->next) sum += u->v - base;` (six: today's four hints stop at the fifth; a call whose NULL falls inside the walk traps on the null dereference and is not counted). First line comment says what the sweep must do, and that `LS.open` and `LU.next` sit at the same offset (8) so the first `v` and the later ones fall in one class by layout; if the structs change they split and need one more retry.
- [ ] **Step 2: Baseline.** Sync and build; run `./cereal check --target=x86_64-linux-gnu --eval-sweep=200,1234 tests/cfg_lazy/lazy_list.c 2>/dev/null | grep -ac '^call ll_run .* =>'`; record `n0`. Expected: far below the non-NULL half (about 100 of 200 calls have no NULL); if `n0` is near it, the fixture does not need the class hint: lengthen the walk until it does.
- [ ] **Step 3: Add the check block** to `tests/eval_lazy.sh` in the style of the neighbours: `eval_sweep.sh` against gcc on the fixture, then the finished calls of `--eval-sweep=200,1234` must be at least `T`, provisional `T = 80` (about the non-NULL calls less a margin). Message names the retry. Run `sh tests/eval_lazy.sh ./cereal`. Expected: FAIL on this block only.
- [ ] **Step 4: Commit** the fixture and block (RED is recorded in the message).

### Task 2: The cell-class hint

**Files:** Modify `src/c/eval_run.c`.

**Interfaces:** Produces `static LKey lz_cls(const Lazy *z, int bi, uint64_t off)` (class key of the cell at `off` of blob `bi`) and `lazy_draw(Lazy *z, LKey key, LKey alt, uint64_t drawn)`.

- [ ] **Step 1: `LZ_CLS`, `lz_cls`, `LMAXHINT` 8.** Add the tag beside `LZ_FN`; `lz_cls` returns `lz_key(LZ_CLS, lz_key(off, z->bl.data[bi].poff))`, defined after `Lazy`.
- [ ] **Step 2: `lazy_draw` takes `alt`.** If the forced map has no entry for `key` and `alt` is non-zero, take `alt`'s entry; the value used is logged under `key` as now. Update the forward declaration and every caller: `lazy_off`, `lazy_fn`, `lazy_byte`, the scalar parameter bytes (`:1107`) and the alias draw (`:1141`) pass 0.
- [ ] **Step 3: `lazy_fill` passes the class.** `cls = lz_cls(z, bi, off)`; the `LZ_OUT` draw passes `d == z->nullat ? 0 : lz_key(cls, LZ_OUT)`; the `LZ_TGT` draw passes `lz_key(cls, LZ_TGT)`.
- [ ] **Step 4: `lazy_redraw` writes cell and class.** `LEnt hint[4]` in `lazy_redraw`, `lazy_call1`, `sweep_call` (the `nhint` loops are generic). In the `h->par >= 0` branch keep `hint[0..1]` (cell OUT=1, TGT=earlier blob) and add `cls = lz_cls(z, h->par, h->poff)`; `hint[2] = {lz_key(cls, LZ_OUT), 1}`, `hint[3] = {lz_key(cls, LZ_TGT), z->bl.data[lo].key}`; `n = 4`. Update the comments above `lazy_redraw` and `sweep_call`.
- [ ] **Step 5: Verify.** Sync/build with no warnings; `sh tests/eval_lazy.sh ./cereal` and `sh tests/eval_sweep.sh ./cereal`: the Task 1 block now passes; record `n1` and set `T` a few calls under it (and above `n0`), say so in the commit. Regenerate the sweep goldens if the scripts have them (`tests/eval_sweep.sh` header says how) and read the diff: only calls that stopped before may change. Then `CEREAL_LAZY_REPLAY=1` over both scripts: all ok. Spec `status: active` in this commit.
- [ ] **Step 6: Commit.**

### Task 3: Measure, gate, docs

**Files:** `docs/runs.md` (by the wrapper), `PARKED.md`, `docs/LAZY_FIELD_SHARE_DESIGN.md`, `docs/LAZY_BLOBS_DESIGN.md` if it says the hint is per cell.

- [ ] **Step 1: Probe.** `./cereal check --target=x86_64-linux-gnu -std=gnu99 -I<lua> --eval-only=luaD_reallocstack --eval-sweep=24,1234 --eval-link <lua files>` (the command of the spec's probe, files as in `sweep_meas.sh`). Record the finished-call count (baseline none), and repeat with `LMAXHINT` 16 as a throwaway edit: keep 8 if 16 finishes no more calls, else raise and say why. If the count is zero (only NULL-ended calls can finish, about a tenth), use `--eval-sweep=200,1234`; if still zero, stop and report: the mechanism did not reach the wall.
- [ ] **Step 2: Sweep.** Through `run.ps1`: `LINK=1 sh sweep_meas.sh ./cereal 24` (name `classshare-link24`) for seed 1234 (baseline 6553 of 9599) and `SEED=7` (name `classshare-link24-s7`), plus a baseline for seed 7 from the Task 1 commit (the last without the change) built into `~/cereal-base` with `git -C <repo> archive <commit> | tar -x -C ~/cereal-base && make -C ~/cereal-base -j6` (`.sync.sh` syncs the working tree, so it cannot build the baseline), name `base-link24-s7`. Accept when: edges taken at least the baseline for both seeds, the 536-edge stop smaller, seconds not worse beyond the seed-1234 pair's difference. Solver half = the figure `sweep_meas.sh` prints for edges first reached by a solver round, new run against baseline run, reported on its own; a drop there blocks.
- [ ] **Step 3: Gate.** `run.ps1 -Kind gate -Name classshare-gate ... ~/rungate.sh`; exit 0, goldens as regenerated in Task 2.
- [ ] **Step 4: Docs.** Spec `status: done` with the measured numbers (and its "Cell entries win" ruling corrected: the hint writes both pairs, so solver reruns still get fixed); `PARKED.md` row 248 rewritten (upvalue list first, link the spec, new reference baseline and run ids); grep `docs/` (including `CONCOLIC_DESIGN.md` §10.2, which row 248 cites) for the old per-cell or `CallInfo` wording.
- [ ] **Step 5: Commit** (`docs/runs.md` rows included). Do not push or tag.

## Parked on completion

Different link offsets walked in turn, element 1 versus the rest as two classes, and solver reruns not getting the class hint (a seed's log holds an entry per cell): already stated in the spec's "Cannot cover"; Task 3 adds them to `PARKED.md` row 248 if the measurement shows they still cost edges.
