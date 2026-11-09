---
status: draft
---

# Lazy blobs: sharing by element size — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (inline, small plan). Steps use checkbox syntax.

**Goal:** one retry of a sweep call fixes every pointer cell whose pointee has the size the trap subtracted, so `luaD_reallocstack`'s `stack`, `top`, `tbclist`, upvalue and `CallInfo` cells agree after one or two retries.

**Architecture:** the evaluator records the pointee size of the trapped subtraction (`sub_sz`); `lazy_redraw` writes one size entry (`lz_key(LZ_SZ, sz)` = the earlier blob's key) beside the cell pair; `lazy_fill` turns that entry into a fallback share for a cell of that pointee size when the target blob exists. The offset class (`lz_cls`, `LZ_CLS`) is deleted.

**Spec:** `docs/LAZY_SIZE_SHARE_DESIGN.md` (option 1, approved 2026-11-09). Nothing is built until the user approves this plan.

Builds and gates in WSL: `bash ~/cereal-s1/.sync.sh` (syncs and rebuilds `~/cereal-s1/cereal`), tests as `wsl -e bash -lc 'cd ~/cereal-s1 && sh tests/eval_lazy.sh ./cereal'`, the gate as `~/rungate.sh`, long runs through `run.ps1` (RUNS.md) with a watcher. Every edit is made on the Windows tree, then synced.

## Global Constraints

- `LZ_SZ` is `(uint64_t)7 << 56` (replaces `LZ_CLS`); the size entry's key is `lz_key(LZ_SZ, psz)`, its value the earlier blob's key; no OUT half. `LMAXHINT` stays 8.
- `sub_sz = ev_tinfo(type of operand 0).psz` at both trap sites (`TO_PTRDIFF` and `compare`); never `t->val`. 0 means no size entry.
- `lazy_call1` clears `sub_sz` with `sub_a`, `sub_b`, `pun_obj`.
- The size entry lives only in `sweep_call`'s `cur`; `rec`, seeds, the solver's `force`/`syms`, the call line and replay hold cell keys only.
- The cell pair is still written (a solver rerun logs every cell, so only the cell key reaches the trapped cell). `LZ_ALIAS` and `pun_obj` hints are unchanged.
- A cell takes the size share only if: `Ti.psz != 0`, the draw is not the call's NULL draw (`d == z->nullat`), the cell has no forced entry of its own (`lazy_draw` already tests that), and a blob with the entry's value as key exists in `z->bl` now. The check is made once per cell, before the OUT draw; the OUT and TGT draws then use the same fallback pair (OUT = 1, TGT = the entry's key) through `alt`.
- The PRNG is advanced exactly as today in an unforced run, so a call that never traps is unchanged.

## End-state fit

- **Where:** `same_object`/`compare`/`TO_PTRDIFF` in `src/c/eval.c`, `Ev.sub_sz` in `src/c/eval_int.h`; `lazy_draw`, `lazy_fill`, `lazy_redraw`, `lazy_call1`, `sweep_call` in `src/c/eval_run.c`; new fixture `tests/cfg_lazy/lazy_size.c`; blocks in `tests/eval_lazy.sh`.
- **Replaces:** the offset class. **Deletes:** `lz_cls`, `LZ_CLS`, the class pair in `lazy_redraw`, the `alt` key lookup. `hint_put`, `LZ_ALIAS`, the `pun_obj` hints stay.
- **Docs after:** both specs' status, `PARKED.md` row 248, the `lazy_list.c` header comment and the `eval_lazy.sh` message.

## Review Focus

- A call that never traps is unchanged (goldens unchanged except calls that stopped before).
- Replay: a call's log from a size-hinted run, rerun forced by that log alone, prints the same line (`CEREAL_LAZY_REPLAY=1`).
- The target-exists rule: `lazy_list.c`'s `char *stack` cell (size 1, read before its blob exists) must not share `L`/itself; the fixtures must finish calls, not just run.
- The call's one NULL still ends a list.
- A size share can make the target blob's own same-size cell point at itself, or `char *` cells of unrelated structs share: expected, coverage only; the `lazy_selfptr.c` retry and gcc comparison must stay green.
- Solver reruns of trapped seeds still get fixed through the cell pair (solver-first edges not lower).
- Calls without a NULL stop at the blob budget (`LMAXBL`); the fixtures' expected gain is for the others only.

---

### Task 1: Fixture and failing check

**Files:**
- Create: `tests/cfg_lazy/lazy_size.c`
- Modify: `tests/eval_lazy.sh` (a block after the "class hint ok" block)

**Interfaces:** Produces `long sz_run(struct SZ *s)`; the check reads lines `call sz_run ... =>`.

- [ ] **Step 1: Write the fixture.** `struct SZ { char *base; char *p0, *p1, *p2, *p3, *p4, *p5, *p6, *p7, *p8, *p9; };` and `sz_run`: `char *b = s->base;` then `sum += s->p0 - b; ... s->p9 - b` (ten different offsets, one pointee size, more than `LMAXHINT` 8 fields). Header comment: the offset class needs one retry per field and stops at the ninth; one size entry covers all.
- [ ] **Step 2: Baseline.** Sync, build; `./cereal check --target=x86_64-linux-gnu --eval-sweep=200,1234 tests/cfg_lazy/lazy_size.c 2>/dev/null | grep -ac '^call sz_run .* =>'`; record `n0`. Expected: far below 200 (calls whose NULL draw lands on a field never finish and are not in this count either way).
- [ ] **Step 3: Add the check block** to `tests/eval_lazy.sh` in the style of `lazy_list.c`'s: `eval_sweep.sh` against gcc on the fixture, then finished calls of `--eval-sweep=200,1234` must be at least `T` (placeholder = 180; fixed in Task 2 from the measured `n1`), message "lazy_size.c: only $n of 200 calls of sz_run finished (the size hint)", echo "eval_lazy: size hint ok". The gcc comparison must pass now.
- [ ] **Step 4: Commit** fixture and block (RED recorded in the message: `n0`).

### Task 2: Trap size and the size share

**Files:** Modify `src/c/eval_int.h`, `src/c/eval.c`, `src/c/eval_run.c`; `tests/cfg_lazy/lazy_list.c` (comment), `tests/eval_lazy.sh` (messages, `T`).

**Interfaces:**
- Consumes: `ev_tinfo(Ev*, TypeId, Ti*)` (`eval.c:39`), `Ti.psz`.
- Produces: `Ev.sub_sz` (`uint64_t`); `static bool same_object(Ev *v, TypeId ty, uint64_t a, uint64_t b, const char *what)`; `static int lazy_blob_key(const Lazy *z, LKey key)` (index of the blob with that key, or -1); `lazy_draw(Lazy *z, LKey key, const uint64_t *alt, uint64_t drawn)` (`alt` NULL: none, else the value used when `key` has no forced entry).

- [ ] **Step 1: Record the trap size.** `Ev.sub_sz` beside `sub_a`/`sub_b`. `same_object` takes the operand's `TypeId`; on the trap path it sets `v->sub_sz` to `ev_tinfo(v, ty, &ti) ? ti.psz : 0`. `compare` passes `ot`; `TO_PTRDIFF` passes `NODE(r, KID(r, n, 0))->ty`. `lazy_call1` clears `sub_sz` where it clears `sub_a`.
- [ ] **Step 2: `LZ_SZ`, `lazy_blob_key`, `lazy_draw`.** Replace `LZ_CLS` by `LZ_SZ` and delete `lz_cls`. `lazy_blob_key` replaces the open-coded scan after the TGT draw in `lazy_fill` (use it there too). `lazy_draw`: `if (!f && alt) e.val = *alt;` (adjust the comment). Every caller other than `lazy_fill`'s OUT and TGT draws passes `NULL`.
- [ ] **Step 3: `lazy_fill` resolves once.** Before the OUT draw: `const LEnt *se = ti->psz && d != z->nullat ? llog_at(z->force, lz_key(LZ_SZ, ti->psz)) : NULL;` and `se = NULL` unless `lazy_blob_key(z, se->val) >= 0`; `uint64_t one = 1;` the OUT draw passes `se ? &one : NULL`, the TGT draw passes `se ? &se->val : NULL`. Comment: the call's one NULL keeps its draw, and a size share needs an existing target (a share of the unmade blob would be of `L` itself).
- [ ] **Step 4: `lazy_redraw` writes cell pair and size entry.** `LEnt hint[3]` in `lazy_redraw`, `lazy_call1`, `sweep_call`; in the `h->par >= 0` branch keep `hint[0..1]` and add `hint[2] = {lz_key(LZ_SZ, v->sub_sz), z->bl.data[lo].key}` with `n = v->sub_sz ? 3 : 2`. Update the comments above `lazy_redraw` ("at most three"; a cell named by itself and by its pointee size) and `sweep_call`.
- [ ] **Step 5: Verify.** Sync/build with no warnings. `sh tests/eval_lazy.sh ./cereal`: `lazy_list.c` still at least 180 of 200 (it uses size 1 for `stack` and `v`), `lazy_size.c` now passes; record `n1`, set `T` a few calls under it and well above `n0`, say so in the commit. `sh tests/eval_sweep.sh ./cereal`; regenerate sweep goldens if the script has them (its header says how) and read the diff: only calls that stopped before may change. Then `CEREAL_LAZY_REPLAY=1` over both scripts. Update the `lazy_list.c` header comment and the `eval_lazy.sh` "class hint" message to describe the size hint. If `lazy_list.c` falls under 180, stop and report (the target-exists rule or the trap size is wrong).
- [ ] **Step 6: Commit.** Both specs and this plan `status: active` in this commit.

### Task 3: Measure, gate, docs

**Files:** `docs/runs.md` (by the wrapper), `PARKED.md`, `docs/LAZY_SIZE_SHARE_DESIGN.md`, `docs/LAZY_FIELD_SHARE_DESIGN.md`.

- [ ] **Step 1: Probe.** `./cereal check --target=x86_64-linux-gnu -std=gnu99 -I<lua> --eval-only=luaD_reallocstack --eval-sweep=24,1234 --eval-link <lua files>` (command and `ulimit` of `sweep_meas.sh`/the spec's probe). Record finished calls (today none). If zero, use `--eval-sweep=200,1234`; if still zero, stop and report: the mechanism did not reach the wall, and the exit rule applies.
- [ ] **Step 2: Sweep.** Through `run.ps1`: `LINK=1 sh sweep_meas.sh ./cereal 24` for seed 1234 (name `sizeshare-link24`) and `SEED=7` (`sizeshare-link24-s7`), plus a seed-7 baseline from `3b5a09e` (the last commit without the class) built into `~/cereal-base` with `git archive` (name `base-link24-s7`; `.sync.sh` syncs the working tree, so it cannot build the baseline). Accept when: edges at least 6553 (seed 1234) and the seed-7 baseline, seconds within +10% of 1619, solver-first edges not lower than 2102. Report next to the offset-class run (`classshare-link24`: 6581, 2154 s, 2092). If the gain over 6553 is under the noise of the seed pair or time rises over 10%, the exit rule applies.
- [ ] **Step 3: Gate.** `run.ps1 -Kind gate -Name sizeshare-gate ... ~/rungate.sh`; exit 0, goldens as in Task 2.
- [ ] **Step 4: Docs.** Size spec `status: done` with numbers; `LAZY_FIELD_SHARE_DESIGN.md` `status: superseded-by: LAZY_SIZE_SHARE_DESIGN.md` and the same for its plan; `PARKED.md` row 248 gets the result; grep `docs/` for `lz_cls`, "cell class" and "class hint" and fix or link.
- [ ] **Step 5: Commit** (`docs/runs.md` rows included). Do not push or tag.

### Exit

If Task 2 Step 5 fails twice, or Task 3's acceptance fails: `git checkout c2bbf16 -- src tests` (the offset-class state), record the result in `PARKED.md` row 248, specs to `status: done`/`superseded` as the facts say. The user decides before the revert is committed.

## Parked on completion

Lists whose end is not a drawn NULL (the 507-edge `lazy blob budget` stop), solver reruns not seeing the size entry, same-size unrelated types sharing: already in the spec's "Cannot cover" and rulings; Task 3 adds them to `PARKED.md` row 248 if the measurement shows they still cost edges.
