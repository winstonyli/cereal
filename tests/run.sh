#!/bin/sh
# tests/run.sh - cereal test suite.
#   1. differential: `cereal -E` vs `$REFCC -std=c99 -E`, token by token
#   2. dogfood: cereal's own sources through both preprocessors
#   3. lint: `// expect: <id>` annotations checked by tests/verify.py
#   4. query: LSP index queries against expected output
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CEREAL=${CEREAL:-$ROOT/cereal}
REFCC=${REFCC:-gcc}
TMP=${TMPDIR:-/tmp}/cereal-test.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM
pass=0
fail=0

ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*"; }

diff_pp() { # diff_pp NAME FILE FLAGS...
    name=$1 file=$2
    shift 2
    $REFCC -std=c99 "$@" -E "$file" >"$TMP/ref.i" 2>/dev/null
    "$CEREAL" -E "$@" "$file" >"$TMP/out.i" 2>"$TMP/err" ||
        { bad "$name (cereal exited non-zero)"; sed 's/^/    /' "$TMP/err" | head -5; return; }
    if python3 "$ROOT/tests/tokdiff.py" "$TMP/ref.i" "$TMP/out.i" >"$TMP/diff"; then
        ok
    else
        bad "$name"
        sed 's/^/    /' "$TMP/diff"
    fi
}

if command -v "$REFCC" >/dev/null 2>&1; then
    for f in "$ROOT"/tests/pp/*.c; do
        n=$(basename "$f")
        case $n in
        err_*) continue ;;
        esac
        cd "$ROOT/tests/pp"
        diff_pp "pp/$n" "$n"
        diff_pp "pp/$n (gnu99)" "$n" -std=gnu99
        cd "$ROOT"
    done
    for h in assert.h ctype.h errno.h float.h inttypes.h limits.h locale.h \
        math.h setjmp.h signal.h stdarg.h stdbool.h stddef.h stdint.h \
        stdio.h stdlib.h string.h time.h wchar.h wctype.h complex.h fenv.h \
        iso646.h tgmath.h unistd.h pthread.h sys/socket.h sys/stat.h \
        fcntl.h netdb.h stdatomic.h; do
        echo "#include <$h>" >"$TMP/h.c"
        diff_pp "<$h>" "$TMP/h.c"
        diff_pp "<$h> -D_GNU_SOURCE -D_FORTIFY_SOURCE=2 -O2" "$TMP/h.c" \
            -D_GNU_SOURCE -D_FORTIFY_SOURCE=2 -O2
    done
    for f in "$ROOT"/src/*.c "$ROOT"/src/analysis/*.c; do
        [ -f "$f" ] || continue
        diff_pp "dogfood ${f#$ROOT/}" "$f" -D_POSIX_C_SOURCE=200809L -I"$ROOT/src"
    done
else
    echo "note: $REFCC not found, skipping differential tests"
fi

# error cases must be diagnosed (not crash)
for f in "$ROOT"/tests/pp/err_*.c; do
    [ -f "$f" ] || continue
    if python3 "$ROOT/tests/verify.py" "$CEREAL" -E "$f" >"$TMP/v" 2>&1; then
        ok
    else
        bad "pp/$(basename "$f")"
        sed 's/^/    /' "$TMP/v"
    fi
done

for f in "$ROOT"/tests/lint/*.c; do
    [ -f "$f" ] || continue
    if (cd "$ROOT/tests/lint" && python3 "$ROOT/tests/verify.py" "$CEREAL" lint "$(basename "$f")") >"$TMP/v" 2>&1; then
        ok
    else
        bad "lint/$(basename "$f")"
        sed 's/^/    /' "$TMP/v"
    fi
done

for t in "$ROOT"/tests/query/*.cmd; do
    [ -f "$t" ] || continue
    exp=${t%.cmd}.expected
    (cd "$ROOT/tests/query" && sh "$t" "$CEREAL") >"$TMP/q" 2>&1
    if diff -u "$exp" "$TMP/q" >"$TMP/qd"; then
        ok
    else
        bad "query/$(basename "$t")"
        sed 's/^/    /' "$TMP/qd" | head -40
    fi
done

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
