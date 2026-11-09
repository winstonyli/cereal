---
status: done
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
- **Guard:** none added: the change removes a mechanism (the offset class), not a measurable class of drift; the fixtures `lazy_list.c` and `lazy_size.c` are the checks that the share still works.
- **Docs after:** this plan `done`; both specs' status, `PARKED.md` row 248, the `lazy_list.c` header comment and the `eval_lazy.sh` message.

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
- [ ] **Step 2: Baseline.** Sync, build; `./cereal check --target=x86_64-linux-gnu --eval-sweep=200,1234 tests/cfg_lazy/lazy_size.c 2>/dev/null | grep -ac '^call sz_run .* =>'`; record `n0` (26 of 200 measured on the offset-class binary in scratch).
- [ ] **Step 3: Add the check block** to `tests/eval_lazy.sh` in the style of `lazy_list.c`'s: `eval_sweep.sh` against gcc on the fixture, then finished calls of `--eval-sweep=200,1234` must be at least `T` (placeholder = 150; fixed in Task 2 from the measured `n1`. The ceiling is below 200: the call's one NULL lands on one of the 11 pointer-cell draws in about 11/64 of calls and that stop is not retried, so about 166; a 7-field variant measured 169 of 200 with the offset class), message "lazy_size.c: only $n of 200 calls of sz_run finished (the size hint)", echo "eval_lazy: size hint ok". The gcc comparison must pass now.
- [ ] **Step 4: Commit** fixture and block (RED recorded in the message: `n0`).

### Task 2: Trap size and the size share

**Files:** Modify `src/c/eval_int.h`, `src/c/eval.c`, `src/c/eval_run.c`; `tests/cfg_lazy/lazy_list.c` (comment), `tests/eval_lazy.sh` (messages, `T`).

**Interfaces:**
- Consumes: `ev_tinfo(Ev*, TypeId, Ti*)` (`eval.c:39`), `Ti.psz`.
- Produces: `Ev.sub_sz` (`uint64_t`); `static bool same_object(Ev *v, TypeId ty, uint64_t a, uint64_t b, const char *what)`; `static int lazy_blob_key(const Lazy *z, LKey key)` (index of the blob with that key, or -1); `lazy_draw(Lazy *z, LKey key, const uint64_t *alt, uint64_t drawn)` (`alt` NULL: none, else the value used when `key` has no forced entry).

- [ ] **Step 1: Record the trap size.** `Ev.sub_sz` beside `sub_a`/`sub_b`. `same_object` takes the operand's `TypeId`; on the trap path it sets `v->sub_sz` to `ev_tinfo(v, ty, &ti) ? ti.psz : 0`. `compare` passes `ot`; `TO_PTRDIFF` passes `NODE(r, KID(r, n, 0))->ty`. `lazy_call1` clears `sub_sz` where it clears `sub_a`.
- [ ] **Step 2: `LZ_SZ`, `lazy_blob_key`, `lazy_draw`.** Replace `LZ_CLS` by `LZ_SZ` and delete `lz_cls`. `lazy_blob_key` replaces the open-coded scan after the TGT draw in `lazy_fill` (use it there too). `lazy_draw`: `if (!f && alt) e.val = *alt;` (adjust the comment). Update the forward declaration (`eval_run.c:778`). Every caller other than `lazy_fill`'s OUT and TGT draws (`lazy_off`, `lazy_fn`, `lazy_byte`, the scalar parameter bytes, the alias draw) passes `NULL`. A `TO_PTRDIFF` operand that is not a pointer type gives `psz` 0: no size entry.
- [ ] **Step 3: `lazy_fill` resolves once.** Before the OUT draw: `const LEnt *se = ti->psz && d != z->nullat ? llog_at(z->force, lz_key(LZ_SZ, ti->psz)) : NULL;` and `se = NULL` unless `lazy_blob_key(z, se->val) >= 0`; `uint64_t one = 1;` the OUT draw passes `se ? &one : NULL`, the TGT draw passes `se ? &se->val : NULL`. Comment: the call's one NULL keeps its draw, and a size share needs an existing target (a share of the unmade blob would be of `L` itself).
- [ ] **Step 4: `lazy_redraw` writes cell pair and size entry.** `LEnt hint[3]` in `lazy_redraw`, `lazy_call1`, `sweep_call`; in the `h->par >= 0` branch keep `hint[0..1]` and add `hint[2] = {lz_key(LZ_SZ, v->sub_sz), z->bl.data[lo].key}` with `n = v->sub_sz ? 3 : 2`. Update the comments above `lazy_redraw` ("at most three"; a cell named by itself and by its pointee size) and `sweep_call`.
- [ ] **Step 5: Verify.** Sync/build with no warnings. `sh tests/eval_lazy.sh ./cereal`: `lazy_list.c` still at least 180 of 200 (it uses size 1 for `stack` and `v`), `lazy_size.c` now passes; record `n1`, set `T` a few calls under it and well above `n0`, say so in the commit. `sh tests/eval_sweep.sh ./cereal` (it compares against gcc; there are no goldens). The never-trap check is a diff: before this task's code (the Task 1 binary, saved as `cereal-before`) and after, `--eval-sweep=24,1234` over every `tests/cfg_lazy/lazy_*.c` and `tests/eval/*.c`, the `call` lines diffed; only calls that stopped on a subtraction before may change. Then `CEREAL_LAZY_REPLAY=1` over both scripts. Update the `lazy_list.c` header comment and the `eval_lazy.sh` "class hint" message to describe the size hint. (`lazy_list.c` has about 14 pointer draws and still measured 194 of 200, so the NULL-ceiling estimate is rough: take `T` from `n1`, not the estimate.) If `lazy_list.c` falls under 180, stop and report (the target-exists rule or the trap size is wrong).
- [ ] **Step 6: Commit.** Both specs and this plan `status: active` in this commit.

