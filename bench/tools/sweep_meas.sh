#!/bin/sh
# sweep_meas.sh CEREAL N [SOLVER]: edge coverage of `--eval-sweep=N,1234` over the Lua 5.4 and zstd
# libraries under ~/corpus (the generator's coverage is the "taken before" figure, the solver adds the
# rest; onelua.c and ltests.c are left out, zstd is built without its assembly).  Run through the run wrapper (RUNS.md); prints one headline line.  SOLVER defaults to
# 'z3 -in -smt2' (z3 must be on PATH or in ~/tools/z3).  Each run is under 8 GB of address space.  LINK=1 sweeps
# each library as one program (--eval-link, docs/MULTITU_DESIGN.md) instead of file by file.
# STATE=--eval-state also builds state with constructors (docs/CONCOLIC_DESIGN.md).
# LAZY=--eval-lazy makes pointer targets on access (docs/LAZY_BLOBS_DESIGN.md).
C=${1:?cereal binary}
N=${2:-24}
S=${3:-z3 -in -smt2}
PATH=$HOME/tools/z3:$PATH
L=$HOME/corpus/lupa-2.8/third-party/lua54
Z=$HOME/corpus/zstd-1.5.7-dl/zstd-1.5.7/lib
W=$(mktemp -d) || exit 1
trap 'rm -rf "$W"' EXIT
ulimit -S -v 8000000      # a link run raises it by 4.4 GB per file (the source managers' reservations)
nf=0; nbad=0; t0=$(date +%s)
LUA=$(ls "$L"/*.c | grep -Ev '/(luac?|onelua|ltests).c$')
ZSTD=$(ls "$Z"/common/*.c "$Z"/compress/*.c "$Z"/decompress/*.c)
# each file on its own; LINK=1: the files of each library as one program (--eval-link)
run() {
    nf=$((nf + $#))
    # a source manager reserves 4 GB of address space per file and a link keeps them all
    [ -n "$LINK" ] && ulimit -S -v $((8000000 + 4400000 * $#))
    timeout 3600 "$C" check --target=x86_64-linux-gnu -std=gnu99 -DZSTD_DISABLE_ASM -I"$L" -I"$Z" -I"$Z/common" \
        --eval-sweep="$N,1234" --eval-solve="$S" $LINK $STATE $LAZY "$@" >/dev/null 2>"$W/err" </dev/null || nbad=$((nbad + 1))
    grep -a '^cereal: solve:' "$W/err" >>"$W/stats"
    grep -a -e '^cereal: untaken' -e '^cereal: never' -e '^cereal: state' "$W/err" >>"$W/miss"
}
if [ -n "$LINK" ]; then
    LINK=--eval-link
    run $LUA
    run $ZSTD
else
    for f in $LUA $ZSTD; do run "$f"; done
fi
echo "files: $nf, failed: $nbad, seconds: $(($(date +%s) - t0))"
awk '{
    if (match($0, /edges taken [0-9]+ of [0-9]+, [0-9]+ of them/)) {
        split(substr($0, RSTART, RLENGTH), a, " ")
        t += a[3]; d += a[5]; g += a[6]
    }
    if (match($0, /: [0-9]+ queries/)) q += substr($0, RSTART + 2) + 0
}
END { printf "queries %d, edges taken %d of %d (%.1f%%), %d of them first by a solver round\n", q, t, d, 100 * t / d, g }' "$W/stats"
cat "$W/miss" 2>/dev/null
