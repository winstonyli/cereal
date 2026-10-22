---
status: draft
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

## 8. Review log

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
