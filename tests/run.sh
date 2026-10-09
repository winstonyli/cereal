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
#  10. parser: syntax trees and diagnostics against goldens, directly and
#      from tokens regenerated from cells; cereal's own sources parse
#  11. checker: `cereal -fsyntax-only -std=c99 -pedantic` diagnostics (gcc's
#      text) against goldens, directly and from cells; a `// flags: ...`
#      line in a case adds flags (e.g. -Wall, --target=i386, --dump-types);
#      layout parity: random records with $REFCC's sizeof/_Alignof/offsetof
#      as _Static_asserts (tests/gen_layout.py) must check clean
#  12. summaries  13. build options (see their sections)
#  14. C symbol index: `--dump-symbols` goldens, `--verify-symbols` over the
#      checker and parser cases and cereal's own sources
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
    cd "$ROOT/tests/ppstd"
    for std in c99 gnu99 c11 gnu11 c17 gnu17 c18 iso9899:2011 gnu1x; do
        diff_pp "pp/std_macros.c (-std=$std)" std_macros.c -std=$std
    done
    for std in gnu99 gnu11 gnu17; do
        diff_pp "pp/elifdef.c (-std=$std)" elifdef.c -std=$std
    done
    cd "$ROOT"
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
    PEDX=; case "${f##*/}" in err_recovery.c) PEDX=-pedantic;; esac
    if python3 "$ROOT/tests/verify.py" "$CEREAL" -E $PEDX "$f" >"$TMP/v" 2>&1; then
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
    # as is, then with the parallel path and cells forced: same transcript
    for flags in "" "-fparallel=on -fparallel-chunk=1 -fparallel-threads=3"; do
        if python3 "$ROOT/tests/lsp_session.py" "$CEREAL" "$t" \
            ${flags:+--flags "$flags"} >"$TMP/l" 2>&1; then
            ok
        else
            bad "lsp/$(basename "$t") ${flags:-(default)}"
            head -40 "$TMP/l" | sed 's/^/    /'
        fi
    done
done

parse_golden() { # parse_golden NAME FLAGS...  (in tests/parse)
    name=$1
    shift
    "$CEREAL" parse --dump "$@" "$name.c" >"$TMP/po" 2>"$TMP/pe"
    cat "$TMP/pe" "$TMP/po" >"$TMP/p"
    if cmp -s "$TMP/p" "$name.expected"; then
        ok
    else
        bad "parse/$name.c $*"
        diff "$name.expected" "$TMP/p" | head -20 | sed 's/^/    /'
    fi
}
# par_goldens FUNC: run FUNC NAME and FUNC NAME <cells flags> over *.c in the
# current directory, sharded over $GOLDEN_JOBS subshells (each with its own
# scratch dir); their output and counts are merged in shard order.
par_goldens() {
    pg_n=${GOLDEN_JOBS:-4}
    pg_out=$TMP
    pg_k=0
    while [ "$pg_k" -lt "$pg_n" ]; do
        (
            TMP=$pg_out/s$pg_k
            mkdir -p "$TMP"
            pass=0 fail=0 i=0
            for f in *.c; do
                [ -f "$f" ] || continue
                i=$((i + 1))
                [ $((i % pg_n)) -eq "$pg_k" ] || continue
                "$1" "${f%.c}"
                "$1" "${f%.c}" --cells -fparallel-chunk=1 -fparallel-threads=2
            done
            echo "$pass $fail" >"$pg_out/r$pg_k"
        ) >"$pg_out/o$pg_k" 2>&1 &
        pg_k=$((pg_k + 1))
    done
    wait
    pg_k=0
    while [ "$pg_k" -lt "$pg_n" ]; do
        cat "$pg_out/o$pg_k"
        read -r pg_p pg_f <"$pg_out/r$pg_k"
        pass=$((pass + pg_p))
        fail=$((fail + pg_f))
        pg_k=$((pg_k + 1))
    done
}

