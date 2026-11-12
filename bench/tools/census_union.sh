#!/bin/sh
# census_union.sh CEREAL OUTDIR SEED...: census_lua.sh once per seed, all at once (one core and one z3 each), stderr to
# OUTDIR/seed<SEED>.err, then edge_union.sh over them.  Run through the run wrapper (RUNS.md); about 40 min for three seeds.
C=${1:?cereal binary}
D=${2:?output dir}
shift 2
[ $# -ge 1 ] || { echo "census_union: no seeds" >&2; exit 2; }
H=$(dirname "$0")
mkdir -p "$D"
for s in "$@"; do SEED=$s sh "$H/census_lua.sh" "$C" "$D/seed$s.err" & done
wait
set --  $(for f in "$D"/seed*.err; do echo "$f"; done)
sh "$H/edge_union.sh" "$@"
