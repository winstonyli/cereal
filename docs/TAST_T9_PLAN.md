---
status: done
---

# TAST stage T9: aggregates and initializer lists (done, in the back end)

The first draft (2026-10-18) planned a CFG-level lowering to `copy`/`fill`
statement ops, a verifier rule and a byte-level oracle memory. Stress-testing
it against the code and the corpora (2026-10-19) replaced it with the design
below; the draft is in git history (`git log -- docs/TAST_T9_PLAN.md`).

## Design

An aggregate (struct, union, array) value is its address: `gen` of an
aggregate-typed node leaves in `%rax` the address of memory holding it
(`cgx86.c`, "aggregates").

- `load` of an aggregate is the lvalue's address; `store` evaluates the
  destination, then the source, copies `sizeof` bytes and yields the
  destination (so `c = b = a` works).
- A copy of up to 64 bytes is unrolled through `%rdx` in 8/4/2/1-byte moves;
  larger ones use `rep movsb` (`M_REPMOVSB`, bytes `f3 a4`, checked against gas).
- `local x = <aggregate>` is the same copy into the variable's slot.
- `local x = { elems }`: the object is zeroed (`xor` plus stores up to 64
  bytes, else `rep stosb`, `M_REPSTOSB`), then each `elem` is stored in source
  order at its byte offset. A string element is copied cut to its array; a
  struct-valued element is copied. This mirrors the static evaluator's `elem`.
- A `reg`/`tmp` of aggregate type is refused (those slots are 8 bytes).

No new TAST node, so no change to the T8 graph, its goldens or `--verify-cfg`.
Other back ends reuse the same convention or lower the same shapes themselves;
the shared lowering the draft proposed would only have covered `store (load)`.

## Calls and returns (T10, done 2026-10-19, same back-end approach)

System V classification in `cgx86.c` (`agg_class`, `gen_call`, `ret_agg`,
callee prologue): up to 16 bytes an aggregate is one or two eightbytes, each
INTEGER (any non-float scalar or bit-field) or SSE (only floats/doubles),
passed in the next free `rdi..r9` / `xmm0..7` registers, or on the stack when
they do not all fit; over 16 bytes, or packed, it is copied to the stack
(argument) or returned through a hidden `%rdi` pointer that comes back in
`%rax`. Results in registers (`rax`, `rdx`, `xmm0`, `xmm1`) are stored to a
per-call frame temporary and the call yields its address. A cast to a
`transparent_union` passes as its operand. Limits: a register-class aggregate
whose last eightbyte is 3, 5, 6 or 7 bytes, `long double` members, complex.
Checked against gcc in both directions by `tests/asm_exec/abi.c` plus
`tests/asm_exec/gcc/abi.c` (built by gcc and linked into both programs; the
harness drives its `gcc_*` functions); a mutant that classifies every
eightbyte INTEGER is caught.

`va_start`/`va_arg`/`va_copy`/`va_end` (`gen_va`, 2026-10-19): a variadic
function saves `rdi..r9` and the low halves of `xmm0..7` into a 176-byte frame
area at entry; `va_start` fills the 24-byte list from the named-argument
counts; `va_arg` takes the register area while the offset allows, else the
overflow area. Checked by `tests/asm_exec/va.c` (+ `gcc/va.c`), two mutants caught.

## What changed in the plan, and why (review log)

- **Round 1: code against plan.** The consumer is a tree-recursive generator,
  and "value is an address" covers store, local initializer, nested element
  and chained assignment with one mechanism. A separate pass with `copy`/`fill`
  nodes, a verifier and regenerated goldens bought nothing the back end needs.
  Dropped.
- **Round 2: the spike was wrong.** It reported 0 aggregate stores and loads
  in lua54. Counting `--dump-cfg` types by the words `struct`/`union` misses
  typedef names (`Value`, `TValue` is a struct with a union member). Real
  count from the back end: 102 lua54 and 33 libuv functions stopped on an
  aggregate store (`*o1 = *o2`-style copies of 8 to 16 bytes dominate lua).
- **Round 3: the biggest bucket was not aggregates.** 58 functions (52 libuv,
  6 lua) failed as "value of a type outside the subset" because an unnamed
  `void` parameter, `f(void)`, was treated as an argument. A bug in
  `params()`, fixed with a `(void)` function in `tests/asm_exec/data.c`.
- **Round 4: oracle.** The byte-level memory the draft made step 1 is only
  needed by the CFG interpreter; `tests/asm_exec.py` runs real memory against
  gcc, so `tests/asm_exec/agg.c` (13 functions: small/mixed/large copies,
  pointer and chained assignment, unions, partial and zero lists, strings,
  nested lists) is the check. Mutants confirmed it detects a short `rep movsb`
  and a short `rep stosb` (the latter needs stale stack bytes, hence `dirty`).

## Result

lua54: 2160 of 2174 functions emit (was 2052); libuv: 1541 of 1546 (was
1455) with T10 and `va_*`; lua54 2172. Suite 2283 passed. Objects pass `readelf` and assemble.

## Not done (see `PARKED.md`)

Aggregate `const`, `cond`, `comma`, compound-literal
values; bit-field elements of a local list; `va_arg` of register-class aggregates; label addresses.
