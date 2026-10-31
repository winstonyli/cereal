---
status: done
---

# Lazy blobs for `--eval-sweep`

Goal: the sweep's pointer structure comes from what the function dereferences, not from a
layout fixed before the call. Evidence and options: `PARKED.md`, "Pointer structure of the
sweep's generated state"; stop census in `EVAL_DESIGN.md`.

## 1. The problem

Before stage 4, `sweep_ptrs` laid out blobs breadth-first from the parameter types, up to `SMAXOB` (64) blobs
and `SMAXIN` (16 KB). Measured on `luaV_execute` (856 of Lua's 1273 never-finished edges):
at 256 blobs the budget fills with copies of `CallInfo`, `StackValue` and `GCObject` reached
through `next`/`previous` chains, and the `StackValue` under `ci->func.p` gets `gc` = NULL.
More budget does not help (Lua 1923 -> 1870 untaken, same stop); the order is wrong, and the
declared type is wrong too (`GCObject*` is downcast to `LClosure*`, a blob too small for it).
Other stops with the same cause: null function pointers (4873 calls), cross-object
subtraction (18497, `start`/`end` fields linked to different blobs), out-of-bounds reads
(24402).

## 2. Design

**Lazy bytes.** A blob made for the sweep is an `Obj` flagged `lazy`: its bytes start
uninitialized, and its size is the extent of what the call touched so far (at least 16).
Two hooks in `eval.c`, both on paths that are already slow (an uninitialized read, an
out-of-range access); the fast path is unchanged:

1. *Typed load from uninitialized bytes of a lazy object* (`load_n`, where it already notes
   `ld_obj`). The load's `Ti` says what the bytes are, so the hook draws the value then and
   there: a scalar by `sweep_arg` (dictionary draws as today), or, for `ti->ptr`, a pointer
   (section 3). The drawn bytes are written, flagged initialized, and recorded in the trace.
2. *Access past the end of a lazy object* (`ev_access`): the object grows to cover the access,
   up to a cap (4096 bytes, as `sweep_pointee` allows today); past it the call stops as an
   out-of-bounds trap. Object ids do not change (the bytes are reallocated).

Untyped reads (`memcpy` and the host functions through `ev_access`) fill the range with
zero bytes, recorded: a pointer cell copied untyped and read later is NULL. This is the
known cost of not tracking pointer-ness in bytes (see 5).

**Pointers.** The type of a pointer's target is not known at the load, only at the first
access through it. So a pointer drawn at a typed load is one of: NULL (stage 1: 1/8; now at most one NULL draw per call, `Lazy.nullat` in `eval_run.c`); a pointer
into an existing lazy blob of this call, at offset 0 or a small element offset (1/8: this is
sharing, cycles such as `l_G->mainthread == L`, and `start`/`end` pairs into one buffer,
where the offset may lie beyond the current size and is only grown into if dereferenced);
or a pointer to a new pending blob (6/8), size 0, grown and filled by hooks 1 and 2 at its
first access. A pending blob accessed as `LClosure` after being reached as `GCObject` is
just a bigger access: the downcast needs no special case.

A drawn pointer's bytes are marked init 2, and its value differs between the evaluator and gcc, so
the bytes are only ever moved whole: a load of part of it as an integer, a write over part of it
(`ev_pun_ok`: any store, `memcpy`, `memset`, libc write) and a copy of part of it or to a slot of
another alignment (`ev_copy_n`) stop as UNSUP. A whole aligned slot written as an integer is fine
(a union such as Lua's TValue): both sides hold the integer.

**Function pointers (stage 3, built).** Decided at the typed load, not at the call: the load of a
pointer cell whose type is a pointer to a function type is NULL (1/8) or a function, so no
pending blob is ever reinterpreted as code (`Ti.fty`, `lazy_fn` in `eval_run.c`). The candidates
are the swept functions (`sweep_cand`) whose function type prints the same as the pointer's
(`type_print` of the canonical type: return and parameter types as the gcc build spells them),
chosen by the PRNG and a draw key (`LZ_FN`), so the solver's forced map covers it. The link
cell names the function by its index in `Ev.fn` (top bit of the target field); the sweep prints
`fp INDEX NAME` per file, the driver fills `sw_fn` from them (`sw_fp_N`), and `sw_load` writes
the address. Fixture: `tests/cfg_exec/fnptr.c`. Covers `frealloc` and the other
call-through-null stops (about 55 edges).

**Parameters.** A pointer parameter is a pending blob, as today's blob 0 but unsized; record
parameters by value stay eager (their size is known) and their pointer fields are cells
like any other. `SA_STATE` objects (built by constructors) are unchanged.

**The call line.** Today the line is the hex of a fixed layout, and the gcc thunk has the
layout compiled in (`SW_LK` per link). With lazy blobs the line carries the materialized
graph, and the thunk is generic:

    call NAME  n | size_0 .. size_n-1 | args | links | bytes

where `args` binds each pointer parameter to (blob, offset) and each scalar to its bytes,
`links` lists (blob, offset) -> (blob, offset) or NULL for every pointer cell the call
drew, and `bytes` holds the initialized ranges only (the rest is zero on the gcc side: the
call did not read it). `sweep_rt.h` gets a loader that allocates the blobs, copies the
bytes and writes the links; the per-function thunk keeps only the call with its arguments
and the result packing. The result is unchanged (return value, returned pointer tag,
final contents of every blob with link cells zeroed).

**Lifetime.** Lazy blobs are made during a call and die with it: `sweep_call` kills them
as it kills its blobs now, and `snap_restore` already discards what a call made. This is
the same rule a copy-on-write heap would need (`PARKED.md`, "Cheaper state restore": kept
separate; nothing here depends on it).

**Solver.** `sweep_track` gives every blob byte the symbol of its input offset, and
`solve` mutates input bytes. Lazy blobs have no fixed offset: the order of materialization
changes with the path. So a lazy byte's identity is its **key**, (the pointer cell that
first led to the blob, offset in it), parameters keyed by index; the key is stable when a
path change adds or drops other blobs. A tracked run numbers symbols densely by first
use and keeps the table; a solver model is (key, offset, value) triples; a rerun consults
this forced map before drawing. Everything else in `solve` is unchanged. This is the
largest unknown, hence stage 2.

## 3. What it subsumes, and what it cannot

Deleted in stage 4 (2026-10-25): `sweep_ptrs`, `sweep_blob_ptrs`, `SPf`, `sweep_link`, the budget
constants, the union first-member rule, `sweep_pointee`'s element-count guess for
pointer-holding types, the `SW_LK` and `SW_IN`-per-blob thunk code, and the sharing loops in
`sweep_gen`. Covered by the one mechanism: the budget order (856 + 36 + 22 edges), downcasts,
`start`/`end` sharing, function-pointer fields (stage 3), out-of-bounds from short blobs.

Asm: in a sweep call it now runs with every output 0 (as in the build phase) and the call is not printed, so gcc never compares it (2026-10-31, `tests/eval_asm.sh`; `cereal run` still stops on it). Not covered: `strerror`
and similar host strings (needs a pinned string; separate); pointer-ness of untyped copies
(zero fill, section 2); unrelated lazy blobs that a function subtracts (stays a trap; with
sharing at offsets some of the 18497 become legal, not all); a read of a link cell as an
integer stops as unsupported, since cereal's object-id pointer and gcc's address differ.

## 4. Decisions (all four taken as recommended, 2026-10-23; stage 1 is in `LAZY_BLOBS_PLAN.md`)

- **D1 Replace or hybrid.** Replace the eager layout (recommended), reached through a
  parked A/B: stage 1 adds lazy as `--eval-lazy` beside the layout, is measured, and the
  layout is deleted in the same stage that makes lazy the default. Hybrid (eager first
  level, lazy deeper) keeps both mechanisms and the old budget; rejected.
- **D2 Line format.** Self-describing sections as above (recommended) versus one text token
  per blob. The former is smaller and the loader is a loop; the latter is greppable. The
  existing line is hex already, so size is not a concern; choose the binary-in-hex form.
- **D3 Function pointers in this design.** Yes, stage 3 (recommended): one more use of the
  same hook. Alternative: leave as the separate 55-edge item; it would then be a second
  special case beside the lazy hook.
- **D4 Solver order.** Sweep first, solver second (recommended): the solver must keep
  working on the eager layout until lazy is default, which is what the A/B window is for.

Stage 2 decisions (all four taken as recommended, 2026-10-23; the plan is in `LAZY_BLOBS_PLAN.md`):

- **D5 Input identity.** A lazy call's input is its draw log: every draw (scalar byte, pointer-cell
  outcome) has a 64-bit key, a hash of the path of pointer cells from a parameter plus the
  offset; a parameter's key is its index. A rerun consults a forced map (key -> value) before it
  draws. Numbering draws by order was rejected: a path change adds or drops blobs and shifts it.
