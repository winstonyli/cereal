---
status: draft
---

# One evaluator: design

Facts are from the tree of 2026-10-21; anything unread is marked **verify**.
Decisions taken 2026-10-21 (user): interpret the T8 CFG directly, bytecode is
a later tier; step 1 (the interpreter with byte-level memory) first; JIT, PGO
capture, purity and termination bits parked (`PARKED.md`).

## 1. Why

Constant values are computed in four places that each re-implement C's
arithmetic and memory rules:

| where | works on | covers | notes |
|---|---|---|---|
| `src/c/cexpr.c` (`e_*`, `int_op`, `conv_const`, `fold_*`) | the checker's per-node arrays `ck/cv/ef` | integer and float expressions, addresses; 64-bit; gcc's ICE/fold distinctions and deferred overflow warnings | the diagnostics depend on it; no aggregates |
| `src/c/cgdata.c` `ev`/`elem` | TAST `TO_DATA` trees | static initializers: ints, floats, addresses plus offsets | no division or comparison, no `long double`, no compound literals |
| `tests/tast_data.py` | TAST `data` nodes | an oracle: evaluates them in Python and compares bytes and relocations with gcc's object file | 433 lines; independent of `cgdata.c`, so it stays a check unless the C evaluator is compared with gcc's objects directly (decide at E5) |
| `tests/cfg_exec.py` | `--dump-cfg` text | integer functions with one cell per object and byte offset | 358 lines; `asm_exec.py` imports its parser and value helpers; skips floats, aggregate copies, bit-fields, punning, pointers passed between calls, indirect calls (`PARKED.md`) |

Aggregate `constexpr` (`PARKED.md`) needs a fifth. The goal is one evaluator,
in C, over the form every later stage already shares (the typed AST lowered
to basic blocks: `tcfg_int.h`), used for:

1. `constexpr`, `static_assert` and array bounds that need aggregate or wide
   values (step E4);
2. static initializer images (replacing the `cgdata.c` evaluator, E5);
3. the execution oracle for the back end (replacing the Python interpreters,
   E2): differential tests against gcc;
4. `cereal run`, quicker dev iteration (E3).

Non-goals now: a bytecode form, a JIT, profile capture, purity or termination
analysis, a foreign-function interface (all parked, section 8).

## 2. Division of labour

The checker's folder stays the authority on *what is a constant expression*:
ICE versus fold, null pointer constants, which overflow warnings fire and
where. That logic is gcc-diagnostic parity and cannot be derived from values.
The evaluator computes *values and objects* when asked: aggregates, integers
wider than 64 bits later, byte images. Decided against merging them
(`PARKED.md`).

## 3. Input: what the evaluator reads

A function is an `El` (`tast_int.h`: `nodes`, `kids`, `bytes`) plus its
`Cfg` (`tcfg_int.h`: blocks, each a run of post-order `TNode`s, one
terminator `jmp/br/swtch/ijmp/ret`). Inside a block a node's value is
single-use, so the evaluator keeps one value slot per node of the block and
no environment. Frame state: `nreg` registers (`reg N`, stored many times),
`ntmp` temporaries (`tmp N`, address temporaries), `ncl` compound-literal
objects, the locals.

The ops it must cover are those `cgx86.c` handles (so differential testing
compares like with like): `const fconst sym tmp reg complit load store cast
unop binop cmp call intrin f2i va labaddr deref field rfield addr str ptradd
ptrdiff comma post`, `local exprstmt`, `data/elem` for static images, `asm`
refused (the set `case TO_*` in `cgx86.c` lists; `comma` and `post` are still
nodes after lowering, checked 2026-10-21). `tcfg.c` removes `logand`, `logor`,
`cond`, `elvis`, `stmtexpr` and the structured statements, so the evaluator
never sees them.

The evaluator holds no mutable globals: its state is the `Ev` struct. The
checker runs functions on several threads and `bench/tools/globals.sh` fails
on a new `.data`/`.bss` symbol, so a global evaluator state would be rejected
by the gate (and would race).

## 4. Memory model

