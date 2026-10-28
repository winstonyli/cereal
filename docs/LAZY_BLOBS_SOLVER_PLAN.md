---
status: done
---

# Lazy blobs, stage 2: the solver on lazy calls

Design and decisions: `LAZY_BLOBS_DESIGN.md` (D5-D9, taken as recommended 2026-10-23; D8's
pointer solving is parked in `PARKED.md`). Stage 1 (`LAZY_BLOBS_PLAN.md`) is built: `--eval-lazy`
runs without the solver (`x.sv = s.lazy ? NULL : sv`, `if (sv && !s.lazy)` in `eval_sweep`).
This plan lifts that: `--eval-solve --eval-lazy` tracks, solves and reruns lazy calls.

Nothing starts without the user's go-ahead. Tasks have stable ids (S1..S8); implement in
order, each ends with its check. Builds and gates in WSL as in `docs/S1_PLAN.md`.

## Facts the plan rests on (read from the code, 2026-10-23)

- The solver sees an input as a byte string. `sweep_track` gives byte `j` of blob `i` the symbol
  `sx_sym(ob[i].off + j)` and each scalar parameter byte `sx_sym(a->off + j)`; `solve_step1` takes a
  corpus seed (`SSearch.corpus`, byte strings of `insz`), runs it tracked, and for each open edge asks
  `sx_try`; a model is `(offset, byte)` pairs (`SxModel.off/.val`), and `solve_run` writes them over a
  copy of the seed and calls `sweep_call`.
- A lazy call has no byte string: `lazy_call1` draws everything from `rs` while it runs. Its
  draws are: scalar parameter bytes; per pointer parameter whether it aliases an earlier one and
  where; per pointer cell the outcome (NULL / share a blob at an offset / new blob) from the
  `nullat` schedule and `rnd`; per scalar load of an uninitialized blob byte, `sweep_arg`.
- Tracking needs `v->track`, `sweep_track`'s `sx_clear` and path reset, `o->sh` on the symbolic bytes
  (`load_sh` reads it; `lazy_grow` already grows `sh`), `v->ash` for scalar parameters.
- `search_keep(q, s, in, fresh)` and `mut_gen` use `x->in`; both lazy and eager keep it as a byte
  string today (`mut_gen` ignores `x->in` for lazy).
- `sweep_call_lazy` reruns `lazy_call1` with alias hints; the final run's draws are the call's.

## Design in one paragraph

