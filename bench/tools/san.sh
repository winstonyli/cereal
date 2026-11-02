#!/bin/sh
# bench/tools/san.sh - build the tree with ASan+UBSan in ~/cereal-san (not
# /tmp, which a WSL upgrade wipes) and run the test suite
# with the sanitized binary and fail on any sanitizer report it logs.
# Expect "NNN passed, 0 failed" and "findings 0".  LEAKS=1 also reports leaks.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DST=${SAN_DIR:-$HOME/cereal-san}
mkdir -p "$DST"
rsync -a --delete --exclude build --exclude cereal --exclude .git \
    --exclude '*.o' --exclude '*.d' "$ROOT/" "$DST/"
cd "$DST"
nice -n 10 make -j${SAN_JOBS:-8} CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fsanitize-undefined-trap-on-error" >/dev/null
# Every sanitizer report of every cereal run in run.sh goes to a file, also where run.sh
# discards stderr, and the scan at the end fails on any file.  gcc's UBSan runtime has no
# log_path, so UBSan traps (-fsanitize-undefined-trap-on-error) and ASan logs the SIGILL
# (handle_sigill=1) with the frame of the failing line.  A seeded program built the same
# way proves the detector sees a report; its logs go to a separate directory.
LOGS=$(mktemp -d "${TMPDIR:-/tmp}/san-logs.XXXXXX")
mkdir "$LOGS/seed"
export ASAN_OPTIONS=detect_leaks=${LEAKS:-0}:handle_sigill=1:log_path=$LOGS/asan
nice -n 10 sh tests/run.sh 2>&1 | tail -3
echo 'int main(int c, char **v) { int a = 2147483647; (void)v; return a + c; }' > "$LOGS/seed.c"
cc -g -w -fsanitize=address,undefined -fsanitize-undefined-trap-on-error -o "$LOGS/seed.bin" "$LOGS/seed.c"
ASAN_OPTIONS=detect_leaks=0:handle_sigill=1:log_path=$LOGS/seed/asan "$LOGS/seed.bin" 2>/dev/null || true
if ! ls "$LOGS"/seed/* >/dev/null 2>&1; then
    echo "san.sh: the seeded report was not logged; the detector is blind (logs in $LOGS)"
    echo "findings blind"
    exit 1
fi
n=$(find "$LOGS" -maxdepth 1 -type f -name 'asan.*' | wc -l)
if [ "$n" != 0 ]; then
    for f in "$LOGS"/asan.*; do head -6 "$f"; done
    echo "reports kept in $LOGS"
else
    rm -rf "$LOGS"
fi
echo "sanitizer reports in run.sh, seed detected: findings $n"
