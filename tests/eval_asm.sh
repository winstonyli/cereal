#!/bin/sh
# eval_asm.sh CEREAL: a sweep call that runs asm (outputs 0) counts its edges but is never printed, so gcc never compares it.
# tests/cfg_asm/asm.c: the x == 5 edge is reachable only through a call that runs cpuid; the edge after it needs cpuid to report a feature, so one stays untaken.
C=${1:?cereal binary}
D=$(cd "$(dirname "$0")" && pwd)
S=${EVAL_SWEEP_SOLVE:-}
if [ -z "$S" ] || ! command -v "${S%% *}" >/dev/null 2>&1; then
    echo "eval_asm: no solver, skipped"
    exit 0
fi
out=$("$C" check --target=x86_64-linux-gnu --eval-sweep=8,1234 --eval-solve="$S" "$D/cfg_asm/asm.c" 2>&1 </dev/null >/tmp/eval_asm.$$)
echo "$out" | grep -q 'edges taken 3 of 4' || { echo "eval_asm: edges taken not 3 of 4"; echo "$out" | grep -a 'cereal: solve' | head -3; rm -f /tmp/eval_asm.$$; exit 1; }
if grep -aq '^call feat 0000000005000000' /tmp/eval_asm.$$; then echo "eval_asm: a call that ran asm was printed"; rm -f /tmp/eval_asm.$$; exit 1; fi
rm -f /tmp/eval_asm.$$
echo "eval_asm: ok"
