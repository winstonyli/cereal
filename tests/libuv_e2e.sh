#!/bin/sh
# End to end: build libuv's Linux sources from objects our back end wrote
# (--emit-obj), link libuv's own test runner against them with gcc, and compare
# each test's result with the same tests run against a gcc -O0 build.
# Usage: tests/libuv_e2e.sh CEREAL LIBUV_SRC_DIR [TEST_REGEX]   (not part of run.sh: needs the corpus)
C=${1:?cereal binary}; UV=${2:?libuv source dir}; PAT=${3:-.}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
FLAGS="-std=gnu11 -w -D_GNU_SOURCE -I$UV/include -I$UV/src -I$UV/src/unix"
SRCS="fs-poll idna inet random strscpy thread-common threadpool timer uv-data-getter-setters uv-common version strtok
unix/async unix/core unix/dl unix/fs unix/getaddrinfo unix/getnameinfo unix/loop-watcher unix/loop unix/pipe
unix/poll unix/process unix/random-devurandom unix/signal unix/stream unix/tcp unix/thread unix/tty unix/udp
unix/linux unix/procfs-exepath unix/proctitle unix/random-getrandom unix/random-sysctl-linux"
mkdir "$W/ref" "$W/our" "$W/t"
for s in $SRCS; do
    b=$(echo "$s" | tr / _)
    "$C" check --target=x86_64-linux-gnu $FLAGS --emit-obj="$W/our/$b.o" "$UV/src/$s.c" > "$W/$b.log" 2>&1
    if grep -q unsupported "$W/$b.log" || [ ! -f "$W/our/$b.o" ]; then
        echo "FAIL $s: $(grep unsupported "$W/$b.log" | head -3)"; exit 1
    fi
    gcc -O0 $FLAGS -c "$UV/src/$s.c" -o "$W/ref/$b.o" || exit 1
done
# the test runner and the tests, compiled once by gcc and linked with each build
for f in "$UV"/test/*.c; do
    case $f in */benchmark-*|*/run-benchmarks.c|*/runner-win.c|*/test-*win*.c) continue ;; esac
    gcc -O0 -w -std=gnu11 -D_GNU_SOURCE -I"$UV/include" -I"$UV/src" -I"$UV/test" -c "$f" -o "$W/t/$(basename "$f" .c).o" || exit 1
done
for v in ref our; do      # an archive, as libuv's own build links: tests that include a source file keep their copy
    ar rcs "$W/libuv-$v.a" "$W/$v"/*.o || exit 1
    gcc -o "$W/run-$v" "$W"/t/*.o "$W/libuv-$v.a" -lpthread -ldl -lrt -lm || exit 1
done
cd "$UV" || exit 1
"$W/run-ref" --list 2>/dev/null | grep -E "$PAT" | grep -v -E '^tty' > "$W/list"     # tty tests wait for a terminal
n=0; bad=0; pass=0
for t in $(cat "$W/list"); do
    r=$(timeout -s KILL 30 "$W/run-ref" "$t" >/dev/null 2>&1; echo $?)
    o=$(timeout -s KILL 30 "$W/run-our" "$t" >/dev/null 2>&1; echo $?)
    n=$((n+1)); [ "$r" = 0 ] && [ "$o" = 0 ] && pass=$((pass+1))
    if [ "$r" != "$o" ]; then echo "DIFF $t: gcc exit $r, ours exit $o"; bad=$((bad+1)); fi
done
echo "libuv e2e: $n tests, $pass pass with both builds, $bad differ from the gcc build"
[ "$bad" = 0 ]