### Task 3: Measure, gate, docs

**Files:** `docs/runs.md` (by the wrapper), `PARKED.md`, `docs/LAZY_SIZE_SHARE_DESIGN.md`, `docs/LAZY_FIELD_SHARE_DESIGN.md`.

- [ ] **Step 1: Probe.** `./cereal check --target=x86_64-linux-gnu -std=gnu99 -I<lua> --eval-only=luaD_reallocstack --eval-sweep=24,1234 --eval-link <lua files>` (command and `ulimit` of `sweep_meas.sh`/the spec's probe). Record finished calls (today none). If zero, use `--eval-sweep=200,1234`; if still zero, stop and report: the mechanism did not reach the wall, and the exit rule applies.
- [ ] **Step 2: Sweep.** Through `run.ps1`: `LINK=1 sh bench/tools/sweep_meas.sh ./cereal 24` for seed 1234 (name `sizeshare-link24`) and `SEED=7` (`sizeshare-link24-s7`), plus a seed-7 baseline from `3b5a09e` (the last commit without the class) built into `~/cereal-base` with `git archive <commit> | tar -x -C ~/cereal-base && make -C ~/cereal-base -j6` (name `base-link24-s7`; `.sync.sh` syncs the working tree, so it cannot build the baseline). Accept when: edges at least 6553 (seed 1234) and the seed-7 baseline, seconds within +10% of 1619, solver-first edges not lower than 2102. Report next to the offset-class run (`classshare-link24`: 6581, 2154 s, 2092). If the gain over 6553 is under the noise of the seed pair or time rises over 10%, the exit rule applies.
- [ ] **Step 3: Gate.** `run.ps1 -Kind gate -Name sizeshare-gate ... ~/rungate.sh`; exit 0.
- [ ] **Step 4: Docs.** Size spec `status: done` with numbers; `LAZY_FIELD_SHARE_DESIGN.md` `status: superseded-by: LAZY_SIZE_SHARE_DESIGN.md` and the same for its plan; `PARKED.md` row 248 gets the result; grep `docs/` for `lz_cls`, "cell class" and "class hint" and fix or link.
- [ ] **Step 5: Commit** (`docs/runs.md` rows included). Do not push or tag.

### Exit

If Task 2 Step 5 fails twice, or Task 3's acceptance fails: `git checkout c2bbf16 -- src tests` (the offset-class state), record the result in `PARKED.md` row 248, specs to `status: done`/`superseded` as the facts say. The user decides before the revert is committed.

## Parked on completion

Lists whose end is not a drawn NULL (the 507-edge `lazy blob budget` stop), solver reruns not seeing the size entry, same-size unrelated types sharing: already in the spec's "Cannot cover" and rulings; Task 3 adds them to `PARKED.md` row 248 if the measurement shows they still cost edges.

## Result (Tasks 1-2 built, Task 3 sweep 1234 only, 2026-11-09)

- Fixtures: `lazy_size.c` 26 -> 165 of 200 calls finish; `lazy_list.c` 203; `eval_lazy.sh`, `eval_sweep.sh` and both replay runs pass; the before/after sweep diff changes only those two fixtures.
- Probe `luaD_reallocstack` (24 calls, `--eval-link`, seed 1234): 1 finishes (offset class: 0, or 1 with `LMAXHINT` 24).
- Sweep, run `2026-11-09-sizeshare-link24` (836573e) against `2026-11-08-classshare-link24` and `2026-11-08-fieldsmall-link24`: edges 6541 (class 6581, base 6553) of 9599; solver-first 1968 (2092, 2102); seconds 1896 (2154, 1619). The subtraction stop is 15 edges and the `lazy blob budget` stop 531 (507 with the class, 25 at base).
- Acceptance fails: edges under the 6553 base, solver half lower, seconds +17% over 1619. Seed 7 and the gate were not run. The exit rule applies; the revert (`git checkout c2bbf16 -- src tests`) waits for the user.
- Budget probe (throwaway build of `836573e` with `LMAXBL` 128, `luaD_reallocstack`, 24 calls, `--eval-link`, seed 1234): still 1 of 24 finishes. The stops printed are 79 `subtraction of pointers into different objects`, 3 uninitialized reads, 2 null reads, 2 `lazy blob budget`, 1 realloc of a pointer malloc did not return, 1 read through a pointer to no object. So the blob budget is not what stops these calls; the "no end to the list" explanation for the 531 sweep-level budget edges does not carry to this function, and the subtraction hint still does not take for most calls (cause not found).
- Trace (throwaway build of `836573e` printing each trap, cell draw and retry; `luaD_reallocstack`, 24 calls, 88 generator runs, seed 1234). The size hint does take: after the first retry (`stack` vs `stack_last`, size 16) every later `StackValue *` cell shares (`se=1 tgt=1 r=1`), and the walk then runs on through the upvalue and `CallInfo` lists. What ends the runs (final stop of each of the 88): 36 `lazy blob budget` (calls with no NULL: the `CallInfo` list never ends); 40 a subtraction where one operand is NULL (`sub_a` or `sub_b` is 0, so `lazy_redraw` has no blob to name; 30 of them have a NULL draw, the call's one NULL landing on a cell the walk subtracts, 10 have none: a zero-filled field); 12 a subtraction of two blobs that was not retried (`asked` already, or `LMAXHINT` spent); 5 other. So the size share fixes the cells it was meant to, and the wall is now the NULL operands and the unended list, neither of which a share hint can reach. A rule for a NULL operand (the stop names no blob; it would need the cell that drew the NULL) and an end for the list are separate designs.
