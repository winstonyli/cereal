#!/bin/sh
# lib_oracle.sh CEREAL lua|zstd [N]: the evaluator against gcc, call by call, on a whole library linked as one
# program (--eval-link); gcc runs every call in a forked child, so a crash on generated state loses that call
# only (tests/eval_sweep.sh with EVAL_SWEEP_LIB=1).  Prints one summary line and the differing / crashing
# functions.  Run through the run wrapper (RUNS.md).  STATE=1 sweeps with --eval-state.
# Lua is built with luai_makeseed pinned to 0 (its string-hash seed comes from the clock and addresses, so every
# new state would differ); ZSTD_DCtx_get_bmi2 (it reads the CPU) still differs by design; address-dependent functions stop (an address used as an integer).
C=${1:?cereal binary}
LIB=${2:?lua or zstd}
N=${3:-24}
L=$HOME/corpus/lupa-2.8/third-party/lua54
Z=$HOME/corpus/zstd-1.5.7-dl/zstd-1.5.7/lib
S=$(cd "$(dirname "$0")/../../tests" && pwd)/eval_sweep.sh
case $LIB in
lua)  FILES=$(ls "$L"/*.c | grep -Ev '/(luac?|onelua|ltests).c$'); export CEREAL_FLAGS="-std=gnu99 -I$L -Dluai_makeseed(L)=0u" ;;
zstd) FILES=$(ls "$Z"/common/*.c "$Z"/compress/*.c "$Z"/decompress/*.c)
      export CEREAL_FLAGS="-std=gnu99 -DZSTD_DISABLE_ASM -I$Z -I$Z/common" ;;
*) echo "lib_oracle: lua or zstd" >&2; exit 2 ;;
esac
nf=$(echo "$FILES" | wc -w)
ulimit -S -v $((8000000 + 4400000 * nf))
export EVAL_SWEEP_LIB=1 EVAL_SWEEP_LINK=1 EVAL_SWEEP_N=$N
[ -n "$STATE" ] && export EVAL_SWEEP_STATE=1
# shellcheck disable=SC2086
timeout 7200 sh "$S" "$C" $FILES
