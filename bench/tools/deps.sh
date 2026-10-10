#!/bin/sh
# deps.sh [DIR]: the -M family of cereal against gcc-13 ($DEPSCC).
#   no argument: every tests/deps case (tests/deps_case.sh) through both;
#                prints "N cases, M differ" and the differences.
#   DIR:         every C file under DIR (names relative to DIR, flags in
#                $DEPS_FLAGS, e.g. "-Iinclude -DX"): -M, -MM, and
#                -E -MD -MP -MF through both compilers.  Prints "N runs, B
#                byte-identical, S same after normalising the compiler's own
#                include directory, D differ"; the differences ("<" gcc,
#                ">" cereal) go to $DEPS_OUT (default /tmp/deps.out).
# DEPS_JOBS (default 8) bounds the concurrency.  Corpus sources are not
# vendored (corp.sh lists the ones used: lupa, uvloop, zstandard under
# ~/corpus).  A compile_commands.json database is not read: pass its flags.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CEREAL=${CEREAL:-$ROOT/cereal}
GCC=${DEPSCC:-gcc-13}
export ROOT CEREAL GCC

if [ "${1:-}" = --one ]; then # --one DIR FILE: print "id status" per variant
    d=$2 f=$3
    w=$(mktemp -d "${TMPDIR:-/tmp}/deps-one.XXXXXX") || exit 9
    trap 'rm -rf "$w"' EXIT INT TERM
    cd "$d" || exit 9
    norm() { sed 's#/usr/lib/gcc/[^/]*/[0-9]*/#/usr/lib/gcc/T/V/#g' "$1"; }
    for v in M MM MD; do
        case $v in
        M) a="-M" ;;
        MM) a="-MM" ;;
        MD) a="-E -MD -MP -MF $w/x.d -o /dev/null" ;;
        esac
        for t in g c; do
            [ $t = g ] && p=$GCC || p=$CEREAL
            rm -f "$w/x.d"
            # shellcheck disable=SC2086
            if [ $v = MD ]; then
                "$p" $a $DEPS_FLAGS "$f" >/dev/null 2>&1 </dev/null
                [ -f "$w/x.d" ] && cp "$w/x.d" "$w/$t.$v" || : >"$w/$t.$v"
            else
                "$p" $a $DEPS_FLAGS "$f" >"$w/$t.$v" 2>/dev/null </dev/null
            fi
        done
        if cmp -s "$w/g.$v" "$w/c.$v"; then
            echo "same"
        elif [ "$(norm "$w/g.$v")" = "$(norm "$w/c.$v")" ]; then
            echo "norm"
        else
            echo "differ"
            { echo "== $f $v"; diff "$w/g.$v" "$w/c.$v" | cut -c1-200 | head -${DEPS_LINES:-20}; } >&2
        fi
    done
    exit 0
fi

if [ $# -eq 0 ]; then
    tmp=$(mktemp)
    n=0 bad=0
    for f in "$ROOT"/tests/deps/*.cmd; do
        name=$(basename "$f" .cmd)
        n=$((n + 1))
        sh "$ROOT/tests/deps_case.sh" "$GCC" "$name" >"$tmp.g"
        sh "$ROOT/tests/deps_case.sh" "$CEREAL" "$name" >"$tmp.c"
        if ! cmp -s "$tmp.g" "$tmp.c"; then
            bad=$((bad + 1))
            echo "== $name: $(cat "$f")"
            diff "$tmp.g" "$tmp.c" | head -10
        fi
    done
    rm -f "$tmp" "$tmp.g" "$tmp.c"
    echo "$n cases, $bad differ"
    [ "$bad" -eq 0 ]
    exit
fi

d=$(cd "$1" && pwd) || exit 2
out=${DEPS_OUT:-/tmp/deps.out}
res=$(mktemp)
: >"$out"
(cd "$d" && find . -name '*.c' | sed 's#^\./##' | sort) |
    xargs -P "${DEPS_JOBS:-8}" -I{} sh "$0" --one "$d" {} 2>>"$out" >"$res"
awk '{ n[$1]++ } END { printf "%d runs, %d byte-identical, %d same after normalising, %d differ\n",
        n["same"] + n["norm"] + n["differ"], n["same"], n["norm"], n["differ"] }' "$res"
rm -f "$res"
[ ! -s "$out" ]
