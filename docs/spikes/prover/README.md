# Prover spike S6 (throwaway, 2026-11-13)

`s6.py` is the script behind the S6 and S3/S4 results in `docs/PROVER_DESIGN.md`. It is a spike, not a tool: Python, z3 4.13.3
at `~/tools/z3/z3`, run in WSL on a `cereal check --dump-cfg` text dump. Never let python read stdin.

```sh
cereal check --dump-cfg l1.c > l1.dump
python3 -P s6.py l1.dump blen < /dev/null            # copy:copy.dump blen:l1.dump rev:l2.dump fillnz:l3.dump
T3=1 python3 -P s6.py l2.dump rev < /dev/null        # adds the sum template (rev needs it)
DROP=0 DRIVER=/tmp/dv SRC=l1.c python3 -P s6.py l1.dump blen < /dev/null   # drop requires fact 0, write replay drivers
```

S7: `p1.c` (`pcopy`, needs `T4=1`) and `p2.c` (`fill2`, `T5=1`; proves through the integer tier, `INT=0` turns it off and the write bound fails: nonlinear `bvmul`). Template flags: `T3` sum, `T4` equality, `T5` enclosing-loop condition.

`DROP=n` removes the n-th `requires` fact; `DRIVER`/`SRC` write `<DRIVER>_0.c` and `_1.c` (buffers filled with 0 and 1),
which run under `cereal check --eval-sweep=2,1`. The specs live in the `SPECS` dict; values are 64-bit and 32-bit wrap is not modelled.

S8 (2026-11-19, `s8/`): `fill2` over `int` (the PARKED "Narrow-width index arithmetic" row). `python -P s8/gen.py` writes 12 Int queries
(`QF_NIA`: values as signed Ints in [-2^31, 2^31), the index's unsigned 64-bit view as a two-case disjunction, one query per goal or
no-wrap obligation, one `requires` dropped per group); `s8/bv_goal.smt2` is the same goal in 32-bit bit-vectors. Result (z3, this
machine): the Int queries answer in 0.2-0.3 s each, `unsat` with all of `r >= 0, c >= 0, r*c <= $len(d), r*c <= INT_MAX` and `sat`
(not proved) when `r*c <= $len(d)` is dropped (goal) or `r*c <= INT_MAX` is dropped (both no-wrap obligations); the bit-vector query
has no answer in 90 s. So the narrow Int tier works at 32 bits with signed values and a disjunction for the unsigned view; the open
work is the implementation and its soundness review, not z3 cost.
L1 (2026-11-21, `s8/l1_*.smt2`): the same `fill2` goal with z3's own Int/bit-vector bridges, 32-bit variables with a signed Int view through `bv2nat` (`l1_mixed`) and Int variables with the goal in bit-vectors through `int2bv` (`l1_int2bv`). Both time out at 50 s, like the pure bit-vector query, so the speed of S8 comes from keeping the query in Int, not from z3's conversions. Narrow-width arithmetic stays in the Int tier and converts at the edges.