- **D6 Solver symbols.** Dense ids by first use in a tracked run, with a per-run table id -> key;
  `sx_sym(uint32)` and `SxModel` are unchanged, and a model is mapped back to keys.
- **D7 What is solved.** Scalar bytes (parameters and lazily filled blob bytes) only.
- **D8 Pointer outcomes.** Replayed from the seed (so a tracked rerun is faithful), never
  changed by the solver (the offset into a shared or aliased blob is, see D11, and whether a cell is NULL, see D11a): parked in
  `PARKED.md` ("Pointer outcomes as solver choices"). The
  recorded outcomes are those of the final run of a call (after alias-hint retries).
- **D8a Retries (`lazy_redraw`).** A call that stops on something a different draw would avoid is
  rerun with those draws forced (a hint log laid over the force log; `sweep_call`, at most
  `LMAXHINT` = 4 reruns, 16 gained nothing on the sweep). One function reads the stop and names the
  cells: two blobs subtracted (the later one's pointer cell shares the earlier one, or the later
  parameter's `LZ_ALIAS` draw is set to the earlier parameter); a drawn pointer read as an integer
  (every cell that shares the blob, except the one that made it, gets a new blob). A new rule is a
  new branch there, not a new retry loop. Hints already in the log end the loop.
- **D9 Corpus.** A seed is a log (forced map); `search_keep` stores it and `mut_solve` writes
  the model's keys over a copy. The rotation `mut_gen`, `mut_solve` and `solve_step1` are unchanged.
- **D10 Untyped reads (`Ev.lazy_zero`).** The untouched bytes of an untyped read of a lazy blob (`memcpy`, a libc
  reader; `ev_lazy_zero`) are zero, as before, but each is now a draw with default 0 under the same key as a typed
  load of that byte: the call's log records it, the solver may set it, and `memcpy` carries its symbol along. So
  `MEM_read32(a) == MEM_read32(b)` has both edges (zstd's `ZSTD_count`). Reads longer than `EV_LAZY_ZERO_SYM` (16)
  bytes stay plain zeros, so a big copy makes no thousands of symbols. Host results (`memcmp`, `strlen`) are
  concrete, so no constraint flows through them; modelling those is parked (`PARKED.md` row 245).
- **D11 Pointer offsets are the solver's (`lazy_off`).** Amends D8: which object a pointer names (NULL, share, new, the
  parameter alias) is still replayed and never solved, but the offset into a shared or aliased blob is now three
  byte draws under keys derived from the old `LZ_TOFF` key, each a solver symbol like a scalar byte. A pointer's
  shadow is its whole 64-bit word as an expression (object id constant, offset the low 32 bits); `load_sh`,
  `store_sh_n` and `canon_sh` carry it through memory, parameters and results, `ptr_add_sh` moves it on `+ n`,
  `->field` and `&`, and a compare or subtraction of two pointers of one object becomes a constraint on the
  offsets. A load, store or field access through a symbolic pointer pins its word (as a symbolic index is pinned),
  and a branch on a pointer alone is not symbolic (the object decides). The offset's range (LHEAD to `LOFFMAX`, a
  whole number of pointee elements from LHEAD) goes in `Ev.dom`, asserted with every query, so the solver cannot
  hand back an offset a plain draw could not make. Plain draws keep their 8-byte steps; only the solver goes off
  that grid (fixture `tests/cfg_lazy/lazy_ptrsolve.c`). Left parked: the outcome itself as a symbol (null tests);
  that is D11a.
