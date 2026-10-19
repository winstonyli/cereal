#!/bin/sh
# End to end: build Lua 5.4 from objects our back end wrote (--emit-obj), link
# it with gcc and compare a script's output with a gcc -O0 build.
# Usage: tests/lua_e2e.sh CEREAL LUA_SRC_DIR   (not part of run.sh: needs the corpus)
C=${1:?cereal binary}; LUA=${2:?lua 5.4 source dir}
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
FLAGS="-std=gnu11 -w -DLUA_COMPAT_5_3 -DLUA_USE_POSIX -DLUA_USE_DLOPEN -I$LUA"
mkdir "$W/ref"
cd "$LUA" || exit 1
for f in $(ls *.c | grep -v -E '^(luac|onelua|ltests)\.c$'); do
    b=${f%.c}
    "$C" check --target=x86_64-linux-gnu $FLAGS --emit-obj="$W/$b.o" "$f" > "$W/$b.log" 2>&1
    if grep -q unsupported "$W/$b.log" || [ ! -f "$W/$b.o" ]; then
        echo "FAIL $f: $(grep unsupported "$W/$b.log" | head -3)"; exit 1
    fi
    gcc -O0 -w $FLAGS -c "$f" -o "$W/ref/$b.o" || exit 1
done
gcc -o "$W/lua-ref" "$W"/ref/*.o -lm -ldl || exit 1
gcc -o "$W/lua-ours" "$W"/[a-z]*.o -lm -ldl || exit 1
"$W/lua-ref" "$HERE/lua_e2e.lua" > "$W/ref.out" 2>&1
"$W/lua-ours" "$HERE/lua_e2e.lua" > "$W/ours.out" 2>&1
if cmp -s "$W/ref.out" "$W/ours.out"; then echo "lua e2e: same output ($(wc -l < "$W/ref.out") lines)"; else diff "$W/ref.out" "$W/ours.out" | head; exit 1; fi
