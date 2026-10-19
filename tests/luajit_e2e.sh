#!/bin/sh
# End to end: build LuaJIT 2.1 with its own makefile (gcc: the generated headers, the
# assembler VM lj_vm.o and the reference binary), replace every C object of the library
# and the launcher with one our back end wrote (--emit-obj), link with gcc and compare a
# script's output with the gcc build's.  The interpreter loop (lj_vm.o) stays gcc's.
# Usage: tests/luajit_e2e.sh CEREAL LUAJIT_SRC_DIR   (not part of run.sh: needs the corpus)
C=${1:?cereal binary}; LJ=${2:?luajit source dir (containing src/)}
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
mkdir "$W/lj" && cp -r "$LJ/." "$W/lj" || exit 1
cd "$W/lj/src" || exit 1
make -j8 > "$W/make.log" 2>&1 || { echo "gcc build of luajit failed"; tail -5 "$W/make.log"; exit 1; }
FLAGS="-std=gnu11 -w -D_GNU_SOURCE -DLUAJIT_TARGET=LUAJIT_ARCH_x64 -DLJ_ARCH_HASFPU=1 -DLJ_ABI_SOFTFP=0 -DLJ_ABI_SHADOW_STACK=1 -I."
mkdir "$W/our"
for f in $(ar t libluajit.a | grep -v '^lj_vm\.o$') luajit.o; do
    b=${f%.o}
    "$C" check --target=x86_64-linux-gnu $FLAGS --emit-obj="$W/our/$b.o" "$b.c" > "$W/$b.log" 2>&1
    if grep -q unsupported "$W/$b.log" || [ ! -f "$W/our/$b.o" ]; then
        echo "FAIL $b.c: $(grep unsupported "$W/$b.log" | head -3)"; exit 1
    fi
done
gcc -o "$W/luajit-ours" "$W"/our/*.o lj_vm.o -lm -ldl -Wl,-E || exit 1
./luajit "$HERE/luajit_e2e.lua" > "$W/ref.out" 2>&1
"$W/luajit-ours" "$HERE/luajit_e2e.lua" > "$W/ours.out" 2>&1
if ! cmp -s "$W/ref.out" "$W/ours.out"; then diff "$W/ref.out" "$W/ours.out" | head; exit 1; fi
tr=$("$W/luajit-ours" -jv "$HERE/luajit_e2e.lua" 2>&1 | grep -c '^\[TRACE')   # the trace compiler ran in our objects
[ "$tr" -gt 0 ] || { echo "luajit e2e: no traces compiled"; exit 1; }
echo "luajit e2e: same output ($(wc -l < "$W/ref.out") lines), $tr traces"
