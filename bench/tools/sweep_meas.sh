#!/bin/sh
# sweep_meas.sh CEREAL N [SOLVER]: edge coverage of `--eval-sweep=N,1234` over the Lua 5.4 and zstd
# libraries under ~/corpus (the generator's coverage is the "taken before" figure, the solver adds the
# rest).  Run through the run wrapper (RUNS.md); prints one headline line.  SOLVER defaults to
# 'z3 -in -smt2' (z3 must be on PATH or in ~/tools/z3).  Each file runs under 8 GB of address space.
C=${1:?cereal binary}
N=${2:-24}
S=${3:-z3 -in -smt2}
PATH=$HOME/tools/z3:$PATH
L=$HOME/corpus/lupa-2.8/third-party/lua54
Z=$HOME/corpus/zstd-1.5.7-dl/zstd-1.5.7/lib
W=$(mktemp -d) || exit 1
trap 'rm -rf "$W"' EXIT
ulimit -v 8000000
nf=0; nbad=0; t0=$(date +%s)
for f in $(ls "$L"/*.c | grep -v '/luac\?\.c$') "$Z"/common/*.c "$Z"/compress/*.c "$Z"/decompress/*.c; do
    nf=$((nf + 1))
    timeout 900 "$C" check --target=x86_64-linux-gnu -std=gnu99 -I"$L" -I"$Z" -I"$Z/common" \
        --eval-sweep="$N,1234" --eval-solve="$S" "$f" >/dev/null 2>"$W/err" </dev/null || nbad=$((nbad + 1))
    grep -a '^cereal: solve:' "$W/err" >>"$W/stats"
done
echo "files: $nf, failed: $nbad, seconds: $(($(date +%s) - t0))"
awk '{
    if (match($0, /edges taken [0-9]+ -> [0-9]+ of [0-9]+/)) {
        split(substr($0, RSTART, RLENGTH), a, " ")
        b += a[3]; c += a[5]; d += a[7]
    }
    if (match($0, /: [0-9]+ queries/)) q += substr($0, RSTART + 2) + 0
}
END { printf "queries %d, edges %d -> %d of %d (%.1f%% -> %.1f%%)\n", q, b, c, d, 100 * b / d, 100 * c / d }' "$W/stats"