- **D11a Whether a pointer cell is NULL is the solver's (`SX_MASK`).** Amends D8 and D11 for NULL alone. In a tracked
  run the outcome draw of a pointer cell (`LZ_OUT`) is a solver symbol, and the cell's shadow is `SX_MASK(word, outcome)`:
  the word (a stand-in for a cell that was NULL) when the outcome is non-zero, else 0. So a test against NULL (`if (p)`,
  `!p`, `p == 0`, `p != 0`; `nullable_sh` in `eval.c`) is a condition on the outcome, and every other use strips the
  mask (`strip_sh`) and sees the word as D11 left it, so a model never turns a pointer that is dereferenced or compared
  into NULL by accident. The model says only NULL or not: `solve_run` maps 0 to NULL and non-zero to the seed's own
  non-NULL outcome (share or new), or to a new blob where the seed's cell was NULL; share against new stays replayed.
  A symbol no constraint mentions is not in the model, so the cells no test looks at keep the seed's outcome. `load_sh`
  returns the node itself for eight bytes that are its bytes in order, so a stored pointer loads back as the same node
  (the mask and the pin survive memory), and `pin` skips a constant. Not covered: a pointer parameter (always non-NULL
  by the generator), a pointer cast to `_Bool`, a compare of two pointers of different objects, a pointer a host
  function returns (fixture `tests/cfg_lazy/lazy_nullsolve.c`).