Every draw gets a key (64-bit hash: a parameter's key is its index; a lazy blob's key is
`hash(parent blob key, cell offset)`; a byte's key is `hash(blob key, offset)`; a pointer cell's
outcome key is the same as the cell's byte key tagged "outcome"). A call has a `Log` of entries
`(key, value)` it made, and an optional forced map `(key -> value)` it consults before drawing. A
corpus seed is a `Log` plus the call's starting `rs`. A fresh draw falls back to `rs`, which a rerun restores to that
state, so a rerun is deterministic. Tracking adds, for each scalar byte drawn, a symbol with a
dense id in a per-run table `id -> key`; a model `(id, byte)` becomes forced overrides
`(key, byte)` on a copy of the seed's log. Pointer outcomes are logged and replayed, never
solved (D8; the offset into a shared or aliased blob is solved since D11, `LAZY_BLOBS_DESIGN.md`).

## Tasks

**S1 Keys and the log.** In the lazy section of `eval_run.c`: `LKey` (hash, `uint64_t`), `LLog`
(`VEC` of `{key, val}` plus a sorted-on-demand index for lookup), `LBlob.key`. `Lazy` gets
`LLog *rec` (what this call drew) and `const LLog *force`. One function `lazy_draw(z, key, kind,
bits)` is the only place a draw happens: it returns the forced value if present, else draws from `rs`
(the existing code at that site), and records it. `lazy_fill` (scalar and pointer), the scalar
parameters, the alias-into-earlier choice and the `nullat` schedule all go through it (the
schedule becomes a per-cell outcome key, so it replays). No solver yet. Check: with `force` empty
the output of `--eval-lazy` on `tests/cfg_exec`, `cfg_link`, `cfg_state` is byte-identical to before
(diff of the full sweep output); gate unchanged.

**S2 Replay.** `lazy_call1` takes `force`; `sweep_call_lazy` returns the final run's `rec` to
its caller, restoring `*rs` per attempt as now. Check: a debug assertion mode in `mut_gen`
(`CEREAL_LAZY_REPLAY=1`, test-only): each call is re-run from its own log and must print the same
line; run on the three test dirs and on Lua's `lvm.c` subset.

**S3 Corpus of logs.** `SSearch.corpus` holds seeds as either a byte string (eager) or a `Log`
(lazy, with its starting `rs`): a tagged `SSeed`, not a second corpus; `search_keep`, `search_free`, `solve_step1`'s
`seed = ...` and the `tried` list follow. Check: eager sweep output byte-identical (the same diff
as S1) with and without `--eval-solve` on the three test dirs.

**S4 Tracking on lazy calls.** `sweep_call_lazy` when `v->track`: `sx_clear`, path reset, symbols
for scalar parameter bytes (dense ids) into `ash`, `v->ash = ash`; `lazy_fill` allocates `o->sh`
for the object on first use (all zero) and stores `sx_sym(id)` for each byte it draws; the
id -> key table lives in `Lazy`. Pointer cells and bytes written before being read stay concrete.
Check: a tracked run of the fixture below prints the same call line as the untracked run, and its
path (`v->path`) holds the `k[0] > 5` branch.

**S5 Solve a lazy seed.** `solve_run` for lazy: model `(id, byte)` -> `(key, byte)` through the
table of the tracked run, written over a copy of the seed's log, rerun through `sweep_call_lazy`
with that `force`. A key missing from the new run is ignored (the path changed); a model byte
for a key the run draws differently (pointer outcome changed) is also ignored. Check: new fixture
`tests/cfg_lazy/lazy_guard.c` (`if (cl->k[0] == 12345) ...` behind the same pointer chain) is
reached with `--eval-solve --eval-lazy`.

**S6 Switch it on.** Remove `x.sv = s.lazy ? NULL : sv` and `if (sv && !s.lazy)`; the search loop,
`miss_report` and the stuck report work for lazy. `eval_sweep.sh`'s note and the gate's `EVAL_SWEEP_SOLVE=`
override on the lazy legs go (the gate's solver leg then also runs lazy). Check: gate; the oracle
legs with `EVAL_SWEEP_LAZY=1 EVAL_SWEEP_SOLVE='z3 -in -smt2'` on the three test dirs.

**S7 Measurement (through `run.ps1`, RUNS.md).** `sweep_meas.sh` on Lua and zstd with
`--eval-link --eval-state`: eager with solver (3906 expected: Lua 1934, zstd 1972; this is the
stage's regression check, so run it first on the S1-S6 build), lazy with solver. Report both with the
stuck lines. Success: eager unchanged; lazy-with-solver below lazy-without (3418) and below 3906.
Falsifier: lazy-with-solver not below lazy-without, or eager moved.

**S8 Close-out.** Docs: design stage 2 marked built, this plan `done`, `PARKED.md` row for pointer
outcomes confirmed, `EVAL_DESIGN.md` flag note ("not combined with the solver" removed),
`runs.md`. Architecture check: what grew special cases (the tagged seed), what stage 4 removes
(the byte-string seed path with the eager layout). Commit on `lazy-blobs`; nothing pushed or tagged.

## Risks

- **Key stability.** A model that changes an early branch changes which blobs exist; keys are paths
  so surviving blobs keep them, but a blob reached by a different parent cell gets a new key and
  its bytes are redrawn: the solved edge may then not be reached. This costs yield, not
  correctness; S7 measures it.
- **Replay faithfulness.** A rerun must make the same draws in the same order up to the first
  forced difference; draws not in the log come from `rs` restored to the call's start, so the
  order of unforced draws is deterministic. S2's assertion mode is the guard.
- **Alias hints.** The final run's outcomes are logged, so the hint is baked into the seed; a
  solved rerun that stops on a subtraction retries with hints as a fresh call would. Logged
  hints from the seed are part of `force` and are not re-derived.
- **Log size.** One entry per drawn byte and cell, 16 bytes each; a call is bounded by
  `LMAXBY` and `LMAXBL`, so a seed is at most ~0.5 MB and the corpus bound (`SCORPUS`) keeps total
  memory in check; S3 checks the peak on Lua.
- **Tracked cost.** `sh` arrays double per-object memory in tracked runs only.

## Review log

Round 1 (self, against the code): `lazy_call1` draws are not all in `lazy_fill` (parameters and the
alias-into-earlier choice are in the call function), hence S1's single `lazy_draw`; `nullat` as an
index breaks replay when the path changes, hence per-cell outcome keys; `search_keep`'s
`SCORPUS`/`SSEEDS` logic needs the seed type (S3). Round 2: S6 must also drop the gate's
`EVAL_SWEEP_SOLVE=` overrides, or the solver leg stays off for lazy; S7's eager check must run
before the lazy numbers, since a moved eager figure invalidates the comparison. Round 3: nothing
else material found on a full pass. Round 4 below is the second reader.

## Round 4: second-reader review (Sonnet, 2026-10-23) and what it changed

Findings accepted, each a binding amendment to the task named:

- **S1 always draws, then overrides.** `lazy_draw` consumes `rs` exactly as the code does now
  (`sweep_arg` takes a variable number of `rnd`; the share branch draws target and offset even
  under a hint), then replaces the value with the forced one. A partial force then leaves every
  unforced draw unchanged. The "same draws up to the first forced difference" claim in Risks holds
  only this way.
- **S1 keeps the NULL ordinal.** `nullat` still decides fresh draws (so S1's byte-identical check
  holds); the log records the outcome per cell; a forced outcome takes precedence over `nullat`
  and over a hint, and a forced non-NULL outcome suppresses the NULL at that ordinal (never two
  NULLs).
- **S1 logs a share target as (blob key, offset)**, not a blob index; a missing blob falls back to
  a fresh draw. `LHint.tgt` stays an index (it lives within one call's attempts).
- **S1 normalizes forced parameter bytes** (bool to 0/1, as eager `solve_run` does); float
  parameters get no symbol (S4). Log value encodings and the key hash are fixed in S1's code and
  commented there: key = 64-bit FNV-1a over (parent key, cell offset, kind).
- **S2 changes the signature**: `sweep_call_lazy(..., LLog *rec_out)` moves `rec` out of `Lazy z`
  before it is freed; recording is on only when `x->sv` or replay mode is set. The replay assertion
  prints to a sink and compares status, result bytes and edge coverage, not the printed line (stop
  lines print only under `show_stop`), and skips functions whose call changes globals when there
  is no `snap`. "Lua's lvm.c subset" is the 13-file list in `lv.sh` (lvm, lopcodes, lapi, ldo,
  lobject, ltm, lfunc, lstring, ltable, lgc, lstate, ldebug, lmem), kept as `tests/` data only if
  it is needed.
- **S3's check compares pre-change with post-change output for each flag setting** (with and
  without `--eval-solve`), not the two settings with each other (`solve_run` prints calls into
  the body). `SSeed` owns its log and `rs0`; `search_free` frees it.
- **S4 per attempt**: `sx_clear`, `path.len = 0`, the id -> key table, `rec` and `ash` reset at the
  start of each `lazy_call1` attempt; `v->ash = NULL` after the call; `lazy_fill` refetches the
  object after `lazy_new` before storing `sh`. Tracked memory is 4x (`sh` is `uint32_t` per byte).
  The fixture S4 refers to is `lazy_guard.c` of S5.
- **S5's child seed is the rerun's `rec`** plus the seed's `rs0`, not the patched copy (the path may
  have changed); `solve_run` works on a local copy of `rs0`, never `x->rs`. The fixture's guard is
  one no compared constant reveals (`k[0] * 3 + 7 == 1000`; a constant in the dictionary would let
  the random phase reach it), and the check requires the edge to be credited to a solver round
  (`ss.gain`, "first by a solver round" in the solve line), not merely reached.
- **Accepted, not changed**: a seed from a dictionary phase replays its unforced draws under
  `dict_use`, so they may differ from the original run; forced values do not.

Claims the review checked and found correct: the `x.sv` / `if (sv && !s.lazy)` gates, `lazy_grow`
growing `sh`, `load_n` calling `lazy_fill` before `load_sh`, the draw inventory, `SxModel` and
`sx_sym` taking arbitrary ids (filter against the table length), `snap_restore` and `ev_kill_obj`
freeing `sh`, the final attempt's outcomes baking the hint into the seed.

Still open: whether S7's success bar should be a number (it is "below 3418 and below 3906").

## Result of stage 2 (2026-10-23)

Untaken edges, N=24 seed 1234, `--eval-link --eval-state`, solver on (runs `solver-eager-s7`,
`solver-lazy-s7` in `docs/runs.md`; gate `lazy-solver-gate`: 2315/2315 run and san, 639 findings 0):

| | eager + solver | lazy, no solver | lazy + solver |
|---|---|---|---|
| Lua | 1923 | 1701 | 1551 |
| zstd | 1974 | 1717 | 1682 |
| total | 3897 | 3418 | 3233 |

Lazy with the solver is 185 below lazy without (5%) and 664 below eager with it (17%); edges taken
5262 of 8655 (eager 4666), 973 of them first by a solver round (eager 739). Time 2909 s against
1120 s for the eager run (two runs in parallel, so not a clean timing).
Eager is unchanged: the recorded 3906 (Lua 1934) was stale. The same measurement on the pre-lazy
commit `fe5cb14` and on stage 1 `ae1ad13` (runs `bisect-eager-fe5cb14`, `bisect-eager-ae1ad13`)
gives 1923 + 1974 = 3897, as now; CONCOLIC_DESIGN.md:230 shows the 1932 -> 1923 move that came before.
`luaV_execute` still stops on huge-index out-of-bounds reads (see stage 1 result).
The replay check (`CEREAL_LAZY_REPLAY=1`, run `lazy-replay-lvm3`, Lua `lvm.c` subset and its
dependencies) found one difference, `luai_makeseed`, which hashes `time(NULL)`.
