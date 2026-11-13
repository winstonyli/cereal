#!/bin/sh
# gate.sh: the pre-commit gate, stages overlapped (<= 12 busy threads):
#   run.sh (GOLDEN_JOBS=2) | verify.sh (CEREAL_JOBS=4) | san.sh (SAN_JOBS=4, GOLDEN_JOBS=5, SAN_PY_JOBS=5) | callgrind (1)
# plus globals.sh (mutable-global allowlist, instant) and attr_lists.sh (attribute names live only in src/c/attrs.def) as stage 0
# Writes /tmp/gate.<stage> and prints them in order.  Expect run/san "N passed, 0 failed"
# (N grows with the goldens), san "findings 0", verify dg 2 / c-c++-common 1 / cpp 0 differ (file counts: docs/STATUS.md "Numbers"; cpp is a baseline, not a target),
# callgrind ~3.776G (instructions, deterministic; it read 3.776G at v0.31.0 (3.775G and 3.776G on two gate runs), 3.907G at HEAD 2026-10-30, 3.896G before the C23 work; the 3.4% fall since 10-30 came in with the commits between, not checked which).
# Exit status: 1 when a stage printed a failure (checked at the end).
# Inner loop while iterating: `sh tests/run.sh` alone (GOLDEN_JOBS=n shards its golden loops).
cd "$(dirname "$0")/../.." || exit 1
O=/tmp/gate; rm -f $O.*
t() { n=$1; shift; s=$(date +%s); "$@"; echo "$n $(( $(date +%s) - s ))s" >>$O.time; }
t globals sh bench/tools/globals.sh 2>&1 | tail -3 >$O.0globals
t attrs sh bench/tools/attr_lists.sh 2>&1 | tail -3 >$O.0attrs
t run env GOLDEN_JOBS=2 nice -n 10 sh tests/run.sh 2>&1 | tail -1 >$O.1run &
t verify env CEREAL_JOBS=4 nice -n 10 bench/tools/verify.sh 2>&1 | tail -3 >$O.2verify &
t san env SAN_JOBS=4 GOLDEN_JOBS=5 SAN_PY_JOBS=5 nice -n 10 bench/tools/san.sh 2>&1 | tail -2 >$O.3san &
t callgrind nice -n 10 valgrind --tool=callgrind --callgrind-out-file=/dev/null ./cereal -fsyntax-only -std=c99 -w \
    -I/usr/include/python3.13 -I"$HOME/corpus/uvloop-0.22.1/vendor/libuv/include" \
    -I"$HOME/corpus/uvloop-0.22.1/uvloop" "$HOME/corpus/uvloop-0.22.1/uvloop/loop.c" 2>&1 |
    grep Collected >$O.4cg &
wait
for f in $O.*; do echo "== ${f#$O.}"; cat "$f"; done
# a stage that printed a failure must fail the gate (the exit code used to be the last `cat`'s)
bad=0
grep -q 'all allowed' $O.0globals || { echo "gate: globals not all allowed"; bad=1; }
grep -q 'attr lists: ok' $O.0attrs || { echo "gate: attribute name lists not ok"; bad=1; }
for s in 1run 3san; do
    grep -Eq '^[0-9]+ passed, 0 failed$' $O.$s || { echo "gate: $s did not end 'N passed, 0 failed'"; bad=1; }
done
grep -q 'findings 0$' $O.3san || { echo "gate: san findings not 0"; bad=1; }
grep -q 'Collected' $O.4cg || { echo "gate: callgrind printed no total"; bad=1; }
exit $bad