## 5. Risks

- Out-of-bounds fidelity drops: a short parameter blob grows to the cap instead of trapping.
  A function that reads past a real buffer is not reported by the sweep (it never was a
  sound check; the stop census counts it as a miss, not a finding). `tests/eval_sweep.sh` expects no
  trap on its corpus (a trap is a failure there), so no test relies on the old trap;
  growth only removes traps. Verified 2026-10-23 by reading the script.
- Reads of one lazy range through two typed views (an `int` write, then a pointer read)
  draw nothing the second time: the bytes are initialized, so the pointer is whatever the
  integer bytes decode to, an unlinked wild pointer; it traps in cereal and the call is
  not compared. Same as today's behaviour for a type-confused blob.
- Untyped fills make copied pointer cells NULL (zero): fewer edges on struct-copied
  state, never a wrong result, since the zero bytes are recorded.
- The gcc oracle sees blobs of the final size; a function whose behaviour depends on the
  allocation size (none in `libc` without `malloc_usable_size`) would differ.
- The solver key scheme can lose a path-dependent blob when the path changes: the rerun
  then draws fresh bytes for it, and the answer may be wrong or incomplete, which
  `solve` already tolerates (every answer goes through `sweep_call` and the oracle).

## 6. Stages and the check each leaves

1. Evaluator hooks, the lazy context (PRNG, dictionary, trace), `--eval-lazy` for `sweep_fn`
   (parameters pending; no solver), the new line and loader. Check: a fixture with the
   `luaV_execute` shape (`GCObject*` downcast to a struct holding a pointer to a struct
   with a field read) that must reach both edges under `--eval-sweep --eval-lazy`; and
   `sweep_meas.sh` on Lua and zstd, compared with 3897 untaken (Lua 1923 + zstd 1974; 3906 was stale).
   Falsifier: `luaV_execute` still stuck at the same place, or the oracle mismatching.
2. Solver keys (section 2; built 2026-10-23, results in `LAZY_BLOBS_SOLVER_PLAN.md`: 3233 untaken). Check: the existing solver tests, unchanged edge counts on
   the eager layout, then lazy.
3. Function pointers (built 2026-10-25, `ff63c5d`; Lua 1575 -> 1531, zstd 1682, total 3213; run `fnptr-solver-lazy`). Check: `frealloc` and the call-through-null stops gone from the
   stuck report (7 edges still stop at a call through a null or non-function pointer: `luaU_undump` via `luaM_saferealloc_`, `close_func`).
4. Make lazy the default, delete the layout code (section 3), update `EVAL_DESIGN.md` and
   `PARKED.md`. Built 2026-10-25 (`8490ca1`): `--eval-lazy`, `EVAL_SWEEP_LAZY` and the `LAZY` knobs are gone, as are the
   layout, its thunk, call, tracker, generator and byte-string seeds, `SW_LK`/`SW_IN` and `Ev.ld_max`; one `sweep_pointee_ok` check keeps the
   old size limit so the set of swept functions did not move (the limit was dropped afterwards, see below). `asm_exec.py` skips calls that draw a function pointer.
   Gate green (2316 + 2315 san); run `stage4-solver-lazy`: Lua 1531 + zstd 1682 = 3213 untaken, identical to stage 3. Later, the compare trap (`ac00c6a`) moved it to 3300 (`final-solver-lazy`), the stored-pointer and address-as-integer stops to 3431 (`addr-solver-lazy`), the new reference until the 4096-byte limit on a pointer parameter's target was dropped (`nolim2-solver-lazy`): 5790 of 11120 edges taken (5118 of 8655), the swept set grew by 2465 edges; oracles unchanged, gate green.

## 7. Review log

Round 1 (self, against the code): copies (`ev_copy_n`) of lazy ranges would draw twice for
one source (fixed: untyped reads fill with recorded zeros); out-of-range offsets for
`end` pointers (allowed beyond size, grown on access); the solver's dense input offsets
(replaced by keys); where the gcc side learns the blob sizes (the line header). Round 2:
checked the claims against `eval_run.c` and `tests/sweep_rt.h` (thunk and loader shapes,
`SA_STATE` unchanged, `snap_restore` lifetime); nothing material found beyond what
section 5 lists.
