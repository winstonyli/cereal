#!/bin/sh
# End to end: build libzstd's compressor and decompressor from objects our back
# end wrote (--emit-obj), link tests/zstd_e2e/rt.c (gcc) against them, and compare
# the program's output (every compressed frame's size and checksum) with the same
# program linked against a gcc -O0 build of the library.
# Usage: tests/zstd_e2e.sh CEREAL ZSTD_SRC_DIR   (not part of run.sh: needs the corpus)
C=${1:?cereal binary}; Z=${2:?zstd source dir (containing lib/)}
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
# no SIMD intrinsics, no BMI2 dispatch (cpuid is supported, vector types are not), no assembly decoder
FLAGS="-std=gnu11 -w -DZSTD_NO_INTRINSICS -DDYNAMIC_BMI2=0 -DZSTD_DISABLE_ASM=1 -DXXH_VECTOR=0 -I$Z/lib -I$Z/lib/common"
mkdir "$W/ref" "$W/our"
for f in "$Z"/lib/common/*.c "$Z"/lib/compress/*.c "$Z"/lib/decompress/*.c; do
    case $f in */zstdmt_compress.c) continue ;; esac     # multithreading only: bit-fields are parked
    b=$(basename "$f" .c)
    "$C" check --target=x86_64-linux-gnu $FLAGS --emit-obj="$W/our/$b.o" "$f" > "$W/$b.log" 2>&1
    if grep -q unsupported "$W/$b.log" || [ ! -f "$W/our/$b.o" ]; then
        echo "FAIL $b: $(grep unsupported "$W/$b.log" | head -3)"; exit 1
    fi
    gcc -O0 $FLAGS -c "$f" -o "$W/ref/$b.o" || exit 1
done
for v in ref our; do
    gcc -O0 -w -I"$Z/lib" -o "$W/rt-$v" "$HERE/zstd_e2e/rt.c" "$W/$v"/*.o || exit 1
done
"$W/rt-ref" > "$W/ref.out" || { echo "gcc build fails its own round trip"; tail -3 "$W/ref.out"; exit 1; }
"$W/rt-our" > "$W/our.out"; rc=$?
if [ $rc = 0 ] && cmp -s "$W/ref.out" "$W/our.out"; then
    echo "zstd e2e: $(wc -l < "$W/our.out") lines identical to the gcc build"
else
    echo "zstd e2e: DIFF (exit $rc)"; diff "$W/ref.out" "$W/our.out" | head -5; exit 1
fi