Byte-addressed objects, because the Python oracle's cell-per-offset model is
exactly what blocks aggregate copies, bit-fields and punning.

- An object is `{id, size, bytes, init bitmap, kind, live}`; kinds: local,
  global, string literal, compound literal, heap (`run`), function.
- A pointer value is 64 bits: object id in the high 32, byte offset in the
  low 32 (signed, so one-past-the-end and negative intermediates are
  representable); id 0 is null. The encoding is deterministic. Pointer to
  integer and back round-trips; arithmetic on the integer is not
  provenance-faithful, which is reported as unsupported when the result is
  dereferenced out of its object.
- Loads and stores of any scalar width, bit-fields by the `field`/`rfield`
  offsets, aggregate copies as byte copies, `memcpy`-style punning via bytes.
- The initialization bitmap makes an uninitialized read detectable. Under
  constant evaluation it is an error; under `run` it is reported once with
  the object's name (a sanitizer, not gcc's behaviour).
- Targets: sizes, alignment and layout come from the `TypeTable` for the
  `--target`, so `constexpr` evaluates for the cross target. `long double` is
  evaluated only when the target's format is the host's (x87 80-bit on
  x86-64): otherwise "unsupported", as in `cgdata.c` today.

## 5. UB and fuel

- The evaluator never invents a value for undefined behaviour. Signed
  overflow, division by zero, out-of-range shifts, out-of-bounds or null
  accesses, access after the object's lifetime, uninitialized reads, and
  calls through bad pointers stop evaluation with a typed reason. In
  constant evaluation that reason becomes the checker's "not a constant
  expression" (gcc's wording stays the checker's concern). In `run` the
  program stops with the reason, like a trap (a sanitizer's report, not
  gcc's behaviour). This is the "checked" policy of `docs/SPECS.md`.
- The policy is per UB class (`EvOpts`), because the oracle must match
  `gcc -O0 -fwrapv`: there signed overflow wraps (`cfg_exec.py` `wrap`), so the
  oracle mode sets overflow to "wrap" and keeps the other classes as traps.
  `constexpr` and `run` use "trap" for overflow.
- Fuel: a step counter and a call-depth limit, never wall-clock time, so a
  result depends only on the input (`docs/SPECS.md`, determinism). gcc's
  C++ `constexpr` has the same notion (`-fconstexpr-ops-limit`,
  `-fconstexpr-depth`, `-fconstexpr-loop-limit`; these flags exist only for
  C++, and I did not check their defaults), so the numbers are ours, fixed at
  E1; the oracle keeps its 200000 steps per call. Exhaustion is a distinct
  outcome ("exceeded the step limit", with the limit), never "not constant"
  and never cached as a value. Static initializer images (`ev_object`) run
  under the same budget.
- C23 `constexpr` itself cannot loop or recurse: only an object's
  initializer is constant and there are no constexpr functions, so for
  E4 the limit is a safeguard. It matters for `run`, for tests, and for any
  future evaluation of user functions at compile time.

## 6. Interface

```c
/* eval.h (new) */
typedef struct Ev Ev;
Ev *ev_new(Checker *c, const EvOpts *o);          /* fuel, depth, policy */
void ev_add_func(Ev *v, El *e, Cfg *g);           /* retain a lowered function */
EvRes ev_call(Ev *v, uint32_t fsym, const EvVal *args, size_t n, EvVal *out);
EvRes ev_object(Ev *v, El *e, uint32_t data, EvImage *out); /* bytes + relocs */
EvRes ev_expr(Ev *v, El *e, uint32_t node, EvVal *out);     /* constants */
```

`EvImage` is what `cgdata.c` builds today (a byte image plus relocations), so
E5 swaps `ev`/`elem` for `ev_object`.

## 7. Layering: the one real problem

`tast_unit` runs after the checker has visited a whole unit (root is
`c->nn - 1`), and `unit_run` frees the `El` at its end
(`vec_free(&e.nodes)`, `src/c/tast.c`). Two consequences:

1. **Retention.** `cereal run` and cross-function calls need every function's
   `El` and `Cfg` to live until the program ends. With `c->opt.run` the
   unit's lowering is kept (`ev_add_func`) instead of freed. Memory is the
   lowered functions of the program, which is what a back end holds before
   emitting anyway. A "unit" is one external declaration, and the checker's
   per-node arrays (`ck`, `cv`, `ef`, ...) are reused for the next one, so
   the evaluator reads only the `El`, the `Cfg`, the `TypeTable` and the
   symbol table, never those arrays. One `Checker` (and so one `Ev`) serves
   one translation unit and its units run in order, so `ev_add_func` needs no
   lock (the threads are per TU); it copies what it keeps, and `run` starts
   after the last unit is checked. Local symbol refs (`SYM_LOCAL`) index the
   unit's own table, so the evaluator never calls `csym` on them: parameter
   types are captured when the function is added. One translation unit first: each
   TU has its own `TypeTable`, so calls across TUs would need symbol
   matching and by-value layout conversion (parked).
2. **Mid-check values.** `constexpr` values are needed *during* checking
   (array bounds, `static_assert`, enum values), before the TAST exists. The
   options:
   - **A (recommended after a spike):** elaborate one checked expression
     subtree on demand into a scratch `El` (`tast.c` already takes checker
     node ids, `El.c`), lower it to a one-block CFG, and evaluate it. A
     construct outside the TAST's subset falls back to the checker's folder
     (today's behaviour), so nothing regresses.
   - B: evaluate after the unit and re-check array bounds and similar: no,
     bounds are needed to type the rest of the unit.
   - C: a second evaluator over the checker's node arrays: contradicts the
     goal.
   E1 to E3 do not need this; only E4 does. Spike S1 below decides A.

## 8. Tiers and what is parked

Tier 0 is the CFG interpreter. A bytecode form, an in-process native tier,
profile capture, `pure` and `total` bits with memoized evaluation, and a
foreign-call interface are parked with triggers in `PARKED.md`; the design
leaves their hooks (a per-function call counter and a per-function result
cache key `csum.c`'s digest plus arguments plus fuel) out of the first
version.

### Purity, termination and caching (parked; for the record)

- `pure` (a result is a function of the arguments: no stores outside the
  frame, no volatile, no asm, no impure or unknown callee, no reads of
  mutable globals) is what memoization needs for soundness.
- Termination is a separate property. A pure function that never returns is
  still safely cached (it never produces an entry); the fuel limit, not a
  bit, keeps the evaluator total. A `total` bit only buys evaluation without
  a budget, so it stays a separate, parked bit; "foldable" would be
  `pure && total`, derived, not stored. Folding termination into `pure` would
  make a looping pure function look impure and block caching of its
  prefixes for no gain.

## 9. Steps, spikes, gates

- **S1 (spike, no commit):** (a) elaborate a checked expression into a
  scratch `El` and lower it to a one-block CFG, from where a constexpr
  initializer is finished (`cinit_decl_done`), for option A; count how many
  corpus `constexpr`, array-bound and `static_assert` operands elaborate.
  (b) Interpret five functions from the `tests/cfg_exec/*.c` inputs in a
  throwaway C file to confirm the node-slot and frame design, and measure
  the interpreter's cost per node (a tree walk over `TNode` is expected to
  be tens of times slower than native code: fine for tests, `constexpr` and
  quick runs, not for heavy programs). Estimate the size of `eval.c`
  (expected 1500 to 2000 lines, against 2275 in `cgx86.c`). Report sizes and
  surprises.
- **E1:** `src/c/eval.c`, `eval.h`: frame, objects, ops of section 3, UB and
  fuel; a debug entry `cereal check --eval-main` (hidden) that runs
  `int main(void)`. Gate: new golden tests under `tests/eval/` comparing
  results with gcc-compiled output.
- **E2:** port the oracle: the harness that compares against gcc runs the C
  evaluator on what `cfg_exec.py` covers, then on the cases it skips (floats,
  aggregates, bit-fields, punning, pointers between calls, indirect calls).
  When parity holds and the skipped cases pass, delete the Python
  interpreter in `tests/cfg_exec.py` (its parser and value helpers move to
  `asm_exec.py` or a shared module) and the parked "execution oracle" row.
- **E3:** `cereal run FILE.c...` over E1/E2 (arguments, exit status, the
  host-call whitelist for libc functions; others are reported).
- **E4:** `constexpr` aggregates and braced scalars through `ev_expr`, with
  the checker keeping ICE and diagnostics (option A).
- **E5:** replace `cgdata.c` `ev`/`elem` with `ev_object`. `tests/tast_data.py`
  keeps its job (an independent Python check of the data images against
  gcc's objects) unless E2's harness compares `ev_object` with gcc's object
  bytes directly, in which case it is retired.

Each step ends with the suite, the corpora (zstd, Lua, libuv) and the gate
unchanged, and a mutation check of the new code.

### S1 result (2026-10-21; scratch copy, nothing committed)

The spike was `spike.c` (about 500 lines) plus four hook lines, in a throwaway
copy of the tree.

- **(a) Option A is feasible, with one constraint.** A hook at the end of
  `cexpr_node` (parent not an expression, the `fold_flush` point) called
  `ex()` on the checked subtree into a scratch `El`, and a small tree walk
  evaluated it. Nothing was lowered to a CFG and `cinit_decl_done` was not
  touched; the walk read the TAST tree directly. Integer-only evaluator
  against the checker's folded value: **0 mismatches** over `tests/check`
  with `-std=c23` (3747 ok, 11 not elaborated: `__int128`, `_FloatN`,
  `labaddr`, one lvalue form), Lua 5.4 (8887 ok, 0 not elaborated) and
  zstd 1.5.7 `common`, `compress`, `decompress` (28572 ok, 0 not
  elaborated). The "unevaluable" rest (888, 4337, 4295) is `addr` (address
  constants, for which the checker has `K_ADDR`, not a value), floats and
  non-integer casts; none of it is an elaboration failure.
  Constraints: (1) `ex()` reads the checker's per-node `sym` array, which
  `grow_nodes` allocates only when `opt.tast` is set (`frontend.c:164`, from
  `--dump-tast`, `--dump-cfg`, `--dump-asm`, `--emit-obj`). Without it the
  first probe on an identifier crashed (null `c->sym`, found on zstd; a
  first run without `--dump-tast` also undercounted Lua at 1582). So
  option A needs `sym` always on, 4 bytes per node, or elaboration from a
  checker that has it. (2) `ex()` needs a function context only for
  temporaries and compound literals; the hook set `infunc`, and compound
  literals and statement expressions inside a constant were not exercised.
- **(b) The node-slot and frame design holds.** A CFG interpreter over
  `CBlk` (statement roots, `TK_JMP/BR/SWTCH/RET`, registers by `TO_REG`
  number, locals and parameters as slots keyed by symbol ref) ran all 14
  functions of `tests/cfg_exec/int.c` on 672 argument pairs: **672 of 672
  equal gcc-13 `-O0 -fwrapv`**. Speed: about 160 M nodes/s on a loop
  (`sum(n)`: 50 M nodes in 0.32 s), i.e. a few ns a node. Integer-only
  interpreter code is about 250 lines; a full `eval.c` with byte memory,
  floats, aggregates, bit-fields and calls is still estimated at 1500 to
  2000 lines.
- **Findings.** (1) `El` has no parameter list: parameters had to be
  captured at hook time from the checker's nodes (`cb` of each real
  parameter, section 3 needs an `El` field or a per-function table).
  (2) `TO_POST` wraps a `TO_STORE`, so it must read the old value before the
  store (cgx86's order); the address subtree is evaluated twice, which is
  fine only while addresses are pure. (3) Fuel as a node count worked and
  stopped a 2^31 loop at once.

### E1 groundwork (2026-10-21)

Decided after S1 and done in one commit: (1) `El.params` holds the function's
`N_PARAM` nodes, filled in `func_def`; `cgx86.c`'s own walk is deleted and
`x86_func` reads `L->e->params`. (2) `c->sym` is allocated for every unit, not
only under `opt.tast` (three sites), so elaboration works in any run; measured
cost on the gate's callgrind input: 3.9494G to 3.9503G (+0.02%), 4 bytes per
node. (3) `TO_POST`'s double evaluation of the address is safe: `once_wrap`
(`tast.c`) hoists any address with side effects into a temporary. (4) The
public elaboration entry (an exported wrapper of `ex`) is not added until E1's
first user needs it, to avoid an unused export.

### E1 result (2026-10-21)

`src/c/eval.c` and `eval.h` (about 970 lines): integers, enums, `_Bool`,
pointers (object id and offset), byte memory with an init bitmap and object
lifetimes, locals, parameters, `tmp`/`reg`, string literals, struct and union
copies at store, local, argument and return, direct and indirect calls
(arguments right to left as `cgx86.c`), `switch`, `post`, fuel and a call
depth limit. UB stops evaluation with a reason (division by zero, signed
overflow, shift counts, out of bounds, null, after lifetime, uninitialized
read, write to a string literal, pointer difference across objects). Each
unsupported construct names itself: floats, bit-fields, compound literals,
variadic calls, built-ins, globals, `static` locals.

Entry: `cereal check --eval-main` (hidden; `--eval-wrap` for gcc `-fwrapv`
parity) prints `eval main = N` or `eval main: trap|limit|unsupported: why`.
`tests/eval.sh` (step 13g of `tests/run.sh`) compares `tests/eval/*.c` with a
gcc build on the full `int` value and checks each `trap_*.c` stops with its
reason. Mutation check: 7 of 7 mutants killed (signed shift, signed `<` and
`>`, pointer scaling, argument order, aggregate copy, canonicalization), after
the first test set let two survive (folded constants and a cancelling pair of
compares, both fixed in the tests). Deviation from the first-cut plan: values
are copied in (`ev_add_func` copies, it does not steal), because `El` and
`Cfg` stay owned by `unit_run`.

## 10. Review log

Three adversarial rounds against the code, 2026-10-21; each change above came
from one of them.

- **Round 1 (claims against the tree).** (a) "`comma` and `post` are gone
  after T8" was wrong: `cgx86.c` still has cases for both, `tcfg.c` removes
  `logand/logor/cond/elvis/stmtexpr` and the structured statements; section 3
  now lists the real set. (b) gcc's `-fconstexpr-*` limits exist for C++ only
  and the defaults were not verified (`gcc-15 -Q --help` lists the flags, not
  values); the numbers are now ours, to be fixed at E1. (c) "UB stops
  evaluation" contradicted the oracle, which compares with `gcc -fwrapv`
  (signed overflow wraps): the policy is per UB class and the oracle sets
  overflow to wrap. (d) `run` "reports once" contradicted "never invents a
  value": a UB stop ends the program with the reason.
- **Round 2 (lifetimes and consumers).** (a) The checker's per-node arrays are
  reused by the next unit, so the evaluator may read only `El`, `Cfg`, the
  `TypeTable` and the symbol table. (b) Units run on several threads and
  `globals.sh` rejects new mutable globals: state lives in `Ev`, registration
  is locked or per thread, `run` starts after the last unit. (c) Deleting
  `tests/cfg_exec.py` would break `asm_exec.py`, which imports its parser and
  value helpers: only the interpreter goes. (d) `tests/tast_data.py` is an
  independent oracle against gcc's objects, not a duplicate of `cgdata.c`: it
  stays unless E2's harness covers the same ground. (e) Several TUs have
  separate `TypeTable`s: one TU first, the rest parked.
- **Round 3 (what is still unverified).** Whether a checked expression
  subtree can be elaborated and lowered on demand (option A) is the
  load-bearing assumption of E4; S1 tests it before anything is built. The
  size estimate for `eval.c` and the interpreter's speed are guesses until
  S1 measures them. Nothing else found: the doc has no remaining
  contradiction between sections 2, 3, 5, 7 and 9, and every parked item is a
  `PARKED.md` row (bytecode, native tier, profile capture, `pure`, `total`,
  checker-folder merge decided against, FFI, multi-TU).
