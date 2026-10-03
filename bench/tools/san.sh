#!/bin/sh
# bench/tools/san.sh - build the tree with ASan+UBSan in ~/cereal-san (not
# /tmp, which a WSL upgrade wipes) and run the test suite plus the checker
# over tests/check and tests/parse with the sanitized binary.
# Expect "NNN passed, 0 failed" and "findings 0".  LEAKS=1 also reports leaks.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DST=${SAN_DIR:-$HOME/cereal-san}
mkdir -p "$DST"
rsync -a --delete --exclude build --exclude cereal --exclude .git \
    --exclude '*.o' --exclude '*.d' "$ROOT/" "$DST/"
cd "$DST"
nice -n 10 make -j8 CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" >/dev/null
export ASAN_OPTIONS=detect_leaks=${LEAKS:-0} UBSAN_OPTIONS=print_stacktrace=1
nice -n 10 sh tests/run.sh 2>&1 | tail -3
nice -n 10 python3 bench/tools/san.py "$DST/cereal" tests
