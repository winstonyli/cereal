---
status: draft
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
solved (D8).

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
stuck lines. Success: eager unchanged; lazy-with-solver below lazy-without (3462) and below 3906.
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
else material found on a full pass. Not yet stress-tested by a second reader.
