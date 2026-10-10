#!/bin/sh
# fixits.sh [DIR]: cereal's fix-its against gcc-13 ($FIXITCC).
#   no argument: the sources of tests/check/fixit-*.c (flags: -std=c99
#                -pedantic and the file's "// flags:" line, as run.sh).
#   DIR:         every C file under DIR, flags in $FIXIT_FLAGS (default
#                "-Wall -Wextra -std=gnu17"), run in the file's directory with
#                -I. and -I..
# Both compilers run with -fsyntax-only -fdiagnostics-parseable-fixits (and
# -fdiagnostics-generate-patch when FIXIT_PATCH=1); what is compared is each
# fix-it line with the header line of the diagnostic it follows, for headers
# matching $FIXIT_COVER (the classes cereal produces: docs/C_DESIGN.md 4.1),
# plus the patch when asked.  Prints "N cases, M differ"; the differences
# ("<" gcc, ">" cereal) follow, or go to $FIXIT_OUT (default
# /tmp/fixits.out) when a DIR is given.  FIXIT_JOBS (default 8) bounds the
# concurrency.  FIXIT_LIST names a file of paths (relative to DIR) to run
# instead of all of DIR.  FIXIT_DG=1 (the DIR default) reads a gcc.dg test's
# dg-options for -std= and the -W/-pedantic/-fno- flags.  A case is "same",
# "differ", or "diag" when a covered diagnostic exists on one side only (a
# difference of diagnostics, not of fix-its); only "differ" fails.  With
# FIXIT_PATCH=1 the patches are compared too, but only for a file whose
# every gcc fix-it is of a covered class (the patch holds them all).
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CEREAL=${CEREAL:-$ROOT/cereal}
GCC=${FIXITCC:-gcc-13}
FIXIT_COVER=${FIXIT_COVER:-"; did you mean .|include .<|is defined in header|is a pointer; did you mean|expected .[]),;:(].|missing braces around initializer|add parentheses around left hand side|result of .[0-9]+.[0-9]+. is|by using a hexadecimal constant|keyword to refer to the type|expects argument of type"}
export ROOT CEREAL GCC FIXIT_COVER

if [ "${1:-}" = --one ]; then # --one DIR FILE FLAGS: "same" or "differ" + diff on stderr
    d=$2 f=$3 flags=$4
    w=$(mktemp -d "${TMPDIR:-/tmp}/fixits-one.XXXXXX") || exit 9
    trap 'rm -rf "$w"' EXIT INT TERM
    cd "$d" || exit 9
    pf=
    [ "${FIXIT_PATCH:-}" = 1 ] && pf=-fdiagnostics-generate-patch
    if [ "${FIXIT_DG:-}" = 1 ]; then # the language and warning options of a gcc.dg test
        for o in $(grep -a -m1 dg-options "$f" | sed -n 's/.*dg-options *"\([^"]*\)".*/\1/p'); do
            case $o in
            -std=*) flags="$(echo "$flags" | sed 's/-std=[^ ]*//') $o" ;;
            -pedantic* | -ansi | -W* | -trigraphs | -fno-* | -fgnu89-inline | -fsigned-char | -funsigned-char)
                flags="$flags $o" ;;
            esac
        done
    fi
    nopatch=0
    pick() { # the covered fix-its with their headers, then the patch (unless
        # gcc printed a fix-it of a class cereal does not produce, which the
        # patch would contain)
        awk -v cover="$FIXIT_COVER" -v nopatch="$nopatch" '
            /^[^ ].*: (fatal error|error|warning|note): / { hdr = $0; next }
            /^fix-it:/ { if (hdr ~ cover) print hdr " => " $0; next }
            /^--- / { patch = 1 }
            patch && !nopatch { print }' "$1"
    }
    for t in g c; do
        [ $t = g ] && p=$GCC || p=$CEREAL
        # shellcheck disable=SC2086
        LC_ALL=C timeout 60 "$p" -fsyntax-only -fdiagnostics-parseable-fixits \
            $pf $flags "$f" >/dev/null 2>"$w/$t.err" </dev/null
        if [ $t = g ]; then
            nopatch=$(awk -v cover="$FIXIT_COVER" '
                /^[^ ].*: (fatal error|error|warning|note): / { hdr = $0; next }
                /^fix-it:/ { if (hdr !~ cover) n++ }
                END { print (n > 0) }' "$w/g.err")
        fi
        pick "$w/$t.err" >"$w/$t.fix" || { echo differ; echo "== $f: awk failed" >&2; exit 0; }
    done
    # a covered diagnostic that one compiler lacks entirely is a difference
    # of diagnostics (other parity work), not of fix-its
    all() { grep -a '^[^ ].*: \(fatal error\|error\|warning\|note\): ' "$1" | sort -u; }
    heads() { grep -a ' => fix-it' "$1" | sed 's/ => fix-it.*//' | sort -u; }
    all "$w/g.err" >"$w/g.all"
    all "$w/c.err" >"$w/c.all"
    heads "$w/g.fix" >"$w/g.h"
    heads "$w/c.fix" >"$w/c.h"
    if cmp -s "$w/g.fix" "$w/c.fix"; then
        echo "same"
    else
        if [ -n "$(comm -23 "$w/g.h" "$w/c.all")$(comm -23 "$w/c.h" "$w/g.all")" ]; then
            st=diag
        else
            st=differ
        fi
        echo "$st"
        { echo "== $f ($st)"; diff "$w/g.fix" "$w/c.fix" | cut -c1-300 | head -${FIXIT_LINES:-30}; } >&2
    fi
    exit 0
fi

if [ $# -eq 0 ]; then
    cd "$ROOT/tests/check" || exit 2
    res=$(mktemp) err=$(mktemp)
    for f in fixit-*.c; do
        extra=$(sed -n 's|^// flags: *||p' "$f" | head -1)
        sh "$ROOT/bench/tools/fixits.sh" --one "$ROOT/tests/check" "$f" "-std=c99 -pedantic $extra"
    done >"$res" 2>"$err"
    n=$(wc -l <"$res") bad=$(grep -c -v '^same' "$res")
    cat "$err"
    rm -f "$res" "$err"
    echo "$n cases, $bad differ"
    [ "$bad" -eq 0 ]
    exit
fi

d=$(cd "$1" && pwd) || exit 2
out=${FIXIT_OUT:-/tmp/fixits.out}
res=$(mktemp)
: >"$out"
fl=${FIXIT_FLAGS:--Wall -Wextra -std=gnu17}
FIXIT_DG=${FIXIT_DG:-1}
export FIXIT_DG
(if [ -n "${FIXIT_LIST:-}" ]; then
    cat "$FIXIT_LIST"
else
    cd "$d" && find . -name '*.c' | sed 's#^\./##' | sort
fi) |
    xargs -P "${FIXIT_JOBS:-8}" -I{} sh -c \
        'dir=$(dirname "$2/$3"); exec sh "$0" --one "$dir" "$(basename "$3")" "$1 -I$dir -I$dir/.."' \
        "$0" "$fl" "$d" {} 2>>"$out" >"$res"
n=$(wc -l <"$res") bad=$(grep -c '^differ' "$res") dg=$(grep -c '^diag' "$res")
rm -f "$res"
echo "$n cases, $bad differ, $dg differ in the diagnostic itself (details in $out)"
[ "$bad" -eq 0 ]