cd "$ROOT/tests/parse"
par_goldens parse_golden
cd "$ROOT"
for f in "$ROOT"/src/*.c "$ROOT"/src/analysis/*.c "$ROOT"/src/lsp/*.c \
    "$ROOT"/src/c/*.c; do
    if "$CEREAL" parse --dump -Isrc -D_POSIX_C_SOURCE=200809L "$f" \
        >"$TMP/d1" 2>"$TMP/e1" &&
        "$CEREAL" parse --dump --cells -fparallel-chunk=256 -Isrc \
            -D_POSIX_C_SOURCE=200809L "$f" >"$TMP/d2" 2>&1 &&
        cmp -s "$TMP/d1" "$TMP/d2"; then
        ok
    else
        bad "parse dogfood ${f#$ROOT/}"
        head -5 "$TMP/e1" | sed 's/^/    /'
    fi
done

check_golden() { # check_golden NAME FLAGS...  (in tests/check)
    name=$1
    shift
    extra=$(sed -n 's|^// flags: *||p' "$name.c" | head -1)
    # shellcheck disable=SC2086
    "$CEREAL" -fsyntax-only -std=c99 -pedantic $extra "$@" "$name.c" \
        >"$TMP/co" 2>"$TMP/ce"
    cat "$TMP/ce" "$TMP/co" >"$TMP/c"
    if cmp -s "$TMP/c" "$name.expected"; then
        ok
    else
        bad "check/$name.c $extra $*"
        diff "$name.expected" "$TMP/c" | head -20 | sed 's/^/    /'
    fi
}
if [ -d "$ROOT/tests/check" ]; then
    cd "$ROOT/tests/check"
    par_goldens check_golden
    cd "$ROOT"
fi

layout_ok() { # layout_ok SEED TARGET CC
    if ! python3 "$ROOT/tests/gen_layout.py" --seed "$1" --count 40 \
        --target "$2" --cc "$3" --out "$TMP/lay.c" >"$TMP/le" 2>&1; then
        bad "layout generator seed $1 $2"
        tail -5 "$TMP/le" | sed 's/^/    /'
        return
    fi
    if "$CEREAL" -fsyntax-only -std=c99 --target="$2" "$TMP/lay.c" \
        >"$TMP/lo" 2>&1 && [ ! -s "$TMP/lo" ]; then
        ok
    else
        cp "$TMP/lay.c" "${TMPDIR:-/tmp}/cereal-layout-$2-$1.c"
        bad "layout parity seed $1 $2 (kept ${TMPDIR:-/tmp}/cereal-layout-$2-$1.c)"
        head -10 "$TMP/lo" | sed 's/^/    /'
    fi
}
if command -v "$REFCC" >/dev/null 2>&1; then
    seed=1
    while [ $seed -le "${LAYOUT_N:-4}" ]; do
        layout_ok $seed x86_64 "$REFCC"
        seed=$((seed + 1))
    done
    if echo 'int x;' | $REFCC -m32 -S -o /dev/null -x c - 2>/dev/null; then
        layout_ok 1 i386 "$REFCC -m32"
        layout_ok 2 i386 "$REFCC -m32"
    fi
fi

# 12. summaries: golden dumps of per-unit summaries and read sets, and the
#     relational checks (context independence, body edits, validity)
if [ -d "$ROOT/tests/summary" ]; then
    for f in "$ROOT"/tests/summary/*.c; do
        n=${f%.c}
        for extra in "" "--cells -fparallel-chunk=1 -fparallel-threads=2"; do
            # shellcheck disable=SC2086
            "$CEREAL" check --target=x86_64-linux-gnu --dump-summaries $extra \
                "$f" >"$TMP/sum.out" 2>/dev/null
            if cmp -s "$TMP/sum.out" "$n.expected"; then
                ok
            else
                bad "summary/$(basename "$n").c $extra"
                diff "$n.expected" "$TMP/sum.out" | head -10 | sed 's/^/    /'
            fi
        done
    done
    if python3 "$ROOT/tests/summary.py" "$CEREAL" "$TMP" >"$TMP/sum.rel" 2>&1; then
        ok
    else
        bad "summary relational checks"
        sed 's/^/    /' "$TMP/sum.rel" | head -20
    fi
fi

# 13. build options: the ones cereal skips (driver.c ignored_opts) leave the
#     diagnostics of an erroneous file and the exit status exactly as without
#     them, and so do they for $REFCC
printf 'int x = ;
int f(void) { return y; }
' >"$TMP/bo.c"
"$CEREAL" check -std=c99 "$TMP/bo.c" >"$TMP/bo.base" 2>&1
for f in -c -S -pipe -pthread -shared -static -pie -fPIC -fPIE -fno-plt -g -ggdb3     -fvisibility=hidden -fstack-protector-strong -march=native -mtune=generic     -Wl,-z,now -Wa,--noexecstack -lm "-l m" -L. "-L ." "-x c" -xc "-MD" "-MMD -MP"     "-MF $TMP/x.d" -MFx.d "-MT a" "-MQ a" -Xlinker\ -v -m64 -fsigned-char; do
    # shellcheck disable=SC2086
    "$CEREAL" check -std=c99 $f "$TMP/bo.c" 2>&1 | grep -v '^cereal: note:' >"$TMP/bo.out"
    if cmp -s "$TMP/bo.base" "$TMP/bo.out"; then ok; else bad "option $f changes the output"; fi
done
for f in -ffreestanding -fwrapv -fno-common -m32; do
    "$CEREAL" check -std=c99 $f "$TMP/bo.c" 2>&1 | grep -q "note: '$f' is ignored" &&
        ok || bad "option $f is not reported"
done
for f in "-x c++" "-x c-header"; do
    # shellcheck disable=SC2086
    "$CEREAL" check -std=c99 $f "$TMP/bo.c" >/dev/null 2>&1 && bad "option $f accepted" || ok
done

# 14. C symbol index: golden `--dump-symbols` (plain and from cells), and
#     `--verify-symbols` (every resolved identifier has its event, the index
#     is well formed) over the checker and parser cases and cereal's sources
if [ -d "$ROOT/tests/symidx" ]; then
    cd "$ROOT/tests/symidx"
    for f in *.c; do
        for extra in "" "--cells -fparallel-chunk=1 -fparallel-threads=2"; do
            # shellcheck disable=SC2086
            "$CEREAL" check --target=x86_64-linux-gnu --dump-symbols $extra \
                "$f" >"$TMP/sym.out" 2>/dev/null
            if cmp -s "$TMP/sym.out" "${f%.c}.expected"; then
                ok
            else
                bad "symidx/$f $extra"
                diff "${f%.c}.expected" "$TMP/sym.out" | head -10 | sed 's/^/    /'
            fi
        done
    done
    cd "$ROOT"
fi
sym_verify() { # sym_verify LABEL FILES...: one verdict for the lot
    label=$1
    shift
    for f in "$@"; do
        extra=$(sed -n 's|^// flags: *||p' "$f" | head -1)
        # shellcheck disable=SC2086
        "$CEREAL" -fsyntax-only -std=c99 -pedantic -I"$ROOT/src" \
            -D_POSIX_C_SOURCE=200809L $extra --verify-symbols "$f" \
            2>/dev/null </dev/null | grep -v '^symbols: ' | sed "s|^|$f: |"
    done >"$TMP/sv"
    if [ -s "$TMP/sv" ]; then
        bad "verify-symbols $label"
        head -10 "$TMP/sv" | sed 's/^/    /'
    else
        ok
    fi
}
cd "$ROOT/tests/check" && sym_verify tests/check ./*.c
cd "$ROOT/tests/parse" && sym_verify tests/parse ./*.c
cd "$ROOT" && sym_verify sources src/*.c src/analysis/*.c src/lsp/*.c src/c/*.c

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
