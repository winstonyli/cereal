# Prover spike S6 (throwaway, 2026-11-13)

`s6.py` is the script behind the S6 and S3/S4 results in `docs/PROVER_DESIGN.md`. It is a spike, not a tool: Python, z3 4.13.3
at `~/tools/z3/z3`, run in WSL on a `cereal check --dump-cfg` text dump. Never let python read stdin.

```sh
cereal check --dump-cfg l1.c > l1.dump
python3 -P s6.py l1.dump blen < /dev/null            # copy:copy.dump blen:l1.dump rev:l2.dump fillnz:l3.dump
T3=1 python3 -P s6.py l2.dump rev < /dev/null        # adds the sum template (rev needs it)
DROP=0 DRIVER=/tmp/dv SRC=l1.c python3 -P s6.py l1.dump blen < /dev/null   # drop requires fact 0, write replay drivers
```

S7: `p1.c` (`pcopy`, needs `T4=1`) and `p2.c` (`fill2`, `T5=1`; invariants found, write bound not proved: nonlinear `bvmul`). Template flags: `T3` sum, `T4` equality, `T5` enclosing-loop condition.

`DROP=n` removes the n-th `requires` fact; `DRIVER`/`SRC` write `<DRIVER>_0.c` and `_1.c` (buffers filled with 0 and 1),
which run under `cereal check --eval-sweep=2,1`. The specs live in the `SPECS` dict; values are 64-bit and 32-bit wrap is not modelled.
