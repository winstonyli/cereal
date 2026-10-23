---
status: active
---

# Lazy blobs, stage 1: plan

Design and decisions: `LAZY_BLOBS_DESIGN.md` (D1-D4 decided 2026-10-23: lazy beside the layout
as `--eval-lazy`, then replace; self-describing line; function pointers in stage 3; solver in
stage 2). This plan is stage 1 only: evaluator hooks, the lazy context, the new call line with
its gcc-side loader, `--eval-lazy`, and the measurement. No solver, no function pointers, and
the eager layout stays untouched (it is the baseline until stage 4).

Nothing starts without the user's go-ahead. Tasks have stable ids (L1..L8); implement in
order; each ends with its check. Run builds and gates in WSL as in `docs/S1_PLAN.md`.

## Facts the plan rests on (read from the code, 2026-10-23)

- `ev_access` (`eval.c`) is the one range check: callers are `load_n`, `store_sh_n`, the
  bit-field pair, `ev_copy_n`, `ev_zero_n`, `va_arg` and three helpers in `eval_libc.c`
  (lines 24, 39, 54). `load_n` already stops on `!o->init[...]` and records `ld_obj/ld_off`.
- A sweep call is `sweep_gen` (bytes) -> `sweep_call` (objects from bytes, run, line,
  kill, `snap_restore`). The line is one hex token, `call NAME IN => OUT`; `eval_sweep.sh`
  and `sweep_rt.h` (`sw_main`: `in[1<<15]`, `line[1<<16|256]`) read it.
- Measurement needs `--eval-solve` open (the `untaken edges` report is printed only then);
  `sweep_meas.sh` passes it. So the stage-1 baseline must be eager with the solver rounds
  off (L7), not the 3906 figure, which includes solver gains.

## Tasks

**L1 Hooks in `eval.c` / `eval_int.h`.** `Obj.lazy` (bool); `Ev.lazy_fill`, a function pointer
`bool (*)(Ev *, uint32_t id, uint64_t off, const Ti *ti)`; `EV_LAZY_CAP` 4096. In `ev_access`:
for a lazy object, an access past `size` grows `b`, `init` (and `sh` if present) to cover it
(zero-initialized, ids unchanged) up to the cap, else the existing out-of-bounds trap.
In `load_n`: for a lazy object whose bytes are uninitialized, call `lazy_fill` with the load's
`Ti`; on false, the existing uninitialized-read trap. A second entry `ev_lazy_zero(v, o, off, n)`
(untyped: zero, initialized, recorded through the same pointer with `ti` NULL) called from
`ev_copy_n` for its source and from the `eval_libc.c` read helpers (the three `ev_access`
calls there; check which are reads). Writes need no fill. Check: gate unchanged (2314/2314,
639 findings 0): no object is lazy outside the sweep, so the fast path is untouched; plus a
`git diff --stat` and a grep that `lazy` appears only on slow paths.

**L2 Lazy context (`eval_run.c`).** `struct Lazy`: PRNG state; `blob[]` (id, cap on size,
`key` = parent blob and offset, or parameter index; `ini` = initial bytes, grown with the
object); `link[]` (blob, off, target blob + 1 or 0, target off); totals (blobs <= 64, bytes
<= 64 KB per call, beyond which a fill returns false: "lazy budget"). `lazy_fill`: scalar `Ti`
-> `sweep_arg` (dictionary included), written to `b`, `init`, `ini`; pointer `Ti` -> NULL (1/8),
share (1/8: an existing blob chosen uniformly, offset 0 or a multiple of 8 up to 64),
else a new pending blob (`ev_new_obj(v, 0, false)`, `lazy`), recording the link. Pointer
cells are 8 bytes; the stored value is `mkptr(id, off)`. Check: a small C unit check in
`tests/` is not possible (static functions); the check is L6's fixture.

**L3 Lazy call (`eval_run.c`).** `sweep_fn_lazy`: the `SFn` for `--eval-lazy` holds only
parameters (scalars as today; pointer parameters `SA_PTR` with no blob; record parameters
by value get an eager blob of their size, whose pointer fields are filled by hook 1 like any
other cell; `SA_STATE` as today). `sweep_call_lazy`: make the lazy context; pending blobs for
pointer parameters (a parameter aliases an earlier one with probability 1/3 at a multiple of 8
up to 64, as `sweep_gen` does for the layout); scalars drawn from the context PRNG;
`v->lazy_fill` set; `ev_call`; build the line (L4); kill every lazy blob and the parameter
blobs; `snap_restore`; `ev_trim`. `sweep_where` for lazy blobs names the key path
("lazy blob param 1 +8 +16"): types are not known for a pending blob, so field names are
lost in the stuck report (stated in the design; the loaded type of the stuck read can be
added later). Check: builds clean with `-Wall -Wextra -pedantic`; the gate is unchanged.

**L4 Line writer and gcc loader.** Line payload, one hex token (binary little-endian):
`u32 n; u32 size[n]; args (per parameter: scalar bytes, or u64 (blob+1)<<32|off; records: their
blob index); u32 nlinks; {u32 blob, off, target+1, toff}[]; initial bytes of each blob`.
Output as today (return bytes, pointer tag over the blobs, final bytes of every blob in
creation order with link cells zeroed). `sweep_rt.h`: `sw_load` builds the graph (malloc per
blob, copy the initial bytes, write links) and the buffers grow to `in[1<<17]`,
`line[1<<18|256]` (64 KB of blob bytes is 128 KB of hex plus the header). `sweep_thunk` for a
lazy function emits the call with arguments from the graph and the result packing over its
blobs; the state-builder replay (`q%zu`) is reused as is. Check: the lazy sweep of
`tests/eval/*.c` through `eval_sweep.sh` with `EVAL_SWEEP_LAZY=1` (new knob, adds
`--eval-lazy` to the cereal side only) compares equal to gcc on every call.

