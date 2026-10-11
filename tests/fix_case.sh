#!/bin/sh
# fix_case.sh PROG NAME [JOBS]: run tests/fix/NAME.sh (a script using
# tests/fix/lib.sh) in a scratch directory with PROG as cereal and -j JOBS,
# and print what it prints: for each `run`, the exit status, stdout, stderr
# and the changes to the files.  Used by tests/run.sh (section 21) and
# bench/tools/fixgate.sh.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
prog=$1 name=$2 jobs=${3:-4}
W=$(mktemp -d "${TMPDIR:-/tmp}/fix-case.XXXXXX") || exit 9
trap 'chmod -R u+rwx "$W" 2>/dev/null; rm -rf "$W"' EXIT INT TERM
mkdir "$W/t" "$W/snap"
export LC_ALL=C W CEREAL=$prog J=$jobs TESTS=$ROOT/tests
cd "$W" || exit 9
sh "$ROOT/tests/fix/$name.sh" </dev/null
