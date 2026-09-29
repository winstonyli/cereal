#!/bin/sh
# tests/run.sh - cereal test suite.
#   1. differential: `cereal -E` vs `$REFCC -std=c99 -E`, token by token
#   2. dogfood: cereal's own sources through both preprocessors
#   3. lint: `// expect: <id>` annotations checked by tests/verify.py
#   4. query: LSP index queries against expected output
#   5. parallel: `-fparallel=on` byte-identical to sequential for -E, lint
#      and index (output, diagnostics, exit status) at adversarial chunk
#      sizes, plus a fuzzer; the cell cache: random edit sequences, each
#      build byte-identical to a full sequential one (tests/fuzz_cells.py)
#   6. gcc fuzz: random programs vs $REFCC (tests/fuzz_gcc.py, fixed seed)
#   7. -j: many translation units at once print exactly what -j1 prints
#   8. macro graph: every expansion lies in its invocation's static closure
#      (`cereal index --check-graph`), on all inputs and a fuzzer
#   9. lsp: scripted language-server sessions against golden transcripts
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

diff_pp() { # diff_pp NAME FILE FLAGS...  (ALLOW_ERRORS=1: erroneous input)
    name=$1 file=$2
    shift 2
    $REFCC -std=c99 "$@" -E "$file" >"$TMP/ref.i" 2>/dev/null
    if ! "$CEREAL" -E -fcheck-macro-versions "$@" "$file" >"$TMP/out.i" 2>"$TMP/err" &&
        [ "${ALLOW_ERRORS:-0}" = 0 ]; then
        bad "$name (cereal exited non-zero)"; sed 's/^/    /' "$TMP/err" | head -5; return
    fi
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
        cd "$ROOT/tests/pp"
        case $n in
        err_*) ALLOW_ERRORS=1 diff_pp "pp/$n (recovery)" "$n" ;;
        *)
            diff_pp "pp/$n" "$n"
            diff_pp "pp/$n (gnu99)" "$n" -std=gnu99
            ;;
        esac
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
    for f in "$ROOT"/src/*.c "$ROOT"/src/analysis/*.c "$ROOT"/src/lsp/*.c; do
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

par_same() { # par_same NAME FILE FLAGS...  (MODE: -E, lint or index)
    name=$1 file=$2
    shift 2
    mode=${MODE:--E}
    "$CEREAL" $mode -fparallel=off "$@" "$file" >"$TMP/seq.i" 2>"$TMP/seq.e"
    rs=$?
    for cfg in 1:1:0 3:1:0 3:1:2 4:64:0; do # threads:chunk:window
        t=${cfg%%:*} c=${cfg#*:} w=${cfg##*:}
        "$CEREAL" $mode -fparallel=on -fparallel-threads=$t \
            -fparallel-chunk=${c%:*} -fparallel-window=$w "$@" "$file" \
            >"$TMP/par.i" 2>"$TMP/par.e"
        rp=$?
        if [ $rs != $rp ] || ! cmp -s "$TMP/seq.i" "$TMP/par.i" ||
            ! cmp -s "$TMP/seq.e" "$TMP/par.e"; then
            bad "parallel $mode $name (threads:chunk:window $cfg)"
            diff "$TMP/seq.i" "$TMP/par.i" | head -10 | sed 's/^/    /'
            diff "$TMP/seq.e" "$TMP/par.e" | head -10 | sed 's/^/    /'
            return
        fi
    done
    ok
}

for f in "$ROOT"/tests/pp/*.c "$ROOT"/tests/lint/*.c; do
    cd "$(dirname "$f")"
    par_same "${f#$ROOT/tests/}" "$(basename "$f")"
    MODE=lint par_same "${f#$ROOT/tests/}" "$(basename "$f")" -Weverything
    MODE=index par_same "${f#$ROOT/tests/}" "$(basename "$f")" --all
    cd "$ROOT"
done
for f in "$ROOT"/src/*.c; do
    par_same "dogfood ${f#$ROOT/}" "$f" -D_POSIX_C_SOURCE=200809L -I"$ROOT/src"
done
for h in stdio.h stdlib.h pthread.h; do
    echo "#include <$h>" >"$TMP/h.c"
    par_same "<$h>" "$TMP/h.c" -D_GNU_SOURCE -O2
done
if command -v "$REFCC" >/dev/null 2>&1; then
    if python3 "$ROOT/tests/fuzz_gcc.py" "$CEREAL" "${FUZZ_GCC_N:-40}" "${FUZZ_SEED:-1}" >"$TMP/fg" 2>&1; then
        ok
    else
        bad "gcc fuzz (reduced cases kept)"
        sed 's/^/    /' "$TMP/fg" | tail -10
    fi
fi
if python3 "$ROOT/tests/fuzz_par.py" "$CEREAL" "${FUZZ_N:-40}" "${FUZZ_SEED:-1}" >"$TMP/fz" 2>&1; then
    ok
else
    bad "parallel fuzz"
    sed 's/^/    /' "$TMP/fz" | tail -10
fi
if python3 "$ROOT/tests/fuzz_cells.py" "$CEREAL" "${FUZZ_N:-40}" "${FUZZ_SEED:-1}" >"$TMP/fc" 2>&1; then
    ok
else
    bad "cell cache fuzz (random edits vs full rebuilds)"
    sed 's/^/    /' "$TMP/fc" | tail -10
fi

jobs_same() { # jobs_same NAME DIR ARGS...
    name=$1 dir=$2
    shift 2
    (cd "$dir" && "$CEREAL" "$@" -j1 >"$TMP/j1.o" 2>"$TMP/j1.e"; echo $? >"$TMP/j1.r")
    (cd "$dir" && "$CEREAL" "$@" -j8 >"$TMP/j8.o" 2>"$TMP/j8.e"; echo $? >"$TMP/j8.r")
    if cmp -s "$TMP/j1.o" "$TMP/j8.o" && cmp -s "$TMP/j1.e" "$TMP/j8.e" &&
        cmp -s "$TMP/j1.r" "$TMP/j8.r"; then
        ok
    else
        bad "-j $name"
        diff "$TMP/j1.e" "$TMP/j8.e" | head -5 | sed 's/^/    /'
    fi
}
jobs_same "lint tests/lint" "$ROOT/tests/lint" lint ./*.c
jobs_same "-E tests/pp" "$ROOT/tests/pp" -E -fparallel=on -fparallel-chunk=64 ./*.c
jobs_same "-E dogfood" "$ROOT" -E -Isrc -D_POSIX_C_SOURCE=200809L src/*.c src/analysis/*.c src/lsp/*.c

graph_ok() { # graph_ok NAME DIR ARGS...
    name=$1 dir=$2
    shift 2
    # erroneous inputs exit 1 but must still report a clean graph
    (cd "$dir" && "$CEREAL" index --check-graph "$@") >"$TMP/g" 2>/dev/null
    if grep -q ' 0 outside their closure$' "$TMP/g"; then
        ok
    else
        bad "graph $name"
        grep '^graph:' "$TMP/g" | head -5 | sed 's/^/    /'
    fi
}
for f in "$ROOT"/tests/pp/*.c "$ROOT"/tests/lint/*.c; do
    graph_ok "${f#$ROOT/tests/}" "$(dirname "$f")" "$(basename "$f")"
done
for f in "$ROOT"/src/*.c "$ROOT"/src/analysis/*.c "$ROOT"/src/lsp/*.c; do
    graph_ok "dogfood ${f#$ROOT/}" "$ROOT" -Isrc -D_POSIX_C_SOURCE=200809L "$f"
done
if python3 "$ROOT/tests/fuzz_graph.py" "$CEREAL" "${FUZZ_N:-40}" "${FUZZ_SEED:-1}" >"$TMP/fz" 2>&1; then
    ok
else
    bad "graph fuzz (reduced cases kept)"
    sed 's/^/    /' "$TMP/fz" | tail -5
fi

for t in "$ROOT"/tests/lsp/*.json; do
    [ -f "$t" ] || continue
    if python3 "$ROOT/tests/lsp_session.py" "$CEREAL" "$t" >"$TMP/l" 2>&1; then
        ok
    else
        bad "lsp/$(basename "$t")"
        head -40 "$TMP/l" | sed 's/^/    /'
    fi
done

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
