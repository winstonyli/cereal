#!/bin/sh
# eval_lazy.sh CEREAL: the sweep reaches what the old fixed layout's budget lost
# (docs/LAZY_BLOBS_DESIGN.md).  tests/cfg_lazy/lazy_downcast.c is the shape of Lua's luaV_execute: a
# pointer cast to the larger struct it starts, and a pointer inside it that decides the result.  The
# sweep must print calls with the five results of lz_run past the first pointer (the first four result bytes, hex; -1 needs the call's one NULL at draw 0, so it is not required).
C=${1:?cereal binary}
D=$(cd "$(dirname "$0")" && pwd)
got=$("$C" check --target=x86_64-linux-gnu --eval-sweep=24,1234 "$D/cfg_lazy/lazy_downcast.c" 2>/dev/null </dev/null |
    awk '$1 == "call" && $2 == "lz_run" && $4 == "=>" { print substr($5, 1, 8) }' | sort -u | tr '\n' ' ')
want="01000000 02000000 03000000 fdffffff feffffff "
if [ "$got" != "$want" ]; then
    echo "eval_lazy: lz_run results: got '$got', want '$want'"
    exit 1
fi
echo "eval_lazy: ok"
# With the solver, lazy calls are solved too: lg_run's guard needs k[0] == 331, behind a pointer.
if command -v z3 >/dev/null 2>&1; then
    got=$("$C" check --target=x86_64-linux-gnu --eval-sweep=24,1234 --eval-solve='z3 -in -smt2' "$D/cfg_lazy/lazy_guard.c" 2>/dev/null </dev/null |
        awk '$1 == "call" && $2 == "lg_run" && $4 == "=>" { print substr($5, 1, 8) }' | sort -u | tr '\n' ' ')
    case "$got" in
    *09000000*) ;;
    *) echo "eval_lazy: lg_run with the solver: got '$got', want 09000000 among them"; exit 1 ;;
    esac
    echo "eval_lazy: solver ok"
fi
