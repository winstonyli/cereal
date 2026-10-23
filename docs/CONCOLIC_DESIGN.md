---
status: active
---

# Concolic input search for `--eval-sweep`

Goal: inputs that reach edges the seeded random calls and the dictionary draws do not
(`EVAL_DESIGN.md`, "Where the missed edges are": 28% of the missed edges are in functions
whose calls mostly finish, so they are data-dependent compares, not stops). Method: run a call
concretely, record the branch conditions as expressions over the call's input bytes, negate
one, ask a solver for bytes that satisfy the prefix and the negation, run the result as one
more call. Every solved call goes through `sweep_call` like any other, so it is printed only
if it finishes and is compared with gcc by the same oracle: the solver can be wrong or
incomplete and the sweep stays correct.

## 1. Pieces

1. **Expressions** (`src/c/symx.c`, `symx.h`): an arena per call of nodes
   `{op, width, a, b, k}` over 64-bit slot values and 8-bit input symbols `in[k]`
   (k is the byte offset in the call's input). No hash-consing in v1. Cleared at the start of
   every tracked call.
2. **Shadow values.** `ev_node` gains a side result `v->sh` (an expression id, 0 =
   concrete), reset to 0 on entry and set only by the cases that propagate it, so any
   operation not listed is a concretization (sound for search, a lost opportunity at worst).
   Propagating cases: `TO_CONST` (0), `TO_LOAD`/`TO_RFIELD` of scalar integers (from the
   bytes' shadows), `TO_STORE` (the stored value's), `TO_CAST` integer to integer,
   `TO_UNOP` (`-`, `~`, `!`), `TO_BINOP` integer (`+ - * / % & | ^ << >>`), `TO_CMP`
   integer, `TO_COMMA` (second), and arguments, parameters and return values.
3. **Shadow memory.** `Obj.sh`: `uint32_t *` per byte, allocated on the first symbolic
   store, freed with the object. `store_n` splits an integer's expression into bytes
   (extract), `load_n` concatenates them and extends as `ev_canon` does. Every writer of
   `Obj.b` clears or sets the shadow: seven sites in `eval.c`/`eval_libc.c` (found by
   `grep "\->b\["`) and `ev_copy_n` (copies shadows), `ev_zero_n` (clears). Host functions
   other than `memcpy`/`memmove` read and write concretely (limitation, section 6).
4. **Path.** `run_fn` records at each `TK_BR` and `TK_SWTCH` whose condition has a shadow:
   `{expr, Blk*, edge taken}`; at most 256 entries per call. Concretized symbolic values that
   steer memory or traps (pointer index of `TO_PTRADD`, shift count, divisor) add a *pin*
   entry `expr == value`, so a solved prefix keeps the same addresses.
5. **Edge bookkeeping.** `Blk.seen` (one bit per edge, as the scratch probe has it) becomes
   a real field: it says which edges any call has taken. A branch is a target when its other
   edge is unseen. The same field gives the sweep a coverage number.
6. **Solver interface** (plug and play): `Solver` in `symx.h` is
   `{ open(cmd), close, check(prefix, target) -> SAT/UNSAT/UNKNOWN + model bytes }`
   at the level of the expression DAG. Backend 1 prints SMT-LIB2 (QF_BV) to a solver process
   over a pipe (`z3 -in -smt2`, or any SMT-LIB2 solver: cvc5, bitwuzla, yices), selected with
   `--eval-solve=CMD`. A later in-repo backend (bit-blaster and SAT core) implements the same
   three functions on the same DAG, so nothing above it changes.
7. **Driver** (`eval_run.c`, after the dictionary phase, only with `--eval-solve`): for each
   function with unseen edges and each of up to 4 finishing random inputs: rerun with tracking
   on, walk the path once keeping the prefix asserted (push, assert negation of entry i,
   check, pop, then assert entry i), and for each SAT result build a new input (old bytes
   overridden by the model's symbols) and run it tracked, enqueueing it. Budgets: 48
   queries per function, solver `rlimit` per query (deterministic, not wall-clock). A solved
   call that does not finish is dropped like any other.

## 2. What is symbolic

Scalar parameters' bytes and the data bytes of every blob. Not symbolic: pointer fields, alias
words and the layout (they are structure, and a model must not rewrite them: their input
bytes are excluded from the symbol set). Pointers never are (object ids are concrete);
symbolic offsets are pinned (section 1.4).

## 3. Exactness

"Exact" holds for the modelled operations: a query is a conjunction of bit-vector facts
about the input bytes and the solver's model satisfies them. Anything concretized (floats,
host functions, bit-fields, atomics, overflow built-ins, pointer values) enters the path as a
constant, so a model can still make a later branch go the other way; the check is the run
itself, which is what is printed and compared. No claim of completeness.

## 4. CLI and failure

`--eval-solve=CMD` with `--eval-sweep`. If CMD does not start or does not answer `sat` to a
trivial probe, the sweep exits with an error naming the command; it does not fall back to the
random calls silently.

## 5. Checks

`tests/cfg_exec/solve.c`: functions guarded by constants and joint conditions (`p[0] == 0x42 &&
p[1] * 3 == 99`, `x * x == 144`, a switch on a sum of two bytes, a byte compared with a
neighbouring byte). `tests/eval_sweep.sh` with `EVAL_SWEEP_SOLVE` set runs the whole cfg_exec
set with the solver (results must still equal gcc's) and checks that, for `solve.c`, a call
reaching the guarded edge exists (an input with the magic bytes in the printed calls).
Skipped with a note when the command is not found. The seen-edge count per function is
printed by `--eval-sweep` with `-v`-style output so coverage can be compared with and without
the solver.

## 6. Known limits

- Host functions (`strcmp`, `memcmp`, `strlen`, `qsort`) read symbolic bytes and return
  concrete results; a branch on `strcmp(a, "kw") == 0` is therefore not flipped. Fix later:
  a shadow rule per host function that matters.
- Symbolic pointer offsets are pinned, not explored.
- Loops: the path is cut at 256 entries; deep loops give a short prefix.
- Floats and bit-fields stay concrete.
- Needs a POSIX `fork`/pipe (Linux, WSL); no Windows-native backend.

## 7. Plan of work

T1 `symx.c`: arena, printer to SMT-LIB2, pipe backend, probe; a unit check that solves
`x * 3 == 99` through the command. T2 shadow memory and the propagating cases in `eval.c`; with
tracking off every number is unchanged (gate). T3 path recording and `Blk.seen`. T4 driver
and `--eval-solve`. T5 `solve.c`, the sweep check, measurements on zstd and Lua, gate, docs.

## 8. Result (2026-10-22; T1-T5 built, working tree on `64507ad`, gate row `concolic-t1-t5b` in `docs/runs.md`)

As built (differences from section 1 in bold):
- Expressions: ids are arena index + 1, ops are SYM/CONST/BYTE/JOIN/EXT plus 64-bit slot ops and
  six compares; `sx_byte` looks through EXT and JOIN so a store/load round trip yields the
  original byte. **`ev_node` is a wrapper over `ev_node1(r, n, &sh)`**, not a side field the cases
  reset; propagating cases set `*sh`, the rest leave 0. Pointer and float values are never
  symbolic (`Ti.ptr`, `Ti.flt`).
- Solver: `sx_begin` resets, sets `produce-models`, optional `rlimit`, logic `QF_BV`; nodes are
  printed as `define-fun` once per session before `(push 1)`. **`sx_try` takes an array of
  `SxCond {expr, want}`** (a switch alternative is several conditions), answers SAT/UNSAT/UNKNOWN/ERROR
  and returns the declared symbols' bytes. `sx_open` probes with `x*3 == 99`; a solver that fails
  makes the sweep exit with code 2.
- Edges: `Blk.seen` bits are branch 0/1; switch cases by index (bits 0..60, later ones share 61),
  default 62. Pins are applied to `TO_PTRADD` indices and to divisors and shift counts.
- Driver (`solve_fn`): 48 queries per function, 4 seeds, 8 expansions per seed, `rlimit` 5000000 per
  query. Every blob data byte and scalar parameter value is symbolic; a bool parameter is
  normalized to 0/1 after a model.
- Stats line on stderr: queries, sat/unsat/unknown, calls, fresh edges, edges taken of total and how
  many of them a solver round took first (callees included), max nodes, functions seeded, tracked
  runs, symbolic branch steps, steps with an unseen edge.
- **One search loop** (2026-10-22; replaces the phases of section 1.7): per function a corpus of
  finishing inputs (`SSearch`: the first 4, then any that took a new edge, at most 16) and rounds of
  three mutations: random draws (the first N, collecting the dictionary), dictionary draws (64 rounds),
  and `solve_step` (one tracked run of the next corpus input and its queries, interleaved after each
  dictionary round, then until the caps `SEXPAND` 8 tracked runs and `SQUERY` 48 queries are spent). A
  solved child that takes a new edge joins the corpus and is tracked in turn. Every finishing call is
  printed and compared with gcc as before. Variants parked: `PARKED.md` (round budget by progress,
  yield-weighted mutations, printing only new-edge calls). Corpus (zstd and Lua 5.4, N=24, run
  `2026-10-22-sweep-unified-24e`): 6164 of 14106 edges (43.7%) against 5991 (42.5%) with the
  phases (`sweep-base-24`): the solver now starts from every corpus input, not four seeds.

Checks: `tests/symx_unit.c` (solves `x*3 == 99` through the command); `tests/cfg_exec/solve.c`
(without the solver none of the five guarded calls reaches its edge; with it the edges taken go
13 -> 18 of 18); with the solver int.c, mem.c, ops.c, ptrs.c and solve.c still equal gcc call for
call (19/19, 5/5, 137/137, 22/22, 6/6); tracking off leaves every number unchanged (46 eval tests,
the sweep).

Corpus (zstd and Lua, N=24): +123 edges, 43.4% -> 44.8%. The solver finds what is data-guarded; most
of the remaining misses sit behind state (context structs that are zero, tables built by earlier
calls), so the next lever is the state fill and the generator, not input solving.

Why edges stay untaken (survey, 2026-10-22, run `2026-10-22-sweep-link-miss2`: Lua and zstd as two
links, N=24, solver on; `miss_report` in `eval_run.c`, printed with the solver's stats line; each
untaken edge is classed by its branch's condition). 4248 untaken edges:

| class | Lua | zstd | total |
|---|---|---|---|
| function never finished (no call completed) | 1561 | 313 | 1874 (44%) |
| integer value (a truth test of a loaded integer) | 185 | 753 | 938 |
| call result | 54 | 541 | 595 |
| integer compare | 238 | 238 | 476 |
| pointer compare (two pointers) | 52 | 91 | 143 |
| pointer null test | 104 | 13 | 117 (2.8%) |
| switch | 67 | 30 | 97 |
| float | 8 | 0 | 8 |

Readings: a pointer-topology mutation (NULL, alias, link per slot) has a ceiling of about 260 edges
(null tests plus pointer compares, 6%), so it is not the lever. The biggest class is functions
where no call ever finishes (Lua: 1561 of 2269). The loaded-integer and call-result classes (zstd) are
state the generator leaves at its random bytes or zero.

The same functions by the first reason a call stopped (run `2026-10-22-sweep-link-why2`, edges of
the functions that never finished, examples in brackets):

| first stop | edges | examples |
|---|---|---|
| read through a null pointer | 1060 | `lua_geti`, `aux_rawset`, `lua_next` (Lua) |
| read out of bounds | 171 + 90 | `lua_arith`, `lua_gettable`; `ZSTD_splitBlock_byChunks` |
| `asm` (not supported) | 83 | `ZSTD_cpuSupportsBmi2` |
| subtraction of pointers into different objects | 80 + 63 | `ZSTD_updateDUBT`; `luaD_growstack`, `relstack` |
| call through a null pointer | 71 | `lua_pushlstring`, `lua_createtable` (the allocator in the global state) |
| pointer arithmetic out of range | 44 + 29 | `luaK_nil`; `ZSTD_insertBt1` |
| strcmp through a null pointer | 27 | `auxgetstr`, `luaT_init` |
| a callee with no body and not in the host library | about 80 | `strpbrk`, `strspn`, `getc`, `strcoll`, `fmod`, `strtod` |

About 1450 of the 1874 edges stop on the Lua API and zstd match-state functions meeting a state whose
invariants fail: `L->ci`, `L->top` against `L->stack`, `G(L)->frealloc` and the string table are
NULL or unrelated pointers in a generated state. This is the case for building state with earlier
calls (`luaL_newstate`, `ZSTD_createCCtx`), the parked 2b in `PARKED.md`; a deeper or better-shaped
generated record would not repair relations between its pointers. The cheap rest: `asm` (83),
more host-library functions (about 80).

State from earlier calls (prototype, 2026-10-22; `--eval-state`):
- A constructor is a function with no parameters that returns a pointer to a record (found by
  shape). It runs before everything else in a building phase (`Ev.build`), where (as everywhere
  now: `time` and `clock` return 0 and the sweep's gcc driver defines them the same way) `_setjmp` returns 0 and
  `longjmp` stops the call as an error path (real non-local return is parked). The objects it made
  are snapshotted whole.
- A function with a parameter of such a pointer type gets the returned pointer (object and offset;
  `luaL_newstate` returns the `lua_State` inside a larger allocation) instead of a blob made from the
  type, and every call restores the snapshot afterwards, so calls stay independent. These functions
  are swept in variants (below). Their gcc side is a thunk named `NAME__stN` that replays the
  builders per call (by symbol alias when one is in another file; a static builder in another file
  leaves the function unswept with state) and compares call results only: the return value and the
  input blobs, not the state graph (decision 3a).
  A `const Ctx *` parameter takes the `Ctx *` a constructor returns (the key drops qualifiers). The
  driver defines `time` and `clock` as 0 so a constructor that reads the clock agrees with the build
  phase. Test: `tests/cfg_state/state.c`, stage 13e1s of `tests/run.sh`.
- Chains and several states of one type (step 3): after the plain constructors, two rounds run a
  builder that takes a state of its own return type on each state the round began with
  (`lua_newthread(L)`), giving up to `SMAXPER` (4) states of a type, 16 in all; the result must be a
  new object. A function with a state parameter is swept once per way to pick (`NAME__st0`,
  `__st1`, ...: the nth state of each parameter's type, modulo their number). The gcc thunk replays
  every builder in order, so its state matches the snapshot exactly; a function whose file cannot call
  every builder (a static one in another file) is not swept with state. Builders of another type
  (`luaH_new(L)`, `luaF_newCclosure(L, n)`, getters such as `index2value`) were tried and left out:
  they disturb the first state and lost edges (4449 against 4653; run `...state6`).
- One loop (variants): `eval_sweep` visits each function once and sweeps it in variants: one per
  way to pick its built states (`state_variants`: the largest number of states of any parameter's type,
  0 when it takes none or a file cannot call every builder), then the plain one with generated
  parameters. There is no separate state pass, no held output and no pass flag. The variants share the
  function's edges (`Blk.seen`) and a function counts as finished if any variant finished. Run
  `...state8`: untaken edges Lua 1934, zstd 1972 (1943 and 1978 with two passes); the "edges taken"
  total reads 4660 because a function's count is taken at its own sweep and no longer sees edges later
  callers take (the final untaken report is the measure).
- Result with chains (run `2026-10-22-sweep-link-state7`): edges taken 4693 of 8655 (54.2%); Lua
  untaken 1943 (never finished 1293), three `lua_State`s; zstd unchanged (1978).
- Build-phase `asm` zeroes its output lvalues (any asm, not cpuid-specific), so `ZSTD_cpuid` builds
  contexts. Host functions added for the state pass: `fflush`, `__errno_location` (one pinned int),
  `strstr`, `strspn`, `strcspn`, `strpbrk`, `strcoll`, `fmod`, `ldexp`; `Obj.pin` keeps host objects
  across a restore.
- Failing externals (`h_fail` in `eval_libc.c`; later, run `sweep-link-state10`): file, environment and
  locale functions (`fopen`, `getenv`, `getc`, `fread`, `setlocale`, ...) and zstd's optional trace hooks
  are host functions that return 0 with no effect, so a call that reaches them goes on down its error
  path. Lua 1932 -> 1923 and zstd 1972 -> 1974 untaken (-7 of 3906); state-pass stops 862 -> 826. Not
  `system` and `remove` (effects), `strtod` and `__ctype_b_loc` (need real results); `strerror` (11
  stops) is next in the census. A full sandbox that runs externals natively was weighed and not built:
  random arguments make a real `fopen` fail the same way, so it would reach the same error paths.
- Result with these (run `2026-10-22-sweep-link-state4`): edges taken 4653 of 8655 (53.8%), 663
  queries. Lua untaken 1973 (never finished 1316). zstd builds CCtx/DCtx but stays at 1988 untaken
  (never finished 331): a fresh context reaches only the fresh-context behavior, so it needs
  configured contexts (chains, several states of one type).
- Result (run `2026-10-22-sweep-link-state3`, Lua and zstd as two links, N=24, solver on): edges taken
  4612 of 8655 (53.3%) against 4341 (50.2%) without the flag, +271 edges, all in Lua (untaken 2269 ->
  2019; the never-finished class 1561 -> 1365). zstd is unchanged: its constructors stop on `asm`
  (`ZSTD_cpuid`), only the `XXH32` and `XXH64` states and `ZSTD_CCtx_params` are built.
- The first attempt passed offset 0 and 405 of 487 state-pass stops were "read of an uninitialized
  value"; keeping the whole returned pointer fixed it. Taking the snapshot after the normal pass
  copied every object the pass had leaked (20 GB): the state is built first, on a clean heap (127
  objects, 5878 bytes for Lua).
- What still stops with built state (zstd, state pass): 11 uninitialized reads, 7 `ZSTD_trace_compress_begin`
  with no body, reads out of bounds and through null pointers.
- Call sequences (`--eval-seq`, a prototype, reverted; runs `sweep-link-seq1..4` in `docs/runs.md`): up to
  3 slots in the input, each naming a pool function (integer and state parameters only, 8 random probes,
  kept if it finished and changed the state without destroying it) and its integer arguments, run on the
  state before the target. Untaken edges, Lua + zstd: 1900 + 1969 = 3869 against 1934 + 1972 = 3906 with
  `--eval-state` alone (-37, under 1%), at 611-723 s against 576 s on Lua. A first-found pool of 64 gave
  Lua 1878 but ran over 20 min. A pool ranked by (finished, fewest bytes changed) is cheap and weak:
  `lua_pushnil`, `lua_status`, ... change little and unlock little. Fault-directed retry (the load that
  produced the bad pointer or byte is tracked; a pool function seen to write that word runs first): 197
  traps tried a writer, 1 finished, no new edge. The 1275 Lua functions that never finish stop on state
  invariants (`abort()` in `luaD_throw` with no handler, OOB and null reads of fields no pool call sets),
  which a prefix of integer-argument calls does not establish. Decided against; per-shape builders stay.

## 9. Review log

Round 1 (against the code): (a) registers and temporaries are frame objects, so all data
flow outside one expression passes through memory bytes: shadow memory plus a per-node side
result covers it; no separate register shadow. (b) argument passing and returns go through
`args[]`/`result` in `run_fn`: they need parallel shadow arrays (parameters are stored with
`store_n`, aggregates with `ev_copy_n`, which already carry shadows). (c) a model that moves a
pointer index changes the path: pin entries (1.4). (d) a model that rewrites a pointer field or
alias word breaks the blob layout: excluded from the symbols (2). (e) `strcmp` and friends: limit
6, not a correctness issue because the run is the check. (f) a stale shadow after an unknown
writer would give a wrong expression, not a wrong value: all seven writers are listed in 1.3
and the invariant is checked in debug by comparing `load_n` of a shadowed byte against its
expression evaluated on the input.
Round 2: (g) nondeterminism: the solver's model is arbitrary but the same version answers the
same; the printed calls depend on the solver version, no golden of the call list exists
(`eval_sweep.sh` compares evaluator to gcc per call), so only the coverage number varies;
`rlimit`, not time, bounds each query. (h) silent fallback: forbidden, section 4. (i) target
edges inside callees: allowed (a path entry names its `Blk*`), the caller's input still
drives them.
