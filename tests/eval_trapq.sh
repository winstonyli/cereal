#!/bin/sh
# eval_trapq.sh CEREAL: the trapped-call queue of the solver sweep (src/c/eval_run.c, trap_step).
# tests/cfg_trapq/trapq.c has an edge only reachable through a call that traps; with the queue the solver
# takes every edge, without it one stays untaken.  Needs EVAL_SWEEP_SOLVE (e.g. 'z3 -in -smt2'); skipped without.
C=${1:?cereal binary}
D=$(cd "$(dirname "$0")" && pwd)
S=${EVAL_SWEEP_SOLVE:-}
if [ -z "$S" ] || ! command -v "${S%% *}" >/dev/null 2>&1; then
    echo "eval_trapq: no solver, skipped"
    exit 0
fi
out=$("$C" check --target=x86_64-linux-gnu --eval-sweep=8,1234 --eval-solve="$S" --eval-only=deep "$D/cfg_trapq/trapq.c" 2>&1 </dev/null >/dev/null)
echo "$out" | grep -q 'untaken edges 0:' || { echo "eval_trapq: an edge stayed untaken"; echo "$out" | grep -a 'cereal: \(solve\|untaken\|call\)' | head; exit 1; }
echo "eval_trapq: ok"
