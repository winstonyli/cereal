#!/bin/sh
# census_zstd.sh CEREAL OUT [EXTRA...]: the zstd (common, compress, decompress; no assembly) link sweep of sweep_meas.sh (seed ${SEED:-1234}, 24 calls, the solver) with
# --eval-miss, stderr to OUT: its "stuck: N FUNCTION: WHY" lines say which functions sit behind each stop (PARKED.md, "Lazy blob budget").
# EXTRA is more cereal options, e.g. --eval-only=luaV_execute.  Run through the run wrapper (RUNS.md).
C=${1:?cereal binary}
O=${2:?output file}
shift 2
PATH=$HOME/tools/z3:$PATH
Z=$HOME/corpus/zstd-1.5.7-dl/zstd-1.5.7/lib
command -v z3 >/dev/null || { echo "census_zstd: z3 not found (looked in $HOME/tools/z3)" >&2; exit 2; }
SRC=$(ls "$Z"/common/*.c "$Z"/compress/*.c "$Z"/decompress/*.c)
ulimit -S -v $((8000000 + 4400000 * $(echo $SRC | wc -w)))
"$C" check --target=x86_64-linux-gnu -std=gnu99 -DZSTD_DISABLE_ASM -I"$Z" -I"$Z/common" --eval-sweep="24,${SEED:-1234}" --eval-solve="z3 -in -smt2" \
    --eval-link --eval-miss "$@" $SRC >/dev/null 2>"$O" </dev/null
rc=$?
echo "exit $rc; stuck lines: $(grep -ac '^cereal: stuck:' "$O")"
exit $rc
