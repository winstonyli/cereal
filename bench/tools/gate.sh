#!/bin/sh
# gate.sh: the pre-commit gate, stages overlapped (<= 12 busy threads):
#   run.sh (3 golden shards) | verify.sh (4) | san.sh (make -j4, 3 shards, san.py 4) | callgrind (1)
# Writes /tmp/gate.<stage> and prints them in order.  Expect run/san "N passed, 0 failed"
# (N grows with the goldens), san "findings 0", verify dg 74 / c-c++-common 35 differ (of 3744 / 636 files run),
# callgrind ~3.70G (instructions, deterministic).
# Inner loop while iterating: `sh tests/run.sh` alone (GOLDEN_JOBS=n shards its golden loops).
cd "$(dirname "$0")/../.." || exit 1
O=/tmp/gate; rm -f $O.*
GOLDEN_JOBS=3 nice -n 10 sh tests/run.sh 2>&1 | tail -1 >$O.1run &
CEREAL_JOBS=4 nice -n 10 bench/tools/verify.sh 2>&1 | tail -3 >$O.2verify &
SAN_JOBS=4 GOLDEN_JOBS=3 SAN_PY_JOBS=4 nice -n 10 bench/tools/san.sh 2>&1 | tail -2 >$O.3san &
nice -n 10 valgrind --tool=callgrind --callgrind-out-file=/dev/null ./cereal -fsyntax-only -std=c99 -w \
    -I/usr/include/python3.13 -I"$HOME/corpus/uvloop-0.22.1/vendor/libuv/include" \
    -I"$HOME/corpus/uvloop-0.22.1/uvloop" "$HOME/corpus/uvloop-0.22.1/uvloop/loop.c" 2>&1 |
    grep Collected >$O.4cg &
wait
for f in $O.*; do echo "== ${f#$O.}"; cat "$f"; done
