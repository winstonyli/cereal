#!/bin/sh
# census_union.sh CEREAL OUTDIR SEED...: census_lua.sh once per seed, all at once (one core and one z3 each), stderr to
# OUTDIR/seed<SEED>.err, then edge_union.sh over them.  Run through the run wrapper (RUNS.md); about 40 min for three seeds.
# Fails (rc 1) if any census failed.
C=${1:?cereal binary}
D=${2:?output dir}
shift 2
[ $# -ge 1 ] || { echo "census_union: no seeds" >&2; exit 2; }
H=$(dirname "$0")
mkdir -p "$D"
S="$*"
pids=
for s in $S; do
    SEED=$s sh "$H/census_lua.sh" "$C" "$D/seed$s.err" &
    pids="$pids $!"
done
bad=0
for p in $pids; do wait "$p" || bad=1; done
set --
for s in $S; do set -- "$@" "$D/seed$s.err"; done
sh "$H/edge_union.sh" "$@"
exit $bad
