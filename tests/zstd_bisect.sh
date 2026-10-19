#!/bin/sh
# Which zstd object miscompiles: link the round-trip program with the gcc build of the
# library except for one object of ours at a time, and report the ones that fail.
# Usage: tests/zstd_bisect.sh CEREAL ZSTD_SRC_DIR WORKDIR [OBJECT...]
C=${1:?cereal binary}; Z=${2:?zstd source dir}; W=${3:?work dir}; shift 3
HERE=$(cd "$(dirname "$0")" && pwd)
FLAGS="-std=gnu11 -w -DZSTD_NO_INTRINSICS -DDYNAMIC_BMI2=0 -DZSTD_DISABLE_ASM=1 -DXXH_VECTOR=0 -I$Z/lib -I$Z/lib/common"
mkdir -p "$W/ref" "$W/our"
for f in "$Z"/lib/common/*.c "$Z"/lib/compress/*.c "$Z"/lib/decompress/*.c; do
    b=$(basename "$f" .c)
    case $b in zstdmt_compress) continue ;; esac
    [ -f "$W/ref/$b.o" ] || gcc -O0 $FLAGS -c "$f" -o "$W/ref/$b.o" || exit 1
    "$C" check --target=x86_64-linux-gnu $FLAGS --emit-obj="$W/our/$b.o" "$f" > "$W/$b.log" 2>&1
done
gcc -O0 -w -I"$Z/lib" -o "$W/rt-ref" "$HERE/zstd_e2e/rt.c" "$W"/ref/*.o || exit 1
"$W/rt-ref" > "$W/ref.out"
for o in ${@:-$(ls "$W/our" | sed 's/\.o$//')}; do
    mkdir -p "$W/mix"; rm -f "$W/mix"/*.o
    cp "$W"/ref/*.o "$W/mix/"; cp "$W/our/$o.o" "$W/mix/$o.o"
    gcc -o "$W/rt-mix" "$HERE/zstd_e2e/rt.c" -I"$Z/lib" "$W"/mix/*.o 2>/dev/null || { echo "$o: link failed"; continue; }
    if timeout 60 "$W/rt-mix" > "$W/mix.out" 2>/dev/null && cmp -s "$W/ref.out" "$W/mix.out"; then :; else echo "BAD $o"; fi
done