**L5 Flag and loop.** `--eval-lazy` (option, usage text, `opt.eval_lazy`); in `eval_sweep`,
`sweep_fn`/`mut_gen` choose the lazy variants; `rotation` is `mut_gen` only in lazy mode
(no solver, `x.sv` NULL for the rotation; the miss report is still printed when `--eval-solve`
is given, so the measurement works). Check: `--eval-lazy` without `--eval-sweep` is an
error, as the other sweep flags are.

**L6 Fixture and check.** `tests/eval/lazy_downcast.c` in the shape of `luaV_execute`: a
`GCObject*` field reached from a parameter, downcast to a larger struct, with a pointer field
whose pointee's field decides three distinct results. Check: a lazy run prints calls with all
three results (an awk count in `eval_sweep.sh`'s lazy branch, or a small sh test beside it);
the same file under the eager layout is expected to miss at least one (recorded as the
motivating negative). `EVAL_SWEEP_LAZY=1` is added to the gate's sweep leg only after L7
shows no oracle mismatch on the corpora.

**L7 Measurement (through `run.ps1`, RUNS.md).** Baseline first: eager, `--eval-state`, solver
rounds off (a throwaway env switch in `eval_sweep`, not committed; row recorded). Then
`--eval-lazy`, the same flags. Lua and zstd, `N=24`, seed 1234, `bench/tools/sweep_meas.sh`.
Also the oracle comparison on both corpora (the gate's sweep leg with the knob).
Success: `luaV_execute` is no longer stuck at `StackValue.val.value_.gc` and the untaken total
is below the baseline; no oracle mismatch. Falsified if `luaV_execute` stops at a new place
for a reason the hooks cannot reach (record where, and why) or the oracle mismatches on
a systematic cause (record it; fix or park). The report is written before any decision on
stage 2, with the numbers and the stuck lines.

**L8 Close-out.** Docs: set `status: active` on the design and this plan when work starts and
`done` at the end of stage 1; `PARKED.md` row (pointer structure) updated with the numbers;
`EVAL_DESIGN.md` gets a short paragraph and the flag; `runs.md` rows. Architecture check:
what the hooks added (`Obj.lazy`, `Ev.lazy_fill`) against what stage 4 deletes.

## Review log

Round 1 (self, against the code): the baseline problem (3906 includes solver gains: L7 uses
a no-solver eager baseline); `eval_sweep.sh`'s `CEREAL_FLAGS` goes to gcc too, so the lazy flag
needs its own knob (`EVAL_SWEEP_LAZY`, like `EVAL_SWEEP_STATE`); the loader buffers in
`sweep_rt.h` are smaller than a 64 KB graph's hex; `ev_copy_n` and the `eval_libc.c` readers
can reach uninitialized lazy bytes untyped (L1 zero fill). Round 2: contradictions between
tasks (L3 kills blobs before `snap_restore`; the order matches `sweep_call`); the measurement
claim (`miss_report` needs `sv`) rechecked in `eval_sweep`; nothing further.
Open points to settle while implementing, to be reported if they turn into decisions: the
exact share offsets (L2), the stuck report's names (L3).

## Result of stage 1 (2026-10-23)

Untaken edges, N=24 seed 1234, `--eval-link --eval-state`, solver off (eager baseline from
`sweep-lazy-base`; lazy runs `lazy-lua-alias`, `lazy-zstd-alias` in `docs/runs.md`):

| | eager | lazy | change |
|---|---|---|---|
| Lua | 2061 | 1708 | -353 (17%) |
| zstd | 2029 | 1754 | -275 (14%) |
| total | 4090 | 3462 | -628 (15%) |

Findings that changed the design while building it:
- A NULL at 1/8 per drawn pointer cell kills any deep chain (`ci->func->gc->p->k`): all 852
  `luaV_execute` edges stopped on a NULL. Now at most one NULL per call, in half the calls.
- Pointers the program means as one object (saved pc and `p->code`) were separate blobs, so
  their subtraction trapped. A stop on such a subtraction now reruns the call with the later
  pointer draw aliased into the earlier blob (`LHint`, up to 4 per call). This is stage 2's key
  (path of pointer cells from a parameter) used for pointer aliasing.
- Integer reads of, and writes over, a pointer cell the sweep drew stop the call as unsupported
  (gcc's pointer bytes differ), as does any use the oracle cannot reproduce.
- A drawn size can make the program `malloc` 600 MB+: `ev_new_obj` now traps on a failed
  allocation instead of a fatal exit.

`luaV_execute` is no longer stuck on `StackValue.val.value_.gc`; it now stops with "read out of
bounds" a few levels further in (`p->code` side). Remaining zstd stops: inline asm (81 edges).
Oracle: `cfg_exec`, `cfg_link`, `cfg_state` clean under `EVAL_SWEEP_LAZY=1`; the Lua and zstd
corpora were not compared against gcc (the gate has no leg for them).

Headroom (2026-10-23, runs `lazy-lua-head`, `lazy-zstd-head`; the table above is with it): the
remaining `luaV_execute` stop was reads at negative offsets (`p - 16`, `p - 24`: `top - 1`,
container_of). A pointer to a lazy object now starts `LHEAD` (32) bytes in, so the object has
room before it; the gcc side needs no change (tags are offsets from the object start).
`luaV_execute` still stops on a read out of bounds a level or two further in (no offset
evidence kept: next probe is the temporary `LZDBG` print of object, offset, size at the trap).
