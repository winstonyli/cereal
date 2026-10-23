#!/bin/sh
# eval_lazy.sh CEREAL: --eval-lazy reaches what the eager layout's budget can lose
# (docs/LAZY_BLOBS_DESIGN.md).  tests/cfg_exec/lazy_downcast.c is the shape of Lua's luaV_execute: a
# pointer cast to the larger struct it starts, and a pointer inside it that decides the result.  The
# lazy sweep must print calls with all six results of lz_run (the first four result bytes, hex).
C=${1:?cereal binary}
D=$(cd "$(dirname "$0")" && pwd)
got=$("$C" check --target=x86_64-linux-gnu --eval-sweep=24,1234 --eval-lazy "$D/cfg_exec/lazy_downcast.c" 2>/dev/null </dev/null |
    awk '$1 == "call" && $2 == "lz_run" && $4 == "=>" { print substr($5, 1, 8) }' | sort -u | tr '\n' ' ')
want="01000000 02000000 03000000 fdffffff feffffff ffffffff "
if [ "$got" != "$want" ]; then
    echo "eval_lazy: lz_run results: got '$got', want '$want'"
    exit 1
fi
echo "eval_lazy: ok"
