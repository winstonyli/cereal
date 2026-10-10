#!/bin/sh
# deps_case.sh PROG NAME: run tests/deps/NAME.cmd with PROG (cereal, or a gcc;
# word-split, so it may carry options) in a scratch copy of tests/deps, and
# print what a test compares: stdout, every file the command created (except
# *.i), and the exit status.  Used by tests/run.sh (section 17) and
# bench/tools/deps.sh.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
prog=$1 name=$2
w=$(mktemp -d "${TMPDIR:-/tmp}/deps-case.XXXXXX") || exit 9
trap 'rm -rf "$w"' EXIT INT TERM
mkdir "$w/t"
cp -r "$ROOT/tests/deps/." "$w/t/"
cd "$w/t" || exit 9
export LC_ALL=C
find . -type f | sort >"$w/before"
# shellcheck disable=SC2046,SC2086
$prog $(cat "$ROOT/tests/deps/$name.cmd") >"$w/out" 2>/dev/null </dev/null
rc=$?
cat "$w/out"
find . -type f | sort | comm -13 "$w/before" - | grep -v '\.i$' |
    while IFS= read -r f; do
        echo "== $f"
        cat "$f"
    done
echo "exit $rc"
